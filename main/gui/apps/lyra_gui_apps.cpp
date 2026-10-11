/* SPDX-License-Identifier: Apache-2.0 */
#include "../lyra_gui_internal.h"

#include <atomic>
#include <cmath>
#include <new>

namespace lyra::gui::internal {
namespace {
constexpr int kAppFilePageSize = 5;
constexpr int64_t kMicrosecondsPerSecond = 1000000;

struct FileScan {
    std::atomic<bool> running{false};
    std::atomic<bool> complete{false};
    std::atomic<size_t> found{};
    lyra::media::FolderFile *files{};
    size_t count{};
    bool succeeded{};
    bool refresh_cache{};
};

struct FileScanJob {
    FileScan *scan;
    lyra::media::FileKind kind;
    bool refresh_cache;
};

FileScan s_photo_scan;
FileScan s_ebook_scan;
lv_timer_t *s_file_scan_timer{};
lv_timer_t *s_stopwatch_timer{};
lv_timer_t *s_countdown_timer{};
lv_obj_t *s_stopwatch_readout{};
lv_obj_t *s_countdown_readout{};
lv_obj_t *s_countdown_status{};
lv_obj_t *s_file_scan_count{};

struct CalculatorState {
    double accumulator{};
    char operation{};
    bool waiting_for_operand{};
    char display[24]{"0"};
} s_calculator;

bool s_stopwatch_running{};
int64_t s_stopwatch_started_us{};
int64_t s_stopwatch_elapsed_us{};
int64_t s_stopwatch_laps[8]{};
size_t s_stopwatch_lap_count{};

bool s_countdown_running{};
int64_t s_countdown_end_us{};
int64_t s_countdown_remaining_us{5 * 60 * kMicrosecondsPerSecond};

int s_calendar_month_offset{};

void app_destination_cb(lv_event_t *event)
{
    const View target = static_cast<View>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    navigate_to(target);
}

void file_scan_progress(void *context, size_t found)
{
    auto *scan = static_cast<FileScan *>(context);
    if (scan) scan->found.store(found, std::memory_order_release);
}

void file_scan_task(void *argument)
{
    auto *job = static_cast<FileScanJob *>(argument);
    lyra::media::FolderFile *files = nullptr;
    size_t count = 0;
    bool succeeded = lyra::media::folder_tree_files(
        "/sdcard", job->kind, &files, &count, file_scan_progress,
        job->scan, job->refresh_cache);
    job->scan->files = files;
    job->scan->count = count;
    job->scan->succeeded = succeeded;
    job->scan->found.store(count, std::memory_order_release);
    job->scan->complete.store(true, std::memory_order_release);
    job->scan->running.store(false, std::memory_order_release);
    delete job;
    vTaskDelete(nullptr);
}

bool start_file_scan(FileScan *scan, lyra::media::FileKind kind)
{
    if (scan->running.load(std::memory_order_acquire)) return true;
    if (scan->complete.load(std::memory_order_acquire)) return true;
    auto *job = new (std::nothrow) FileScanJob{scan, kind, scan->refresh_cache};
    if (!job) {
        scan->succeeded = false;
        scan->complete.store(true, std::memory_order_release);
        return false;
    }
    scan->refresh_cache = false;
    scan->found.store(0, std::memory_order_release);
    scan->running.store(true, std::memory_order_release);
    if (xTaskCreate(file_scan_task, "lyra.app.files", 16 * 1024, job, 2, nullptr) != pdPASS) {
        scan->running.store(false, std::memory_order_release);
        delete job;
        scan->succeeded = false;
        scan->complete.store(true, std::memory_order_release);
        return false;
    }
    return true;
}

void file_scan_poll_cb(lv_timer_t *timer)
{
    if (s_view != View::Photos && s_view != View::Ebook) {
        lv_timer_delete(timer);
        s_file_scan_timer = nullptr;
        return;
    }
    FileScan &scan = s_view == View::Photos ? s_photo_scan : s_ebook_scan;
    if (scan.complete.load(std::memory_order_acquire) &&
        !scan.running.load(std::memory_order_acquire)) {
        lv_timer_delete(timer);
        s_file_scan_timer = nullptr;
        s_file_scan_count = nullptr;
        render(s_view);
        return;
    }
    if (s_file_scan_count) {
        char count_text[32];
        format_u32(lyra::i18n::StringId::AppFilesFound,
            static_cast<uint32_t>(std::min<size_t>(
                scan.found.load(std::memory_order_acquire), UINT32_MAX)),
            count_text, sizeof(count_text));
        set_label_text_if_changed(s_file_scan_count, count_text);
    }
}

void ensure_file_scan_poll()
{
    if (!s_file_scan_timer) s_file_scan_timer = lv_timer_create(file_scan_poll_cb, 150, nullptr);
}

void format_stopwatch(int64_t elapsed_us, char *output, size_t capacity)
{
    const uint64_t centiseconds = static_cast<uint64_t>(std::max<int64_t>(0, elapsed_us)) / 10000;
    const uint64_t minutes = centiseconds / 6000;
    const uint64_t seconds = (centiseconds / 100) % 60;
    const uint64_t fraction = centiseconds % 100;
    std::snprintf(output, capacity, "%02llu:%02llu.%02llu",
        static_cast<unsigned long long>(minutes),
        static_cast<unsigned long long>(seconds),
        static_cast<unsigned long long>(fraction));
}

int64_t stopwatch_elapsed()
{
    return s_stopwatch_elapsed_us + (s_stopwatch_running ?
        std::max<int64_t>(0, esp_timer_get_time() - s_stopwatch_started_us) : 0);
}

void update_stopwatch_readout()
{
    if (!s_stopwatch_readout) return;
    char text[24];
    format_stopwatch(stopwatch_elapsed(), text, sizeof(text));
    set_label_text_if_changed(s_stopwatch_readout, text);
}

void stopwatch_timer_cb(lv_timer_t *timer)
{
    if (s_view != View::Stopwatch) {
        lv_timer_delete(timer);
        s_stopwatch_timer = nullptr;
        return;
    }
    update_stopwatch_readout();
}

void ensure_stopwatch_timer()
{
    if (!s_stopwatch_timer) s_stopwatch_timer = lv_timer_create(stopwatch_timer_cb, 40, nullptr);
}

void format_countdown(int64_t remaining_us, char *output, size_t capacity)
{
    const int64_t seconds = std::max<int64_t>(0, remaining_us) / kMicrosecondsPerSecond;
    std::snprintf(output, capacity, "%02lld:%02lld",
        static_cast<long long>(seconds / 60), static_cast<long long>(seconds % 60));
}

int64_t countdown_remaining()
{
    return s_countdown_running ?
        std::max<int64_t>(0, s_countdown_end_us - esp_timer_get_time()) :
        std::max<int64_t>(0, s_countdown_remaining_us);
}

void update_countdown_readout()
{
    if (!s_countdown_readout) return;
    char text[20];
    format_countdown(countdown_remaining(), text, sizeof(text));
    set_label_text_if_changed(s_countdown_readout, text);
}

void countdown_timer_cb(lv_timer_t *timer)
{
    if (!s_countdown_running) {
        lv_timer_delete(timer);
        s_countdown_timer = nullptr;
        return;
    }
    if (s_countdown_running && esp_timer_get_time() >= s_countdown_end_us) {
        s_countdown_remaining_us = 0;
        s_countdown_running = false;
        if (s_countdown_status) {
            lv_label_set_text(s_countdown_status, tr(lyra::i18n::StringId::AppTimerFinished));
        }
        if (s_timer_alarm_enabled) {
            const esp_err_t alarm_result = lyra::audio::play_timer_alarm();
            if (alarm_result != ESP_OK) {
                ESP_LOGW(kTag, "could not play timer alarm: %s",
                         esp_err_to_name(alarm_result));
            }
        }
        lv_timer_delete(timer);
        s_countdown_timer = nullptr;
    }
    if (s_view == View::Timer) update_countdown_readout();
}

void ensure_countdown_timer()
{
    if (!s_countdown_timer) s_countdown_timer = lv_timer_create(countdown_timer_cb, 100, nullptr);
}

void calculator_format(double value)
{
    if (!std::isfinite(value)) {
        copy_ui_text(s_calculator.display, sizeof(s_calculator.display),
                     tr(lyra::i18n::StringId::AppCalculatorError));
        s_calculator.waiting_for_operand = true;
        s_calculator.operation = 0;
        return;
    }
    std::snprintf(s_calculator.display, sizeof(s_calculator.display), "%.8g", value);
}

double calculator_value()
{
    char *end = nullptr;
    const double value = std::strtod(s_calculator.display, &end);
    return end == s_calculator.display ? 0.0 : value;
}

double apply_calculator_operation(double left, double right, char operation)
{
    switch (operation) {
        case '+': return left + right;
        case '-': return left - right;
        case '*': return left * right;
        case '/': return right == 0.0 ? std::nan("") : left / right;
        default: return right;
    }
}

void calculator_key_cb(lv_event_t *event)
{
    const uintptr_t key = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    if (key <= 9) {
        if (s_calculator.waiting_for_operand ||
            std::strcmp(s_calculator.display, tr(lyra::i18n::StringId::AppCalculatorError)) == 0) {
            copy_ui_text(s_calculator.display, sizeof(s_calculator.display), "0");
            s_calculator.waiting_for_operand = false;
        }
        const size_t length = std::strlen(s_calculator.display);
        if (length < sizeof(s_calculator.display) - 2) {
            if (length == 1 && s_calculator.display[0] == '0') {
                s_calculator.display[0] = static_cast<char>('0' + key);
                s_calculator.display[1] = '\0';
            } else {
                s_calculator.display[length] = static_cast<char>('0' + key);
                s_calculator.display[length + 1] = '\0';
            }
        }
    } else if (key == 10) {
        if (s_calculator.waiting_for_operand) {
            copy_ui_text(s_calculator.display, sizeof(s_calculator.display), "0");
            s_calculator.waiting_for_operand = false;
        }
        if (!std::strchr(s_calculator.display, '.')) {
            const size_t length = std::strlen(s_calculator.display);
            if (length < sizeof(s_calculator.display) - 2) {
                s_calculator.display[length] = '.';
                s_calculator.display[length + 1] = '\0';
            }
        }
    } else if (key >= 11 && key <= 14) {
        if (s_calculator.operation && !s_calculator.waiting_for_operand) {
            calculator_format(apply_calculator_operation(
                s_calculator.accumulator, calculator_value(), s_calculator.operation));
        }
        s_calculator.accumulator = calculator_value();
        s_calculator.operation = key == 11 ? '+' : key == 12 ? '-' : key == 13 ? '*' : '/';
        s_calculator.waiting_for_operand = true;
    } else if (key == 15) {
        if (s_calculator.operation) {
            calculator_format(apply_calculator_operation(
                s_calculator.accumulator, calculator_value(), s_calculator.operation));
            s_calculator.operation = 0;
            s_calculator.waiting_for_operand = true;
        }
    } else if (key == 16) {
        s_calculator = {};
        copy_ui_text(s_calculator.display, sizeof(s_calculator.display), "0");
    }
    render(View::Calculator);
}

void calendar_shift_cb(lv_event_t *event)
{
    const int direction = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)) ? 1 : -1;
    s_calendar_month_offset = std::clamp(s_calendar_month_offset + direction, -1199, 1199);
    render(View::Calendar);
}

