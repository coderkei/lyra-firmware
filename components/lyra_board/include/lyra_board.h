/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LYRA_BOARD_DISPLAY_WIDTH  320
#define LYRA_BOARD_DISPLAY_HEIGHT 480

typedef struct {
    lv_display_t *display;
    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_io_handle_t panel_io;
    bool touch_available;
} lyra_board_display_t;

esp_err_t lyra_board_display_init(lyra_board_display_t *board);

// The panel is initialized with its backlight off so uninitialized LCD RAM is
// never exposed. Enable it only after the application has transferred its
// first complete frame.
esp_err_t lyra_board_display_set_backlight(bool enabled);

// Sets the LCD backlight duty cycle. The value is expressed as a percentage
// so the UI does not need to know the LEDC timer resolution used by the board.
esp_err_t lyra_board_display_set_brightness(uint8_t brightness_percent);

// Start the LVGL worker after the initial screen has been created. Keeping
// this separate from init prevents the worker from racing app_main while it
// builds the first LVGL object tree.
esp_err_t lyra_board_display_start(void);

#ifdef __cplusplus
}
#endif
