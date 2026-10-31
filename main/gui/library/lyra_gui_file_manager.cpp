/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../lyra_gui_internal.h"

#include <atomic>
#include <cerrno>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

namespace lyra::gui::internal {
namespace {

using lyra::media::FileKind;
using lyra::media::kMaxPath;
using lyra::i18n::StringId;

enum class EntryKind : uint8_t { Folder, Audio, Image, Text, Playlist, Other };
enum class SortMode : uint8_t { NameAscending, NameDescending, Type, Count };
enum class EntryAction : uint8_t { Open, Play, AddToQueue, Rename, Move, Copy, Delete, Details };
enum class NameInputMode : uint8_t { NewFolder, Rename };
enum class JobKind : uint8_t { Delete, Copy };

constexpr size_t kMaxEntries = 10000;
constexpr size_t kReadBatch = 24;
constexpr size_t kRowBatch = 12;
constexpr int kRowPitch = 58;
constexpr int kToolbarHeight = 40;
constexpr unsigned kMaxTreeDepth = 16;
constexpr const char *kRoot = "/sdcard";
// Card-relative paths are data, shown as-is in every language.
constexpr const char *kRootSeparator = "/";

// Each listing keeps one compact entry per child and a single name pool in
// PSRAM, read once with readdir(). FAT has no per-entry size in readdir, and
// stat() rescans the directory, so sizes are read only for visible rows.
struct Entry {
    uint32_t name_offset;
    EntryKind kind;
};

struct Listing {
    char path[kMaxPath];
    Entry *entries;
    char *names;
    size_t count;
    size_t capacity;
    size_t names_used;
    size_t names_capacity;
    SortMode sorted;
    bool valid;
    bool failed;
    bool truncated;
};

struct Clipboard {
    bool active;
    bool copy;
    char path[kMaxPath];
};

struct Job {
    std::atomic<bool> running{false};
    std::atomic<bool> done{false};
    std::atomic<uint32_t> items{0};
    std::atomic<uint32_t> kilobytes{0};
    bool succeeded{};
    JobKind kind{};
    char source[kMaxPath]{};
    char target[kMaxPath]{};
};

struct ListContext {
    size_t loaded;
    int top;
};

struct ActionTarget {
    char path[kMaxPath];
    EntryKind kind;
    size_t index;
};

Listing s_listing{};
SortMode s_sort_mode = SortMode::NameAscending;
Clipboard s_clipboard{};
Job s_job;
lv_timer_t *s_job_timer;
lv_obj_t *s_job_overlay;
lv_obj_t *s_job_label;
ActionTarget s_action{};
NameInputMode s_name_mode = NameInputMode::NewFolder;
char s_name_input[lyra::media::kMaxName];
char s_name_source[kMaxPath];

// The SD gate is held for one bounded VFS step so queued audio reads win.
struct SdGate {
    bool held;
    SdGate() : held(lyra::sd::acquire(lyra::sd::Client::Filesystem)) {}
    ~SdGate() { if (held) lyra::sd::release(lyra::sd::Client::Filesystem); }
    SdGate(const SdGate &) = delete;
    SdGate &operator=(const SdGate &) = delete;
};

bool join_path(char *output, size_t capacity, const char *directory, const char *name)
{
    const int length = std::snprintf(output, capacity, "%s/%s", directory, name);
    return length > 0 && static_cast<size_t>(length) < capacity;
}

const char *base_name(const char *path)
{
    const char *slash = std::strrchr(path, '/');
    return slash ? slash + 1 : path;
}

void parent_path(const char *path, char *output, size_t capacity)
{
    copy_ui_text(output, capacity, path);
    char *slash = std::strrchr(output, '/');
    if (slash) *slash = '\0';
    if (std::strlen(output) < std::strlen(kRoot)) copy_ui_text(output, capacity, kRoot);
}

bool is_root(const char *path)
{
    return std::strcmp(path, kRoot) == 0;
}

// True when path is the playing file or one of its parent folders.
bool path_in_use(const char *path)
{
    const lyra::audio::Status audio = lyra::audio::status();
    const size_t length = std::strlen(path);
    return audio.path[0] && std::strncmp(audio.path, path, length) == 0 &&
           (audio.path[length] == '\0' || audio.path[length] == '/');
}

bool path_exists(const char *path, struct stat *info = nullptr)
{
    struct stat local{};
    SdGate gate;
    return ::stat(path, info ? info : &local) == 0;
}

EntryKind classify(const char *name, bool directory)
{
    if (directory) return EntryKind::Folder;
    FileKind kind;
    if (!lyra::media::classify_file(name, &kind)) return EntryKind::Other;
    switch (kind) {
        case FileKind::Audio: return EntryKind::Audio;
        case FileKind::Image: return EntryKind::Image;
        case FileKind::Text: return EntryKind::Text;
        case FileKind::Playlist: return EntryKind::Playlist;
    }
    return EntryKind::Other;
}

const char *entry_name(size_t index)
{
    return s_listing.names + s_listing.entries[index].name_offset;
}

bool entry_path(size_t index, char *output, size_t capacity)
{
    return index < s_listing.count &&
           join_path(output, capacity, s_listing.path, entry_name(index));
}

bool append_entry(const char *name, EntryKind kind)
{
    if (s_listing.count == kMaxEntries) {
        s_listing.truncated = true;
        return false;
    }
    if (s_listing.count == s_listing.capacity) {
        const size_t capacity = s_listing.capacity ? s_listing.capacity * 2 : 64;
        auto *entries = static_cast<Entry *>(heap_caps_realloc(s_listing.entries,
            capacity * sizeof(Entry), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!entries) {
            s_listing.truncated = true;
            return false;
        }
        s_listing.entries = entries;
        s_listing.capacity = capacity;
    }
    const size_t length = std::strlen(name) + 1;
    if (s_listing.names_used + length > s_listing.names_capacity) {
        size_t capacity = s_listing.names_capacity ? s_listing.names_capacity * 2 : 4096;
        while (capacity < s_listing.names_used + length) capacity *= 2;
        auto *names = static_cast<char *>(heap_caps_realloc(s_listing.names, capacity,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!names) {
            s_listing.truncated = true;
            return false;
        }
        s_listing.names = names;
        s_listing.names_capacity = capacity;
    }
    std::memcpy(s_listing.names + s_listing.names_used, name, length);
    s_listing.entries[s_listing.count++] = {static_cast<uint32_t>(s_listing.names_used), kind};
    s_listing.names_used += length;
    return true;
}

void sort_listing()
{
    const char *names = s_listing.names;
    const SortMode mode = s_sort_mode;
    std::sort(s_listing.entries, s_listing.entries + s_listing.count,
              [names, mode](const Entry &left, const Entry &right) {
        const bool left_folder = left.kind == EntryKind::Folder;
        const bool right_folder = right.kind == EntryKind::Folder;
        if (left_folder != right_folder) return left_folder;
        if (mode == SortMode::Type && left.kind != right.kind) return left.kind < right.kind;
        const int order = ::strcasecmp(names + left.name_offset, names + right.name_offset);
        return mode == SortMode::NameDescending ? order > 0 : order < 0;
    });
    s_listing.sorted = mode;
}

void load_listing(const char *path)
{
    if (s_listing.valid && std::strcmp(s_listing.path, path) == 0) {
        if (s_listing.sorted != s_sort_mode) sort_listing();
        return;
    }
    copy_ui_text(s_listing.path, sizeof(s_listing.path), path);
    s_listing.count = 0;
    s_listing.names_used = 0;
    s_listing.failed = false;
    s_listing.truncated = false;
    s_listing.valid = true;
    if (!lyra::media::begin_file_access()) {
        s_listing.failed = true;
        return;
    }
    DIR *directory;
    {
        SdGate gate;
        directory = opendir(path);
    }
    if (!directory) {
        lyra::media::end_file_access();
        s_listing.failed = true;
        return;
    }
    bool more = true;
    while (more) {
        SdGate gate;
        for (size_t read = 0; read < kReadBatch; ++read) {
            errno = 0;
            const dirent *entry = readdir(directory);
            if (!entry) {
                more = false;
                s_listing.failed = errno != 0;
                break;
            }
            if (entry->d_name[0] == '.') continue;
            bool folder = entry->d_type == DT_DIR;
            if (entry->d_type == DT_UNKNOWN) {
                char child[kMaxPath];
                struct stat info{};
                folder = join_path(child, sizeof(child), path, entry->d_name) &&
                         ::stat(child, &info) == 0 && S_ISDIR(info.st_mode);
            }
            if (!append_entry(entry->d_name, classify(entry->d_name, folder))) {
                more = false;
                break;
            }
        }
    }
    {
        SdGate gate;
        closedir(directory);
    }
    lyra::media::end_file_access();
    sort_listing();
}

const char *kind_icon(EntryKind kind)
{
    switch (kind) {
        case EntryKind::Folder: return LV_SYMBOL_DIRECTORY;
        case EntryKind::Audio: return LV_SYMBOL_AUDIO;
        case EntryKind::Image: return LV_SYMBOL_IMAGE;
        case EntryKind::Playlist: return LV_SYMBOL_LIST;
        case EntryKind::Text:
        case EntryKind::Other: break;
    }
    return LV_SYMBOL_FILE;
}

const char *kind_name(EntryKind kind)
{
    switch (kind) {
        case EntryKind::Folder: return tr(StringId::Folder);
        case EntryKind::Audio: return tr(StringId::FileTypeAudio);
        case EntryKind::Image: return tr(StringId::FileTypeImage);
        case EntryKind::Text: return tr(StringId::FileTypeText);
        case EntryKind::Playlist: return tr(StringId::FileTypePlaylist);
        case EntryKind::Other: break;
    }
    return tr(StringId::FileTypeOther);
}

const char *sort_mode_name(SortMode mode)
{
    switch (mode) {
        case SortMode::NameDescending: return tr(StringId::FileSortDescending);
        case SortMode::Type: return tr(StringId::FileSortType);
        case SortMode::NameAscending:
        case SortMode::Count: break;
    }
    return tr(StringId::FileSortAscending);
}

int32_t current_scroll()
{
    return s_scroll_body ? std::max<int32_t>(0, lv_obj_get_scroll_y(s_scroll_body)) : 0;
}

// Re-renders the folder in place, keeping the visible position.
void refresh_folder()
{
    s_pending_scroll_restore_y = current_scroll();
    render(s_view);
}

void open_folder(const char *path)
{
    push_navigation_state();
    s_list_page = 0;
    if (is_root(path)) {
        render(View::Folders);
        return;
    }
    copy_ui_text(s_folder_path, sizeof(s_folder_path), path);
    render(View::FolderDetail);
}

bool collect_folder_audio(size_t *indices, size_t *count, size_t selected, size_t *position)
{
    *count = 0;
    *position = SIZE_MAX;
    for (size_t index = 0; index < s_listing.count && *count < lyra::media::kMaxTracks; ++index) {
        if (s_listing.entries[index].kind != EntryKind::Audio) continue;
        char path[kMaxPath];
        size_t track = 0;
        if (!entry_path(index, path, sizeof(path)) ||
            !lyra::media::resolve_audio_path(path, &track)) continue;
        if (index == selected) *position = *count;
        indices[(*count)++] = track;
    }
    return *position != SIZE_MAX;
}

// Plays the folder's audio in the order shown, starting at the chosen file.
void play_folder_audio(size_t selected)
{
    auto *indices = static_cast<size_t *>(heap_caps_malloc(
        lyra::media::kMaxTracks * sizeof(size_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    size_t count = 0, position = 0;
    const bool found = indices && collect_folder_audio(indices, &count, selected, &position);
    const bool started = found && play_file_queue(indices, count, position);
    heap_caps_free(indices);
    if (started) navigate_to(View::Player);
    else show_notice(tr(StringId::Files), tr(StringId::FileOpenFailed));
}

void play_folder_tree(const char *path)
{
    auto *indices = static_cast<size_t *>(heap_caps_malloc(
        lyra::media::kMaxTracks * sizeof(size_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    size_t count = 0;
    const bool started = indices &&
        lyra::media::folder_tree_tracks(path, indices, lyra::media::kMaxTracks, &count) &&
        play_file_queue(indices, count, 0);
    heap_caps_free(indices);
    if (started) navigate_to(View::Player);
    else show_notice(tr(StringId::Files), tr(StringId::FolderNoSupportedFiles));
}

void show_details(const ActionTarget &target);

void open_entry(size_t index)
{
    char path[kMaxPath];
    if (!entry_path(index, path, sizeof(path))) return;
    switch (s_listing.entries[index].kind) {
        case EntryKind::Folder:
            open_folder(path);
            return;
        case EntryKind::Audio:
            if (s_queue_add_mode) {
                size_t track = 0;
                if (lyra::media::resolve_audio_path(path, &track)) show_queue_add_picker_for_track(track);
                return;
            }
            play_folder_audio(index);
            return;
        case EntryKind::Image:
            open_document_file(path, FileKind::Image, s_view);
            return;
        case EntryKind::Text:
            open_document_file(path, FileKind::Text, s_view);
            return;
        case EntryKind::Playlist:
            open_folder_playlist(path);
            return;
        case EntryKind::Other: {
            ActionTarget target{};
            copy_ui_text(target.path, sizeof(target.path), path);
            target.kind = EntryKind::Other;
            target.index = index;
            show_details(target);
            return;
        }
    }
}

lv_obj_t *make_overlay()
{
    lv_obj_t *overlay = make_box(s_screen, 0, 0, kScreenWidth, kScreenHeight, kOverlay);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_90, 0);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(overlay);
    lv_obj_add_event_cb(overlay, [](lv_event_t *event) {
        if (lv_event_get_target_obj(event) == lv_event_get_current_target_obj(event)) {
            lv_obj_delete(lv_event_get_current_target_obj(event));
        }
    }, LV_EVENT_CLICKED, nullptr);
    return overlay;
}

lv_obj_t *make_dialog_title(lv_obj_t *dialog, lv_obj_t *overlay, const char *text, int width)
{
    lv_obj_t *title = make_label(dialog, text, kTextPrimary);
    make_marquee(title, width - 64);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 14, 13);
    lv_obj_t *close = make_button(dialog, width - 42, 6, 34, 34, kSurface, 17);
    lv_obj_t *close_icon = make_label(close, LV_SYMBOL_CLOSE, kTextSecondary);
    lv_obj_center(close_icon);
    lv_obj_add_event_cb(close, dismiss_overlay_cb, LV_EVENT_CLICKED, overlay);
    return title;
}

void add_detail_line(lv_obj_t *dialog, int *y, StringId label, const char *value)
{
    lv_obj_t *caption = make_label(dialog, tr(label), kTextMuted);
    lv_obj_set_pos(caption, 14, *y);
    lv_obj_t *text = make_label(dialog, value, kTextPrimary);
    lv_obj_set_width(text, 252);
    lv_label_set_long_mode(text, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_pos(text, 14, *y + 22);
    lv_obj_update_layout(text);
    *y += 30 + std::min<int>(lv_obj_get_height(text), 66);
}

void show_details(const ActionTarget &target)
{
    struct stat info{};
    const bool have_info = path_exists(target.path, &info);
    lv_obj_t *overlay = make_overlay();
    lv_obj_t *dialog = make_box(overlay, 20, 70, 280, 340, kSurfaceRaised, 12);
    make_dialog_title(dialog, overlay, tr(StringId::Information), 280);
    int y = 52;
    add_detail_line(dialog, &y, StringId::Filename, base_name(target.path));
    char location[kMaxPath];
    parent_path(target.path, location, sizeof(location));
    add_detail_line(dialog, &y, StringId::FilePath,
                    location[std::strlen(kRoot)] ? location + std::strlen(kRoot) : kRootSeparator);
    char value[48];
    if (target.kind == EntryKind::Folder) {
        copy_ui_text(value, sizeof(value), kind_name(target.kind));
    } else if (have_info) {
        char size[24];
        format_file_size(static_cast<uint64_t>(info.st_size), size, sizeof(size));
        std::snprintf(value, sizeof(value), "%s / %s", kind_name(target.kind), size);
    } else {
        copy_ui_text(value, sizeof(value), tr(StringId::Unknown));
    }
    add_detail_line(dialog, &y, StringId::FileSize, value);
    format_file_date(have_info && info.st_mtime > 0 ? static_cast<uint64_t>(info.st_mtime) : 0,
                     value, sizeof(value));
    add_detail_line(dialog, &y, StringId::FileDateInfo, value);
    lv_obj_set_height(dialog, std::min(y + 8, 400));
    lv_obj_set_y(dialog, (kScreenHeight - lv_obj_get_height(dialog)) / 2);
}

// ---- Background delete/copy -------------------------------------------------

bool delete_tree(const char *path, unsigned depth)
{
    struct stat info{};
    if (!path_exists(path, &info)) return false;
    if (!S_ISDIR(info.st_mode)) {
        if (lyra::sd::remove(path, lyra::sd::Client::Filesystem) != 0) return false;
        s_job.items.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    if (depth > kMaxTreeDepth) return false;
    constexpr size_t kNames = 16;
    auto *names = static_cast<char (*)[lyra::media::kMaxPath]>(heap_caps_malloc(
        kNames * kMaxPath, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    auto *child = static_cast<char *>(heap_caps_malloc(kMaxPath, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    bool ok = names && child;
    // Read a small batch, close the directory, then delete those entries, so
    // FAT never iterates a directory while its entries are being removed.
    while (ok) {
        size_t found = 0;
        {
            SdGate gate;
            DIR *directory = opendir(path);
            if (!directory) {
                ok = false;
                break;
            }
            while (found < kNames) {
                const dirent *entry = readdir(directory);
                if (!entry) break;
                if (!std::strcmp(entry->d_name, ".") || !std::strcmp(entry->d_name, "..")) continue;
                copy_ui_text(names[found++], kMaxPath, entry->d_name);
            }
            closedir(directory);
        }
        if (!found) break;
        for (size_t i = 0; ok && i < found; ++i) {
            ok = join_path(child, kMaxPath, path, names[i]) && delete_tree(child, depth + 1);
        }
    }
    heap_caps_free(names);
    heap_caps_free(child);
    if (!ok) return false;
    {
        SdGate gate;
        ok = ::rmdir(path) == 0;
    }
    if (ok) s_job.items.fetch_add(1, std::memory_order_relaxed);
    return ok;
}

bool copy_file(const char *source, const char *target)
{
    constexpr size_t kChunk = 8192;
    auto *buffer = static_cast<uint8_t *>(heap_caps_malloc(kChunk, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!buffer) buffer = static_cast<uint8_t *>(heap_caps_malloc(kChunk, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buffer) return false;
    FILE *input = lyra::sd::open(source, "rb", lyra::sd::Client::Filesystem);
    FILE *output = input ? lyra::sd::open(target, "wb", lyra::sd::Client::Filesystem) : nullptr;
    bool ok = input && output;
    uint64_t copied = 0;
    while (ok) {
        const size_t read = lyra::sd::read(input, buffer, kChunk, lyra::sd::Client::Filesystem);
        if (!read) {
            ok = !std::ferror(input);
            break;
        }
        ok = lyra::sd::write_exact(output, buffer, read, lyra::sd::Client::Filesystem);
        copied += read;
        s_job.kilobytes.store(static_cast<uint32_t>(copied / 1024), std::memory_order_relaxed);
    }
    if (output && lyra::sd::close(output, lyra::sd::Client::Filesystem) != 0) ok = false;
    if (input) lyra::sd::close(input, lyra::sd::Client::Filesystem);
    heap_caps_free(buffer);
    if (!ok && output) lyra::sd::remove(target, lyra::sd::Client::Filesystem);
    if (ok) s_job.items.fetch_add(1, std::memory_order_relaxed);
    return ok;
}

void file_job_task(void *)
{
    bool ok = lyra::media::begin_file_access();
    if (ok) {
        ok = s_job.kind == JobKind::Delete ? delete_tree(s_job.source, 0) :
                                             copy_file(s_job.source, s_job.target);
        lyra::media::end_file_access();
    }
    s_job.succeeded = ok;
    s_job.done.store(true, std::memory_order_release);
    s_job.running.store(false, std::memory_order_release);
    vTaskDelete(nullptr);
}

void update_job_label()
{
    if (!s_job_label) return;
    char text[48];
    if (s_job.kind == JobKind::Copy) {
        format_float(StringId::SizeMegabytes,
                     s_job.kilobytes.load(std::memory_order_relaxed) / 1024.0, text, sizeof(text));
    } else {
        format_u32(StringId::FileProgressItems, s_job.items.load(std::memory_order_relaxed),
                   text, sizeof(text));
    }
    set_label_text_if_changed(s_job_label, text);
}

void job_poll_cb(lv_timer_t *timer)
{
    if (!s_job.done.load(std::memory_order_acquire)) {
        update_job_label();
        return;
    }
    lv_timer_delete(timer);
    s_job_timer = nullptr;
    if (s_job_overlay) lv_obj_delete(s_job_overlay);
    s_listing.valid = false;
    const bool folder_view = s_view == View::Folders || s_view == View::FolderDetail;
    if (folder_view) refresh_folder();
    if (!s_job.succeeded) {
        show_notice(tr(StringId::Files), tr(StringId::FileOperationFailed));
    } else if (folder_view) {
        show_toast(tr(s_job.kind == JobKind::Delete ? StringId::FileDeleted : StringId::FileCopied));
    }
}

void start_job(JobKind kind, const char *source, const char *target)
{
    if (s_job.running.load(std::memory_order_acquire)) return;
    s_job.kind = kind;
    copy_ui_text(s_job.source, sizeof(s_job.source), source);
    copy_ui_text(s_job.target, sizeof(s_job.target), target ? target : "");
    s_job.items.store(0, std::memory_order_relaxed);
    s_job.kilobytes.store(0, std::memory_order_relaxed);
    s_job.succeeded = false;
    s_job.done.store(false, std::memory_order_relaxed);
    s_job.running.store(true, std::memory_order_release);
    if (xTaskCreate(file_job_task, "lyra.files", 8192, nullptr, 2, nullptr) != pdPASS) {
        s_job.running.store(false, std::memory_order_release);
        show_notice(tr(StringId::Files), tr(StringId::FileMemoryError));
        return;
    }
    s_job_overlay = make_box(s_screen, 0, 0, kScreenWidth, kScreenHeight, kOverlay);
    lv_obj_set_style_bg_opa(s_job_overlay, LV_OPA_80, 0);
    lv_obj_add_flag(s_job_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(s_job_overlay);
    lv_obj_add_event_cb(s_job_overlay, [](lv_event_t *) {
        s_job_overlay = nullptr;
        s_job_label = nullptr;
    }, LV_EVENT_DELETE, nullptr);
    lv_obj_t *dialog = make_box(s_job_overlay, 40, 160, 240, 160, kSurfaceRaised, 12);
    lv_obj_t *spinner = lv_spinner_create(dialog);
    lv_obj_set_size(spinner, 48, 48);
    lv_obj_align(spinner, LV_ALIGN_TOP_MID, 0, 16);
    lv_spinner_set_anim_params(spinner, 900, 250);
    lv_obj_set_style_arc_color(spinner, kDivider, LV_PART_MAIN);
    lv_obj_set_style_arc_color(spinner, kAccent, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(spinner, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_width(spinner, 4, LV_PART_INDICATOR);
    lv_obj_t *title = make_label(dialog, tr(kind == JobKind::Delete ? StringId::FileDeleting :
                                                                      StringId::FileCopying), kTextPrimary);
    make_marquee(title, 220);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 76);
    s_job_label = make_label(dialog, "", kTextSecondary);
    lv_obj_align(s_job_label, LV_ALIGN_TOP_MID, 0, 106);
    update_job_label();
    if (!s_job_timer) s_job_timer = lv_timer_create(job_poll_cb, 150, nullptr);
}

// ---- Actions ----------------------------------------------------------------

void confirm_delete_cb(lv_event_t *event)
{
    lv_obj_t *overlay = static_cast<lv_obj_t *>(lv_event_get_user_data(event));
    if (overlay) lv_obj_delete(overlay);
    if (s_clipboard.active && std::strncmp(s_clipboard.path, s_action.path,
                                           std::strlen(s_action.path)) == 0) {
        s_clipboard.active = false;
    }
    start_job(JobKind::Delete, s_action.path, nullptr);
}

void show_delete_confirmation()
{
    lv_obj_t *overlay = make_overlay();
    lv_obj_t *dialog = make_box(overlay, 20, 130, 280, 220, kSurfaceRaised, 12);
    lv_obj_t *title = make_label(dialog, tr(StringId::FileDeleteQuestion), kTextPrimary);
    make_marquee(title, 248);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 18);
    lv_obj_t *name = make_label(dialog, base_name(s_action.path), kAccent);
    make_marquee(name, 248);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 46);
    lv_obj_t *message = make_label(dialog, tr(s_action.kind == EntryKind::Folder ?
        StringId::FolderDeleteMessage : StringId::FileDeleteMessage), kTextSecondary);
    lv_obj_set_width(message, 248);
    lv_label_set_long_mode(message, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(message, LV_ALIGN_TOP_MID, 0, 78);
    lv_obj_t *cancel = make_button(dialog, 14, 160, 116, 46, kSurface, 7);
    lv_obj_t *cancel_label = make_label(cancel, tr(StringId::Cancel), kTextSecondary);
    lv_obj_center(cancel_label);
    lv_obj_add_event_cb(cancel, dismiss_overlay_cb, LV_EVENT_CLICKED, overlay);
    lv_obj_t *confirm = make_button(dialog, 150, 160, 116, 46, lv_color_hex(0x991B1B), 7);
    lv_obj_t *confirm_label = make_label(confirm, tr(StringId::Delete), kTextOnAccent);
    lv_obj_center(confirm_label);
    lv_obj_add_event_cb(confirm, confirm_delete_cb, LV_EVENT_CLICKED, overlay);
}

void begin_name_input(NameInputMode mode, const char *source)
{
    s_name_mode = mode;
    copy_ui_text(s_name_source, sizeof(s_name_source), source ? source : "");
    copy_ui_text(s_name_input, sizeof(s_name_input), source ? base_name(source) : "");
    navigate_to(View::FileNameInput);
}

void run_action(EntryAction action)
{
    const bool busy = path_in_use(s_action.path);
    switch (action) {
        case EntryAction::Open:
            if (s_action.index < s_listing.count) open_entry(s_action.index);
            return;
        case EntryAction::Play:
            if (s_action.kind == EntryKind::Folder) play_folder_tree(s_action.path);
            else play_folder_audio(s_action.index);
            return;
        case EntryAction::AddToQueue:
            if (s_action.kind == EntryKind::Folder) {
                show_queue_add_picker_for_folder(s_action.path);
            } else {
                size_t track = 0;
                if (lyra::media::resolve_audio_path(s_action.path, &track)) {
                    show_queue_add_picker_for_track(track);
                } else {
                    show_notice(tr(StringId::Queue), tr(StringId::QueueAddFailed));
                }
            }
            return;
        case EntryAction::Details:
            show_details(s_action);
            return;
        case EntryAction::Rename:
        case EntryAction::Move:
        case EntryAction::Delete:
            if (busy) {
                show_notice(tr(StringId::Files), tr(StringId::FileInUse));
                return;
            }
            if (action == EntryAction::Rename) begin_name_input(NameInputMode::Rename, s_action.path);
            else if (action == EntryAction::Delete) show_delete_confirmation();
            else {
                s_clipboard = {true, false, {}};
                copy_ui_text(s_clipboard.path, sizeof(s_clipboard.path), s_action.path);
                refresh_folder();
            }
            return;
        case EntryAction::Copy:
            s_clipboard = {true, true, {}};
            copy_ui_text(s_clipboard.path, sizeof(s_clipboard.path), s_action.path);
            refresh_folder();
            return;
    }
}

void action_button_cb(lv_event_t *event)
{
    const auto action = static_cast<EntryAction>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    lv_obj_t *overlay = lv_obj_get_parent(lv_obj_get_parent(lv_event_get_current_target_obj(event)));
    // Close the sheet first; most actions render a new page or dialog.
    lv_obj_delete(overlay);
    run_action(action);
}

void show_entry_actions(size_t index)
{
    if (index >= s_listing.count || !entry_path(index, s_action.path, sizeof(s_action.path))) return;
    s_action.kind = s_listing.entries[index].kind;
    s_action.index = index;
    struct Option { EntryAction action; StringId label; const char *icon; };
    Option options[8];
    size_t count = 0;
    const EntryKind kind = s_action.kind;
    const bool audio = kind == EntryKind::Audio;
    const bool folder = kind == EntryKind::Folder;
    if (!audio && kind != EntryKind::Other) options[count++] = {EntryAction::Open, StringId::FileOpen, LV_SYMBOL_RIGHT};
    if (audio || folder) {
        options[count++] = {EntryAction::Play, StringId::FilePlay, LV_SYMBOL_PLAY};
        options[count++] = {EntryAction::AddToQueue, StringId::FileAddToQueue, LV_SYMBOL_LIST};
    }
    options[count++] = {EntryAction::Rename, StringId::FileRename, LV_SYMBOL_EDIT};
    options[count++] = {EntryAction::Move, StringId::FileMove, LV_SYMBOL_CUT};
    if (!folder) options[count++] = {EntryAction::Copy, StringId::FileCopy, LV_SYMBOL_COPY};
    options[count++] = {EntryAction::Delete, StringId::Delete, LV_SYMBOL_TRASH};
    options[count++] = {EntryAction::Details, StringId::Information, LV_SYMBOL_FILE};

    constexpr int kOptionPitch = 44;
    const int height = 52 + static_cast<int>(count) * kOptionPitch + 6;
    lv_obj_t *overlay = make_overlay();
    lv_obj_t *dialog = make_box(overlay, 20, (kScreenHeight - height) / 2, 280, height, kSurfaceRaised, 12);
    make_dialog_title(dialog, overlay, base_name(s_action.path), 280);
    for (size_t i = 0; i < count; ++i) {
        const bool danger = options[i].action == EntryAction::Delete;
        lv_obj_t *button = make_button(dialog, 10, 50 + static_cast<int>(i) * kOptionPitch, 260, 40,
                                       danger ? kDangerSurface : kSurface, 7);
        lv_obj_t *icon = make_label(button, options[i].icon, danger ? kTextPrimary : kAccent);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 12, 0);
        lv_obj_t *label = make_label(button, tr(options[i].label), kTextPrimary);
        make_marquee(label, 200);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 44, 0);
        lv_obj_add_event_cb(button, action_button_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<uintptr_t>(options[i].action)));
    }
}

// ---- Clipboard --------------------------------------------------------------

void paste_cb(lv_event_t *)
{
    if (!s_clipboard.active) return;
    char target[kMaxPath];
    if (!join_path(target, sizeof(target), s_folder_path, base_name(s_clipboard.path))) {
        show_notice(tr(StringId::Files), tr(StringId::FileOperationFailed));
        return;
    }
    if (std::strcmp(target, s_clipboard.path) == 0 || path_exists(target)) {
        show_notice(tr(StringId::Files), tr(StringId::FileNameExists));
        return;
    }
    if (s_clipboard.copy) {
        s_clipboard.active = false;
        start_job(JobKind::Copy, s_clipboard.path, target);
        return;
    }
    const size_t source_length = std::strlen(s_clipboard.path);
    if (std::strncmp(s_folder_path, s_clipboard.path, source_length) == 0 &&
        (s_folder_path[source_length] == '\0' || s_folder_path[source_length] == '/')) {
        show_notice(tr(StringId::Files), tr(StringId::FileMoveIntoItself));
        return;
    }
    if (path_in_use(s_clipboard.path)) {
        show_notice(tr(StringId::Files), tr(StringId::FileInUse));
        return;
    }
    // A move on one FAT volume only rewrites directory entries.
    const bool moved = lyra::sd::rename(s_clipboard.path, target, lyra::sd::Client::Filesystem) == 0;
    if (!moved) {
        show_notice(tr(StringId::Files), tr(StringId::FileOperationFailed));
        return;
    }
    s_clipboard.active = false;
    s_listing.valid = false;
    refresh_folder();
    show_toast(tr(StringId::FileMoved));
}

void make_clipboard_bar(lv_obj_t *body, int y)
{
    lv_obj_t *bar = make_box(body, 7, y, 306, 44, kAccentSurface, 6);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    char text[lyra::media::kMaxName + 24];
    format_text(s_clipboard.copy ? StringId::FileReadyToCopy : StringId::FileReadyToMove,
                base_name(s_clipboard.path), text, sizeof(text));
    lv_obj_t *label = make_label(bar, text, kTextOnAccent);
    make_marquee(label, 150);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_t *paste = make_button(bar, 166, 5, 92, 34, kAccentDark, 6);
    lv_obj_t *paste_label = make_label(paste, tr(StringId::FilePasteHere), kTextOnAccent);
    make_marquee(paste_label, 84);
    lv_obj_center(paste_label);
    lv_obj_add_event_cb(paste, paste_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *cancel = make_button(bar, 262, 5, 38, 34, kSurface, 6);
    lv_obj_t *cancel_icon = make_label(cancel, LV_SYMBOL_CLOSE, kTextSecondary);
    lv_obj_center(cancel_icon);
    lv_obj_add_event_cb(cancel, [](lv_event_t *) {
        s_clipboard.active = false;
        refresh_folder();
    }, LV_EVENT_CLICKED, nullptr);
}

// ---- Rows -------------------------------------------------------------------

void entry_clicked_cb(lv_event_t *event)
{
    const size_t index = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    if (index < s_listing.count) open_entry(index);
}

void entry_long_pressed_cb(lv_event_t *event)
{
    lv_indev_wait_release(lv_indev_active());
    show_entry_actions(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
}

void entry_more_cb(lv_event_t *event)
{
    lv_event_stop_bubbling(event);
    const size_t index = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    const EntryKind kind = index < s_listing.count ? s_listing.entries[index].kind : EntryKind::Other;
    char path[kMaxPath];
    if (s_queue_add_mode && entry_path(index, path, sizeof(path))) {
        if (kind == EntryKind::Folder) {
            show_queue_add_picker_for_folder(path);
            return;
        }
        size_t track = 0;
        if (kind == EntryKind::Audio && lyra::media::resolve_audio_path(path, &track)) {
            show_queue_add_picker_for_track(track);
            return;
        }
    }
    show_entry_actions(index);
}

void make_entry_row(lv_obj_t *body, int y, size_t index)
{
    const EntryKind kind = s_listing.entries[index].kind;
    const char *name = entry_name(index);
    void *payload = reinterpret_cast<void *>(index);
    lv_obj_t *row = make_button(body, 7, y, 306, 54, kSurface, 5, true);
    lv_obj_t *icon = make_label(row, kind_icon(kind),
        kind == EntryKind::Other ? kTextMuted : kAccent);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_t *title = make_label(row, name, kTextPrimary);
    make_marquee(title, 200);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 43, -9);
    char subtitle[48];
    if (kind == EntryKind::Folder) {
        copy_ui_text(subtitle, sizeof(subtitle), tr(StringId::Folder));
    } else {
        char path[kMaxPath];
        struct stat info{};
        if (entry_path(index, path, sizeof(path)) && path_exists(path, &info)) {
            char size[24];
            format_file_size(static_cast<uint64_t>(info.st_size), size, sizeof(size));
            std::snprintf(subtitle, sizeof(subtitle), "%s / %s", kind_name(kind), size);
        } else {
            copy_ui_text(subtitle, sizeof(subtitle), kind_name(kind));
        }
    }
    lv_obj_t *detail = make_label(row, subtitle, kTextSecondary);
    lv_obj_set_width(detail, 200);
    lv_label_set_long_mode(detail, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(detail, LV_ALIGN_LEFT_MID, 43, 11);
    const bool queueable = s_queue_add_mode && (kind == EntryKind::Folder || kind == EntryKind::Audio);
    lv_obj_t *more = make_button(row, 252, 8, 46, 38, queueable ? kAccentDark : kSurfaceRaised, 6);
    lv_obj_t *more_icon = make_label(more, queueable ? LV_SYMBOL_PLUS :
        LV_SYMBOL_BULLET LV_SYMBOL_BULLET LV_SYMBOL_BULLET, queueable ? kTextOnAccent : kTextSecondary);
    lv_obj_center(more_icon);
    lv_obj_add_event_cb(more, entry_more_cb, LV_EVENT_CLICKED, payload);
    lv_obj_add_event_cb(row, entry_clicked_cb, LV_EVENT_CLICKED, payload);
    lv_obj_add_event_cb(row, entry_long_pressed_cb, LV_EVENT_LONG_PRESSED, payload);
}

void append_rows(lv_obj_t *body, ListContext *context)
{
    const size_t end = std::min(context->loaded + kRowBatch, s_listing.count);
    for (size_t index = context->loaded; index < end; ++index) {
        make_entry_row(body, context->top + static_cast<int>(index) * kRowPitch, index);
    }
    context->loaded = end;
}

void list_event_cb(lv_event_t *event)
{
    auto *context = static_cast<ListContext *>(lv_event_get_user_data(event));
    if (lv_event_get_code(event) == LV_EVENT_DELETE) {
        lv_free(context);
        return;
    }
    if (context->loaded >= s_listing.count) return;
    lv_obj_t *body = lv_event_get_current_target_obj(event);
    if (lv_obj_get_scroll_bottom(body) < 160) append_rows(body, context);
}

void toolbar_button(lv_obj_t *body, int x, int width, const char *text, lv_event_cb_t callback,
                    bool enabled = true, bool highlight = false)
{
    lv_obj_t *button = make_button(body, x, 0, width, kToolbarHeight,
                                   highlight ? kAccentSurface : kSurfaceRaised, 6, true);
    lv_obj_t *label = make_label(button, text, enabled ? (highlight ? kAccent : kTextPrimary) : kTextMuted);
    make_marquee(label, width - 8);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
    if (enabled) lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, nullptr);
}

void make_toolbar(lv_obj_t *body)
{
    toolbar_button(body, 7, 44, LV_SYMBOL_UP, [](lv_event_t *) {
        char parent[kMaxPath];
        parent_path(s_folder_path, parent, sizeof(parent));
        open_folder(parent);
    }, !is_root(s_folder_path));
    const char *relative = s_folder_path + std::strlen(kRoot);
    const char *shown = relative[0] ? relative : kRootSeparator;
    lv_obj_t *path = make_label(body, shown, kTextMuted);
    make_marquee(path, 116);
    lv_obj_set_pos(path, 58, 10);
    toolbar_button(body, 180, 72, sort_mode_name(s_sort_mode), [](lv_event_t *) {
        s_sort_mode = static_cast<SortMode>((static_cast<uint8_t>(s_sort_mode) + 1) %
                                            static_cast<uint8_t>(SortMode::Count));
        s_pending_scroll_restore_y = 0;
        render(s_view);
    });
    toolbar_button(body, 257, 56, LV_SYMBOL_PLUS " " LV_SYMBOL_DIRECTORY, [](lv_event_t *) {
        begin_name_input(NameInputMode::NewFolder, nullptr);
    });
}

bool valid_name(const char *name)
{
    const size_t length = std::strlen(name);
    if (!length || name[0] == '.' || name[length - 1] == '.' || name[length - 1] == ' ') return false;
    for (const char *cursor = name; *cursor; ++cursor) {
        const unsigned char c = static_cast<unsigned char>(*cursor);
        if (c < 0x20 || std::strchr("/\\:*?\"<>|", c)) return false;
    }
    return true;
}

void trim_name(char *name)
{
    size_t start = 0;
    while (name[start] == ' ') ++start;
    size_t length = std::strlen(name + start);
    std::memmove(name, name + start, length + 1);
    while (length && name[length - 1] == ' ') name[--length] = '\0';
}

} // namespace

void invalidate_file_listing()
{
    s_listing.valid = false;
}

char *file_name_input_text()
{
    return s_name_input;
}

size_t file_name_input_capacity()
{
    return sizeof(s_name_input);
}

bool file_name_input_creates_folder()
{
    return s_name_mode == NameInputMode::NewFolder;
}

void submit_file_name_input()
{
    trim_name(s_name_input);
    if (!valid_name(s_name_input)) {
        show_notice(tr(StringId::Files), tr(StringId::FileNameInvalid));
        return;
    }
    const bool creating = s_name_mode == NameInputMode::NewFolder;
    char directory[kMaxPath];
    if (creating) copy_ui_text(directory, sizeof(directory), s_folder_path);
    else parent_path(s_name_source, directory, sizeof(directory));
    char target[kMaxPath];
    if (!join_path(target, sizeof(target), directory, s_name_input)) {
        show_notice(tr(StringId::Files), tr(StringId::FileOperationFailed));
        return;
    }
    if (!creating && std::strcmp(target, s_name_source) == 0) {
        navigate_back(View::Folders);
        return;
    }
    // FAT names are case-insensitive, so a case-only rename finds itself.
    const bool case_change = !creating && ::strcasecmp(target, s_name_source) == 0;
    if (!case_change && path_exists(target)) {
        show_notice(tr(StringId::Files), tr(StringId::FileNameExists));
        return;
    }
    bool ok;
    if (creating) {
        SdGate gate;
        ok = ::mkdir(target, 0775) == 0;
    } else {
        ok = !path_in_use(s_name_source) &&
             lyra::sd::rename(s_name_source, target, lyra::sd::Client::Filesystem) == 0;
    }
    if (!ok) {
        show_notice(tr(StringId::Files), tr(StringId::FileOperationFailed));
        return;
    }
    if (!creating && s_clipboard.active && std::strcmp(s_clipboard.path, s_name_source) == 0) {
        copy_ui_text(s_clipboard.path, sizeof(s_clipboard.path), target);
    }
    s_name_input[0] = '\0';
    s_listing.valid = false;
    navigate_back(View::Folders);
    show_toast(tr(creating ? StringId::FileFolderCreated : StringId::FileRenamed));
}

void folder_back_cb(lv_event_t *)
{
    navigate_back(View::Folders);
}

void render_folders(bool detail)
{
    if (!detail) copy_ui_text(s_folder_path, sizeof(s_folder_path), kRoot);
    load_listing(s_folder_path);
    char count_text[32];
    format_count(StringId::FileItemCount, static_cast<uint32_t>(s_listing.count),
                 count_text, sizeof(count_text));
    make_header(detail ? base_name(s_folder_path) : tr(StringId::Files),
                detail ? View::Folders : View::Menu, true, count_text,
                detail ? folder_back_cb : nullptr);
    lv_obj_t *body = make_scroll_body(72);
    make_toolbar(body);
    int top = kToolbarHeight + 6;
    if (s_clipboard.active) {
        make_clipboard_bar(body, top);
        top += 50;
    }
    if (detail && s_queue_add_mode) {
        lv_obj_t *add_folder = make_button(body, 7, top, 306, 42, kSurfaceRaised, 6, true);
        lv_obj_t *add_icon = make_label(add_folder, LV_SYMBOL_PLUS, kAccent);
        lv_obj_align(add_icon, LV_ALIGN_LEFT_MID, 14, 0);
        lv_obj_t *add_label = make_label(add_folder, tr(StringId::AddFolderToQueue), kTextPrimary);
        make_marquee(add_label, 240);
        lv_obj_align(add_label, LV_ALIGN_LEFT_MID, 42, 0);
        lv_obj_add_event_cb(add_folder, [](lv_event_t *) {
            show_queue_add_picker_for_folder(s_folder_path);
        }, LV_EVENT_CLICKED, nullptr);
        top += 50;
    }
    if (s_listing.truncated) {
        char notice[64];
        format_u32(StringId::FileListTruncated, static_cast<uint32_t>(s_listing.count),
                   notice, sizeof(notice));
        lv_obj_t *label = make_label(body, notice, kTextMuted);
        make_marquee(label, 296);
        lv_obj_set_pos(label, 12, top);
        top += 28;
    }
    if (s_listing.failed && s_listing.count == 0) {
        lv_obj_t *label = make_label(body, tr(StringId::AppFileScanFailed), kTextMuted);
        lv_obj_set_pos(label, 12, top + 6);
        return;
    }
    if (s_listing.count == 0) {
        lv_obj_t *label = make_label(body, tr(StringId::FileEmptyFolder), kTextMuted);
        lv_obj_set_pos(label, 12, top + 6);
        return;
    }
    auto *context = static_cast<ListContext *>(lv_malloc_zeroed(sizeof(ListContext)));
    if (!context) return;
    context->top = top;
    append_rows(body, context);
    if (s_pending_scroll_restore_y > 0) {
        lv_obj_update_layout(body);
        while (context->loaded < s_listing.count &&
               lv_obj_get_scroll_bottom(body) < s_pending_scroll_restore_y) {
            append_rows(body, context);
            lv_obj_update_layout(body);
        }
    }
    lv_obj_add_event_cb(body, list_event_cb, LV_EVENT_SCROLL, context);
    lv_obj_add_event_cb(body, list_event_cb, LV_EVENT_SCROLL_END, context);
    lv_obj_add_event_cb(body, list_event_cb, LV_EVENT_DELETE, context);
}

} // namespace lyra::gui::internal
