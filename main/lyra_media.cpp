/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lyra_media.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lyra_audio.h"
#include "lyra_board_pins.h"
#include "lyra_png.h"
#include "lyra_progressive_jpeg.h"
#include "lyra_sd.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdmmc_cmd.h"

namespace lyra::media {

// These helpers are defined below the anonymous implementation namespace,
// but the background search task needs to call them.
void group_location(GroupKind kind, uint32_t *offset, uint32_t *count);
bool find_track_by_path_locked(const char *path, size_t *track_index);
bool track_at_locked(size_t index, Track *out);

namespace {

using namespace lyra::board::jc3248w535en;

constexpr const char *kTag = "lyra.media";
constexpr const char *kMount = "/sdcard";
constexpr const char *kPlaylistDir = "/sdcard/Playlists";
constexpr const char *kFavoritesPath = "/sdcard/Playlists/Favorites.m3u8";
constexpr const char *kFavoritesTempPath = "/sdcard/Playlists/Favorites.tmp";
constexpr const char *kFavoritesBackupPath = "/sdcard/Playlists/Favorites.bak";
constexpr const char *kPlaylistEditTempPath = "/sdcard/Playlists/.lyra-edit.tmp";
constexpr const char *kPlaylistEditBackupPath = "/sdcard/Playlists/.lyra-edit.bak";
constexpr const char *kDataDir = "/sdcard/.lyra";
constexpr const char *kQueueSnapshotPath = "/sdcard/.lyra/queue-v1.m3u8";
constexpr const char *kQueueSnapshotTempPath = "/sdcard/.lyra/queue-v1.tmp";
constexpr const char *kQueueSnapshotBackupPath = "/sdcard/.lyra/queue-v1.bak";
constexpr const char *kArtworkDir = "/sdcard/.lyra/covers";
constexpr const char *kCatalogPath = "/sdcard/.lyra/catalog-v10.bin";
constexpr const char *kCatalogTempPath = "/sdcard/.lyra/catalog-v10.tmp";
constexpr const char *kCatalogBackupPath = "/sdcard/.lyra/catalog-v10.bak";
// A scan can hold several recursive directory frames while artwork decoding
// runs on the background core. Keep the worker isolated from the UI with
// enough stack for the FAT/VFS and decoder call chains.
constexpr size_t kScanStack = 24 * 1024;
constexpr size_t kDurationStack = 12 * 1024;
constexpr size_t kSortStack = 16 * 1024;
constexpr size_t kArtworkReadChunkBytes = 8192;
constexpr size_t kMaximumEmbeddedImageBytes = 16u * 1024u * 1024u;
constexpr size_t kMaximumOggCommentPacketBytes = 2u * 1024u * 1024u;
// A card can still be completing its power-up/reset sequence when the
// application starts, especially after flashing while the socket remains
// powered. Keep the normal 20 MHz path fast, but give transient bring-up
// failures several chances to recover before declaring the card unavailable.
constexpr int kSdMountAttempts = 5;
constexpr int kSdInitialSettleMs = 250;
constexpr int kSdRetryDelayMs = 250;
constexpr int kSdFallbackFrequencyKHz = 10000;
// Only decoded covers that required libjpeg's SD backing store are persisted.
// Versioning prevents an older raw-cache layout from being accepted silently.
constexpr uint32_t kLargeArtworkCacheVersion = 1;
constexpr const char *kSettingsNamespace = "lyra";
constexpr const char *kArtworkSdCacheKey = "art_sd_cache";
constexpr const char *kArtworkSizeKey = "art_size";
constexpr const char *kSongsSortKey = "sort_songs";
constexpr const char *kAlbumsSortKey = "sort_albums";
constexpr const char *kArtistsSortKey = "sort_artists";
constexpr const char *kSongsSortCachePath = "/sdcard/.lyra/sort-songs-v1.bin";
constexpr const char *kAlbumsSortCachePath = "/sdcard/.lyra/sort-albums-v1.bin";
constexpr const char *kArtistsSortCachePath = "/sdcard/.lyra/sort-artists-v1.bin";
constexpr char kSortCacheMagic[8] = {'L', 'Y', 'R', 'A', 'S', 'O', 'R', 'T'};
constexpr uint32_t kSortCacheVersion = 1;
// Keep cover extraction and scaling off PRO_CPU, which owns realtime audio.
constexpr BaseType_t kArtworkCore = 1;

struct CatalogHeader {
    char magic[8];
    uint32_t version;
    uint32_t header_size;
    uint32_t track_count;
    uint32_t track_size;
    uint32_t records_offset;
    uint32_t title_order_offset;
    uint32_t path_index_offset;
    uint32_t artist_group_offset;
    uint32_t artist_group_count;
    uint32_t album_group_offset;
    uint32_t album_group_count;
    uint32_t genre_group_offset;
    uint32_t genre_group_count;
    uint32_t year_group_offset;
    uint32_t year_group_count;
    uint32_t checksum;
    uint32_t file_size;
    uint32_t flags;
};

struct GroupRecord {
    char name[kMaxName];
    uint32_t track_count;
    uint32_t members_offset;
    uint32_t representative_track;
};

struct PathIndex {
    uint64_t hash;
    uint32_t track_index;
    uint32_t reserved;
};

struct SortKey {
    char value[kMaxName];
    uint32_t physical_index;
};

struct SortEntry {
    uint32_t source_index;
    char primary[kMaxName];
    char secondary[kMaxName];
    uint64_t numeric;
};

struct SortCacheHeader {
    char magic[8];
    uint32_t version;
    uint32_t catalog_checksum;
    uint32_t item_count;
    uint8_t section;
    uint8_t sort_code;
    uint16_t reserved;
};

struct GroupTrackEntry {
    uint32_t logical_index;
    uint32_t disc_number;
    uint32_t track_number;
    char title[kMaxName];
};

struct CachedTrack {
    uint32_t physical_index;
    uint32_t generation;
    Track track;
};

constexpr char kCatalogMagic[8] = {'L', 'Y', 'R', 'A', 'C', 'A', 'T', '8'};
constexpr uint32_t kCatalogVersion = 10;
constexpr uint32_t kCatalogFlagCapacityReached = 1u << 0;
constexpr size_t kTrackCacheSize = 24;
constexpr size_t kArtworkQueueSize = 16;

struct ArtworkRequest {
    Track track;
    ArtworkSize size;
};

FILE *s_catalog;
char s_catalog_path[kMaxPath]{};
CatalogHeader s_catalog_header{};
uint32_t *s_title_order;
uint32_t *s_physical_to_logical;
PathIndex *s_path_index;
uint32_t *s_song_sort_order;
uint32_t *s_album_sort_order;
uint32_t *s_artist_sort_order;
uint32_t *s_artist_album_order;
uint32_t *s_album_for_logical;
uint32_t *s_duration_cache;
uint8_t *s_duration_ready;
size_t s_artist_album_count;
size_t s_artist_album_artist = SIZE_MAX;
uint32_t s_artist_album_generation;
uint32_t s_song_sort_generation;
uint32_t s_album_sort_generation;
uint32_t s_artist_sort_generation;
uint8_t s_song_sort_code;
uint8_t s_album_sort_code;
uint8_t s_artist_sort_code;
CachedTrack *s_track_cache;
size_t s_cache_cursor;
ArtworkRequest s_artwork_queue[kArtworkQueueSize]{};
size_t s_artwork_queue_head;
size_t s_artwork_queue_count;
bool s_artwork_task_running;
uint64_t s_artwork_active_hash;
uint16_t *s_artwork_pixels;
uint16_t s_artwork_pixel_size;
uint32_t s_artwork_key;
bool s_artwork_valid;
uint32_t s_artwork_failed_key;
bool s_artwork_failed;
Playlist s_playlists[kMaxPlaylists]{};
size_t s_track_count;
size_t s_playlist_count;
SemaphoreHandle_t s_mutex;
sdmmc_card_t *s_card;
Status s_status{};
bool s_shutdown_requested;
ArtworkDiagnostics s_artwork_diagnostics{};
SearchStatus s_search_status{};
bool s_search_cancel_requested;
bool s_duration_cancel_requested;
uint32_t s_duration_catalog_token;
bool s_sort_cancel_requested;
uint32_t s_sort_catalog_token;
SearchResult *s_search_results;
SearchCategory s_cached_search_category = SearchCategory::Songs;
char s_cached_search_query[48]{};
bool s_nvs_ready;
SortSetting s_sort_settings[] = {
    {SortField::Title, SortDirection::Ascending},
    {SortField::Title, SortDirection::Ascending},
    {SortField::Title, SortDirection::Ascending},
};

class Lock {
public:
    Lock() { if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY); }
    ~Lock() { if (s_mutex) xSemaphoreGive(s_mutex); }
};

void refresh_capacity();
void start_duration_indexing_if_needed();
void start_sort_cache_indexing_locked(SortSection section);
bool build_song_sort_order_locked();
bool build_group_sort_order_locked(SortSection section);

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

void warm_sort_caches_locked()
{
    const SortSetting songs = current_sort_setting(SortSection::Songs);
    const SortSetting albums = current_sort_setting(SortSection::Albums);
    const SortSetting artists = current_sort_setting(SortSection::Artists);
    if (songs.field != SortField::Duration &&
        (songs.field != SortField::Title || songs.direction != SortDirection::Ascending)) {
        build_song_sort_order_locked();
    }
    if (albums.field != SortField::Duration &&
        (albums.field != SortField::Title || albums.direction != SortDirection::Ascending)) {
        build_group_sort_order_locked(SortSection::Albums);
    }
    if (artists.field != SortField::Duration &&
        (artists.field != SortField::Title || artists.direction != SortDirection::Ascending)) {
        build_group_sort_order_locked(SortSection::Artists);
    }
}

void copy_text(char *destination, size_t capacity, const char *source)
{
    if (capacity == 0) return;
    if (!source) source = "";
    const size_t length = std::min(std::strlen(source), capacity - 1);
    std::memcpy(destination, source, length);
    destination[length] = '\0';
}

bool join_path(char *destination, size_t capacity, const char *parent, const char *child)
{
    if (!destination || capacity == 0 || !parent || !child) return false;
    const size_t parent_length = std::strlen(parent);
    const bool add_separator = parent_length > 0 && parent[parent_length - 1] != '/' && child[0] != '/';
    const size_t child_length = std::strlen(child);
    const size_t required = parent_length + (add_separator ? 1 : 0) + child_length + 1;
    if (required > capacity) {
        destination[0] = '\0';
        return false;
    }
    std::memcpy(destination, parent, parent_length);
    size_t offset = parent_length;
    if (add_separator) destination[offset++] = '/';
    std::memcpy(destination + offset, child, child_length);
    destination[offset + child_length] = '\0';
    return true;
}

bool equals_ci(const char *left, const char *right)
{
    while (*left && *right) {
        if (std::tolower(static_cast<unsigned char>(*left)) !=
            std::tolower(static_cast<unsigned char>(*right))) return false;
        ++left;
        ++right;
    }
    return *left == *right;
}

bool contains_ci(const char *text, const char *query)
{
    if (!query || !*query) return true;
    const size_t query_length = std::strlen(query);
    for (; *text; ++text) {
        size_t i = 0;
        while (i < query_length && text[i] &&
               std::tolower(static_cast<unsigned char>(text[i])) ==
                   std::tolower(static_cast<unsigned char>(query[i]))) ++i;
        if (i == query_length) return true;
    }
    return false;
}

uint64_t hash_path(const char *path)
{
    uint64_t hash = 14695981039346656037ull;
    for (const uint8_t *p = reinterpret_cast<const uint8_t *>(path); *p; ++p) {
        hash = (hash ^ *p) * 1099511628211ull;
    }
    return hash;
}

uint32_t checksum_update(uint32_t hash, const void *data, size_t length)
{
    const auto *bytes = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < length; ++i) hash = (hash ^ bytes[i]) * 16777619u;
    return hash;
}

bool checksum_file_payload(FILE *file, uint32_t start, uint32_t end, uint32_t *out)
{
    if (!file || !out || end < start || std::fseek(file, start, SEEK_SET) != 0) return false;
    constexpr size_t kChecksumBufferSize = 4096;
    auto *buffer = static_cast<uint8_t *>(heap_caps_malloc(kChecksumBufferSize, MALLOC_CAP_INTERNAL));
    if (!buffer) return false;
    uint32_t hash = 2166136261u;
    uint32_t remaining = end - start;
    while (remaining) {
        const size_t wanted = std::min<size_t>(kChecksumBufferSize, remaining);
        const size_t read = std::fread(buffer, 1, wanted, file);
        if (read != wanted) {
            heap_caps_free(buffer);
            return false;
        }
        hash = checksum_update(hash, buffer, read);
        remaining -= static_cast<uint32_t>(read);
    }
    heap_caps_free(buffer);
    *out = hash;
    return true;
}

const char *extension_of(const char *name)
{
    const char *dot = std::strrchr(name, '.');
    return dot && dot[1] ? dot + 1 : "";
}

bool compatible_audio(const char *name)
{
    const char *ext = extension_of(name);
    constexpr const char *formats[] = {"mp3", "wav", "flac", "aac", "m4a", "ogg", "opus",
                                       "aiff", "aif", "aifc"};
    for (const char *format : formats) if (equals_ci(ext, format)) return true;
    return false;
}

const char *base_name(const char *path)
{
    const char *slash = std::strrchr(path, '/');
    return slash ? slash + 1 : path;
}

void parent_name(const char *path, char *out, size_t capacity)
{
    char scratch[kMaxPath];
    copy_text(scratch, sizeof(scratch), path);
    char *slash = std::strrchr(scratch, '/');
    if (slash) *slash = '\0';
    copy_text(out, capacity, base_name(scratch));
}

void metadata_from_path(Track *track)
{
    char stem[kMaxName];
    copy_text(stem, sizeof(stem), base_name(track->path));
    char *dot = std::strrchr(stem, '.');
    if (dot) *dot = '\0';
    char *separator = std::strstr(stem, " - ");
    if (separator) {
        *separator = '\0';
        copy_text(track->artist, sizeof(track->artist), stem);
        copy_text(track->title, sizeof(track->title), separator + 3);
    } else {
        copy_text(track->title, sizeof(track->title), stem);
        copy_text(track->artist, sizeof(track->artist), "Unknown artist");
    }
    parent_name(track->path, track->album, sizeof(track->album));
    track->album_artist[0] = '\0';
    copy_text(track->composer, sizeof(track->composer), "Unknown");
    copy_text(track->genre, sizeof(track->genre), "Unknown genre");
    copy_text(track->year, sizeof(track->year), "Unknown");
    copy_text(track->format, sizeof(track->format), extension_of(track->path));
}

uint32_t read_be32(const uint8_t *bytes)
{
    return (static_cast<uint32_t>(bytes[0]) << 24) |
           (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) |
           static_cast<uint32_t>(bytes[3]);
}

uint64_t read_be64(const uint8_t *bytes)
{
    return (static_cast<uint64_t>(read_be32(bytes)) << 32) | read_be32(bytes + 4);
}

uint32_t read_syncsafe32(const uint8_t *bytes)
{
    if ((bytes[0] | bytes[1] | bytes[2] | bytes[3]) & 0x80) return 0;
    return (static_cast<uint32_t>(bytes[0]) << 21) |
           (static_cast<uint32_t>(bytes[1]) << 14) |
           (static_cast<uint32_t>(bytes[2]) << 7) |
           static_cast<uint32_t>(bytes[3]);
}

uint32_t read_le32(const uint8_t *bytes)
{
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) |
           (static_cast<uint32_t>(bytes[3]) << 24);
}

uint32_t parse_tag_number(const char *text)
{
    if (!text) return 0;
    while (*text && !std::isdigit(static_cast<unsigned char>(*text))) ++text;
    if (!*text) return 0;
    char *end = nullptr;
    const unsigned long value = std::strtoul(text, &end, 10);
    return value > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(value);
}

int16_t parse_replay_gain_tenths_db(const char *text)
{
    if (!text) return 0;
    char *end = nullptr;
    const float decibels = std::strtof(text, &end);
    if (end == text || !std::isfinite(decibels)) return 0;
    constexpr float kMinimumReplayGainDb = -12.0f;
    constexpr float kMaximumReplayGainDb = 12.0f;
    return static_cast<int16_t>(std::lround(
        std::clamp(decibels, kMinimumReplayGainDb, kMaximumReplayGainDb) * 10.0f));
}

void copy_tag_text(char *destination, size_t capacity, const uint8_t *source,
                   size_t length, uint8_t encoding = 3)
{
    if (!destination || capacity == 0 || !source || length == 0) return;
    size_t used = 0;
    if (encoding == 1 || encoding == 2) {
        bool little_endian = encoding == 1;
        size_t position = 0;
        if (length >= 2 && source[0] == 0xFF && source[1] == 0xFE) {
            little_endian = true;
            position = 2;
        } else if (length >= 2 && source[0] == 0xFE && source[1] == 0xFF) {
            little_endian = false;
            position = 2;
        }
        while (position + 1 < length && used + 1 < capacity) {
            const uint16_t value = little_endian
                ? static_cast<uint16_t>(source[position] | (source[position + 1] << 8))
                : static_cast<uint16_t>((source[position] << 8) | source[position + 1]);
            position += 2;
            if (value == 0) break;
            if (value < 0x80) destination[used++] = static_cast<char>(value);
            else if (value < 0x800 && used + 2 < capacity) {
                destination[used++] = static_cast<char>(0xC0 | (value >> 6));
                destination[used++] = static_cast<char>(0x80 | (value & 0x3F));
            } else if (used + 3 < capacity) {
                destination[used++] = static_cast<char>(0xE0 | (value >> 12));
                destination[used++] = static_cast<char>(0x80 | ((value >> 6) & 0x3F));
                destination[used++] = static_cast<char>(0x80 | (value & 0x3F));
            }
        }
    } else {
        for (size_t i = 0; i < length && source[i] && used + 1 < capacity; ++i) {
            const uint8_t value = source[i];
            if (encoding == 0 && value >= 0x80 && used + 2 < capacity) {
                destination[used++] = static_cast<char>(0xC0 | (value >> 6));
                destination[used++] = static_cast<char>(0x80 | (value & 0x3F));
            } else if (value >= 0x20 || value == '\t') {
                destination[used++] = static_cast<char>(value);
            }
        }
    }
    while (used > 0 && std::isspace(static_cast<unsigned char>(destination[used - 1]))) --used;
    destination[used] = '\0';
}

void normalize_year(char *year)
{
    if (!year) return;
    for (size_t i = 0; i + 3 < 8 && year[i]; ++i) {
        if (std::isdigit(static_cast<unsigned char>(year[i])) &&
            std::isdigit(static_cast<unsigned char>(year[i + 1])) &&
            std::isdigit(static_cast<unsigned char>(year[i + 2])) &&
            std::isdigit(static_cast<unsigned char>(year[i + 3]))) {
            char normalized[5] = {year[i], year[i + 1], year[i + 2], year[i + 3], '\0'};
            copy_text(year, 8, normalized);
            return;
        }
    }
    copy_text(year, 8, "Unknown");
}

void assign_tag(Track *track, const char *key, const uint8_t *value, size_t length,
                uint8_t encoding = 3)
{
    char decoded[kMaxName]{};
    copy_tag_text(decoded, sizeof(decoded), value, length, encoding);
    if (!decoded[0]) return;
    if (equals_ci(key, "TITLE")) copy_text(track->title, sizeof(track->title), decoded);
    else if (equals_ci(key, "ARTIST")) copy_text(track->artist, sizeof(track->artist), decoded);
    else if (equals_ci(key, "ALBUM")) copy_text(track->album, sizeof(track->album), decoded);
    else if (equals_ci(key, "ALBUMARTIST") || equals_ci(key, "ALBUM ARTIST")) {
        copy_text(track->album_artist, sizeof(track->album_artist), decoded);
    } else if (equals_ci(key, "COMPOSER")) {
        copy_text(track->composer, sizeof(track->composer), decoded);
    }
    else if (equals_ci(key, "GENRE")) copy_text(track->genre, sizeof(track->genre), decoded);
    else if (equals_ci(key, "DATE") || equals_ci(key, "YEAR")) {
        copy_text(track->year, sizeof(track->year), decoded);
        normalize_year(track->year);
    } else if (equals_ci(key, "TRACKNUMBER") || equals_ci(key, "TRACK") ||
               equals_ci(key, "TRCK")) {
        track->track_number = parse_tag_number(decoded);
    } else if (equals_ci(key, "DISCNUMBER") || equals_ci(key, "DISC") ||
               equals_ci(key, "TPOS")) {
        track->disc_number = parse_tag_number(decoded);
    } else if (equals_ci(key, "REPLAYGAIN_TRACK_GAIN")) {
        track->replay_gain_tenths_db = parse_replay_gain_tenths_db(decoded);
    }
}

void assign_id3_user_text_tag(Track *track, const uint8_t *text, size_t length)
{
    if (!track || !text || length < 3) return;
    const uint8_t encoding = text[0];
    size_t separator = 1;
    if (encoding == 1 || encoding == 2) {
        while (separator + 1 < length &&
               (text[separator] != 0 || text[separator + 1] != 0)) {
            separator += 2;
        }
        if (separator + 1 >= length) return;
    } else {
        while (separator < length && text[separator] != 0) ++separator;
        if (separator >= length) return;
    }

    char key[64]{};
    copy_tag_text(key, sizeof(key), text + 1, separator - 1, encoding);
    const size_t value_start = separator + (encoding == 1 || encoding == 2 ? 2 : 1);
    if (!key[0] || value_start >= length) return;
    assign_tag(track, key, text + value_start, length - value_start, encoding);
}