int calendar_weekday_monday_first(int year, int month, int day)
{
    constexpr int offsets[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (month < 3) --year;
    const int sunday_first = (year + year / 4 - year / 100 + year / 400 +
                              offsets[month - 1] + day) % 7;
    return (sunday_first + 6) % 7;
}

int calendar_days_in_month(int year, int month)
{
    constexpr int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) return 29;
    return days[month - 1];
}

void file_reload_cb(lv_event_t *)
{
    if (s_view != View::Photos && s_view != View::Ebook) return;
    FileScan &scan = s_view == View::Photos ? s_photo_scan : s_ebook_scan;
    if (scan.running.load(std::memory_order_acquire)) return;
    release_app_file_scan(s_view);
    scan.refresh_cache = true;
    s_list_page = 0;
    render(s_view);
}

void stopwatch_action_cb(lv_event_t *event)
{
    const uintptr_t action = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    const int64_t now = esp_timer_get_time();
    if (action == 0) {
        if (s_stopwatch_running) {
            s_stopwatch_elapsed_us += std::max<int64_t>(0, now - s_stopwatch_started_us);
            s_stopwatch_running = false;
        } else {
            s_stopwatch_started_us = now;
            s_stopwatch_running = true;
            ensure_stopwatch_timer();
        }
    } else if (action == 1) {
        if (s_stopwatch_lap_count < sizeof(s_stopwatch_laps) / sizeof(s_stopwatch_laps[0])) {
            s_stopwatch_laps[s_stopwatch_lap_count++] = stopwatch_elapsed();
        }
    } else {
        s_stopwatch_running = false;
        s_stopwatch_elapsed_us = 0;
        s_stopwatch_lap_count = 0;
        if (s_stopwatch_timer) {
            lv_timer_delete(s_stopwatch_timer);
            s_stopwatch_timer = nullptr;
        }
    }
    render(View::Stopwatch);
}

