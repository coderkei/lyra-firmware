/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lyra_board.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "lyra_axs15231b.h"
#include "lyra_board_pins.h"

#include <algorithm>

namespace {

using namespace lyra::board::jc3248w535en;

constexpr spi_host_device_t kDisplaySpiHost = SPI2_HOST;
constexpr uint32_t kPixelClockHz = 50 * 1000 * 1000;
// Direct DMA from PSRAM underruns on this ESP32-S3 board at the panel's
// 50 MHz quad rate, so the panel driver copies bounded chunks through an
// internal DMA buffer. Keep that private allocation small enough to coexist
// with the I2S buffers and avoid starving the allocator once playback starts.
constexpr uint32_t kTransportBufferLines = 8;
constexpr uint32_t kDisplayQueueDepth = 1;
constexpr uint32_t kLvglTickMs = 2;
constexpr uint32_t kDisplayRefreshMs = 16;
// LVGL's bounded TinyJPEG decoder performs its header probe on the caller's
// stack with a 4 KB work area. The previous 6 KB task was sufficient for the
// shape/text UI but overflowed as soon as the first deferred cover was loaded.
// Keep enough headroom for the decoder plus the nested refresh/timer pipeline.
constexpr uint32_t kLvglTaskStack = 16 * 1024;
constexpr uint32_t kTouchReadMs = 10;
// The polled AXS15231B occasionally returns an empty record between valid
// contact records. Keep the contact continuous across two missed polls so
// LVGL does not turn one drag into a sequence of clicks.
constexpr uint32_t kTouchReleaseDebounceMs = 30;
constexpr uint32_t kTouchAddress = 0x3B;
constexpr const char *kTag = "lyra.board";
constexpr ledc_mode_t kBacklightSpeedMode = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t kBacklightTimer = LEDC_TIMER_0;
constexpr ledc_channel_t kBacklightChannel = LEDC_CHANNEL_0;
constexpr ledc_timer_bit_t kBacklightDutyResolution = LEDC_TIMER_10_BIT;
constexpr uint32_t kBacklightFrequencyHz = 20000;
constexpr uint32_t kBacklightMaximumDuty = (1u << 10) - 1u;

static SemaphoreHandle_t s_lvgl_mutex;
static bool s_lvgl_task_started;
static i2c_master_bus_handle_t s_touch_bus;
static i2c_master_dev_handle_t s_touch_device;
static TaskHandle_t s_touch_task;
static portMUX_TYPE s_touch_state_lock = portMUX_INITIALIZER_UNLOCKED;
static int16_t s_touch_x;
static int16_t s_touch_y;
static bool s_touch_pressed;
static uint32_t s_last_touch_error_ms;
static bool s_backlight_initialized;
static bool s_backlight_enabled;
static uint8_t s_backlight_brightness_percent = 72;

static bool lvgl_flush_done(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *user_ctx)
{
    lv_display_flush_ready(static_cast<lv_display_t *>(user_ctx));
    return false;
}

static void lvgl_flush(lv_display_t *display, const lv_area_t *, uint8_t *px_map)
{
    auto *panel = static_cast<esp_lcd_panel_handle_t>(lv_display_get_user_data(display));

    // Direct mode can report several damaged areas from one refresh. They are
    // all accumulated in this persistent full-screen buffer, so only transmit
    // after the final area has been rendered.
    if (!lv_display_flush_is_last(display)) {
        lv_display_flush_ready(display);
        return;
    }

    // The AXS15231B QSPI command stream must restart at row zero. LVGL draws
    // only invalidated pixels, but the physical transfer remains one complete
    // top-to-bottom frame in the panel's native byte order.
    const esp_err_t ret = esp_lcd_panel_draw_bitmap(panel, 0, 0,
                                                    LYRA_BOARD_DISPLAY_WIDTH,
                                                    LYRA_BOARD_DISPLAY_HEIGHT, px_map);
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "LCD flush failed: %s", esp_err_to_name(ret));
        lv_display_flush_ready(display);
    }
}

static void lvgl_tick(void *)
{
    lv_tick_inc(kLvglTickMs);
}

