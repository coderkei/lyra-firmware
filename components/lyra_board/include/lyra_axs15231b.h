/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"
#include "esp_lcd_panel_dev.h"
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int command;
    const void *data;
    size_t data_bytes;
    uint32_t delay_ms;
} lyra_axs15231b_init_cmd_t;

typedef struct {
    const lyra_axs15231b_init_cmd_t *init_cmds;
    size_t init_cmds_count;
    bool use_qspi;
} lyra_axs15231b_vendor_config_t;

esp_err_t lyra_axs15231b_new_panel(
    esp_lcd_panel_io_handle_t io,
    const esp_lcd_panel_dev_config_t *panel_config,
    esp_lcd_panel_handle_t *ret_panel);

#ifdef __cplusplus
}
#endif
