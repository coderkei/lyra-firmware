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
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "lyra_axs15231b.h"
#include "lyra_board_pins.h"
#include "sdkconfig.h"

#include <algorithm>
#include <atomic>
#include <cstring>

namespace {

using namespace lyra::board::jc3248w535en;

constexpr spi_host_device_t kDisplaySpiHost = SPI2_HOST;
constexpr uint32_t kPixelClockHz = 50 * 1000 * 1000;
// PSRAM DMA underruns at this QSPI rate. Reserve two small strips at boot
// so playback can never fragment or exhaust the LCD's DMA workspace.
constexpr size_t kTransportBufferLines = 4;
constexpr size_t kDisplayQueueDepth = 2;
constexpr size_t kRowBytes = LYRA_BOARD_DISPLAY_WIDTH * sizeof(uint16_t);
constexpr size_t kStripBytes = kRowBytes * kTransportBufferLines;
constexpr size_t kFrameBytes = kRowBytes * LYRA_BOARD_DISPLAY_HEIGHT;
static_assert(kRowBytes % 64 == 0);
constexpr uint32_t kDisplayRefreshMs = 16;
constexpr int kGuiCore = 1;
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

static bool s_lvgl_task_started;
static spi_device_handle_t s_lcd_spi;
static TaskHandle_t s_lcd_task;
static SemaphoreHandle_t s_lcd_done;
static uint32_t *s_dma_strips[kDisplayQueueDepth];
static const uint8_t *s_frame_pixels;
static size_t s_frame_rows;
static size_t s_dirty_rows;
static bool s_frame_pending;
static esp_err_t s_frame_result = ESP_ERR_INVALID_STATE;
static std::atomic<bool> s_retry_frame{false};
#ifdef CONFIG_LYRA_GUI_FRAME_TIMING
static int64_t s_render_started_us;
static uint32_t s_render_us;
#endif
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

static esp_err_t send_frame(const uint8_t *pixels, size_t rows)
{
    esp_err_t result = spi_device_acquire_bus(s_lcd_spi, portMAX_DELAY);
    if (result != ESP_OK) return result;

    // Preserve the board's CASET + RAMWR sequence and keep CS asserted from
    // the RAMWR header through the last quad-data strip. Never send RASET.
    constexpr int last_column = LYRA_BOARD_DISPLAY_WIDTH - 1;
    spi_transaction_t window{};
    window.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_CS_KEEP_ACTIVE;
    window.length = 32;
    window.tx_data[0] = 0x02;
    window.tx_data[2] = LCD_CMD_CASET;
    result = spi_device_polling_transmit(s_lcd_spi, &window);

    window.flags = SPI_TRANS_USE_TXDATA;
    window.tx_data[0] = 0;
    window.tx_data[2] = last_column >> 8;
    window.tx_data[3] = last_column & 0xff;
    if (result == ESP_OK) result = spi_device_polling_transmit(s_lcd_spi, &window);

    spi_transaction_t ram_write{};
    ram_write.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_CS_KEEP_ACTIVE;
    ram_write.length = 32;
    ram_write.tx_data[0] = 0x32;
    ram_write.tx_data[2] = LCD_CMD_RAMWR;
    if (result == ESP_OK) result = spi_device_polling_transmit(s_lcd_spi, &ram_write);

    spi_transaction_t strips[kDisplayQueueDepth]{};
    size_t submitted = 0;
    size_t completed = 0;
    for (size_t y = 0; result == ESP_OK && y < rows; y += kTransportBufferLines) {
        if (submitted - completed == kDisplayQueueDepth) {
            spi_transaction_t *done = nullptr;
            result = spi_device_get_trans_result(s_lcd_spi, &done, portMAX_DELAY);
            if (result != ESP_OK) break;
            ++completed;
        }
        const size_t index = submitted % kDisplayQueueDepth;
        const size_t lines = std::min(kTransportBufferLines, rows - y);
        const size_t bytes = lines * kRowBytes;
        const uint8_t *source = pixels + y * kRowBytes;
        uint32_t *destination = s_dma_strips[index];
        // Draw in native RGB565 so LVGL can use its native blend and image
        // copy paths. Swap both pixels in each word while staging for DMA.
        for (size_t word = 0; word < bytes / sizeof(uint32_t); ++word) {
            uint32_t value;
            std::memcpy(&value, source + word * sizeof(value), sizeof(value));
            destination[word] = ((value & 0x00ff00ffU) << 8) | ((value & 0xff00ff00U) >> 8);
        }
        auto &strip = strips[index];
        strip = {};
        strip.flags = SPI_TRANS_MODE_QIO | SPI_TRANS_DMA_BUFFER_ALIGN_MANUAL;
        if (y + lines < rows) strip.flags |= SPI_TRANS_CS_KEEP_ACTIVE;
        strip.length = bytes * 8;
        strip.tx_buffer = destination;
        result = spi_device_queue_trans(s_lcd_spi, &strip, portMAX_DELAY);
        if (result == ESP_OK) ++submitted;
    }
    // Drain every queued strip before the descriptors or buffers can be reused.
    while (completed < submitted) {
        spi_transaction_t *done = nullptr;
        if (spi_device_get_trans_result(s_lcd_spi, &done, portMAX_DELAY) == ESP_OK) ++completed;
    }
    if (result != ESP_OK) {
        // End an interrupted stream with a NOP so CS is released before retry.
        spi_transaction_t end{};
        end.flags = SPI_TRANS_USE_TXDATA;
        end.length = 32;
        end.tx_data[0] = 0x02;
        spi_device_polling_transmit(s_lcd_spi, &end);
    }
    spi_device_release_bus(s_lcd_spi);
    return result;
}

static void lcd_task(void *)
{
    bool full_frame = true;
    int64_t next_error_log_us = 0;
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const size_t rows = full_frame ? LYRA_BOARD_DISPLAY_HEIGHT : s_frame_rows;
#ifdef CONFIG_LYRA_GUI_FRAME_TIMING
        const int64_t started_us = esp_timer_get_time();
        const uint32_t render_us = s_render_us;
#endif
        s_frame_result = send_frame(s_frame_pixels, rows);
        full_frame = s_frame_result != ESP_OK;
        if (full_frame) {
            const int64_t now = esp_timer_get_time();
            if (now >= next_error_log_us) {
                ESP_LOGE(kTag, "LCD transfer failed: %s", esp_err_to_name(s_frame_result));
                next_error_log_us = now + 1'000'000;
            }
            s_retry_frame.store(true, std::memory_order_release);
        }
        xSemaphoreGive(s_lcd_done);
#ifdef CONFIG_LYRA_GUI_FRAME_TIMING
        if (!full_frame) {
            const int64_t now = esp_timer_get_time();
            static int64_t previous_us = 0;
            static uint32_t frames = 0, samples = 0, over_budget = 0, worst_us = 0;
            static uint32_t max_render_us = 0, max_transfer_us = 0;
            const int64_t interval = now - previous_us;
            if (previous_us && interval <= 100'000) {
                ++samples;
                if (interval > 33'333) ++over_budget;
                worst_us = std::max(worst_us, static_cast<uint32_t>(interval));
            }
            previous_us = now;
            max_render_us = std::max(max_render_us, render_us);
            max_transfer_us = std::max(max_transfer_us, static_cast<uint32_t>(now - started_us));
            if (++frames == 120) {
                ESP_LOGI(kTag, "LCD 120 frames: intervals=%lu over_33ms=%lu max_gap=%lu us "
                         "max_render=%lu us max_transfer=%lu us",
                         static_cast<unsigned long>(samples), static_cast<unsigned long>(over_budget),
                         static_cast<unsigned long>(worst_us), static_cast<unsigned long>(max_render_us),
                         static_cast<unsigned long>(max_transfer_us));
                frames = samples = over_budget = worst_us = max_render_us = max_transfer_us = 0;
            }
        }
#endif
    }
}

static void lvgl_wait_for_flush(lv_display_t *)
{
    if (!s_frame_pending) return;
    xSemaphoreTake(s_lcd_done, portMAX_DELAY);
    s_frame_pending = false;
}

static void lvgl_flush(lv_display_t *display, const lv_area_t *area, uint8_t *px_map)
{
    // The controller needs a stream starting at row zero. Stop after the
    // lowest dirty row; rows below it already match the panel's GRAM.
    s_dirty_rows = std::max(s_dirty_rows, static_cast<size_t>(area->y2 + 1));
    if (!lv_display_flush_is_last(display)) {
        lv_display_flush_ready(display);
        return;
    }
    s_frame_pixels = px_map;
    s_frame_rows = s_dirty_rows;
    s_dirty_rows = 0;
    s_frame_pending = true;
#ifdef CONFIG_LYRA_GUI_FRAME_TIMING
    s_render_us = static_cast<uint32_t>(esp_timer_get_time() - s_render_started_us);
#endif
    xTaskNotifyGive(s_lcd_task);
}

static uint32_t lvgl_time_ms()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

#ifdef CONFIG_LYRA_GUI_FRAME_TIMING
static void lvgl_refresh_started(lv_event_t *)
{
    s_render_started_us = esp_timer_get_time();
}
#endif

static void lvgl_task(void *)
{
    while (true) {
        if (s_retry_frame.exchange(false, std::memory_order_acq_rel)) {
            lv_obj_invalidate(lv_screen_active());
        }
        uint32_t delay_ms = lv_timer_handler();
        // Keep touch-to-frame latency low even when LVGL has no animation
        // timer pending; the display task remains isolated on APP_CPU.
        vTaskDelay(pdMS_TO_TICKS(std::clamp<uint32_t>(delay_ms, 1, kTouchReadMs)));
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
    TickType_t next_poll = xTaskGetTickCount();

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
        // Retain a stable input cadence without catch-up bursts on I2C errors.
        if (ret != ESP_OK) next_poll = xTaskGetTickCount();
        xTaskDelayUntil(&next_poll, pdMS_TO_TICKS(kTouchReadMs));
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
    // Keep polling off the audio core. The short I2C poll can preempt LVGL
    // rendering so contact changes are still sampled during a long frame.
    if (xTaskCreatePinnedToCore(touch_poll_task, "lyra_touch", 3072, nullptr, 7,
                                &s_touch_task, kGuiCore) != pdPASS) {
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

static esp_err_t init_panel_registers()
{
    // Use the panel adapter for the vendor's startup sequence, then hand the
    // same SPI bus and CS pin to the allocation-free pixel transport.
    esp_lcd_panel_io_spi_config_t io_config{};
    io_config.cs_gpio_num = static_cast<gpio_num_t>(kDisplayChipSelectGpio);
    io_config.dc_gpio_num = GPIO_NUM_NC;
    io_config.spi_mode = 3;
    io_config.pclk_hz = kPixelClockHz;
    io_config.trans_queue_depth = 1;
    io_config.lcd_cmd_bits = 32;
    io_config.lcd_param_bits = 8;
    io_config.flags.quad_mode = true;

    esp_lcd_panel_io_handle_t io = nullptr;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(kDisplaySpiHost, &io_config, &io),
                        kTag, "display panel IO init failed");
    lyra_axs15231b_vendor_config_t vendor_config{};
    vendor_config.use_qspi = true;
    esp_lcd_panel_dev_config_t panel_config{};
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 16;
    panel_config.reset_gpio_num = GPIO_NUM_NC;
    panel_config.vendor_config = &vendor_config;
    esp_lcd_panel_handle_t panel = nullptr;
    esp_err_t result = lyra_axs15231b_new_panel(io, &panel_config, &panel);
    if (result == ESP_OK) result = esp_lcd_panel_reset(panel);
    if (result == ESP_OK) result = esp_lcd_panel_init(panel);
    if (result == ESP_OK) result = esp_lcd_panel_disp_on_off(panel, true);
    if (panel) esp_lcd_panel_del(panel);
    esp_lcd_panel_io_del(io);
    return result;
}

static esp_err_t init_lcd_transport()
{
    spi_device_interface_config_t config{};
    config.mode = 3;
    config.clock_speed_hz = kPixelClockHz;
    config.spics_io_num = kDisplayChipSelectGpio;
    config.flags = SPI_DEVICE_HALFDUPLEX;
    config.queue_size = kDisplayQueueDepth;
    ESP_RETURN_ON_ERROR(spi_bus_add_device(kDisplaySpiHost, &config, &s_lcd_spi),
                        kTag, "LCD transport device setup failed");
    int actual_khz = 0;
    if (spi_device_get_actual_freq(s_lcd_spi, &actual_khz) == ESP_OK) {
        ESP_LOGI(kTag, "LCD QSPI clock: %d kHz", actual_khz);
    }

    s_lcd_done = xSemaphoreCreateBinary();
    if (!s_lcd_done) return ESP_ERR_NO_MEM;
    for (auto &strip : s_dma_strips) {
        strip = static_cast<uint32_t *>(heap_caps_aligned_alloc(
            64, kStripBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
        if (!strip) return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(lcd_task, "lyra_lcd", 3072, nullptr, 6,
                                &s_lcd_task, kGuiCore) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
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
    bus_config.max_transfer_sz = kStripBytes;
    bus_config.flags = SPICOMMON_BUSFLAG_QUAD;
    // Display completions stay off the audio/mixer core.
    bus_config.isr_cpu_id = ESP_INTR_CPU_AFFINITY_1;
    ESP_RETURN_ON_ERROR(spi_bus_initialize(kDisplaySpiHost, &bus_config, SPI_DMA_CH_AUTO),
                        kTag, "display SPI bus init failed");

    ESP_RETURN_ON_ERROR(init_panel_registers(), kTag, "AXS15231B initialization failed");
    ESP_RETURN_ON_ERROR(init_lcd_transport(), kTag, "LCD transport initialization failed");

    lv_init();
    lv_tick_set_cb(lvgl_time_ms);
    board->display = lv_display_create(LYRA_BOARD_DISPLAY_WIDTH, LYRA_BOARD_DISPLAY_HEIGHT);
    if (board->display == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    // LVGL direct mode waits for the previous flush before rendering even
    // with two buffers, then copies the old damage across buffers. One
    // persistent framebuffer avoids that copy and frees 300 KiB of PSRAM.
    void *framebuffer = heap_caps_aligned_calloc(64, 1, kFrameBytes, MALLOC_CAP_SPIRAM);
    if (!framebuffer) return ESP_ERR_NO_MEM;
    lv_display_set_color_format(board->display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(board->display, framebuffer, nullptr, kFrameBytes,
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_timer_set_period(lv_display_get_refr_timer(board->display), kDisplayRefreshMs);
    lv_display_set_flush_cb(board->display, lvgl_flush);
    lv_display_set_flush_wait_cb(board->display, lvgl_wait_for_flush);
#ifdef CONFIG_LYRA_GUI_FRAME_TIMING
    lv_display_add_event_cb(board->display, lvgl_refresh_started, LV_EVENT_REFR_START, nullptr);
#endif
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
    if (s_lcd_task == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_lvgl_task_started) {
        return ESP_OK;
    }
    // GUI callbacks read/write NVS and other flash-backed settings. Flash
    // operations disable the cache, so this stack must remain in internal RAM.
    const BaseType_t task_created = xTaskCreatePinnedToCoreWithCaps(
        lvgl_task, "lyra_gui", kLvglTaskStack, nullptr, 5, nullptr, kGuiCore,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (task_created != pdPASS) {
        ESP_LOGE(kTag, "LVGL task allocation failed: internal free=%u largest=%u",
                 static_cast<unsigned>(heap_caps_get_free_size(
                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(
                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
        return ESP_ERR_NO_MEM;
    }
    s_lvgl_task_started = true;
    return ESP_OK;
}

extern "C" esp_err_t lyra_board_display_finish_boot_frame(lv_display_t *display)
{
    lvgl_wait_for_flush(display);
    lv_display_flush_ready(display);
    return s_frame_result;
}