static void lvgl_task(void *)
{
    while (true) {
        xSemaphoreTakeRecursive(s_lvgl_mutex, portMAX_DELAY);
        uint32_t delay_ms = lv_timer_handler();
        xSemaphoreGiveRecursive(s_lvgl_mutex);
        if (delay_ms < 2) delay_ms = 2;
        // Keep touch-to-frame latency low even when LVGL has no animation
        // timer pending; the display task remains isolated on APP_CPU.
        if (delay_ms > 10) delay_ms = 10;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

static void set_touch_state(bool pressed, int16_t x, int16_t y)
{
    portENTER_CRITICAL(&s_touch_state_lock);
    s_touch_pressed = pressed;
    s_touch_x = x;
    s_touch_y = y;
    portEXIT_CRITICAL(&s_touch_state_lock);
}

static void touch_poll_task(void *)
{
    // AXS15231B touch read command: one touch record (8 bytes) following
    // the controller's 11-byte polling request. Keep this transaction out of
    // LVGL's timer thread so a slow or disconnected I2C device cannot freeze
    // rendering and input dispatch together.
    constexpr uint8_t kTouchRequest[] = {
        0xB5, 0xAB, 0xA5, 0x5A, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
    };

    bool contact_active = false;
    int16_t last_x = 0;
    int16_t last_y = 0;
    uint32_t last_contact_ms = 0;

    while (true) {
        uint8_t response[8] = {};
        const esp_err_t ret = i2c_master_transmit_receive(
            s_touch_device, kTouchRequest, sizeof(kTouchRequest), response, sizeof(response), 20);

        const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
        if (ret == ESP_OK && response[1] == 1) {
            const uint16_t raw_x = static_cast<uint16_t>(((response[2] & 0x0F) << 8) | response[3]);
            const uint16_t raw_y = static_cast<uint16_t>(((response[4] & 0x0F) << 8) | response[5]);
            last_x = static_cast<int16_t>(std::min<uint16_t>(raw_x, LYRA_BOARD_DISPLAY_WIDTH - 1));
            last_y = static_cast<int16_t>(std::min<uint16_t>(raw_y, LYRA_BOARD_DISPLAY_HEIGHT - 1));
            last_contact_ms = now;
            contact_active = true;
        } else if (ret != ESP_OK) {
            if (now - s_last_touch_error_ms > 2000) {
                ESP_LOGW(kTag, "touch read unavailable: %s", esp_err_to_name(ret));
                s_last_touch_error_ms = now;
            }
        }

        if (contact_active && now - last_contact_ms > kTouchReleaseDebounceMs) {
            contact_active = false;
        }
        set_touch_state(contact_active, last_x, last_y);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void touch_read(lv_indev_t *, lv_indev_data_t *data)
{
    bool pressed;
    int16_t x;
    int16_t y;
    portENTER_CRITICAL(&s_touch_state_lock);
    pressed = s_touch_pressed;
    x = s_touch_x;
    y = s_touch_y;
    portEXIT_CRITICAL(&s_touch_state_lock);

    data->point.x = x;
    data->point.y = y;
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static esp_err_t start_touch_polling()
{
    if (s_touch_device == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xTaskCreatePinnedToCore(touch_poll_task, "lyra_touch", 3072, nullptr, 4, &s_touch_task, 0) != pdPASS) {
        s_touch_task = nullptr;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t init_touch()
{
    i2c_master_bus_config_t bus_config{};
    bus_config.i2c_port = static_cast<i2c_port_num_t>(kTouchI2cPort);
    bus_config.sda_io_num = static_cast<gpio_num_t>(kTouchSdaGpio);
    bus_config.scl_io_num = static_cast<gpio_num_t>(kTouchSclGpio);
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7;
    bus_config.flags.enable_internal_pullup = true;

    esp_err_t ret = i2c_new_master_bus(&bus_config, &s_touch_bus);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = i2c_master_probe(s_touch_bus, kTouchAddress, 100);
    if (ret != ESP_OK) {
        ESP_LOGW(kTag, "AXS15231B touch was not detected; GUI remains usable without touch");
        i2c_del_master_bus(s_touch_bus);
        s_touch_bus = nullptr;
        return ret;
    }

    i2c_device_config_t device_config{};
    device_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    device_config.device_address = kTouchAddress;
    device_config.scl_speed_hz = 400000;
    return i2c_master_bus_add_device(s_touch_bus, &device_config, &s_touch_device);
}

static esp_err_t init_backlight()
{
    ledc_timer_config_t timer_config{};
    timer_config.speed_mode = kBacklightSpeedMode;
    timer_config.duty_resolution = kBacklightDutyResolution;
    timer_config.timer_num = kBacklightTimer;
    timer_config.freq_hz = kBacklightFrequencyHz;
    timer_config.clk_cfg = LEDC_AUTO_CLK;
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_config), kTag, "backlight timer setup failed");

    ledc_channel_config_t channel_config{};
    channel_config.gpio_num = kDisplayBacklightGpio;
    channel_config.speed_mode = kBacklightSpeedMode;
    channel_config.channel = kBacklightChannel;
    channel_config.intr_type = LEDC_INTR_DISABLE;
    channel_config.timer_sel = kBacklightTimer;
    channel_config.duty = 0;
    channel_config.hpoint = 0;
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel_config), kTag, "backlight channel setup failed");
    s_backlight_initialized = true;
    s_backlight_enabled = false;
    return ESP_OK;
}

static esp_err_t apply_backlight()
{
    if (!s_backlight_initialized) return ESP_ERR_INVALID_STATE;
    const uint32_t duty = s_backlight_enabled ?
        (static_cast<uint32_t>(s_backlight_brightness_percent) * kBacklightMaximumDuty) / 100u : 0;
    ESP_RETURN_ON_ERROR(ledc_set_duty(kBacklightSpeedMode, kBacklightChannel, duty), kTag,
                        "backlight duty update failed");
    return ledc_update_duty(kBacklightSpeedMode, kBacklightChannel);
}

} // namespace

extern "C" esp_err_t lyra_board_display_init(lyra_board_display_t *board)
{
    if (board == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *board = {};

    ESP_RETURN_ON_ERROR(init_backlight(), kTag, "backlight init failed");

    spi_bus_config_t bus_config{};
    bus_config.sclk_io_num = kDisplayClockGpio;
    bus_config.data0_io_num = kDisplayData0Gpio;
    bus_config.data1_io_num = kDisplayData1Gpio;
    bus_config.data2_io_num = kDisplayData2Gpio;
    bus_config.data3_io_num = kDisplayData3Gpio;
    bus_config.max_transfer_sz = LYRA_BOARD_DISPLAY_WIDTH * kTransportBufferLines * sizeof(uint16_t);
    bus_config.flags = SPICOMMON_BUSFLAG_QUAD;
    ESP_RETURN_ON_ERROR(spi_bus_initialize(kDisplaySpiHost, &bus_config, SPI_DMA_CH_AUTO),
                        kTag, "display SPI bus init failed");

    esp_lcd_panel_io_spi_config_t io_config{};
    io_config.cs_gpio_num = static_cast<gpio_num_t>(kDisplayChipSelectGpio);
    io_config.dc_gpio_num = GPIO_NUM_NC;
    io_config.spi_mode = 3;
    io_config.pclk_hz = kPixelClockHz;
    io_config.trans_queue_depth = kDisplayQueueDepth;
    io_config.lcd_cmd_bits = 32;
    io_config.lcd_param_bits = 8;
    io_config.flags.quad_mode = true;
    // Leave psram_dma_direct disabled. ESP-IDF will copy each bounded chunk
    // into an internal DMA-capable buffer before transmission. This is the
    // reliable path on JC3248W535EN at 50 MHz QSPI.
    io_config.flags.psram_dma_direct = false;

    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(kDisplaySpiHost, &io_config, &board->panel_io),
                        kTag, "display panel IO init failed");

    const lyra_axs15231b_vendor_config_t vendor_config = {
        .init_cmds = nullptr,
        .init_cmds_count = 0,
        .use_qspi = true,
    };
    esp_lcd_panel_dev_config_t panel_config{};
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 16;
    panel_config.reset_gpio_num = GPIO_NUM_NC;
    panel_config.vendor_config = const_cast<lyra_axs15231b_vendor_config_t *>(&vendor_config);

    ESP_RETURN_ON_ERROR(lyra_axs15231b_new_panel(board->panel_io, &panel_config, &board->panel),
                        kTag, "AXS15231B panel creation failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(board->panel), kTag, "panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(board->panel), kTag, "panel init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(board->panel, true), kTag, "panel enable failed");

    lv_init();
    board->display = lv_display_create(LYRA_BOARD_DISPLAY_WIDTH, LYRA_BOARD_DISPLAY_HEIGHT);
    if (board->display == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    // QSPI partial transfers cannot address an arbitrary row safely: RAMWRC
    // continues from the panel's current stream position. Persistent full-
    // screen buffers let LVGL render only damaged areas while every physical
    // flush remains a deterministic top-to-bottom RAMWR stream, matching the
    // board vendor's known-good port.
    const size_t draw_buffer_size = LYRA_BOARD_DISPLAY_WIDTH * LYRA_BOARD_DISPLAY_HEIGHT * sizeof(uint16_t);
    void *buffer_one = heap_caps_aligned_calloc(64, 1, draw_buffer_size, MALLOC_CAP_SPIRAM);
    if (buffer_one == nullptr) {
        ESP_LOGE(kTag, "LVGL primary PSRAM frame buffer allocation failed");
        return ESP_ERR_NO_MEM;
    }
    void *buffer_two = heap_caps_aligned_calloc(64, 1, draw_buffer_size, MALLOC_CAP_SPIRAM);
    if (buffer_two == nullptr) {
        ESP_LOGW(kTag, "LVGL secondary PSRAM frame buffer unavailable; using single buffering");
    }
    lv_display_set_color_format(board->display, LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_display_set_buffers(board->display, buffer_one, buffer_two, draw_buffer_size,
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_timer_set_period(lv_display_get_refr_timer(board->display), kDisplayRefreshMs);
    lv_display_set_user_data(board->display, board->panel);
    lv_display_set_flush_cb(board->display, lvgl_flush);

    const esp_lcd_panel_io_callbacks_t callbacks = {
        .on_color_trans_done = lvgl_flush_done,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_register_event_callbacks(board->panel_io, &callbacks, board->display),
                        kTag, "panel callback registration failed");

    esp_timer_create_args_t tick_args{};
    tick_args.callback = lvgl_tick;
    tick_args.name = "lyra_lvgl_tick";
    esp_timer_handle_t tick_timer = nullptr;
    ESP_RETURN_ON_ERROR(esp_timer_create(&tick_args, &tick_timer), kTag, "LVGL tick timer creation failed");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(tick_timer, kLvglTickMs * 1000),
                        kTag, "LVGL tick timer start failed");

    s_lvgl_mutex = xSemaphoreCreateRecursiveMutex();
    if (s_lvgl_mutex == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t touch_ret = init_touch();
    if (touch_ret == ESP_OK) {
        const esp_err_t touch_task_ret = start_touch_polling();
        if (touch_task_ret == ESP_OK) {
            lv_indev_t *touch = lv_indev_create();
            if (touch == nullptr) {
                ESP_LOGW(kTag, "touch input device allocation failed; GUI remains usable without touch");
            } else {
                lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
                lv_indev_set_display(touch, board->display);
                lv_indev_set_read_cb(touch, touch_read);
                lv_timer_set_period(lv_indev_get_read_timer(touch), kTouchReadMs);
                // Recognize a drag quickly so a scroll gesture is not
                // misreported as a row click on the capacitive panel.
                lv_indev_set_scroll_limit(touch, 4);
                lv_indev_set_scroll_throw(touch, 20);
                board->touch_available = true;
            }
        } else {
            ESP_LOGW(kTag, "touch polling task could not start: %s", esp_err_to_name(touch_task_ret));
        }
    }

    ESP_LOGI(kTag, "GUI display ready: %dx%d, PSRAM free=%u", LYRA_BOARD_DISPLAY_WIDTH,
             LYRA_BOARD_DISPLAY_HEIGHT, static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    return ESP_OK;
}

extern "C" esp_err_t lyra_board_display_set_backlight(bool enabled)
{
    s_backlight_enabled = enabled;
    return apply_backlight();
}

extern "C" esp_err_t lyra_board_display_set_brightness(uint8_t brightness_percent)
{
    s_backlight_brightness_percent = std::min<uint8_t>(brightness_percent, 100);
    return apply_backlight();
}

extern "C" esp_err_t lyra_board_display_start(void)
{
    if (s_lvgl_mutex == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_lvgl_task_started) {
        return ESP_OK;
    }
    if (xTaskCreatePinnedToCore(lvgl_task, "lyra_gui", kLvglTaskStack, nullptr, 5, nullptr, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_lvgl_task_started = true;
    return ESP_OK;
}
