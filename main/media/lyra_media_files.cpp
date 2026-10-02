/* SPDX-License-Identifier: Apache-2.0 */
#include "lyra_media_internal.h"
#include <strings.h>

namespace lyra::media::internal {
namespace {
// Paths, not full Track records, stay resident. IDs occupy a disjoint range
// that still fits the queue UI's 16-bit payload. No catalog file is written.
char **s_direct_paths;
size_t s_direct_count;
size_t s_file_access_count;
uint16_t *s_direct_slots;
constexpr size_t kDirectSlots = 32768;
Track s_direct_cache;
size_t s_direct_cached_id = SIZE_MAX;
}

bool file_access_active_locked() { return s_file_access_count != 0; }

bool resolve_audio_path_locked(const char *path, size_t *index)
{
    if (!path || !index || !s_status.mounted || s_shutdown_requested ||
        std::strncmp(path, "/sdcard/", 8) != 0 ||
        std::strlen(path) >= kMaxPath || !compatible_audio(path)) return false;
    if (find_track_by_path_locked(path, index)) return true;
    struct stat info{};
    if (stat(path, &info) != 0 || !S_ISREG(info.st_mode)) return false;
    if (!s_direct_paths) s_direct_paths = static_cast<char **>(heap_caps_calloc(
        kMaxTracks, sizeof(char *), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!s_direct_slots) s_direct_slots = static_cast<uint16_t *>(heap_caps_calloc(
        kDirectSlots, sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!s_direct_paths || !s_direct_slots) return false;
    size_t slot = hash_path(path) % kDirectSlots;
    while (s_direct_slots[slot]) {
        const size_t candidate = s_direct_slots[slot] - 1;
        if (std::strcmp(s_direct_paths[candidate], path) == 0) {
            *index = kMaxTracks + candidate;
            return true;
        }
        slot = (slot + 1) % kDirectSlots;
    }
    if (s_direct_count == kMaxTracks) return false;
    char *copy = static_cast<char *>(heap_caps_malloc(
        std::strlen(path) + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!copy) return false;
    std::strcpy(copy, path);
    *index = kMaxTracks + s_direct_count;
    s_direct_slots[slot] = s_direct_count + 1;
    s_direct_paths[s_direct_count++] = copy;
    return true;
}

bool transient_track_at_locked(size_t index, Track *out)
{
    if (!out || index < kMaxTracks || index - kMaxTracks >= s_direct_count ||
        !s_status.mounted || s_shutdown_requested) return false;
    const char *path = s_direct_paths[index - kMaxTracks];
    struct stat info{};
    if (stat(path, &info) != 0 || !S_ISREG(info.st_mode)) return false;
    if (s_direct_cached_id != index ||
        s_direct_cache.size_bytes != static_cast<uint64_t>(info.st_size) ||
        s_direct_cache.modified_time != (info.st_mtime > 0 ? static_cast<uint64_t>(info.st_mtime) : 0)) {
        s_direct_cache = {};
        copy_text(s_direct_cache.path, sizeof(s_direct_cache.path), path);
        s_direct_cache.size_bytes = info.st_size;
        s_direct_cache.modified_time = (info.st_mtime > 0 ? static_cast<uint64_t>(info.st_mtime) : 0);
        metadata_from_path(&s_direct_cache);
        read_fast_metadata(&s_direct_cache);
        if (!s_direct_cache.album_artist[0]) copy_text(s_direct_cache.album_artist,
            sizeof(s_direct_cache.album_artist), s_direct_cache.artist);
        s_direct_cached_id = index;
    }
    *out = s_direct_cache;
    return true;
}
} // namespace lyra::media::internal

namespace lyra::media {
using namespace internal;
bool begin_file_access()
{
    Lock lock;
    if (!s_status.mounted || s_shutdown_requested) return false;
    ++s_file_access_count;
    return true;
}
void end_file_access()
{
    Lock lock;
    if (s_file_access_count) --s_file_access_count;
}

bool resolve_audio_path(const char *path, size_t *index)
{
    Lock lock;
    return resolve_audio_path_locked(path, index);
}

namespace {
bool collect_folder_tree(const char *path, size_t *indices, size_t capacity,
                         size_t *count, unsigned depth)
{
    if (depth > 32) return false;
    DIR *directory = opendir(path);
    if (!directory) return false;
    bool ok = true;
    while (ok) {
        errno = 0;
        dirent *entry = readdir(directory);
        if (!entry) { ok = errno == 0; break; }
        if (entry->d_name[0] == '.') continue;
        char child[kMaxPath];
        struct stat info{};
        if (!join_path(child, sizeof(child), path, entry->d_name)) { ok = false; break; }
        if (stat(child, &info) != 0) { ok = false; break; }
        if (S_ISDIR(info.st_mode)) {
            ok = collect_folder_tree(child, indices, capacity, count, depth + 1);
        } else if (S_ISREG(info.st_mode) && compatible_audio(child)) {
            if (*count == capacity || !resolve_audio_path(child, &indices[*count])) ok = false;
            else ++*count;
        }
    }
    closedir(directory);
    return ok;
}
} // namespace

bool folder_tree_tracks(const char *path, size_t *indices, size_t capacity, size_t *count)
{
    if (!count) return false;
    *count = 0;
    if (!path || !indices || !capacity || !begin_file_access()) return false;
    const bool ok = collect_folder_tree(path, indices, capacity, count, 0);
    end_file_access();
    return ok && *count > 0;
}

namespace {
// Normalise a local playlist entry without allowing .. to escape the card.
bool playlist_entry_path(const char *playlist, const char *entry, char *out)
{
    if (!entry[0] || std::strchr(entry, ':')) return false;
    char joined[kMaxPath * 2];
    int length;
    if (!std::strncmp(entry, "/sdcard/", 8)) length = std::snprintf(joined, sizeof(joined), "%s", entry);
    else if (entry[0] == '/') length = std::snprintf(joined, sizeof(joined), "/sdcard%s", entry);
    else {
        const char *slash = std::strrchr(playlist, '/');
        if (!slash) return false;
        length = std::snprintf(joined, sizeof(joined), "%.*s/%s", int(slash - playlist), playlist, entry);
    }
    if (length < 0 || size_t(length) >= sizeof(joined)) return false;
    std::strcpy(out, "/sdcard");
    size_t used = 7;
    char *part = joined + 8;
    while (*part) {
        while (*part == '/') ++part;
        if (!*part) break;
        char *end = std::strchr(part, '/');
        if (end) *end = '\0';
        if (!std::strcmp(part, "..")) {
            if (used == 7) return false;
            while (used > 7 && out[used - 1] != '/') --used;
            --used;
            out[used] = '\0';
        } else if (std::strcmp(part, ".")) {
            const size_t count = std::strlen(part);
            if (used + count + 2 > kMaxPath) return false;
            out[used++] = '/';
            std::memcpy(out + used, part, count + 1);
            used += count;
        }
        if (!end) break;
        part = end + 1;
    }
    return used > 7;
}
} // namespace

esp_err_t load_playlist_file(const char *path, size_t *indices, size_t capacity, size_t *count)
{
    if (!count) return ESP_ERR_INVALID_ARG;
    *count = 0;
    if (!path || !indices || !capacity || std::strncmp(path, "/sdcard/", 8) ||
        std::strlen(path) >= kMaxPath) return ESP_ERR_INVALID_ARG;
    if (!begin_file_access()) return ESP_ERR_INVALID_STATE;
    FILE *file = lyra::sd::open(path, "rb", lyra::sd::Client::Filesystem);
    if (!file) { end_file_access(); return ESP_ERR_NOT_FOUND; }
    char line[kMaxPath + 4], chunk[1024];
    size_t used = 0;
    bool overlong = false;
    esp_err_t result = ESP_OK;
    const auto consume_line = [&]() {
        if (overlong) return;
        line[used] = '\0';
        char *entry = line;
        if (used >= 3 && uint8_t(entry[0]) == 0xEF && uint8_t(entry[1]) == 0xBB && uint8_t(entry[2]) == 0xBF) entry += 3;
        while (*entry && std::isspace(static_cast<unsigned char>(*entry))) ++entry;
        size_t length = std::strlen(entry);
        while (length && std::isspace(static_cast<unsigned char>(entry[length - 1]))) entry[--length] = '\0';
        if (!length || entry[0] == '#') return;
        for (char *p = entry; *p; ++p) if (*p == '\\') *p = '/';
        char absolute[kMaxPath];
        size_t index;
        if (!playlist_entry_path(path, entry, absolute)) return;
        bool found = resolve_audio_path(absolute, &index);
        // Lyra's generated portable playlists use card-root relative paths.
        // Prefer standard playlist-directory relative paths, then try the root.
        if (!found && entry[0] != '/' && std::strncmp(entry, "../", 3)) {
            if (playlist_entry_path("/sdcard/root.m3u", entry, absolute))
                found = resolve_audio_path(absolute, &index);
        }
        if (!found) return;
        if (*count == capacity) { result = ESP_ERR_INVALID_SIZE; return; }
        indices[(*count)++] = index;
    };
    while (result == ESP_OK) {
        const size_t bytes = lyra::sd::read(file, chunk, sizeof(chunk), lyra::sd::Client::Filesystem);
        if (!bytes) {
            if (std::ferror(file)) result = ESP_FAIL;
            else if (used || overlong) consume_line();
            break;
        }
        for (size_t i = 0; i < bytes && result == ESP_OK; ++i) {
            if (chunk[i] == '\n') { consume_line(); used = 0; overlong = false; }
            else if (chunk[i] == '\0' || used + 1 >= sizeof(line)) overlong = true;
            else if (!overlong) line[used++] = chunk[i];
        }
    }
    lyra::sd::close(file, lyra::sd::Client::Filesystem);
    end_file_access();
    if (result != ESP_OK) *count = 0;
    return result;
}

size_t folder_files(const char *path, size_t offset, FolderFile *files,
                    size_t capacity, size_t *total)
{
    if (total) *total = 0;
    if (!path || !files || !capacity || !status().mounted) return 0;
    DIR *directory = opendir(path);
    if (!directory) return 0;
    size_t matched = 0, found = 0;
    while (dirent *entry = readdir(directory)) {
        if (entry->d_name[0] == '.') continue;
        FileKind kind;
        const char *ext = extension_of(entry->d_name);
        if (compatible_audio(entry->d_name)) kind = FileKind::Audio;
        else if (!strcasecmp(ext, "png") || !strcasecmp(ext, "bmp") ||
                 !strcasecmp(ext, "jpg") || !strcasecmp(ext, "jpeg")) kind = FileKind::Image;
        else if (!strcasecmp(ext, "txt") || !strcasecmp(ext, "lrc")) kind = FileKind::Text;
        else if (!strcasecmp(ext, "m3u") || !strcasecmp(ext, "m3u8")) kind = FileKind::Playlist;
        else continue;
        char child[kMaxPath];
        struct stat info{};
        if (!join_path(child, sizeof(child), path, entry->d_name) ||
            stat(child, &info) != 0 || !S_ISREG(info.st_mode)) continue;
        if (matched++ < offset || found >= capacity) continue;
        copy_text(files[found].path, sizeof(files[found].path), child);
        files[found++].kind = kind;
    }
    closedir(directory);
    if (total) *total = matched;
    return found;
}
} // namespace lyra::media
