/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../lyra_gui_internal.h"

namespace lyra::gui::internal {

namespace {

// Power actions call through the media filesystem and audio persistence
// layers, both of which have deeper call chains than the normal GUI event
// handler. Keep the one-shot task large enough for queue serialization and
// safe card unmounting.
constexpr uint32_t kPowerActionTaskStack = 8 * 1024;

struct FactoryResetFailure {
    char message[112];
};

void factory_reset_failure_async_cb(void *context)
{
    auto *failure = static_cast<FactoryResetFailure *>(context);
    if (failure) {
        render(View::SystemSettings);
        show_notice(tr(lyra::i18n::StringId::FactoryReset), failure->message);
        heap_caps_free(failure);
    }
}

void post_factory_reset_failure(esp_err_t result)
{
    auto *failure = static_cast<FactoryResetFailure *>(
        heap_caps_malloc(sizeof(FactoryResetFailure), MALLOC_CAP_8BIT));
    if (!failure) {
        ESP_LOGE(kTag, "could not allocate factory reset error message: %s",
                 esp_err_to_name(result));
        return;
    }
    format_text(lyra::i18n::StringId::FactoryResetFailed,
                esp_err_to_name(result), failure->message, sizeof(failure->message));
    lv_async_call(factory_reset_failure_async_cb, failure);
}

const char *clock_date_format_name()
{
    switch (s_date_format) {
    case DateFormat::DayMonthYear: return tr(lyra::i18n::StringId::DateFormatDayMonth);
    case DateFormat::MonthDayYear: return tr(lyra::i18n::StringId::DateFormatMonthDay);
    case DateFormat::YearMonthDay: return tr(lyra::i18n::StringId::DateFormatYearMonth);
    }
    return tr(lyra::i18n::StringId::DateFormatDayMonth);
}

const char *clock_time_format_name()
{
    return s_use_24_hour ? tr(lyra::i18n::StringId::TwentyFourHour) :
                           tr(lyra::i18n::StringId::TwelveHour);
}

const char *clock_dst_name()
{
    return s_dst_enabled ? tr(lyra::i18n::StringId::DaylightSavingOn) :
                           tr(lyra::i18n::StringId::DaylightSavingOff);
}

} // namespace

void format_clock_time(const lyra::clock::DateTime &value, char *output, size_t capacity)
{
    if (!output || capacity == 0) return;
    if (s_use_24_hour) {
        std::snprintf(output, capacity, "%02d:%02d", value.hour, value.minute);
        return;
    }
    const bool pm = value.hour >= 12;
    int hour = value.hour % 12;
    if (hour == 0) hour = 12;
    std::snprintf(output, capacity, "%d:%02d %s", hour, value.minute,
                  tr(pm ? lyra::i18n::StringId::Pm : lyra::i18n::StringId::Am));
}

void format_clock_date_time(const lyra::clock::DateTime &value,
                            char *output, size_t capacity)
{
    if (!output || capacity == 0) return;
    char time_text[16];
    format_clock_time(value, time_text, sizeof(time_text));
    switch (s_date_format) {
    case DateFormat::DayMonthYear:
        std::snprintf(output, capacity, "%02d/%02d/%04d %s",
                      value.day, value.month, value.year, time_text);
        break;
    case DateFormat::MonthDayYear:
        std::snprintf(output, capacity, "%02d/%02d/%04d %s",
                      value.month, value.day, value.year, time_text);
        break;
    case DateFormat::YearMonthDay:
        std::snprintf(output, capacity, "%04d-%02d-%02d %s",
                      value.year, value.month, value.day, time_text);
        break;
    }
}

void toggle_bool_cb(lv_event_t *event)
{
    lv_obj_t *toggle = static_cast<lv_obj_t *>(lv_event_get_user_data(event));
    bool *value = static_cast<bool *>(lv_obj_get_user_data(toggle));
    *value = !*value;
    if (value == &s_gapless || value == &s_replay_gain || value == &s_quick_seek) {
        save_user_settings();
        if (value == &s_replay_gain) apply_replay_gain_to_current_track();
    }
    if (value == &s_dark_mode) {
        apply_theme_palette();
        save_user_settings();
        render(s_view);
        return;
    }
    if (value == &s_show_nav) {
        // This setting changes the usable page height and dock object tree.
        render(s_view);
        return;
    }
    if (value == &s_speaker_output_enabled) {
        const esp_err_t result = lyra::audio::set_speaker_output_enabled(*value);
        if (result != ESP_OK) {
            *value = !*value;
            ESP_LOGW(kTag, "could not change on-board speaker output: %s",
                     esp_err_to_name(result));
            return;
        }
        save_user_settings();
    }

    lv_obj_set_style_bg_color(toggle, *value ? kAccent : kDivider, 0);
    lv_obj_set_x(lv_obj_get_child(toggle, 0), *value ? 21 : 3);
}

void make_setting_toggle(lv_obj_t *parent, int y, const char *title, const char *subtitle, bool *value)
{
    lv_obj_t *row = make_button(parent, 7, y, 306, 62, kSurface, 6, true);
    lv_obj_t *title_label = make_label(row, title, kTextPrimary);
    make_marquee(title_label, 220);
    lv_obj_align(title_label, LV_ALIGN_LEFT_MID, 12, subtitle == nullptr ? 0 : -10);
    if (subtitle != nullptr) {
        lv_obj_t *sub = make_label(row, subtitle, kTextMuted);
        make_marquee(sub, 220);
        lv_obj_align(sub, LV_ALIGN_LEFT_MID, 12, 12);
    }
    lv_obj_t *toggle = make_box(row, 250, 17, 42, 24, *value ? kAccent : kDivider, 12);
    make_box(toggle, *value ? 21 : 3, 3, 18, 18,
             *value ? kTextOnAccent : kTextPrimary, 9);
    lv_obj_set_user_data(toggle, value);
    lv_obj_add_event_cb(row, toggle_bool_cb, LV_EVENT_CLICKED, toggle);
}

void accent_colour_cb(lv_event_t *event)
{
    const uintptr_t index = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    if (index >= kAccentPaletteCount) return;
    if (s_accent_colour == index) return;
    s_accent_colour = static_cast<uint8_t>(index);
    apply_theme_palette();
    save_user_settings();
    render(s_view);
}

void make_accent_selector(lv_obj_t *parent, int y)
{
    lv_obj_t *card = make_box(parent, 7, y, 306, 110, kSurface, 6);
    lv_obj_t *title = make_label(card, tr(lyra::i18n::StringId::AccentColour), kTextPrimary);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 9);

    for (size_t index = 0; index < kAccentPaletteCount; ++index) {
        const int column = static_cast<int>(index % 4);
        const int row = static_cast<int>(index / 4);
        const AccentPalette &accent = kAccentPalettes[index];
        const uint32_t rgb = s_dark_mode ? accent.dark_rgb : accent.light_rgb;
        lv_obj_t *swatch = make_box(card, 14 + column * 72, 42 + row * 36, 28, 28,
                                    lv_color_hex(rgb), 14);
        lv_obj_set_style_border_width(swatch, s_accent_colour == index ? 3 : 1, 0);
        lv_obj_set_style_border_color(swatch,
                                      s_accent_colour == index ? kTextPrimary : kDivider, 0);
        lv_obj_add_flag(swatch, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(swatch, accent_colour_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<uintptr_t>(index)));
    }
}

void artwork_setting_cb(lv_event_t *event)
{
    const ArtworkSetting setting = static_cast<ArtworkSetting>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    const lyra::media::Status status = lyra::media::status();
    const esp_err_t result = setting == ArtworkSetting::SdCache ?
        lyra::media::set_artwork_sd_cache_enabled(!status.artwork_sd_cache_enabled) :
        lyra::media::set_artwork_size(
            status.artwork_size == lyra::media::kLargeArtworkSize ?
            lyra::media::kDefaultArtworkSize : lyra::media::kLargeArtworkSize);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "could not save artwork setting: %s", esp_err_to_name(result));
    }
    render(View::SystemSettings);
}

