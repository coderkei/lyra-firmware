/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Native ESP-IDF 6.x AXS15231B panel adapter for the JC3248W535EN.
 *
 * The command values below are the board-specific QSPI sequence from the
 * staged Guition BSP, expressed through the current esp_lcd panel interface.
 * Touch is intentionally kept out of this component; the application owns
 * the new ESP-IDF I2C master handle and polls the controller during GUI bring-up.
 */

#include "lyra_axs15231b.h"

#include <cstdlib>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr uint32_t kQspiWriteCommand = 0x02U;
constexpr uint32_t kQspiWriteColor = 0x32U;
constexpr const char *kTag = "lyra.axs15231b";

struct AxsPanel {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    gpio_num_t reset_gpio;
    bool reset_active_high;
    bool use_qspi;
    int x_gap;
    int y_gap;
    uint8_t madctl;
    uint8_t colmod;
    uint8_t bytes_per_pixel;
    const lyra_axs15231b_init_cmd_t *init_cmds;
    size_t init_cmds_count;
};

static esp_err_t panel_delete(esp_lcd_panel_t *panel);
static esp_err_t panel_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_init(esp_lcd_panel_t *panel);
static esp_err_t panel_draw(esp_lcd_panel_t *panel, int x_start, int y_start,
                            int x_end, int y_end, const void *color_data);
static esp_err_t panel_invert(esp_lcd_panel_t *panel, bool invert);
static esp_err_t panel_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y);
static esp_err_t panel_swap_xy(esp_lcd_panel_t *panel, bool swap_axes);
static esp_err_t panel_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap);
static esp_err_t panel_display_on(esp_lcd_panel_t *panel, bool on);

static esp_err_t tx_param(AxsPanel *panel, int command, const void *data, size_t size)
{
    uint32_t encoded = static_cast<uint32_t>(command & 0xff);
    if (panel->use_qspi) {
        encoded = (encoded << 8) | (kQspiWriteCommand << 24);
    }
    return esp_lcd_panel_io_tx_param(panel->io, encoded, data, size);
}

static esp_err_t tx_color(AxsPanel *panel, int command, const void *data, size_t size)
{
    uint32_t encoded = static_cast<uint32_t>(command & 0xff);
    if (panel->use_qspi) {
        encoded = (encoded << 8) | (kQspiWriteColor << 24);
    }
    return esp_lcd_panel_io_tx_color(panel->io, encoded, data, size);
}

