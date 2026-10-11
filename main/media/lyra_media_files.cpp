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

namespace {
constexpr char kFolderTreeCacheMagic[8] = {'L', 'Y', 'R', 'A', 'A', 'P', 'P', 'S'};
constexpr uint32_t kFolderTreeCacheVersion = 1;
constexpr size_t kFolderTreeCacheMaximumFiles = 20000;

struct FolderTreeCacheHeader {
    char magic[8];
    uint32_t version;
    uint32_t kind;
    uint32_t count;
    uint32_t checksum;
};

struct FolderTreeFileCollection {
    FolderFile *files{};
    size_t count{};
    size_t capacity{};
    uint32_t heap_caps{MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT};
    FileKind kind{FileKind::Text};
    FolderTreeProgressCallback progress{};
    void *progress_context{};
};

bool is_supported_tree_file(const char *name, FileKind kind)
{
    const char *ext = extension_of(name);
    if (kind == FileKind::Image) {
        return !strcasecmp(ext, "png") || !strcasecmp(ext, "bmp") ||
               !strcasecmp(ext, "jpg") || !strcasecmp(ext, "jpeg");
    }
    return kind == FileKind::Text &&
           (!strcasecmp(ext, "txt") || !strcasecmp(ext, "lrc"));
}

const char *folder_tree_cache_path(FileKind kind)
{
    return kind == FileKind::Image ? "/sdcard/.lyra/app-images-v1.bin" :
           kind == FileKind::Text ? "/sdcard/.lyra/app-text-v1.bin" : nullptr;
}

uint32_t folder_tree_checksum_add(uint32_t checksum, const void *data, size_t size)
{
    const auto *bytes = static_cast<const uint8_t *>(data);
    for (size_t index = 0; index < size; ++index) {
        checksum = (checksum ^ bytes[index]) * 16777619u;
    }
    return checksum;
}

FolderFile *allocate_folder_tree_files(size_t count)
{
    if (!count || count > SIZE_MAX / sizeof(FolderFile)) return nullptr;
    FolderFile *files = static_cast<FolderFile *>(heap_caps_calloc(
        count, sizeof(FolderFile), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!files) {
        files = static_cast<FolderFile *>(heap_caps_calloc(
            count, sizeof(FolderFile), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    return files;
}

bool load_folder_tree_cache(FileKind kind, FolderFile **files, size_t *count,
                            FolderTreeProgressCallback progress, void *progress_context)
{
    const char *cache_path = folder_tree_cache_path(kind);
    if (!cache_path || !begin_file_access()) return false;
    FILE *file = std::fopen(cache_path, "rb");
    if (!file) {
        end_file_access();
        return false;
    }

    FolderTreeCacheHeader header{};
    bool valid = std::fread(&header, sizeof(header), 1, file) == 1 &&
        std::memcmp(header.magic, kFolderTreeCacheMagic, sizeof(header.magic)) == 0 &&
        header.version == kFolderTreeCacheVersion &&
        header.kind == static_cast<uint32_t>(kind) &&
        header.count <= kFolderTreeCacheMaximumFiles;
    FolderFile *loaded = valid ? allocate_folder_tree_files(header.count) : nullptr;
    if (valid && header.count && !loaded) valid = false;

    uint32_t checksum = 2166136261u;
    for (size_t index = 0; valid && index < header.count; ++index) {
        uint16_t path_length = 0;
        if (std::fread(&path_length, sizeof(path_length), 1, file) != 1 ||
            path_length == 0 || path_length >= sizeof(loaded[index].path) ||
            std::fread(loaded[index].path, 1, path_length, file) != path_length) {
            valid = false;
            break;
        }
        loaded[index].path[path_length] = '\0';
        if (std::strncmp(loaded[index].path, "/sdcard/", 8) != 0 ||
            std::strstr(loaded[index].path, "/../") != nullptr ||
            !is_supported_tree_file(loaded[index].path, kind)) {
            valid = false;
            break;
        }
        loaded[index].kind = kind;
        checksum = folder_tree_checksum_add(checksum, &path_length, sizeof(path_length));
        checksum = folder_tree_checksum_add(checksum, loaded[index].path, path_length);
        if (progress) progress(progress_context, index + 1);
    }
    if (valid && (checksum != header.checksum || std::fgetc(file) != EOF)) valid = false;
    const bool closed = std::fclose(file) == 0;
    end_file_access();
    valid = valid && closed;
    if (!valid) {
        heap_caps_free(loaded);
        return false;
    }
    *files = loaded;
    *count = header.count;
    return true;
}

void save_folder_tree_cache(FileKind kind, const FolderFile *files, size_t count)
{
    const char *cache_path = folder_tree_cache_path(kind);
    if (!cache_path || count > kFolderTreeCacheMaximumFiles || !begin_file_access()) return;
    struct stat directory_info{};
    if (stat("/sdcard/.lyra", &directory_info) != 0 && mkdir("/sdcard/.lyra", 0775) != 0) {
        end_file_access();
        return;
    }

    char temporary[64];
    const int path_size = std::snprintf(temporary, sizeof(temporary), "%s.tmp", cache_path);
    if (path_size <= 0 || static_cast<size_t>(path_size) >= sizeof(temporary)) {
        end_file_access();
        return;
    }
    FILE *file = std::fopen(temporary, "wb");
    if (!file) {
        end_file_access();
        return;
    }

    uint32_t checksum = 2166136261u;
    bool valid = true;
    for (size_t index = 0; index < count; ++index) {
        const size_t path_length = strnlen(files[index].path, sizeof(files[index].path));
        if (!path_length || path_length >= sizeof(files[index].path) ||
            path_length > UINT16_MAX) {
            valid = false;
            break;
        }
        const uint16_t stored_length = static_cast<uint16_t>(path_length);
        checksum = folder_tree_checksum_add(checksum, &stored_length, sizeof(stored_length));
        checksum = folder_tree_checksum_add(checksum, files[index].path, path_length);
    }

    FolderTreeCacheHeader header{};
    std::memcpy(header.magic, kFolderTreeCacheMagic, sizeof(header.magic));
    header.version = kFolderTreeCacheVersion;
    header.kind = static_cast<uint32_t>(kind);
    header.count = static_cast<uint32_t>(count);
    header.checksum = checksum;
    valid = valid && std::fwrite(&header, sizeof(header), 1, file) == 1;
    for (size_t index = 0; valid && index < count; ++index) {
        const uint16_t path_length = static_cast<uint16_t>(
            strnlen(files[index].path, sizeof(files[index].path)));
        valid = std::fwrite(&path_length, sizeof(path_length), 1, file) == 1 &&
                std::fwrite(files[index].path, 1, path_length, file) == path_length;
    }
    const bool closed = std::fclose(file) == 0;
    valid = valid && closed;
    if (valid) {
        std::remove(cache_path);
        valid = std::rename(temporary, cache_path) == 0;
    }
    if (!valid) std::remove(temporary);
    end_file_access();
}

bool append_tree_file(FolderTreeFileCollection *collection, const char *path)
{
    if (collection->count == collection->capacity) {
        const size_t next_capacity = collection->capacity ? collection->capacity * 2 : 32;
        if (next_capacity < collection->capacity ||
            next_capacity > SIZE_MAX / sizeof(FolderFile)) return false;
        const bool first_allocation = collection->files == nullptr;
        FolderFile *grown = static_cast<FolderFile *>(heap_caps_realloc(
            collection->files, next_capacity * sizeof(FolderFile), collection->heap_caps));
        if (!grown && first_allocation &&
            collection->heap_caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) {
            collection->heap_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
            grown = static_cast<FolderFile *>(heap_caps_realloc(
                collection->files, next_capacity * sizeof(FolderFile), collection->heap_caps));
        }
        if (!grown) return false;
        collection->files = grown;
        collection->capacity = next_capacity;
    }
    copy_text(collection->files[collection->count].path,
              sizeof(collection->files[collection->count].path), path);
    collection->files[collection->count++].kind = collection->kind;
    if (collection->progress) collection->progress(collection->progress_context,
                                                   collection->count);
    return true;
}

bool collect_tree_files(const char *path, FolderTreeFileCollection *collection,
                        unsigned depth)
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
        if (!join_path(child, sizeof(child), path, entry->d_name)) continue;
        if (stat(child, &info) != 0) continue;
        if (S_ISDIR(info.st_mode)) {
            ok = collect_tree_files(child, collection, depth + 1);
        } else if (S_ISREG(info.st_mode) &&
                   is_supported_tree_file(entry->d_name, collection->kind)) {
            ok = append_tree_file(collection, child);
        }
    }
    closedir(directory);
    return ok;
}

int compare_tree_file_paths(const void *left, const void *right)
{
    const auto *a = static_cast<const FolderFile *>(left);
    const auto *b = static_cast<const FolderFile *>(right);
    const char *a_name = std::strrchr(a->path, '/');
    const char *b_name = std::strrchr(b->path, '/');
    const int name_order = strcasecmp(a_name ? a_name + 1 : a->path,
                                     b_name ? b_name + 1 : b->path);
    return name_order ? name_order : strcasecmp(a->path, b->path);
}
} // namespace

bool folder_tree_files(const char *path, FileKind kind, FolderFile **files,
                       size_t *count, FolderTreeProgressCallback progress,
                       void *progress_context, bool refresh_cache)
{
    if (files) *files = nullptr;
    if (count) *count = 0;
    if (!files || !count || !path ||
        (kind != FileKind::Image && kind != FileKind::Text) ||
        std::strncmp(path, "/sdcard", 7) != 0 ||
        (path[7] && path[7] != '/')) return false;

    const bool cacheable_root = std::strcmp(path, "/sdcard") == 0;
    if (cacheable_root && !refresh_cache &&
        load_folder_tree_cache(kind, files, count, progress, progress_context)) return true;
    if (progress) progress(progress_context, 0);
    if (!begin_file_access()) return false;

    FolderTreeFileCollection collection{};
    collection.kind = kind;
    collection.progress = progress;
    collection.progress_context = progress_context;
    const bool ok = collect_tree_files(path, &collection, 0);
    end_file_access();
    if (!ok) {
        heap_caps_free(collection.files);
        return false;
    }
    if (collection.count > 1) {
        std::qsort(collection.files, collection.count, sizeof(FolderFile),
                   compare_tree_file_paths);
    }
    *files = collection.files;
    *count = collection.count;
    if (cacheable_root) save_folder_tree_cache(kind, collection.files, collection.count);
    return true;
}

void free_folder_tree_files(FolderFile *files)
{
    heap_caps_free(files);
}
} // namespace lyra::media