void countdown_adjust_cb(lv_event_t *event)
{
    const int direction = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)) ? 1 : -1;
    const int64_t now = esp_timer_get_time();
    int64_t remaining = countdown_remaining() +
        direction * 60 * kMicrosecondsPerSecond;
    remaining = std::clamp<int64_t>(remaining, 0, 99 * 60 * kMicrosecondsPerSecond);
    s_countdown_remaining_us = remaining;
    if (s_countdown_running) {
        s_countdown_end_us = now + remaining;
        if (remaining == 0) s_countdown_running = false;
    }
    if (s_countdown_status) lv_label_set_text(s_countdown_status, "");
    if (!s_countdown_running && s_countdown_timer) {
        lv_timer_delete(s_countdown_timer);
        s_countdown_timer = nullptr;
    }
    render(View::Timer);
}

void countdown_action_cb(lv_event_t *event)
{
    const uintptr_t action = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    if (action == 0) {
        if (s_countdown_running) {
            s_countdown_remaining_us = countdown_remaining();
            s_countdown_running = false;
        } else if (s_countdown_remaining_us > 0) {
            s_countdown_end_us = esp_timer_get_time() + s_countdown_remaining_us;
            s_countdown_running = true;
            ensure_countdown_timer();
        }
    } else {
        s_countdown_running = false;
        s_countdown_remaining_us = 5 * 60 * kMicrosecondsPerSecond;
        if (s_countdown_timer) {
            lv_timer_delete(s_countdown_timer);
            s_countdown_timer = nullptr;
        }
    }
    render(View::Timer);
}

} // namespace

