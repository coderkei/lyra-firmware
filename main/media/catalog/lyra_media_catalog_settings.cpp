/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../lyra_media_internal.h"

namespace lyra::media::internal {

void refresh_capacity();
void start_duration_indexing_if_needed();
void start_sort_cache_indexing_locked(SortSection section);

esp_err_t mount_sd_card()
{
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = static_cast<gpio_num_t>(kSdClockGpio);
    slot.cmd = static_cast<gpio_num_t>(kSdCommandGpio);
    slot.d0 = static_cast<gpio_num_t>(kSdData0Gpio);
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mount_config{};
    mount_config.format_if_mount_failed = false;
    // Only progressive covers whose coefficient arrays cannot fit safely in
    // PSRAM use transient SD working files. Their final decoded image is kept
    // in the SD artwork cache so the expensive spill decode is not repeated.
    mount_config.max_files = 12;
    mount_config.allocation_unit_size = 16 * 1024;

    esp_err_t result = ESP_FAIL;
    s_card = nullptr;
    for (int attempt = 0; attempt < kSdMountAttempts; ++attempt) {
        const int frequency_khz = attempt < 2 ? SDMMC_FREQ_DEFAULT : kSdFallbackFrequencyKHz;
        const int delay_ms = attempt == 0 ? kSdInitialSettleMs : kSdRetryDelayMs;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));

        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        host.max_freq_khz = frequency_khz;
        sdmmc_card_t *card = nullptr;
        ESP_LOGI(kTag, "MicroSD mount attempt %d/%d at %d kHz",
                 attempt + 1, kSdMountAttempts, frequency_khz);
        result = esp_vfs_fat_sdmmc_mount(kMount, &host, &slot, &mount_config, &card);
        if (result == ESP_OK) {
            s_card = card;
            if (frequency_khz != SDMMC_FREQ_DEFAULT) {
                ESP_LOGW(kTag, "MicroSD mounted at fallback %d kHz; check socket pullups and 3.3 V stability",
                         frequency_khz);
            }
            return ESP_OK;
        }

        // The VFS helper tears down the failed host/card initialization. Do
        // not retain an output pointer from a failed attempt; the next pass
        // must start with a clean card handle and host configuration.
        s_card = nullptr;
        ESP_LOGW(kTag, "MicroSD mount attempt %d/%d failed: %s",
                 attempt + 1, kSdMountAttempts, esp_err_to_name(result));
    }
    return result;
}

esp_err_t ensure_nvs_ready()
{
    if (s_nvs_ready) return ESP_OK;
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        result = nvs_flash_erase();
        if (result == ESP_OK) result = nvs_flash_init();
    }
    if (result == ESP_ERR_INVALID_STATE) result = ESP_OK;
    if (result == ESP_OK) s_nvs_ready = true;
    return result;
}

void load_artwork_settings()
{
    s_status.artwork_sd_cache_enabled = true;
    s_status.artwork_size = kDefaultArtworkSize;
    if (!s_nvs_ready) return;
    nvs_handle_t handle;
    if (nvs_open(kSettingsNamespace, NVS_READONLY, &handle) != ESP_OK) return;
    uint8_t cache_enabled = 1;
    uint16_t size = kDefaultArtworkSize;
    if (nvs_get_u8(handle, kArtworkSdCacheKey, &cache_enabled) == ESP_OK &&
        cache_enabled <= 1) {
        s_status.artwork_sd_cache_enabled = cache_enabled != 0;
    }
    if (nvs_get_u16(handle, kArtworkSizeKey, &size) == ESP_OK &&
        (size == kDefaultArtworkSize || size == kLargeArtworkSize)) {
        s_status.artwork_size = size;
    }
    nvs_close(handle);
}

esp_err_t save_artwork_settings()
{
    if (!s_nvs_ready) return ESP_ERR_INVALID_STATE;
    nvs_handle_t handle;
    esp_err_t result = nvs_open(kSettingsNamespace, NVS_READWRITE, &handle);
    if (result != ESP_OK) return result;
    result = nvs_set_u8(handle, kArtworkSdCacheKey,
                        s_status.artwork_sd_cache_enabled ? 1 : 0);
    if (result == ESP_OK) result = nvs_set_u16(handle, kArtworkSizeKey,
                                               s_status.artwork_size);
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result;
}

uint8_t sort_setting_code(SortSetting setting)
{
    return static_cast<uint8_t>(static_cast<uint8_t>(setting.field) * 2u +
                                static_cast<uint8_t>(setting.direction));
}