void make_artwork_setting_toggle(lv_obj_t *parent, int y, const char *title,
                                 const char *subtitle, bool value,
                                 ArtworkSetting setting)
{
    lv_obj_t *row = make_button(parent, 7, y, 306, 62, kSurface, 6, true);
    lv_obj_t *title_label = make_label(row, title, kTextPrimary);
    make_marquee(title_label, 220);
    lv_obj_align(title_label, LV_ALIGN_LEFT_MID, 12, -10);
    lv_obj_t *sub = make_label(row, subtitle, kTextMuted);
    make_marquee(sub, 220);
    lv_obj_align(sub, LV_ALIGN_LEFT_MID, 12, 12);
    lv_obj_t *toggle = make_box(row, 250, 17, 42, 24, value ? kAccent : kDivider, 12);
    make_box(toggle, value ? 21 : 3, 3, 18, 18,
             value ? kTextOnAccent : kTextPrimary, 9);
    lv_obj_add_event_cb(row, artwork_setting_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(setting)));
}

void volume_slider_cb(lv_event_t *event)
{
    lv_obj_t *slider = lv_event_get_current_target_obj(event);
    const uint8_t display_value = static_cast<uint8_t>(lv_slider_get_value(slider));
    const uint8_t audio_value = audio_volume_percent_from_display(display_value);
    lyra::audio::set_volume(audio_value);
    update_status_volume_label(audio_value);
    lv_obj_t *value_label = static_cast<lv_obj_t *>(lv_obj_get_user_data(slider));
    if (value_label != nullptr) {
        char text[32];
        std::snprintf(text, sizeof(text), "%u%%",
                      static_cast<unsigned>(display_value));
        lv_label_set_text(value_label, text);
    }
}

void volume_slider_released_cb(lv_event_t *)
{
    const esp_err_t result = lyra::audio::save_volume();
    if (result != ESP_OK) ESP_LOGW(kTag, "could not save volume: %s", esp_err_to_name(result));
}

void make_volume_control(lv_obj_t *parent, int y)
{
    const lyra::audio::Status audio_status = lyra::audio::status();
    lv_obj_t *card = make_box(parent, 7, y, 306, 84, kSurface, 6);
    lv_obj_t *title = make_label(card, tr(lyra::i18n::StringId::Volume), kTextPrimary);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 10);
    char value_text[32];
    std::snprintf(value_text, sizeof(value_text), "%u%%",
                  static_cast<unsigned>(display_volume_percent(audio_status.volume_percent)));
    lv_obj_t *value_label = make_label(card, value_text, kAccent);
    lv_obj_align(value_label, LV_ALIGN_TOP_RIGHT, -12, 10);

    lv_obj_t *slider = lv_slider_create(card);
    lv_obj_set_pos(slider, 12, 54);
    lv_obj_set_size(slider, 282, 14);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, display_volume_percent(audio_status.volume_percent), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, kDivider, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, kAccentDark, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, kTextOnAccent, LV_PART_KNOB);
    lv_obj_set_style_border_color(slider, kAccent, LV_PART_KNOB);
    lv_obj_set_style_border_width(slider, 2, LV_PART_KNOB);
    lv_obj_set_user_data(slider, value_label);
    lv_obj_add_event_cb(slider, volume_slider_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(slider, volume_slider_released_cb, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(slider, volume_slider_released_cb, LV_EVENT_PRESS_LOST, nullptr);
}

void brightness_slider_cb(lv_event_t *event)
{
    lv_obj_t *slider = lv_event_get_current_target_obj(event);
    s_brightness_percent = static_cast<uint8_t>(lv_slider_get_value(slider));
    const esp_err_t result = lyra_board_display_set_brightness(s_brightness_percent);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "could not set display brightness: %s", esp_err_to_name(result));
    }
    lv_obj_t *value_label = static_cast<lv_obj_t *>(lv_obj_get_user_data(slider));
    if (value_label != nullptr) {
        char text[16];
        std::snprintf(text, sizeof(text), "%u%%", static_cast<unsigned>(s_brightness_percent));
        lv_label_set_text(value_label, text);
    }
}

void brightness_slider_released_cb(lv_event_t *)
{
    save_user_settings();
}

void make_brightness_control(lv_obj_t *parent, int y)
{
    lv_obj_t *card = make_box(parent, 7, y, 306, 84, kSurface, 6);
    lv_obj_t *title = make_label(card, tr(lyra::i18n::StringId::Brightness), kTextPrimary);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 10);
    char value_text[16];
    std::snprintf(value_text, sizeof(value_text), "%u%%", static_cast<unsigned>(s_brightness_percent));
    lv_obj_t *value_label = make_label(card, value_text, kAccent);
    lv_obj_align(value_label, LV_ALIGN_TOP_RIGHT, -12, 10);

    lv_obj_t *slider = lv_slider_create(card);
    lv_obj_set_pos(slider, 12, 54);
    lv_obj_set_size(slider, 282, 14);
    lv_slider_set_range(slider, 1, 100);
    lv_slider_set_value(slider, s_brightness_percent, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, kDivider, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, kAccentDark, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, kTextOnAccent, LV_PART_KNOB);
    lv_obj_set_style_border_color(slider, kAccent, LV_PART_KNOB);
    lv_obj_set_style_border_width(slider, 2, LV_PART_KNOB);
    lv_obj_set_user_data(slider, value_label);
    lv_obj_add_event_cb(slider, brightness_slider_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(slider, brightness_slider_released_cb, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(slider, brightness_slider_released_cb, LV_EVENT_PRESS_LOST, nullptr);
}

const char *sort_section_name(lyra::media::SortSection section)
{
    switch (section) {
    case lyra::media::SortSection::Songs: return tr(lyra::i18n::StringId::Songs);
    case lyra::media::SortSection::Albums: return tr(lyra::i18n::StringId::Albums);
    case lyra::media::SortSection::Artists: return tr(lyra::i18n::StringId::Artists);
    }
    return tr(lyra::i18n::StringId::Songs);
}

const char *sort_field_name(lyra::media::SortField field)
{
    switch (field) {
    case lyra::media::SortField::Title: return tr(lyra::i18n::StringId::SortTitle);
    case lyra::media::SortField::Album: return tr(lyra::i18n::StringId::SortAlbum);
    case lyra::media::SortField::TrackNumber: return tr(lyra::i18n::StringId::SortTrackNumber);
    case lyra::media::SortField::Duration: return tr(lyra::i18n::StringId::SortDuration);
    case lyra::media::SortField::Artist: return tr(lyra::i18n::StringId::SortArtist);
    case lyra::media::SortField::DateModified: return tr(lyra::i18n::StringId::SortDateModified);
    }
    return tr(lyra::i18n::StringId::SortTitle);
}

const char *sort_direction_name(lyra::media::SortDirection direction)
{
    return direction == lyra::media::SortDirection::Ascending ?
        tr(lyra::i18n::StringId::Ascending) : tr(lyra::i18n::StringId::Descending);
}

void sorting_section_cb(lv_event_t *event)
{
    s_sort_section = static_cast<lyra::media::SortSection>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    navigate_to(View::SortingOptions);
}

void sorting_option_cb(lv_event_t *event)
{
    const uintptr_t value = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    const auto field = static_cast<lyra::media::SortField>(value / 2u);
    const auto direction = static_cast<lyra::media::SortDirection>(value % 2u);
    const esp_err_t result = lyra::media::set_sort_setting(s_sort_section, field, direction);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "could not save %s sorting setting: %s",
                 sort_section_name(s_sort_section), esp_err_to_name(result));
    }
    render(View::SortingOptions);
}