void release_app_file_scan(View view)
{
    if (view != View::Photos && view != View::Ebook) return;
    FileScan &scan = view == View::Photos ? s_photo_scan : s_ebook_scan;
    if (scan.running.load(std::memory_order_acquire)) return;
    lyra::media::free_folder_tree_files(scan.files);
    scan.files = nullptr;
    scan.count = 0;
    scan.succeeded = false;
    scan.found.store(0, std::memory_order_release);
    scan.refresh_cache = false;
    scan.complete.store(false, std::memory_order_release);
}

void stop_apps_runtime_timers(View next_view)
{
    if (s_file_scan_timer) lv_timer_delete(s_file_scan_timer);
    if (s_stopwatch_timer) lv_timer_delete(s_stopwatch_timer);
    if (s_countdown_timer && !s_countdown_running) lv_timer_delete(s_countdown_timer);
    s_file_scan_timer = nullptr;
    s_stopwatch_timer = nullptr;
    if (!s_countdown_running) s_countdown_timer = nullptr;
    s_stopwatch_readout = nullptr;
    s_countdown_readout = nullptr;
    s_countdown_status = nullptr;
    s_file_scan_count = nullptr;
    (void)next_view;
}

void render_apps()
{
    make_header(tr(lyra::i18n::StringId::Apps), View::Menu, true);
    lv_obj_t *body = make_scroll_body(72);
    struct AppEntry { lyra::i18n::StringId label; View view; const char *icon; };
    constexpr AppEntry apps[] = {
        {lyra::i18n::StringId::Calculator, View::Calculator, "+"},
        {lyra::i18n::StringId::Calendar, View::Calendar, LV_SYMBOL_DIRECTORY},
        {lyra::i18n::StringId::Stopwatch, View::Stopwatch, LV_SYMBOL_PLAY},
        {lyra::i18n::StringId::Timer, View::Timer, LV_SYMBOL_REFRESH},
        {lyra::i18n::StringId::Photos, View::Photos, LV_SYMBOL_IMAGE},
        {lyra::i18n::StringId::Ebook, View::Ebook, LV_SYMBOL_FILE},
    };
    constexpr int x[] = {9, 164};
    constexpr int y[] = {8, 116, 224};
    for (size_t i = 0; i < sizeof(apps) / sizeof(apps[0]); ++i) {
        lv_obj_t *button = make_button(body, x[i % 2], y[i / 2], 147, 102,
            is_cute_theme() ? lv_color_hex(0xF3EDFF) : kSurfaceRaised,
            is_cute_theme() ? 18 : 9, true);
        lv_obj_t *icon = make_label(button, apps[i].icon, kAccent);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_28, 0);
        lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 17);
        lv_obj_t *label = make_label(button, tr(apps[i].label), kTextPrimary);
        make_marquee(label, 132);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -12);
        lv_obj_add_event_cb(button, app_destination_cb, LV_EVENT_CLICKED,
            reinterpret_cast<void *>(static_cast<uintptr_t>(apps[i].view)));
        if (s_neon_hud) add_hud_frame(button, true);
    }
}

