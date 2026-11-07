/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "media/lyra_media_internal.h"

namespace lyra::media {
using namespace internal;

esp_err_t init()
{
    if (!lyra::sd::init()) return ESP_ERR_NO_MEM;
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;
    artwork::init();
    init_thumbnails();
    static StaticSemaphore_t stats_file_mutex_storage;
    if (!s_playback_stats_file_mutex) {
        s_playback_stats_file_mutex = xSemaphoreCreateMutexStatic(&stats_file_mutex_storage);
    }
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
        load_playback_stats();
        start_duration_indexing_if_needed();
        ESP_LOGI(kTag, "MicroSD mounted; library scan is user initiated");
    } else {
        ESP_LOGW(kTag, "MicroSD mount failed: %s", esp_err_to_name(result));
    }
    return result;
}

Status status() { Lock lock; return s_status; }

bool card_info(CardInfo *out)
{
    if (!out) return false;
    Lock lock;
    *out = {};
    if (!s_status.mounted || !s_card) return false;
    out->mounted = true;
    out->capacity_bytes = static_cast<uint64_t>(s_card->csd.capacity) *
                          static_cast<uint64_t>(s_card->csd.sector_size);
    out->manufacturer_id = s_card->cid.mfg_id;
    out->oem_id = s_card->cid.oem_id;
    std::memcpy(out->name, s_card->cid.name, sizeof(out->name));
    out->revision = s_card->cid.revision;
    out->serial = s_card->cid.serial;
    out->date = s_card->cid.date;
    return true;
}

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
    // Serialize reset with publication of the background stats snapshot.
    Lock stats_file_lock(s_playback_stats_file_mutex);
    Lock lock;
    if (!s_status.mounted || s_status.scanning || s_status.duration_indexing ||
        s_status.sorting_indexing ||
        s_status.artwork_busy || s_search_status.running) return ESP_ERR_INVALID_STATE;
    const esp_err_t playlists = clear_playlists_locked();
    const esp_err_t playback_stats = clear_playback_stats_locked();
    const esp_err_t artwork = clear_artwork_cache_locked();
    const esp_err_t catalog = clear_catalog_locked();
    refresh_capacity();
    return playlists == ESP_OK && playback_stats == ESP_OK &&
                   artwork == ESP_OK && catalog == ESP_OK ? ESP_OK : ESP_FAIL;
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
    if (size != kSmallArtworkSize && size != kLargeArtworkSize) return ESP_ERR_INVALID_ARG;
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
    // Media workers never write flash, so a PSRAM stack is safe and keeps
    // search working when the internal heap is fragmented.
    if (xTaskCreatePinnedToCoreWithCaps(search_task, "lyra_search", 8192, nullptr, 1, nullptr, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
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
    // The 24 KiB scan stack never fits a fragmented internal heap. The scan
    // only touches the MicroSD card, never flash, so its stack lives in PSRAM.
    if (xTaskCreatePinnedToCoreWithCaps(scan_task, "lyra_sd_scan", kScanStack, nullptr, 2,
                                        nullptr, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        Lock lock;
        s_status.scanning = false;
        s_status.last_error = ESP_ERR_NO_MEM;
        ESP_LOGE(kTag, "could not start the library scan task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t shutdown(uint32_t timeout_ms)
{
    ESP_LOGI(kTag, "safe shutdown requested; stopping new MicroSD work");
    {
        Lock lock;
        s_shutdown_requested = true;
        s_artwork_queue_count = 0;
        s_search_cancel_requested = true;
        s_sort_cancel_requested = true;
        const esp_err_t stats_result = start_playback_stats_save_locked();
        if (stats_result != ESP_OK) {
            ESP_LOGW(kTag, "could not start playback statistics save: %s",
                     esp_err_to_name(stats_result));
        }
    }

    const TickType_t started = xTaskGetTickCount();
    while (true) {
        bool storage_busy;
        {
            Lock lock;
            storage_busy = s_status.scanning || s_status.duration_indexing ||
                           s_status.sorting_indexing ||
                           s_status.artwork_busy || s_search_status.running ||
                           file_access_active_locked() || playback_stats_save_active_locked();
        }
        if (!storage_busy) break;
        if ((xTaskGetTickCount() - started) * portTICK_PERIOD_MS >= timeout_ms) {
            ESP_LOGE(kTag, "timed out waiting for MicroSD background work before shutdown");
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    ESP_LOGI(kTag, "MicroSD background work stopped; releasing catalog state");
    Lock lock;
    heap_caps_free(s_artwork_pixels);
    s_artwork_pixels = nullptr;
    s_artwork_valid = false;
    s_artwork_key = 0;
    if (!s_status.mounted || !s_card) return ESP_OK;
    clear_runtime_catalog();
    ESP_LOGI(kTag, "unmounting MicroSD filesystem");
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
    if (index >= kMaxTracks) return transient_track_at_locked(index, out);
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

bool track_summary_at(size_t index, TrackSummary *out)
{
    if (!out) return false;
    Lock lock;
    if (index < kMaxTracks) {
        if (!s_catalog || index >= s_track_count) return false;
        const uint32_t physical = s_title_order[index];
        if (copy_resident_track_summary(physical, out)) return true;
        for (size_t i = 0; i < kTrackCacheSize; ++i) {
            const CachedTrack &entry = s_track_cache[i];
            if (entry.generation == s_status.catalog_generation && entry.physical_index == physical) {
                copy_text(out->title, sizeof(out->title), entry.track.title);
                copy_text(out->artist, sizeof(out->artist), entry.track.artist);
                return true;
            }
        }
    }
    // Guarded by the media lock; kept in PSRAM so list rows cost no internal RAM.
    static Track *scratch_buffer = static_cast<Track *>(
        heap_caps_malloc(sizeof(Track), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!scratch_buffer) return false;
    Track &scratch = *scratch_buffer;
    if (index >= kMaxTracks) {
        if (!transient_track_at_locked(index, &scratch)) return false;
    } else {
        if (!read_physical(s_catalog, s_catalog_header, s_title_order[index], &scratch)) return false;
    }
    copy_text(out->title, sizeof(out->title), scratch.title);
    copy_text(out->artist, sizeof(out->artist), scratch.artist);
    return true;
}

bool sorted_track_at(size_t index, size_t *track_index)
{
    return sorted_tracks(index, track_index, 1) == 1;
}

size_t sorted_tracks(size_t offset, size_t *track_indices, size_t capacity)
{
    if (!track_indices || !capacity) return 0;
    Lock lock;
    if (offset >= s_track_count) return 0;
    const size_t count = std::min(capacity, s_track_count - offset);
    const SortSetting setting = current_sort_setting(SortSection::Songs);
    const bool title_order = setting.field == SortField::Title && setting.direction == SortDirection::Ascending;
    const uint32_t *order = nullptr;
    if (!title_order) {
        if (prepare_song_sort_order_locked()) order = s_song_sort_order;
        else start_sort_cache_indexing_locked(SortSection::Songs);
    }
    // The logical indices already are in title order, including while a
    // different order is being prepared by the background indexer.
    for (size_t i = 0; i < count; ++i) track_indices[i] = order ? order[offset + i] : offset + i;
    return count;
}

bool copy_artwork(const Track &track, uint16_t *pixels, size_t pixel_count)
{
    if (!pixels || !track.path[0]) return false;
    Lock lock;
    const size_t required = static_cast<size_t>(s_status.artwork_size) * s_status.artwork_size;
    if (pixel_count < required || !s_artwork_valid || !s_artwork_pixels ||
        s_artwork_pixel_size != s_status.artwork_size ||
        s_artwork_key != artwork::key(track)) return false;
    std::memcpy(pixels, s_artwork_pixels, required * sizeof(uint16_t));
    return true;
}

esp_err_t request_artwork(const Track &track, ArtworkSize size)
{
    (void)size;
    if (!track.path[0]) return ESP_ERR_INVALID_ARG;
    const uint32_t key = artwork::key(track);

    bool start_worker = false;
    {
        Lock lock;
        if (!s_status.mounted || s_status.scanning || s_shutdown_requested) return ESP_ERR_INVALID_STATE;
        if (s_artwork_valid && s_artwork_key == key) return ESP_OK;
        if (s_artwork_failed && s_artwork_failed_key == key) return ESP_ERR_NOT_FOUND;
        const uint64_t wanted_hash = artwork::request_key(track, size);
        if (s_artwork_task_running && s_artwork_active_hash == wanted_hash) return ESP_OK;
        for (size_t i = 0; i < s_artwork_queue_count; ++i) {
            const ArtworkRequest &queued = s_artwork_queue[(s_artwork_queue_head + i) % kArtworkQueueSize];
            if (artwork::key(queued.track) == artwork::key(track)) return ESP_OK;
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
    // Cover decoding overlaps LCD refresh and uses PSRAM for its large
    // buffers. Keep its 24 KB stack there too, leaving internal DMA memory
    // available to the LCD and I2S paths.
    if (start_worker && xTaskCreatePinnedToCoreWithCaps(
            artwork_task, "lyra_artwork", kScanStack, nullptr, 1, nullptr,
            kArtworkCore, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
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

static bool group_record_locked(GroupKind kind, size_t index, GroupRecord *out)
{
    uint32_t offset = 0;
    uint32_t count = 0;
    group_location(kind, &offset, &count);
    if (!s_catalog || index >= count) return false;
    if (const GroupRecord *record = resident_group_record(kind, index)) {
        *out = *record;
        return true;
    }
    return std::fseek(s_catalog, offset + index * sizeof(GroupRecord), SEEK_SET) == 0 &&
           std::fread(out, sizeof(*out), 1, s_catalog) == 1;
}

bool group_at(GroupKind kind, size_t index, Group *out)
{
    if (!out) return false;
    Lock lock;
    GroupRecord record;
    if (!group_record_locked(kind, index, &record)) return false;
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
    GroupRecord group;
    if (!group_record_locked(kind, group_index, &group) || offset >= group.track_count) return 0;
    const size_t wanted = std::min(capacity, static_cast<size_t>(group.track_count) - offset);
    if (kind == GroupKind::Album || kind == GroupKind::Artist) {
        const uint32_t *order = group_track_order_locked(kind, group);
        if (!order) return 0;
        std::copy_n(order + offset, wanted, track_indices);
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
    GroupRecord group;
    if (!group_record_locked(kind, group_index, &group) || group.track_count == 0) return false;

    if (kind == GroupKind::Album || kind == GroupKind::Artist) {
        const uint32_t *order = group_track_order_locked(kind, group);
        if (!order) return false;
        const uint32_t *end = order + group.track_count;
        const uint32_t *current = std::find(order, end, current_track);
        if (current == end) return false;
        const int64_t next = (current - order) + direction;
        const size_t target = next < 0 ? group.track_count - 1 :
                              next >= static_cast<int64_t>(group.track_count) ? 0 :
                              static_cast<size_t>(next);
        *track_index = order[target];
        return true;
    }

    if (std::fseek(s_catalog, group.members_offset, SEEK_SET) != 0) return false;
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

} // namespace lyra::media
