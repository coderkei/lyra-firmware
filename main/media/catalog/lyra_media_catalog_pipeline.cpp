/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../lyra_media_internal.h"

#include <ctime>

namespace lyra::media::internal {

uint64_t recorded_modified_time(const struct stat &info)
{
    if (info.st_mtime > 0) return static_cast<uint64_t>(info.st_mtime);
    // Some cards/filesystems can expose a zero timestamp. The clock is
    // initialized before media startup, so retain a deterministic date for
    // that file instead of losing the timestamp from the catalog entirely.
    const std::time_t current = std::time(nullptr);
    return current > 0 ? static_cast<uint64_t>(current) : 0;
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

// Artwork extraction, decoding, and the SD-backed artwork cache live in lyra_media_artwork.cpp.
void process_artwork_request(const ArtworkRequest &request)
{
    const uint32_t key = artwork::key(request.track);
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
    if (!sd_cache_enabled || !artwork::read_cache(key, artwork_size, &pixels)) {
        bool used_sd_backing = false;
        if (!artwork::extract_and_decode(request.track, &pixels, artwork_size,
                                         &s_artwork_diagnostics, sd_cache_enabled,
                                         &used_sd_backing)) {
            Lock lock;
            s_artwork_failed_key = key;
            s_artwork_failed = true;
            return;
        }
        if (used_sd_backing && !artwork::write_cache(key, artwork_size, pixels)) {
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
                s_artwork_active_hash = artwork::request_key(request.track, request.size);
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

enum class ScanRecordState : uint8_t {
    Missing,
    Changed,
    Unchanged,
};

ScanRecordState read_cached_track_if_unchanged_locked(const char *path,
                                                       const struct stat &info,
                                                       Track *track)
{
    if (!path || !track || !s_catalog || !s_path_index || !s_title_order) {
        return ScanRecordState::Missing;
    }

    const uint64_t wanted = hash_path(path);
    const PathIndex probe{wanted, 0, 0};
    const PathIndex *candidate = std::lower_bound(
        s_path_index, s_path_index + s_track_count, probe,
        [](const PathIndex &left, const PathIndex &right) {
            return left.hash < right.hash;
        });
    for (; candidate != s_path_index + s_track_count && candidate->hash == wanted;
         ++candidate) {
        const uint32_t logical = candidate->track_index;
        if (logical >= s_track_count) continue;
        const uint32_t physical = s_title_order[logical];
        Track cached{};
        if (!read_physical(s_catalog, s_catalog_header, physical, &cached) ||
            std::strcmp(cached.path, path) != 0) {
            continue;
        }
        // FAT cards can expose a zero timestamp. In that case size is the
        // only stable filesystem signal available, so do not force a metadata
        // re-read on every scan merely because the display-time fallback
        // changes.
        const bool timestamp_changed = info.st_mtime > 0 &&
                                        cached.modified_time !=
                                            static_cast<uint64_t>(info.st_mtime);
        if (cached.size_bytes != static_cast<uint64_t>(info.st_size) ||
            timestamp_changed) {
            return ScanRecordState::Changed;
        }
        // Duration probing is lazy and may only exist in the resident cache.
        // Carry a known value into the replacement catalog so an incremental
        // scan does not make unchanged tracks pay that cost again.
        apply_cached_duration_locked(physical, &cached);
        *track = cached;
        return ScanRecordState::Unchanged;
    }
    return ScanRecordState::Missing;
}

bool scan_directory(const char *path, FILE *catalog, size_t *count,
                    bool *capacity_reached, TickType_t *last_progress,
                    size_t *reused_count, size_t *rescanned_count,
                    size_t *matched_existing_count, size_t depth)
{
    if (depth > 12) return true;
    DIR *directory = opendir(path);
    if (!directory) {
        if (errno != ENOENT) ESP_LOGW(kTag, "cannot open %s: %s", path, std::strerror(errno));
        return errno == ENOENT;
    }
    bool success = true;
    while (true) {
        errno = 0;
        dirent *entry = readdir(directory);
        if (!entry) {
            if (errno != 0) {
                ESP_LOGW(kTag, "cannot read %s: %s", path, std::strerror(errno));
                success = false;
            }
            break;
        }
        if (*capacity_reached || std::ferror(catalog)) break;
        if (entry->d_name[0] == '.') continue;
        char child[kMaxPath];
        if (!join_path(child, sizeof(child), path, entry->d_name)) continue;
        struct stat info{};
        if (stat(child, &info) != 0) {
            if (errno != ENOENT) {
                ESP_LOGW(kTag, "cannot stat %s: %s", child, std::strerror(errno));
                success = false;
            }
            continue;
        }
        if (S_ISDIR(info.st_mode)) {
            if (std::strcmp(child, kPlaylistDir) != 0 && std::strcmp(child, kDataDir) != 0) {
                if (!scan_directory(child, catalog, count, capacity_reached, last_progress,
                                    reused_count, rescanned_count, matched_existing_count,
                                    depth + 1)) {
                    success = false;
                }
            }
        } else if (S_ISREG(info.st_mode) && compatible_audio(child)) {
            if (*count >= kMaxTracks) {
                *capacity_reached = true;
                break;
            }
            ScanRecordState state = ScanRecordState::Missing;
            Track track{};
            {
                Lock lock;
                state = read_cached_track_if_unchanged_locked(child, info, &track);
            }
            if (state == ScanRecordState::Missing || state == ScanRecordState::Changed) {
                copy_text(track.path, sizeof(track.path), child);
                track.size_bytes = static_cast<uint64_t>(info.st_size);
                track.modified_time = recorded_modified_time(info);
                metadata_from_path(&track);
                // Read only compact text-tag blocks. Embedded pictures remain
                // deferred to the low-priority artwork worker so art-heavy cards
                // do not make the initial scan decode or copy megabytes per song.
                read_fast_metadata(&track);
                if (!track.album_artist[0]) {
                    copy_text(track.album_artist, sizeof(track.album_artist), track.artist);
                }
                if (rescanned_count) ++(*rescanned_count);
            } else if (reused_count) {
                ++(*reused_count);
            }
            if (state != ScanRecordState::Missing && matched_existing_count) {
                ++(*matched_existing_count);
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
    return success && !std::ferror(catalog);
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
        track->modified_time = recorded_modified_time(current);
    }
}

bool read_logical_track_locked(uint32_t logical_index, Track *out, bool refresh_file)
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

bool validate_catalog(FILE *file, CatalogHeader *header, bool verify_checksum)
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

bool load_catalog_file(const char *path, bool verify_checksum)
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
    auto *album_groups = static_cast<GroupRecord *>(heap_caps_malloc(
        header.album_group_count * sizeof(GroupRecord), MALLOC_CAP_SPIRAM));
    const bool allocated = header.track_count == 0 ||
                           (order && inverse && paths && duration_cache && duration_ready &&
                            album_for_logical &&
                            (!header.album_group_count || album_groups));
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
            const uint64_t album_groups_end = static_cast<uint64_t>(
                header.album_group_offset) +
                static_cast<uint64_t>(header.album_group_count) * sizeof(GroupRecord);
            const uint64_t album_members_start = album_groups_end;
            if (header.album_group_count &&
                (album_groups_end > header.file_size ||
                 std::fseek(file, header.album_group_offset, SEEK_SET) != 0 ||
                 std::fread(album_groups, sizeof(GroupRecord),
                            header.album_group_count, file) != header.album_group_count ||
                 std::fseek(file, static_cast<long>(album_members_start), SEEK_SET) != 0)) {
                read = false;
            }
            uint64_t member_cursor = album_members_start;
            for (uint32_t album_index = 0;
                 read && album_index < header.album_group_count; ++album_index) {
                const GroupRecord &album = album_groups[album_index];
                const uint64_t album_members_end = member_cursor +
                    static_cast<uint64_t>(album.track_count) * sizeof(uint32_t);
                if (album.members_offset != member_cursor ||
                    album_members_end < member_cursor ||
                    album_members_end > header.file_size) {
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
                member_cursor = album_members_end;
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
        heap_caps_free(album_groups);
        std::fclose(file);
        return false;
    }
    heap_caps_free(album_groups);
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
    size_t reused_count = 0;
    size_t rescanned_count = 0;
    size_t matched_existing_count = 0;
    size_t previous_track_count = 0;
    {
        Lock lock;
        previous_track_count = s_track_count;
    }
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
            if (!scan_directory(kMount, file, &track_count, &capacity_reached,
                                &last_progress, &reused_count, &rescanned_count,
                                &matched_existing_count)) {
                result = ESP_FAIL;
            }
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
        // Do not synchronously warm non-default sort caches here. Their
        // builders hold the media mutex while reading and sorting the full
        // catalog, which would block LVGL's status poll and make the scan
        // spinner appear frozen. The existing background sort indexer builds
        // each cache on demand with visible progress when that view is opened.
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
        const size_t removed_count = previous_track_count > matched_existing_count ?
                                     previous_track_count - matched_existing_count : 0;
        ESP_LOGI(kTag, "scan complete: %u tracks%s, %u reused, %u rescanned, %u removed, %u playlists; discovery=%u ms total=%u ms; stack margin %u bytes",
                 static_cast<unsigned>(track_count), capacity_reached ? " (10,000 limit reached)" : "",
                 static_cast<unsigned>(reused_count),
                 static_cast<unsigned>(rescanned_count),
                 static_cast<unsigned>(removed_count),
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


} // namespace lyra::media::internal