void read_id3_text_tags(FILE *file, Track *track, const uint8_t header[10],
                        long tag_start = 0)
{
    const uint8_t version = header[3];
    if (version < 2 || version > 4) return;
    const uint32_t tag_size = read_syncsafe32(header + 6);
    uint64_t position = 10;
    const uint64_t tag_end = position + tag_size;
    if (header[5] & 0x40) {
        uint8_t extended[4]{};
        if (std::fseek(file, tag_start + 10, SEEK_SET) != 0 ||
            std::fread(extended, 1, 4, file) != 4) return;
        const uint32_t extended_size = version == 4 ? read_syncsafe32(extended) : read_be32(extended);
        position += version == 4 ? extended_size : extended_size + 4;
    }
    while (position + (version == 2 ? 6u : 10u) <= tag_end) {
        if (std::fseek(file, tag_start + static_cast<long>(position), SEEK_SET) != 0) break;
        uint8_t frame_header[10]{};
        const size_t header_size = version == 2 ? 6 : 10;
        if (std::fread(frame_header, 1, header_size, file) != header_size || frame_header[0] == 0) break;
        const uint32_t frame_size = version == 2
            ? ((static_cast<uint32_t>(frame_header[3]) << 16) |
               (static_cast<uint32_t>(frame_header[4]) << 8) | frame_header[5])
            : (version == 4 ? read_syncsafe32(frame_header + 4) : read_be32(frame_header + 4));
        position += header_size;
        if (frame_size == 0 || position + frame_size > tag_end) break;
        const char *key = nullptr;
        if ((version == 2 && std::memcmp(frame_header, "TT2", 3) == 0) ||
            (version > 2 && std::memcmp(frame_header, "TIT2", 4) == 0)) key = "TITLE";
        else if ((version == 2 && std::memcmp(frame_header, "TP1", 3) == 0) ||
                 (version > 2 && std::memcmp(frame_header, "TPE1", 4) == 0)) key = "ARTIST";
        else if ((version == 2 && std::memcmp(frame_header, "TP2", 3) == 0) ||
                 (version > 2 && std::memcmp(frame_header, "TPE2", 4) == 0)) key = "ALBUMARTIST";
        else if ((version == 2 && std::memcmp(frame_header, "TAL", 3) == 0) ||
                 (version > 2 && std::memcmp(frame_header, "TALB", 4) == 0)) key = "ALBUM";
        else if ((version == 2 && std::memcmp(frame_header, "TCM", 3) == 0) ||
                 (version > 2 && std::memcmp(frame_header, "TCOM", 4) == 0)) key = "COMPOSER";
        else if ((version == 2 && std::memcmp(frame_header, "TCO", 3) == 0) ||
                 (version > 2 && std::memcmp(frame_header, "TCON", 4) == 0)) key = "GENRE";
        else if ((version == 2 && std::memcmp(frame_header, "TYE", 3) == 0) ||
                 (version > 2 && (std::memcmp(frame_header, "TYER", 4) == 0 ||
                                  std::memcmp(frame_header, "TDRC", 4) == 0))) key = "YEAR";
        else if ((version == 2 && std::memcmp(frame_header, "TRK", 3) == 0) ||
                 (version > 2 && std::memcmp(frame_header, "TRCK", 4) == 0)) key = "TRACKNUMBER";
        else if ((version == 2 && std::memcmp(frame_header, "TPA", 3) == 0) ||
                 (version > 2 && std::memcmp(frame_header, "TPOS", 4) == 0)) key = "DISCNUMBER";
        const bool user_text = (version == 2 && std::memcmp(frame_header, "TXX", 3) == 0) ||
                               (version > 2 && std::memcmp(frame_header, "TXXX", 4) == 0);
        if (key || user_text) {
            uint8_t text[512]{};
            const size_t wanted = std::min<size_t>(frame_size, sizeof(text));
            if (std::fseek(file, tag_start + static_cast<long>(position), SEEK_SET) == 0 &&
                std::fread(text, 1, wanted, file) == wanted && wanted > 1) {
                if (user_text) assign_id3_user_text_tag(track, text, wanted);
                else assign_tag(track, key, text + 1, wanted - 1, text[0]);
            }
        }
        position += frame_size;
    }
}

bool read_ogg_comment_packet(FILE *file, uint8_t *packet, size_t capacity,
                             size_t *packet_length)
{
    if (!file || !packet || capacity == 0 || !packet_length ||
        std::fseek(file, 0, SEEK_SET) != 0) return false;
    *packet_length = 0;

    uint8_t page_header[27]{};
    uint8_t lacing[255]{};
    uint8_t discard[256]{};
    size_t current_length = 0;
    bool overflow = false;
    unsigned packet_index = 0;
    while (packet_index < 2) {
        if (lyra::sd::read(file, page_header, sizeof(page_header),
                           lyra::sd::Client::Filesystem) != sizeof(page_header) ||
            std::memcmp(page_header, "OggS", 4) != 0 || page_header[4] != 0) return false;
        const size_t segment_count = page_header[26];
        if (lyra::sd::read(file, lacing, segment_count,
                           lyra::sd::Client::Filesystem) != segment_count) return false;
        for (size_t segment = 0; segment < segment_count; ++segment) {
            size_t remaining = lacing[segment];
            while (remaining > 0) {
                const size_t wanted = std::min(remaining, sizeof(discard));
                uint8_t *destination = discard;
                if (!overflow && current_length + wanted <= capacity) {
                    destination = packet + current_length;
                } else {
                    overflow = true;
                }
                const size_t count = lyra::sd::read(file, destination, wanted,
                                                     lyra::sd::Client::Filesystem);
                if (count != wanted) return false;
                if (!overflow) current_length += count;
                remaining -= count;
            }
            if (lacing[segment] != 255) {
                if (packet_index == 1 && !overflow) {
                    *packet_length = current_length;
                    return true;
                }
                ++packet_index;
                current_length = 0;
                overflow = false;
            }
        }
    }
    return false;
}

bool equals_ci_bytes(const uint8_t *bytes, size_t length, const char *text)
{
    if (!bytes || !text) return false;
    size_t index = 0;
    for (; index < length && text[index]; ++index) {
        const unsigned char left = static_cast<unsigned char>(std::tolower(bytes[index]));
        const unsigned char right = static_cast<unsigned char>(std::tolower(text[index]));
        if (left != right) return false;
    }
    return index == length && text[index] == '\0';
}

void parse_vorbis_comments(const uint8_t *data, size_t length, size_t cursor,
                           Track *track)
{
    if (!data || !track || cursor + 8 > length) return;
    const uint32_t vendor_length = read_le32(data + cursor);
    cursor += 4;
    if (vendor_length > length - cursor) return;
    cursor += vendor_length;
    if (cursor + 4 > length) return;
    const uint32_t comment_count = read_le32(data + cursor);
    cursor += 4;
    for (uint32_t index = 0; index < comment_count && cursor + 4 <= length; ++index) {
        const uint32_t comment_length = read_le32(data + cursor);
        cursor += 4;
        if (comment_length > length - cursor) break;
        const uint8_t *equals = static_cast<const uint8_t *>(
            std::memchr(data + cursor, '=', comment_length));
        if (equals) {
            char key[32]{};
            const size_t key_length = std::min<size_t>(
                equals - (data + cursor), sizeof(key) - 1);
            std::memcpy(key, data + cursor, key_length);
            assign_tag(track, key, equals + 1,
                       comment_length - static_cast<size_t>(equals + 1 - (data + cursor)));
        }
        cursor += comment_length;
    }
}