// This is the JC3248W535EN vendor sequence, kept local so the native adapter
// is self-contained and does not compile the historical Arduino BSP.
static const uint8_t kBb[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5A, 0xA5};
static const uint8_t kA0[] = {0xC0, 0x10, 0x00, 0x02, 0x00, 0x00, 0x04, 0x3F, 0x20, 0x05, 0x3F, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t kA2[] = {0x30, 0x3C, 0x24, 0x14, 0xD0, 0x20, 0xFF, 0xE0, 0x40, 0x19, 0x80, 0x80, 0x80, 0x20, 0xF9, 0x10, 0x02, 0xFF, 0xFF, 0xF0, 0x90, 0x01, 0x32, 0xA0, 0x91, 0xE0, 0x20, 0x7F, 0xFF, 0x00, 0x5A};
static const uint8_t kD0[] = {0xE0, 0x40, 0x51, 0x24, 0x08, 0x05, 0x10, 0x01, 0x20, 0x15, 0x42, 0xC2, 0x22, 0x22, 0xAA, 0x03, 0x10, 0x12, 0x60, 0x14, 0x1E, 0x51, 0x15, 0x00, 0x8A, 0x20, 0x00, 0x03, 0x3A, 0x12};
static const uint8_t kA3[] = {0xA0, 0x06, 0xAA, 0x00, 0x08, 0x02, 0x0A, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x55, 0x55};
static const uint8_t kC1[] = {0x31, 0x04, 0x02, 0x02, 0x71, 0x05, 0x24, 0x55, 0x02, 0x00, 0x41, 0x00, 0x53, 0xFF, 0xFF, 0xFF, 0x4F, 0x52, 0x00, 0x4F, 0x52, 0x00, 0x45, 0x3B, 0x0B, 0x02, 0x0D, 0x00, 0xFF, 0x40};
static const uint8_t kC3[] = {0x00, 0x00, 0x00, 0x50, 0x03, 0x00, 0x00, 0x00, 0x01, 0x80, 0x01};
static const uint8_t kC4[] = {0x00, 0x24, 0x33, 0x80, 0x00, 0xEA, 0x64, 0x32, 0xC8, 0x64, 0xC8, 0x32, 0x90, 0x90, 0x11, 0x06, 0xDC, 0xFA, 0x00, 0x00, 0x80, 0xFE, 0x10, 0x10, 0x00, 0x0A, 0x0A, 0x44, 0x50};
static const uint8_t kC5[] = {0x18, 0x00, 0x00, 0x03, 0xFE, 0x3A, 0x4A, 0x20, 0x30, 0x10, 0x88, 0xDE, 0x0D, 0x08, 0x0F, 0x0F, 0x01, 0x3A, 0x4A, 0x20, 0x10, 0x10, 0x00};
static const uint8_t kC6[] = {0x05, 0x0A, 0x05, 0x0A, 0x00, 0xE0, 0x2E, 0x0B, 0x12, 0x22, 0x12, 0x22, 0x01, 0x03, 0x00, 0x3F, 0x6A, 0x18, 0xC8, 0x22};
static const uint8_t kC7[] = {0x50, 0x32, 0x28, 0x00, 0xA2, 0x80, 0x8F, 0x00, 0x80, 0xFF, 0x07, 0x11, 0x9C, 0x67, 0xFF, 0x24, 0x0C, 0x0D, 0x0E, 0x0F};
static const uint8_t kC9[] = {0x33, 0x44, 0x44, 0x01};
static const uint8_t kCf[] = {0x2C, 0x1E, 0x88, 0x58, 0x13, 0x18, 0x56, 0x18, 0x1E, 0x68, 0x88, 0x00, 0x65, 0x09, 0x22, 0xC4, 0x0C, 0x77, 0x22, 0x44, 0xAA, 0x55, 0x08, 0x08, 0x12, 0xA0, 0x08};
static const uint8_t kD5[] = {0x40, 0x8E, 0x8D, 0x01, 0x35, 0x04, 0x92, 0x74, 0x04, 0x92, 0x74, 0x04, 0x08, 0x6A, 0x04, 0x46, 0x03, 0x03, 0x03, 0x03, 0x82, 0x01, 0x03, 0x00, 0xE0, 0x51, 0xA1, 0x00, 0x00, 0x00};
static const uint8_t kD6[] = {0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE, 0x93, 0x00, 0x01, 0x83, 0x07, 0x07, 0x00, 0x07, 0x07, 0x00, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x00, 0x84, 0x00, 0x20, 0x01, 0x00};
static const uint8_t kD7[] = {0x03, 0x01, 0x0B, 0x09, 0x0F, 0x0D, 0x1E, 0x1F, 0x18, 0x1D, 0x1F, 0x19, 0x40, 0x8E, 0x04, 0x00, 0x20, 0xA0, 0x1F};
static const uint8_t kD8[] = {0x02, 0x00, 0x0A, 0x08, 0x0E, 0x0C, 0x1E, 0x1F, 0x18, 0x1D, 0x1F, 0x19};
static const uint8_t kD9[] = {0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F};
static const uint8_t kDd[] = {0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F};
static const uint8_t kDf[] = {0x44, 0x73, 0x4B, 0x69, 0x00, 0x0A, 0x02, 0x90};
static const uint8_t kE0[] = {0x3B, 0x28, 0x10, 0x16, 0x0C, 0x06, 0x11, 0x28, 0x5C, 0x21, 0x0D, 0x35, 0x13, 0x2C, 0x33, 0x28, 0x0D};
static const uint8_t kE1[] = {0x37, 0x28, 0x10, 0x16, 0x0B, 0x06, 0x11, 0x28, 0x5C, 0x21, 0x0D, 0x35, 0x14, 0x2C, 0x33, 0x28, 0x0F};
static const uint8_t kE2[] = {0x3B, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x35, 0x44, 0x32, 0x0C, 0x14, 0x14, 0x36, 0x3A, 0x2F, 0x0D};
static const uint8_t kE3[] = {0x37, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x35, 0x44, 0x32, 0x0C, 0x14, 0x14, 0x36, 0x32, 0x2F, 0x0F};
static const uint8_t kE4[] = {0x3B, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x39, 0x44, 0x2E, 0x0C, 0x14, 0x14, 0x36, 0x3A, 0x2F, 0x0D};
static const uint8_t kE5[] = {0x37, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x39, 0x44, 0x2E, 0x0C, 0x14, 0x14, 0x36, 0x3A, 0x2F, 0x0F};
static const uint8_t kA4[] = {0x85, 0x85, 0x95, 0x85};
static const uint8_t kA4Panel[] = {
    0x85, 0x85, 0x95, 0x82, 0xAF, 0xAA, 0xAA, 0x80,
    0x10, 0x30, 0x40, 0x40, 0x20, 0xFF, 0x60, 0x30,
};
static const uint8_t kZeros8[] = {0, 0, 0, 0, 0, 0, 0, 0};
static const uint8_t kZeros4[] = {0, 0, 0, 0};

static const lyra_axs15231b_init_cmd_t kDefaultInit[] = {
    {0xBB, kBb, sizeof(kBb), 0}, {0xA0, kA0, sizeof(kA0), 0}, {0xA2, kA2, sizeof(kA2), 0},
    {0xD0, kD0, sizeof(kD0), 0}, {0xA3, kA3, sizeof(kA3), 0}, {0xC1, kC1, sizeof(kC1), 0},
    {0xC3, kC3, sizeof(kC3), 0}, {0xC4, kC4, sizeof(kC4), 0}, {0xC5, kC5, sizeof(kC5), 0},
    {0xC6, kC6, sizeof(kC6), 0}, {0xC7, kC7, sizeof(kC7), 0}, {0xC9, kC9, sizeof(kC9), 0},
    {0xCF, kCf, sizeof(kCf), 0}, {0xD5, kD5, sizeof(kD5), 0}, {0xD6, kD6, sizeof(kD6), 0},
    {0xD7, kD7, sizeof(kD7), 0}, {0xD8, kD8, sizeof(kD8), 0}, {0xD9, kD9, sizeof(kD9), 0},
    {0xDD, kDd, sizeof(kDd), 0}, {0xDF, kDf, sizeof(kDf), 0}, {0xE0, kE0, sizeof(kE0), 0},
    {0xE1, kE1, sizeof(kE1), 0}, {0xE2, kE2, sizeof(kE2), 0}, {0xE3, kE3, sizeof(kE3), 0},
    {0xE4, kE4, sizeof(kE4), 0}, {0xE5, kE5, sizeof(kE5), 0},
    {0xA4, kA4Panel, sizeof(kA4Panel), 0}, {0xA4, kA4, sizeof(kA4), 0},
    {0xBB, kZeros8, sizeof(kZeros8), 0}, {LCD_CMD_NORON, nullptr, 0, 0},
    {LCD_CMD_SLPOUT, nullptr, 0, 120}, {LCD_CMD_RAMWR, kZeros4, sizeof(kZeros4), 0},
};

static AxsPanel *as_panel(esp_lcd_panel_t *panel)
{
    return __containerof(panel, AxsPanel, base);
}

static esp_err_t panel_delete(esp_lcd_panel_t *panel)
{
    auto *device = as_panel(panel);
    if (device->reset_gpio >= 0) {
        gpio_reset_pin(device->reset_gpio);
    }
    std::free(device);
    return ESP_OK;
}

static esp_err_t panel_reset(esp_lcd_panel_t *panel)
{
    auto *device = as_panel(panel);
    if (device->reset_gpio >= 0) {
        gpio_set_level(device->reset_gpio, device->reset_active_high ? 1 : 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(device->reset_gpio, device->reset_active_high ? 0 : 1);
        vTaskDelay(pdMS_TO_TICKS(120));
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(tx_param(device, LCD_CMD_SWRESET, nullptr, 0), kTag, "software reset failed");
    vTaskDelay(pdMS_TO_TICKS(120));
    return ESP_OK;
}

static esp_err_t panel_init(esp_lcd_panel_t *panel)
{
    auto *device = as_panel(panel);
    ESP_RETURN_ON_ERROR(tx_param(device, LCD_CMD_SLPOUT, nullptr, 0), kTag, "sleep-out failed");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(tx_param(device, LCD_CMD_MADCTL, &device->madctl, 1), kTag, "MADCTL failed");
    ESP_RETURN_ON_ERROR(tx_param(device, LCD_CMD_COLMOD, &device->colmod, 1), kTag, "COLMOD failed");

    const auto *commands = device->init_cmds ? device->init_cmds : kDefaultInit;
    const size_t command_count = device->init_cmds ? device->init_cmds_count : (sizeof(kDefaultInit) / sizeof(kDefaultInit[0]));
    for (size_t index = 0; index < command_count; ++index) {
        const auto &command = commands[index];
        ESP_RETURN_ON_ERROR(tx_param(device, command.command, command.data, command.data_bytes), kTag, "panel init command failed");
        if (command.delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(command.delay_ms));
        }
    }
    ESP_RETURN_ON_ERROR(tx_param(device, LCD_CMD_DISPON, nullptr, 0), kTag, "display-on failed");
    vTaskDelay(pdMS_TO_TICKS(20));
    return ESP_OK;
}

static esp_err_t panel_draw(esp_lcd_panel_t *panel, int x_start, int y_start,
                            int x_end, int y_end, const void *color_data)
{
    auto *device = as_panel(panel);
    if (x_start >= x_end || y_start >= y_end || color_data == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    x_start += device->x_gap;
    x_end += device->x_gap;
    y_start += device->y_gap;
    y_end += device->y_gap;

    uint8_t column_data[] = {
        static_cast<uint8_t>((x_start >> 8) & 0xff), static_cast<uint8_t>(x_start & 0xff),
        static_cast<uint8_t>(((x_end - 1) >> 8) & 0xff), static_cast<uint8_t>((x_end - 1) & 0xff),
    };
    ESP_RETURN_ON_ERROR(tx_param(device, LCD_CMD_CASET, column_data, sizeof(column_data)), kTag, "column window failed");

    // The JC3248W535EN vendor AXS15231B QSPI driver sets the column window
    // only. Sending a QSPI RASET transaction here corrupts later partial
    // updates (the LVGL performance label is an easy way to reproduce it).
    // Keep RASET for a future non-QSPI adapter, where it is required.
    if (!device->use_qspi) {
        const uint8_t row_data[] = {
            static_cast<uint8_t>((y_start >> 8) & 0xff), static_cast<uint8_t>(y_start & 0xff),
            static_cast<uint8_t>(((y_end - 1) >> 8) & 0xff), static_cast<uint8_t>((y_end - 1) & 0xff),
        };
        ESP_RETURN_ON_ERROR(tx_param(device, LCD_CMD_RASET, row_data, sizeof(row_data)), kTag, "row window failed");
    }

    const size_t byte_count = static_cast<size_t>(x_end - x_start) * static_cast<size_t>(y_end - y_start) * device->bytes_per_pixel;
    return tx_color(device, y_start == 0 ? LCD_CMD_RAMWR : LCD_CMD_RAMWRC, color_data, byte_count);
}

static esp_err_t panel_invert(esp_lcd_panel_t *panel, bool invert)
{
    auto *device = as_panel(panel);
    return tx_param(device, invert ? LCD_CMD_INVON : LCD_CMD_INVOFF, nullptr, 0);
}

static esp_err_t panel_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    auto *device = as_panel(panel);
    if (mirror_x) device->madctl |= LCD_CMD_MX_BIT; else device->madctl &= ~LCD_CMD_MX_BIT;
    if (mirror_y) device->madctl |= LCD_CMD_MY_BIT; else device->madctl &= ~LCD_CMD_MY_BIT;
    return tx_param(device, LCD_CMD_MADCTL, &device->madctl, 1);
}

static esp_err_t panel_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    auto *device = as_panel(panel);
    if (swap_axes) device->madctl |= LCD_CMD_MV_BIT; else device->madctl &= ~LCD_CMD_MV_BIT;
    return tx_param(device, LCD_CMD_MADCTL, &device->madctl, 1);
}

static esp_err_t panel_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    auto *device = as_panel(panel);
    device->x_gap = x_gap;
    device->y_gap = y_gap;
    return ESP_OK;
}

