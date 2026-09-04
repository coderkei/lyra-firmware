/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lyra_boot_test.h"

#include "esp_log.h"
#include "esp_image_format.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace lyra::boot_test {
namespace {

constexpr const char *kTag = "lyra.boot_test";
constexpr const char *kSettingsNamespace = "lyra";
constexpr const char *kPendingKey = "boot_test_pending";
constexpr const char *kOriginalAddressKey = "boot_test_orig";

esp_err_t ensure_nvs_ready()
{
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        result = nvs_flash_erase();
        if (result == ESP_OK) result = nvs_flash_init();
    }
    if (result == ESP_ERR_INVALID_STATE) result = ESP_OK;
    return result;
}

const esp_partition_t *find_app_at_address(uint32_t address)
{
    esp_partition_iterator_t iterator = esp_partition_find(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, nullptr);
    while (iterator) {
        const esp_partition_t *partition = esp_partition_get(iterator);
        if (partition && partition->address == address) {
            esp_partition_iterator_release(iterator);
            return partition;
        }
        iterator = esp_partition_next(iterator);
    }
    esp_partition_iterator_release(iterator);
    return nullptr;
}

esp_err_t clear_pending(nvs_handle_t handle)
{
    esp_err_t result = nvs_erase_key(handle, kPendingKey);
    if (result == ESP_ERR_NVS_NOT_FOUND) result = ESP_OK;
    if (result == ESP_OK) {
        result = nvs_erase_key(handle, kOriginalAddressKey);
        if (result == ESP_ERR_NVS_NOT_FOUND) result = ESP_OK;
    }
    if (result == ESP_OK) result = nvs_commit(handle);
    return result;
}

} // namespace

esp_err_t validate(uint8_t ota_slot)
{
    if (ota_slot > 1) return ESP_ERR_INVALID_ARG;
    const esp_partition_t *target = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP,
        static_cast<esp_partition_subtype_t>(ESP_PARTITION_SUBTYPE_APP_OTA_MIN + ota_slot),
        nullptr);
    if (!target) return ESP_ERR_NOT_FOUND;

    const esp_partition_pos_t position = {
        .offset = target->address,
        .size = target->size,
    };
    esp_image_metadata_t metadata{};
    return esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &position, &metadata);
}

esp_err_t request_once(uint8_t ota_slot)
{
    if (ota_slot > 1) return ESP_ERR_INVALID_ARG;
    const esp_partition_t *target = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP,
        static_cast<esp_partition_subtype_t>(ESP_PARTITION_SUBTYPE_APP_OTA_MIN + ota_slot),
        nullptr);
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!target || !running) return ESP_ERR_NOT_FOUND;

    const esp_err_t validation_result = validate(ota_slot);
    if (validation_result != ESP_OK) return validation_result;

    const esp_err_t nvs_result = ensure_nvs_ready();
    if (nvs_result != ESP_OK) return nvs_result;
    nvs_handle_t handle;
    esp_err_t result = nvs_open(kSettingsNamespace, NVS_READWRITE, &handle);
    if (result != ESP_OK) return result;
    result = nvs_set_u8(handle, kPendingKey, 1);
    if (result == ESP_OK) result = nvs_set_u32(handle, kOriginalAddressKey, running->address);
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    if (result != ESP_OK) return result;

    // ESP-IDF validates the image before changing OTA data. The startup
    // restore marker above makes this boot selection one-shot for Lyra images.
    result = esp_ota_set_boot_partition(target);
    if (result != ESP_OK) {
        if (nvs_open(kSettingsNamespace, NVS_READWRITE, &handle) == ESP_OK) {
            clear_pending(handle);
            nvs_close(handle);
        }
        return result;
    }
    ESP_LOGI(kTag, "one-shot boot requested for %s", target->label);
    esp_restart();
    return ESP_FAIL;
}

void restore_pending()
{
    if (ensure_nvs_ready() != ESP_OK) return;

    nvs_handle_t handle;
    if (nvs_open(kSettingsNamespace, NVS_READWRITE, &handle) != ESP_OK) return;
    uint8_t pending = 0;
    uint32_t original_address = 0;
    const bool has_pending = nvs_get_u8(handle, kPendingKey, &pending) == ESP_OK && pending == 1 &&
                             nvs_get_u32(handle, kOriginalAddressKey, &original_address) == ESP_OK;
    if (!has_pending) {
        nvs_close(handle);
        return;
    }

    const esp_partition_t *original = find_app_at_address(original_address);
    const esp_err_t result = original ? esp_ota_set_boot_partition(original) : ESP_ERR_NOT_FOUND;
    if (result == ESP_OK) {
        const esp_err_t clear_result = clear_pending(handle);
        if (clear_result != ESP_OK) {
            ESP_LOGW(kTag, "test boot restored but marker could not be cleared: %s",
                     esp_err_to_name(clear_result));
        } else {
            ESP_LOGI(kTag, "restored persistent boot partition to %s", original->label);
        }
    } else {
        ESP_LOGW(kTag, "could not restore persistent boot partition: %s",
                 esp_err_to_name(result));
    }
    nvs_close(handle);
}

} // namespace lyra::boot_test
