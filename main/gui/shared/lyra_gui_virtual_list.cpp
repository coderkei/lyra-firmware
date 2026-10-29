/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../lyra_gui_internal.h"

#include <atomic>
#include <initializer_list>

#include "freertos/semphr.h"

namespace lyra::gui::internal {
namespace {

// Scrolling cost on this panel is dominated by redrawing the visible area,
// which a page already paid. What paging avoided was everything that grows
// with the list: thousands of LVGL objects moved on every scroll step, and
// SD catalog reads on the GUI thread. This list keeps one screen of row slots
// and rebinds them by position; their text comes from a cache that a
// low-priority task fills ahead of the scroll direction.
constexpr size_t kCacheSize = 128;
constexpr size_t kMaxSlots = 16;
constexpr size_t kPrefetchBehind = 16;
constexpr size_t kPrefetchAhead = 48;
constexpr uint32_t kTickMs = 40;
constexpr int64_t kBubbleHoldUs = 700 * 1000;
constexpr int kStripWidth = 28;
constexpr int kStripGap = 3;
constexpr int kJumpCellHeight = 34;
constexpr int kLetterHeight = 17;
constexpr size_t kMaxLetters = 64;

struct CacheEntry {
    size_t position;
    uint32_t generation;
    bool ready;
    VirtualRow row;
};

// Shared with the prefetch task; every field is guarded by s_mutex.
CacheEntry *s_cache;
SemaphoreHandle_t s_mutex;
TaskHandle_t s_worker;
uint32_t s_generation;
VirtualFetch s_request_fetch;
size_t s_request_first;
size_t s_request_last;
size_t s_request_center;
bool s_request_forward = true;
std::atomic<bool> s_rows_ready{false};

struct ListState {
    lv_obj_t *body;
    VirtualListSpec spec;
    lv_obj_t *slots[kMaxSlots];
    size_t slot_position[kMaxSlots];
    bool slot_filled[kMaxSlots];
    size_t slot_count;
    int width;
    size_t last_first;
    bool forward;
    bool needs_update;
    lv_timer_t *timer;
    lv_obj_t *bubble;
    lv_obj_t *bubble_label;
    int64_t bubble_hide_us;
    lv_obj_t *strip;
    int strip_top;
    int strip_height;
    lyra::media::SortLocation *letters;
    size_t letter_count;
    bool letters_pending;
};

ListState s_list{};

struct Lock {
    Lock() { xSemaphoreTake(s_mutex, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(s_mutex); }
    Lock(const Lock &) = delete;
    Lock &operator=(const Lock &) = delete;
};

bool cached_row(size_t position, VirtualRow *row)
{
    Lock lock;
    const CacheEntry &entry = s_cache[position % kCacheSize];
    if (!entry.ready || entry.generation != s_generation || entry.position != position) return false;
    *row = entry.row;
    return true;
}

void store_row(uint32_t generation, size_t position, const VirtualRow &row)
{
    Lock lock;
    if (generation != s_generation) return;
    CacheEntry &entry = s_cache[position % kCacheSize];
    entry.position = position;
    entry.generation = generation;
    entry.ready = true;
    entry.row = row;
}

// Next uncached position in the requested window, nearest first and leaning
// into the direction of travel.
bool next_request(uint32_t *generation, VirtualFetch *fetch, size_t *position)
{
    Lock lock;
    if (!s_request_fetch || s_request_first >= s_request_last) return false;
    const auto missing = [](size_t candidate) {
        const CacheEntry &entry = s_cache[candidate % kCacheSize];
        return !entry.ready || entry.generation != s_generation || entry.position != candidate;
    };
    const size_t center = std::clamp(s_request_center, s_request_first, s_request_last - 1);
    const size_t span = s_request_last - s_request_first;
    for (size_t step = 0; step < span; ++step) {
        const size_t ahead = s_request_forward ? center + step : center - std::min(step, center);
        const size_t behind = s_request_forward ? center - std::min(step, center) : center + step;
        for (size_t candidate : {ahead, behind}) {
            if (candidate < s_request_first || candidate >= s_request_last || !missing(candidate)) continue;
            *generation = s_generation;
            *fetch = s_request_fetch;
            *position = candidate;
            return true;
        }
    }
    return false;
}

void prefetch_task(void *)
{
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        uint32_t generation = 0;
        VirtualFetch fetch = nullptr;
        size_t position = 0;
        while (next_request(&generation, &fetch, &position)) {
            VirtualRow row{};
            // A failed lookup is cached as an empty row so it is not retried
            // in a tight loop; the next catalog change starts a new list.
            if (!fetch(position, &row)) row = {};
            store_row(generation, position, row);
            s_rows_ready.store(true, std::memory_order_release);
        }
    }
}

bool ensure_prefetch()
{
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
    if (!s_cache) {
        s_cache = static_cast<CacheEntry *>(heap_caps_calloc(
            kCacheSize, sizeof(CacheEntry), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    if (s_mutex && s_cache && !s_worker) {
        // Low priority on either core: prefetch uses idle time beside the GUI
        // and below audio, so it never delays a frame or a decode.
        xTaskCreatePinnedToCoreWithCaps(prefetch_task, "lyra_rows", 8 * 1024, nullptr, 2,
                                        &s_worker, tskNO_AFFINITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    return s_mutex && s_cache && s_worker;
}

void request_window(size_t first_visible)
{
    const size_t count = s_list.spec.count;
    const size_t first = s_list.forward ? first_visible - std::min(first_visible, kPrefetchBehind / 2) :
                                          first_visible - std::min(first_visible, kPrefetchAhead);
    const size_t last = std::min(count, s_list.forward ? first_visible + kPrefetchAhead :
                                                         first_visible + kPrefetchBehind + s_list.slot_count);
    {
        Lock lock;
        s_request_fetch = s_list.spec.fetch;
        s_request_first = first;
        s_request_last = last;
        s_request_center = s_list.forward ? first_visible : first_visible + s_list.slot_count - 1;
        s_request_forward = s_list.forward;
    }
    xTaskNotifyGive(s_worker);
}

bool slot_holds_input(lv_obj_t *slot)
{
    // Deleting the pressed object resets the input device and would abort a
    // drag that started on a row's button; such a slot is rebound later.
    for (lv_obj_t *object = lv_indev_get_active_obj(); object; object = lv_obj_get_parent(object)) {
        if (object == slot) return lv_indev_get_active_obj() != slot;
    }
    return false;
}

void bind_slot(size_t slot_index, size_t position)
{
    lv_obj_t *slot = s_list.slots[slot_index];
    if (slot_holds_input(slot)) return;
    VirtualRow row;
    const bool ready = cached_row(position, &row);
    const bool same_position = s_list.slot_position[slot_index] == position;
    if (same_position && (s_list.slot_filled[slot_index] || !ready)) return;
    if (!same_position) {
        lv_obj_set_y(slot, s_list.spec.top + static_cast<int>(position) * s_list.spec.pitch);
    }
    lv_obj_remove_flag(slot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clean(slot);
    s_list.slot_position[slot_index] = position;
    s_list.slot_filled[slot_index] = ready;
    if (!ready) return;
    s_list.spec.build(slot, position, row, s_list.width);
    // The slot receives the touch, so the row itself is never the pressed
    // object when it is recycled. Nested action buttons stay clickable.
    const uint32_t children = lv_obj_get_child_count(slot);
    for (uint32_t i = 0; i < children; ++i) {
        lv_obj_remove_flag(lv_obj_get_child(slot, i), LV_OBJ_FLAG_CLICKABLE);
    }
}

size_t first_visible_position()
{
    const int32_t offset = lv_obj_get_scroll_y(s_list.body) - s_list.spec.top;
    const size_t first = offset > 0 ? static_cast<size_t>(offset / s_list.spec.pitch) : 0;
    return std::min(first, s_list.spec.count ? s_list.spec.count - 1 : 0);
}

void update_slots()
{
    if (!s_list.body || !s_list.spec.count) return;
    const size_t visible = first_visible_position();
    if (visible != s_list.last_first) s_list.forward = visible > s_list.last_first;
    s_list.last_first = visible;
    const size_t first = visible ? visible - 1 : 0;
    const size_t last = std::min(s_list.spec.count, first + s_list.slot_count);
    for (size_t position = first; position < last; ++position) {
        const size_t slot = position % s_list.slot_count;
        if (s_list.slot_position[slot] == position && s_list.slot_filled[slot]) continue;
        bind_slot(slot, position);
    }
    request_window(visible);
}

const lyra::media::SortLocation *mark_at(size_t position)
{
    const lyra::media::SortLocation *mark = nullptr;
    for (size_t i = 0; i < s_list.letter_count && s_list.letters[i].item_position <= position; ++i) {
        mark = &s_list.letters[i];
    }
    return mark;
}

void show_bubble(const lyra::media::SortLocation *mark)
{
    if (!mark || !s_list.bubble) return;
    // The bubble has room for the full value (a date, 3:45) the strip abbreviates.
    char text[lyra::media::kMaxName];
    format_jump_label(*mark, text, sizeof(text));
    if (!text[0]) return;
    set_label_text_if_changed(s_list.bubble_label, text);
    lv_obj_remove_flag(s_list.bubble, LV_OBJ_FLAG_HIDDEN);
    s_list.bubble_hide_us = esp_timer_get_time() + kBubbleHoldUs;
}

void scroll_to_position(size_t position)
{
    const int32_t target = s_list.spec.top + static_cast<int32_t>(position) * s_list.spec.pitch;
    const int32_t maximum = lv_obj_get_scroll_y(s_list.body) + lv_obj_get_scroll_bottom(s_list.body);
    lv_obj_scroll_to_y(s_list.body, std::min(target, std::max<int32_t>(0, maximum)), LV_ANIM_OFF);
}

void strip_touch_cb(lv_event_t *event)
{
    if (!s_list.body || !s_list.letter_count) return;
    lv_point_t point;
    lv_indev_get_point(lv_indev_active(), &point);
    const int32_t offset = point.y - (s_list.strip_top + kJumpCellHeight);
    const int32_t height = s_list.strip_height - kJumpCellHeight;
    if (height <= 0) return;
    const size_t index = std::min(s_list.letter_count - 1, static_cast<size_t>(
        std::clamp<int32_t>(offset, 0, height - 1) * static_cast<int32_t>(s_list.letter_count) / height));
    scroll_to_position(s_list.letters[index].item_position);
    show_bubble(&s_list.letters[index]);
    (void)event;
}

void build_letters()
{
    if (!s_list.strip || !s_list.letter_count) return;
    lv_obj_t *letters = make_box(s_list.strip, 0, kJumpCellHeight, kStripWidth,
                                 s_list.strip_height - kJumpCellHeight, kSurfaceRaised);
    lv_obj_set_style_bg_opa(letters, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(letters, LV_OBJ_FLAG_CLICKABLE);
    for (lv_event_code_t code : {LV_EVENT_PRESSED, LV_EVENT_PRESSING}) {
        lv_obj_add_event_cb(letters, strip_touch_cb, code, nullptr);
    }
    const int height = s_list.strip_height - kJumpCellHeight;
    const size_t rows = std::max<size_t>(1, std::min<size_t>(s_list.letter_count,
                                                             static_cast<size_t>(height / kLetterHeight)));
    // Labels sample the letters when they do not all fit; touch still maps
    // proportionally onto every letter.
    for (size_t row = 0; row < rows; ++row) {
        const size_t index = row * s_list.letter_count / rows;
        lv_obj_t *label = make_label(letters, s_list.letters[index].initial, kAccent);
        lv_obj_set_width(label, kStripWidth);
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(label, 0, static_cast<int>(row) * height / static_cast<int>(rows));
    }
}

void load_letters()
{
    if (!s_list.letters) {
        s_list.letters = static_cast<lyra::media::SortLocation *>(heap_caps_malloc(
            kMaxLetters * sizeof(lyra::media::SortLocation), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!s_list.letters) return;
    }
    bool pending = false;
    sort_jump_locations(s_list.spec.letter_section, s_list.spec.count, s_list.letters,
                           kMaxLetters, &s_list.letter_count, &pending);
    s_list.letters_pending = pending && !s_list.letter_count;
    build_letters();
}

void tick_cb(lv_timer_t *)
{
    if (!s_list.body) return;
    if (s_list.needs_update || s_rows_ready.exchange(false, std::memory_order_acq_rel)) {
        s_list.needs_update = false;
        update_slots();
    }
    if (s_list.letters_pending) load_letters();
    if (s_list.bubble && s_list.bubble_hide_us && esp_timer_get_time() >= s_list.bubble_hide_us &&
        !lv_obj_is_scrolling(s_list.body)) {
        s_list.bubble_hide_us = 0;
        lv_obj_add_flag(s_list.bubble, LV_OBJ_FLAG_HIDDEN);
    }
}

void body_event_cb(lv_event_t *event)
{
    switch (lv_event_get_code(event)) {
        case LV_EVENT_SCROLL:
            update_slots();
            if (s_list.letter_count && lv_obj_is_scrolling(s_list.body)) {
                show_bubble(mark_at(first_visible_position()));
            }
            break;
        case LV_EVENT_SCROLL_BEGIN:
            for (size_t i = 0; i < s_list.slot_count; ++i) {
                if (lv_obj_get_child_count(s_list.slots[i])) {
                    lv_obj_remove_state(lv_obj_get_child(s_list.slots[i], 0), LV_STATE_PRESSED);
                }
            }
            break;
        case LV_EVENT_DELETE:
            if (s_list.timer) lv_timer_delete(s_list.timer);
            {
                Lock lock;
                ++s_generation;
                s_request_fetch = nullptr;
            }
            heap_caps_free(s_list.letters);
            s_list = {};
            break;
        default:
            break;
    }
}

// Forwards touches from the persistent slot to the row it currently shows,
// so existing row callbacks and pressed styles work unchanged.
void slot_event_cb(lv_event_t *event)
{
    lv_obj_t *slot = lv_event_get_current_target_obj(event);
    if (lv_event_get_target_obj(event) != slot || !lv_obj_get_child_count(slot)) return;
    lv_obj_t *row = lv_obj_get_child(slot, 0);
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) lv_obj_add_state(row, LV_STATE_PRESSED);
    else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) lv_obj_remove_state(row, LV_STATE_PRESSED);
    lv_obj_send_event(row, code, nullptr);
}

lv_obj_t *make_plain_object(lv_obj_t *parent)
{
    lv_obj_t *object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    return object;
}

} // namespace

int virtual_list_row_width()
{
    return s_list.width ? s_list.width : 306;
}

void make_virtual_list(lv_obj_t *body, const VirtualListSpec &spec)
{
    if (!body || !spec.fetch || !spec.build || spec.pitch <= 0 || !ensure_prefetch()) return;
    if (s_list.body) lv_obj_remove_event_cb(s_list.body, body_event_cb);
    if (s_list.timer) lv_timer_delete(s_list.timer);
    heap_caps_free(s_list.letters);
    s_list = {};
    {
        Lock lock;
        ++s_generation;
        s_request_fetch = nullptr;
    }
    lv_obj_update_layout(body);
    s_list.body = body;
    s_list.spec = spec;
    s_list.width = spec.letter_strip ? 306 - kStripWidth - kStripGap : 306;
    s_list.forward = true;
    const int viewport = lv_obj_get_height(body);
    s_list.slot_count = std::min<size_t>(kMaxSlots, static_cast<size_t>(viewport / spec.pitch) + 4);

    if (s_pending_list_position != SIZE_MAX && spec.count) {
        const size_t position = std::min(s_pending_list_position, spec.count - 1);
        s_pending_scroll_restore_y = spec.top + static_cast<int32_t>(position) * spec.pitch;
    }
    // Fill the first screen synchronously, exactly as a page used to, so the
    // list never opens blank; everything beyond it is prefetched.
    const int32_t start_y = std::max<int32_t>(0, s_pending_scroll_restore_y);
    const size_t start = start_y > spec.top ? static_cast<size_t>((start_y - spec.top) / spec.pitch) : 0;
    const uint32_t generation = s_generation;
    for (size_t position = start ? start - 1 : 0;
         position < std::min(spec.count, start + s_list.slot_count); ++position) {
        VirtualRow row{};
        if (!spec.fetch(position, &row)) row = {};
        store_row(generation, position, row);
    }

    for (size_t i = 0; i < s_list.slot_count; ++i) {
        lv_obj_t *slot = make_plain_object(body);
        lv_obj_set_size(slot, kScreenWidth, spec.row_height);
        lv_obj_add_flag(slot, LV_OBJ_FLAG_HIDDEN);
        for (lv_event_code_t code : {LV_EVENT_PRESSED, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST,
                                     LV_EVENT_CLICKED, LV_EVENT_LONG_PRESSED}) {
            lv_obj_add_event_cb(slot, slot_event_cb, code, nullptr);
        }
        s_list.slots[i] = slot;
        s_list.slot_position[i] = SIZE_MAX;
    }
    // A 1 px marker gives the body its full scroll height without rows.
    lv_obj_t *extent = make_plain_object(body);
    lv_obj_remove_flag(extent, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(extent, 1, 1);
    lv_obj_set_pos(extent, 0, spec.top + static_cast<int>(spec.count) * spec.pitch + 8);

    for (lv_event_code_t code : {LV_EVENT_SCROLL, LV_EVENT_SCROLL_BEGIN, LV_EVENT_DELETE}) {
        lv_obj_add_event_cb(body, body_event_cb, code, nullptr);
    }
    s_list.timer = lv_timer_create(tick_cb, kTickMs, nullptr);
    s_list.needs_update = true;
    if (start_y == 0) update_slots();

    if (spec.letter_strip) {
        lv_area_t area;
        lv_obj_get_coords(body, &area);
        s_list.strip_top = area.y1 + 4;
        s_list.strip_height = lv_obj_get_height(body) - 8;
        s_list.strip = make_box(s_screen, kScreenWidth - kStripWidth - 4, s_list.strip_top,
                                kStripWidth, s_list.strip_height, kSurfaceRaised, 6);
        lv_obj_remove_flag(s_list.strip, LV_OBJ_FLAG_CLICKABLE);
        if (spec.jump_button) {
            lv_obj_t *jump = make_button(s_list.strip, 0, 0, kStripWidth, kJumpCellHeight,
                                         kSurfaceRaised, 6);
            lv_obj_t *icon = make_label(jump, LV_SYMBOL_LIST, kAccent);
            lv_obj_center(icon);
            lv_obj_add_event_cb(jump, [](lv_event_t *) {
                const size_t page_size = library_page_size();
                const size_t count = s_list.spec.count;
                open_page_jump(count, page_size, (count + page_size - 1) / page_size);
            }, LV_EVENT_CLICKED, nullptr);
        }
        s_list.bubble = make_box(s_screen, (kScreenWidth - 128) / 2, s_list.strip_top + 40,
                                 128, 44, kAccentDark, 12);
        lv_obj_remove_flag(s_list.bubble, LV_OBJ_FLAG_CLICKABLE);
        // The UI font covers every catalog script; the large Latin font does not.
        s_list.bubble_label = make_label(s_list.bubble, "", kTextOnAccent);
        lv_obj_set_width(s_list.bubble_label, 120);
        lv_label_set_long_mode(s_list.bubble_label, LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_style_text_align(s_list.bubble_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(s_list.bubble_label);
        lv_obj_add_flag(s_list.bubble, LV_OBJ_FLAG_HIDDEN);
        load_letters();
    }
}

} // namespace lyra::gui::internal