void render_sorting_settings()
{
    make_header(tr(lyra::i18n::StringId::Sorting), View::Settings, true);
    lv_obj_t *body = make_scroll_body(72);
    constexpr lyra::media::SortSection sections[] = {
        lyra::media::SortSection::Songs,
        lyra::media::SortSection::Albums,
        lyra::media::SortSection::Artists,
    };
    const char *subtitles[] = {
        tr(lyra::i18n::StringId::ChooseSongOrder),
        tr(lyra::i18n::StringId::ChooseAlbumOrder),
        tr(lyra::i18n::StringId::ChooseArtistOrder),
    };
    for (size_t index = 0; index < 3; ++index) {
        lv_obj_t *row = make_row(body, static_cast<int>(index) * 58, LV_SYMBOL_LIST,
                                 sort_section_name(sections[index]), subtitles[index],
                                 View::SortingOptions, 54);
        lv_obj_remove_event_cb(row, route_cb);
        lv_obj_add_event_cb(row, sorting_section_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<uintptr_t>(sections[index])));
    }
}

void make_sorting_option_row(lv_obj_t *parent, int y, lyra::media::SortField field,
                             lyra::media::SortDirection direction,
                             const lyra::media::SortSetting &selected)
{
    const bool active = selected.field == field && selected.direction == direction;
    lv_obj_t *row = make_button(parent, 7, y, 306, 54,
                                active ? kAccentSurface : kSurface, 6);
    char title[64];
    format_text_text(lyra::i18n::StringId::SortFieldDirection,
                     sort_field_name(field), sort_direction_name(direction),
                     title, sizeof(title));
    lv_obj_t *label = make_label(row, title, active ? kAccent : kTextPrimary);
    make_marquee(label, 232);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 12, 0);
    if (active) {
        lv_obj_t *check = make_label(row, LV_SYMBOL_OK, kAccent);
        lv_obj_align(check, LV_ALIGN_RIGHT_MID, -14, 0);
    }
    const uintptr_t payload = static_cast<uintptr_t>(field) * 2u +
                              static_cast<uintptr_t>(direction);
    lv_obj_add_event_cb(row, sorting_option_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(payload));
}

void render_sorting_options()
{
    char title[32];
    format_text(lyra::i18n::StringId::SortBy, sort_section_name(s_sort_section),
                title, sizeof(title));
    make_header(title, View::SortingSettings, true);
    lv_obj_t *body = make_scroll_body(72);
    const lyra::media::SortSetting selected = lyra::media::sort_setting(s_sort_section);
    constexpr lyra::media::SortField fields[] = {
        lyra::media::SortField::Title,
        lyra::media::SortField::Album,
        lyra::media::SortField::TrackNumber,
        lyra::media::SortField::Duration,
        lyra::media::SortField::Artist,
        lyra::media::SortField::DateModified,
    };
    constexpr lyra::media::SortDirection directions[] = {
        lyra::media::SortDirection::Ascending,
        lyra::media::SortDirection::Descending,
    };
    size_t row = 0;
    for (const auto field : fields) {
        for (const auto direction : directions) {
            make_sorting_option_row(body, static_cast<int>(row++ * 58), field,
                                    direction, selected);
        }
    }
}

void clock_settings_cb(lv_event_t *)
{
    navigate_to(View::ClockSettings);
}

void begin_clock_input(ClockInputKind kind)
{
    const lyra::clock::DateTime current = lyra::clock::now();
    s_clock_input_kind = kind;
    s_clock_input_cursor = 0;
    s_clock_input_pm = current.hour >= 12;
    if (kind == ClockInputKind::Time) {
        int hour = current.hour;
        if (!s_use_24_hour) {
            hour %= 12;
            if (hour == 0) hour = 12;
        }
        std::snprintf(s_clock_input_digits, sizeof(s_clock_input_digits), "%02d%02d",
                      hour, current.minute);
        navigate_to(View::ClockTimeSettings);
        return;
    }

    switch (s_date_format) {
    case DateFormat::DayMonthYear:
        std::snprintf(s_clock_input_digits, sizeof(s_clock_input_digits), "%02d%02d%04d",
                      current.day, current.month, current.year);
        break;
    case DateFormat::MonthDayYear:
        std::snprintf(s_clock_input_digits, sizeof(s_clock_input_digits), "%02d%02d%04d",
                      current.month, current.day, current.year);
        break;
    case DateFormat::YearMonthDay:
        std::snprintf(s_clock_input_digits, sizeof(s_clock_input_digits), "%04d%02d%02d",
                      current.year, current.month, current.day);
        break;
    }
    navigate_to(View::ClockDateSettings);
}

void clock_time_settings_cb(lv_event_t *)
{
    begin_clock_input(ClockInputKind::Time);
}

void clock_date_settings_cb(lv_event_t *)
{
    begin_clock_input(ClockInputKind::Date);
}

void clock_date_format_cb(lv_event_t *)
{
    s_date_format = static_cast<DateFormat>(
        (static_cast<uint8_t>(s_date_format) + 1u) % 3u);
    save_user_settings();
    render(View::ClockSettings);
}

void clock_time_format_cb(lv_event_t *)
{
    s_use_24_hour = !s_use_24_hour;
    save_user_settings();
    render(View::ClockSettings);
}

void clock_dst_cb(lv_event_t *)
{
    const bool enabled = !s_dst_enabled;
    const esp_err_t result = lyra::clock::shift_minutes(enabled ? 60 : -60);
    if (result != ESP_OK) {
        show_notice(tr(lyra::i18n::StringId::Clock),
                    tr(lyra::i18n::StringId::ClockInvalid));
        return;
    }
    s_dst_enabled = enabled;
    save_user_settings();
    render(View::ClockSettings);
}

void clock_input_digit_cb(lv_event_t *event)
{
    const uintptr_t payload = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    if (payload < 1 || payload > 10) return;
    const size_t length = s_clock_input_kind == ClockInputKind::Time ? 4 : 8;
    if (s_clock_input_cursor >= length) return;
    s_clock_input_digits[s_clock_input_cursor] = static_cast<char>('0' + payload - 1u);
    if (s_clock_input_cursor + 1 < length) ++s_clock_input_cursor;
    render(s_clock_input_kind == ClockInputKind::Time ? View::ClockTimeSettings :
                                                        View::ClockDateSettings);
}

void clock_input_cursor_cb(lv_event_t *event)
{
    const uintptr_t payload = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    const size_t length = s_clock_input_kind == ClockInputKind::Time ? 4 : 8;
    if (payload == 1 && s_clock_input_cursor > 0) --s_clock_input_cursor;
    if (payload == 2 && s_clock_input_cursor + 1 < length) ++s_clock_input_cursor;
    render(s_clock_input_kind == ClockInputKind::Time ? View::ClockTimeSettings :
                                                        View::ClockDateSettings);
}

void clock_input_meridiem_cb(lv_event_t *)
{
    s_clock_input_pm = !s_clock_input_pm;
    render(View::ClockTimeSettings);
}

int clock_input_value(size_t offset, size_t count)
{
    int value = 0;
    for (size_t index = 0; index < count; ++index) {
        const char digit = s_clock_input_digits[offset + index];
        if (digit < '0' || digit > '9') return -1;
        value = value * 10 + (digit - '0');
    }
    return value;
}

void clock_input_save_cb(lv_event_t *)
{
    lyra::clock::DateTime value = lyra::clock::now();
    if (s_clock_input_kind == ClockInputKind::Time) {
        int hour = clock_input_value(0, 2);
        const int minute = clock_input_value(2, 2);
        if (hour < 0 || minute < 0) return;
        if (s_use_24_hour) {
            value.hour = hour;
        } else {
            if (hour < 1 || hour > 12) {
                show_notice(tr(lyra::i18n::StringId::Clock),
                            tr(lyra::i18n::StringId::ClockInvalid));
                return;
            }
            value.hour = hour % 12 + (s_clock_input_pm ? 12 : 0);
        }
        value.minute = minute;
    } else {
        if (s_date_format == DateFormat::YearMonthDay) {
            value.year = clock_input_value(0, 4);
            value.month = clock_input_value(4, 2);
            value.day = clock_input_value(6, 2);
        } else {
            const int first = clock_input_value(0, 2);
            const int second = clock_input_value(2, 2);
            value.year = clock_input_value(4, 4);
            value.day = first;
            if (s_date_format == DateFormat::DayMonthYear) {
                value.month = second;
            } else {
                value.month = first;
                value.day = second;
            }
        }
        if (value.year < 0 || value.month < 0 || value.day < 0) return;
    }

    const esp_err_t result = lyra::clock::set(value);
    if (result != ESP_OK) {
        show_notice(tr(lyra::i18n::StringId::Clock),
                    tr(lyra::i18n::StringId::ClockInvalid));
        return;
    }
    navigate_back(View::ClockSettings);
}

