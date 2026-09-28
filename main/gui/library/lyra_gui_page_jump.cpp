/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../lyra_gui_internal.h"

namespace lyra::gui::internal {

namespace {

constexpr size_t kPageJumpDigitsCapacity = 8;
constexpr size_t kLocationBatchSize = 30;
constexpr int kLocationCellWidth = 98;
constexpr int kLocationCellHeight = 44;
constexpr int kLocationColumnGap = 6;
constexpr int kLocationRowGap = 5;

struct SortLocationIdentity {
    lyra::media::SortSection section;
    lyra::media::SortSetting setting;
    size_t page_size;
    size_t item_count;
    uint32_t catalog_generation;
    uint32_t sorting_generation;
    uint32_t duration_generation;
};

struct SortLocationRequest {
    SortLocationIdentity identity;
    lyra::media::SortLocation *entries;
    size_t capacity;
    size_t count;
    bool complete;
    bool failed;
};

struct SortLocationViewContext {
    SortLocationIdentity identity;
    size_t next_entry;
};

size_t s_page_jump_total = 0;
size_t s_page_jump_page_size = 1;
size_t s_page_jump_page_count = 0;
char s_page_jump_digits[kPageJumpDigitsCapacity] = "";
bool s_page_jump_has_sort = false;
bool s_page_jump_show_sort = false;
View s_page_jump_return_view = View::Menu;
lyra::media::SortSection s_page_jump_sort_section = lyra::media::SortSection::Songs;
lv_obj_t *s_page_jump_value_label = nullptr;
lv_obj_t *s_page_jump_go_button = nullptr;
lv_obj_t *s_page_jump_go_label = nullptr;
lv_obj_t *s_sort_location_body = nullptr;
lv_obj_t *s_sort_location_loading = nullptr;
lv_timer_t *s_sort_location_timer = nullptr;
SortLocationViewContext *s_sort_location_view_context = nullptr;
SortLocationRequest *s_sort_location_request = nullptr;
bool s_sort_location_worker_running = false;
portMUX_TYPE s_sort_location_mux = portMUX_INITIALIZER_UNLOCKED;

bool page_jump_sort_section(View view, lyra::media::SortSection *section)
{
    if (!section) return false;
    switch (view) {
    case View::LibrarySongs:
        *section = lyra::media::SortSection::Songs;
        return true;
    case View::LibraryAlbums:
        *section = lyra::media::SortSection::Albums;
        return true;
    case View::LibraryArtists:
        *section = lyra::media::SortSection::Artists;
        return true;
    default:
        return false;
    }
}

bool same_sort_location_identity(const SortLocationIdentity &left,
                                 const SortLocationIdentity &right)
{
    return left.section == right.section &&
           left.setting.field == right.setting.field &&
           left.setting.direction == right.setting.direction &&
           left.page_size == right.page_size &&
           left.item_count == right.item_count &&
           left.catalog_generation == right.catalog_generation &&
           left.sorting_generation == right.sorting_generation &&
           left.duration_generation == right.duration_generation;
}

SortLocationIdentity current_sort_location_identity()
{
    const lyra::media::Status status = lyra::media::status();
    return {
        s_page_jump_sort_section,
        lyra::media::sort_setting(s_page_jump_sort_section),
        s_page_jump_page_size,
        std::min(s_page_jump_total, lyra::media::kMaxTracks),
        status.catalog_generation,
        status.sorting_generation,
        status.duration_generation,
    };
}

bool sort_location_indexing(const SortLocationIdentity &identity,
                            const lyra::media::Status &status)
{
    return status.sorting_indexing ||
           (identity.setting.field == lyra::media::SortField::Duration &&
            status.duration_indexing);
}

uint32_t local_date_key(uint64_t timestamp, int *year, int *month, int *day)
{
    if (!timestamp) {
        if (year) *year = 0;
        if (month) *month = 0;
        if (day) *day = 0;
        return 0;
    }
    const std::time_t raw = static_cast<std::time_t>(timestamp);
    std::tm local{};
    if (!localtime_r(&raw, &local)) return 0;
    if (year) *year = local.tm_year + 1900;
    if (month) *month = local.tm_mon + 1;
    if (day) *day = local.tm_mday;
    return static_cast<uint32_t>((local.tm_year + 1900) * 10000 +
                                 (local.tm_mon + 1) * 100 + local.tm_mday);
}

void format_location_date(uint64_t timestamp, char *output, size_t capacity)
{
    if (!output || capacity == 0) return;
    int year = 0;
    int month = 0;
    int day = 0;
    if (local_date_key(timestamp, &year, &month, &day) == 0) {
        copy_ui_text(output, capacity, tr(lyra::i18n::StringId::Unavailable));
        return;
    }
    switch (s_date_format) {
    case DateFormat::DayMonthYear:
        std::snprintf(output, capacity, "%02d/%02d/%04d", day, month, year);
        break;
    case DateFormat::MonthDayYear:
        std::snprintf(output, capacity, "%02d/%02d/%04d", month, day, year);
        break;
    case DateFormat::YearMonthDay:
        std::snprintf(output, capacity, "%04d-%02d-%02d", year, month, day);
        break;
    }
}

void format_location_duration(uint64_t milliseconds, char *output, size_t capacity)
{
    const uint64_t seconds = milliseconds / 1000u;
    const uint64_t hours = seconds / 3600u;
    const uint64_t minutes = (seconds / 60u) % 60u;
    const uint64_t remainder = seconds % 60u;
    if (hours) {
        std::snprintf(output, capacity, "%llu:%02llu:%02llu",
                      static_cast<unsigned long long>(hours),
                      static_cast<unsigned long long>(minutes),
                      static_cast<unsigned long long>(remainder));
    } else {
        std::snprintf(output, capacity, "%llu:%02llu",
                      static_cast<unsigned long long>(seconds / 60u),
                      static_cast<unsigned long long>(remainder));
    }
}

void return_to_page(size_t target_page)
{
    if (s_navigation_depth) {
        const NavigationState previous = s_navigation[--s_navigation_depth];
        restore_navigation_state(previous, target_page);
    } else {
        s_list_page = target_page;
        render(s_page_jump_return_view);
    }
}

void free_sort_location_request(SortLocationRequest *request)
{
    if (!request) return;
    heap_caps_free(request->entries);
    heap_caps_free(request);
}

void finish_sort_location_request(SortLocationRequest *request, bool failed,
                                  size_t count)
{
    portENTER_CRITICAL(&s_sort_location_mux);
    request->count = count;
    request->failed = failed;
    request->complete = true;
    s_sort_location_worker_running = false;
    portEXIT_CRITICAL(&s_sort_location_mux);
}

void sort_location_index_task(void *argument)
{
    auto *request = static_cast<SortLocationRequest *>(argument);
    bool success = request != nullptr;
    if (success && request->capacity) {
        auto *entries = static_cast<lyra::media::SortLocation *>(heap_caps_malloc(
            request->capacity * sizeof(lyra::media::SortLocation),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!entries) {
            finish_sort_location_request(request, true, 0);
            vTaskDelete(nullptr);
            return;
        }
        portENTER_CRITICAL(&s_sort_location_mux);
        request->entries = entries;
        portEXIT_CRITICAL(&s_sort_location_mux);
    }
    size_t count = 0;
    const esp_err_t result = success ? lyra::media::sort_locations(
        request->identity.section, request->entries, request->capacity, &count) : ESP_FAIL;
    if (request) {
        const lyra::media::Status status = lyra::media::status();
        const lyra::media::SortSetting setting =
            lyra::media::sort_setting(request->identity.section);
        success = result == ESP_OK &&
            status.catalog_generation == request->identity.catalog_generation &&
            status.sorting_generation == request->identity.sorting_generation &&
            (request->identity.setting.field != lyra::media::SortField::Duration ||
             status.duration_generation == request->identity.duration_generation) &&
            setting.field == request->identity.setting.field &&
            setting.direction == request->identity.setting.direction;
        finish_sort_location_request(request, !success, success ? count : 0);
    }
    vTaskDelete(nullptr);
}

void ensure_sort_location_request(const SortLocationIdentity &identity,
                                  const lyra::media::Status &status)
{
    if (sort_location_indexing(identity, status)) return;

    SortLocationRequest *old_request = nullptr;
    portENTER_CRITICAL(&s_sort_location_mux);
    if (s_sort_location_request &&
        same_sort_location_identity(s_sort_location_request->identity, identity)) {
        portEXIT_CRITICAL(&s_sort_location_mux);
        return;
    }
    if (s_sort_location_worker_running) {
        portEXIT_CRITICAL(&s_sort_location_mux);
        return;
    }
    old_request = s_sort_location_request;
    s_sort_location_request = nullptr;
    portEXIT_CRITICAL(&s_sort_location_mux);
    free_sort_location_request(old_request);

    auto *request = static_cast<SortLocationRequest *>(heap_caps_calloc(
        1, sizeof(SortLocationRequest), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!request) {
        request = static_cast<SortLocationRequest *>(heap_caps_calloc(
            1, sizeof(SortLocationRequest), MALLOC_CAP_8BIT));
    }
    if (!request) return;
    request->identity = identity;
    request->capacity = identity.item_count;

    portENTER_CRITICAL(&s_sort_location_mux);
    s_sort_location_request = request;
    s_sort_location_worker_running = true;
    portEXIT_CRITICAL(&s_sort_location_mux);
    if (xTaskCreatePinnedToCoreWithCaps(
            sort_location_index_task, "lyra_jump_idx", 16 * 1024, request,
            1, nullptr, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        finish_sort_location_request(request, true, 0);
    }
}

bool sort_location_request_status(const SortLocationIdentity &identity,
                                  size_t *count, bool *complete, bool *failed)
{
    bool matches = false;
    portENTER_CRITICAL(&s_sort_location_mux);
    SortLocationRequest *request = s_sort_location_request;
    if (request && same_sort_location_identity(request->identity, identity)) {
        matches = true;
        if (count) *count = request->count;
        if (complete) *complete = request->complete;
        if (failed) *failed = request->failed;
    }
    portEXIT_CRITICAL(&s_sort_location_mux);
    return matches;
}

bool sort_location_worker_active()
{
    portENTER_CRITICAL(&s_sort_location_mux);
    const bool active = s_sort_location_worker_running;
    portEXIT_CRITICAL(&s_sort_location_mux);
    return active;
}

bool copy_sort_location_entry(const SortLocationIdentity &identity, size_t index,
                              lyra::media::SortLocation *entry)
{
    if (!entry) return false;
    bool available = false;
    portENTER_CRITICAL(&s_sort_location_mux);
    SortLocationRequest *request = s_sort_location_request;
    if (request && same_sort_location_identity(request->identity, identity) &&
        index < request->count) {
        *entry = request->entries[index];
        available = true;
    }
    portEXIT_CRITICAL(&s_sort_location_mux);
    return available;
}

void format_sort_location_label(const lyra::media::SortLocation &entry, char *output,
                                size_t capacity)
{
    if (!output || capacity == 0) return;
    switch (entry.kind) {
    case lyra::media::SortLocationKind::Initial:
        copy_ui_text(output, capacity, entry.initial);
        break;
    case lyra::media::SortLocationKind::Date:
        format_location_date(entry.value, output, capacity);
        break;
    case lyra::media::SortLocationKind::Duration:
        format_location_duration(entry.value, output, capacity);
        break;
    case lyra::media::SortLocationKind::TrackNumber:
        std::snprintf(output, capacity, "%llu",
                      static_cast<unsigned long long>(entry.value));
        break;
    }
}

void location_cb(lv_event_t *event)
{
    const size_t target_page = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    if (target_page < s_page_jump_page_count) {
        return_to_page(target_page);
    }
}

void append_location_cells(lv_obj_t *body, SortLocationViewContext *context)
{
    if (!body || !context) return;
    size_t appended = 0;
    while (appended < kLocationBatchSize) {
        lyra::media::SortLocation entry{};
        if (!copy_sort_location_entry(context->identity, context->next_entry,
                                      &entry)) break;
        char label_text[lyra::media::kMaxName];
        format_sort_location_label(entry, label_text, sizeof(label_text));
        const size_t cell = context->next_entry++;
        const int column = static_cast<int>(cell % 3u);
        const int row = static_cast<int>(cell / 3u);
        lv_obj_t *button = make_button(body,
            7 + column * (kLocationCellWidth + kLocationColumnGap),
            4 + row * (kLocationCellHeight + kLocationRowGap),
            kLocationCellWidth, kLocationCellHeight, kSurfaceRaised, 6, true);
        lv_obj_t *label = make_label(button, label_text, kTextPrimary);
        lv_obj_center(label);
        lv_obj_add_event_cb(button, location_cb, LV_EVENT_CLICKED,
            reinterpret_cast<void *>(entry.item_position / context->identity.page_size));
        ++appended;
    }
}

void sort_location_list_cb(lv_event_t *event)
{
    auto *context = static_cast<SortLocationViewContext *>(lv_event_get_user_data(event));
    if (!context) return;
    if (lv_event_get_code(event) == LV_EVENT_DELETE) {
        if (s_sort_location_view_context == context) {
            s_sort_location_view_context = nullptr;
            s_sort_location_body = nullptr;
            s_sort_location_loading = nullptr;
        }
        lv_free(context);
        return;
    }
    lv_obj_t *body = lv_event_get_current_target_obj(event);
    if (lv_obj_get_scroll_bottom(body) < 140) append_location_cells(body, context);
}

void stop_sort_location_timer()
{
    if (!s_sort_location_timer) return;
    lv_timer_delete(s_sort_location_timer);
    s_sort_location_timer = nullptr;
}

void sort_location_poll_cb(lv_timer_t *timer)
{
    if (s_view != View::PageJump || !s_page_jump_show_sort ||
        timer != s_sort_location_timer) {
        stop_sort_location_timer();
        return;
    }
    const SortLocationIdentity identity = current_sort_location_identity();
    const lyra::media::Status status = lyra::media::status();
    ensure_sort_location_request(identity, status);

    size_t count = 0;
    bool complete = false;
    bool failed = false;
    const bool matches = sort_location_request_status(identity, &count,
                                                       &complete, &failed);
    if (!matches && !sort_location_worker_active() &&
        !sort_location_indexing(identity, status)) {
        if (s_sort_location_loading) {
            lv_label_set_text(s_sort_location_loading,
                              tr(lyra::i18n::StringId::Unavailable));
        }
        stop_sort_location_timer();
        return;
    }
    if (s_sort_location_loading) {
        if (sort_location_indexing(identity, status)) {
            lv_label_set_text(s_sort_location_loading,
                              tr(lyra::i18n::StringId::BuildingSortCache));
        } else if (complete && failed) {
            lv_label_set_text(s_sort_location_loading,
                              tr(lyra::i18n::StringId::Unavailable));
        } else if (complete && count == 0) {
            lv_label_set_text(s_sort_location_loading,
                              tr(lyra::i18n::StringId::Unavailable));
        } else if (count != 0 && s_sort_location_body) {
            lv_obj_add_flag(s_sort_location_loading, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_sort_location_body && count && !failed && s_view == View::PageJump &&
        lv_obj_get_scroll_bottom(s_sort_location_body) < 140) {
        auto *context = s_sort_location_view_context;
        if (context && same_sort_location_identity(context->identity, identity)) {
            append_location_cells(s_sort_location_body, context);
        }
    }
    if (complete && !sort_location_indexing(identity, status)) {
        stop_sort_location_timer();
    }
}

void start_sort_location_timer()
{
    stop_sort_location_timer();
    s_sort_location_timer = lv_timer_create(sort_location_poll_cb, 100, nullptr);
}

bool page_input_value(size_t *page)
{
    if (!page || !s_page_jump_digits[0]) return false;
    char *end = nullptr;
    const unsigned long parsed = std::strtoul(s_page_jump_digits, &end, 10);
    if (!end || *end != '\0' || parsed == 0 || parsed > s_page_jump_page_count) return false;
    *page = static_cast<size_t>(parsed - 1u);
    return true;
}

void apply_page_input()
{
    size_t target_page = 0;
    if (!page_input_value(&target_page)) return;
    return_to_page(target_page);
}

void refresh_page_jump_input()
{
    if (s_page_jump_value_label) lv_label_set_text(s_page_jump_value_label, s_page_jump_digits);
    size_t ignored = 0;
    const bool valid = page_input_value(&ignored);
    if (s_page_jump_go_button) {
        lv_obj_set_style_bg_color(s_page_jump_go_button,
                                  valid ? kAccentDark : kSurfaceRaised, 0);
    }
    if (s_page_jump_go_label) {
        lv_obj_set_style_text_color(s_page_jump_go_label,
                                    valid ? kTextOnAccent : kTextMuted, 0);
    }
}

void page_jump_key_cb(lv_event_t *event)
{
    const uintptr_t action = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    if (action <= 9u) {
        const size_t length = std::strlen(s_page_jump_digits);
        if (length + 1 < sizeof(s_page_jump_digits)) {
            s_page_jump_digits[length] = static_cast<char>('0' + action);
            s_page_jump_digits[length + 1] = '\0';
        }
    } else if (action == 10u) {
        s_page_jump_digits[0] = '\0';
    } else if (action == 11u) {
        const size_t length = std::strlen(s_page_jump_digits);
        if (length) s_page_jump_digits[length - 1] = '\0';
    } else if (action == 12u) {
        apply_page_input();
        return;
    }
    refresh_page_jump_input();
}

void page_jump_tab_cb(lv_event_t *event)
{
    s_page_jump_show_sort = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)) != 0;
    render(View::PageJump);
}

void make_page_jump_tab(lv_obj_t *parent, int x, const char *label, bool selected,
                        bool show_sort)
{
    lv_obj_t *button = make_button(parent, x, 0, 149, 38,
                                   selected ? kAccentDark : kSurface, 6, true);
    lv_obj_t *text = make_label(button, label,
                                selected ? kTextOnAccent : kTextSecondary);
    lv_obj_center(text);
    lv_obj_add_event_cb(button, page_jump_tab_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(show_sort ? 1u : 0u));
}

void render_page_entry()
{
    lv_obj_t *display = make_box(s_screen, 32, 125, 256, 52, kSurfaceRaised, 7, true);
    s_page_jump_value_label = make_label(display, s_page_jump_digits, kTextPrimary);
    lv_obj_align(s_page_jump_value_label, LV_ALIGN_CENTER, -10, 0);
    lv_obj_t *backspace = make_button(display, 211, 4, 40, 44, kSurface, 5, true);
    lv_obj_t *backspace_icon = make_label(backspace, LV_SYMBOL_BACKSPACE, kTextSecondary);
    lv_obj_center(backspace_icon);
    lv_obj_add_event_cb(backspace, page_jump_key_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(11u));

    constexpr int key_width = 88;
    constexpr int key_height = 48;
    constexpr int column_gap = 8;
    constexpr int row_gap = 5;
    constexpr int first_x = (kScreenWidth - (key_width * 3 + column_gap * 2)) / 2;
    constexpr int first_y = 188;
    for (unsigned digit = 1; digit <= 9; ++digit) {
        const unsigned index = digit - 1;
        const int x = first_x + static_cast<int>(index % 3u) * (key_width + column_gap);
        const int y = first_y + static_cast<int>(index / 3u) * (key_height + row_gap);
        lv_obj_t *button = make_button(s_screen, x, y, key_width, key_height,
                                       kSurfaceRaised, 7, true);
        char text[2] = {static_cast<char>('0' + digit), '\0'};
        lv_obj_t *label = make_label(button, text, kTextPrimary);
        lv_obj_center(label);
        lv_obj_add_event_cb(button, page_jump_key_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(digit));
    }

    const int final_y = first_y + 3 * (key_height + row_gap);
    lv_obj_t *clear = make_button(s_screen, first_x, final_y, key_width, key_height,
                                  kSurfaceRaised, 7, true);
    lv_obj_t *clear_label = make_label(clear, tr(lyra::i18n::StringId::Clear),
                                       kTextSecondary);
    lv_obj_center(clear_label);
    lv_obj_add_event_cb(clear, page_jump_key_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(10u));

    lv_obj_t *zero = make_button(s_screen, first_x + key_width + column_gap, final_y,
                                 key_width, key_height, kSurfaceRaised, 7, true);
    char zero_text[8];
    format_u32(lyra::i18n::StringId::QueueNumber, 0, zero_text, sizeof(zero_text));
    lv_obj_t *zero_label = make_label(zero, zero_text, kTextPrimary);
    lv_obj_center(zero_label);
    lv_obj_add_event_cb(zero, page_jump_key_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(0u));

    s_page_jump_go_button = make_button(s_screen,
        first_x + 2 * (key_width + column_gap), final_y, key_width, key_height,
        kSurfaceRaised, 7, true);
    s_page_jump_go_label = make_label(s_page_jump_go_button,
                                      tr(lyra::i18n::StringId::PageJumpGo), kTextMuted);
    lv_obj_center(s_page_jump_go_label);
    lv_obj_add_event_cb(s_page_jump_go_button, page_jump_key_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(12u));
    refresh_page_jump_input();
}

void render_sort_locations()
{
    const lyra::media::SortSetting setting =
        lyra::media::sort_setting(s_page_jump_sort_section);
    lv_obj_t *field = make_label(s_screen, sort_field_name(setting.field), kTextSecondary);
    lv_obj_align(field, LV_ALIGN_TOP_MID, 0, 119);
    s_sort_location_body = make_scroll_body(146);
    s_sort_location_loading = make_label(s_sort_location_body,
        tr(lyra::i18n::StringId::BuildingSortCache), kTextMuted);
    lv_obj_set_pos(s_sort_location_loading, 12, 8);

    const SortLocationIdentity identity = current_sort_location_identity();
    auto *context = static_cast<SortLocationViewContext *>(
        lv_malloc_zeroed(sizeof(SortLocationViewContext)));
    if (!context) {
        lv_label_set_text(s_sort_location_loading,
                          tr(lyra::i18n::StringId::Unavailable));
        return;
    }
    context->identity = identity;
    s_sort_location_view_context = context;
    lv_obj_add_event_cb(s_sort_location_body, sort_location_list_cb,
                        LV_EVENT_SCROLL, context);
    lv_obj_add_event_cb(s_sort_location_body, sort_location_list_cb,
                        LV_EVENT_SCROLL_END, context);
    lv_obj_add_event_cb(s_sort_location_body, sort_location_list_cb,
                        LV_EVENT_DELETE, context);

    const lyra::media::Status status = lyra::media::status();
    ensure_sort_location_request(identity, status);
    size_t count = 0;
    bool complete = false;
    bool failed = false;
    if (sort_location_request_status(identity, &count, &complete, &failed)) {
        if (complete && (failed || count == 0) &&
            !sort_location_indexing(identity, status)) {
            lv_label_set_text(s_sort_location_loading,
                              tr(lyra::i18n::StringId::Unavailable));
        } else if (count) {
            lv_obj_add_flag(s_sort_location_loading, LV_OBJ_FLAG_HIDDEN);
            append_location_cells(s_sort_location_body, context);
        }
    }
    if (!complete || sort_location_indexing(identity, status)) {
        start_sort_location_timer();
    }
}

} // namespace

void open_page_jump(size_t total, size_t page_size, size_t page_count)
{
    if (page_size == 0 || page_count <= 1) return;
    s_page_jump_total = total;
    s_page_jump_page_size = page_size;
    s_page_jump_page_count = page_count;
    s_page_jump_return_view = s_view;
    s_page_jump_digits[0] = '\0';
    s_page_jump_show_sort = false;
    s_page_jump_has_sort = page_jump_sort_section(s_view, &s_page_jump_sort_section);
    push_navigation_state();
    render(View::PageJump);
}

void render_page_jump()
{
    stop_sort_location_timer();
    s_sort_location_body = nullptr;
    s_sort_location_loading = nullptr;
    s_sort_location_view_context = nullptr;
    s_page_jump_value_label = nullptr;
    s_page_jump_go_button = nullptr;
    s_page_jump_go_label = nullptr;
    make_header(tr(lyra::i18n::StringId::PageJumpTitle), View::Menu, true,
                nullptr, nav_back_cb);
    if (s_page_jump_has_sort) {
        lv_obj_t *tabs = make_box(s_screen, 7, 72, 306, 38, kBackground);
        make_page_jump_tab(tabs, 0, tr(lyra::i18n::StringId::PageJumpPageTab),
                           !s_page_jump_show_sort, false);
        make_page_jump_tab(tabs, 157, tr(lyra::i18n::StringId::PageJumpSortTab),
                           s_page_jump_show_sort, true);
    }
    if (s_page_jump_show_sort && s_page_jump_has_sort) render_sort_locations();
    else render_page_entry();
}

} // namespace lyra::gui::internal
