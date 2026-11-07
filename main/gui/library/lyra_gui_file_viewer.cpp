/* SPDX-License-Identifier: Apache-2.0 */
#include "../lyra_gui_internal.h"
#include "lyra_image_file.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <new>
#include <sys/stat.h>
#include <unistd.h>

namespace lyra::gui::internal {
namespace {
constexpr size_t kTextPageBytes = 2048;
// Photos use the whole screen; the controls float above them.
constexpr uint16_t kImageViewHeight = kScreenHeight;
constexpr uint32_t kNativeZoom = 65536;
constexpr int64_t kDetailDelayUs = 50000;
constexpr uint16_t kDetailPatchSize = 512;
char s_file_path[lyra::media::kMaxPath];
lyra::media::FileKind s_file_kind;
View s_file_back_view = View::Folders;
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
    lv_obj_t *root{}, *body{}, *label{}, *image{}, *detail{}, *canvas{}, *status{}, *bookmark_menu{};
    lv_obj_t *top_bar{}, *bottom_bar{}, *title{}, *subtitle{}, *toast{}, *toast_label{};
    lv_obj_t *progress_track{}, *progress_fill{}, *previous_button{}, *next_button{};
    lv_obj_t *message{}, *spinner{}, *zoom_label{};
    lv_obj_t *buttons[8]{}, *button_labels[8]{};
    unsigned button_count{};
    lv_point_t drag_point{}, press_point{};
    bool dragging{}, moved{}, chrome_hidden{};
    lv_timer_t *timer{}, *toast_timer{};
    uint16_t *pixels{}, *detail_pixels{}, *work_pixels{};
    lv_image_dsc_t descriptor{}, detail_descriptor{};
    lyra::media::ImageFileInfo file_info{};
    lyra::media::ImageViewport work_view{};
    lyra::media::ImageViewport detail_view{};
    uint32_t work_revision{}, applied_revision{};
    uint32_t zoom{kNativeZoom}, fit_zoom{kNativeZoom};
    int64_t changed_at_us{};
    uint16_t preview_width{}, preview_height{};
    char text[kTextPageBytes + 1]{};
    uint32_t offset{}, next_offset{}, file_size{};
    uint32_t previous_offsets[32]{};
    unsigned previous_count{};
    uint8_t font_size{};
    // Reader page colour: 0 light, 1 dark, 2 sepia (persisted in bookmarks).
    uint8_t dark{};
    bool is_text{}, loaded{}, decoding_started{}, work_initial{}, work_ok{}, changing_scale{};
};

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

std::atomic<bool> s_image_task_caps{false};

void image_task(void *argument)
{
    auto *v = static_cast<Viewer *>(argument);
    const bool psram_stack = s_image_task_caps.load(); // Set before this task was created.
    const lyra::media::ImageReadControl control{&v->cancelled, &v->revision, v->work_revision};
    const bool access = lyra::media::begin_file_access();
    bool ok = access && !control.cancelled();
    if (!access) ESP_LOGW(kTag, "image viewer: MicroSD access unavailable");
    if (ok && v->work_initial) {
        ok = lyra::media::image_file_info(v->path, &v->file_info, control);
        if (!ok && !control.cancelled()) {
            ESP_LOGW(kTag, "image viewer: cannot read header of %s", v->path);
        }
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
        const size_t bytes = size_t(v->work_view.width) * v->work_view.height * 2;
        v->work_pixels = static_cast<uint16_t *>(heap_caps_malloc(
            bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!v->work_pixels) {
            ESP_LOGW(kTag, "image viewer: no PSRAM for %u bytes (largest free %u)",
                     static_cast<unsigned>(bytes), static_cast<unsigned>(
                         heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
        }
        ok = v->work_pixels && lyra::media::decode_image_file_viewport(v->path,
            v->file_info, v->work_view, v->work_pixels, control);
        if (v->work_pixels && !ok && !control.cancelled()) {
            ESP_LOGW(kTag, "image viewer: decode failed for %s (%ux%u view %ux%u)", v->path,
                     static_cast<unsigned>(v->file_info.width), static_cast<unsigned>(v->file_info.height),
                     static_cast<unsigned>(v->work_view.width), static_cast<unsigned>(v->work_view.height));
        }
    }
    if (access) lyra::media::end_file_access();
    v->work_ok = ok;
    v->complete.store(true, std::memory_order_release);
    s_image_busy.store(false);
    release(v);
    // A PSRAM-stack task must free its static stack through the caps API.
    if (psram_stack) vTaskDeleteWithCaps(nullptr);
    else vTaskDelete(nullptr);
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
            mark->font < 3 && mark->dark < 3) return true;
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

void viewer_action(lv_event_t *event);
bool text_page(Viewer *v, uint32_t offset, uint32_t scroll = 0);
void save_bookmark(Viewer *v);
void remove_bookmark(Viewer *v);
void request_image_detail(Viewer *v);
uint32_t scaled_dimension(uint32_t source, uint32_t zoom);

// ---- Shared chrome ------------------------------------------------------------
// The viewers draw their own surfaces instead of theme widgets: photos need a
// neutral black surround and the reader keeps its own page colours.

constexpr int kBarHeight = 56;
constexpr int kReaderBottomHeight = 64;
constexpr int kRoundButton = 40;
constexpr uint32_t kToastMs = 2200;
constexpr uint32_t kHintMs = 3200;

struct ReaderPalette {
    uint32_t page, surface, pressed, text, muted, divider;
};

// Indices match the persisted bookmark value: 0 light, 1 dark, 2 sepia.
constexpr ReaderPalette kReaderPalettes[] = {
    {0xFBFAF7, 0xEEECE7, 0xDEDBD3, 0x1E1E1E, 0x6E6A63, 0xD9D5CC},
    {0x121315, 0x212328, 0x33363D, 0xE8E8E8, 0x9A9CA1, 0x2E3036},
    {0xF4ECD8, 0xE9DDC2, 0xDCCDAC, 0x4A3A2A, 0x87725C, 0xD8C9A8},
};
constexpr unsigned kReaderThemeCount = 3;
// Sheet order: light, sepia, dark.
constexpr uint8_t kReaderThemeOrder[] = {0, 2, 1};

const ReaderPalette &reader_palette(const Viewer *v)
{
    return kReaderPalettes[v->dark < kReaderThemeCount ? v->dark : 0];
}

void set_round(lv_obj_t *object, int radius)
{
    lv_obj_set_style_radius(object, radius, 0);
}

lv_obj_t *centered_label(lv_obj_t *parent, const char *text, lv_color_t color)
{
    lv_obj_t *label = make_label(parent, text, color);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_center(label);
    return label;
}

void add_chrome_button(Viewer *v, lv_obj_t *button, lv_obj_t *label)
{
    if (v->button_count >= sizeof(v->buttons) / sizeof(v->buttons[0])) return;
    v->buttons[v->button_count] = button;
    v->button_labels[v->button_count] = label;
    ++v->button_count;
}

// A round or pill-shaped control that runs viewer_action().
lv_obj_t *chrome_button(Viewer *v, lv_obj_t *parent, int x, int y, int width, int height,
                        const char *text, unsigned action)
{
    lv_obj_t *button = viewer_box(parent, x, y, width, height, lv_color_hex(0xFFFFFF));
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    set_round(button, height / 2);
    lv_obj_t *label = centered_label(button, text, lv_color_hex(0xFFFFFF));
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_max_width(label, width - 8, 0);
    lv_obj_set_user_data(button, reinterpret_cast<void *>(uintptr_t(action)));
    lv_obj_add_event_cb(button, viewer_action, LV_EVENT_CLICKED, v);
    add_chrome_button(v, button, label);
    return button;
}

// Photo chrome floats over the picture as translucent glass.
void style_photo_button(lv_obj_t *button)
{
    lv_obj_set_style_bg_color(button, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_20, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_40, LV_STATE_PRESSED);
}

void toast_hide_cb(lv_timer_t *timer)
{
    auto *v = static_cast<Viewer *>(lv_timer_get_user_data(timer));
    v->toast_timer = nullptr;  // One-shot timers delete themselves.
    if (v->toast) lv_obj_add_flag(v->toast, LV_OBJ_FLAG_HIDDEN);
}

void viewer_toast(Viewer *v, const char *text, uint32_t duration_ms = kToastMs)
{
    if (!v->toast || !text) return;
    lv_label_set_text(v->toast_label, text);
    lv_obj_remove_flag(v->toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(v->toast);
    if (v->toast_timer) lv_timer_delete(v->toast_timer);
    v->toast_timer = lv_timer_create(toast_hide_cb, duration_ms, v);
    if (v->toast_timer) lv_timer_set_repeat_count(v->toast_timer, 1);
}

void make_toast(Viewer *v, int bottom_gap)
{
    v->toast = viewer_box(v->root, 0, 0, 10, 10, lv_color_hex(0x000000));
    lv_obj_set_size(v->toast, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(v->toast, LV_OPA_80, 0);
    lv_obj_set_style_pad_hor(v->toast, 16, 0);
    lv_obj_set_style_pad_ver(v->toast, 10, 0);
    set_round(v->toast, 18);
    v->toast_label = make_label(v->toast, "", lv_color_hex(0xFFFFFF));
    lv_obj_set_style_text_color(v->toast_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(v->toast_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_max_width(v->toast_label, 264, 0);
    lv_label_set_long_mode(v->toast_label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(v->toast, LV_ALIGN_BOTTOM_MID, 0, -bottom_gap);
    lv_obj_remove_flag(v->toast, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(v->toast, LV_OBJ_FLAG_HIDDEN);
}

const char *file_name(const char *path)
{
    const char *slash = std::strrchr(path, '/');
    return slash ? slash + 1 : path;
}

// Title and subtitle in a top bar, with room for a back button on the left
// and one control on the right.
void make_title(Viewer *v, lv_obj_t *bar, lv_color_t title_color, lv_color_t subtitle_color)
{
    v->title = make_label(bar, file_name(v->path), title_color);
    lv_obj_set_style_text_color(v->title, title_color, 0);
    make_marquee(v->title, 196, MarqueeMode::Focus);
    lv_obj_set_pos(v->title, 60, 9);
    v->subtitle = make_label(bar, "", subtitle_color);
    lv_obj_set_style_text_color(v->subtitle, subtitle_color, 0);
    lv_obj_set_width(v->subtitle, 196);
    lv_label_set_long_mode(v->subtitle, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(v->subtitle, 60, 31);
}

// ---- Reader ------------------------------------------------------------------

void reader_style(Viewer *v)
{
    const ReaderPalette &palette = reader_palette(v);
    const lv_color_t page = lv_color_hex(palette.page);
    const lv_color_t surface = lv_color_hex(palette.surface);
    const lv_color_t text = lv_color_hex(palette.text);
    const lv_color_t muted = lv_color_hex(palette.muted);
    lv_obj_set_style_bg_color(v->root, page, 0);
    lv_obj_set_style_bg_color(v->body, page, 0);
    for (lv_obj_t *bar : {v->top_bar, v->bottom_bar}) {
        lv_obj_set_style_bg_color(bar, page, 0);
        lv_obj_set_style_border_color(bar, lv_color_hex(palette.divider), 0);
    }
    lv_obj_set_style_text_color(v->label, text, 0);
    lv_obj_set_style_text_font(v->label, reader_font(v->font_size), 0);
    // Generous leading reads more like a page than a log file.
    lv_obj_set_style_text_line_space(v->label, v->font_size == 2 ? 6 : 5, 0);
    lv_obj_set_style_text_color(v->title, text, 0);
    lv_obj_set_style_text_color(v->subtitle, muted, 0);
    for (unsigned i = 0; i < v->button_count; ++i) {
        lv_obj_set_style_bg_color(v->buttons[i], surface, 0);
        lv_obj_set_style_bg_opa(v->buttons[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(v->buttons[i], lv_color_hex(palette.pressed), LV_STATE_PRESSED);
        lv_obj_set_style_text_color(v->button_labels[i], text, 0);
    }
    lv_obj_set_style_bg_color(v->progress_track, lv_color_hex(palette.divider), 0);
    lv_obj_update_layout(v->body);
}

void update_reader_progress(Viewer *v)
{
    if (!v->progress_fill) return;
    const int32_t scroll = std::max<int32_t>(0, lv_obj_get_scroll_y(v->body));
    const int32_t range = scroll + std::max<int32_t>(0, lv_obj_get_scroll_bottom(v->body));
    const uint64_t page_bytes = v->next_offset > v->offset ? v->next_offset - v->offset : 0;
    const uint64_t position = v->offset + (range > 0 ? page_bytes * scroll / range : page_bytes);
    const unsigned percent = v->file_size ?
        unsigned(std::min<uint64_t>(100, position * 100 / v->file_size)) : 100;
    const int track = lv_obj_get_width(v->progress_track);
    lv_obj_set_width(v->progress_fill, std::max(6, track * int(percent) / 100));
    char text[24];
    format_u32(lyra::i18n::StringId::ReaderProgress, percent, text, sizeof(text));
    set_label_text_if_changed(v->subtitle, text);
    // Page buttons fade when there is no page in that direction.
    lv_obj_set_style_opa(v->previous_button, v->offset ? LV_OPA_COVER : LV_OPA_40, 0);
    lv_obj_set_style_opa(v->next_button, v->next_offset < v->file_size ? LV_OPA_COVER : LV_OPA_40, 0);
}

void close_reader_options(Viewer *v)
{
    if (!v->bookmark_menu) return;
    lv_obj_t *sheet = v->bookmark_menu;
    v->bookmark_menu = nullptr;
    // Called from the sheet's own buttons, so delete after the event ends.
    lv_obj_delete_async(sheet);
}

void show_reader_options(Viewer *v);

void reader_option_cb(lv_event_t *event)
{
    auto *v = static_cast<Viewer *>(lv_event_get_user_data(event));
    const uintptr_t choice = reinterpret_cast<uintptr_t>(
        lv_obj_get_user_data(lv_event_get_current_target_obj(event)));
    const unsigned group = choice >> 8;
    const unsigned value = choice & 0xFF;
    if (group == 0) {
        v->font_size = uint8_t(value);
        reader_style(v);
        update_reader_progress(v);
        show_reader_options(v);  // Redraw the selection.
        return;
    }
    if (group == 1) {
        v->dark = uint8_t(value);
        reader_style(v);
        show_reader_options(v);
        return;
    }
    close_reader_options(v);
    if (value == 0) {
        save_bookmark(v);
    } else if (value == 1) {
        Bookmark mark;
        if (read_bookmark(v, &mark)) {
            v->font_size = mark.font;
            v->dark = mark.dark;
            if (text_page(v, mark.offset, mark.scroll)) v->previous_count = 0;
        } else {
            viewer_toast(v, tr(lyra::i18n::StringId::FileReadFailed));
        }
    } else if (value == 2) {
        remove_bookmark(v);
    }
}

lv_obj_t *option_button(Viewer *v, lv_obj_t *parent, int x, int y, int width, int height,
                        uint32_t fill, bool selected, unsigned group, unsigned value)
{
    lv_obj_t *button = viewer_box(parent, x, y, width, height, lv_color_hex(fill));
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    set_round(button, 10);
    lv_obj_set_style_border_width(button, selected ? 2 : 1, 0);
    lv_obj_set_style_border_color(button, selected ? kAccent :
                                  lv_color_hex(reader_palette(v).divider), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(reader_palette(v).pressed), LV_STATE_PRESSED);
    lv_obj_set_user_data(button, reinterpret_cast<void *>(uintptr_t((group << 8) | value)));
    lv_obj_add_event_cb(button, reader_option_cb, LV_EVENT_CLICKED, v);
    return button;
}

lv_obj_t *section_label(lv_obj_t *parent, int y, lyra::i18n::StringId id, lv_color_t color)
{
    lv_obj_t *label = make_label(parent, tr(id), color);
    lv_obj_set_style_text_color(label, color, 0);
    make_marquee(label, 280);
    lv_obj_set_pos(label, 20, y);
    return label;
}

// The "Aa" sheet: text size, page colour and bookmarks.
void show_reader_options(Viewer *v)
{
    close_reader_options(v);
    Bookmark mark;
    const bool saved = read_bookmark(v, &mark);
    const ReaderPalette &palette = reader_palette(v);
    const lv_color_t text = lv_color_hex(palette.text);
    const lv_color_t muted = lv_color_hex(palette.muted);

    lv_obj_t *scrim = viewer_box(v->root, 0, 0, kScreenWidth, kScreenHeight, lv_color_hex(0));
    lv_obj_set_style_bg_opa(scrim, LV_OPA_40, 0);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scrim, [](lv_event_t *event) {
        if (lv_event_get_target_obj(event) != lv_event_get_current_target_obj(event)) return;
        close_reader_options(static_cast<Viewer *>(lv_event_get_user_data(event)));
    }, LV_EVENT_CLICKED, v);
    v->bookmark_menu = scrim;

    constexpr int kSheetHeight = 326;
    // Extra height hides the bottom corners below the screen edge.
    lv_obj_t *sheet = viewer_box(scrim, 0, kScreenHeight - kSheetHeight, kScreenWidth,
                                 kSheetHeight + 16, lv_color_hex(palette.page));
    set_round(sheet, 16);
    lv_obj_add_flag(sheet, LV_OBJ_FLAG_CLICKABLE);  // Taps on the sheet stay here.
    lv_obj_t *handle = viewer_box(sheet, (kScreenWidth - 36) / 2, 8, 36, 4,
                                  lv_color_hex(palette.divider));
    set_round(handle, 2);

    lv_obj_t *heading = make_label(sheet, tr(lyra::i18n::StringId::ReaderOptions), text);
    lv_obj_set_style_text_color(heading, text, 0);
    make_marquee(heading, 220);
    lv_obj_set_pos(heading, 20, 24);
    lv_obj_t *close = viewer_box(sheet, kScreenWidth - 20 - 36, 18, 36, 36,
                                 lv_color_hex(palette.surface));
    set_round(close, 18);
    lv_obj_add_flag(close, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(close, lv_color_hex(palette.pressed), LV_STATE_PRESSED);
    centered_label(close, LV_SYMBOL_CLOSE, text);
    lv_obj_add_event_cb(close, [](lv_event_t *event) {
        close_reader_options(static_cast<Viewer *>(lv_event_get_user_data(event)));
    }, LV_EVENT_CLICKED, v);

    section_label(sheet, 70, lyra::i18n::StringId::ReaderTextSize, muted);
    for (unsigned size = 0; size < 3; ++size) {
        lv_obj_t *button = option_button(v, sheet, 20 + int(size) * 96, 94, 88, 48,
                                         palette.surface, v->font_size == size, 0, size);
        lv_obj_t *sample = centered_label(button, tr(lyra::i18n::StringId::ReaderSample), text);
        lv_obj_set_style_text_font(sample, reader_font(size), 0);
    }

    section_label(sheet, 154, lyra::i18n::StringId::Theme, muted);
    constexpr lyra::i18n::StringId kThemeNames[] = {
        lyra::i18n::StringId::ReaderThemeLight, lyra::i18n::StringId::ReaderThemeDark,
        lyra::i18n::StringId::ReaderThemeSepia,
    };
    for (unsigned slot = 0; slot < kReaderThemeCount; ++slot) {
        const uint8_t theme = kReaderThemeOrder[slot];
        const ReaderPalette &swatch = kReaderPalettes[theme];
        lv_obj_t *button = option_button(v, sheet, 20 + int(slot) * 96, 178, 88, 48,
                                         swatch.page, v->dark == theme, 1, theme);
        lv_obj_set_style_bg_color(button, lv_color_hex(swatch.surface), LV_STATE_PRESSED);
        lv_obj_t *name = centered_label(button, tr(kThemeNames[theme]), lv_color_hex(swatch.text));
        lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(name, 80);
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(name);
    }

    section_label(sheet, 238, lyra::i18n::StringId::ReaderBookmark, muted);
    const struct {
        int x, width;
        const char *text;
        bool enabled;
    } actions[] = {
        {20, 112, tr(lyra::i18n::StringId::SaveKey), true},
        {136, 112, tr(lyra::i18n::StringId::ReaderBookmarkJump), saved},
        {252, 48, LV_SYMBOL_TRASH, saved},
    };
    for (unsigned action = 0; action < 3; ++action) {
        lv_obj_t *button = option_button(v, sheet, actions[action].x, 262, actions[action].width,
                                         44, palette.surface, false, 2, action);
        lv_obj_t *label = centered_label(button, actions[action].text,
                                         action == 0 ? kAccent : text);
        make_marquee(label, actions[action].width - 12);
        lv_obj_center(label);
        if (!actions[action].enabled) {
            lv_obj_add_state(button, LV_STATE_DISABLED);
            lv_obj_remove_flag(button, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_opa(button, LV_OPA_40, 0);
        }
    }
}

// ---- Photos ------------------------------------------------------------------

void image_message(Viewer *v, const char *text, bool busy)
{
    if (!v->message) return;
    lv_obj_remove_flag(v->message, LV_OBJ_FLAG_HIDDEN);
    if (v->spinner) {
        if (busy) lv_obj_remove_flag(v->spinner, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(v->spinner, LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(v->status, text);
    lv_obj_align(v->status, LV_ALIGN_TOP_MID, 0, busy ? 64 : 0);
}

void hide_image_message(Viewer *v)
{
    if (v->message) lv_obj_add_flag(v->message, LV_OBJ_FLAG_HIDDEN);
}

void update_zoom_label(Viewer *v)
{
    if (!v->zoom_label) return;
    char text[24];
    format_u32(lyra::i18n::StringId::ReaderProgress,
               uint32_t((uint64_t(v->zoom) * 100 + kNativeZoom / 2) / kNativeZoom),
               text, sizeof(text));
    set_label_text_if_changed(v->zoom_label, text);
}

void set_photo_chrome(Viewer *v, bool visible)
{
    v->chrome_hidden = !visible;
    for (lv_obj_t *object : {v->top_bar, v->bottom_bar}) {
        if (!object) continue;
        if (visible) lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    }
    if (!visible && v->toast) lv_obj_add_flag(v->toast, LV_OBJ_FLAG_HIDDEN);
}

bool text_page(Viewer *v, uint32_t offset, uint32_t scroll)
{
    FileAccess access;
    if (!access.ready) { viewer_toast(v, tr(lyra::i18n::StringId::NoMicroSdCard)); return false; }
    FILE *file = lyra::sd::open(v->path, "rb", lyra::sd::Client::Filesystem);
    if (!file) { viewer_toast(v, tr(lyra::i18n::StringId::FileOpenFailed)); return false; }
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
    if (failed) { viewer_toast(v, tr(lyra::i18n::StringId::FileReadFailed)); return false; }
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
    reader_style(v);
    lv_obj_scroll_to_y(v->body, scroll, LV_ANIM_OFF);
    update_reader_progress(v);
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
    if (!access.ready) { viewer_toast(v, tr(lyra::i18n::StringId::NoMicroSdCard)); return; }
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
    viewer_toast(v, ok ? tr(lyra::i18n::StringId::ReaderBookmarkSaved) : tr(lyra::i18n::StringId::ReaderBookmarkFailed));
}

void remove_bookmark(Viewer *v)
{
    FileAccess access;
    if (!access.ready) { viewer_toast(v, tr(lyra::i18n::StringId::NoMicroSdCard)); return; }
    char path[100], backup[108], temp[108];
    bookmark_path(v, path, sizeof(path));
    std::snprintf(backup, sizeof(backup), "%s.bak", path);
    std::snprintf(temp, sizeof(temp), "%s.tmp", path);
    // Remove the recovery copy first so a deleted bookmark cannot reappear.
    const auto remove_if_present = [](const char *file) {
        errno = 0;
        return lyra::sd::remove(file, lyra::sd::Client::Filesystem) == 0 || errno == ENOENT;
    };
    const bool ok = remove_if_present(backup) && remove_if_present(path) && remove_if_present(temp);
    viewer_toast(v, tr(ok ? lyra::i18n::StringId::ReaderBookmarkRemoved :
                                lyra::i18n::StringId::ReaderBookmarkRemoveFailed));
}

uint32_t scaled_dimension(uint32_t source, uint32_t zoom)
{
    return std::max<uint64_t>(1, uint64_t(source) * zoom / kNativeZoom);
}

bool image_view_contains(const lyra::media::ImageViewport &patch, uint32_t x, uint32_t y)
{
    return patch.width && patch.height && x >= patch.x && y >= patch.y &&
        uint64_t(x) + kScreenWidth <= uint64_t(patch.x) + patch.width &&
        uint64_t(y) + kImageViewHeight <= uint64_t(patch.y) + patch.height;
}

lyra::media::ImageViewport image_detail_view(Viewer *v)
{
    const uint32_t width = scaled_dimension(v->file_info.width, v->zoom);
    const uint32_t height = scaled_dimension(v->file_info.height, v->zoom);
    const uint32_t x = std::max<int32_t>(0, lv_obj_get_scroll_x(v->body));
    const uint32_t y = std::max<int32_t>(0, lv_obj_get_scroll_y(v->body));
    const uint16_t patch_width = std::min<uint32_t>(kDetailPatchSize, std::max<uint32_t>(kScreenWidth, width));
    const uint16_t patch_height = std::min<uint32_t>(kDetailPatchSize, std::max<uint32_t>(kImageViewHeight, height));
    const uint32_t patch_x = std::min<uint32_t>(
        x > (patch_width - kScreenWidth) / 2 ? x - (patch_width - kScreenWidth) / 2 : 0,
        width > patch_width ? width - patch_width : 0);
    const uint32_t patch_y = std::min<uint32_t>(
        y > (patch_height - kImageViewHeight) / 2 ? y - (patch_height - kImageViewHeight) / 2 : 0,
        height > patch_height ? height - patch_height : 0);
    return {width, height, patch_x, patch_y, patch_width, patch_height};
}

void request_image_detail(Viewer *v)
{
    if (!v->loaded || v->changing_scale) return;
    const uint32_t x = std::max<int32_t>(0, lv_obj_get_scroll_x(v->body));
    const uint32_t y = std::max<int32_t>(0, lv_obj_get_scroll_y(v->body));
    if (v->detail && !lv_obj_has_flag(v->detail, LV_OBJ_FLAG_HIDDEN) &&
        image_view_contains(v->detail_view, x, y)) {
        // A completed patch already covers the screen. Cancel any pending
        // request for an area the user has panned away from.
        if (v->revision.load() != v->applied_revision) {
            v->applied_revision = v->revision.fetch_add(1) + 1;
        }
        return;
    }
    if (v->decoding_started && v->revision.load() == v->work_revision &&
        image_view_contains(v->work_view, x, y)) return;
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
    v->detail_view = {};
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
    update_zoom_label(v);
    request_image_detail(v);
}

void viewer_action(lv_event_t *event)
{
    auto *v = static_cast<Viewer *>(lv_event_get_user_data(event));
    const auto action = lv_obj_get_user_data(lv_event_get_current_target_obj(event));
    switch (reinterpret_cast<uintptr_t>(action)) {
        case 0: navigate_back(s_file_back_view); return;
        case 1: image_scale(v, std::max(std::min(v->fit_zoom, kNativeZoom), v->zoom / 2)); break;
        case 2: image_scale(v, std::min(std::max(v->fit_zoom, kNativeZoom * 4), v->zoom * 2)); break;
        case 3: image_scale(v, v->fit_zoom, true); break;
        case 4: show_reader_options(v); break;
        case 7: text_previous(v); break;
        case 8: text_next(v); break;
        case 9: image_scale(v, kNativeZoom); break;
    }
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
        v->work_view = image_detail_view(v);
    }
    v->references.fetch_add(1);
    // Prefer an internal stack for decode speed; a fragmented internal heap
    // must not make every image fail, so fall back to a PSRAM stack.
    s_image_task_caps.store(false);
    bool started = xTaskCreatePinnedToCore(image_task, "file_image", 24 * 1024, v, 1, nullptr, 1) == pdPASS;
    if (!started) {
        s_image_task_caps.store(true);
        ESP_LOGW(kTag, "image viewer: internal stack unavailable (largest free %u); using PSRAM",
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
        started = xTaskCreatePinnedToCoreWithCaps(image_task, "file_image", 24 * 1024, v, 1, nullptr, 1,
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS;
    }
    if (!started) {
        ESP_LOGE(kTag, "image viewer: cannot start the decode task");
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
                v->detail_view = v->work_view;
                image_descriptor(&v->detail_descriptor, v->detail_pixels,
                    v->detail_view.width, v->detail_view.height);
                if (!v->detail) v->detail = lv_image_create(v->canvas);
                lv_image_set_src(v->detail, &v->detail_descriptor);
                lv_obj_set_pos(v->detail, v->work_view.x, v->work_view.y);
                lv_obj_remove_flag(v->detail, LV_OBJ_FLAG_HIDDEN);
                v->applied_revision = v->work_revision;
            }
            if (v->work_initial) {
                hide_image_message(v);
                char size[40];
                std::snprintf(size, sizeof(size), "%u x %u",
                              static_cast<unsigned>(v->file_info.width),
                              static_cast<unsigned>(v->file_info.height));
                lv_label_set_text(v->subtitle, size);
                viewer_toast(v, tr(lyra::i18n::StringId::ImageViewerHint), kHintMs);
            }
        } else if (current) {
            v->applied_revision = v->work_revision;
            if (!v->work_initial) {
                viewer_toast(v, tr(lyra::i18n::StringId::ImageDecodeFailed));
            } else {
                image_message(v, tr(lyra::i18n::StringId::ImageDecodeFailed), false);
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
        if (!v->loaded) image_message(v, tr(lyra::i18n::StringId::ImageLoading), true);
        start_image_task(v);
    }
}

void reader_bars(Viewer *v)
{
    v->top_bar = viewer_box(v->root, 0, 0, kScreenWidth, kBarHeight, lv_color_hex(0xFFFFFF));
    lv_obj_set_style_border_side(v->top_bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(v->top_bar, 1, 0);
    chrome_button(v, v->top_bar, 10, 8, kRoundButton, kRoundButton, LV_SYMBOL_LEFT, 0);
    make_title(v, v->top_bar, lv_color_hex(0), lv_color_hex(0));
    chrome_button(v, v->top_bar, kScreenWidth - 10 - 52, 8, 52, kRoundButton,
                  tr(lyra::i18n::StringId::ReaderSample), 4);

    v->bottom_bar = viewer_box(v->root, 0, kScreenHeight - kReaderBottomHeight, kScreenWidth,
                               kReaderBottomHeight, lv_color_hex(0xFFFFFF));
    lv_obj_set_style_border_side(v->bottom_bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(v->bottom_bar, 1, 0);
    v->previous_button = chrome_button(v, v->bottom_bar, 12, 10, 44, 44, LV_SYMBOL_LEFT, 7);
    v->next_button = chrome_button(v, v->bottom_bar, kScreenWidth - 12 - 44, 10, 44, 44,
                                   LV_SYMBOL_RIGHT, 8);
    constexpr int kTrackX = 72;
    v->progress_track = viewer_box(v->bottom_bar, kTrackX, 30, kScreenWidth - kTrackX * 2, 4,
                                   lv_color_hex(0xDDDDDD));
    set_round(v->progress_track, 2);
    v->progress_fill = viewer_box(v->progress_track, 0, 0, 6, 4, kAccent);
    set_round(v->progress_fill, 2);
}

void photo_bars(Viewer *v)
{
    // Translucent glass bars; tapping the photo hides them.
    v->top_bar = viewer_box(v->root, 0, 0, kScreenWidth, kBarHeight, lv_color_hex(0));
    lv_obj_set_style_bg_opa(v->top_bar, LV_OPA_60, 0);
    style_photo_button(chrome_button(v, v->top_bar, 10, 8, kRoundButton, kRoundButton,
                                     LV_SYMBOL_LEFT, 0));
    make_title(v, v->top_bar, lv_color_hex(0xFFFFFF), lv_color_hex(0xB8B8B8));
    style_photo_button(chrome_button(v, v->top_bar, kScreenWidth - 10 - 52, 8, 52,
                                     kRoundButton, "1:1", 9));

    constexpr int kPillWidth = 236;
    constexpr int kPillHeight = 52;
    v->bottom_bar = viewer_box(v->root, (kScreenWidth - kPillWidth) / 2,
                               kScreenHeight - kPillHeight - 16, kPillWidth, kPillHeight,
                               lv_color_hex(0));
    lv_obj_set_style_bg_opa(v->bottom_bar, LV_OPA_60, 0);
    set_round(v->bottom_bar, kPillHeight / 2);
    style_photo_button(chrome_button(v, v->bottom_bar, 6, 6, 40, 40, LV_SYMBOL_MINUS, 1));
    v->zoom_label = make_label(v->bottom_bar, "", lv_color_hex(0xFFFFFF));
    lv_obj_set_style_text_color(v->zoom_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_width(v->zoom_label, 62);
    lv_obj_set_style_text_align(v->zoom_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(v->zoom_label, 50, 16);
    style_photo_button(chrome_button(v, v->bottom_bar, 116, 6, 40, 40, LV_SYMBOL_PLUS, 2));
    style_photo_button(chrome_button(v, v->bottom_bar, 162, 6, 68, 40,
                                     tr(lyra::i18n::StringId::ImageFit), 3));

    // Centred loading and error state.
    v->message = viewer_box(v->root, 20, kScreenHeight / 2 - 60, kScreenWidth - 40, 120,
                            lv_color_hex(0));
    lv_obj_set_style_bg_opa(v->message, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(v->message, LV_OBJ_FLAG_CLICKABLE);
    v->spinner = lv_spinner_create(v->message);
    lv_obj_set_size(v->spinner, 44, 44);
    lv_obj_align(v->spinner, LV_ALIGN_TOP_MID, 0, 6);
    lv_spinner_set_anim_params(v->spinner, 900, 240);
    lv_obj_set_style_arc_width(v->spinner, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_width(v->spinner, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(v->spinner, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_arc_color(v->spinner, kAccent, LV_PART_INDICATOR);
    v->status = make_label(v->message, "", lv_color_hex(0xD0D0D0));
    lv_obj_set_style_text_color(v->status, lv_color_hex(0xD0D0D0), 0);
    lv_obj_set_width(v->status, kScreenWidth - 40);
    lv_obj_set_style_text_align(v->status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(v->status, LV_LABEL_LONG_MODE_WRAP);
}

void photo_touch_cb(lv_event_t *event)
{
    auto *v = static_cast<Viewer *>(lv_event_get_user_data(event));
    const lv_event_code_t code = lv_event_get_code(event);
    lv_indev_t *indev = lv_indev_active();
    lv_point_t point{};
    if (indev) lv_indev_get_point(indev, &point);
    if (code == LV_EVENT_PRESSED) {
        v->press_point = point;
        v->drag_point = point;
        v->moved = false;
        v->dragging = v->loaded;
        return;
    }
    if (code == LV_EVENT_PRESSING) {
        if (std::abs(point.x - v->press_point.x) > 8 || std::abs(point.y - v->press_point.y) > 8) {
            v->moved = true;
        }
        if (!v->dragging) return;
        // LVGL's drag scroll picks one axis; photos pan on both.
        const int dx = point.x - v->drag_point.x;
        const int dy = point.y - v->drag_point.y;
        v->drag_point = point;
        const int max_x = std::max<int>(0, int(scaled_dimension(v->file_info.width, v->zoom)) - int(kScreenWidth));
        const int max_y = std::max<int>(0, int(scaled_dimension(v->file_info.height, v->zoom)) - int(kImageViewHeight));
        lv_obj_scroll_to(v->body, std::clamp<int>(lv_obj_get_scroll_x(v->body) - dx, 0, max_x),
            std::clamp<int>(lv_obj_get_scroll_y(v->body) - dy, 0, max_y), LV_ANIM_OFF);
        return;
    }
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        v->dragging = false;
        // A tap without movement shows or hides the controls.
        if (code == LV_EVENT_RELEASED && !v->moved) set_photo_chrome(v, v->chrome_hidden);
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
        if (v->toast_timer) lv_timer_delete(v->toast_timer);
        v->timer = nullptr;
        v->toast_timer = nullptr;
        if (v->image) lv_image_cache_drop(&v->descriptor);
        if (v->detail) lv_image_cache_drop(&v->detail_descriptor);
        v->bookmark_menu = nullptr;  // Deleted with the root.
        v->toast = nullptr;
        release(v);
    }, LV_EVENT_DELETE, v);

    if (v->is_text) {
        Bookmark mark;
        const bool saved = read_bookmark(v, &mark);
        if (saved) { v->font_size = mark.font; v->dark = mark.dark; }
        v->body = viewer_box(v->root, 0, kBarHeight, kScreenWidth,
                             kScreenHeight - kBarHeight - kReaderBottomHeight, lv_color_hex(0xFFFFFF));
        lv_obj_add_flag(v->body, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(v->body, LV_OBJ_FLAG_SCROLL_MOMENTUM);
        lv_obj_set_scroll_dir(v->body, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(v->body, LV_SCROLLBAR_MODE_ACTIVE);
        lv_obj_set_style_width(v->body, 3, LV_PART_SCROLLBAR);
        lv_obj_set_style_bg_color(v->body, kAccent, LV_PART_SCROLLBAR);
        lv_obj_set_style_bg_opa(v->body, LV_OPA_60, LV_PART_SCROLLBAR);
        lv_obj_set_style_pad_top(v->body, 14, 0);
        lv_obj_set_style_pad_bottom(v->body, 24, 0);
        v->label = make_label(v->body, "", lv_color_hex(0));
        lv_obj_set_pos(v->label, 20, 0);
        lv_obj_set_width(v->label, kScreenWidth - 40);
        lv_label_set_long_mode(v->label, LV_LABEL_LONG_MODE_WRAP);
        reader_bars(v);
        make_toast(v, kReaderBottomHeight + 12);
        lv_obj_add_event_cb(v->body, [](lv_event_t *event) {
            update_reader_progress(static_cast<Viewer *>(lv_event_get_user_data(event)));
        }, LV_EVENT_SCROLL, v);
        reader_style(v);
        text_page(v, saved ? mark.offset : 0, saved ? mark.scroll : 0);
        return;
    }

    v->body = viewer_box(v->root, 0, 0, kScreenWidth, kImageViewHeight, lv_color_hex(0));
    lv_obj_add_flag(v->body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(v->body, LV_DIR_NONE);
    lv_obj_set_scrollbar_mode(v->body, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(v->body, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_clear_flag(v->body, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_clear_flag(v->body, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_flag(v->body, LV_OBJ_FLAG_CLICKABLE);
    photo_bars(v);
    make_toast(v, 84);
    lv_obj_add_event_cb(v->body, [](lv_event_t *event) {
        request_image_detail(static_cast<Viewer *>(lv_event_get_user_data(event)));
    }, LV_EVENT_SCROLL, v);
    for (lv_event_code_t code : {LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED,
                                 LV_EVENT_PRESS_LOST}) {
        lv_obj_add_event_cb(v->body, photo_touch_cb, code, v);
    }
    image_message(v, tr(lyra::i18n::StringId::ImageLoading), true);
    v->timer = lv_timer_create(image_poll, 50, v);
    if (!v->timer) { image_message(v, tr(lyra::i18n::StringId::FileMemoryError), false); return; }
    start_image_task(v);
}

void open_document_file(const char *path, lyra::media::FileKind kind, View back_view)
{
    copy_ui_text(s_file_path, sizeof(s_file_path), path);
    s_file_kind = kind;
    s_file_back_view = back_view;
    if (back_view == View::Photos || back_view == View::Ebook) {
        release_app_file_scan(back_view);
    }
    navigate_to(View::FileViewer);
}

View file_viewer_back_view()
{
    return s_file_back_view;
}

namespace {
struct DocumentRowContext {
    lyra::media::FolderFile file;
    View back_view;
};
} // namespace

void make_document_row(lv_obj_t *parent, int y, const lyra::media::FolderFile &file,
                       View back_view)
{
    auto *context = static_cast<DocumentRowContext *>(lv_malloc(sizeof(DocumentRowContext)));
    if (!context) return;
    context->file = file;
    context->back_view = back_view;
    lv_obj_t *row = make_button(parent, 7, y, 306, 54, kSurface, 5, true);
    const char *name = std::strrchr(file.path, '/');
    lv_obj_t *icon = make_label(row, file.kind == lyra::media::FileKind::Image ?
        LV_SYMBOL_IMAGE : LV_SYMBOL_FILE, kAccent);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_t *label = make_label(row, name ? name + 1 : file.path, kTextPrimary);
    make_marquee(label, 250);
    if (back_view == View::Photos || back_view == View::Ebook) {
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 43, -9);
        char parent_path[lyra::media::kMaxPath];
        copy_ui_text(parent_path, sizeof(parent_path), file.path);
        char *slash = std::strrchr(parent_path, '/');
        if (slash && slash != parent_path) *slash = '\0';
        lv_obj_t *location = make_label(row, parent_path, kTextMuted);
        make_marquee(location, 250);
        lv_obj_align(location, LV_ALIGN_LEFT_MID, 43, 12);
    } else {
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 43, 0);
    }
    lv_obj_add_event_cb(row, [](lv_event_t *event) {
        auto *context = static_cast<DocumentRowContext *>(lv_event_get_user_data(event));
        if (context->file.kind == lyra::media::FileKind::Playlist) {
            open_folder_playlist(context->file.path);
        } else {
            open_document_file(context->file.path, context->file.kind, context->back_view);
        }
    }, LV_EVENT_CLICKED, context);
    lv_obj_add_event_cb(row, [](lv_event_t *event) {
        lv_free(lv_event_get_user_data(event));
    }, LV_EVENT_DELETE, context);
}
} // namespace lyra::gui::internal
