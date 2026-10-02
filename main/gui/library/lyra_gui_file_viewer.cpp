/* SPDX-License-Identifier: Apache-2.0 */
#include "../lyra_gui_internal.h"
#include "lyra_image_file.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include <atomic>
#include <new>
#include <sys/stat.h>
#include <unistd.h>

namespace lyra::gui::internal {
namespace {
constexpr size_t kTextPageBytes = 2048;
constexpr uint16_t kImageViewHeight = 408;
constexpr uint32_t kNativeZoom = 65536;
constexpr int64_t kDetailDelayUs = 150000;
char s_file_path[lyra::media::kMaxPath];
lyra::media::FileKind s_file_kind;
std::atomic<bool> s_image_busy{false};

class FileAccess {
public:
    FileAccess() : ready(lyra::media::begin_file_access()) {}
    ~FileAccess() { if (ready) lyra::media::end_file_access(); }
    bool ready;
};

struct Viewer {
    std::atomic<unsigned> references{1};
    std::atomic<bool> complete{false};
    std::atomic<bool> cancelled{false};
    std::atomic<uint32_t> revision{0};
    char path[lyra::media::kMaxPath]{};
    lv_obj_t *root{}, *body{}, *label{}, *image{}, *detail{}, *canvas{}, *status{};
    lv_timer_t *timer{};
    uint16_t *pixels{}, *detail_pixels{}, *work_pixels{};
    lv_image_dsc_t descriptor{}, detail_descriptor{};
    lyra::media::ImageFileInfo file_info{};
    lyra::media::ImageViewport work_view{};
    uint32_t work_revision{}, applied_revision{};
    uint32_t zoom{kNativeZoom}, fit_zoom{kNativeZoom};
    int64_t changed_at_us{};
    uint16_t preview_width{}, preview_height{};
    char text[kTextPageBytes + 1]{};
    uint32_t offset{}, next_offset{}, file_size{};
    uint32_t previous_offsets[32]{};
    unsigned previous_count{};
    uint8_t font_size{};
    bool dark{}, is_text{}, loaded{}, decoding_started{}, work_initial{}, work_ok{}, changing_scale{};
};

// Viewer surfaces deliberately bypass theme decorations so the reader always
// has the requested black/white contrast and images have a neutral surround.
lv_obj_t *viewer_box(lv_obj_t *parent, int x, int y, int width, int height, lv_color_t color)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, width, height);
    lv_obj_set_style_bg_color(box, color, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    return box;
}

void release(Viewer *v)
{
    if (v->references.fetch_sub(1) == 1) {
        heap_caps_free(v->pixels);
        heap_caps_free(v->detail_pixels);
        heap_caps_free(v->work_pixels);
        delete v;
    }
}

void image_task(void *argument)
{
    auto *v = static_cast<Viewer *>(argument);
    const lyra::media::ImageReadControl control{&v->cancelled, &v->revision, v->work_revision};
    const bool access = lyra::media::begin_file_access();
    bool ok = access && !control.cancelled();
    if (ok && v->work_initial) {
        ok = lyra::media::image_file_info(v->path, &v->file_info, control);
        if (ok) {
            v->fit_zoom = std::max<uint32_t>(1, std::min<uint64_t>(
                uint64_t(kScreenWidth) * kNativeZoom / v->file_info.width,
                uint64_t(kImageViewHeight) * kNativeZoom / v->file_info.height));
            v->preview_width = std::max<uint64_t>(1,
                uint64_t(v->file_info.width) * v->fit_zoom / kNativeZoom);
            v->preview_height = std::max<uint64_t>(1,
                uint64_t(v->file_info.height) * v->fit_zoom / kNativeZoom);
            v->work_view = {v->preview_width, v->preview_height, 0, 0,
                            v->preview_width, v->preview_height};
        }
    }
    if (ok) {
        v->work_pixels = static_cast<uint16_t *>(heap_caps_malloc(
            size_t(v->work_view.width) * v->work_view.height * 2,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        ok = v->work_pixels && lyra::media::decode_image_file_viewport(v->path,
            v->file_info, v->work_view, v->work_pixels, control);
    }
    if (access) lyra::media::end_file_access();
    v->work_ok = ok;
    v->complete.store(true, std::memory_order_release);
    s_image_busy.store(false);
    release(v);
    vTaskDelete(nullptr);
}

struct Bookmark {
    uint32_t magic = 0x4C594231;
    uint32_t offset{}, scroll{};
    uint8_t font{}, dark{};
    uint8_t reserved[2]{};
    char path[lyra::media::kMaxPath]{};
};

void bookmark_path(const Viewer *v, char *out, size_t capacity)
{
    uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(v->path); *p; ++p)
        hash = (hash ^ *p) * 1099511628211ULL;
    std::snprintf(out, capacity, "/sdcard/.lyra/bookmarks/%08lx%08lx.bin",
        static_cast<unsigned long>(hash >> 32), static_cast<unsigned long>(hash & 0xFFFFFFFFu));
}

bool read_bookmark(Viewer *v, Bookmark *mark)
{
    FileAccess access;
    if (!access.ready) return false;
    char path[100], backup[108];
    bookmark_path(v, path, sizeof(path));
    std::snprintf(backup, sizeof(backup), "%s.bak", path);
    for (const char *candidate : {path, backup}) {
        FILE *file = lyra::sd::open(candidate, "rb", lyra::sd::Client::Filesystem);
        if (!file) continue;
        const bool ok = lyra::sd::read_exact(file, mark, sizeof(*mark), lyra::sd::Client::Filesystem);
        lyra::sd::close(file, lyra::sd::Client::Filesystem);
        if (ok && mark->magic == 0x4C594231 &&
            std::memcmp(mark->path, v->path, sizeof(mark->path)) == 0 &&
            mark->font < 3 && mark->dark < 2) return true;
    }
    return false;
}

const lv_font_t *reader_font(unsigned size)
{
    static lv_font_t medium = lv_font_montserrat_18;
    static lv_font_t large = lv_font_montserrat_28;
    medium.fallback = lyra::font::ui();
    large.fallback = lyra::font::ui();
    return size == 0 ? lyra::font::ui() : size == 1 ? &medium : &large;
}

void reader_style(Viewer *v)
{
    const lv_color_t bg = lv_color_hex(v->dark ? 0 : 0xFFFFFF);
    const lv_color_t fg = lv_color_hex(v->dark ? 0xFFFFFF : 0);
    lv_obj_set_style_bg_color(v->root, bg, 0);
    lv_obj_set_style_bg_color(v->body, bg, 0);
    lv_obj_set_style_text_color(v->label, fg, 0);
    lv_obj_set_style_text_font(v->label, reader_font(v->font_size), 0);
    lv_obj_set_style_text_color(v->status, fg, 0);
    lv_obj_update_layout(v->body);
}

bool text_page(Viewer *v, uint32_t offset, uint32_t scroll = 0)
{
    FileAccess access;
    if (!access.ready) { lv_label_set_text(v->status, tr(lyra::i18n::StringId::NoMicroSdCard)); return false; }
    FILE *file = lyra::sd::open(v->path, "rb", lyra::sd::Client::Filesystem);
    if (!file) { lv_label_set_text(v->status, tr(lyra::i18n::StringId::FileOpenFailed)); return false; }
    if (lyra::sd::seek(file, 0, SEEK_END, lyra::sd::Client::Filesystem) != 0) {
        lyra::sd::close(file, lyra::sd::Client::Filesystem); return false;
    }
    const long length = std::ftell(file);
    if (length < 0) { lyra::sd::close(file, lyra::sd::Client::Filesystem); return false; }
    v->file_size = length;
    if (offset >= v->file_size) offset = 0;
    if (lyra::sd::seek(file, offset, SEEK_SET, lyra::sd::Client::Filesystem) != 0) {
        lyra::sd::close(file, lyra::sd::Client::Filesystem); return false;
    }
    size_t count = lyra::sd::read(file, v->text, kTextPageBytes, lyra::sd::Client::Filesystem);
    const bool failed = std::ferror(file) != 0;
    lyra::sd::close(file, lyra::sd::Client::Filesystem);
    if (failed) { lv_label_set_text(v->status, tr(lyra::i18n::StringId::FileReadFailed)); return false; }
    // Never split a UTF-8 character between pages. At an arbitrary restored
    // offset (e.g. a changed file), discard leading continuation bytes.
    size_t start = 0;
    while (start < count && (uint8_t(v->text[start]) & 0xC0) == 0x80) ++start;
    if (offset + count < v->file_size) {
        size_t last = count;
        while (last > start && (uint8_t(v->text[last - 1]) & 0xC0) == 0x80) --last;
        if (last > start) {
            --last;
            const uint8_t lead = v->text[last];
            const unsigned needed = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
            if (count - last < needed) count = last;
        }
    }
    v->offset = offset + start;
    v->next_offset = offset + count;
    std::memmove(v->text, v->text + start, count - start);
    count -= start;
    if (v->offset == 0 && count >= 3 && uint8_t(v->text[0]) == 0xEF &&
        uint8_t(v->text[1]) == 0xBB && uint8_t(v->text[2]) == 0xBF) {
        std::memmove(v->text, v->text + 3, count - 3); count -= 3;
    }
    for (size_t i = 0; i < count; ++i) {
        if (v->text[i] == '\r') v->text[i] = ' ';
        if (v->text[i] == '\t') v->text[i] = ' ';
        if (!v->text[i]) v->text[i] = ' ';
    }
    v->text[count] = '\0';
    lv_label_set_text(v->label, count ? v->text : tr(lyra::i18n::StringId::ReaderEmptyFile));
    char position[96];
    std::snprintf(position, sizeof(position), "%lu / %lu bytes",
        static_cast<unsigned long>(v->offset), static_cast<unsigned long>(v->file_size));
    lv_label_set_text(v->status, position);
    reader_style(v);
    lv_obj_scroll_to_y(v->body, scroll, LV_ANIM_OFF);
    return true;
}

void text_next(Viewer *v)
{
    if (v->next_offset >= v->file_size) return;
    const uint32_t previous = v->offset;
    if (text_page(v, v->next_offset)) {
        if (v->previous_count == 32) {
            std::memmove(v->previous_offsets, v->previous_offsets + 1, 31 * sizeof(uint32_t));
            --v->previous_count;
        }
        v->previous_offsets[v->previous_count++] = previous;
    }
}

void text_previous(Viewer *v, bool bottom = false)
{
    if (!v->offset) return;
    const uint32_t offset = v->previous_count ? v->previous_offsets[v->previous_count - 1] :
        (v->offset > kTextPageBytes ? v->offset - kTextPageBytes : 0);
    if (text_page(v, offset)) {
        if (v->previous_count) --v->previous_count;
        if (bottom) lv_obj_scroll_to_y(v->body, std::max<int32_t>(0,
            lv_obj_get_scroll_bottom(v->body) + lv_obj_get_scroll_y(v->body)), LV_ANIM_OFF);
    }
}

void save_bookmark(Viewer *v)
{
    FileAccess access;
    if (!access.ready) { lv_label_set_text(v->status, tr(lyra::i18n::StringId::NoMicroSdCard)); return; }
    Bookmark mark;
    mark.offset = v->offset;
    mark.scroll = std::max<int32_t>(0, lv_obj_get_scroll_y(v->body));
    mark.font = v->font_size;
    mark.dark = v->dark;
    copy_ui_text(mark.path, sizeof(mark.path), v->path);
    char path[100], temp[108], backup[108];
    bookmark_path(v, path, sizeof(path));
    std::snprintf(temp, sizeof(temp), "%s.tmp", path);
    std::snprintf(backup, sizeof(backup), "%s.bak", path);
    bool ok = lyra::sd::acquire(lyra::sd::Client::Filesystem);
    if (ok) {
        struct stat info{};
        ok = (stat("/sdcard/.lyra", &info) == 0 && S_ISDIR(info.st_mode)) ||
             mkdir("/sdcard/.lyra", 0775) == 0;
        ok = ok && ((stat("/sdcard/.lyra/bookmarks", &info) == 0 && S_ISDIR(info.st_mode)) ||
             mkdir("/sdcard/.lyra/bookmarks", 0775) == 0);
        lyra::sd::release(lyra::sd::Client::Filesystem);
    }
    FILE *file = ok ? lyra::sd::open(temp, "wb", lyra::sd::Client::Filesystem) : nullptr;
    ok = file && lyra::sd::write_exact(file, &mark, sizeof(mark), lyra::sd::Client::Filesystem);
    if (file) {
        if (lyra::sd::acquire(lyra::sd::Client::Filesystem)) {
            ok = ok && std::fflush(file) == 0 && fsync(fileno(file)) == 0;
            lyra::sd::release(lyra::sd::Client::Filesystem);
        } else ok = false;
        ok = lyra::sd::close(file, lyra::sd::Client::Filesystem) == 0 && ok;
    }
    if (ok) {
        lyra::sd::remove(backup, lyra::sd::Client::Filesystem);
        struct stat info{};
        if (stat(path, &info) == 0)
            ok = lyra::sd::rename(path, backup, lyra::sd::Client::Filesystem) == 0;
        if (ok) {
            ok = lyra::sd::rename(temp, path, lyra::sd::Client::Filesystem) == 0;
            if (!ok) lyra::sd::rename(backup, path, lyra::sd::Client::Filesystem);
        }
        if (ok) lyra::sd::remove(backup, lyra::sd::Client::Filesystem);
    }
    if (!ok) lyra::sd::remove(temp, lyra::sd::Client::Filesystem);
    lv_label_set_text(v->status, ok ? tr(lyra::i18n::StringId::ReaderBookmarkSaved) : tr(lyra::i18n::StringId::ReaderBookmarkFailed));
}

uint32_t scaled_dimension(uint32_t source, uint32_t zoom)
{
    return std::max<uint64_t>(1, uint64_t(source) * zoom / kNativeZoom);
}

void request_image_detail(Viewer *v)
{
    if (!v->loaded || v->changing_scale) return;
    v->revision.fetch_add(1);
    v->changed_at_us = esp_timer_get_time();
}

void image_scale(Viewer *v, uint32_t zoom, bool centre = false)
{
    if (!v->loaded) return;
    const uint32_t old_w = scaled_dimension(v->file_info.width, v->zoom);
    const uint32_t old_h = scaled_dimension(v->file_info.height, v->zoom);
    const auto anchor = [](uint32_t dimension, int scroll, int extent) -> int64_t {
        const int offset = dimension < unsigned(extent) ? (extent - dimension) / 2 : 0;
        return std::clamp<int64_t>((int64_t(scroll) + extent / 2 - offset) * kNativeZoom /
            dimension, 0, kNativeZoom);
    };
    const int64_t ax = centre ? kNativeZoom / 2 :
        anchor(old_w, lv_obj_get_scroll_x(v->body), kScreenWidth);
    const int64_t ay = centre ? kNativeZoom / 2 :
        anchor(old_h, lv_obj_get_scroll_y(v->body), kImageViewHeight);
    v->changing_scale = true;
    v->zoom = zoom;
    const int width = scaled_dimension(v->file_info.width, zoom);
    const int height = scaled_dimension(v->file_info.height, zoom);
    // The preview stretches immediately. A viewport patch from the original
    // file replaces its visible pixels once the requested decode completes.
    if (v->detail) lv_obj_add_flag(v->detail, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(v->canvas, std::max(kScreenWidth, width), std::max<int>(kImageViewHeight, height));
    lv_obj_set_size(v->image, width, height);
    lv_image_set_inner_align(v->image, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_center(v->image);
    lv_obj_update_layout(v->body);
    const int x = std::clamp<int64_t>(ax * width / kNativeZoom - kScreenWidth / 2,
        0, std::max(0, width - kScreenWidth));
    const int y = std::clamp<int64_t>(ay * height / kNativeZoom - kImageViewHeight / 2,
        0, std::max<int>(0, height - kImageViewHeight));
    lv_obj_scroll_to(v->body, x, y, LV_ANIM_OFF);
    v->changing_scale = false;
    request_image_detail(v);
}

void viewer_action(lv_event_t *event)
{
    auto *v = static_cast<Viewer *>(lv_event_get_user_data(event));
    const auto action = lv_obj_get_user_data(lv_event_get_current_target_obj(event));
    switch (reinterpret_cast<uintptr_t>(action)) {
        case 0: navigate_back(View::Folders); return;
        case 1:
            if (v->is_text) { if (v->font_size) --v->font_size; reader_style(v); }
            else image_scale(v, std::max(std::min(v->fit_zoom, kNativeZoom), v->zoom / 2));
            break;
        case 2:
            if (v->is_text) { if (v->font_size < 2) ++v->font_size; reader_style(v); }
            else image_scale(v, std::min(std::max(v->fit_zoom, kNativeZoom * 4), v->zoom * 2));
            break;
        case 3:
            if (v->is_text) { v->dark = !v->dark; reader_style(v); }
            else image_scale(v, v->fit_zoom, true);
            break;
        case 4: save_bookmark(v); break;
        case 5:
            if (lv_obj_get_scroll_y(v->body) <= 0) text_previous(v, true);
            else lv_obj_scroll_by(v->body, 0, 280, LV_ANIM_OFF);
            break;
        case 6:
            if (lv_obj_get_scroll_bottom(v->body) <= 0) text_next(v);
            else lv_obj_scroll_by(v->body, 0, -280, LV_ANIM_OFF);
            break;
        case 7: text_previous(v); break;
        case 8: text_next(v); break;
        case 9: image_scale(v, kNativeZoom); break;
    }
}

void control(Viewer *v, int x, int y, int width, const char *text, unsigned action)
{
    lv_obj_t *button = viewer_box(v->root, x, y, width, 42, lv_color_hex(0x202020));
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(button, 4, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x404040), LV_STATE_PRESSED);
    lv_obj_t *label = make_label(button, text, lv_color_hex(0xFFFFFF));
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
    make_marquee(label, width - 8);
    lv_obj_center(label);
    lv_obj_set_user_data(button, reinterpret_cast<void *>(uintptr_t(action)));
    lv_obj_add_event_cb(button, viewer_action, LV_EVENT_CLICKED, v);
}

void image_descriptor(lv_image_dsc_t *descriptor, uint16_t *pixels, unsigned width, unsigned height)
{
    *descriptor = {};
    descriptor->header.magic = LV_IMAGE_HEADER_MAGIC;
    descriptor->header.cf = LV_COLOR_FORMAT_RGB565;
    descriptor->header.w = width;
    descriptor->header.h = height;
    descriptor->header.stride = width * 2;
    descriptor->data_size = width * height * 2;
    descriptor->data = reinterpret_cast<const uint8_t *>(pixels);
}

void start_image_task(Viewer *v)
{
    if (s_image_busy.exchange(true)) return;
    v->decoding_started = true;
    v->complete.store(false);
    v->work_initial = !v->loaded;
    v->work_revision = v->revision.load();
    if (v->loaded) {
        v->work_view = {scaled_dimension(v->file_info.width, v->zoom),
            scaled_dimension(v->file_info.height, v->zoom),
            static_cast<uint32_t>(std::max<int32_t>(0, lv_obj_get_scroll_x(v->body))),
            static_cast<uint32_t>(std::max<int32_t>(0, lv_obj_get_scroll_y(v->body))),
            kScreenWidth, kImageViewHeight};
    }
    v->references.fetch_add(1);
    if (xTaskCreatePinnedToCore(image_task, "file_image", 24 * 1024, v, 1, nullptr, 1) != pdPASS) {
        s_image_busy.store(false);
        release(v);
        v->work_ok = false;
        v->complete.store(true);
    }
}

void image_poll(lv_timer_t *timer)
{
    auto *v = static_cast<Viewer *>(lv_timer_get_user_data(timer));
    if (v->decoding_started) {
        if (!v->complete.load(std::memory_order_acquire)) return;
        v->decoding_started = false;
        const bool current = v->revision.load() == v->work_revision;
        if (current && v->work_ok) {
            if (v->work_initial) {
                v->pixels = v->work_pixels;
                v->work_pixels = nullptr;
                v->loaded = true;
                image_descriptor(&v->descriptor, v->pixels, v->preview_width, v->preview_height);
                v->canvas = viewer_box(v->body, 0, 0, kScreenWidth, kImageViewHeight, lv_color_hex(0));
                lv_obj_clear_flag(v->canvas, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_clear_flag(v->canvas, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_add_flag(v->canvas, LV_OBJ_FLAG_SCROLL_CHAIN);
                v->image = lv_image_create(v->canvas);
                lv_image_set_src(v->image, &v->descriptor);
                v->zoom = v->fit_zoom;
                image_scale(v, v->fit_zoom, true);
                // The preview already contains the fit view's original pixels.
                v->applied_revision = v->revision.load();
            } else {
                if (v->detail) lv_image_cache_drop(&v->detail_descriptor);
                heap_caps_free(v->detail_pixels);
                v->detail_pixels = v->work_pixels;
                v->work_pixels = nullptr;
                image_descriptor(&v->detail_descriptor, v->detail_pixels, kScreenWidth, kImageViewHeight);
                if (!v->detail) v->detail = lv_image_create(v->canvas);
                lv_image_set_src(v->detail, &v->detail_descriptor);
                lv_obj_set_pos(v->detail, v->work_view.x, v->work_view.y);
                lv_obj_remove_flag(v->detail, LV_OBJ_FLAG_HIDDEN);
                v->applied_revision = v->work_revision;
            }
            lv_label_set_text(v->status, tr(lyra::i18n::StringId::ImagePanHint));
        } else if (current) {
            lv_label_set_text(v->status, tr(lyra::i18n::StringId::ImageDecodeFailed));
            v->applied_revision = v->work_revision;
            if (v->work_initial) {
                heap_caps_free(v->work_pixels); v->work_pixels = nullptr;
                v->timer = nullptr;
                lv_timer_delete(timer);
                return;
            }
        }
        heap_caps_free(v->work_pixels); v->work_pixels = nullptr;
    }
    if (!v->loaded || (v->revision.load() != v->applied_revision &&
        esp_timer_get_time() - v->changed_at_us >= kDetailDelayUs)) {
        // Requests coalesce until scrolling settles. Superseded work observes
        // revision changes in row loops and input callbacks and exits early.
        lv_label_set_text(v->status, tr(lyra::i18n::StringId::ImageLoading));
        start_image_task(v);
    }
}
} // namespace

void render_file_viewer()
{
    auto *v = new (std::nothrow) Viewer;
    if (!v) { show_notice(tr(lyra::i18n::StringId::FilePath), tr(lyra::i18n::StringId::FileMemoryError)); return; }
    copy_ui_text(v->path, sizeof(v->path), s_file_path);
    v->is_text = s_file_kind == lyra::media::FileKind::Text;
    v->root = viewer_box(s_screen, 0, 0, kScreenWidth, kScreenHeight, lv_color_hex(0));
    lv_obj_add_flag(v->root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(v->root, [](lv_event_t *event) {
        auto *v = static_cast<Viewer *>(lv_event_get_user_data(event));
        v->cancelled.store(true);
        if (v->timer) lv_timer_delete(v->timer);
        if (v->image) lv_image_cache_drop(&v->descriptor);
        if (v->detail) lv_image_cache_drop(&v->detail_descriptor);
        release(v);
    }, LV_EVENT_DELETE, v);
    v->body = viewer_box(v->root, 0, 72, 320, v->is_text ? 356 : kImageViewHeight, lv_color_hex(0));
    lv_obj_add_flag(v->body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(v->body, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_scroll_dir(v->body, v->is_text ? LV_DIR_VER : LV_DIR_ALL);
    lv_obj_set_scrollbar_mode(v->body, LV_SCROLLBAR_MODE_AUTO);
    v->status = make_label(v->root, v->is_text ? "" : tr(lyra::i18n::StringId::ImageLoading), lv_color_hex(0xFFFFFF));
    lv_obj_set_style_text_color(v->status, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_pos(v->status, 8, 49);
    lv_obj_set_width(v->status, 304);
    lv_label_set_long_mode(v->status, LV_LABEL_LONG_MODE_DOTS);
    control(v, 4, 4, 48, LV_SYMBOL_LEFT, 0);
    control(v, 56, 4, 44, v->is_text ? "A-" : "-", 1);
    control(v, 104, 4, 44, v->is_text ? "A+" : "+", 2);
    control(v, 152, 4, 60, v->is_text ? "B/W" : tr(lyra::i18n::StringId::ImageFit), 3);
    if (v->is_text) {
        control(v, 216, 4, 100, tr(lyra::i18n::StringId::ReaderBookmark), 4);
        control(v, 4, 434, 74, LV_SYMBOL_PREV, 7);
        control(v, 82, 434, 74, LV_SYMBOL_UP, 5);
        control(v, 160, 434, 74, LV_SYMBOL_DOWN, 6);
        control(v, 238, 434, 78, LV_SYMBOL_NEXT, 8);
        v->label = make_label(v->body, "", lv_color_hex(0));
        lv_obj_set_pos(v->label, 10, 4);
        lv_obj_set_width(v->label, 300);
        lv_label_set_long_mode(v->label, LV_LABEL_LONG_MODE_WRAP);
        Bookmark mark;
        const bool saved = read_bookmark(v, &mark);
        if (saved) { v->font_size = mark.font; v->dark = mark.dark; }
        text_page(v, saved ? mark.offset : 0, saved ? mark.scroll : 0);
    } else {
        control(v, 216, 4, 100, "1:1", 9);
        lv_obj_add_event_cb(v->body, [](lv_event_t *event) {
            request_image_detail(static_cast<Viewer *>(lv_event_get_user_data(event)));
        }, LV_EVENT_SCROLL, v);
        v->timer = lv_timer_create(image_poll, 50, v);
        if (!v->timer) { lv_label_set_text(v->status, tr(lyra::i18n::StringId::FileMemoryError)); return; }
        start_image_task(v);
    }
}

void make_document_row(lv_obj_t *parent, int y, const lyra::media::FolderFile &file)
{
    auto *copy = static_cast<lyra::media::FolderFile *>(lv_malloc(sizeof(file)));
    if (!copy) return;
    *copy = file;
    lv_obj_t *row = make_button(parent, 7, y, 306, 54, kSurface, 5, true);
    const char *name = std::strrchr(file.path, '/');
    lv_obj_t *icon = make_label(row, file.kind == lyra::media::FileKind::Image ?
        LV_SYMBOL_IMAGE : LV_SYMBOL_FILE, kAccent);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_t *label = make_label(row, name ? name + 1 : file.path, kTextPrimary);
    make_marquee(label, 250);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 43, 0);
    lv_obj_add_event_cb(row, [](lv_event_t *event) {
        auto *file = static_cast<lyra::media::FolderFile *>(lv_event_get_user_data(event));
        copy_ui_text(s_file_path, sizeof(s_file_path), file->path);
        s_file_kind = file->kind;
        navigate_to(View::FileViewer);
    }, LV_EVENT_CLICKED, copy);
    lv_obj_add_event_cb(row, [](lv_event_t *event) {
        lv_free(lv_event_get_user_data(event));
    }, LV_EVENT_DELETE, copy);
}
} // namespace lyra::gui::internal