void render_calculator()
{
    make_header(tr(lyra::i18n::StringId::Calculator), View::Apps, true);
    lv_obj_t *body = make_scroll_body(72);
    lv_obj_t *display = make_box(body, 8, 5, 304, 62, kSurfaceRaised, 8, true);
    lv_obj_t *value = make_label(display, s_calculator.display, kTextPrimary);
    lv_obj_set_width(value, 216);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(value, &lv_font_montserrat_28, 0);
    lv_obj_align(value, LV_ALIGN_RIGHT_MID, -12, 0);
    lv_obj_t *clear = make_button(display, 8, 9, 64, 44, kAccentSurface, 6, true);
    lv_obj_t *clear_label = make_label(clear, tr(lyra::i18n::StringId::Clear), kTextPrimary);
    make_marquee(clear_label, 58);
    lv_obj_center(clear_label);
    lv_obj_add_event_cb(clear, calculator_key_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(uintptr_t(16)));

    constexpr const char *keys[4][4] = {
        {"7", "8", "9", "/"}, {"4", "5", "6", "*"},
        {"1", "2", "3", "-"}, {"0", ".", "=", "+"},
    };
    constexpr uintptr_t values[4][4] = {
        {7, 8, 9, 14}, {4, 5, 6, 13},
        {1, 2, 3, 12}, {0, 10, 15, 11},
    };
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            const int x = 8 + column * 76;
            const int y = 78 + row * 62;
            const bool operation = column == 3 || values[row][column] == 15;
            lv_obj_t *button = make_button(body, x, y, 70, 56,
                operation ? kAccentSurface : kSurface, 7, true);
            lv_obj_t *label = make_label(button, keys[row][column],
                operation ? kAccent : kTextPrimary);
            lv_obj_set_style_text_font(label, &lv_font_montserrat_18, 0);
            lv_obj_center(label);
            lv_obj_add_event_cb(button, calculator_key_cb, LV_EVENT_CLICKED,
                reinterpret_cast<void *>(values[row][column]));
            if (s_neon_hud) add_hud_frame(button, false);
        }
    }
}