static esp_err_t panel_display_on(esp_lcd_panel_t *panel, bool on)
{
    return tx_param(as_panel(panel), on ? LCD_CMD_DISPON : LCD_CMD_DISPOFF, nullptr, 0);
}

} // namespace

extern "C" esp_err_t lyra_axs15231b_new_panel(
    esp_lcd_panel_io_handle_t io,
    const esp_lcd_panel_dev_config_t *panel_config,
    esp_lcd_panel_handle_t *ret_panel)
{
    if (io == nullptr || panel_config == nullptr || ret_panel == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (panel_config->bits_per_pixel != 16) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    auto *device = static_cast<AxsPanel *>(std::calloc(1, sizeof(AxsPanel)));
    if (device == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    device->io = io;
    device->reset_gpio = panel_config->reset_gpio_num;
    device->reset_active_high = panel_config->flags.reset_active_high;
    device->madctl = panel_config->rgb_ele_order == LCD_RGB_ELEMENT_ORDER_BGR ? LCD_CMD_BGR_BIT : 0;
    device->colmod = 0x55;
    device->bytes_per_pixel = 2;
    device->use_qspi = true;

    if (panel_config->vendor_config != nullptr) {
        const auto *vendor = static_cast<const lyra_axs15231b_vendor_config_t *>(panel_config->vendor_config);
        device->init_cmds = vendor->init_cmds;
        device->init_cmds_count = vendor->init_cmds_count;
        device->use_qspi = vendor->use_qspi;
    }

    if (device->reset_gpio >= 0) {
        gpio_config_t reset_config{};
        reset_config.mode = GPIO_MODE_OUTPUT;
        reset_config.pin_bit_mask = 1ULL << device->reset_gpio;
        const esp_err_t ret = gpio_config(&reset_config);
        if (ret != ESP_OK) {
            std::free(device);
            return ret;
        }
    }

    device->base.del = panel_delete;
    device->base.reset = panel_reset;
    device->base.init = panel_init;
    device->base.draw_bitmap = panel_draw;
    device->base.invert_color = panel_invert;
    device->base.mirror = panel_mirror;
    device->base.swap_xy = panel_swap_xy;
    device->base.set_gap = panel_set_gap;
    device->base.disp_on_off = panel_display_on;
    *ret_panel = &device->base;
    ESP_LOGI(kTag, "AXS15231B native panel adapter ready");
    return ESP_OK;
}
