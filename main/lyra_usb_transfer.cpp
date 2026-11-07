/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lyra_usb_transfer.h"

#include <cstdlib>

#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lyra_board_pins.h"
#include "sdkconfig.h"
#include "sdmmc_cmd.h"

#if CONFIG_TINYUSB_MSC_ENABLED
#include "tinyusb.h"
#include "tinyusb_msc.h"
#endif

namespace lyra::usb_transfer {
namespace {

constexpr const char *kTag = "lyra.usb";

#if CONFIG_TINYUSB_MSC_ENABLED
using namespace lyra::board::jc3248w535en;

constexpr int kCardAttempts = 4;
constexpr int kFallbackFrequencyKHz = 10000;
sdmmc_card_t *s_card = nullptr;
tinyusb_msc_storage_handle_t s_storage = nullptr;

// Same bus setup as the media service's mount: 1-bit with internal pull-ups,
// dropping to a slower clock when the card does not answer at full speed.
esp_err_t init_card()
{
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = static_cast<gpio_num_t>(kSdClockGpio);
    slot.cmd = static_cast<gpio_num_t>(kSdCommandGpio);
    slot.d0 = static_cast<gpio_num_t>(kSdData0Gpio);
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t result = sdmmc_host_init();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) return result;
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    result = sdmmc_host_init_slot(host.slot, &slot);
    if (result != ESP_OK) {
        sdmmc_host_deinit();
        return result;
    }
    s_card = static_cast<sdmmc_card_t *>(calloc(1, sizeof(sdmmc_card_t)));
    if (!s_card) {
        sdmmc_host_deinit();
        return ESP_ERR_NO_MEM;
    }
    for (int attempt = 0; attempt < kCardAttempts; ++attempt) {
        host.max_freq_khz = attempt < 2 ? SDMMC_FREQ_DEFAULT : kFallbackFrequencyKHz;
        result = sdmmc_card_init(&host, s_card);
        if (result == ESP_OK) return ESP_OK;
        ESP_LOGW(kTag, "MicroSD init attempt %d/%d failed: %s", attempt + 1, kCardAttempts,
                 esp_err_to_name(result));
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    free(s_card);
    s_card = nullptr;
    sdmmc_host_deinit();
    return result;
}
#endif

} // namespace

bool available()
{
#if CONFIG_TINYUSB_MSC_ENABLED
    return true;
#else
    return false;
#endif
}

esp_err_t start()
{
#if CONFIG_TINYUSB_MSC_ENABLED
    if (s_storage) return ESP_OK;
    esp_err_t result = init_card();
    if (result != ESP_OK) return result;

    tinyusb_msc_storage_config_t storage{};
    storage.medium.card = s_card;
    storage.mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB;
    storage.fat_fs.base_path = nullptr;
    storage.fat_fs.config.max_files = 4;
    // Never format someone's music card, whatever the computer reports.
    storage.fat_fs.do_not_format = true;
    storage.fat_fs.format_flags = 0;
    result = tinyusb_msc_new_storage_sdmmc(&storage, &s_storage);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "cannot create USB storage: %s", esp_err_to_name(result));
        return result;
    }

    // Field by field rather than TINYUSB_DEFAULT_CONFIG(): its C compound
    // literal is not portable C++. Descriptors come from Kconfig.
    tinyusb_config_t usb{};
    usb.port = TINYUSB_PORT_FULL_SPEED_0;
    usb.phy.skip_setup = false;
    usb.phy.self_powered = false;
    usb.phy.vbus_monitor_io = -1;
    usb.task.size = 4096;
    usb.task.priority = 5;
    usb.task.xCoreID = 1;
    result = tinyusb_driver_install(&usb);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "cannot start USB device: %s", esp_err_to_name(result));
        tinyusb_msc_delete_storage(s_storage);
        s_storage = nullptr;
        return result;
    }
    ESP_LOGI(kTag, "MicroSD exposed as a USB drive; serial/JTAG is unavailable until restart");
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

void finish_and_restart()
{
#if CONFIG_TINYUSB_MSC_ENABLED
    if (s_storage) {
        // Fails only while writes are pending; the restart below still
        // detaches the drive, as unplugging the cable would.
        const esp_err_t result = tinyusb_msc_delete_storage(s_storage);
        if (result != ESP_OK) ESP_LOGW(kTag, "USB storage still busy: %s", esp_err_to_name(result));
        s_storage = nullptr;
    }
#endif
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

} // namespace lyra::usb_transfer