void render_calendar()
{
    make_header(tr(lyra::i18n::StringId::Calendar), View::Apps, true);
    lv_obj_t *body = make_scroll_body(72);
    const lyra::clock::DateTime today = lyra::clock::now();
    const int month_index = today.year * 12 + today.month - 1 + s_calendar_month_offset;
    const int year = month_index / 12;
    const int month = month_index % 12 + 1;
    const int first_weekday = calendar_weekday_monday_first(year, month, 1);
    const int days = calendar_days_in_month(year, month);

    lv_obj_t *previous = make_button(body, 8, 5, 48, 40, kSurfaceRaised, 6, true);
    lv_obj_t *previous_icon = make_label(previous, LV_SYMBOL_LEFT, kTextPrimary);
    lv_obj_center(previous_icon);
    lv_obj_add_event_cb(previous, calendar_shift_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(uintptr_t(0)));
    char month_text[16];
    std::snprintf(month_text, sizeof(month_text), "%04d-%02d", year, month);
    lv_obj_t *month_label = make_label(body, month_text, kTextPrimary);
    lv_obj_set_width(month_label, 180);
    lv_obj_set_style_text_align(month_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(month_label, LV_ALIGN_TOP_MID, 0, 16);
    lv_obj_t *next = make_button(body, 264, 5, 48, 40, kSurfaceRaised, 6, true);
    lv_obj_t *next_icon = make_label(next, LV_SYMBOL_RIGHT, kTextPrimary);
    lv_obj_center(next_icon);
    lv_obj_add_event_cb(next, calendar_shift_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(uintptr_t(1)));

    constexpr lyra::i18n::StringId weekdays[] = {
        lyra::i18n::StringId::AppWeekdayMon,
        lyra::i18n::StringId::AppWeekdayTue,
        lyra::i18n::StringId::AppWeekdayWed,
        lyra::i18n::StringId::AppWeekdayThu,
        lyra::i18n::StringId::AppWeekdayFri,
        lyra::i18n::StringId::AppWeekdaySat,
        lyra::i18n::StringId::AppWeekdaySun,
    };
    for (int column = 0; column < 7; ++column) {
        lv_obj_t *weekday = make_label(body, tr(weekdays[column]), kTextSecondary);
        lv_obj_set_width(weekday, 40);
        lv_obj_set_style_text_align(weekday, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(weekday, 11 + column * 43, 54);
    }
    for (int day = 1; day <= days; ++day) {
        const int cell = first_weekday + day - 1;
        const int column = cell % 7;
        const int row = cell / 7;
        const bool is_today = year == today.year && month == today.month && day == today.day;
        if (is_today) {
            lv_obj_t *highlight = make_box(body, 11 + column * 43, 82 + row * 42,
                                          40, 36, kAccentSurface, LV_RADIUS_CIRCLE);
            lv_obj_clear_flag(highlight, LV_OBJ_FLAG_CLICKABLE);
        }
        char day_text[12];
        std::snprintf(day_text, sizeof(day_text), "%d", day);
        lv_obj_t *label = make_label(body, day_text, is_today ? kAccent : kTextPrimary);
        lv_obj_set_width(label, 40);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(label, 11 + column * 43, 90 + row * 42);
    }
}

void render_stopwatch()
{
    make_header(tr(lyra::i18n::StringId::Stopwatch), View::Apps, true);
    lv_obj_t *body = make_scroll_body(72);
    lv_obj_t *panel = make_box(body, 8, 15, 304, 90, kSurfaceRaised, 10, true);
    s_stopwatch_readout = make_label(panel, "", kTextPrimary);
    lv_obj_set_style_text_font(s_stopwatch_readout, &lv_font_montserrat_28, 0);
    lv_obj_center(s_stopwatch_readout);
    update_stopwatch_readout();

    constexpr int button_x[] = {8, 112, 216};
    constexpr int button_action[] = {0, 1, 2};
    const lyra::i18n::StringId button_label[] = {
        s_stopwatch_running ? lyra::i18n::StringId::AppPause :
                              lyra::i18n::StringId::AppStart,
        lyra::i18n::StringId::AppLap,
        lyra::i18n::StringId::AppReset,
    };
    for (int i = 0; i < 3; ++i) {
        lv_obj_t *button = make_button(body, button_x[i], 122, 96, 48,
            i == 0 ? kAccentSurface : kSurface, 7, true);
        lv_obj_t *label = make_label(button, tr(button_label[i]), kTextPrimary);
        make_marquee(label, 88);
        lv_obj_center(label);
        lv_obj_add_event_cb(button, stopwatch_action_cb, LV_EVENT_CLICKED,
            reinterpret_cast<void *>(static_cast<uintptr_t>(button_action[i])));
    }
    for (size_t index = 0; index < s_stopwatch_lap_count && index < 5; ++index) {
        char lap[32];
        char elapsed[24];
        format_stopwatch(s_stopwatch_laps[index], elapsed, sizeof(elapsed));
        std::snprintf(lap, sizeof(lap), "%s  %s", tr(lyra::i18n::StringId::AppLap), elapsed);
        lv_obj_t *label = make_label(body, lap, kTextSecondary);
        lv_obj_set_pos(label, 18, 188 + static_cast<int>(index) * 32);
    }
    if (s_stopwatch_running) ensure_stopwatch_timer();
}

void render_timer()
{
    make_header(tr(lyra::i18n::StringId::Timer), View::Apps, true);
    lv_obj_t *body = make_scroll_body(72);
    lv_obj_t *panel = make_box(body, 8, 8, 304, 90, kSurfaceRaised, 10, true);
    s_countdown_readout = make_label(panel, "", kTextPrimary);
    lv_obj_set_style_text_font(s_countdown_readout, &lv_font_montserrat_28, 0);
    lv_obj_center(s_countdown_readout);
    update_countdown_readout();
    s_countdown_status = make_label(body, "", kAccent);
    lv_obj_set_width(s_countdown_status, 300);
    lv_obj_set_style_text_align(s_countdown_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_countdown_status, 10, 104);

    lv_obj_t *minutes = make_label(body, tr(lyra::i18n::StringId::AppMinutes), kTextSecondary);
    lv_obj_set_style_text_align(minutes, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(minutes, 110);
    lv_obj_align(minutes, LV_ALIGN_TOP_MID, 0, 130);
    lv_obj_t *minus = make_button(body, 60, 152, 68, 46, kSurface, 7, true);
    lv_obj_t *minus_label = make_label(minus,
        tr(lyra::i18n::StringId::AppMinuteDown), kTextPrimary);
    lv_obj_center(minus_label);
    lv_obj_add_event_cb(minus, countdown_adjust_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(uintptr_t(0)));
    lv_obj_t *plus = make_button(body, 192, 152, 68, 46, kSurface, 7, true);
    lv_obj_t *plus_label = make_label(plus,
        tr(lyra::i18n::StringId::AppMinuteUp), kTextPrimary);
    lv_obj_center(plus_label);
    lv_obj_add_event_cb(plus, countdown_adjust_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(uintptr_t(1)));

    lv_obj_t *start = make_button(body, 24, 218, 128, 50, kAccentSurface, 7, true);
    lv_obj_t *start_label = make_label(start, tr(s_countdown_running ?
        lyra::i18n::StringId::AppPause : lyra::i18n::StringId::AppStart), kTextPrimary);
    make_marquee(start_label, 116);
    lv_obj_center(start_label);
    lv_obj_add_event_cb(start, countdown_action_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(uintptr_t(0)));
    lv_obj_t *reset = make_button(body, 168, 218, 128, 50, kSurface, 7, true);
    lv_obj_t *reset_label = make_label(reset, tr(lyra::i18n::StringId::AppReset), kTextPrimary);
    make_marquee(reset_label, 116);
    lv_obj_center(reset_label);
    lv_obj_add_event_cb(reset, countdown_action_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(uintptr_t(1)));
    make_setting_toggle(body, 278, tr(lyra::i18n::StringId::TimerAlarm),
        tr(s_timer_alarm_enabled ? lyra::i18n::StringId::On :
                                   lyra::i18n::StringId::Off),
        &s_timer_alarm_enabled);
    if (s_countdown_running) ensure_countdown_timer();
}

void render_app_files()
{
    const bool photos = s_view == View::Photos;
    FileScan &scan = photos ? s_photo_scan : s_ebook_scan;
    const lyra::media::FileKind kind = photos ? lyra::media::FileKind::Image :
                                                lyra::media::FileKind::Text;
    lv_obj_t *header = make_header(tr(photos ? lyra::i18n::StringId::Photos :
                                               lyra::i18n::StringId::Ebook),
                                   View::Apps, true, "");
    lv_obj_t *reload = make_button(header, 272, 4, 40, 36, kBackground, 5, true);
    lv_obj_t *reload_icon = make_label(reload, LV_SYMBOL_REFRESH, kAccent);
    lv_obj_center(reload_icon);
    lv_obj_add_event_cb(reload, file_reload_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *list = make_scroll_body(72);
    if (!scan.complete.load(std::memory_order_acquire)) {
        lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLLABLE);
        start_file_scan(&scan, kind);
        ensure_file_scan_poll();
        lv_obj_t *spinner = lv_spinner_create(list);
        lv_obj_set_size(spinner, 48, 48);
        lv_spinner_set_anim_params(spinner, 900, 250);
        lv_obj_set_style_arc_color(spinner, kDivider, LV_PART_MAIN);
        lv_obj_set_style_arc_color(spinner, kAccent, LV_PART_INDICATOR);
        lv_obj_set_style_arc_width(spinner, 4, LV_PART_MAIN);
        lv_obj_set_style_arc_width(spinner, 4, LV_PART_INDICATOR);
        lv_obj_align(spinner, LV_ALIGN_TOP_MID, 0, 14);
        lv_obj_t *reading = make_label(list,
            tr(lyra::i18n::StringId::ReadingMicroSdFolders), kTextMuted);
        lv_obj_set_width(reading, 300);
        lv_obj_set_style_text_align(reading, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(reading, 10, 76);
        char count_text[32];
        format_u32(lyra::i18n::StringId::AppFilesFound,
            static_cast<uint32_t>(std::min<size_t>(
                scan.found.load(std::memory_order_acquire), UINT32_MAX)),
            count_text, sizeof(count_text));
        s_file_scan_count = make_label(list, count_text, kTextSecondary);
        lv_obj_set_width(s_file_scan_count, 300);
        lv_obj_set_style_text_align(s_file_scan_count, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(s_file_scan_count, 10, 108);
        return;
    }
    s_list_page_count = 0;
    if (!scan.succeeded) {
        const lyra::i18n::StringId error = lyra::media::status().mounted ?
            lyra::i18n::StringId::AppFileScanFailed : lyra::i18n::StringId::NoMicroSdCard;
        lv_obj_t *message = make_label(list, tr(error), kTextMuted);
        lv_obj_set_pos(message, 12, 18);
        return;
    }
    if (!scan.count) {
        lv_obj_t *empty = make_label(list,
            tr(lyra::i18n::StringId::AppNoSupportedFiles), kTextMuted);
        lv_obj_set_pos(empty, 12, 18);
        return;
    }

    const size_t pages = (scan.count + kAppFilePageSize - 1) / kAppFilePageSize;
    s_list_page_count = pages;
    if (s_list_page >= pages) s_list_page = pages - 1;
    const size_t first = s_list_page * kAppFilePageSize;
    const size_t last = std::min(first + kAppFilePageSize, scan.count);
    size_t row = 0;
    for (size_t index = first; index < last; ++index) {
        make_document_row(list, 6 + static_cast<int>(row++) * 58,
                          scan.files[index], s_view);
    }
    make_page_controls(list, 6 + static_cast<int>(row) * 58 + 3,
                       scan.count, kAppFilePageSize);
}

} // namespace lyra::gui::internal