void clock_input_cancel_cb(lv_event_t *)
{
    navigate_back(View::ClockSettings);
}

void make_clock_input_display(lv_obj_t *parent)
{
    constexpr int kDisplayWidth = 306;
    constexpr int kDigitWidth = 24;
    constexpr int kSeparatorWidth = 12;
    const size_t length = s_clock_input_kind == ClockInputKind::Time ? 4 : 8;
    const size_t separator_count = s_clock_input_kind == ClockInputKind::Time ? 1 : 2;
    const int total_width = static_cast<int>(length) * kDigitWidth +
                            static_cast<int>(separator_count) * kSeparatorWidth;
    lv_obj_t *display = make_box(parent, 7, 6, kDisplayWidth, 54, kAccentSurface, 6);
    int x = (kDisplayWidth - total_width) / 2;
    for (size_t index = 0; index < length; ++index) {
        char digit[2] = {s_clock_input_digits[index], '\0'};
        lv_obj_t *label = make_label(display, digit,
                                     index == s_clock_input_cursor ? kAccent : kTextPrimary);
        lv_obj_set_width(label, kDigitWidth);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(label, x, 12);
        if (index == s_clock_input_cursor) {
            make_box(display, x + 3, 40, kDigitWidth - 6, 2, kAccent, 1);
        }
        x += kDigitWidth;
        const bool separator = s_clock_input_kind == ClockInputKind::Time ? index == 1 :
            (s_date_format == DateFormat::YearMonthDay ? index == 3 || index == 5 :
                                                          index == 1 || index == 3);
        if (separator) {
            const char separator_text[] = ":";
            const char *date_separator = s_date_format == DateFormat::YearMonthDay ? "-" : "/";
            lv_obj_t *label = make_label(display,
                                         s_clock_input_kind == ClockInputKind::Time ?
                                             separator_text : date_separator,
                                         kTextSecondary);
            lv_obj_set_width(label, kSeparatorWidth);
            lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_pos(label, x, 12);
            x += kSeparatorWidth;
        }
    }
}

void make_clock_input_keypad(lv_obj_t *parent, int y)
{
    constexpr const char *kDigits[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"};
    for (size_t index = 0; index < 9; ++index) {
        const int row = static_cast<int>(index / 3);
        const int column = static_cast<int>(index % 3);
        lv_obj_t *button = make_button(parent, 18 + column * 96, y + row * 46,
                                       86, 40, kSurfaceRaised, 6);
        lv_obj_t *label = make_label(button, kDigits[index], kTextPrimary);
        lv_obj_center(label);
        lv_obj_add_event_cb(button, clock_input_digit_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(index + 2u));
    }

    lv_obj_t *left = make_button(parent, 18, y + 138, 86, 40, kSurfaceRaised, 6);
    lv_obj_t *left_label = make_label(left, LV_SYMBOL_LEFT, kTextPrimary);
    lv_obj_center(left_label);
    lv_obj_add_event_cb(left, clock_input_cursor_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(1)));

    lv_obj_t *zero = make_button(parent, 114, y + 138, 86, 40, kSurfaceRaised, 6);
    lv_obj_t *zero_label = make_label(zero, kDigits[9], kTextPrimary);
    lv_obj_center(zero_label);
    lv_obj_add_event_cb(zero, clock_input_digit_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(1)));

    lv_obj_t *right = make_button(parent, 210, y + 138, 86, 40, kSurfaceRaised, 6);
    lv_obj_t *right_label = make_label(right, LV_SYMBOL_RIGHT, kTextPrimary);
    lv_obj_center(right_label);
    lv_obj_add_event_cb(right, clock_input_cursor_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(2)));
}

