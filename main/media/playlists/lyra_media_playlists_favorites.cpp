/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../lyra_media_internal.h"

namespace lyra::media {
using namespace internal;

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