void read_ogg_text_tags(FILE *file, Track *track)
{
    auto *packet = static_cast<uint8_t *>(heap_caps_malloc(
        kMaximumOggCommentPacketBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!packet) packet = static_cast<uint8_t *>(std::malloc(kMaximumOggCommentPacketBytes));
    if (!packet) return;
    size_t length = 0;
    if (read_ogg_comment_packet(file, packet, kMaximumOggCommentPacketBytes, &length)) {
        if (length >= 8 && std::memcmp(packet, "OpusTags", 8) == 0) {
            parse_vorbis_comments(packet, length, 8, track);
        } else if (length >= 7 && packet[0] == 3 &&
                   std::memcmp(packet + 1, "vorbis", 6) == 0) {
            parse_vorbis_comments(packet, length, 7, track);
        }
    }
    heap_caps_free(packet);
}

struct Mp4Atom {
    uint64_t start;
    uint64_t data_start;
    uint64_t end;
    uint64_t data_size;
    uint8_t header_size;
    uint8_t type[4];
};

bool read_mp4_atom(FILE *file, uint64_t position, uint64_t limit, Mp4Atom *atom)
{
    if (!file || !atom || position + 8 > limit ||
        std::fseek(file, static_cast<long>(position), SEEK_SET) != 0) return false;
    uint8_t header[16]{};
    if (std::fread(header, 1, 8, file) != 8) return false;
    uint64_t size = read_be32(header);
    uint8_t header_size = 8;
    if (size == 1) {
        if (position + 16 > limit || std::fread(header + 8, 1, 8, file) != 8) return false;
        size = read_be64(header + 8);
        header_size = 16;
    } else if (size == 0) {
        size = limit - position;
    }
    if (size < header_size || size > limit - position) return false;
    atom->start = position;
    atom->data_start = position + header_size;
    atom->end = position + size;
    atom->data_size = size - header_size;
    atom->header_size = header_size;
    std::memcpy(atom->type, header + 4, 4);
    return true;
}

bool mp4_type(const Mp4Atom &atom, const char (&type)[5])
{
    return std::memcmp(atom.type, type, 4) == 0;
}

bool read_mp4_text_value(FILE *file, const Mp4Atom &item, const char *key, Track *track)
{
    uint64_t cursor = item.data_start;
    while (cursor + 8 <= item.end) {
        Mp4Atom child{};
        if (!read_mp4_atom(file, cursor, item.end, &child)) return false;
        if (mp4_type(child, "data") && child.data_size >= 8) {
            const uint64_t value_start = child.data_start + 8;
            const size_t value_length = static_cast<size_t>(child.data_size - 8);
            auto *value = static_cast<uint8_t *>(heap_caps_malloc(
                std::min<size_t>(value_length, kMaxName), MALLOC_CAP_INTERNAL));
            if (!value) return false;
            const size_t wanted = std::min<size_t>(value_length, kMaxName);
            const bool read = std::fseek(file, static_cast<long>(value_start), SEEK_SET) == 0 &&
                              std::fread(value, 1, wanted, file) == wanted;
            if (read) assign_tag(track, key, value, wanted);
            heap_caps_free(value);
            return read;
        }
        cursor = child.end;
    }
    return false;
}

bool read_mp4_position_value(FILE *file, const Mp4Atom &item, bool disc, Track *track)
{
    if (!file || !track) return false;
    uint64_t cursor = item.data_start;
    while (cursor + 8 <= item.end) {
        Mp4Atom child{};
        if (!read_mp4_atom(file, cursor, item.end, &child)) return false;
        if (mp4_type(child, "data") && child.data_size >= 12) {
            uint8_t value[8]{};
            const uint64_t value_start = child.data_start + 8;
            if (std::fseek(file, static_cast<long>(value_start), SEEK_SET) != 0 ||
                std::fread(value, 1, sizeof(value), file) != sizeof(value)) return false;
            const uint32_t number = (static_cast<uint32_t>(value[2]) << 8) | value[3];
            if (disc) track->disc_number = number;
            else track->track_number = number;
            return true;
        }
        cursor = child.end;
    }
    return false;
}

void read_mp4_ilst(FILE *file, const Mp4Atom &ilst, Track *track)
{
    uint64_t cursor = ilst.data_start;
    while (cursor + 8 <= ilst.end) {
        Mp4Atom item{};
        if (!read_mp4_atom(file, cursor, ilst.end, &item)) return;
        const char *key = nullptr;
        if (std::memcmp(item.type, "\xA9" "nam", 4) == 0) key = "TITLE";
        else if (std::memcmp(item.type, "\xA9" "ART", 4) == 0) key = "ARTIST";
        else if (std::memcmp(item.type, "aART", 4) == 0) key = "ALBUMARTIST";
        else if (std::memcmp(item.type, "\xA9" "alb", 4) == 0) key = "ALBUM";
        else if (std::memcmp(item.type, "\xA9" "wrt", 4) == 0) key = "COMPOSER";
        else if (std::memcmp(item.type, "\xA9" "gen", 4) == 0) key = "GENRE";
        else if (std::memcmp(item.type, "\xA9" "day", 4) == 0) key = "YEAR";
        if (key) read_mp4_text_value(file, item, key, track);
        else if (std::memcmp(item.type, "trkn", 4) == 0) {
            read_mp4_position_value(file, item, false, track);
        } else if (std::memcmp(item.type, "disk", 4) == 0) {
            read_mp4_position_value(file, item, true, track);
        }
        cursor = item.end;
    }
}

bool mp4_container(const Mp4Atom &atom)
{
    return mp4_type(atom, "moov") || mp4_type(atom, "trak") || mp4_type(atom, "mdia") ||
           mp4_type(atom, "minf") || mp4_type(atom, "stbl") || mp4_type(atom, "udta") ||
           mp4_type(atom, "meta") || mp4_type(atom, "ilst") || mp4_type(atom, "edts") ||
           mp4_type(atom, "dinf") || mp4_type(atom, "mvex") || mp4_type(atom, "moof") ||
           mp4_type(atom, "traf");
}

void read_mp4_metadata_tree(FILE *file, uint64_t start, uint64_t end, Track *track,
                            unsigned depth = 0)
{
    if (!file || !track || depth > 8) return;
    uint64_t cursor = start;
    while (cursor + 8 <= end) {
        Mp4Atom atom{};
        if (!read_mp4_atom(file, cursor, end, &atom)) return;
        if (mp4_type(atom, "ilst")) {
            read_mp4_ilst(file, atom, track);
        } else if (mp4_container(atom)) {
            uint64_t child_start = atom.data_start;
            if (mp4_type(atom, "meta") && child_start + 4 <= atom.end) child_start += 4;
            read_mp4_metadata_tree(file, child_start, atom.end, track, depth + 1);
        }
        cursor = atom.end;
    }
}

bool find_mp4_moov(FILE *file, Mp4Atom *moov)
{
    if (!file || !moov || std::fseek(file, 0, SEEK_END) != 0) return false;
    const long file_end = std::ftell(file);
    if (file_end < 12) return false;
    uint64_t cursor = 0;
    while (cursor + 8 <= static_cast<uint64_t>(file_end)) {
        Mp4Atom atom{};
        if (!read_mp4_atom(file, cursor, file_end, &atom)) return false;
        if (mp4_type(atom, "moov")) {
            *moov = atom;
            return true;
        }
        cursor = atom.end;
    }
    return false;
}

void read_mp4_text_tags(FILE *file, Track *track)
{
    Mp4Atom moov{};
    if (!find_mp4_moov(file, &moov)) return;
    read_mp4_metadata_tree(file, moov.data_start, moov.end, track);
}

void read_flac_text_tags(FILE *file, Track *track)
{
    uint64_t position = 4;
    bool last = false;
    while (!last) {
        if (std::fseek(file, static_cast<long>(position), SEEK_SET) != 0) return;
        uint8_t block_header[4]{};
        if (std::fread(block_header, 1, 4, file) != 4) return;
        last = (block_header[0] & 0x80) != 0;
        const uint8_t type = block_header[0] & 0x7F;
        const uint32_t length = (static_cast<uint32_t>(block_header[1]) << 16) |
                                (static_cast<uint32_t>(block_header[2]) << 8) | block_header[3];
        position += 4;
        if (type == 4 && length >= 8 && length <= 64u * 1024u) {
            auto *data = static_cast<uint8_t *>(heap_caps_malloc(length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!data) data = static_cast<uint8_t *>(std::malloc(length));
            if (!data) return;
            const bool read = std::fread(data, 1, length, file) == length;
            if (read) {
                size_t cursor = 0;
                const uint32_t vendor = read_le32(data);
                if (vendor > length - 8) {
                    heap_caps_free(data);
                    return;
                }
                cursor = 4u + static_cast<size_t>(vendor);
                if (cursor + 4 <= length) {
                    const uint32_t comments = read_le32(data + cursor);
                    cursor += 4;
                    for (uint32_t i = 0; i < comments && cursor + 4 <= length; ++i) {
                        const uint32_t comment_length = read_le32(data + cursor);
                        cursor += 4;
                        if (comment_length > length - cursor) break;
                        const uint8_t *equals = static_cast<const uint8_t *>(
                            std::memchr(data + cursor, '=', comment_length));
                        if (equals) {
                            char key[16]{};
                            const size_t key_length = std::min<size_t>(equals - (data + cursor), sizeof(key) - 1);
                            std::memcpy(key, data + cursor, key_length);
                            const uint8_t *value = equals + 1;
                            assign_tag(track, key, value,
                                       comment_length - static_cast<size_t>(value - (data + cursor)));
                        }
                        cursor += comment_length;
                    }
                }
            }
            heap_caps_free(data);
            return;
        }
        position += length;
    }
}

const char *riff_info_key(const uint8_t *id)
{
    if (std::memcmp(id, "INAM", 4) == 0) return "TITLE";
    if (std::memcmp(id, "IART", 4) == 0) return "ARTIST";
    if (std::memcmp(id, "IPRD", 4) == 0) return "ALBUM";
    if (std::memcmp(id, "IGNR", 4) == 0) return "GENRE";
    if (std::memcmp(id, "ICRD", 4) == 0) return "YEAR";
    return nullptr;
}

void read_wav_text_tags(FILE *file, Track *track)
{
    if (!file || !track || std::fseek(file, 0, SEEK_SET) != 0) return;
    uint8_t header[12]{};
    if (std::fread(header, 1, sizeof(header), file) != sizeof(header) ||
        std::memcmp(header, "RIFF", 4) != 0 || std::memcmp(header + 8, "WAVE", 4) != 0) return;
    if (std::fseek(file, 0, SEEK_END) != 0) return;
    const long file_end = std::ftell(file);
    uint64_t position = 12;
    while (position + 8 <= static_cast<uint64_t>(file_end)) {
        uint8_t chunk[8]{};
        if (std::fseek(file, static_cast<long>(position), SEEK_SET) != 0 ||
            std::fread(chunk, 1, sizeof(chunk), file) != sizeof(chunk)) return;
        const uint32_t length = read_le32(chunk + 4);
        const uint64_t data_start = position + 8;
        if (data_start + length > static_cast<uint64_t>(file_end)) return;
        if (std::memcmp(chunk, "LIST", 4) == 0 && length >= 4 &&
            std::fseek(file, static_cast<long>(data_start), SEEK_SET) == 0) {
            uint8_t list_type[4]{};
            if (std::fread(list_type, 1, 4, file) == 4 && std::memcmp(list_type, "INFO", 4) == 0) {
                uint64_t cursor = data_start + 4;
                const uint64_t list_end = data_start + length;
                while (cursor + 8 <= list_end) {
                    uint8_t item[8]{};
                    if (std::fseek(file, static_cast<long>(cursor), SEEK_SET) != 0 ||
                        std::fread(item, 1, sizeof(item), file) != sizeof(item)) break;
                    const uint32_t item_length = read_le32(item + 4);
                    if (cursor + 8 + item_length > list_end) break;
                    const char *key = riff_info_key(item);
                    if (key && item_length > 0) {
                        uint8_t value[512]{};
                        const size_t wanted = std::min<size_t>(item_length, sizeof(value));
                        if (std::fread(value, 1, wanted, file) == wanted) {
                            assign_tag(track, key, value, wanted);
                        }
                    }
                    cursor += 8 + item_length + (item_length & 1u);
                }
            }
        } else if ((std::memcmp(chunk, "id3 ", 4) == 0 ||
                    std::memcmp(chunk, "ID3 ", 4) == 0) && length >= 10) {
            uint8_t id3[10]{};
            if (std::fseek(file, static_cast<long>(data_start), SEEK_SET) == 0 &&
                std::fread(id3, 1, sizeof(id3), file) == sizeof(id3) &&
                std::memcmp(id3, "ID3", 3) == 0) {
                read_id3_text_tags(file, track, id3, static_cast<long>(data_start));
            }
        }
        position = data_start + length + (length & 1u);
    }
}

void read_aiff_text_tags(FILE *file, Track *track)
{
    if (!file || !track || std::fseek(file, 0, SEEK_SET) != 0) return;
    uint8_t form[12]{};
    if (std::fread(form, 1, sizeof(form), file) != sizeof(form) ||
        std::memcmp(form, "FORM", 4) != 0 ||
        (std::memcmp(form + 8, "AIFF", 4) != 0 && std::memcmp(form + 8, "AIFC", 4) != 0)) return;
    if (std::fseek(file, 0, SEEK_END) != 0) return;
    const long file_end = std::ftell(file);
    uint64_t position = 12;
    while (position + 8 <= static_cast<uint64_t>(file_end)) {
        uint8_t chunk[8]{};
        if (std::fseek(file, static_cast<long>(position), SEEK_SET) != 0 ||
            std::fread(chunk, 1, sizeof(chunk), file) != sizeof(chunk)) return;
        const uint32_t length = read_be32(chunk + 4);
        const uint64_t data_start = position + 8;
        if (data_start + length > static_cast<uint64_t>(file_end)) return;
        const char *key = nullptr;
        if (std::memcmp(chunk, "NAME", 4) == 0) key = "TITLE";
        else if (std::memcmp(chunk, "AUTH", 4) == 0) key = "ARTIST";
        else if (std::memcmp(chunk, "ANNO", 4) == 0) key = "GENRE";
        if (key && length > 0) {
            uint8_t value[512]{};
            const size_t wanted = std::min<size_t>(length, sizeof(value));
            if (std::fread(value, 1, wanted, file) == wanted) assign_tag(track, key, value, wanted);
        } else if (std::memcmp(chunk, "ID3 ", 4) == 0 && length >= 10) {
            uint8_t id3[10]{};
            if (std::fseek(file, static_cast<long>(data_start), SEEK_SET) == 0 &&
                std::fread(id3, 1, sizeof(id3), file) == sizeof(id3) &&
                std::memcmp(id3, "ID3", 3) == 0) {
                read_id3_text_tags(file, track, id3, static_cast<long>(data_start));
            }
        }
        position = data_start + length + (length & 1u);
    }
}

void read_fast_metadata(Track *track)
{
    if (!track) return;
    const char *extension = extension_of(track->path);
    if (!equals_ci(extension, "mp3") && !equals_ci(extension, "flac") &&
        !equals_ci(extension, "m4a") && !equals_ci(extension, "mp4") &&
        !equals_ci(extension, "ogg") && !equals_ci(extension, "opus") &&
        !equals_ci(extension, "wav") && !equals_ci(extension, "aiff") &&
        !equals_ci(extension, "aif") && !equals_ci(extension, "aifc")) return;
    FILE *file = std::fopen(track->path, "rb");
    if (!file) return;
    uint8_t header[10]{};
    const size_t read = std::fread(header, 1, sizeof(header), file);
    if (equals_ci(extension, "m4a") || equals_ci(extension, "mp4")) {
        read_mp4_text_tags(file, track);
    } else if (equals_ci(extension, "ogg") || equals_ci(extension, "opus")) {
        read_ogg_text_tags(file, track);
    } else if (equals_ci(extension, "wav")) {
        read_wav_text_tags(file, track);
    } else if (equals_ci(extension, "aiff") || equals_ci(extension, "aif") ||
               equals_ci(extension, "aifc")) {
        read_aiff_text_tags(file, track);
    } else if (read == sizeof(header) && std::memcmp(header, "ID3", 3) == 0) {
        read_id3_text_tags(file, track, header);
    } else if (read >= 4 && std::memcmp(header, "fLaC", 4) == 0) {
        read_flac_text_tags(file, track);
    }
    std::fclose(file);
}

bool ensure_directory(const char *path)
{
    if (mkdir(path, 0775) == 0) return true;
    struct stat info{};
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

void remove_stale_jpeg_work_files()
{
    DIR *directory = opendir(kDataDir);
    if (!directory) return;
    while (dirent *entry = readdir(directory)) {
        const char *name = entry->d_name;
        const size_t length = std::strlen(name);
        const bool jpeg_temp = length >= 14 && std::strncmp(name, "jpeg-", 5) == 0 &&
                               std::strcmp(name + length - 4, ".tmp") == 0;
        if (!jpeg_temp) continue;
        char path[kMaxPath];
        if (join_path(path, sizeof(path), kDataDir, name)) std::remove(path);
    }
    closedir(directory);
}

uint32_t artwork_key(const Track &track)
{
    uint32_t hash = 2166136261u;
    const auto update = [&hash](const char *text) {
        for (const uint8_t *p = reinterpret_cast<const uint8_t *>(text); *p; ++p) {
            hash = (hash ^ *p) * 16777619u;
        }
    };
    // The Albums browser groups on Track::album, so use the same identity
    // here. This guarantees every song in an album resolves to one shared
    // decoded source and one in-memory display asset.
    update(track.album);
    return hash;
}

uint64_t artwork_request_key(const Track &track, ArtworkSize size)
{
    (void)size;
    return artwork_key(track);
}


enum class ArtworkFormat : uint8_t { Jpeg, Png };

struct ArtworkBlob {
    uint8_t *data;
    size_t length;
    size_t capacity;
    ArtworkFormat format;
    bool psram;
};

void free_artwork_blob(ArtworkBlob *blob)
{
    if (!blob) return;
    heap_caps_free(blob->data);
    *blob = {};
}

void *alloc_artwork_pixels(size_t size)
{
    // Artwork is read by the CPU and never submitted directly to a DMA
    // engine. Keep it exclusively in PSRAM so a large cover can never consume
    // the scarce internal heap used by the LCD transport and audio DMA.
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

bool grow_artwork_blob(ArtworkBlob *blob, size_t wanted)
{
    if (!blob || wanted > kMaximumEmbeddedImageBytes) return false;
    if (wanted <= blob->capacity) return true;
    size_t capacity = blob->capacity == 0 ? 16 * 1024 : blob->capacity;
    while (capacity < wanted) {
        if (capacity > kMaximumEmbeddedImageBytes / 2) {
            capacity = kMaximumEmbeddedImageBytes;
            break;
        }
        capacity *= 2;
    }
    // The compressed source is also CPU-only artwork memory. Do not fall
    // back to internal SRAM when PSRAM is fragmented; preserving the LCD and
    // I2S DMA heap is more important than showing a cover on that request.
    auto *next = static_cast<uint8_t *>(heap_caps_malloc(
        capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!next) return false;
    if (blob->data && blob->length) std::memcpy(next, blob->data, blob->length);
    heap_caps_free(blob->data);
    blob->data = next;
    blob->capacity = capacity;
    return true;
}

bool append_artwork_byte(ArtworkBlob *blob, uint8_t value)
{
    if (!grow_artwork_blob(blob, blob->length + 1)) return false;
    blob->data[blob->length++] = value;
    return true;
}

bool copy_embedded_image_from_memory(const uint8_t *data, size_t length,
                                     ArtworkBlob *blob)
{
    if (!data || !blob || length < 4) return false;
    constexpr uint8_t kPngSignature[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    size_t image_start = SIZE_MAX;
    ArtworkFormat format = ArtworkFormat::Jpeg;
    for (size_t index = 0; index + 3 <= length; ++index) {
        if (index + sizeof(kPngSignature) <= length &&
            std::memcmp(data + index, kPngSignature, sizeof(kPngSignature)) == 0) {
            image_start = index;
            format = ArtworkFormat::Png;
            break;
        }
        if (data[index] == 0xFF && data[index + 1] == 0xD8 && data[index + 2] == 0xFF) {
            image_start = index;
            format = ArtworkFormat::Jpeg;
            break;
        }
    }
    if (image_start == SIZE_MAX || length - image_start > kMaximumEmbeddedImageBytes) return false;
    const bool prefer_psram = blob->psram;
    free_artwork_blob(blob);
    blob->psram = prefer_psram;
    blob->format = format;
    if (!grow_artwork_blob(blob, length - image_start)) return false;
    std::memcpy(blob->data, data + image_start, length - image_start);
    blob->length = length - image_start;
    return true;
}

int base64_digit(uint8_t value)
{
    if (value >= 'A' && value <= 'Z') return value - 'A';
    if (value >= 'a' && value <= 'z') return value - 'a' + 26;
    if (value >= '0' && value <= '9') return value - '0' + 52;
    if (value == '+') return 62;
    if (value == '/') return 63;
    return -1;
}

bool decode_base64(const uint8_t *encoded, size_t length, uint8_t *decoded,
                   size_t capacity, size_t *decoded_length)
{
    if (!encoded || !decoded || !decoded_length) return false;
    size_t output = 0;
    uint32_t accumulator = 0;
    unsigned bits = 0;
    for (size_t index = 0; index < length; ++index) {
        if (encoded[index] == '=' || std::isspace(encoded[index])) continue;
        const int digit = base64_digit(encoded[index]);
        if (digit < 0) return false;
        accumulator = (accumulator << 6) | static_cast<uint32_t>(digit);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (output >= capacity) return false;
            decoded[output++] = static_cast<uint8_t>((accumulator >> bits) & 0xFFu);
        }
    }
    *decoded_length = output;
    return output > 0;
}

bool find_ogg_comment(const uint8_t *data, size_t length, size_t cursor,
                      const char *wanted_key, const uint8_t **value,
                      size_t *value_length)
{
    if (!data || !wanted_key || !value || !value_length || cursor + 8 > length) return false;
    const uint32_t vendor_length = read_le32(data + cursor);
    cursor += 4;
    if (vendor_length > length - cursor) return false;
    cursor += vendor_length;
    if (cursor + 4 > length) return false;
    const uint32_t comment_count = read_le32(data + cursor);
    cursor += 4;
    for (uint32_t index = 0; index < comment_count && cursor + 4 <= length; ++index) {
        const uint32_t comment_length = read_le32(data + cursor);
        cursor += 4;
        if (comment_length > length - cursor) return false;
        const uint8_t *equals = static_cast<const uint8_t *>(
            std::memchr(data + cursor, '=', comment_length));
        if (equals && equals_ci_bytes(data + cursor,
                                      static_cast<size_t>(equals - (data + cursor)), wanted_key)) {
            *value = equals + 1;
            *value_length = comment_length - static_cast<size_t>(equals + 1 - (data + cursor));
            return true;
        }
        cursor += comment_length;
    }
    return false;
}

bool extract_ogg_image(FILE *file, ArtworkBlob *blob)
{
    auto *packet = static_cast<uint8_t *>(heap_caps_malloc(
        kMaximumOggCommentPacketBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!packet) packet = static_cast<uint8_t *>(std::malloc(kMaximumOggCommentPacketBytes));
    if (!packet) return false;
    size_t packet_length = 0;
    bool result = false;
    if (read_ogg_comment_packet(file, packet, kMaximumOggCommentPacketBytes, &packet_length)) {
        size_t comment_start = 0;
        if (packet_length >= 8 && std::memcmp(packet, "OpusTags", 8) == 0) comment_start = 8;
        else if (packet_length >= 7 && packet[0] == 3 &&
                 std::memcmp(packet + 1, "vorbis", 6) == 0) comment_start = 7;
        if (comment_start != 0) {
            const uint8_t *encoded = nullptr;
            size_t encoded_length = 0;
            if (find_ogg_comment(packet, packet_length, comment_start,
                                 "METADATA_BLOCK_PICTURE", &encoded, &encoded_length) ||
                find_ogg_comment(packet, packet_length, comment_start,
                                 "COVERART", &encoded, &encoded_length)) {
                const size_t capacity = std::min<size_t>(
                    kMaximumEmbeddedImageBytes + 1024,
                    (encoded_length / 4u) * 3u + 4u);
                auto *decoded = static_cast<uint8_t *>(heap_caps_malloc(
                    capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                if (!decoded) decoded = static_cast<uint8_t *>(std::malloc(capacity));
                if (decoded) {
                    size_t decoded_length = 0;
                    if (decode_base64(encoded, encoded_length, decoded, capacity,
                                      &decoded_length)) {
                        result = copy_embedded_image_from_memory(decoded, decoded_length, blob);
                    }
                    heap_caps_free(decoded);
                }
            }
        }
    }
    heap_caps_free(packet);
    return result;
}

// Reads only the embedded payload's actual image. The SD gate is taken for
// each 8 KiB transaction and is fully released before any decoder runs.
bool read_embedded_image(FILE *source, uint64_t offset, uint32_t max_length,
                         ArtworkBlob *blob)
{
    if (!source || !blob || max_length < 4 || max_length > kMaximumEmbeddedImageBytes) return false;

    // A tag can contain more than one picture. Do not let a malformed first
    // candidate contaminate the next candidate's in-memory source.
    const bool prefer_psram = blob->psram;
    free_artwork_blob(blob);
    blob->psram = prefer_psram;
    if (lyra::sd::seek(source, static_cast<long>(offset), SEEK_SET,
                       lyra::sd::Client::Artwork) != 0) return false;

    enum class JpegState : uint8_t {
        MarkerCode,
        SegmentLengthHigh,
        SegmentLengthLow,
        SegmentData,
        Entropy,
        EntropyMarkerCode,
    };
    JpegState jpeg_state = JpegState::MarkerCode;
    uint8_t jpeg_marker = 0;
    uint16_t jpeg_segment_length = 0;
    size_t jpeg_segment_remaining = 0;
    bool jpeg_segment_to_entropy = false;

    uint8_t buffer[kArtworkReadChunkBytes];
    uint8_t window[8]{};
    size_t window_length = 0;
    size_t remaining = max_length;
    bool found = false;
    constexpr uint8_t png_signature[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    uint8_t png_header[8]{};
    size_t png_header_length = 0;
    uint32_t png_chunk_length = 0;
    size_t png_chunk_remaining = 0;
    size_t png_crc_remaining = 0;
    char png_chunk_type[5]{};

    while (remaining > 0) {
        const size_t wanted = std::min(remaining, sizeof(buffer));
        const size_t count = lyra::sd::read(source, buffer, wanted, lyra::sd::Client::Artwork);
        if (count == 0) break;
        remaining -= count;
        for (size_t i = 0; i < count; ++i) {
            const uint8_t value = buffer[i];
            if (!found) {
                if (window_length < sizeof(window)) window[window_length++] = value;
                else {
                    std::memmove(window, window + 1, sizeof(window) - 1);
                    window[sizeof(window) - 1] = value;
                }
                const bool jpeg = window_length >= 3 &&
                    window[window_length - 3] == 0xFF &&
                    window[window_length - 2] == 0xD8 &&
                    window[window_length - 1] == 0xFF;
                const bool png = window_length == sizeof(png_signature) &&
                    std::memcmp(window, png_signature, sizeof(png_signature)) == 0;
                if (jpeg || png) {
                    found = true;
                    blob->format = png ? ArtworkFormat::Png : ArtworkFormat::Jpeg;
                    const size_t signature_length = png ? sizeof(png_signature) : 3;
                    for (size_t j = window_length - signature_length; j < window_length; ++j) {
                        if (!append_artwork_byte(blob, window[j])) return false;
                    }
                    window_length = 0;
                }
                continue;
            }

            if (!append_artwork_byte(blob, value)) return false;
            if (blob->format == ArtworkFormat::Jpeg) {
                // EOI is only meaningful as a JPEG marker. Do not stop on
                // FF D9 bytes inside APP/EXIF/ICC payloads or entropy data.
                switch (jpeg_state) {
                case JpegState::MarkerCode:
                    if (value == 0xFF) break; // Marker fill bytes.
                    if (value == 0xD9) return true;
                    if (value == 0xD8 || value == 0x01 ||
                        (value >= 0xD0 && value <= 0xD7)) break;
                    jpeg_marker = value;
                    jpeg_state = JpegState::SegmentLengthHigh;
                    break;
                case JpegState::SegmentLengthHigh:
                    jpeg_segment_length = static_cast<uint16_t>(value) << 8;
                    jpeg_state = JpegState::SegmentLengthLow;
                    break;
                case JpegState::SegmentLengthLow:
                    jpeg_segment_length = static_cast<uint16_t>(jpeg_segment_length | value);
                    if (jpeg_segment_length < 2) return false;
                    jpeg_segment_remaining = jpeg_segment_length - 2;
                    jpeg_segment_to_entropy = jpeg_marker == 0xDA;
                    jpeg_state = jpeg_segment_remaining == 0
                        ? (jpeg_segment_to_entropy ? JpegState::Entropy : JpegState::MarkerCode)
                        : JpegState::SegmentData;
                    break;
                case JpegState::SegmentData:
                    if (jpeg_segment_remaining > 0) --jpeg_segment_remaining;
                    if (jpeg_segment_remaining == 0) {
                        jpeg_state = jpeg_segment_to_entropy ? JpegState::Entropy :
                                                                JpegState::MarkerCode;
                    }
                    break;
                case JpegState::Entropy:
                    if (value == 0xFF) jpeg_state = JpegState::EntropyMarkerCode;
                    break;
                case JpegState::EntropyMarkerCode:
                    if (value == 0x00 || (value >= 0xD0 && value <= 0xD7)) {
                        jpeg_state = JpegState::Entropy;
                    } else if (value == 0xD9) {
                        return true;
                    } else if (value == 0xFF) {
                        // Additional marker fill byte.
                    } else {
                        jpeg_marker = value;
                        jpeg_segment_to_entropy = true;
                        jpeg_state = JpegState::SegmentLengthHigh;
                    }
                    break;
                }
            } else {
                // A PNG chunk has a four-byte length, four-byte type, data,
                // and four-byte CRC. Parsing the chunk framing avoids a false
                // IEND match inside compressed IDAT data.
                if (png_header_length < sizeof(png_header)) {
                    png_header[png_header_length++] = value;
                    if (png_header_length == sizeof(png_header)) {
                        png_chunk_length = (static_cast<uint32_t>(png_header[0]) << 24) |
                            (static_cast<uint32_t>(png_header[1]) << 16) |
                            (static_cast<uint32_t>(png_header[2]) << 8) | png_header[3];
                        std::memcpy(png_chunk_type, png_header + 4, 4);
                        png_chunk_type[4] = '\0';
                        if (png_chunk_length > kMaximumEmbeddedImageBytes) return false;
                        png_chunk_remaining = png_chunk_length;
                        png_crc_remaining = 4;
                    }
                } else if (png_chunk_remaining > 0) {
                    --png_chunk_remaining;
                } else if (png_crc_remaining > 0) {
                    --png_crc_remaining;
                    if (png_crc_remaining == 0) {
                        if (std::strcmp(png_chunk_type, "IEND") == 0 && png_chunk_length == 0) {
                            return true;
                        }
                        png_header_length = 0;
                    }
                }
            }
        }
        if (remaining > 0) vTaskDelay(1);
    }
    return found && blob->length > 0;
}

bool decode_artwork_blob(const ArtworkBlob &blob, uint16_t *pixels,
                         uint16_t target_size, bool preserve_aspect,
                         bool allow_sd_backing,
                         bool *used_sd_backing)
{
    if (used_sd_backing) *used_sd_backing = false;
    if (!blob.data || blob.length == 0 || !pixels || target_size == 0) return false;
    std::fill(pixels, pixels + static_cast<size_t>(target_size) * target_size,
              static_cast<uint16_t>(0x1104));
    if (blob.format == ArtworkFormat::Png) {
        return decode_png(blob.data, blob.length, pixels, target_size, target_size,
                          preserve_aspect, 0x102020);
    }

    // libjpeg-turbo handles baseline, progressive, uncommon chroma sampling,
    // grayscale, and CMYK JPEGs consistently. TinyJPEG was faster for a narrow
    // subset but returned format errors for valid embedded covers.
    return decode_progressive_jpeg(blob.data, blob.length, pixels, target_size,
                                   preserve_aspect, allow_sd_backing, used_sd_backing);
}

void artwork_cache_path(uint32_t key, uint16_t artwork_size, char *path, size_t capacity)
{
    if (!path || capacity == 0) return;
    std::snprintf(path, capacity, "%s/%08lx.large-v%u.%u.rgb565", kArtworkDir,
                  static_cast<unsigned long>(key),
                  static_cast<unsigned>(kLargeArtworkCacheVersion),
                  static_cast<unsigned>(artwork_size));
}

bool valid_artwork_cache(const char *path, uint16_t artwork_size)
{
    struct stat info{};
    const off_t expected = static_cast<off_t>(artwork_size) * artwork_size * sizeof(uint16_t);
    return path && stat(path, &info) == 0 && S_ISREG(info.st_mode) &&
           info.st_size == expected;
}

bool read_artwork_cache(uint32_t key, uint16_t artwork_size, uint16_t **out_pixels)
{
    if (!out_pixels) return false;
    *out_pixels = nullptr;
    char path[kMaxPath];
    artwork_cache_path(key, artwork_size, path, sizeof(path));
    if (!valid_artwork_cache(path, artwork_size)) return false;

    const size_t pixel_count = static_cast<size_t>(artwork_size) * artwork_size;
    auto *pixels = static_cast<uint16_t *>(alloc_artwork_pixels(
        pixel_count * sizeof(uint16_t)));
    FILE *input = pixels ? lyra::sd::open(path, "rb", lyra::sd::Client::Artwork) : nullptr;
    const bool read = input && lyra::sd::read_exact(
        input, pixels, pixel_count * sizeof(uint16_t), lyra::sd::Client::Artwork);
    if (input) lyra::sd::close(input, lyra::sd::Client::Artwork);
    if (!read) {
        heap_caps_free(pixels);
        lyra::sd::remove(path, lyra::sd::Client::Artwork);
        return false;
    }
    *out_pixels = pixels;
    ESP_LOGI(kTag, "artwork loaded from large-image SD cache: key=%08lx",
             static_cast<unsigned long>(key));
    return true;
}

bool write_artwork_cache(uint32_t key, uint16_t artwork_size, const uint16_t *pixels)
{
    if (!pixels || !ensure_directory(kDataDir) || !ensure_directory(kArtworkDir)) return false;
    char path[kMaxPath];
    char temporary[kMaxPath];
    artwork_cache_path(key, artwork_size, path, sizeof(path));
    const int length = std::snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(temporary)) return false;

    const size_t bytes = static_cast<size_t>(artwork_size) * artwork_size * sizeof(uint16_t);
    FILE *output = lyra::sd::open(temporary, "wb", lyra::sd::Client::Artwork);
    const bool unbuffered = output && std::setvbuf(output, nullptr, _IONBF, 0) == 0;
    const bool written = unbuffered && lyra::sd::write_exact(
        output, pixels, bytes, lyra::sd::Client::Artwork);
    const bool closed = !output || lyra::sd::close(output, lyra::sd::Client::Artwork) == 0;
    if (!written || !closed) {
        lyra::sd::remove(temporary, lyra::sd::Client::Artwork);
        return false;
    }
    lyra::sd::remove(path, lyra::sd::Client::Artwork);
    if (lyra::sd::rename(temporary, path, lyra::sd::Client::Artwork) != 0) {
        lyra::sd::remove(temporary, lyra::sd::Client::Artwork);
        return false;
    }
    ESP_LOGI(kTag, "artwork persisted after SD-backed decode: key=%08lx bytes=%u",
             static_cast<unsigned long>(key), static_cast<unsigned>(bytes));
    return true;
}

bool extract_id3_image(FILE *file, const uint8_t header[10], ArtworkBlob *blob,
                       long tag_start = 0);
bool extract_flac_image(FILE *file, ArtworkBlob *blob);
bool extract_mp4_image(FILE *file, ArtworkBlob *blob);
bool extract_ogg_image(FILE *file, ArtworkBlob *blob);
bool extract_wav_image(FILE *file, ArtworkBlob *blob);
bool extract_aiff_image(FILE *file, ArtworkBlob *blob);

bool extract_embedded_image_from_track(FILE *file, const uint8_t header[10], ArtworkBlob *blob)
{
    if (!file || !blob) return false;
    if (std::memcmp(header, "ID3", 3) == 0) {
        return extract_id3_image(file, header, blob);
    }
    if (std::memcmp(header, "fLaC", 4) == 0) {
        return extract_flac_image(file, blob);
    }
    if (std::memcmp(header + 4, "ftyp", 4) == 0) {
        return extract_mp4_image(file, blob);
    }
    if (std::memcmp(header, "OggS", 4) == 0) {
        return extract_ogg_image(file, blob);
    }
    if (std::memcmp(header, "RIFF", 4) == 0) {
        return extract_wav_image(file, blob);
    }
    if (std::memcmp(header, "FORM", 4) == 0) {
        return extract_aiff_image(file, blob);
    }
    return false;
}

bool extract_id3_image(FILE *file, const uint8_t header[10], ArtworkBlob *blob,
                       long tag_start)
{
    const uint8_t version = header[3];
    if (version < 2 || version > 4) return false;
    const uint32_t tag_size = read_syncsafe32(header + 6);
    if (tag_size == 0) return false;
    uint64_t position = 10;
    const uint64_t tag_end = position + tag_size;

    while (position + (version == 2 ? 6u : 10u) <= tag_end) {
        if (lyra::sd::seek(file, tag_start + static_cast<long>(position), SEEK_SET,
                           lyra::sd::Client::Artwork) != 0) return false;
        uint8_t frame_header[10]{};
        const size_t header_size = version == 2 ? 6 : 10;
        if (lyra::sd::read(file, frame_header, header_size,
                           lyra::sd::Client::Artwork) != header_size) return false;
        if (frame_header[0] == 0) break;
        const bool picture = version == 2 ? std::memcmp(frame_header, "PIC", 3) == 0 :
                                            std::memcmp(frame_header, "APIC", 4) == 0;
        const uint32_t frame_size = version == 2 ?
            ((static_cast<uint32_t>(frame_header[3]) << 16) |
             (static_cast<uint32_t>(frame_header[4]) << 8) | frame_header[5]) :
            (version == 4 ? read_syncsafe32(frame_header + 4) : read_be32(frame_header + 4));
        position += header_size;
        if (frame_size == 0 || position + frame_size > tag_end) break;
        if (picture && read_embedded_image(file, tag_start + position, frame_size, blob)) return true;
        position += frame_size;
        if (position < tag_end) vTaskDelay(1);
    }
    return false;
}

bool extract_wav_image(FILE *file, ArtworkBlob *blob)
{
    if (!file || !blob || lyra::sd::seek(file, 0, SEEK_END, lyra::sd::Client::Artwork) != 0) {
        return false;
    }
    const long file_end = std::ftell(file);
    if (file_end < 20 || lyra::sd::seek(file, 0, SEEK_SET, lyra::sd::Client::Artwork) != 0) {
        return false;
    }
    uint8_t header[12]{};
    if (lyra::sd::read(file, header, sizeof(header), lyra::sd::Client::Artwork) != sizeof(header) ||
        std::memcmp(header, "RIFF", 4) != 0 || std::memcmp(header + 8, "WAVE", 4) != 0) return false;
    uint64_t position = 12;
    while (position + 8 <= static_cast<uint64_t>(file_end)) {
        uint8_t chunk[8]{};
        if (lyra::sd::seek(file, static_cast<long>(position), SEEK_SET,
                           lyra::sd::Client::Artwork) != 0 ||
            lyra::sd::read(file, chunk, sizeof(chunk), lyra::sd::Client::Artwork) != sizeof(chunk)) {
            return false;
        }
        const uint32_t length = read_le32(chunk + 4);
        const uint64_t data_start = position + 8;
        if (data_start + length > static_cast<uint64_t>(file_end)) return false;
        if ((std::memcmp(chunk, "id3 ", 4) == 0 || std::memcmp(chunk, "ID3 ", 4) == 0) &&
            length >= 10 && lyra::sd::seek(file, static_cast<long>(data_start), SEEK_SET,
                                             lyra::sd::Client::Artwork) == 0) {
            uint8_t id3[10]{};
            if (lyra::sd::read(file, id3, sizeof(id3), lyra::sd::Client::Artwork) == sizeof(id3) &&
                std::memcmp(id3, "ID3", 3) == 0 &&
                extract_id3_image(file, id3, blob, static_cast<long>(data_start))) return true;
        }
        position = data_start + length + (length & 1u);
    }
    return false;
}

bool extract_aiff_image(FILE *file, ArtworkBlob *blob)
{
    if (!file || !blob || lyra::sd::seek(file, 0, SEEK_END, lyra::sd::Client::Artwork) != 0) {
        return false;
    }
    const long file_end = std::ftell(file);
    if (file_end < 20 || lyra::sd::seek(file, 0, SEEK_SET, lyra::sd::Client::Artwork) != 0) {
        return false;
    }
    uint8_t form[12]{};
    if (lyra::sd::read(file, form, sizeof(form), lyra::sd::Client::Artwork) != sizeof(form) ||
        std::memcmp(form, "FORM", 4) != 0 ||
        (std::memcmp(form + 8, "AIFF", 4) != 0 && std::memcmp(form + 8, "AIFC", 4) != 0)) return false;
    uint64_t position = 12;
    while (position + 8 <= static_cast<uint64_t>(file_end)) {
        uint8_t chunk[8]{};
        if (lyra::sd::seek(file, static_cast<long>(position), SEEK_SET,
                           lyra::sd::Client::Artwork) != 0 ||
            lyra::sd::read(file, chunk, sizeof(chunk), lyra::sd::Client::Artwork) != sizeof(chunk)) {
            return false;
        }
        const uint32_t length = read_be32(chunk + 4);
        const uint64_t data_start = position + 8;
        if (data_start + length > static_cast<uint64_t>(file_end)) return false;
        if (std::memcmp(chunk, "ID3 ", 4) == 0 && length >= 10 &&
            lyra::sd::seek(file, static_cast<long>(data_start), SEEK_SET,
                           lyra::sd::Client::Artwork) == 0) {
            uint8_t id3[10]{};
            if (lyra::sd::read(file, id3, sizeof(id3), lyra::sd::Client::Artwork) == sizeof(id3) &&
                std::memcmp(id3, "ID3", 3) == 0 &&
                extract_id3_image(file, id3, blob, static_cast<long>(data_start))) return true;
        }
        position = data_start + length + (length & 1u);
    }
    return false;
}

bool extract_flac_image(FILE *file, ArtworkBlob *blob)
{
    uint64_t position = 4;
    bool last = false;
    while (!last) {
        if (lyra::sd::seek(file, static_cast<long>(position), SEEK_SET,
                           lyra::sd::Client::Artwork) != 0) return false;
        uint8_t block_header[4]{};
        if (lyra::sd::read(file, block_header, sizeof(block_header),
                           lyra::sd::Client::Artwork) != sizeof(block_header)) return false;
        last = (block_header[0] & 0x80) != 0;
        const uint8_t type = block_header[0] & 0x7F;
        const uint32_t length = (static_cast<uint32_t>(block_header[1]) << 16) |
                                (static_cast<uint32_t>(block_header[2]) << 8) | block_header[3];
        position += sizeof(block_header);
        if (type == 6 && length >= 32) {
            uint64_t cursor = position;
            uint8_t word[4];
            auto read_word = [&]() -> uint32_t {
                if (lyra::sd::seek(file, static_cast<long>(cursor), SEEK_SET,
                                   lyra::sd::Client::Artwork) != 0 ||
                    lyra::sd::read(file, word, sizeof(word), lyra::sd::Client::Artwork) != sizeof(word)) return UINT32_MAX;
                cursor += 4;
                return read_be32(word);
            };
            (void)read_word(); // Picture type.
            const uint32_t mime_length = read_word();
            if (mime_length == UINT32_MAX || cursor + mime_length > position + length) return false;
            cursor += mime_length;
            const uint32_t description_length = read_word();
            if (description_length == UINT32_MAX || cursor + description_length > position + length) return false;
            cursor += description_length;
            for (int i = 0; i < 4; ++i) if (read_word() == UINT32_MAX) return false;
            const uint32_t data_length = read_word();
            if (data_length != UINT32_MAX && cursor + data_length <= position + length &&
                read_embedded_image(file, cursor, data_length, blob)) return true;
        }
        position += length;
        if (!last) vTaskDelay(1);
    }
    return false;
}

bool extract_mp4_image(FILE *file, ArtworkBlob *blob)
{
    if (lyra::sd::seek(file, 0, SEEK_END, lyra::sd::Client::Artwork) != 0) return false;
    const long end = std::ftell(file);
    if (end < 12) return false;

    // Locate the top-level `moov` atom with header-sized reads. This seeks
    // over `mdat` instead of scanning the entire audio payload on large M4A
    // files. Cover metadata lives beneath `moov`.
    uint64_t moov_start = 0;
    uint64_t moov_end = 0;
    uint64_t position = 0;
    while (position + 8 <= static_cast<uint64_t>(end)) {
        uint8_t header[16]{};
        if (lyra::sd::seek(file, static_cast<long>(position), SEEK_SET,
                           lyra::sd::Client::Artwork) != 0 ||
            lyra::sd::read(file, header, 8, lyra::sd::Client::Artwork) != 8) return false;
        uint64_t atom_size = read_be32(header);
        uint64_t header_size = 8;
        if (atom_size == 1) {
            if (lyra::sd::read(file, header + 8, 8, lyra::sd::Client::Artwork) != 8) return false;
            atom_size = read_be64(header + 8);
            header_size = 16;
        } else if (atom_size == 0) {
            atom_size = static_cast<uint64_t>(end) - position;
        }
        if (atom_size < header_size || position + atom_size > static_cast<uint64_t>(end)) return false;
        if (std::memcmp(header + 4, "moov", 4) == 0) {
            moov_start = position + header_size;
            moov_end = position + atom_size;
            break;
        }
        position += atom_size;
        if (position + 8 <= static_cast<uint64_t>(end)) vTaskDelay(1);
    }
    if (moov_end <= moov_start || lyra::sd::seek(file, static_cast<long>(moov_start), SEEK_SET,
                                                  lyra::sd::Client::Artwork) != 0) return false;

    // Locate `covr` inside the comparatively small metadata tree, then stream
    // its nested payload directly into the in-memory decoder source.
    uint8_t buffer[2056];
    size_t carried = 0;
    uint64_t absolute = moov_start;
    while (absolute < moov_end) {
        const size_t wanted = static_cast<size_t>(std::min<uint64_t>(2048, moov_end - absolute));
        const size_t read = lyra::sd::read(file, buffer + carried, wanted,
                                           lyra::sd::Client::Artwork);
        if (read == 0) break;
        const size_t total = carried + read;
        const uint64_t buffer_start = absolute >= carried ? absolute - carried : 0;
        for (size_t i = 4; i + 4 <= total; ++i) {
            if (std::memcmp(buffer + i, "covr", 4) != 0) continue;
            const uint32_t atom_size = read_be32(buffer + i - 4);
            const uint64_t atom_start = buffer_start + i - 4;
            if (atom_size >= 16 && atom_size <= 16u * 1024u * 1024u &&
                atom_start + atom_size <= moov_end &&
                read_embedded_image(file, atom_start + 8, atom_size - 8, blob)) return true;
            if (lyra::sd::seek(file, static_cast<long>(absolute + read), SEEK_SET,
                               lyra::sd::Client::Artwork) != 0) return false;
        }
        absolute += read;
        carried = std::min<size_t>(8, total);
        std::memmove(buffer, buffer + total - carried, carried);
        if (absolute < moov_end) vTaskDelay(1);
    }
    return false;
}

bool extract_and_decode_album_artwork(const Track &track, uint16_t **out_pixels,
                                      uint16_t artwork_size, bool allow_sd_backing,
                                      bool *used_sd_backing)
{
    if (!out_pixels) return false;
    *out_pixels = nullptr;
    if (used_sd_backing) *used_sd_backing = false;
    FILE *file = lyra::sd::open(track.path, "rb", lyra::sd::Client::Artwork);
    if (!file) return false;
    uint8_t header[10]{};
    const int64_t source_started = esp_timer_get_time();
    const size_t header_read = lyra::sd::read(file, header, sizeof(header),
                                               lyra::sd::Client::Artwork);
    ArtworkBlob blob{};
    blob.psram = true;
    const bool extracted = header_read == sizeof(header) &&
        extract_embedded_image_from_track(file, header, &blob);
    lyra::sd::close(file, lyra::sd::Client::Artwork);
    s_artwork_diagnostics.source_read_us = static_cast<uint32_t>(std::max<int64_t>(
        0, esp_timer_get_time() - source_started));
    s_artwork_diagnostics.source_psram = blob.psram;
    if (!extracted) {
        free_artwork_blob(&blob);
        return false;
    }

    const size_t player_pixels = static_cast<size_t>(artwork_size) * artwork_size;
    auto *player = static_cast<uint16_t *>(alloc_artwork_pixels(
        player_pixels * sizeof(uint16_t)));
    if (!player) {
        heap_caps_free(player);
        free_artwork_blob(&blob);
        return false;
    }

    const int64_t decode_started = esp_timer_get_time();
    const bool decoded = decode_artwork_blob(blob, player, artwork_size, true,
                                             allow_sd_backing, used_sd_backing);
    s_artwork_diagnostics.decode_us = static_cast<uint32_t>(std::max<int64_t>(
        0, esp_timer_get_time() - decode_started));
    ++s_artwork_diagnostics.decode_count;
    s_artwork_diagnostics.player_pixels_internal = esp_ptr_internal(player);
    if (!decoded) {
        ESP_LOGW(kTag, "artwork decode failed: format=%s bytes=%u",
                 blob.format == ArtworkFormat::Png ? "png" : "jpeg",
                 static_cast<unsigned>(blob.length));
        heap_caps_free(player);
        free_artwork_blob(&blob);
        return false;
    }
    ESP_LOGI(kTag, "artwork album decode: source=%u us decode=%u us "
             "compressed=%u bytes source=%s player=%s workspace=%s output=%ux%u",
             static_cast<unsigned>(s_artwork_diagnostics.source_read_us),
             static_cast<unsigned>(s_artwork_diagnostics.decode_us),
             static_cast<unsigned>(blob.length), blob.psram ? "psram" : "internal",
             esp_ptr_internal(player) ? "internal" : "psram",
             used_sd_backing && *used_sd_backing ? "sd" : "psram",
             static_cast<unsigned>(artwork_size), static_cast<unsigned>(artwork_size));
    free_artwork_blob(&blob);
    *out_pixels = player;
    return true;
}

void process_artwork_request(const ArtworkRequest &request)
{
    const uint32_t key = artwork_key(request.track);
    uint16_t artwork_size = kDefaultArtworkSize;
    bool sd_cache_enabled = true;
    {
        Lock lock;
        if ((s_artwork_valid && s_artwork_key == key) ||
            (s_artwork_failed && s_artwork_failed_key == key)) return;
        artwork_size = s_status.artwork_size;
        sd_cache_enabled = s_status.artwork_sd_cache_enabled;
    }

    uint16_t *pixels = nullptr;
    if (!sd_cache_enabled || !read_artwork_cache(key, artwork_size, &pixels)) {
        bool used_sd_backing = false;
        if (!extract_and_decode_album_artwork(request.track, &pixels, artwork_size,
                                              sd_cache_enabled, &used_sd_backing)) {
            Lock lock;
            s_artwork_failed_key = key;
            s_artwork_failed = true;
            return;
        }
        if (used_sd_backing && !write_artwork_cache(key, artwork_size, pixels)) {
            ESP_LOGW(kTag, "could not persist SD-backed artwork cache: key=%08lx",
                     static_cast<unsigned long>(key));
        }
    }
    {
        uint16_t *old_pixels = nullptr;
        {
            Lock lock;
            old_pixels = s_artwork_pixels;
            s_artwork_pixels = pixels;
            s_artwork_key = key;
            s_artwork_pixel_size = artwork_size;
            s_artwork_valid = true;
            s_artwork_failed = false;
        }
        heap_caps_free(old_pixels);
        return;
    }
}

void artwork_task(void *)
{
    while (true) {
        ArtworkRequest request{};
        bool finished = false;
        {
            Lock lock;
            if (s_artwork_queue_count == 0) {
                s_artwork_task_running = false;
                s_artwork_active_hash = 0;
                s_status.artwork_busy = false;
                finished = true;
            } else {
                request = s_artwork_queue[s_artwork_queue_head];
                s_artwork_queue_head = (s_artwork_queue_head + 1) % kArtworkQueueSize;
                --s_artwork_queue_count;
                s_artwork_active_hash = artwork_request_key(request.track, request.size);
            }
        }
        if (finished) break;

        process_artwork_request(request);
        {
            Lock lock;
            ++s_status.artwork_generation;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelete(nullptr);
}

bool playlist_line_to_absolute(const char *line, char *absolute, size_t capacity);

void search_task(void *)
{
    size_t track_count_snapshot = 0;
    uint32_t records_offset = 0;
    uint32_t group_offset = 0;
    uint32_t group_count = 0;
    uint32_t catalog_generation = 0;
    size_t playlist_count_snapshot = 0;
    SearchCategory category = SearchCategory::Songs;
    uint32_t *inverse = nullptr;
    Playlist *playlists = nullptr;
    char query[sizeof(s_cached_search_query)]{};
    char catalog_path[kMaxPath]{};
    {
        Lock lock;
        track_count_snapshot = s_track_count;
        records_offset = s_catalog_header.records_offset;
        catalog_generation = s_status.catalog_generation;
        category = s_cached_search_category;
        copy_text(query, sizeof(query), s_cached_search_query);
        copy_text(catalog_path, sizeof(catalog_path), s_catalog_path);

        if (category == SearchCategory::Songs) {
            inverse = static_cast<uint32_t *>(heap_caps_malloc(
                track_count_snapshot * sizeof(uint32_t), MALLOC_CAP_SPIRAM));
            if (track_count_snapshot && inverse) {
                std::memcpy(inverse, s_physical_to_logical,
                            track_count_snapshot * sizeof(uint32_t));
            }
        } else if (category == SearchCategory::Albums || category == SearchCategory::Artists) {
            const GroupKind kind = category == SearchCategory::Albums ?
                                   GroupKind::Album : GroupKind::Artist;
            group_location(kind, &group_offset, &group_count);
        } else {
            playlist_count_snapshot = s_playlist_count;
            playlists = static_cast<Playlist *>(heap_caps_malloc(
                playlist_count_snapshot * sizeof(Playlist), MALLOC_CAP_SPIRAM));
            if (playlist_count_snapshot && playlists) {
                std::memcpy(playlists, s_playlists,
                            playlist_count_snapshot * sizeof(Playlist));
            }
        }
    }

    const bool reads_catalog = category != SearchCategory::Playlists;
    FILE *file = reads_catalog ? std::fopen(catalog_path, "rb") : nullptr;
    esp_err_t result = (!reads_catalog || file ||
                        (category == SearchCategory::Songs && track_count_snapshot == 0) ||
                        ((category == SearchCategory::Albums || category == SearchCategory::Artists) &&
                         group_count == 0)) ? ESP_OK : ESP_FAIL;
    if (category == SearchCategory::Songs && track_count_snapshot && !inverse) result = ESP_ERR_NO_MEM;
    if (category == SearchCategory::Playlists && playlist_count_snapshot && !playlists) {
        result = ESP_ERR_NO_MEM;
    }

    size_t matched = 0;
    Track track{};
    if (result == ESP_OK && category == SearchCategory::Songs) {
        if (track_count_snapshot && std::fseek(file, records_offset, SEEK_SET) != 0) result = ESP_FAIL;
        for (size_t physical = 0; result == ESP_OK && physical < track_count_snapshot; ++physical) {
            {
                Lock lock;
                if (s_search_cancel_requested || s_shutdown_requested ||
                    catalog_generation != s_status.catalog_generation) {
                    result = ESP_ERR_INVALID_STATE;
                    break;
                }
            }
            if (std::fread(&track, sizeof(track), 1, file) != 1) {
                result = ESP_FAIL;
                break;
            }
            if (contains_ci(track.title, query) || contains_ci(base_name(track.path), query)) {
                if (matched < kMaxTracks) {
                    s_search_results[matched++] = {
                        inverse[physical], 0, 0};
                }
            }
            if ((physical & 31u) == 31u || physical + 1 == track_count_snapshot) {
                Lock lock;
                s_search_status.processed = physical + 1;
            }
        }
    } else if (result == ESP_OK &&
               (category == SearchCategory::Albums || category == SearchCategory::Artists)) {
        if (group_count && std::fseek(file, group_offset, SEEK_SET) != 0) result = ESP_FAIL;
        for (uint32_t group_index = 0; result == ESP_OK && group_index < group_count; ++group_index) {
            {
                Lock lock;
                if (s_search_cancel_requested || s_shutdown_requested ||
                    catalog_generation != s_status.catalog_generation) {
                    result = ESP_ERR_INVALID_STATE;
                    break;
                }
            }
            GroupRecord group{};
            if (std::fread(&group, sizeof(group), 1, file) != 1) {
                result = ESP_FAIL;
                break;
            }
            if (contains_ci(group.name, query) && matched < kMaxTracks) {
                s_search_results[matched++] = {0, group_index, 0};
            }
            if ((group_index & 31u) == 31u || group_index + 1 == group_count) {
                Lock lock;
                s_search_status.processed = group_index + 1;
            }
        }
    } else if (result == ESP_OK && category == SearchCategory::Playlists) {
        size_t total_processed = 0;
        for (size_t playlist_index = 0;
             result == ESP_OK && playlist_index < playlist_count_snapshot; ++playlist_index) {
            FILE *playlist_file = std::fopen(playlists[playlist_index].path, "r");
            if (!playlist_file) {
                result = ESP_FAIL;
                break;
            }
            char line[kMaxPath + 8];
            while (result == ESP_OK && std::fgets(line, sizeof(line), playlist_file)) {
                line[std::strcspn(line, "\r\n")] = '\0';
                if (!line[0] || line[0] == '#') continue;
                ++total_processed;
                {
                    Lock lock;
                    if (s_search_cancel_requested || s_shutdown_requested ||
                        catalog_generation != s_status.catalog_generation) {
                        result = ESP_ERR_INVALID_STATE;
                        break;
                    }
                }
                char absolute[kMaxPath];
                if (!playlist_line_to_absolute(line, absolute, sizeof(absolute))) continue;
                size_t track_index = 0;
                Track playlist_track{};
                bool found = false;
                {
                    Lock lock;
                    found = find_track_by_path_locked(absolute, &track_index) &&
                            track_at_locked(track_index, &playlist_track);
                }
                if (found && contains_ci(playlist_track.title, query) && matched < kMaxTracks) {
                    s_search_results[matched++] = {
                        static_cast<uint32_t>(track_index), 0,
                        static_cast<uint32_t>(playlist_index)};
                }
                if ((total_processed & 31u) == 31u) {
                    Lock lock;
                    s_search_status.processed = total_processed;
                }
            }
            std::fclose(playlist_file);
            {
                Lock lock;
                s_search_status.processed = total_processed;
            }
        }
    }

    if (file) std::fclose(file);
    heap_caps_free(inverse);
    heap_caps_free(playlists);
    {
        Lock lock;
        s_search_status.running = false;
        s_search_status.result = result;
        s_search_status.result_count = result == ESP_OK ? matched : 0;
        s_search_status.ready = result == ESP_OK;
        ++s_search_status.generation;
    }
    vTaskDelete(nullptr);
}

void scan_directory(const char *path, FILE *catalog, size_t *count,
                    bool *capacity_reached, TickType_t *last_progress, size_t depth = 0)
{
    if (depth > 12) return;
    DIR *directory = opendir(path);
    if (!directory) {
        ESP_LOGW(kTag, "cannot open %s", path);
        return;
    }
    while (dirent *entry = readdir(directory)) {
        if (*capacity_reached || std::ferror(catalog)) break;
        if (entry->d_name[0] == '.') continue;
        char child[kMaxPath];
        if (!join_path(child, sizeof(child), path, entry->d_name)) continue;
        struct stat info{};
        if (stat(child, &info) != 0) continue;
        if (S_ISDIR(info.st_mode)) {
            if (std::strcmp(child, kPlaylistDir) != 0 && std::strcmp(child, kDataDir) != 0) {
                scan_directory(child, catalog, count, capacity_reached, last_progress, depth + 1);
            }
        } else if (S_ISREG(info.st_mode) && compatible_audio(child)) {
            if (*count >= kMaxTracks) {
                *capacity_reached = true;
                break;
            }
            Track track{};
            copy_text(track.path, sizeof(track.path), child);
            track.size_bytes = static_cast<uint64_t>(info.st_size);
            track.modified_time = static_cast<uint64_t>(info.st_mtime);
            metadata_from_path(&track);
            // Read only compact text-tag blocks. Embedded pictures remain
            // deferred to the low-priority artwork worker so art-heavy cards
            // do not make the initial scan decode or copy megabytes per song.
            read_fast_metadata(&track);
            if (!track.album_artist[0]) {
                copy_text(track.album_artist, sizeof(track.album_artist), track.artist);
            }
            if (std::fwrite(&track, sizeof(track), 1, catalog) != 1) {
                ESP_LOGE(kTag, "catalog record write failed at track %u", static_cast<unsigned>(*count));
                break;
            }
            ++(*count);
            const TickType_t now = xTaskGetTickCount();
            if (now - *last_progress >= pdMS_TO_TICKS(500)) {
                Lock lock;
                s_status.scan_found = *count;
                *last_progress = now;
            }
        }
    }
    closedir(directory);
}

size_t count_playlist_entries(const char *path)
{
    FILE *file = std::fopen(path, "r");
    if (!file) return 0;
    size_t count = 0;
    char line[kMaxPath + 8];
    while (std::fgets(line, sizeof(line), file)) {
        char *start = line;
        while (*start == ' ' || *start == '\t') ++start;
        if (*start && *start != '#' && *start != '\r' && *start != '\n') ++count;
    }
    std::fclose(file);
    return count;
}

esp_err_t ensure_favorites_file()
{
    if (mkdir(kPlaylistDir, 0775) != 0) {
        struct stat info{};
        if (stat(kPlaylistDir, &info) != 0 || !S_ISDIR(info.st_mode)) return ESP_FAIL;
    }
    struct stat existing{};
    if (stat(kFavoritesPath, &existing) == 0) return ESP_OK;
    if (stat(kFavoritesBackupPath, &existing) == 0 &&
        std::rename(kFavoritesBackupPath, kFavoritesPath) == 0) return ESP_OK;
    FILE *file = std::fopen(kFavoritesPath, "w");
    if (!file) return ESP_FAIL;
    const bool written = std::fputs("#EXTM3U\n", file) >= 0;
    const bool closed = std::fclose(file) == 0;
    return written && closed ? ESP_OK : ESP_FAIL;
}

void load_playlists(Playlist *playlists, size_t *count)
{
    *count = 0;
    DIR *directory = opendir(kPlaylistDir);
    if (!directory) return;
    while (dirent *entry = readdir(directory)) {
        if (*count >= kMaxPlaylists || entry->d_name[0] == '.') continue;
        const char *ext = extension_of(entry->d_name);
        if (!equals_ci(ext, "m3u") && !equals_ci(ext, "m3u8")) continue;
        Playlist &playlist = playlists[(*count)++];
        if (!join_path(playlist.path, sizeof(playlist.path), kPlaylistDir, entry->d_name)) {
            --(*count);
            continue;
        }
        copy_text(playlist.name, sizeof(playlist.name), entry->d_name);
        char *dot = std::strrchr(playlist.name, '.');
        if (dot) *dot = '\0';
        playlist.track_count = count_playlist_entries(playlist.path);
    }
    closedir(directory);
    std::sort(playlists, playlists + *count, [](const Playlist &left, const Playlist &right) {
        const bool left_favorites = std::strcmp(left.path, kFavoritesPath) == 0;
        const bool right_favorites = std::strcmp(right.path, kFavoritesPath) == 0;
        if (left_favorites != right_favorites) return left_favorites;
        return std::strcmp(left.name, right.name) < 0;
    });
}

void reload_playlists_locked()
{
    std::memset(s_playlists, 0, sizeof(s_playlists));
    load_playlists(s_playlists, &s_playlist_count);
    s_status.playlist_count = s_playlist_count;
}

bool publish_playlist_edit(const char *path)
{
    if (!path) return false;
    std::remove(kPlaylistEditBackupPath);
    if (std::rename(path, kPlaylistEditBackupPath) != 0) return false;
    if (std::rename(kPlaylistEditTempPath, path) != 0) {
        std::rename(kPlaylistEditBackupPath, path);
        std::remove(kPlaylistEditTempPath);
        return false;
    }
    std::remove(kPlaylistEditBackupPath);
    return true;
}

bool playlist_line_to_absolute(const char *line, char *absolute, size_t capacity)
{
    if (std::strncmp(line, kMount, std::strlen(kMount)) == 0) {
        copy_text(absolute, capacity, line);
        return true;
    }
    return join_path(absolute, capacity, kMount, line);
}

void refresh_capacity()
{
    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
    if (esp_vfs_fat_info(kMount, &total_bytes, &free_bytes) == ESP_OK) {
        s_status.total_bytes = total_bytes;
        s_status.free_bytes = free_bytes;
        return;
    }
    struct statvfs fs{};
    if (statvfs(kMount, &fs) == 0) {
        const uint64_t block_size = fs.f_frsize ? fs.f_frsize : fs.f_bsize;
        s_status.total_bytes = static_cast<uint64_t>(fs.f_blocks) * block_size;
        s_status.free_bytes = static_cast<uint64_t>(fs.f_bavail) * block_size;
    }
}

bool read_physical(FILE *file, const CatalogHeader &header, uint32_t physical_index, Track *out)
{
    if (!file || !out || physical_index >= header.track_count) return false;
    const long offset = static_cast<long>(header.records_offset) +
                        static_cast<long>(physical_index) * sizeof(Track);
    return std::fseek(file, offset, SEEK_SET) == 0 && std::fread(out, sizeof(Track), 1, file) == 1;
}

void apply_cached_duration_locked(uint32_t physical_index, Track *track)
{
    if (!track || !s_duration_cache || !s_duration_ready ||
        physical_index >= s_track_count || !s_duration_ready[physical_index]) return;
    track->duration_ms = s_duration_cache[physical_index];
}

void refresh_track_file_state(Track *track)
{
    if (!track || !track->path[0]) return;
    struct stat current{};
    if (stat(track->path, &current) == 0 && S_ISREG(current.st_mode)) {
        track->size_bytes = static_cast<uint64_t>(current.st_size);
        track->modified_time = static_cast<uint64_t>(current.st_mtime);
    }
}

bool read_logical_track_locked(uint32_t logical_index, Track *out, bool refresh_file = true)
{
    if (!out || !s_catalog || logical_index >= s_track_count || !s_title_order) return false;
    const uint32_t physical = s_title_order[logical_index];
    if (!read_physical(s_catalog, s_catalog_header, physical, out)) return false;
    apply_cached_duration_locked(physical, out);
    if (refresh_file) refresh_track_file_state(out);
    return true;
}

int compare_sort_entry(const SortEntry &left, const SortEntry &right,
                       SortSetting setting)
{
    int compared = 0;
    if (setting.field == SortField::Title || setting.field == SortField::Album ||
        setting.field == SortField::Artist) {
        compared = std::strcmp(left.primary, right.primary);
    } else if (left.numeric < right.numeric) {
        compared = -1;
    } else if (left.numeric > right.numeric) {
        compared = 1;
    }
    if (compared != 0 && setting.direction == SortDirection::Descending) compared = -compared;
    if (compared != 0) return compared;
    compared = std::strcmp(left.secondary, right.secondary);
    if (compared != 0) return compared;
    if (left.source_index < right.source_index) return -1;
    if (left.source_index > right.source_index) return 1;
    return 0;
}

void populate_track_sort_entry(const Track &track, uint32_t logical_index,
                               SortSetting setting, SortEntry *entry)
{
    if (!entry) return;
    *entry = {};
    entry->source_index = logical_index;
    copy_text(entry->secondary, sizeof(entry->secondary), track.title);
    switch (setting.field) {
    case SortField::Title:
        copy_text(entry->primary, sizeof(entry->primary), track.title);
        break;
    case SortField::Album:
        copy_text(entry->primary, sizeof(entry->primary), track.album);
        break;
    case SortField::Artist:
        copy_text(entry->primary, sizeof(entry->primary), track.artist);
        break;
    case SortField::TrackNumber:
        entry->numeric = track.track_number;
        break;
    case SortField::Duration:
        entry->numeric = track.duration_ms;
        break;
    case SortField::DateModified:
        entry->numeric = track.modified_time;
        break;
    }
}

bool prepare_song_sort_order_locked()
{
    const SortSetting setting = current_sort_setting(SortSection::Songs);
    const uint8_t code = sort_setting_code(setting);
    if (s_song_sort_generation == s_status.catalog_generation &&
        s_song_sort_code == code && (!s_track_count || s_song_sort_order)) return true;

    heap_caps_free(s_song_sort_order);
    s_song_sort_order = nullptr;
    s_song_sort_generation = 0;
    if (s_track_count == 0) {
        s_song_sort_code = code;
        s_song_sort_generation = s_status.catalog_generation;
        return true;
    }
    if (load_sort_cache_locked(SortSection::Songs, code,
                               static_cast<uint32_t>(s_track_count), &s_song_sort_order)) {
        s_song_sort_code = code;
        s_song_sort_generation = s_status.catalog_generation;
        return true;
    }
    return false;
}

bool build_song_sort_order_locked()
{
    const SortSetting setting = current_sort_setting(SortSection::Songs);
    const uint8_t code = sort_setting_code(setting);
    if (prepare_song_sort_order_locked()) return true;

    auto *entries = static_cast<SortEntry *>(heap_caps_calloc(
        s_track_count, sizeof(SortEntry), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    auto *order = static_cast<uint32_t *>(heap_caps_malloc(
        s_track_count * sizeof(uint32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!entries || !order) {
        heap_caps_free(entries);
        heap_caps_free(order);
        return false;
    }
    for (size_t physical = 0; physical < s_track_count; ++physical) {
        Track track{};
        if (!read_physical(s_catalog, s_catalog_header, physical, &track)) {
            heap_caps_free(entries);
            heap_caps_free(order);
            return false;
        }
        apply_cached_duration_locked(physical, &track);
        populate_track_sort_entry(track, s_physical_to_logical[physical], setting,
                                  &entries[physical]);
    }
    std::sort(entries, entries + s_track_count,
              [setting](const SortEntry &left, const SortEntry &right) {
                  return compare_sort_entry(left, right, setting) < 0;
              });
    for (size_t index = 0; index < s_track_count; ++index) {
        order[index] = entries[index].source_index;
    }
    heap_caps_free(entries);
    s_song_sort_order = order;
    s_song_sort_code = code;
    s_song_sort_generation = s_status.catalog_generation;
    save_sort_cache_locked(SortSection::Songs, setting, order,
                           static_cast<uint32_t>(s_track_count));
    return true;
}

bool prepare_group_sort_order_locked(SortSection section)
{
    const bool albums = section == SortSection::Albums;
    const uint32_t groups_count = albums ? s_catalog_header.album_group_count :
                                           s_catalog_header.artist_group_count;
    uint32_t **order_slot = albums ? &s_album_sort_order : &s_artist_sort_order;
    uint32_t *&order = *order_slot;
    uint32_t &generation = albums ? s_album_sort_generation : s_artist_sort_generation;
    uint8_t &sort_code = albums ? s_album_sort_code : s_artist_sort_code;
    const SortSetting setting = current_sort_setting(section);
    const uint8_t code = sort_setting_code(setting);
    if (generation == s_status.catalog_generation && sort_code == code &&
        (!groups_count || order)) return true;

    heap_caps_free(order);
    order = nullptr;
    generation = 0;
    if (groups_count == 0) {
        sort_code = code;
        generation = s_status.catalog_generation;
        return true;
    }
    if (load_sort_cache_locked(section, code, groups_count, order_slot)) {
        sort_code = code;
        generation = s_status.catalog_generation;
        return true;
    }
    return false;
}

bool build_group_sort_order_locked(SortSection section)
{
    const bool albums = section == SortSection::Albums;
    uint32_t groups_offset = albums ? s_catalog_header.album_group_offset :
                                     s_catalog_header.artist_group_offset;
    const uint32_t groups_count = albums ? s_catalog_header.album_group_count :
                                           s_catalog_header.artist_group_count;
    uint32_t **order_slot = albums ? &s_album_sort_order : &s_artist_sort_order;
    uint32_t *&order = *order_slot;
    uint32_t *generation_slot = albums ? &s_album_sort_generation : &s_artist_sort_generation;
    uint32_t &generation = *generation_slot;
    uint8_t *sort_code_slot = albums ? &s_album_sort_code : &s_artist_sort_code;
    uint8_t &sort_code = *sort_code_slot;
    const SortSetting setting = current_sort_setting(section);
    const uint8_t code = sort_setting_code(setting);
    if (generation == s_status.catalog_generation && sort_code == code &&
        (!groups_count || order)) return true;

    heap_caps_free(order);
    order = nullptr;
    generation = 0;
    if (groups_count == 0) {
        sort_code = code;
        generation = s_status.catalog_generation;
        return true;
    }
    if (load_sort_cache_locked(section, code, groups_count, order_slot)) {
        sort_code = code;
        generation = s_status.catalog_generation;
        return true;
    }

    auto *entries = static_cast<SortEntry *>(heap_caps_calloc(
        groups_count, sizeof(SortEntry), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    auto *new_order = static_cast<uint32_t *>(heap_caps_malloc(
        groups_count * sizeof(uint32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!entries || !new_order) {
        heap_caps_free(entries);
        heap_caps_free(new_order);
        return false;
    }

    for (uint32_t group_index = 0; group_index < groups_count; ++group_index) {
        GroupRecord group{};
        if (std::fseek(s_catalog, groups_offset + group_index * sizeof(GroupRecord), SEEK_SET) != 0 ||
            std::fread(&group, sizeof(group), 1, s_catalog) != 1) {
            heap_caps_free(entries);
            heap_caps_free(new_order);
            return false;
        }
        SortEntry &entry = entries[group_index];
        entry = {};
        entry.source_index = group_index;
        copy_text(entry.secondary, sizeof(entry.secondary), group.name);
        Track representative{};
        read_logical_track_locked(group.representative_track, &representative, false);
        uint32_t minimum_track = UINT32_MAX;
        uint64_t total_duration = 0;
        uint64_t latest_modified = 0;
        const bool needs_members = setting.field == SortField::TrackNumber ||
                                    setting.field == SortField::Duration ||
                                    setting.field == SortField::DateModified;
        if (needs_members) {
            for (uint32_t member_index = 0; member_index < group.track_count; ++member_index) {
                if (std::fseek(s_catalog, group.members_offset + member_index * sizeof(uint32_t), SEEK_SET) != 0) {
                    break;
                }
                uint32_t logical_index = 0;
                if (std::fread(&logical_index, sizeof(logical_index), 1, s_catalog) != 1) break;
                Track member{};
                if (!read_logical_track_locked(logical_index, &member, false)) continue;
                if (member.track_number && member.track_number < minimum_track) {
                    minimum_track = member.track_number;
                }
                total_duration = std::min<uint64_t>(UINT64_MAX - member.duration_ms,
                                                    total_duration) + member.duration_ms;
                latest_modified = std::max(latest_modified, member.modified_time);
            }
        }
        switch (setting.field) {
        case SortField::Title:
            copy_text(entry.primary, sizeof(entry.primary), group.name);
            break;
        case SortField::Album:
            copy_text(entry.primary, sizeof(entry.primary), albums ? group.name : representative.album);
            break;
        case SortField::Artist:
            copy_text(entry.primary, sizeof(entry.primary), albums ? representative.artist : group.name);
            break;
        case SortField::TrackNumber:
            entry.numeric = minimum_track == UINT32_MAX ? 0 : minimum_track;
            break;
        case SortField::Duration:
            entry.numeric = total_duration;
            break;
        case SortField::DateModified:
            entry.numeric = latest_modified;
            break;
        }
    }
    std::sort(entries, entries + groups_count,
              [setting](const SortEntry &left, const SortEntry &right) {
                  return compare_sort_entry(left, right, setting) < 0;
              });
    for (uint32_t index = 0; index < groups_count; ++index) {
        new_order[index] = entries[index].source_index;
    }
    heap_caps_free(entries);
    order = new_order;
    sort_code = code;
    generation = s_status.catalog_generation;
    save_sort_cache_locked(section, setting, new_order, groups_count);
    return true;
}

bool read_logical_sort_snapshot(FILE *file, const CatalogHeader &header,
                                const uint32_t *title_order, size_t track_count,
                                const uint32_t *duration_cache,
                                const uint8_t *duration_ready,
                                uint32_t logical_index, Track *out)
{
    if (!file || !title_order || !out || logical_index >= track_count) return false;
    const uint32_t physical = title_order[logical_index];
    if (!read_physical(file, header, physical, out)) return false;
    if (duration_cache && duration_ready && duration_ready[physical]) {
        out->duration_ms = duration_cache[physical];
    }
    return true;
}

void update_sort_task_progress(uint32_t token, size_t processed)
{
    Lock lock;
    if (token == s_sort_catalog_token && s_status.sorting_indexing) {
        s_status.sorting_indexed = processed;
    }
}

void sort_cache_task(void *argument)
{
    const SortSection section = static_cast<SortSection>(
        reinterpret_cast<uintptr_t>(argument));
    CatalogHeader header{};
    char catalog_path[kMaxPath]{};
    SortSetting setting{};
    uint32_t token = 0;
    size_t track_count_snapshot = 0;
    uint32_t *title_order = nullptr;
    uint32_t *physical_to_logical = nullptr;
    uint32_t *duration_cache = nullptr;
    uint8_t *duration_ready = nullptr;
    bool request_valid = false;
    {
        Lock lock;
        request_valid = s_status.sorting_indexing && !s_sort_cancel_requested &&
                        s_catalog && s_catalog_path[0];
        if (!request_valid) {
            // The task was canceled before it acquired the media lock.
            if (s_status.sorting_indexing) {
                s_status.sorting_indexing = false;
                ++s_status.sorting_generation;
            }
        } else {
            token = s_sort_catalog_token;
            header = s_catalog_header;
            copy_text(catalog_path, sizeof(catalog_path), s_catalog_path);
            setting = current_sort_setting(section);
            track_count_snapshot = s_track_count;
            const size_t count = section == SortSection::Songs ? s_track_count :
                                 section == SortSection::Albums ? s_catalog_header.album_group_count :
                                                                  s_catalog_header.artist_group_count;
            s_status.sorting_total = count;
            if (section == SortSection::Songs) {
                physical_to_logical = static_cast<uint32_t *>(heap_caps_malloc(
                    s_track_count * sizeof(uint32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                if (physical_to_logical && s_track_count) {
                    std::memcpy(physical_to_logical, s_physical_to_logical,
                                s_track_count * sizeof(uint32_t));
                }
            } else {
                title_order = static_cast<uint32_t *>(heap_caps_malloc(
                    s_track_count * sizeof(uint32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                if (title_order && s_track_count) {
                    std::memcpy(title_order, s_title_order,
                                s_track_count * sizeof(uint32_t));
                }
            }
            if (setting.field == SortField::Duration && s_track_count) {
                duration_cache = static_cast<uint32_t *>(heap_caps_malloc(
                    s_track_count * sizeof(uint32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                duration_ready = static_cast<uint8_t *>(heap_caps_malloc(
                    s_track_count * sizeof(uint8_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                if (duration_cache && duration_ready) {
                    std::memcpy(duration_cache, s_duration_cache,
                                s_track_count * sizeof(uint32_t));
                    std::memcpy(duration_ready, s_duration_ready,
                                s_track_count * sizeof(uint8_t));
                }
            }
        }
    }
    if (!request_valid) {
        vTaskDelete(nullptr);
        return;
    }

    const size_t item_count = section == SortSection::Songs ? track_count_snapshot :
                              section == SortSection::Albums ? header.album_group_count :
                                                               header.artist_group_count;
    auto *entries = static_cast<SortEntry *>(heap_caps_calloc(
        item_count, sizeof(SortEntry), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    auto *order = static_cast<uint32_t *>(heap_caps_malloc(
        item_count * sizeof(uint32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    FILE *catalog = std::fopen(catalog_path, "rb");
    const bool index_snapshot_ready = section == SortSection::Songs ?
        physical_to_logical != nullptr : title_order != nullptr;
    const bool duration_snapshot_ready = setting.field != SortField::Duration ||
        track_count_snapshot == 0 || (duration_cache && duration_ready);
    bool success = entries && order && catalog && index_snapshot_ready &&
                   duration_snapshot_ready;
    if (success && section == SortSection::Songs) {
        for (size_t physical = 0; physical < track_count_snapshot; ++physical) {
            Track track{};
            if (!read_physical(catalog, header, static_cast<uint32_t>(physical), &track)) {
                success = false;
                break;
            }
            if (duration_cache && duration_ready && duration_ready[physical]) {
                track.duration_ms = duration_cache[physical];
            }
            populate_track_sort_entry(track, physical_to_logical[physical], setting,
                                      &entries[physical]);
            if ((physical & 31u) == 31u) {
                update_sort_task_progress(token, physical + 1);
                vTaskDelay(1);
            }
        }
    } else if (success) {
        const bool albums = section == SortSection::Albums;
        const uint32_t groups_offset = albums ? header.album_group_offset :
                                                header.artist_group_offset;
        for (uint32_t group_index = 0; group_index < item_count; ++group_index) {
            GroupRecord group{};
            if (std::fseek(catalog, groups_offset + group_index * sizeof(GroupRecord), SEEK_SET) != 0 ||
                std::fread(&group, sizeof(group), 1, catalog) != 1) {
                success = false;
                break;
            }
            SortEntry &entry = entries[group_index];
            entry = {};
            entry.source_index = group_index;
            copy_text(entry.secondary, sizeof(entry.secondary), group.name);
            Track representative{};
            read_logical_sort_snapshot(catalog, header, title_order, track_count_snapshot,
                                        duration_cache, duration_ready,
                                        group.representative_track, &representative);
            uint32_t minimum_track = UINT32_MAX;
            uint64_t total_duration = 0;
            uint64_t latest_modified = 0;
            const bool needs_members = setting.field == SortField::TrackNumber ||
                                        setting.field == SortField::Duration ||
                                        setting.field == SortField::DateModified;
            if (needs_members) {
                if (std::fseek(catalog, group.members_offset, SEEK_SET) != 0) {
                    success = false;
                    break;
                }
                for (uint32_t member_index = 0; member_index < group.track_count; ++member_index) {
                    uint32_t logical_index = 0;
                    Track member{};
                    if (std::fseek(catalog, group.members_offset +
                                          member_index * sizeof(uint32_t), SEEK_SET) != 0 ||
                        std::fread(&logical_index, sizeof(logical_index), 1, catalog) != 1 ||
                        !read_logical_sort_snapshot(catalog, header, title_order,
                                                    track_count_snapshot, duration_cache,
                                                    duration_ready, logical_index, &member)) {
                        success = false;
                        break;
                    }
                    if (member.track_number && member.track_number < minimum_track) {
                        minimum_track = member.track_number;
                    }
                    total_duration = std::min<uint64_t>(UINT64_MAX - member.duration_ms,
                                                        total_duration) + member.duration_ms;
                    latest_modified = std::max(latest_modified, member.modified_time);
                }
                if (!success) break;
            }
            switch (setting.field) {
            case SortField::Title:
                copy_text(entry.primary, sizeof(entry.primary), group.name);
                break;
            case SortField::Album:
                copy_text(entry.primary, sizeof(entry.primary), albums ? group.name : representative.album);
                break;
            case SortField::Artist:
                copy_text(entry.primary, sizeof(entry.primary), albums ? representative.artist : group.name);
                break;
            case SortField::TrackNumber:
                entry.numeric = minimum_track == UINT32_MAX ? 0 : minimum_track;
                break;
            case SortField::Duration:
                entry.numeric = total_duration;
                break;
            case SortField::DateModified:
                entry.numeric = latest_modified;
                break;
            }
            if ((group_index & 31u) == 31u) {
                update_sort_task_progress(token, group_index + 1);
                vTaskDelay(1);
            }
        }
    }
    if (catalog) std::fclose(catalog);
    if (success) {
        std::sort(entries, entries + item_count,
                  [setting](const SortEntry &left, const SortEntry &right) {
                      return compare_sort_entry(left, right, setting) < 0;
                  });
        for (size_t index = 0; index < item_count; ++index) {
            order[index] = entries[index].source_index;
        }
    }
    heap_caps_free(entries);
    heap_caps_free(title_order);
    heap_caps_free(physical_to_logical);
    heap_caps_free(duration_cache);
    heap_caps_free(duration_ready);

    {
        Lock lock;
        const bool current = success && !s_sort_cancel_requested &&
                             token == s_sort_catalog_token && s_catalog &&
                             current_sort_setting(section).field == setting.field &&
                             current_sort_setting(section).direction == setting.direction;
        if (current) {
            uint32_t **order_slot = section == SortSection::Songs ? &s_song_sort_order :
                                    section == SortSection::Albums ? &s_album_sort_order :
                                                                      &s_artist_sort_order;
            uint32_t &generation = section == SortSection::Songs ? s_song_sort_generation :
                                    section == SortSection::Albums ? s_album_sort_generation :
                                                                      s_artist_sort_generation;
            uint8_t &sort_code = section == SortSection::Songs ? s_song_sort_code :
                                 section == SortSection::Albums ? s_album_sort_code :
                                                                   s_artist_sort_code;
            heap_caps_free(*order_slot);
            *order_slot = order;
            order = nullptr;
            sort_code = sort_setting_code(setting);
            generation = s_status.catalog_generation;
            save_sort_cache_locked(section, setting, *order_slot,
                                   static_cast<uint32_t>(item_count));
        }
        if (s_status.sorting_indexing) {
            s_status.sorting_indexing = false;
            s_status.sorting_indexed = success ? item_count : 0;
            ++s_status.sorting_generation;
        }
    }
    heap_caps_free(order);
    vTaskDelete(nullptr);
}

void start_sort_cache_indexing_locked(SortSection section)
{
    if (s_status.sorting_indexing || s_status.scanning || s_shutdown_requested ||
        !s_catalog || !s_status.mounted) return;
    const size_t total = section == SortSection::Songs ? s_track_count :
                         section == SortSection::Albums ? s_catalog_header.album_group_count :
                                                          s_catalog_header.artist_group_count;
    if (total == 0) return;
    s_sort_cancel_requested = false;
    s_status.sorting_indexing = true;
    s_status.sorting_indexed = 0;
    s_status.sorting_total = total;
    s_status.sorting_section = static_cast<uint8_t>(section);
    if (xTaskCreatePinnedToCore(sort_cache_task, "lyra_sort", kSortStack,
                                reinterpret_cast<void *>(static_cast<uintptr_t>(section)),
                                1, nullptr, kArtworkCore) != pdPASS) {
        s_status.sorting_indexing = false;
        ++s_status.sorting_generation;
        s_sort_cancel_requested = true;
        ESP_LOGW(kTag, "could not start background sort cache indexer");
    }
}

bool build_group_track_order_locked(GroupKind kind, const GroupRecord &group,
                                    GroupTrackEntry **entries_out)
{
    if (!entries_out) return false;
    *entries_out = nullptr;
    if (group.track_count == 0) return true;
    auto *entries = static_cast<GroupTrackEntry *>(heap_caps_calloc(
        group.track_count, sizeof(GroupTrackEntry), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!entries) return false;
    for (uint32_t index = 0; index < group.track_count; ++index) {
        uint32_t logical_index = 0;
        Track track{};
        if (std::fseek(s_catalog, group.members_offset + index * sizeof(uint32_t), SEEK_SET) != 0 ||
            std::fread(&logical_index, sizeof(logical_index), 1, s_catalog) != 1 ||
            !read_logical_track_locked(logical_index, &track, false)) {
            heap_caps_free(entries);
            return false;
        }
        entries[index].logical_index = logical_index;
        entries[index].disc_number = track.disc_number ? track.disc_number : 1;
        entries[index].track_number = track.track_number ? track.track_number : UINT32_MAX;
        copy_text(entries[index].title, sizeof(entries[index].title), track.title);
    }
    if (kind == GroupKind::Album) {
        std::sort(entries, entries + group.track_count,
                  [](const GroupTrackEntry &left, const GroupTrackEntry &right) {
                      if (left.disc_number != right.disc_number) {
                          return left.disc_number < right.disc_number;
                      }
                      if (left.track_number != right.track_number) {
                          return left.track_number < right.track_number;
                      }
                      const int title = std::strcmp(left.title, right.title);
                      return title == 0 ? left.logical_index < right.logical_index : title < 0;
                  });
    } else if (kind == GroupKind::Artist) {
        std::sort(entries, entries + group.track_count,
                  [](const GroupTrackEntry &left, const GroupTrackEntry &right) {
                      const int title = std::strcmp(left.title, right.title);
                      if (title != 0) return title < 0;
                      if (left.disc_number != right.disc_number) {
                          return left.disc_number < right.disc_number;
                      }
                      if (left.track_number != right.track_number) {
                          return left.track_number < right.track_number;
                      }
                      return left.logical_index < right.logical_index;
                  });
    }
    *entries_out = entries;
    return true;
}

bool populate_sort_keys(FILE *file, const CatalogHeader &header, SortKey *keys,
                        const char *(*field)(const Track &))
{
    if (std::fseek(file, header.records_offset, SEEK_SET) != 0) return false;
    Track track{};
    for (uint32_t i = 0; i < header.track_count; ++i) {
        if (std::fread(&track, sizeof(track), 1, file) != 1) return false;
        copy_text(keys[i].value, sizeof(keys[i].value), field(track));
        keys[i].physical_index = i;
    }
    std::sort(keys, keys + header.track_count, [](const SortKey &left, const SortKey &right) {
        const int compared = std::strcmp(left.value, right.value);
        return compared == 0 ? left.physical_index < right.physical_index : compared < 0;
    });
    return true;
}

bool append_group_index(FILE *file, CatalogHeader *header, SortKey *keys,
                        const uint32_t *physical_to_logical,
                        const char *(*field)(const Track &),
                        uint32_t *group_offset, uint32_t *group_count)
{
    if (!populate_sort_keys(file, *header, keys, field)) return false;
    uint32_t groups = 0;
    for (uint32_t i = 0; i < header->track_count;) {
        ++groups;
        uint32_t next = i + 1;
        while (next < header->track_count && std::strcmp(keys[i].value, keys[next].value) == 0) ++next;
        i = next;
    }
    if (std::fseek(file, 0, SEEK_END) != 0) return false;
    const long records_start = std::ftell(file);
    if (records_start < 0) return false;
    const uint32_t members_start = static_cast<uint32_t>(records_start) + groups * sizeof(GroupRecord);
    uint32_t member_cursor = 0;
    for (uint32_t i = 0; i < header->track_count;) {
        uint32_t next = i + 1;
        while (next < header->track_count && std::strcmp(keys[i].value, keys[next].value) == 0) ++next;
        GroupRecord group{};
        copy_text(group.name, sizeof(group.name), keys[i].value);
        group.track_count = next - i;
        group.members_offset = members_start + member_cursor * sizeof(uint32_t);
        group.representative_track = physical_to_logical[keys[i].physical_index];
        if (std::fwrite(&group, sizeof(group), 1, file) != 1) return false;
        member_cursor += group.track_count;
        i = next;
    }
    for (uint32_t i = 0; i < header->track_count; ++i) {
        const uint32_t logical = physical_to_logical[keys[i].physical_index];
        if (std::fwrite(&logical, sizeof(logical), 1, file) != 1) return false;
    }
    *group_offset = static_cast<uint32_t>(records_start);
    *group_count = groups;
    return true;
}

bool finish_catalog(FILE *file, CatalogHeader *header, SortKey *keys, bool capacity_reached)
{
    if (!populate_sort_keys(file, *header, keys, [](const Track &track) { return track.title; })) return false;
    auto *inverse = static_cast<uint32_t *>(heap_caps_malloc(
        header->track_count * sizeof(uint32_t), MALLOC_CAP_SPIRAM));
    auto *paths = static_cast<PathIndex *>(heap_caps_malloc(
        header->track_count * sizeof(PathIndex), MALLOC_CAP_SPIRAM));
    if ((header->track_count && !inverse) || (header->track_count && !paths)) {
        heap_caps_free(inverse);
        heap_caps_free(paths);
        return false;
    }

    if (std::fseek(file, 0, SEEK_END) != 0) {
        heap_caps_free(inverse);
        heap_caps_free(paths);
        return false;
    }
    header->title_order_offset = static_cast<uint32_t>(std::ftell(file));
    for (uint32_t logical = 0; logical < header->track_count; ++logical) {
        const uint32_t physical = keys[logical].physical_index;
        inverse[physical] = logical;
        if (std::fwrite(&physical, sizeof(physical), 1, file) != 1) {
            heap_caps_free(inverse);
            heap_caps_free(paths);
            return false;
        }
    }

    Track track{};
    if (std::fseek(file, header->records_offset, SEEK_SET) != 0) {
        heap_caps_free(inverse);
        heap_caps_free(paths);
        return false;
    }
    for (uint32_t physical = 0; physical < header->track_count; ++physical) {
        if (std::fread(&track, sizeof(track), 1, file) != 1) {
            heap_caps_free(inverse);
            heap_caps_free(paths);
            return false;
        }
        paths[physical] = {hash_path(track.path), inverse[physical], 0};
    }
    std::sort(paths, paths + header->track_count, [](const PathIndex &left, const PathIndex &right) {
        return left.hash == right.hash ? left.track_index < right.track_index : left.hash < right.hash;
    });
    if (std::fseek(file, 0, SEEK_END) != 0) {
        heap_caps_free(inverse);
        heap_caps_free(paths);
        return false;
    }
    header->path_index_offset = static_cast<uint32_t>(std::ftell(file));
    if (header->track_count && std::fwrite(paths, sizeof(PathIndex), header->track_count, file) != header->track_count) {
        heap_caps_free(inverse);
        heap_caps_free(paths);
        return false;
    }

    const bool groups_ok =
        append_group_index(file, header, keys, inverse, [](const Track &track) { return track.artist; },
                           &header->artist_group_offset, &header->artist_group_count) &&
        append_group_index(file, header, keys, inverse, [](const Track &track) { return track.album; },
                           &header->album_group_offset, &header->album_group_count) &&
        append_group_index(file, header, keys, inverse, [](const Track &track) { return track.genre; },
                           &header->genre_group_offset, &header->genre_group_count) &&
        append_group_index(file, header, keys, inverse, [](const Track &track) { return track.year; },
                           &header->year_group_offset, &header->year_group_count);
    heap_caps_free(inverse);
    heap_caps_free(paths);
    if (!groups_ok || std::fseek(file, 0, SEEK_END) != 0) return false;
    header->file_size = static_cast<uint32_t>(std::ftell(file));
    header->flags = capacity_reached ? kCatalogFlagCapacityReached : 0;
    if (std::fflush(file) != 0 ||
        !checksum_file_payload(file, header->records_offset, header->file_size, &header->checksum)) return false;
    if (std::fseek(file, 0, SEEK_SET) != 0 || std::fwrite(header, sizeof(*header), 1, file) != 1) return false;
    return std::fflush(file) == 0;
}

bool validate_catalog(FILE *file, CatalogHeader *header, bool verify_checksum = true)
{
    if (!file || std::fseek(file, 0, SEEK_SET) != 0 || std::fread(header, sizeof(*header), 1, file) != 1) return false;
    if (std::memcmp(header->magic, kCatalogMagic, sizeof(kCatalogMagic)) != 0 ||
        header->version != kCatalogVersion || header->header_size != sizeof(CatalogHeader) ||
        header->track_size != sizeof(Track) || header->track_count > kMaxTracks ||
        header->records_offset != sizeof(CatalogHeader)) return false;
    struct stat info{};
    if (fstat(fileno(file), &info) != 0 || static_cast<uint64_t>(info.st_size) != header->file_size) return false;
    const uint64_t records_end = static_cast<uint64_t>(header->records_offset) +
                                 static_cast<uint64_t>(header->track_count) * sizeof(Track);
    const uint64_t order_end = static_cast<uint64_t>(header->title_order_offset) +
                               static_cast<uint64_t>(header->track_count) * sizeof(uint32_t);
    const uint64_t paths_end = static_cast<uint64_t>(header->path_index_offset) +
                               static_cast<uint64_t>(header->track_count) * sizeof(PathIndex);
    if (records_end > header->file_size || order_end > header->file_size || paths_end > header->file_size ||
        header->artist_group_count > header->track_count || header->album_group_count > header->track_count ||
        header->genre_group_count > header->track_count || header->year_group_count > header->track_count ||
        static_cast<uint64_t>(header->artist_group_offset) + header->artist_group_count * sizeof(GroupRecord) > header->file_size ||
        static_cast<uint64_t>(header->album_group_offset) + header->album_group_count * sizeof(GroupRecord) > header->file_size ||
        static_cast<uint64_t>(header->genre_group_offset) + header->genre_group_count * sizeof(GroupRecord) > header->file_size ||
        static_cast<uint64_t>(header->year_group_offset) + header->year_group_count * sizeof(GroupRecord) > header->file_size) return false;
    if (!verify_checksum) return true;
    uint32_t checksum = 0;
    return checksum_file_payload(file, header->records_offset, header->file_size, &checksum) &&
           checksum == header->checksum;
}

void clear_runtime_catalog()
{
    s_search_cancel_requested = true;
    s_duration_cancel_requested = true;
    s_sort_cancel_requested = true;
    ++s_duration_catalog_token;
    ++s_sort_catalog_token;
    s_search_status.ready = false;
    if (s_catalog) std::fclose(s_catalog);
    s_catalog = nullptr;
    s_catalog_path[0] = '\0';
    heap_caps_free(s_title_order);
    heap_caps_free(s_physical_to_logical);
    heap_caps_free(s_path_index);
    heap_caps_free(s_song_sort_order);
    heap_caps_free(s_album_sort_order);
    heap_caps_free(s_artist_sort_order);
    heap_caps_free(s_artist_album_order);
    heap_caps_free(s_album_for_logical);
    heap_caps_free(s_duration_cache);
    heap_caps_free(s_duration_ready);
    s_title_order = nullptr;
    s_physical_to_logical = nullptr;
    s_path_index = nullptr;
    s_song_sort_order = nullptr;
    s_album_sort_order = nullptr;
    s_artist_sort_order = nullptr;
    s_artist_album_order = nullptr;
    s_album_for_logical = nullptr;
    s_duration_cache = nullptr;
    s_duration_ready = nullptr;
    s_artist_album_count = 0;
    s_artist_album_artist = SIZE_MAX;
    s_artist_album_generation = 0;
    s_song_sort_generation = 0;
    s_album_sort_generation = 0;
    s_artist_sort_generation = 0;
    s_track_count = 0;
    s_catalog_header = {};
    s_status.track_count = 0;
    s_status.capacity_reached = false;
    s_status.duration_indexing = false;
    s_status.duration_indexed = 0;
    ++s_status.duration_generation;
    s_status.sorting_indexing = false;
    s_status.sorting_indexed = 0;
    s_status.sorting_total = 0;
    ++s_status.sorting_generation;
    heap_caps_free(s_artwork_pixels);
    s_artwork_pixels = nullptr;
    s_artwork_pixel_size = 0;
    s_artwork_key = 0;
    s_artwork_valid = false;
    s_artwork_failed_key = 0;
    s_artwork_failed = false;
    ++s_status.catalog_generation;
}

bool remove_existing_file(const char *path)
{
    struct stat info{};
    return stat(path, &info) != 0 || std::remove(path) == 0;
}

bool remove_directory_files(const char *directory_path)
{
    DIR *directory = opendir(directory_path);
    if (!directory) return true;
    bool success = true;
    while (dirent *entry = readdir(directory)) {
        if (entry->d_name[0] == '.') continue;
        char path[kMaxPath];
        if (!join_path(path, sizeof(path), directory_path, entry->d_name)) {
            success = false;
            continue;
        }
        struct stat info{};
        if (stat(path, &info) == 0 && S_ISREG(info.st_mode) && std::remove(path) != 0) success = false;
    }
    closedir(directory);
    return success;
}

esp_err_t clear_playlists_locked()
{
    const bool removed = remove_directory_files(kPlaylistDir);
    const bool favorites_ready = ensure_favorites_file() == ESP_OK;
    std::memset(s_playlists, 0, sizeof(s_playlists));
    load_playlists(s_playlists, &s_playlist_count);
    s_status.playlist_count = s_playlist_count;
    return removed && favorites_ready ? ESP_OK : ESP_FAIL;
}

esp_err_t clear_artwork_cache_locked()
{
    const bool removed = remove_directory_files(kArtworkDir);
    heap_caps_free(s_artwork_pixels);
    s_artwork_pixels = nullptr;
    s_artwork_pixel_size = 0;
    s_artwork_key = 0;
    s_artwork_valid = false;
    s_artwork_failed_key = 0;
    s_artwork_failed = false;
    ++s_status.artwork_generation;
    return removed ? ESP_OK : ESP_FAIL;
}

esp_err_t clear_catalog_locked()
{
    clear_runtime_catalog();
    bool removed = remove_existing_file(kCatalogPath);
    removed = remove_existing_file(kCatalogTempPath) && removed;
    removed = remove_existing_file(kCatalogBackupPath) && removed;
    return removed ? ESP_OK : ESP_FAIL;
}

bool load_catalog_file(const char *path, bool verify_checksum = true)
{
    FILE *file = std::fopen(path, "rb");
    CatalogHeader header{};
    if (!file || !validate_catalog(file, &header, verify_checksum)) {
        if (file) std::fclose(file);
        return false;
    }
    auto *order = static_cast<uint32_t *>(heap_caps_malloc(header.track_count * sizeof(uint32_t), MALLOC_CAP_SPIRAM));
    auto *inverse = static_cast<uint32_t *>(heap_caps_malloc(header.track_count * sizeof(uint32_t), MALLOC_CAP_SPIRAM));
    auto *paths = static_cast<PathIndex *>(heap_caps_malloc(header.track_count * sizeof(PathIndex), MALLOC_CAP_SPIRAM));
    auto *duration_cache = static_cast<uint32_t *>(heap_caps_calloc(
        header.track_count, sizeof(uint32_t), MALLOC_CAP_SPIRAM));
    auto *duration_ready = static_cast<uint8_t *>(heap_caps_calloc(
        header.track_count, sizeof(uint8_t), MALLOC_CAP_SPIRAM));
    auto *album_for_logical = static_cast<uint32_t *>(heap_caps_malloc(
        header.track_count * sizeof(uint32_t), MALLOC_CAP_SPIRAM));
    const bool allocated = header.track_count == 0 ||
                           (order && inverse && paths && duration_cache && duration_ready &&
                            album_for_logical);
    bool read = allocated &&
        (header.track_count == 0 ||
         (std::fseek(file, header.title_order_offset, SEEK_SET) == 0 &&
          std::fread(order, sizeof(uint32_t), header.track_count, file) == header.track_count &&
          std::fseek(file, header.path_index_offset, SEEK_SET) == 0 &&
          std::fread(paths, sizeof(PathIndex), header.track_count, file) == header.track_count));
    if (read && header.track_count) {
        std::memset(inverse, 0xFF, header.track_count * sizeof(uint32_t));
        for (uint32_t logical = 0; logical < header.track_count; ++logical) {
            const uint32_t physical = order[logical];
            if (physical >= header.track_count || inverse[physical] != UINT32_MAX) {
                read = false;
                break;
            }
            inverse[physical] = logical;
        }
        for (uint32_t i = 0; read && i < header.track_count; ++i) {
            if (paths[i].track_index >= header.track_count ||
                (i > 0 && paths[i - 1].hash > paths[i].hash)) read = false;
        }
        if (read) {
            std::memset(album_for_logical, 0xFF,
                        header.track_count * sizeof(uint32_t));
            for (uint32_t album_index = 0;
                 read && album_index < header.album_group_count; ++album_index) {
                GroupRecord album{};
                if (std::fseek(file, header.album_group_offset +
                                      album_index * sizeof(GroupRecord), SEEK_SET) != 0 ||
                    std::fread(&album, sizeof(album), 1, file) != 1 ||
                    static_cast<uint64_t>(album.members_offset) +
                        static_cast<uint64_t>(album.track_count) * sizeof(uint32_t) >
                        header.file_size ||
                    std::fseek(file, album.members_offset, SEEK_SET) != 0) {
                    read = false;
                    break;
                }
                for (uint32_t member = 0; member < album.track_count; ++member) {
                    uint32_t logical_index = 0;
                    if (std::fread(&logical_index, sizeof(logical_index), 1, file) != 1 ||
                        logical_index >= header.track_count) {
                        read = false;
                        break;
                    }
                    album_for_logical[logical_index] = album_index;
                }
            }
        }
    }
    if (!read) {
        heap_caps_free(order);
        heap_caps_free(inverse);
        heap_caps_free(paths);
        heap_caps_free(duration_cache);
        heap_caps_free(duration_ready);
        heap_caps_free(album_for_logical);
        std::fclose(file);
        return false;
    }
    clear_runtime_catalog();
    s_catalog = file;
    copy_text(s_catalog_path, sizeof(s_catalog_path), path);
    s_catalog_header = header;
    s_title_order = order;
    s_physical_to_logical = inverse;
    s_path_index = paths;
    s_album_for_logical = album_for_logical;
    s_duration_cache = duration_cache;
    s_duration_ready = duration_ready;
    s_duration_cancel_requested = false;
    s_sort_cancel_requested = false;
    s_track_count = header.track_count;
    s_status.track_count = header.track_count;
    s_status.capacity_reached = (header.flags & kCatalogFlagCapacityReached) != 0;
    ++s_status.catalog_generation;
    return true;
}

void load_cached_state()
{
    bool loaded = load_catalog_file(kCatalogPath);
    if (!loaded) loaded = load_catalog_file(kCatalogBackupPath);
    ensure_favorites_file();
    load_playlists(s_playlists, &s_playlist_count);
    if (loaded) {
        ESP_LOGI(kTag, "restored %u-track MicroSD catalog; resident indexes=%u bytes",
                 static_cast<unsigned>(s_track_count),
                 static_cast<unsigned>(s_track_count * (sizeof(uint32_t) * 2 + sizeof(PathIndex))));
    } else {
        ESP_LOGI(kTag, "no valid saved catalog; run Scan music library once");
    }
    s_status.playlist_count = s_playlist_count;
}

esp_err_t publish_catalog()
{
    Lock lock;
    clear_runtime_catalog();
    std::remove(kCatalogBackupPath);
    if (std::rename(kCatalogPath, kCatalogBackupPath) != 0) {
        struct stat existing{};
        if (stat(kCatalogPath, &existing) == 0) {
            load_catalog_file(kCatalogPath);
            return ESP_FAIL;
        }
    }
    if (std::rename(kCatalogTempPath, kCatalogPath) != 0) {
        std::rename(kCatalogBackupPath, kCatalogPath);
        load_catalog_file(kCatalogPath);
        return ESP_FAIL;
    }
    if (!load_catalog_file(kCatalogPath, false)) {
        std::remove(kCatalogPath);
        std::rename(kCatalogBackupPath, kCatalogPath);
        load_catalog_file(kCatalogPath);
        return ESP_FAIL;
    }
    std::remove(kCatalogBackupPath);
    return ESP_OK;
}

bool duration_sort_requested_locked()
{
    for (const SortSetting setting : s_sort_settings) {
        if (setting.field == SortField::Duration) return true;
    }
    return false;
}

void duration_task(void *)
{
    uint32_t catalog_token = 0;
    size_t count = 0;
    {
        Lock lock;
        catalog_token = s_duration_catalog_token;
        count = s_track_count;
    }

    for (size_t physical = 0; physical < count; ++physical) {
        char path[kMaxPath]{};
        uint32_t catalog_duration = 0;
        {
            Lock lock;
            if (s_duration_cancel_requested || s_shutdown_requested ||
                catalog_token != s_duration_catalog_token || !s_catalog) break;
            Track track{};
            if (!read_physical(s_catalog, s_catalog_header, static_cast<uint32_t>(physical), &track)) break;
            catalog_duration = track.duration_ms;
            if (s_duration_ready && s_duration_ready[physical]) {
                ++s_status.duration_indexed;
                continue;
            }
            if (catalog_duration != 0) {
                if (s_duration_cache) s_duration_cache[physical] = catalog_duration;
                if (s_duration_ready) s_duration_ready[physical] = 1;
                ++s_status.duration_indexed;
                continue;
            }
            copy_text(path, sizeof(path), track.path);
        }

        const uint32_t duration = path[0] ? lyra::audio::probe_duration_ms(path) : 0;
        {
            Lock lock;
            if (s_duration_cancel_requested || s_shutdown_requested ||
                catalog_token != s_duration_catalog_token) break;
            if (s_duration_cache) s_duration_cache[physical] = duration;
            if (s_duration_ready) s_duration_ready[physical] = 1;
            ++s_status.duration_indexed;
        }
        // Keep the low-priority indexer cooperative even for formats whose
        // duration parser has to inspect a large stream.
        if ((physical & 7u) == 7u) vTaskDelay(1);
    }

    {
        Lock lock;
        if (catalog_token == s_duration_catalog_token &&
            !s_duration_cancel_requested && !s_shutdown_requested) {
            s_status.duration_indexing = false;
            s_song_sort_generation = 0;
            s_album_sort_generation = 0;
            s_artist_sort_generation = 0;
            for (size_t index = 0; index < kTrackCacheSize; ++index) {
                s_track_cache[index].generation = 0;
            }
            ++s_status.duration_generation;
        } else if (s_status.duration_indexing) {
            s_status.duration_indexing = false;
        }
    }
    vTaskDelete(nullptr);
}

void start_duration_indexing_if_needed()
{
    {
        Lock lock;
        if (!s_catalog || !s_status.mounted || s_status.scanning ||
            s_status.duration_indexing || s_shutdown_requested ||
            !duration_sort_requested_locked()) return;
        s_status.duration_indexing = true;
        s_status.duration_indexed = 0;
        s_duration_cancel_requested = false;
    }
    if (xTaskCreatePinnedToCore(duration_task, "lyra_duration", kDurationStack,
                                nullptr, 1, nullptr, kArtworkCore) != pdPASS) {
        Lock lock;
        s_status.duration_indexing = false;
        s_duration_cancel_requested = true;
        ESP_LOGW(kTag, "could not start background duration indexer");
    }
}

void scan_task(void *)
{
    const TickType_t scan_started = xTaskGetTickCount();
    TickType_t discovery_finished = scan_started;
    Playlist *playlists = static_cast<Playlist *>(std::calloc(kMaxPlaylists, sizeof(Playlist)));
    SortKey *keys = static_cast<SortKey *>(heap_caps_malloc(kMaxTracks * sizeof(SortKey), MALLOC_CAP_SPIRAM));
    size_t track_count = 0;
    size_t playlist_count = 0;
    bool capacity_reached = false;
    esp_err_t result = (!playlists || !keys) ? ESP_ERR_NO_MEM : ESP_OK;
    if (result == ESP_OK && !ensure_directory(kDataDir)) result = ESP_FAIL;
    FILE *file = result == ESP_OK ? std::fopen(kCatalogTempPath, "wb+") : nullptr;
    if (result == ESP_OK && !file) result = ESP_FAIL;
    CatalogHeader header{};
    std::memcpy(header.magic, kCatalogMagic, sizeof(header.magic));
    header.version = kCatalogVersion;
    header.header_size = sizeof(CatalogHeader);
    header.track_size = sizeof(Track);
    header.records_offset = sizeof(CatalogHeader);
    if (result == ESP_OK && std::fwrite(&header, sizeof(header), 1, file) != 1) result = ESP_FAIL;
    if (result == ESP_OK) {
        DIR *root = opendir(kMount);
        if (!root) {
            result = ESP_FAIL;
        } else {
            closedir(root);
            TickType_t last_progress = xTaskGetTickCount();
            scan_directory(kMount, file, &track_count, &capacity_reached, &last_progress);
            discovery_finished = xTaskGetTickCount();
            {
                Lock lock;
                s_status.scan_found = track_count;
                s_status.scan_indexing = true;
            }
            header.track_count = static_cast<uint32_t>(track_count);
            if (std::ferror(file) || !finish_catalog(file, &header, keys, capacity_reached) ||
                fsync(fileno(file)) != 0) result = ESP_FAIL;
        }
    }
    if (file && std::fclose(file) != 0) result = ESP_FAIL;
    if (result == ESP_OK) {
        load_playlists(playlists, &playlist_count);
        result = publish_catalog();
        if (result == ESP_OK) {
            Lock lock;
            warm_sort_caches_locked();
        }
    } else {
        std::remove(kCatalogTempPath);
    }
    {
        Lock lock;
        if (result == ESP_OK) {
            std::memcpy(s_playlists, playlists, playlist_count * sizeof(Playlist));
            s_playlist_count = playlist_count;
            s_status.playlist_count = playlist_count;
        }
        s_status.last_error = result;
        s_status.scanning = false;
        s_status.scan_found = track_count;
        s_status.scan_indexing = false;
        refresh_capacity();
    }
    heap_caps_free(keys);
    std::free(playlists);
    if (result == ESP_OK) {
        ESP_LOGI(kTag, "scan complete: %u tracks%s, %u playlists; discovery=%u ms total=%u ms; stack margin %u bytes",
                 static_cast<unsigned>(track_count), capacity_reached ? " (10,000 limit reached)" : "",
                 static_cast<unsigned>(playlist_count),
                 static_cast<unsigned>((discovery_finished - scan_started) * portTICK_PERIOD_MS),
                 static_cast<unsigned>((xTaskGetTickCount() - scan_started) * portTICK_PERIOD_MS),
                 static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
        start_duration_indexing_if_needed();
    } else {
        ESP_LOGE(kTag, "scan failed after %u tracks: %s; previous catalog retained",
                 static_cast<unsigned>(track_count), esp_err_to_name(result));
    }
    vTaskDelete(nullptr);
}

} // namespace

esp_err_t init()
{
    if (!lyra::sd::init()) return ESP_ERR_NO_MEM;
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;
    if (!s_track_cache) s_track_cache = static_cast<CachedTrack *>(
        heap_caps_calloc(kTrackCacheSize, sizeof(CachedTrack), MALLOC_CAP_SPIRAM));
    if (!s_track_cache) return ESP_ERR_NO_MEM;
    s_shutdown_requested = false;
    const esp_err_t nvs_result = ensure_nvs_ready();
    if (nvs_result != ESP_OK) {
        ESP_LOGW(kTag, "artwork settings persistence unavailable: %s",
                 esp_err_to_name(nvs_result));
    }
    load_artwork_settings();
    load_sort_settings();
    ESP_LOGI(kTag, "artwork settings: sd_cache=%s size=%ux%u",
             s_status.artwork_sd_cache_enabled ? "enabled" : "disabled",
             static_cast<unsigned>(s_status.artwork_size),
             static_cast<unsigned>(s_status.artwork_size));

    const esp_err_t result = mount_sd_card();
    {
        Lock lock;
        s_status.mounted = result == ESP_OK;
        s_status.last_error = result;
        if (result == ESP_OK) refresh_capacity();
    }
    if (result == ESP_OK) {
        if (ensure_directory(kDataDir)) remove_stale_jpeg_work_files();
        load_cached_state();
        start_duration_indexing_if_needed();
        ESP_LOGI(kTag, "MicroSD mounted; library scan is user initiated");
    } else {
        ESP_LOGW(kTag, "MicroSD mount failed: %s", esp_err_to_name(result));
    }
    return result;
}

Status status() { Lock lock; return s_status; }

esp_err_t clear_playlists()
{
    Lock lock;
    if (!s_status.mounted || s_status.scanning || s_status.artwork_busy || s_search_status.running) return ESP_ERR_INVALID_STATE;
    const esp_err_t result = clear_playlists_locked();
    refresh_capacity();
    return result;
}

esp_err_t clear_artwork_cache()
{
    Lock lock;
    if (!s_status.mounted || s_status.scanning || s_status.artwork_busy ||
        s_search_status.running) return ESP_ERR_INVALID_STATE;
    const esp_err_t result = clear_artwork_cache_locked();
    refresh_capacity();
    return result;
}

esp_err_t clear_all_databases()
{
    Lock lock;
    if (!s_status.mounted || s_status.scanning || s_status.duration_indexing ||
        s_status.sorting_indexing ||
        s_status.artwork_busy || s_search_status.running) return ESP_ERR_INVALID_STATE;
    const esp_err_t playlists = clear_playlists_locked();
    const esp_err_t artwork = clear_artwork_cache_locked();
    const esp_err_t catalog = clear_catalog_locked();
    refresh_capacity();
    return playlists == ESP_OK && artwork == ESP_OK && catalog == ESP_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t set_artwork_sd_cache_enabled(bool enabled)
{
    Lock lock;
    if (s_status.artwork_busy || s_status.scanning) return ESP_ERR_INVALID_STATE;
    if (s_status.artwork_sd_cache_enabled == enabled) return ESP_OK;
    s_status.artwork_sd_cache_enabled = enabled;
    heap_caps_free(s_artwork_pixels);
    s_artwork_pixels = nullptr;
    s_artwork_pixel_size = 0;
    s_artwork_valid = false;
    s_artwork_failed = false;
    ++s_status.artwork_generation;
    return save_artwork_settings();
}

esp_err_t set_artwork_size(uint16_t size)
{
    if (size != kDefaultArtworkSize && size != kLargeArtworkSize) return ESP_ERR_INVALID_ARG;
    Lock lock;
    if (s_status.artwork_busy || s_status.scanning) return ESP_ERR_INVALID_STATE;
    if (s_status.artwork_size == size) return ESP_OK;
    s_status.artwork_size = size;
    heap_caps_free(s_artwork_pixels);
    s_artwork_pixels = nullptr;
    s_artwork_pixel_size = 0;
    s_artwork_valid = false;
    s_artwork_failed = false;
    ++s_status.artwork_generation;
    return save_artwork_settings();
}

SortSetting sort_setting(SortSection section)
{
    Lock lock;
    return current_sort_setting(section);
}

esp_err_t set_sort_setting(SortSection section, SortField field,
                           SortDirection direction)
{
    if (static_cast<uint8_t>(section) >= 3 ||
        static_cast<uint8_t>(field) >= 6 ||
        static_cast<uint8_t>(direction) >= 2) return ESP_ERR_INVALID_ARG;
    esp_err_t result = ESP_OK;
    {
        Lock lock;
        const SortSetting next{field, direction};
        const size_t index = static_cast<size_t>(section);
        if (s_sort_settings[index].field == next.field &&
            s_sort_settings[index].direction == next.direction) return ESP_OK;
        s_sort_settings[index] = next;
        if (section == SortSection::Songs) {
            s_song_sort_generation = 0;
        } else if (section == SortSection::Albums) {
            s_album_sort_generation = 0;
        } else {
            s_artist_sort_generation = 0;
        }
        result = save_sort_setting(section);
    }
    if (field == SortField::Duration) start_duration_indexing_if_needed();
    if (field != SortField::Duration) {
        const bool title_default = field == SortField::Title &&
                                    direction == SortDirection::Ascending;
        if (!title_default) {
            Lock lock;
            const bool ready = section == SortSection::Songs ? prepare_song_sort_order_locked() :
                               prepare_group_sort_order_locked(section);
            if (!ready) start_sort_cache_indexing_locked(section);
        }
    }
    return result;
}

ArtworkDiagnostics artwork_diagnostics()
{
    Lock lock;
    return s_artwork_diagnostics;
}

esp_err_t start_search(SearchCategory category, const char *query)
{
    if (!query || !query[0]) return ESP_ERR_INVALID_ARG;
    {
        Lock lock;
        if (!s_status.mounted || !s_catalog || s_status.scanning ||
            s_search_status.running || s_shutdown_requested) return ESP_ERR_INVALID_STATE;
        if (!s_search_results) {
            s_search_results = static_cast<SearchResult *>(
                heap_caps_malloc(kMaxTracks * sizeof(SearchResult), MALLOC_CAP_SPIRAM));
            if (!s_search_results) return ESP_ERR_NO_MEM;
        }
        s_cached_search_category = category;
        copy_text(s_cached_search_query, sizeof(s_cached_search_query), query);
        s_search_cancel_requested = false;
        s_search_status.running = true;
        s_search_status.ready = false;
        s_search_status.processed = 0;
        if (category == SearchCategory::Playlists) {
            size_t total = 0;
            for (size_t i = 0; i < s_playlist_count; ++i) total += s_playlists[i].track_count;
            s_search_status.total = total;
        } else if (category == SearchCategory::Albums) {
            s_search_status.total = s_catalog_header.album_group_count;
        } else if (category == SearchCategory::Artists) {
            s_search_status.total = s_catalog_header.artist_group_count;
        } else {
            s_search_status.total = s_track_count;
        }
        s_search_status.result_count = 0;
        s_search_status.result = ESP_OK;
    }
    if (xTaskCreatePinnedToCore(search_task, "lyra_search", 8192, nullptr, 1, nullptr, 0) != pdPASS) {
        Lock lock;
        s_search_status.running = false;
        s_search_status.result = ESP_ERR_NO_MEM;
        ++s_search_status.generation;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

SearchStatus search_status()
{
    Lock lock;
    return s_search_status;
}

size_t search_results(size_t offset, SearchResult *results, size_t capacity, size_t *total)
{
    if (!results || capacity == 0) return 0;
    Lock lock;
    if (!s_search_status.ready) return 0;
    if (total) *total = s_search_status.result_count;
    if (offset >= s_search_status.result_count) return 0;
    const size_t count = std::min(capacity, s_search_status.result_count - offset);
    std::memcpy(results, s_search_results + offset, count * sizeof(SearchResult));
    return count;
}

esp_err_t start_scan()
{
    {
        Lock lock;
        if (s_shutdown_requested) return ESP_ERR_INVALID_STATE;
    }
    if (!status().mounted) {
        const esp_err_t mount_result = init();
        if (mount_result != ESP_OK) return mount_result;
    }
    {
        Lock lock;
        if (!s_status.mounted) return ESP_ERR_INVALID_STATE;
        if (s_status.scanning || s_status.duration_indexing || s_status.sorting_indexing) {
            return ESP_ERR_INVALID_STATE;
        }
        if (s_status.artwork_busy || s_search_status.running) return ESP_ERR_INVALID_STATE;
        s_status.scanning = true;
        s_status.last_error = ESP_OK;
        s_status.scan_found = 0;
        s_status.scan_indexing = false;
    }
    if (xTaskCreatePinnedToCore(scan_task, "lyra_sd_scan", kScanStack, nullptr, 2, nullptr, 0) != pdPASS) {
        Lock lock;
        s_status.scanning = false;
        s_status.last_error = ESP_ERR_NO_MEM;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t shutdown(uint32_t timeout_ms)
{
    {
        Lock lock;
        s_shutdown_requested = true;
        s_artwork_queue_count = 0;
        s_search_cancel_requested = true;
        s_sort_cancel_requested = true;
    }

    const TickType_t started = xTaskGetTickCount();
    while (true) {
        bool storage_busy;
        {
            Lock lock;
            storage_busy = s_status.scanning || s_status.duration_indexing ||
                           s_status.sorting_indexing ||
                           s_status.artwork_busy || s_search_status.running;
        }
        if (!storage_busy) break;
        if ((xTaskGetTickCount() - started) * portTICK_PERIOD_MS >= timeout_ms) {
            ESP_LOGE(kTag, "timed out waiting for MicroSD background work before shutdown");
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    Lock lock;
    heap_caps_free(s_artwork_pixels);
    s_artwork_pixels = nullptr;
    s_artwork_valid = false;
    s_artwork_key = 0;
    if (!s_status.mounted || !s_card) return ESP_OK;
    clear_runtime_catalog();
    const esp_err_t result = esp_vfs_fat_sdcard_unmount(kMount, s_card);
    if (result == ESP_OK) {
        s_card = nullptr;
        s_status.mounted = false;
        ESP_LOGI(kTag, "MicroSD safely unmounted");
    } else {
        load_catalog_file(kCatalogPath);
        ESP_LOGE(kTag, "MicroSD unmount failed: %s", esp_err_to_name(result));
    }
    return result;
}

size_t track_count() { Lock lock; return s_track_count; }

bool track_at_locked(size_t index, Track *out)
{
    if (!out || !s_catalog || index >= s_track_count) return false;
    const uint32_t physical = s_title_order[index];
    for (size_t i = 0; i < kTrackCacheSize; ++i) {
        const CachedTrack &cached = s_track_cache[i];
        if (cached.generation == s_status.catalog_generation && cached.physical_index == physical) {
            *out = cached.track;
            return true;
        }
    }
    CachedTrack &slot = s_track_cache[s_cache_cursor++ % kTrackCacheSize];
    if (!read_physical(s_catalog, s_catalog_header, physical, &slot.track)) return false;
    apply_cached_duration_locked(physical, &slot.track);
    // Treat the path as authoritative. This also repairs display data from an
    // older/corrupt cache record without trusting a serialized format or size.
    copy_text(slot.track.format, sizeof(slot.track.format), extension_of(slot.track.path));
    refresh_track_file_state(&slot.track);
    slot.physical_index = physical;
    slot.generation = s_status.catalog_generation;
    *out = slot.track;
    return true;
}

bool track_at(size_t index, Track *out)
{
    Lock lock;
    return track_at_locked(index, out);
}

bool sorted_track_at(size_t index, size_t *track_index)
{
    if (!track_index) return false;
    Lock lock;
    if (index >= s_track_count) return false;
    const SortSetting setting = current_sort_setting(SortSection::Songs);
    if (setting.field == SortField::Title && setting.direction == SortDirection::Ascending) {
        *track_index = index;
        return true;
    }
    if (!prepare_song_sort_order_locked()) {
        start_sort_cache_indexing_locked(SortSection::Songs);
        if (!s_title_order || !s_physical_to_logical) return false;
        *track_index = s_physical_to_logical[s_title_order[index]];
        return true;
    }
    *track_index = s_song_sort_order[index];
    return true;
}

bool copy_artwork(const Track &track, uint16_t *pixels, size_t pixel_count)
{
    if (!pixels || !track.path[0]) return false;
    Lock lock;
    const size_t required = static_cast<size_t>(s_status.artwork_size) * s_status.artwork_size;
    if (pixel_count < required || !s_artwork_valid || !s_artwork_pixels ||
        s_artwork_pixel_size != s_status.artwork_size ||
        s_artwork_key != artwork_key(track)) return false;
    std::memcpy(pixels, s_artwork_pixels, required * sizeof(uint16_t));
    return true;
}

esp_err_t request_artwork(const Track &track, ArtworkSize size)
{
    (void)size;
    if (!track.path[0]) return ESP_ERR_INVALID_ARG;
    const uint32_t key = artwork_key(track);

    bool start_worker = false;
    {
        Lock lock;
        if (!s_status.mounted || s_status.scanning || s_shutdown_requested) return ESP_ERR_INVALID_STATE;
        if (s_artwork_valid && s_artwork_key == key) return ESP_OK;
        if (s_artwork_failed && s_artwork_failed_key == key) return ESP_ERR_NOT_FOUND;
        const uint64_t wanted_hash = artwork_request_key(track, size);
        if (s_artwork_task_running && s_artwork_active_hash == wanted_hash) return ESP_OK;
        for (size_t i = 0; i < s_artwork_queue_count; ++i) {
            const ArtworkRequest &queued = s_artwork_queue[(s_artwork_queue_head + i) % kArtworkQueueSize];
            if (artwork_key(queued.track) == artwork_key(track)) return ESP_OK;
        }
        if (s_artwork_queue_count >= kArtworkQueueSize) return ESP_ERR_NO_MEM;
        ArtworkRequest &request = s_artwork_queue[(s_artwork_queue_head + s_artwork_queue_count) % kArtworkQueueSize];
        request.track = track;
        request.size = size;
        ++s_artwork_queue_count;
        if (!s_artwork_task_running) {
            s_artwork_task_running = true;
            s_status.artwork_busy = true;
            start_worker = true;
        }
    }
    if (start_worker && xTaskCreatePinnedToCore(artwork_task, "lyra_artwork", kScanStack,
                                                nullptr, 1, nullptr, kArtworkCore) != pdPASS) {
        Lock lock;
        s_artwork_task_running = false;
        s_status.artwork_busy = false;
        s_artwork_queue_count = 0;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void group_location(GroupKind kind, uint32_t *offset, uint32_t *count)
{
    if (kind == GroupKind::Artist) {
        *offset = s_catalog_header.artist_group_offset;
        *count = s_catalog_header.artist_group_count;
    } else if (kind == GroupKind::Album) {
        *offset = s_catalog_header.album_group_offset;
        *count = s_catalog_header.album_group_count;
    } else if (kind == GroupKind::Genre) {
        *offset = s_catalog_header.genre_group_offset;
        *count = s_catalog_header.genre_group_count;
    } else {
        *offset = s_catalog_header.year_group_offset;
        *count = s_catalog_header.year_group_count;
    }
}

size_t group_count(GroupKind kind)
{
    Lock lock;
    uint32_t offset = 0;
    uint32_t count = 0;
    group_location(kind, &offset, &count);
    return count;
}

bool group_at(GroupKind kind, size_t index, Group *out)
{
    if (!out) return false;
    Lock lock;
    uint32_t offset = 0;
    uint32_t count = 0;
    group_location(kind, &offset, &count);
    if (!s_catalog || index >= count ||
        std::fseek(s_catalog, offset + index * sizeof(GroupRecord), SEEK_SET) != 0) return false;
    GroupRecord record{};
    if (std::fread(&record, sizeof(record), 1, s_catalog) != 1) return false;
    copy_text(out->name, sizeof(out->name), record.name);
    out->track_count = record.track_count;
    out->representative_track = record.representative_track;
    return true;
}

bool sorted_group_index_at(SortSection section, size_t index, size_t *group_index)
{
    if (!group_index || section == SortSection::Songs) return false;
    Lock lock;
    const GroupKind kind = section == SortSection::Albums ? GroupKind::Album : GroupKind::Artist;
    uint32_t count = 0;
    uint32_t ignored_offset = 0;
    group_location(kind, &ignored_offset, &count);
    if (index >= count) return false;
    const SortSetting setting = current_sort_setting(section);
    if (setting.field == SortField::Title && setting.direction == SortDirection::Ascending) {
        *group_index = index;
        return true;
    }
    if (!prepare_group_sort_order_locked(section)) {
        start_sort_cache_indexing_locked(section);
        *group_index = index;
        return true;
    }
    const uint32_t *order = section == SortSection::Albums ? s_album_sort_order : s_artist_sort_order;
    if (!order) return false;
    *group_index = order[index];
    return true;
}

size_t group_tracks(GroupKind kind, size_t group_index, size_t offset,
                    size_t *track_indices, size_t capacity)
{
    if (!track_indices || capacity == 0) return 0;
    Lock lock;
    uint32_t groups_offset = 0;
    uint32_t groups_count = 0;
    group_location(kind, &groups_offset, &groups_count);
    if (!s_catalog || group_index >= groups_count ||
        std::fseek(s_catalog, groups_offset + group_index * sizeof(GroupRecord), SEEK_SET) != 0) return 0;
    GroupRecord group{};
    if (std::fread(&group, sizeof(group), 1, s_catalog) != 1 || offset >= group.track_count) return 0;
    const size_t wanted = std::min(capacity, static_cast<size_t>(group.track_count) - offset);
    if (kind == GroupKind::Album || kind == GroupKind::Artist) {
        GroupTrackEntry *entries = nullptr;
        if (!build_group_track_order_locked(kind, group, &entries)) return 0;
        for (size_t index = 0; index < wanted; ++index) {
            track_indices[index] = entries[offset + index].logical_index;
        }
        heap_caps_free(entries);
        return wanted;
    }
    if (std::fseek(s_catalog, group.members_offset + offset * sizeof(uint32_t), SEEK_SET) != 0) return 0;
    return std::fread(track_indices, sizeof(uint32_t), wanted, s_catalog);
}

bool group_adjacent_track(GroupKind kind, size_t group_index, size_t current_track,
                          int direction, size_t *track_index)
{
    if (!track_index || direction == 0) return false;
    Lock lock;
    uint32_t groups_offset = 0;
    uint32_t groups_count = 0;
    group_location(kind, &groups_offset, &groups_count);
    if (!s_catalog || group_index >= groups_count ||
        std::fseek(s_catalog, groups_offset + group_index * sizeof(GroupRecord), SEEK_SET) != 0) return false;
    GroupRecord group{};
    if (std::fread(&group, sizeof(group), 1, s_catalog) != 1 || group.track_count == 0 ||
        std::fseek(s_catalog, group.members_offset, SEEK_SET) != 0) return false;

    if (kind == GroupKind::Album || kind == GroupKind::Artist) {
        GroupTrackEntry *entries = nullptr;
        if (!build_group_track_order_locked(kind, group, &entries)) return false;
        size_t current_position = SIZE_MAX;
        for (size_t index = 0; index < group.track_count; ++index) {
            if (entries[index].logical_index == current_track) {
                current_position = index;
                break;
            }
        }
        if (current_position == SIZE_MAX) {
            heap_caps_free(entries);
            return false;
        }
        const int64_t next = static_cast<int64_t>(current_position) + direction;
        const size_t target = next < 0 ? group.track_count - 1 :
                              next >= static_cast<int64_t>(group.track_count) ? 0 :
                              static_cast<size_t>(next);
        *track_index = entries[target].logical_index;
        heap_caps_free(entries);
        return true;
    }

    uint32_t first = 0;
    uint32_t previous = 0;
    uint32_t member = 0;
    bool found_current = false;
    for (uint32_t i = 0; i < group.track_count; ++i) {
        if (std::fread(&member, sizeof(member), 1, s_catalog) != 1) return false;
        if (i == 0) first = member;
        if (direction > 0 && found_current) {
            *track_index = member;
            return true;
        }
        if (member == current_track) {
            if (direction < 0 && i > 0) {
                *track_index = previous;
                return true;
            }
            found_current = true;
        }
        previous = member;
    }
    if (!found_current) return false;
    *track_index = direction < 0 ? previous : first;
    return true;
}

bool build_artist_album_order_locked(size_t artist_group_index)
{
    if (s_artist_album_generation == s_status.catalog_generation &&
        s_artist_album_artist == artist_group_index) return true;
    heap_caps_free(s_artist_album_order);
    s_artist_album_order = nullptr;
    s_artist_album_count = 0;
    s_artist_album_artist = SIZE_MAX;
    s_artist_album_generation = 0;

    uint32_t artist_offset = 0;
    uint32_t artist_count = 0;
    group_location(GroupKind::Artist, &artist_offset, &artist_count);
    if (!s_catalog || artist_group_index >= artist_count) return false;
    GroupRecord artist{};
    if (std::fseek(s_catalog, artist_offset + artist_group_index * sizeof(GroupRecord), SEEK_SET) != 0 ||
        std::fread(&artist, sizeof(artist), 1, s_catalog) != 1) return false;

    const uint32_t match_capacity = std::max<uint32_t>(artist.track_count, 1u);
    auto *matches = static_cast<uint32_t *>(heap_caps_malloc(
        match_capacity * sizeof(uint32_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!matches) return false;
    size_t match_count = 0;
    if (!s_album_for_logical ||
        std::fseek(s_catalog, artist.members_offset, SEEK_SET) != 0) {
        heap_caps_free(matches);
        return false;
    }
    for (uint32_t member_index = 0; member_index < artist.track_count; ++member_index) {
        uint32_t logical_index = 0;
        if (std::fread(&logical_index, sizeof(logical_index), 1, s_catalog) != 1 ||
            logical_index >= s_track_count) break;
        const uint32_t album_index = s_album_for_logical[logical_index];
        if (album_index == UINT32_MAX) continue;
        bool duplicate = false;
        for (size_t existing = 0; existing < match_count; ++existing) {
            if (matches[existing] == album_index) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate && match_count < match_capacity) matches[match_count++] = album_index;
    }
    std::sort(matches, matches + match_count);
    s_artist_album_order = matches;
    s_artist_album_count = match_count;
    s_artist_album_artist = artist_group_index;
    s_artist_album_generation = s_status.catalog_generation;
    return true;
}

size_t artist_album_count(size_t artist_group_index)
{
    Lock lock;
    if (!build_artist_album_order_locked(artist_group_index)) return 0;
    return s_artist_album_count;
}

bool artist_album_at(size_t artist_group_index, size_t offset, size_t *album_group_index)
{
    if (!album_group_index) return false;
    Lock lock;
    if (!build_artist_album_order_locked(artist_group_index) ||
        offset >= s_artist_album_count) return false;
    *album_group_index = s_artist_album_order[offset];
    return true;
}

size_t playlist_count() { Lock lock; return s_playlist_count; }

bool playlist_at(size_t index, Playlist *out)
{
    if (!out) return false;
    Lock lock;
    if (index >= s_playlist_count) return false;
    *out = s_playlists[index];
    return true;
}

bool find_track_by_path_locked(const char *path, size_t *track_index)
{
    if (!path || !track_index || !s_path_index) return false;
    const uint64_t wanted = hash_path(path);
    const PathIndex probe{wanted, 0, 0};
    const PathIndex *begin = s_path_index;
    const PathIndex *end = s_path_index + s_track_count;
    const PathIndex *candidate = std::lower_bound(begin, end, probe,
        [](const PathIndex &left, const PathIndex &right) { return left.hash < right.hash; });
    if (candidate == end || candidate->hash != wanted) return false;
    if (candidate + 1 == end || (candidate + 1)->hash != wanted) {
        *track_index = candidate->track_index;
        return true;
    }
    Track track{};
    for (; candidate != end && candidate->hash == wanted; ++candidate) {
        if (track_at_locked(candidate->track_index, &track) && std::strcmp(track.path, path) == 0) {
            *track_index = candidate->track_index;
            return true;
        }
    }
    return false;
}

size_t playlist_tracks(size_t playlist_index, size_t offset,
                       size_t *track_indices, size_t capacity)
{
    if (!track_indices || capacity == 0) return 0;
    char playlist_path[kMaxPath];
    {
        Lock lock;
        if (playlist_index >= s_playlist_count) return 0;
        copy_text(playlist_path, sizeof(playlist_path), s_playlists[playlist_index].path);
    }
    FILE *file = std::fopen(playlist_path, "r");
    if (!file) return 0;
    size_t found = 0;
    size_t entry = 0;
    char line[kMaxPath + 8];
    while (found < capacity && std::fgets(line, sizeof(line), file)) {
        line[std::strcspn(line, "\r\n")] = '\0';
        if (!line[0] || line[0] == '#') continue;
        char absolute[kMaxPath];
        if (!playlist_line_to_absolute(line, absolute, sizeof(absolute))) continue;
        if (entry++ < offset) continue;
        Lock lock;
        size_t track_index = 0;
        if (find_track_by_path_locked(absolute, &track_index)) {
            track_indices[found++] = track_index;
        }
    }
    std::fclose(file);
    return found;
}

bool playlist_adjacent_track(size_t playlist_index, size_t current_track,
                             int direction, size_t *track_index)
{
    if (!track_index || direction == 0) return false;
    char playlist_path[kMaxPath];
    {
        Lock lock;
        if (playlist_index >= s_playlist_count) return false;
        copy_text(playlist_path, sizeof(playlist_path), s_playlists[playlist_index].path);
    }
    FILE *file = std::fopen(playlist_path, "r");
    if (!file) return false;

    size_t first = 0;
    size_t previous = 0;
    bool have_first = false;
    bool have_previous = false;
    bool found_current = false;
    bool success = false;
    char line[kMaxPath + 8];
    while (std::fgets(line, sizeof(line), file)) {
        line[std::strcspn(line, "\r\n")] = '\0';
        if (!line[0] || line[0] == '#') continue;
        char absolute[kMaxPath];
        if (!playlist_line_to_absolute(line, absolute, sizeof(absolute))) continue;
        size_t member = 0;
        {
            Lock lock;
            if (!find_track_by_path_locked(absolute, &member)) continue;
        }
        if (!have_first) {
            first = member;
            have_first = true;
        }
        if (direction > 0 && found_current) {
            *track_index = member;
            success = true;
            break;
        }
        if (!found_current && member == current_track) {
            if (direction < 0 && have_previous) {
                *track_index = previous;
                success = true;
                break;
            }
            found_current = true;
        }
        previous = member;
        have_previous = true;
    }
    if (!success && found_current && have_first) {
        *track_index = direction < 0 ? previous : first;
        success = true;
    }
    std::fclose(file);
    return success;
}

esp_err_t create_playlist(const char *name, size_t *created_index)
{
    if (!name || !*name) return ESP_ERR_INVALID_ARG;
    char safe[kMaxName];
    size_t used = 0;
    for (const char *p = name; *p && used + 1 < sizeof(safe); ++p) {
        const unsigned char ch = static_cast<unsigned char>(*p);
        if (ch >= 32 && *p != '/' && *p != '\\' && *p != ':' && *p != '*' && *p != '?' &&
            *p != '"' && *p != '<' && *p != '>' && *p != '|') safe[used++] = *p;
    }
    while (used && safe[used - 1] == ' ') --used;
    safe[used] = '\0';
    if (!used) return ESP_ERR_INVALID_ARG;
    if (mkdir(kPlaylistDir, 0775) != 0) {
        struct stat info{};
        if (stat(kPlaylistDir, &info) != 0 || !S_ISDIR(info.st_mode)) return ESP_FAIL;
    }
    char path[kMaxPath];
    char filename[kMaxName + 6];
    const size_t safe_length = std::strlen(safe);
    std::memcpy(filename, safe, safe_length);
    std::memcpy(filename + safe_length, ".m3u8", 6);
    if (!join_path(path, sizeof(path), kPlaylistDir, filename)) return ESP_ERR_INVALID_SIZE;
    struct stat existing{};
    if (stat(path, &existing) == 0) return ESP_ERR_INVALID_STATE;
    FILE *file = std::fopen(path, "w");
    if (!file) return ESP_FAIL;
    std::fputs("#EXTM3U\n", file);
    if (std::fclose(file) != 0) return ESP_FAIL;
    Lock lock;
    if (s_playlist_count >= kMaxPlaylists) return ESP_ERR_NO_MEM;
    Playlist &playlist = s_playlists[s_playlist_count];
    copy_text(playlist.name, sizeof(playlist.name), safe);
    copy_text(playlist.path, sizeof(playlist.path), path);
    playlist.track_count = 0;
    if (created_index) *created_index = s_playlist_count;
    ++s_playlist_count;
    s_status.playlist_count = s_playlist_count;
    ++s_status.catalog_generation;
    return ESP_OK;
}

esp_err_t add_to_playlist(size_t playlist_index, size_t track_index)
{
    char playlist_path[kMaxPath];
    char track_path[kMaxPath];
    {
        Lock lock;
        if (playlist_index >= s_playlist_count || track_index >= s_track_count) return ESP_ERR_INVALID_ARG;
        if (std::strcmp(s_playlists[playlist_index].path, kFavoritesPath) == 0) {
            // Release the lock before the favorite service performs file I/O.
            playlist_path[0] = '\0';
        } else {
            copy_text(playlist_path, sizeof(playlist_path), s_playlists[playlist_index].path);
        }
        Track track{};
        if (!track_at_locked(track_index, &track)) return ESP_FAIL;
        copy_text(track_path, sizeof(track_path), track.path);
    }
    if (!playlist_path[0]) return set_favorite(track_index, true);
    FILE *file = std::fopen(playlist_path, "a");
    if (!file) return ESP_FAIL;
    const char *relative = std::strncmp(track_path, kMount, std::strlen(kMount)) == 0
                               ? track_path + std::strlen(kMount) : track_path;
    std::fprintf(file, "%s\n", relative);
    if (std::fclose(file) != 0) return ESP_FAIL;
    Lock lock;
    ++s_playlists[playlist_index].track_count;
    ++s_status.catalog_generation;
    return ESP_OK;
}

esp_err_t remove_from_playlist(size_t playlist_index, size_t track_index)
{
    char playlist_path[kMaxPath];
    char track_path[kMaxPath];
    bool favorites = false;
    {
        Lock lock;
        if (!s_status.mounted || s_shutdown_requested ||
            playlist_index >= s_playlist_count || track_index >= s_track_count) {
            return ESP_ERR_INVALID_STATE;
        }
        favorites = std::strcmp(s_playlists[playlist_index].path, kFavoritesPath) == 0;
        copy_text(playlist_path, sizeof(playlist_path), s_playlists[playlist_index].path);
        Track track{};
        if (!track_at_locked(track_index, &track)) return ESP_FAIL;
        copy_text(track_path, sizeof(track_path), track.path);
    }
    if (favorites) return set_favorite(track_index, false);

    std::remove(kPlaylistEditTempPath);
    FILE *input = std::fopen(playlist_path, "r");
    FILE *output = std::fopen(kPlaylistEditTempPath, "w");
    if (!input || !output) {
        if (input) std::fclose(input);
        if (output) std::fclose(output);
        std::remove(kPlaylistEditTempPath);
        return ESP_FAIL;
    }
    bool removed = false;
    bool written = true;
    char line[kMaxPath + 8];
    while (std::fgets(line, sizeof(line), input)) {
        char normalized[kMaxPath + 8];
        copy_text(normalized, sizeof(normalized), line);
        normalized[std::strcspn(normalized, "\r\n")] = '\0';
        char absolute[kMaxPath];
        const bool matches = !removed && normalized[0] && normalized[0] != '#' &&
                             playlist_line_to_absolute(normalized, absolute, sizeof(absolute)) &&
                             std::strcmp(absolute, track_path) == 0;
        if (matches) {
            removed = true;
            continue;
        }
        if (std::fputs(line, output) < 0) {
            written = false;
            break;
        }
    }
    written = written && std::fflush(output) == 0 && fsync(fileno(output)) == 0;
    const bool input_closed = std::fclose(input) == 0;
    const bool output_closed = std::fclose(output) == 0;
    if (!removed || !written || !input_closed || !output_closed ||
        !publish_playlist_edit(playlist_path)) {
        std::remove(kPlaylistEditTempPath);
        return removed ? ESP_FAIL : ESP_ERR_NOT_FOUND;
    }

    Lock lock;
    reload_playlists_locked();
    ++s_status.catalog_generation;
    return ESP_OK;
}

esp_err_t delete_playlist(size_t playlist_index)
{
    char playlist_path[kMaxPath];
    bool favorites = false;
    {
        Lock lock;
        if (!s_status.mounted || s_shutdown_requested || playlist_index >= s_playlist_count) {
            return ESP_ERR_INVALID_STATE;
        }
        copy_text(playlist_path, sizeof(playlist_path), s_playlists[playlist_index].path);
        favorites = std::strcmp(playlist_path, kFavoritesPath) == 0;
    }

    if (favorites) {
        if (ensure_favorites_file() != ESP_OK) return ESP_FAIL;
        std::remove(kPlaylistEditTempPath);
        FILE *file = std::fopen(kPlaylistEditTempPath, "w");
        if (!file) return ESP_FAIL;
        const bool written = std::fputs("#EXTM3U\n", file) >= 0 &&
                             std::fflush(file) == 0 && fsync(fileno(file)) == 0;
        const bool closed = std::fclose(file) == 0;
        if (!written || !closed || !publish_playlist_edit(playlist_path)) {
            std::remove(kPlaylistEditTempPath);
            return ESP_FAIL;
        }
    } else if (std::remove(playlist_path) != 0) {
        return ESP_FAIL;
    }

    Lock lock;
    reload_playlists_locked();
    ++s_status.catalog_generation;
    return ESP_OK;
}

bool queue_snapshot_exists()
{
    {
        Lock lock;
        if (!s_status.mounted || s_shutdown_requested) return false;
    }
    struct stat info{};
    if (stat(kQueueSnapshotPath, &info) == 0 && S_ISREG(info.st_mode)) return true;
    // Complete the backup-style publish used below if power was lost after
    // moving the previous snapshot out of the way but before publishing temp.
    if (stat(kQueueSnapshotBackupPath, &info) != 0 || !S_ISREG(info.st_mode)) return false;
    if (std::rename(kQueueSnapshotBackupPath, kQueueSnapshotPath) != 0) return false;
    return stat(kQueueSnapshotPath, &info) == 0 && S_ISREG(info.st_mode);
}

esp_err_t save_queue_snapshot(const size_t *track_indices, size_t track_count,
                              size_t current_position)
{
    if (!track_indices || track_count == 0 || track_count > kMaxTracks ||
        current_position >= track_count) return ESP_ERR_INVALID_ARG;
    {
        Lock lock;
        if (!s_status.mounted || s_shutdown_requested) return ESP_ERR_INVALID_STATE;
    }
    if (!ensure_directory(kDataDir)) return ESP_FAIL;

    std::remove(kQueueSnapshotTempPath);
    FILE *file = std::fopen(kQueueSnapshotTempPath, "w");
    if (!file) return ESP_FAIL;
    bool written = std::fputs("#EXTM3U\n#LYRA_QUEUE_V1\n", file) >= 0 &&
                   std::fprintf(file, "#CURRENT=%u\n",
                                static_cast<unsigned>(current_position)) > 0;
    for (size_t position = 0; written && position < track_count; ++position) {
        Track track{};
        {
            Lock lock;
            if (!s_status.mounted || s_shutdown_requested ||
                !track_at_locked(track_indices[position], &track)) {
                written = false;
            }
        }
        if (!written) break;
        const char *relative = std::strncmp(track.path, kMount, std::strlen(kMount)) == 0
                                   ? track.path + std::strlen(kMount) : track.path;
        written = std::fprintf(file, "%s\n", relative) > 0;
    }
    written = written && std::fflush(file) == 0 && fsync(fileno(file)) == 0;
    const bool closed = std::fclose(file) == 0;
    if (!written || !closed) {
        std::remove(kQueueSnapshotTempPath);
        return ESP_FAIL;
    }

    std::remove(kQueueSnapshotBackupPath);
    if (std::rename(kQueueSnapshotPath, kQueueSnapshotBackupPath) != 0 && errno != ENOENT) {
        std::remove(kQueueSnapshotTempPath);
        return ESP_FAIL;
    }
    if (std::rename(kQueueSnapshotTempPath, kQueueSnapshotPath) != 0) {
        std::rename(kQueueSnapshotBackupPath, kQueueSnapshotPath);
        std::remove(kQueueSnapshotTempPath);
        return ESP_FAIL;
    }
    std::remove(kQueueSnapshotBackupPath);
    return ESP_OK;
}

size_t load_queue_snapshot(size_t *track_indices, size_t capacity,
                           size_t *current_position)
{
    if (!track_indices || capacity == 0) return 0;
    if (!queue_snapshot_exists()) return 0;
    FILE *file = std::fopen(kQueueSnapshotPath, "r");
    if (!file) return 0;

    bool version_valid = false;
    bool current_valid = false;
    size_t saved_current = 0;
    size_t saved_entry = 0;
    size_t restored_current = 0;
    bool restored_current_valid = false;
    size_t restored = 0;
    char line[kMaxPath + 32];
    while (std::fgets(line, sizeof(line), file)) {
        line[std::strcspn(line, "\r\n")] = '\0';
        if (std::strcmp(line, "#LYRA_QUEUE_V1") == 0) {
            version_valid = true;
            continue;
        }
        if (std::strncmp(line, "#CURRENT=", 9) == 0) {
            char *end = nullptr;
            const unsigned long parsed = std::strtoul(line + 9, &end, 10);
            if (end && *end == '\0' && parsed <= SIZE_MAX) {
                saved_current = static_cast<size_t>(parsed);
                current_valid = true;
            }
            continue;
        }
        if (!line[0] || line[0] == '#') continue;
        const size_t entry = saved_entry++;
        char absolute[kMaxPath];
        if (!playlist_line_to_absolute(line, absolute, sizeof(absolute))) continue;
        size_t track_index = 0;
        {
            Lock lock;
            if (!find_track_by_path_locked(absolute, &track_index)) continue;
        }
        if (restored >= capacity) continue;
        track_indices[restored] = track_index;
        if (current_valid && entry == saved_current) {
            restored_current = restored;
            restored_current_valid = true;
        }
        ++restored;
    }
    std::fclose(file);
    if (!version_valid || restored == 0) return 0;
    if (current_position) {
        *current_position = restored_current_valid ? restored_current :
                            (current_valid ? std::min(saved_current, restored - 1) : 0);
    }
    return restored;
}

bool is_favorite(size_t track_index)
{
    char target[kMaxPath];
    {
        Lock lock;
        if (track_index >= s_track_count || !s_status.mounted) return false;
        Track track{};
        if (!track_at_locked(track_index, &track)) return false;
        copy_text(target, sizeof(target), track.path);
    }
    FILE *file = std::fopen(kFavoritesPath, "r");
    if (!file) return false;
    bool found = false;
    char line[kMaxPath + 8];
    while (std::fgets(line, sizeof(line), file)) {
        line[std::strcspn(line, "\r\n")] = '\0';
        if (!line[0] || line[0] == '#') continue;
        char absolute[kMaxPath];
        if (playlist_line_to_absolute(line, absolute, sizeof(absolute)) &&
            std::strcmp(absolute, target) == 0) {
            found = true;
            break;
        }
    }
    std::fclose(file);
    return found;
}

esp_err_t set_favorite(size_t track_index, bool favorite)
{
    char target[kMaxPath];
    {
        Lock lock;
        if (track_index >= s_track_count || !s_status.mounted || s_shutdown_requested) {
            return ESP_ERR_INVALID_STATE;
        }
        Track track{};
        if (!track_at_locked(track_index, &track)) return ESP_FAIL;
        copy_text(target, sizeof(target), track.path);
    }
    const esp_err_t favorites_result = ensure_favorites_file();
    if (favorites_result != ESP_OK) return favorites_result;
    {
        Lock lock;
        bool registered = false;
        for (size_t i = 0; i < s_playlist_count; ++i) {
            if (std::strcmp(s_playlists[i].path, kFavoritesPath) == 0) {
                registered = true;
                break;
            }
        }
        // A deleted or externally removed Favorites file is recreated above.
        // Refresh the runtime list too, so it immediately reappears in Library.
        if (!registered) reload_playlists_locked();
    }
    FILE *input = std::fopen(kFavoritesPath, "r");
    FILE *output = std::fopen(kFavoritesTempPath, "w");
    if (!input || !output) {
        if (input) std::fclose(input);
        if (output) std::fclose(output);
        std::remove(kFavoritesTempPath);
        return ESP_FAIL;
    }
    std::fputs("#EXTM3U\n", output);
    bool found = false;
    char line[kMaxPath + 8];
    while (std::fgets(line, sizeof(line), input)) {
        line[std::strcspn(line, "\r\n")] = '\0';
        if (!line[0] || line[0] == '#') continue;
        char absolute[kMaxPath];
        const bool target_line = playlist_line_to_absolute(line, absolute, sizeof(absolute)) &&
                                 std::strcmp(absolute, target) == 0;
        if (target_line) {
            if (found) continue;
            found = true;
            if (!favorite) continue;
        }
        std::fprintf(output, "%s\n", line);
    }
    if (favorite && !found) {
        const char *relative = std::strncmp(target, kMount, std::strlen(kMount)) == 0
                                   ? target + std::strlen(kMount) : target;
        std::fprintf(output, "%s\n", relative);
    }
    const bool input_closed = std::fclose(input) == 0;
    const bool output_flushed = std::fflush(output) == 0;
    const bool output_closed = std::fclose(output) == 0;
    if (!input_closed || !output_flushed || !output_closed) {
        std::remove(kFavoritesTempPath);
        return ESP_FAIL;
    }
    std::remove(kFavoritesBackupPath);
    if (std::rename(kFavoritesPath, kFavoritesBackupPath) != 0 ||
        std::rename(kFavoritesTempPath, kFavoritesPath) != 0) {
        std::rename(kFavoritesBackupPath, kFavoritesPath);
        std::remove(kFavoritesTempPath);
        return ESP_FAIL;
    }
    std::remove(kFavoritesBackupPath);

    Lock lock;
    for (size_t i = 0; i < s_playlist_count; ++i) {
        if (std::strcmp(s_playlists[i].path, kFavoritesPath) == 0) {
            s_playlists[i].track_count = count_playlist_entries(kFavoritesPath);
            break;
        }
    }
    ++s_status.catalog_generation;
    return ESP_OK;
}

size_t child_folders(const char *path, size_t offset, char names[][kMaxName],
                     size_t capacity, size_t *total)
{
    if (!path || !names || capacity == 0) return 0;
    DIR *directory = opendir(path);
    if (!directory) return 0;
    size_t matched = 0;
    size_t found = 0;
    while (dirent *entry = readdir(directory)) {
        if (entry->d_name[0] == '.') continue;
        char child[kMaxPath];
        if (!join_path(child, sizeof(child), path, entry->d_name)) continue;
        struct stat info{};
        if (stat(child, &info) == 0 && S_ISDIR(info.st_mode) && std::strcmp(child, kPlaylistDir) != 0) {
            if (matched++ < offset || found >= capacity) continue;
            copy_text(names[found++], kMaxName, entry->d_name);
        }
    }
    closedir(directory);
    if (total) *total = matched;
    return found;
}

size_t folder_tracks(const char *path, size_t offset, size_t *track_indices, size_t capacity, size_t *total)
{
    if (!path || !track_indices || capacity == 0) return 0;
    DIR *directory = opendir(path);
    if (!directory) return 0;
    Lock lock;
    size_t matched = 0;
    size_t found = 0;
    while (dirent *entry = readdir(directory)) {
        if (entry->d_name[0] == '.' || !compatible_audio(entry->d_name)) continue;
        char child[kMaxPath];
        if (!join_path(child, sizeof(child), path, entry->d_name)) continue;
        struct stat info{};
        if (stat(child, &info) != 0 || !S_ISREG(info.st_mode)) continue;
        size_t track_index = 0;
        if (!find_track_by_path_locked(child, &track_index)) continue;
        if (matched++ < offset || found >= capacity) continue;
        track_indices[found++] = track_index;
    }
    closedir(directory);
    if (total) *total = matched;
    return found;
}

} // namespace lyra::media