bool decode_sort_setting(uint8_t code, SortSetting *setting)
{
    if (!setting || code >= 12) return false;
    setting->field = static_cast<SortField>(code / 2u);
    setting->direction = static_cast<SortDirection>(code % 2u);
    return true;
}

void load_sort_settings()
{
    if (!s_nvs_ready) return;
    nvs_handle_t handle;
    if (nvs_open(kSettingsNamespace, NVS_READONLY, &handle) != ESP_OK) return;
    const char *keys[] = {kSongsSortKey, kAlbumsSortKey, kArtistsSortKey};
    for (size_t index = 0; index < 3; ++index) {
        uint8_t code = sort_setting_code(s_sort_settings[index]);
        if (nvs_get_u8(handle, keys[index], &code) == ESP_OK) {
            SortSetting loaded{};
            if (decode_sort_setting(code, &loaded)) s_sort_settings[index] = loaded;
        }
    }
    nvs_close(handle);
}

esp_err_t save_sort_setting(SortSection section)
{
    if (!s_nvs_ready) return ESP_ERR_INVALID_STATE;
    const char *key = section == SortSection::Songs ? kSongsSortKey :
                      section == SortSection::Albums ? kAlbumsSortKey : kArtistsSortKey;
    nvs_handle_t handle;
    esp_err_t result = nvs_open(kSettingsNamespace, NVS_READWRITE, &handle);
    if (result != ESP_OK) return result;
    const size_t index = static_cast<size_t>(section);
    result = nvs_set_u8(handle, key, sort_setting_code(s_sort_settings[index]));
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result;
}

SortSetting current_sort_setting(SortSection section)
{
    const size_t index = static_cast<size_t>(section);
    return index < 3 ? s_sort_settings[index] : s_sort_settings[0];
}

const char *sort_cache_path(SortSection section)
{
    return section == SortSection::Songs ? kSongsSortCachePath :
           section == SortSection::Albums ? kAlbumsSortCachePath : kArtistsSortCachePath;
}

bool load_sort_cache_locked(SortSection section, uint8_t sort_code,
                            uint32_t item_count, uint32_t **order_slot)
{
    if (!order_slot || item_count == 0) return false;
    FILE *file = std::fopen(sort_cache_path(section), "rb");
    if (!file) return false;
    SortCacheHeader header{};
    const bool header_ok = std::fread(&header, sizeof(header), 1, file) == 1 &&
                           std::memcmp(header.magic, kSortCacheMagic, sizeof(header.magic)) == 0 &&
                           header.version == kSortCacheVersion &&
                           header.catalog_checksum == s_catalog_header.checksum &&
                           header.item_count == item_count &&
                           header.section == static_cast<uint8_t>(section) &&
                           header.sort_code == sort_code;
    auto *order = header_ok ? static_cast<uint32_t *>(heap_caps_malloc(
        item_count * sizeof(uint32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) : nullptr;
    const bool read = header_ok && order &&
                      std::fread(order, sizeof(uint32_t), item_count, file) == item_count;
    std::fclose(file);
    if (!read) {
        heap_caps_free(order);
        return false;
    }
    for (uint32_t index = 0; index < item_count; ++index) {
        if (order[index] >= item_count) {
            heap_caps_free(order);
            return false;
        }
    }
    *order_slot = order;
    return true;
}

void save_sort_cache_locked(SortSection section, SortSetting setting,
                            const uint32_t *order, uint32_t item_count)
{
    if (!order || item_count == 0 ||
        (setting.field == SortField::Duration && s_status.duration_indexing)) return;
    const char *path = sort_cache_path(section);
    char temporary[kMaxPath];
    if (std::snprintf(temporary, sizeof(temporary), "%s.tmp", path) <= 0) return;
    std::remove(temporary);
    FILE *file = std::fopen(temporary, "wb");
    if (!file) return;
    SortCacheHeader header{};
    std::memcpy(header.magic, kSortCacheMagic, sizeof(header.magic));
    header.version = kSortCacheVersion;
    header.catalog_checksum = s_catalog_header.checksum;
    header.item_count = item_count;
    header.section = static_cast<uint8_t>(section);
    header.sort_code = sort_setting_code(setting);
    const bool written = std::fwrite(&header, sizeof(header), 1, file) == 1 &&
                         std::fwrite(order, sizeof(uint32_t), item_count, file) == item_count &&
                         std::fflush(file) == 0 && fsync(fileno(file)) == 0;
    const bool closed = std::fclose(file) == 0;
    if (!written || !closed) {
        std::remove(temporary);
        return;
    }
    std::remove(path);
    if (std::rename(temporary, path) != 0) std::remove(temporary);
}

} // namespace lyra::media::internal