void render_clock_input(ClockInputKind kind)
{
    s_clock_input_kind = kind;
    const bool is_time = kind == ClockInputKind::Time;
    make_header(is_time ? tr(lyra::i18n::StringId::SetTime) :
                          tr(lyra::i18n::StringId::SetDate),
                View::ClockSettings, true, nullptr, clock_input_cancel_cb);
    lv_obj_t *body = make_box(s_screen, 0, 72, kScreenWidth, content_height(72), kBackground);
    make_clock_input_display(body);
    if (is_time && !s_use_24_hour) {
        lv_obj_t *meridiem = make_button(body, 126, 66, 68, 32, kSurfaceRaised, 6);
        lv_obj_t *label = make_label(meridiem,
                                     tr(s_clock_input_pm ? lyra::i18n::StringId::Pm :
                                                             lyra::i18n::StringId::Am),
                                     kAccent);
        lv_obj_center(label);
        lv_obj_add_event_cb(meridiem, clock_input_meridiem_cb, LV_EVENT_CLICKED, nullptr);
    }
    make_clock_input_keypad(body, 106);

    lv_obj_t *save = make_button(body, 14, 300, 136, 44, kAccentDark, 7);
    lv_obj_t *save_label = make_label(save, tr(lyra::i18n::StringId::SaveKey), kTextOnAccent);
    lv_obj_center(save_label);
    lv_obj_add_event_cb(save, clock_input_save_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *cancel = make_button(body, 164, 300, 136, 44, kSurface, 7);
    lv_obj_t *cancel_label = make_label(cancel, tr(lyra::i18n::StringId::Cancel), kTextSecondary);
    lv_obj_center(cancel_label);
    lv_obj_add_event_cb(cancel, clock_input_cancel_cb, LV_EVENT_CLICKED, nullptr);
}

void render_clock_time_settings()
{
    render_clock_input(ClockInputKind::Time);
}

void render_clock_date_settings()
{
    render_clock_input(ClockInputKind::Date);
}

void render_clock_settings()
{
    make_header(tr(lyra::i18n::StringId::Clock), View::SystemSettings, true);
    lv_obj_t *body = make_scroll_body(72);
    lv_obj_t *summary = make_box(body, 7, 0, 306, 58, kAccentSurface, 6);
    lv_obj_t *summary_title = make_label(summary, tr(lyra::i18n::StringId::TimeAndDate), kTextPrimary);
    lv_obj_align(summary_title, LV_ALIGN_LEFT_MID, 12, -10);
    char summary_value[32];
    format_clock_date_time(lyra::clock::now(), summary_value, sizeof(summary_value));
    lv_obj_t *summary_label = make_label(summary, summary_value, kAccent);
    lv_obj_align(summary_label, LV_ALIGN_LEFT_MID, 12, 12);

    lv_obj_t *time = make_row(body, 64, LV_SYMBOL_SETTINGS,
                              tr(lyra::i18n::StringId::SetTime), nullptr,
                              View::ClockTimeSettings, 54);
    lv_obj_remove_event_cb(time, route_cb);
    lv_obj_add_event_cb(time, clock_time_settings_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *date = make_row(body, 122, LV_SYMBOL_SETTINGS,
                              tr(lyra::i18n::StringId::SetDate), nullptr,
                              View::ClockDateSettings, 54);
    lv_obj_remove_event_cb(date, route_cb);
    lv_obj_add_event_cb(date, clock_date_settings_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *date_format = make_row(body, 180, LV_SYMBOL_SETTINGS,
                                     tr(lyra::i18n::StringId::DateFormat),
                                     clock_date_format_name(), View::ClockSettings, 54);
    lv_obj_remove_event_cb(date_format, route_cb);
    lv_obj_add_event_cb(date_format, clock_date_format_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *time_format = make_row(body, 238, LV_SYMBOL_SETTINGS,
                                     tr(lyra::i18n::StringId::TimeFormat),
                                     clock_time_format_name(), View::ClockSettings, 54);
    lv_obj_remove_event_cb(time_format, route_cb);
    lv_obj_add_event_cb(time_format, clock_time_format_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *dst = make_row(body, 296, LV_SYMBOL_SETTINGS,
                             tr(lyra::i18n::StringId::DaylightSaving), clock_dst_name(),
                             View::ClockSettings, 54);
    lv_obj_remove_event_cb(dst, route_cb);
    lv_obj_add_event_cb(dst, clock_dst_cb, LV_EVENT_CLICKED, nullptr);
}

void language_option_cb(lv_event_t *event)
{
    const uint8_t value = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(
        lv_event_get_user_data(event)));
    if (!lyra::i18n::is_valid_language(value)) return;

    const auto language = static_cast<lyra::i18n::Language>(value);
    const bool completing_initial_setup = s_language_setup_pending;
    if (language == lyra::i18n::current_language() && !completing_initial_setup) return;

    lyra::i18n::set_language(language);
    // These buffers contain translated status messages rather than user data.
    // Drop them so a later render cannot display text from the previous locale.
    s_search_error[0] = '\0';
    s_database_status[0] = '\0';
    s_debug_status[0] = '\0';
    const esp_err_t result = save_user_settings();
    if (result != ESP_OK) {
        char message[112];
        format_text(lyra::i18n::StringId::LanguageSaveFailed,
                    esp_err_to_name(result), message, sizeof(message));
        show_notice(tr(lyra::i18n::StringId::SelectLanguage), message);
        return;
    }
    if (completing_initial_setup) {
        s_language_setup_pending = false;
        render(View::Menu);
    } else {
        render(s_view);
    }
}

void render_language_options()
{
    make_header(s_language_setup_pending ? tr(lyra::i18n::StringId::SelectLanguage) :
                                            tr(lyra::i18n::StringId::Language),
                View::SystemSettings, !s_language_setup_pending);
    lv_obj_t *body = make_scroll_body(72);
    for (size_t index = 0; index < lyra::i18n::kLanguageCount; ++index) {
        const auto language = static_cast<lyra::i18n::Language>(index);
        make_playback_option_row(body, static_cast<int>(index) * 58,
                                 lyra::i18n::language_name(language),
                                 language == lyra::i18n::current_language(),
                                 language_option_cb, static_cast<uintptr_t>(index));
    }
}

void make_playback_option_row(lv_obj_t *parent, int y, const char *title, bool active,
                              lv_event_cb_t callback, uintptr_t value)
{
    lv_obj_t *row = make_button(parent, 7, y, 306, 54,
                                active ? kAccentSurface : kSurface, 6);
    lv_obj_t *label = make_label(row, title, active ? kAccent : kTextPrimary);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 12, 0);
    if (active) {
        lv_obj_t *check = make_label(row, LV_SYMBOL_OK, kAccent);
        lv_obj_align(check, LV_ALIGN_RIGHT_MID, -14, 0);
    }
    lv_obj_add_event_cb(row, callback, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(value));
}

void crossfade_option_cb(lv_event_t *event)
{
    s_crossfade_seconds = static_cast<uint8_t>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    s_crossfade_fade_in_started_us = 0;
    s_crossfade_fade_in_ends_us = 0;
    s_crossfade_fade_out_started_us = 0;
    s_crossfade_fade_out_ends_us = 0;
    s_crossfade_transition_direction = 0;
    s_crossfade_pause_pending = false;
    lyra::audio::set_transition_gain(100);
    save_user_settings();
    navigate_back(View::PlaybackSettings);
}

void sleep_timer_option_cb(lv_event_t *event)
{
    s_sleep_timer_minutes = static_cast<uint16_t>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    s_sleep_timer_deadline_us = s_sleep_timer_minutes == 0 ? 0 :
        esp_timer_get_time() + static_cast<int64_t>(s_sleep_timer_minutes) * 60 * 1000 * 1000;
    navigate_back(View::PlaybackSettings);
}

void render_crossfade_options()
{
    make_header(tr(lyra::i18n::StringId::Crossfade), View::PlaybackSettings, true);
    lv_obj_t *body = make_scroll_body(72);
    constexpr uint8_t values[] = {0, 1, 2, 4, 6};
    const char *labels[] = {tr(lyra::i18n::StringId::Off),
                            tr(lyra::i18n::StringId::OneSecond),
                            tr(lyra::i18n::StringId::TwoSeconds),
                            tr(lyra::i18n::StringId::FourSeconds),
                            tr(lyra::i18n::StringId::SixSeconds)};
    for (size_t index = 0; index < sizeof(values) / sizeof(values[0]); ++index) {
        make_playback_option_row(body, static_cast<int>(index) * 58, labels[index],
                                 s_crossfade_seconds == values[index], crossfade_option_cb,
                                 static_cast<uintptr_t>(values[index]));
    }
}

void render_sleep_timer_options()
{
    make_header(tr(lyra::i18n::StringId::SleepTimer), View::PlaybackSettings, true);
    lv_obj_t *body = make_scroll_body(72);
    constexpr uint16_t values[] = {0, 15, 30, 45, 60};
    const char *labels[] = {tr(lyra::i18n::StringId::Off),
                            tr(lyra::i18n::StringId::FifteenMinutes),
                            tr(lyra::i18n::StringId::ThirtyMinutes),
                            tr(lyra::i18n::StringId::FortyFiveMinutes),
                            tr(lyra::i18n::StringId::SixtyMinutes)};
    for (size_t index = 0; index < sizeof(values) / sizeof(values[0]); ++index) {
        make_playback_option_row(body, static_cast<int>(index) * 58, labels[index],
                                 s_sleep_timer_minutes == values[index], sleep_timer_option_cb,
                                 static_cast<uintptr_t>(values[index]));
    }
}

void render_settings_menu()
{
    make_header(tr(lyra::i18n::StringId::Settings), View::Menu, true);
    lv_obj_t *body = make_scroll_body(72);
    make_row(body, 0, LV_SYMBOL_PLAY, tr(lyra::i18n::StringId::Playback), nullptr, View::PlaybackSettings, 54);
    make_row(body, 58, LV_SYMBOL_VOLUME_MID, tr(lyra::i18n::StringId::Sound), nullptr, View::SoundSettings, 54);
    make_row(body, 116, LV_SYMBOL_IMAGE, tr(lyra::i18n::StringId::Display), nullptr, View::DisplaySettings, 54);
    make_row(body, 174, LV_SYMBOL_LIST, tr(lyra::i18n::StringId::Sorting), nullptr, View::SortingSettings, 54);
    make_row(body, 232, LV_SYMBOL_SETTINGS, tr(lyra::i18n::StringId::System), nullptr, View::SystemSettings, 54);
    make_row(body, 290, LV_SYMBOL_WARNING, tr(lyra::i18n::StringId::About), nullptr, View::About, 54);
}

void scan_library_cb(lv_event_t *)
{
    if (lyra::media::start_scan() != ESP_OK) {
        render(View::SystemSettings);
        return;
    }
    s_scan_overlay = make_box(s_screen, 0, 0, kScreenWidth, kScreenHeight, kOverlay);
    lv_obj_set_style_bg_opa(s_scan_overlay, LV_OPA_80, 0);
    lv_obj_add_flag(s_scan_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *dialog = make_box(s_scan_overlay, 24, 126, 272, 228, kSurfaceRaised, 14);
    lv_obj_set_style_border_width(dialog, 1, 0);
    lv_obj_set_style_border_color(dialog, kAccentDark, 0);
    lv_obj_t *spinner = lv_spinner_create(dialog);
    lv_obj_set_size(spinner, 62, 62);
    lv_obj_align(spinner, LV_ALIGN_TOP_MID, 0, 25);
    lv_spinner_set_anim_params(spinner, 900, 250);
    lv_obj_set_style_arc_color(spinner, kDivider, LV_PART_MAIN);
    lv_obj_set_style_arc_color(spinner, kAccent, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(spinner, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_width(spinner, 6, LV_PART_INDICATOR);
    lv_obj_t *title = make_label(dialog, tr(lyra::i18n::StringId::ScanningMusicLibrary), kTextPrimary);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 105);
    char initial_count[32];
    format_count(lyra::i18n::StringId::ScanSongsFound, 0,
                 initial_count, sizeof(initial_count));
    s_scan_count_label = make_label(dialog, initial_count, kAccent);
    lv_obj_align(s_scan_count_label, LV_ALIGN_TOP_MID, 0, 137);
    s_scan_phase_label = make_label(dialog, tr(lyra::i18n::StringId::ReadingMicroSdFolders), kTextSecondary);
    lv_obj_align(s_scan_phase_label, LV_ALIGN_TOP_MID, 0, 169);
}

void save_active_queue_snapshot(const lyra::audio::Status &audio_status)
{
    if (!s_has_active_queue) return;
    const size_t count = playback_queue_count();
    size_t current = 0;
    if (count == 0 || !current_queue_position(&current)) return;

    uint32_t playback_position_ms = s_saved_playback_position_pending ?
                                    s_saved_playback_position_ms : 0;
    if (!s_saved_playback_position_pending && audio_status.playing && !audio_status.eof &&
        audio_status.last_error == ESP_OK) {
        auto *current_track = static_cast<lyra::media::Track *>(heap_caps_malloc(
            sizeof(lyra::media::Track), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!current_track) {
            current_track = static_cast<lyra::media::Track *>(heap_caps_malloc(
                sizeof(lyra::media::Track), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        }
        if (current_track && lyra::media::track_at(s_current_track, current_track) &&
            std::strcmp(audio_status.path, current_track->path) == 0) {
            playback_position_ms = audio_status.position_ms;
            if (audio_status.duration_ms > 0 && playback_position_ms > audio_status.duration_ms) {
                playback_position_ms = audio_status.duration_ms;
            }
        }
        heap_caps_free(current_track);
    }

    auto *tracks = static_cast<size_t *>(heap_caps_malloc(
        count * sizeof(size_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!tracks) {
        tracks = static_cast<size_t *>(heap_caps_malloc(
            count * sizeof(size_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    if (!tracks) {
        ESP_LOGW(kTag, "could not allocate queue snapshot");
        return;
    }
    bool complete = true;
    for (size_t position = 0; position < count; ++position) {
        if (!queue_track_at(position, &tracks[position])) {
            complete = false;
            break;
        }
    }
    const esp_err_t result = complete ?
        lyra::media::save_queue_snapshot(tracks, count, current, playback_position_ms) : ESP_FAIL;
    heap_caps_free(tracks);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "could not save queue snapshot: %s", esp_err_to_name(result));
    }
}

void power_action_task(void *context)
{
    const PowerAction action = static_cast<PowerAction>(reinterpret_cast<uintptr_t>(context));
    const bool factory_reset = action == PowerAction::FactoryReset;
    // Give LVGL time to present the shutdown status before filesystem work.
    vTaskDelay(pdMS_TO_TICKS(250));
    const lyra::audio::Status audio_status = lyra::audio::status();
    // A factory reset must not create or update any files on the MicroSD card,
    // so it deliberately skips the normal shutdown queue/volume persistence.
    lyra::audio::stop();
    // The decoder owns an open MicroSD FILE while it is active. Wait for its
    // worker to release that handle before attempting the filesystem unmount.
    for (int wait_count = 0; wait_count < 200; ++wait_count) {
        if (!lyra::audio::status().playing) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (!factory_reset) {
        // Stop playback before writing the queue snapshot so the filesystem
        // write cannot contend with the decoder's changing SD reads.
        save_active_queue_snapshot(audio_status);
        const esp_err_t volume_result = lyra::audio::save_volume();
        if (volume_result != ESP_OK) ESP_LOGW(kTag, "could not save volume before shutdown: %s",
                                              esp_err_to_name(volume_result));
    }

    const esp_err_t media_result = lyra::media::shutdown();
    if (media_result != ESP_OK) {
        ESP_LOGE(kTag, "safe shutdown failed: %s", esp_err_to_name(media_result));
        if (factory_reset) post_factory_reset_failure(media_result);
        // Do not restart or sleep after an unmount failure: preserving the card
        // takes priority over completing the requested power action.
        vTaskDelete(nullptr);
        return;
    }

    if (factory_reset) {
        // This is the default flash NVS partition only. The MicroSD card has
        // already been unmounted and no SD-backed state was saved above.
        esp_err_t result = nvs_flash_erase();
        if (result == ESP_OK) result = nvs_flash_init();
        if (result == ESP_ERR_INVALID_STATE) result = ESP_OK;
        if (result != ESP_OK) {
            ESP_LOGE(kTag, "factory reset failed: %s", esp_err_to_name(result));
            post_factory_reset_failure(result);
            vTaskDelete(nullptr);
            return;
        }
        ESP_LOGI(kTag, "factory reset complete; restarting");
        lyra_board_display_set_backlight(false);
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
        vTaskDelete(nullptr);
        return;
    }

    lyra_board_display_set_backlight(false);
    vTaskDelay(pdMS_TO_TICKS(100));
    if (action == PowerAction::Reboot) {
        ESP_LOGI(kTag, "restarting after safe MicroSD unmount");
        esp_restart();
    }
    ESP_LOGI(kTag, "entering deep sleep after safe MicroSD unmount");
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_deep_sleep_start();
}

void confirm_power_action_cb(lv_event_t *event)
{
    const PowerAction action = static_cast<PowerAction>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    style_root();
    lv_obj_t *label = make_label(s_screen,
                                 action == PowerAction::Reboot ?
                                 tr(lyra::i18n::StringId::Rebooting) :
                                 action == PowerAction::FactoryReset ?
                                 tr(lyra::i18n::StringId::FactoryResetting) :
                                 tr(lyra::i18n::StringId::PoweringOff), kTextPrimary);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, -10);
    if (xTaskCreatePinnedToCore(power_action_task, "lyra_power", kPowerActionTaskStack,
                                reinterpret_cast<void *>(static_cast<uintptr_t>(action)),
                                3, nullptr, 0) != pdPASS) {
        ESP_LOGE(kTag, "could not create power action task");
        render(View::SystemSettings);
    }
}

void cancel_power_action_cb(lv_event_t *event)
{
    lv_obj_delete(static_cast<lv_obj_t *>(lv_event_get_user_data(event)));
}

void show_power_confirmation(PowerAction action)
{
    lv_obj_t *overlay = make_box(s_screen, 0, 0, kScreenWidth, kScreenHeight,
                                 kOverlay);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_90, 0);
    lv_obj_move_foreground(overlay);
    lv_obj_t *dialog = make_box(overlay, 20, 132, 280, 216, kSurfaceRaised, 12);
    lv_obj_t *title = make_label(dialog, action == PowerAction::Reboot ?
                                 tr(lyra::i18n::StringId::RebootLyraQuestion) :
                                 action == PowerAction::FactoryReset ?
                                 tr(lyra::i18n::StringId::FactoryResetQuestion) :
                                 tr(lyra::i18n::StringId::PowerOffLyraQuestion), kTextPrimary);
    lv_obj_set_width(title, 240);
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 22);
    lv_obj_t *message = make_label(dialog,
                                   action == PowerAction::Reboot ?
                                       tr(lyra::i18n::StringId::RebootMessage) :
                                       action == PowerAction::FactoryReset ?
                                       tr(lyra::i18n::StringId::FactoryResetMessage) :
                                       tr(lyra::i18n::StringId::PowerOffMessage),
                                   kTextSecondary);
    lv_obj_set_width(message, 240);
    lv_label_set_long_mode(message, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(message, LV_ALIGN_CENTER, 0, -15);

    lv_obj_t *cancel = make_button(dialog, 14, 152, 116, 48, kSurface, 7);
    lv_obj_t *cancel_label = make_label(cancel, tr(lyra::i18n::StringId::Cancel), kTextSecondary);
    lv_obj_center(cancel_label);
    lv_obj_add_event_cb(cancel, cancel_power_action_cb, LV_EVENT_CLICKED, overlay);

    lv_obj_t *confirm = make_button(dialog, 150, 152, 116, 48,
                                    action == PowerAction::Reboot ? kAccentDark : lv_color_hex(0x991B1B), 7);
    lv_obj_t *confirm_label = make_label(confirm, action == PowerAction::Reboot ?
                                         tr(lyra::i18n::StringId::Reboot) :
                                         action == PowerAction::FactoryReset ?
                                         tr(lyra::i18n::StringId::FactoryResetButton) :
                                         tr(lyra::i18n::StringId::PowerOffButton), kTextOnAccent);
    lv_obj_center(confirm_label);
    lv_obj_add_event_cb(confirm, confirm_power_action_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(action)));
}

void reboot_cb(lv_event_t *) { show_power_confirmation(PowerAction::Reboot); }
void power_off_cb(lv_event_t *) { show_power_confirmation(PowerAction::PowerOff); }
void factory_reset_cb(lv_event_t *) { show_power_confirmation(PowerAction::FactoryReset); }

void confirm_database_action_cb(lv_event_t *event)
{
    const DatabaseAction action = static_cast<DatabaseAction>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    const esp_err_t result = action == DatabaseAction::Playlists ? lyra::media::clear_playlists() :
                             action == DatabaseAction::Artwork ? lyra::media::clear_artwork_cache() :
                                                                 lyra::media::clear_all_databases();
    if (result == ESP_OK) {
        copy_ui_text(s_database_status, sizeof(s_database_status),
                     action == DatabaseAction::Playlists ? tr(lyra::i18n::StringId::PlaylistsCleared) :
                     action == DatabaseAction::Artwork ? tr(lyra::i18n::StringId::AlbumArtCacheCleared) :
                     tr(lyra::i18n::StringId::DatabasesClearedRescan));
    } else if (result == ESP_ERR_INVALID_STATE) {
        copy_ui_text(s_database_status, sizeof(s_database_status),
                     tr(lyra::i18n::StringId::StorageBusy));
    } else {
        copy_ui_text(s_database_status, sizeof(s_database_status),
                     tr(lyra::i18n::StringId::CouldNotClearStorage));
    }
    render(View::DatabaseStorage);
}

void show_database_confirmation(DatabaseAction action)
{
    lv_obj_t *overlay = make_box(s_screen, 0, 0, kScreenWidth, kScreenHeight, kOverlay);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_90, 0);
    lv_obj_move_foreground(overlay);
    lv_obj_t *dialog = make_box(overlay, 20, 118, 280, 244, kSurfaceRaised, 12);
    const char *title_text = action == DatabaseAction::Playlists ?
                             tr(lyra::i18n::StringId::ClearPlaylistsQuestion) :
                             action == DatabaseAction::Artwork ?
                             tr(lyra::i18n::StringId::ClearAlbumArtQuestion) :
                             tr(lyra::i18n::StringId::ClearAllDatabasesQuestion);
    const char *message_text = action == DatabaseAction::Playlists ?
        tr(lyra::i18n::StringId::ClearPlaylistsMessage) :
        action == DatabaseAction::Artwork ?
        tr(lyra::i18n::StringId::ClearAlbumArtMessage) :
        tr(lyra::i18n::StringId::ClearAllDatabasesMessage);
    lv_obj_t *title = make_label(dialog, title_text, kTextPrimary);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 22);
    lv_obj_t *message = make_label(dialog, message_text, kTextSecondary);
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(message, 240);
    lv_label_set_long_mode(message, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(message, LV_ALIGN_CENTER, 0, -17);
    lv_obj_t *cancel = make_button(dialog, 14, 180, 116, 48, kSurface, 7);
    lv_obj_t *cancel_label = make_label(cancel, tr(lyra::i18n::StringId::Cancel), kTextSecondary);
    lv_obj_center(cancel_label);
    lv_obj_add_event_cb(cancel, cancel_power_action_cb, LV_EVENT_CLICKED, overlay);
    lv_obj_t *confirm = make_button(dialog, 150, 180, 116, 48, lv_color_hex(0x991B1B), 7);
    lv_obj_t *confirm_label = make_label(confirm, tr(lyra::i18n::StringId::Clear), kTextOnAccent);
    lv_obj_center(confirm_label);
    lv_obj_add_event_cb(confirm, confirm_database_action_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(action)));
}

void database_action_cb(lv_event_t *event)
{
    show_database_confirmation(static_cast<DatabaseAction>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(event))));
}

void render_database_storage()
{
    make_header(tr(lyra::i18n::StringId::DatabaseStorage), View::SystemSettings, true);
    lv_obj_t *body = make_scroll_body(72);
    lv_obj_t *playlists = make_row(body, 0, LV_SYMBOL_LIST,
                                   tr(lyra::i18n::StringId::ClearPlaylists),
                                   tr(lyra::i18n::StringId::RemovePlaylistsFavorites),
                                   View::DatabaseStorage, 62);
    lv_obj_remove_event_cb(playlists, route_cb);
    lv_obj_add_event_cb(playlists, database_action_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(DatabaseAction::Playlists)));
    lv_obj_t *artwork = make_row(body, 66, LV_SYMBOL_IMAGE,
                                 tr(lyra::i18n::StringId::ClearAlbumArtCache),
                                 tr(lyra::i18n::StringId::RemoveLargeDecodedCovers),
                                 View::DatabaseStorage, 62);
    lv_obj_remove_event_cb(artwork, route_cb);
    lv_obj_add_event_cb(artwork, database_action_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(DatabaseAction::Artwork)));
    lv_obj_t *all = make_row(body, 132, LV_SYMBOL_WARNING,
                             tr(lyra::i18n::StringId::ClearAllDatabases),
                             tr(lyra::i18n::StringId::RescanRequired), View::DatabaseStorage, 62);
    lv_obj_set_style_bg_color(all, kDangerSurface, 0);
    lv_obj_remove_event_cb(all, route_cb);
    lv_obj_add_event_cb(all, database_action_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(DatabaseAction::All)));
    if (s_database_status[0]) {
        lv_obj_t *status = make_label(body, s_database_status, kAccent);
        lv_obj_align(status, LV_ALIGN_TOP_MID, 0, 214);
    }
}

void render_settings_page(View view)
{
    const char *title = view == View::PlaybackSettings ? tr(lyra::i18n::StringId::Playback) :
                        view == View::SoundSettings ? tr(lyra::i18n::StringId::Sound) :
                        view == View::DisplaySettings ? tr(lyra::i18n::StringId::Display) :
                        tr(lyra::i18n::StringId::System);
    make_header(title, View::Settings, true);
    lv_obj_t *body = make_scroll_body(72);
    if (view == View::PlaybackSettings) {
        make_setting_toggle(body, 0, tr(lyra::i18n::StringId::GaplessPlayback), nullptr, &s_gapless);
        make_setting_toggle(body, 66, tr(lyra::i18n::StringId::ReplayGain), nullptr, &s_replay_gain);
        make_setting_toggle(body, 132, tr(lyra::i18n::StringId::QuickSeek), nullptr, &s_quick_seek);
        make_row(body, 198, LV_SYMBOL_LOOP, tr(lyra::i18n::StringId::Crossfade), nullptr, View::CrossfadeOptions, 54);
        make_row(body, 256, LV_SYMBOL_WARNING, tr(lyra::i18n::StringId::SleepTimer), nullptr, View::SleepTimerOptions, 54);
    } else if (view == View::SoundSettings) {
        make_volume_control(body, 0);
        make_setting_toggle(body, 92, tr(lyra::i18n::StringId::OnBoardSpeaker),
                            tr(lyra::i18n::StringId::AlsoBuiltInSpeaker),
                            &s_speaker_output_enabled);
        make_row(body, 158, LV_SYMBOL_SETTINGS, tr(lyra::i18n::StringId::EqPreset),
                 equalizer_preset_name(s_equalizer_preset), View::Equalizer, 62);
    } else if (view == View::DisplaySettings) {
        make_setting_toggle(body, 0, tr(lyra::i18n::StringId::DarkMode),
                            s_dark_mode ? tr(lyra::i18n::StringId::DarkColours) :
                                          tr(lyra::i18n::StringId::LightColours),
                            &s_dark_mode);
        make_accent_selector(body, 66);
        make_setting_toggle(body, 182, tr(lyra::i18n::StringId::VirtualControls),
                            tr(lyra::i18n::StringId::ShowBottomNavigation), &s_show_nav);
        make_brightness_control(body, 248);
        lv_obj_t *screen_timeout = make_row(body, 340, LV_SYMBOL_POWER,
                                             tr(lyra::i18n::StringId::ScreenTimeout),
                                             tr(lyra::i18n::StringId::ComingSoon),
                                             View::DisplaySettings, 62);
        lv_obj_remove_event_cb(screen_timeout, route_cb);
        lv_obj_set_style_opa(screen_timeout, LV_OPA_60, 0);
    } else {
        const lyra::media::Status status = lyra::media::status();
        char sd_status[64];
        if (status.mounted) {
            format_float(lyra::i18n::StringId::MountedFree,
                         static_cast<double>(status.free_bytes) / (1024.0 * 1024.0 * 1024.0),
                         sd_status, sizeof(sd_status));
        } else {
            format_text(lyra::i18n::StringId::NotMounted,
                        esp_err_to_name(status.last_error), sd_status, sizeof(sd_status));
        }
        make_row(body, 0, LV_SYMBOL_SETTINGS, tr(lyra::i18n::StringId::Language),
                 lyra::i18n::language_name(lyra::i18n::current_language()),
                 View::LanguageOptions, 62);
        char clock_text[32];
        format_clock_date_time(lyra::clock::now(), clock_text, sizeof(clock_text));
        lv_obj_t *clock = make_row(body, 66, LV_SYMBOL_SETTINGS,
                                   tr(lyra::i18n::StringId::TimeAndDate), clock_text,
                                   View::ClockSettings, 62);
        lv_obj_remove_event_cb(clock, route_cb);
        lv_obj_add_event_cb(clock, clock_settings_cb, LV_EVENT_CLICKED, nullptr);
        make_row(body, 132, LV_SYMBOL_SD_CARD, tr(lyra::i18n::StringId::MicroSdCard),
                 sd_status, View::SystemSettings, 62);
        char scan_status[48];
        if (status.scanning) {
            copy_ui_text(scan_status, sizeof(scan_status), tr(lyra::i18n::StringId::ScanningMicroSd));
        } else if (status.capacity_reached) {
            copy_ui_text(scan_status, sizeof(scan_status), tr(lyra::i18n::StringId::LibraryLimit));
        } else {
            format_count(lyra::i18n::StringId::IndexedTracks,
                         static_cast<uint32_t>(status.track_count), scan_status, sizeof(scan_status));
        }
        lv_obj_t *scan = make_row(body, 198, LV_SYMBOL_REFRESH,
                                  tr(lyra::i18n::StringId::ScanMusicLibrary), scan_status,
                                  View::SystemSettings, 62);
        lv_obj_remove_event_cb(scan, route_cb);
        lv_obj_add_event_cb(scan, scan_library_cb, LV_EVENT_CLICKED, nullptr);
        make_row(body, 264, LV_SYMBOL_DRIVE, tr(lyra::i18n::StringId::DatabaseStorage),
                 tr(lyra::i18n::StringId::ManagePlaylistsLibrary),
                 View::DatabaseStorage, 62);
        make_artwork_setting_toggle(body, 330, tr(lyra::i18n::StringId::SdAlbumArtCache),
                                    tr(lyra::i18n::StringId::AllowOversizedJpeg),
                                    status.artwork_sd_cache_enabled, ArtworkSetting::SdCache);
        make_artwork_setting_toggle(body, 396, tr(lyra::i18n::StringId::AlbumArt320),
                                    tr(lyra::i18n::StringId::AlbumArt240WhenDisabled),
                                    status.artwork_size == lyra::media::kLargeArtworkSize,
                                    ArtworkSetting::Size320);
        lv_obj_t *reboot = make_row(body, 462, LV_SYMBOL_REFRESH,
                                    tr(lyra::i18n::StringId::Reboot),
                                    tr(lyra::i18n::StringId::SafelyRestartLyra),
                                    View::SystemSettings, 62);
        lv_obj_remove_event_cb(reboot, route_cb);
        lv_obj_add_event_cb(reboot, reboot_cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_t *power_off = make_row(body, 528, LV_SYMBOL_POWER,
                                       tr(lyra::i18n::StringId::PowerOff),
                                       tr(lyra::i18n::StringId::SafelyUnmountAndSleep),
                                       View::SystemSettings, 62);
        lv_obj_remove_event_cb(power_off, route_cb);
        lv_obj_add_event_cb(power_off, power_off_cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_t *factory_reset = make_row(body, 594, LV_SYMBOL_WARNING,
                                           tr(lyra::i18n::StringId::FactoryReset),
                                           tr(lyra::i18n::StringId::FactoryResetDescription),
                                           View::SystemSettings, 62);
        lv_obj_set_style_bg_color(factory_reset, kDangerSurface, 0);
        lv_obj_remove_event_cb(factory_reset, route_cb);
        lv_obj_add_event_cb(factory_reset, factory_reset_cb, LV_EVENT_CLICKED, nullptr);
    }
}

void render_about()
{
    make_header(tr(lyra::i18n::StringId::About), View::Settings, true);
    lv_obj_t *body = make_scroll_body(72);
    if (load_brand_logo()) {
        lv_obj_t *logo = lv_image_create(body);
        lv_image_set_src(logo, &s_brand_logo_descriptor);
        lv_obj_set_pos(logo, 10, 22);
        lv_obj_add_flag(logo, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(logo, [](lv_event_t *) {
            s_debug_tab = DebugTab::Info;
            navigate_to(View::DebugMenu);
        }, LV_EVENT_LONG_PRESSED, nullptr);
    } else {
        lv_obj_t *logo = make_label(body, "Emotivate Lyra", kTextPrimary);
        lv_obj_align(logo, LV_ALIGN_TOP_MID, 0, 38);
        lv_obj_add_flag(logo, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(logo, [](lv_event_t *) {
            s_debug_tab = DebugTab::Info;
            navigate_to(View::DebugMenu);
        }, LV_EVENT_LONG_PRESSED, nullptr);
    }
    lv_obj_t *firmware_heading = make_label(body, tr(lyra::i18n::StringId::FirmwareVersion), kTextMuted);
    lv_obj_align(firmware_heading, LV_ALIGN_TOP_MID, 0, 112);
    lv_obj_t *firmware_version = make_label(body, "1.0.1", kTextPrimary);
    lv_obj_align(firmware_version, LV_ALIGN_TOP_MID, 0, 138);
    lv_obj_t *hardware_heading = make_label(body, tr(lyra::i18n::StringId::HardwareId), kTextMuted);
    lv_obj_align(hardware_heading, LV_ALIGN_TOP_MID, 0, 190);
    lv_obj_t *hardware_id = make_label(body, "JC3248W535EN", kTextPrimary);
    lv_obj_align(hardware_id, LV_ALIGN_TOP_MID, 0, 216);
    lv_obj_t *update = make_button(body, 36, 270, 248, 48, kSurface, 7, true);
    lv_obj_t *update_label = make_label(update, tr(lyra::i18n::StringId::FirmwareUpdate), kTextPrimary);
    lv_obj_center(update_label);
    lv_obj_add_event_cb(update, firmware_update_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *licenses = make_button(body, 36, 326, 248, 48, kSurface, 7, true);
    lv_obj_t *licenses_label = make_label(licenses, tr(lyra::i18n::StringId::Licenses), kTextPrimary);
    lv_obj_center(licenses_label);
    add_route(licenses, View::Licenses);
}

void render_licenses()
{
    make_header(tr(lyra::i18n::StringId::Licenses), View::Settings, true);
    lv_obj_t *body = make_scroll_body(72);
    const char *notices = tr(lyra::i18n::StringId::LicenseNotices);
    lv_obj_t *text = make_label(body, notices, kTextSecondary);
    lv_obj_set_pos(text, 14, 14);
    lv_obj_set_width(text, 292);
    lv_label_set_long_mode(text, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_line_space(text, 5, 0);
}

} // namespace lyra::gui::internal
