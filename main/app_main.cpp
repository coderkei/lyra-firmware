/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lyra_gui.h"
#include "lyra_audio.h"
#include "lyra_boot_test.h"
#include "lyra_media.h"

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lyra_board.h"

namespace {

TaskHandle_t s_splash_waiter;

void render_splash_task(void *context)
{
    lv_refr_now(static_cast<lv_display_t *>(context));
    xTaskNotifyGive(s_splash_waiter);
    vTaskDelete(nullptr);
}

bool render_splash_with_safe_stack(lv_display_t *display)
{
    s_splash_waiter = xTaskGetCurrentTaskHandle();
    // A full direct-mode LVGL refresh needs substantially more stack than
    // ESP-IDF's main task provides. Keep this one-shot renderer isolated; the
    // normal LVGL worker starts only after the final GUI tree is ready.
    const BaseType_t created = xTaskCreatePinnedToCore(
        render_splash_task, "lyra_splash", 12288, display, 5, nullptr, 1);
    if (created != pdPASS) return false;
    return ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000)) != 0;
}

} // namespace

extern "C" void app_main(void)
{
    static const char *kTag = "lyra.app";
    lyra_board_display_t board{};

    // A debug OTA boot is marked before reboot and restored before any normal
    // startup work, making the selected slot a one-shot test.
    lyra::boot_test::restore_pending();

    ESP_LOGI(kTag, "Reset reason: %d", static_cast<int>(esp_reset_reason()));

    const esp_err_t board_ret = lyra_board_display_init(&board);
    if (board_ret != ESP_OK) {
        ESP_LOGE(kTag, "board display init failed: %s", esp_err_to_name(board_ret));
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    if (!board.touch_available) {
        ESP_LOGW(kTag, "touch is unavailable; GUI can still be inspected over a future test input adapter");
    }

    // Render a complete frame while the backlight is still off. The splash
    // remains in panel RAM during SD mounting and catalog restore, replacing
    // the controller's power-on garbage with a stable
    // Lyra boot screen.
    const esp_err_t splash_ret = lyra_gui_show_boot(board.display);
    if (splash_ret != ESP_OK) {
        ESP_LOGE(kTag, "boot screen creation failed: %s", esp_err_to_name(splash_ret));
    } else if (!render_splash_with_safe_stack(board.display)) {
        ESP_LOGE(kTag, "boot screen transfer did not complete");
    } else {
        // The panel transfer is asynchronous. Allow its final queued chunk to
        // complete before revealing the LCD.
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    const esp_err_t backlight_ret = lyra_board_display_set_backlight(true);
    if (backlight_ret != ESP_OK) {
        ESP_LOGE(kTag, "backlight enable failed: %s", esp_err_to_name(backlight_ret));
    }

    const esp_err_t media_ret = lyra::media::init();
    if (media_ret != ESP_OK) {
        ESP_LOGW(kTag, "MicroSD unavailable at startup: %s; insert a card and use Scan to retry",
                 esp_err_to_name(media_ret));
    }
    const esp_err_t audio_ret = lyra::audio::init();
    if (audio_ret != ESP_OK) {
        ESP_LOGW(kTag, "audio decoder unavailable: %s", esp_err_to_name(audio_ret));
    }
    const esp_err_t gui_ret = lyra_gui_start(board.display);
    if (gui_ret != ESP_OK) {
        ESP_LOGE(kTag, "GUI init failed: %s", esp_err_to_name(gui_ret));
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    const esp_err_t start_ret = lyra_board_display_start();
    if (start_ret != ESP_OK) {
        ESP_LOGE(kTag, "LVGL worker start failed: %s", esp_err_to_name(start_ret));
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    ESP_LOGI(kTag, "Lyra GUI is running; MicroSD catalog is ready for a user-initiated scan");
}
