/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../lyra_gui_internal.h"

namespace lyra::gui::internal {

void toggle_bool_cb(lv_event_t *event)
{
    lv_obj_t *toggle = static_cast<lv_obj_t *>(lv_event_get_user_data(event));
    bool *value = static_cast<bool *>(lv_obj_get_user_data(toggle));
    *value = !*value;
    if (value == &s_gapless || value == &s_replay_gain) {
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
    lv_obj_align(title_label, LV_ALIGN_LEFT_MID, 12, subtitle == nullptr ? 0 : -10);
    if (subtitle != nullptr) {
        lv_obj_t *sub = make_label(row, subtitle, kTextMuted);
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
    lv_obj_t *title = make_label(card, "Accent colour", kTextPrimary);
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
    lv_obj_align(title_label, LV_ALIGN_LEFT_MID, 12, -10);
    lv_obj_t *sub = make_label(row, subtitle, kTextMuted);
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
    const int value = lv_slider_get_value(slider);
    lyra::audio::set_volume(static_cast<uint8_t>(value));
    update_status_volume_label(static_cast<uint8_t>(value));
    lv_obj_t *value_label = static_cast<lv_obj_t *>(lv_obj_get_user_data(slider));
    if (value_label != nullptr) {
        char text[32];
        std::snprintf(text, sizeof(text), "%d%%", value);
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
    lv_obj_t *title = make_label(card, "Volume", kTextPrimary);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 10);
    char value_text[32];
    std::snprintf(value_text, sizeof(value_text), "%u%%",
                  static_cast<unsigned>(audio_status.volume_percent));
    lv_obj_t *value_label = make_label(card, value_text, kAccent);
    lv_obj_align(value_label, LV_ALIGN_TOP_RIGHT, -12, 10);

    lv_obj_t *slider = lv_slider_create(card);
    lv_obj_set_pos(slider, 12, 54);
    lv_obj_set_size(slider, 282, 14);
    lv_slider_set_range(slider, 0, lyra::audio::maximum_volume_percent());
    lv_slider_set_value(slider, audio_status.volume_percent, LV_ANIM_OFF);
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
    lv_obj_t *title = make_label(card, "Brightness", kTextPrimary);
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
    case lyra::media::SortSection::Songs: return "Songs";
    case lyra::media::SortSection::Albums: return "Albums";
    case lyra::media::SortSection::Artists: return "Artists";
    }
    return "Songs";
}

const char *sort_field_name(lyra::media::SortField field)
{
    switch (field) {
    case lyra::media::SortField::Title: return "Title";
    case lyra::media::SortField::Album: return "Album";
    case lyra::media::SortField::TrackNumber: return "Track Number";
    case lyra::media::SortField::Duration: return "Duration";
    case lyra::media::SortField::Artist: return "Artist";
    case lyra::media::SortField::DateModified: return "Date Modified";
    }
    return "Title";
}

const char *sort_direction_name(lyra::media::SortDirection direction)
{
    return direction == lyra::media::SortDirection::Ascending ? "Ascending" : "Descending";
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
    make_header("Sorting", View::Settings, true);
    lv_obj_t *body = make_scroll_body(72);
    constexpr lyra::media::SortSection sections[] = {
        lyra::media::SortSection::Songs,
        lyra::media::SortSection::Albums,
        lyra::media::SortSection::Artists,
    };
    constexpr const char *subtitles[] = {
        "Choose the order of every song",
        "Choose the order of album rows",
        "Choose the order of artist rows",
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
    std::snprintf(title, sizeof(title), "%s / %s", sort_field_name(field),
                  sort_direction_name(direction));
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
    std::snprintf(title, sizeof(title), "Sort %s", sort_section_name(s_sort_section));
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
    make_header("Crossfade", View::PlaybackSettings, true);
    lv_obj_t *body = make_scroll_body(72);
    constexpr uint8_t values[] = {0, 1, 2, 4, 6};
    constexpr const char *labels[] = {"Off", "1 second", "2 seconds", "4 seconds", "6 seconds"};
    for (size_t index = 0; index < sizeof(values) / sizeof(values[0]); ++index) {
        make_playback_option_row(body, static_cast<int>(index) * 58, labels[index],
                                 s_crossfade_seconds == values[index], crossfade_option_cb,
                                 static_cast<uintptr_t>(values[index]));
    }
}

void render_sleep_timer_options()
{
    make_header("Sleep timer", View::PlaybackSettings, true);
    lv_obj_t *body = make_scroll_body(72);
    constexpr uint16_t values[] = {0, 15, 30, 45, 60};
    constexpr const char *labels[] = {"Off", "15 minutes", "30 minutes", "45 minutes", "60 minutes"};
    for (size_t index = 0; index < sizeof(values) / sizeof(values[0]); ++index) {
        make_playback_option_row(body, static_cast<int>(index) * 58, labels[index],
                                 s_sleep_timer_minutes == values[index], sleep_timer_option_cb,
                                 static_cast<uintptr_t>(values[index]));
    }
}

void render_settings_menu()
{
    make_header("Settings", View::Menu, true);
    lv_obj_t *body = make_scroll_body(72);
    make_row(body, 0, LV_SYMBOL_PLAY, "Playback", nullptr, View::PlaybackSettings, 54);
    make_row(body, 58, LV_SYMBOL_VOLUME_MID, "Sound", nullptr, View::SoundSettings, 54);
    make_row(body, 116, LV_SYMBOL_IMAGE, "Display", nullptr, View::DisplaySettings, 54);
    make_row(body, 174, LV_SYMBOL_LIST, "Sorting", nullptr, View::SortingSettings, 54);
    make_row(body, 232, LV_SYMBOL_SETTINGS, "System", nullptr, View::SystemSettings, 54);
    make_row(body, 290, LV_SYMBOL_WARNING, "About", nullptr, View::About, 54);
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
    lv_obj_t *title = make_label(dialog, "SCANNING MUSIC LIBRARY", kTextPrimary);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 105);
    s_scan_count_label = make_label(dialog, "0 songs found", kAccent);
    lv_obj_align(s_scan_count_label, LV_ALIGN_TOP_MID, 0, 137);
    s_scan_phase_label = make_label(dialog, "Reading MicroSD folders...", kTextSecondary);
    lv_obj_align(s_scan_phase_label, LV_ALIGN_TOP_MID, 0, 169);
}

void save_active_queue_snapshot()
{
    if (!s_has_active_queue) return;
    const size_t count = playback_queue_count();
    size_t current = 0;
    if (count == 0 || !current_queue_position(&current)) return;

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
        lyra::media::save_queue_snapshot(tracks, count, current) : ESP_FAIL;
    heap_caps_free(tracks);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "could not save queue snapshot: %s", esp_err_to_name(result));
    }
}

void power_action_task(void *context)
{
    const PowerAction action = static_cast<PowerAction>(reinterpret_cast<uintptr_t>(context));
    // Give LVGL time to present the shutdown status before filesystem work.
    vTaskDelay(pdMS_TO_TICKS(250));
    save_active_queue_snapshot();
    const esp_err_t volume_result = lyra::audio::save_volume();
    if (volume_result != ESP_OK) ESP_LOGW(kTag, "could not save volume before shutdown: %s",
                                          esp_err_to_name(volume_result));
    lyra::audio::stop();
    // The decoder owns an open MicroSD FILE while it is active. Wait for its
    // worker to release that handle before attempting the filesystem unmount.
    for (int wait_count = 0; wait_count < 200; ++wait_count) {
        if (!lyra::audio::status().playing) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    const esp_err_t media_result = lyra::media::shutdown();
    if (media_result != ESP_OK) {
        ESP_LOGE(kTag, "safe shutdown failed: %s", esp_err_to_name(media_result));
        // Do not restart or sleep after an unmount failure: preserving the card
        // takes priority over completing the requested power action.
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
                                 action == PowerAction::Reboot ? "REBOOTING\n\nSafely unmounting MicroSD..."
                                                                : "POWERING OFF\n\nSafely unmounting MicroSD...",
                                 kTextPrimary);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, -10);
    if (xTaskCreatePinnedToCore(power_action_task, "lyra_power", 4096,
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
    lv_obj_t *title = make_label(dialog, action == PowerAction::Reboot ? "Reboot Lyra?" : "Power off Lyra?",
                                 kTextPrimary);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 22);
    lv_obj_t *message = make_label(dialog,
                                   action == PowerAction::Reboot
                                       ? "The MicroSD card will be safely\nunmounted before restarting."
                                       : "The MicroSD card will be safely\nunmounted before deep sleep.",
                                   kTextSecondary);
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(message, LV_ALIGN_CENTER, 0, -15);

    lv_obj_t *cancel = make_button(dialog, 14, 152, 116, 48, kSurface, 7);
    lv_obj_t *cancel_label = make_label(cancel, "CANCEL", kTextSecondary);
    lv_obj_center(cancel_label);
    lv_obj_add_event_cb(cancel, cancel_power_action_cb, LV_EVENT_CLICKED, overlay);

    lv_obj_t *confirm = make_button(dialog, 150, 152, 116, 48,
                                    action == PowerAction::Reboot ? kAccentDark : lv_color_hex(0x991B1B), 7);
    lv_obj_t *confirm_label = make_label(confirm, action == PowerAction::Reboot ? "REBOOT" : "POWER OFF",
                                         kTextOnAccent);
    lv_obj_center(confirm_label);
    lv_obj_add_event_cb(confirm, confirm_power_action_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(action)));
}

void reboot_cb(lv_event_t *) { show_power_confirmation(PowerAction::Reboot); }
void power_off_cb(lv_event_t *) { show_power_confirmation(PowerAction::PowerOff); }

void confirm_database_action_cb(lv_event_t *event)
{
    const DatabaseAction action = static_cast<DatabaseAction>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    const esp_err_t result = action == DatabaseAction::Playlists ? lyra::media::clear_playlists() :
                             action == DatabaseAction::Artwork ? lyra::media::clear_artwork_cache() :
                                                                 lyra::media::clear_all_databases();
    if (result == ESP_OK) {
        copy_ui_text(s_database_status, sizeof(s_database_status),
                     action == DatabaseAction::Playlists ? "Playlists cleared" :
                     action == DatabaseAction::Artwork ? "Album art cache cleared" :
                                                          "Databases cleared - rescan required");
    } else if (result == ESP_ERR_INVALID_STATE) {
        copy_ui_text(s_database_status, sizeof(s_database_status), "Storage busy - try again shortly");
    } else {
        copy_ui_text(s_database_status, sizeof(s_database_status), "Could not clear storage");
    }
    render(View::DatabaseStorage);
}

void show_database_confirmation(DatabaseAction action)
{
    lv_obj_t *overlay = make_box(s_screen, 0, 0, kScreenWidth, kScreenHeight, kOverlay);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_90, 0);
    lv_obj_move_foreground(overlay);
    lv_obj_t *dialog = make_box(overlay, 20, 118, 280, 244, kSurfaceRaised, 12);
    const char *title_text = action == DatabaseAction::Playlists ? "Clear playlists?" :
                             action == DatabaseAction::Artwork ? "Clear album art cache?" :
                                                                 "Clear all databases?";
    const char *message_text = action == DatabaseAction::Playlists ?
        "Every saved playlist and favorite\nwill be removed." :
        action == DatabaseAction::Artwork ?
        "Large decoded covers will be removed.\nThey regenerate when needed." :
        "Library index, playlists, and artwork\ncache will be removed. Music files stay.\nA rescan is required.";
    lv_obj_t *title = make_label(dialog, title_text, kTextPrimary);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 22);
    lv_obj_t *message = make_label(dialog, message_text, kTextSecondary);
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(message, 240);
    lv_label_set_long_mode(message, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(message, LV_ALIGN_CENTER, 0, -17);
    lv_obj_t *cancel = make_button(dialog, 14, 180, 116, 48, kSurface, 7);
    lv_obj_t *cancel_label = make_label(cancel, "CANCEL", kTextSecondary);
    lv_obj_center(cancel_label);
    lv_obj_add_event_cb(cancel, cancel_power_action_cb, LV_EVENT_CLICKED, overlay);
    lv_obj_t *confirm = make_button(dialog, 150, 180, 116, 48, lv_color_hex(0x991B1B), 7);
    lv_obj_t *confirm_label = make_label(confirm, "CLEAR", kTextOnAccent);
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
    make_header("Database Storage", View::SystemSettings, true);
    lv_obj_t *body = make_scroll_body(72);
    lv_obj_t *playlists = make_row(body, 0, LV_SYMBOL_LIST, "Clear Playlists",
                                   "Remove playlists and favorites", View::DatabaseStorage, 62);
    lv_obj_remove_event_cb(playlists, route_cb);
    lv_obj_add_event_cb(playlists, database_action_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(DatabaseAction::Playlists)));
    lv_obj_t *artwork = make_row(body, 66, LV_SYMBOL_IMAGE, "Clear Album Art Cache",
                                 "Remove large decoded covers", View::DatabaseStorage, 62);
    lv_obj_remove_event_cb(artwork, route_cb);
    lv_obj_add_event_cb(artwork, database_action_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(DatabaseAction::Artwork)));
    lv_obj_t *all = make_row(body, 132, LV_SYMBOL_WARNING, "Clear All Databases",
                             "Music library rescan required", View::DatabaseStorage, 62);
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
    const char *title = view == View::PlaybackSettings ? "Playback" :
                        view == View::SoundSettings ? "Sound" :
                        view == View::DisplaySettings ? "Display" : "System";
    make_header(title, View::Settings, true);
    lv_obj_t *body = make_scroll_body(72);
    if (view == View::PlaybackSettings) {
        make_setting_toggle(body, 0, "Gapless playback", nullptr, &s_gapless);
        make_setting_toggle(body, 66, "ReplayGain", nullptr, &s_replay_gain);
        make_row(body, 132, LV_SYMBOL_LOOP, "Crossfade", nullptr, View::CrossfadeOptions, 54);
        make_row(body, 190, LV_SYMBOL_WARNING, "Sleep timer", nullptr, View::SleepTimerOptions, 54);
    } else if (view == View::SoundSettings) {
        make_volume_control(body, 0);
        make_setting_toggle(body, 92, "On-board speaker",
                            "Also play through the built-in speaker",
                            &s_speaker_output_enabled);
        make_row(body, 158, LV_SYMBOL_SETTINGS, "EQ preset",
                 equalizer_preset_name(s_equalizer_preset), View::Equalizer, 62);
    } else if (view == View::DisplaySettings) {
        make_setting_toggle(body, 0, "Dark mode",
                            s_dark_mode ? "Dark colours for the interface" :
                                          "Light colours for the interface",
                            &s_dark_mode);
        make_accent_selector(body, 66);
        make_setting_toggle(body, 182, "Virtual controls", "Show bottom navigation bar", &s_show_nav);
        make_brightness_control(body, 248);
        lv_obj_t *screen_timeout = make_row(body, 340, LV_SYMBOL_POWER, "Screen timeout", "Coming soon",
                                             View::DisplaySettings, 62);
        lv_obj_remove_event_cb(screen_timeout, route_cb);
        lv_obj_set_style_opa(screen_timeout, LV_OPA_60, 0);
    } else {
        const lyra::media::Status status = lyra::media::status();
        char sd_status[64];
        if (status.mounted) {
            std::snprintf(sd_status, sizeof(sd_status), "Mounted / %.1f GB free",
                          static_cast<double>(status.free_bytes) / (1024.0 * 1024.0 * 1024.0));
        } else {
            copy_ui_text(sd_status, sizeof(sd_status), "Not mounted (");
            append_ui_text(sd_status, sizeof(sd_status), esp_err_to_name(status.last_error));
            append_ui_text(sd_status, sizeof(sd_status), ")");
        }
        make_row(body, 0, LV_SYMBOL_SD_CARD, "MicroSD card", sd_status, View::SystemSettings, 62);
        char scan_status[48];
        if (status.scanning) {
            copy_ui_text(scan_status, sizeof(scan_status), "Scanning MicroSD...");
        } else if (status.capacity_reached) {
            copy_ui_text(scan_status, sizeof(scan_status), "10,000 tracks (library limit)");
        } else {
            std::snprintf(scan_status, sizeof(scan_status), "%u tracks indexed",
                          static_cast<unsigned>(status.track_count));
        }
        lv_obj_t *scan = make_row(body, 66, LV_SYMBOL_REFRESH, "Scan music library", scan_status,
                                  View::SystemSettings, 62);
        lv_obj_remove_event_cb(scan, route_cb);
        lv_obj_add_event_cb(scan, scan_library_cb, LV_EVENT_CLICKED, nullptr);
        make_row(body, 132, LV_SYMBOL_DRIVE, "Database Storage", "Manage playlists and library data",
                 View::DatabaseStorage, 62);
        make_artwork_setting_toggle(body, 198, "SD album art cache",
                                    "Allow oversized JPEG decoding",
                                    status.artwork_sd_cache_enabled, ArtworkSetting::SdCache);
        make_artwork_setting_toggle(body, 264, "320 x 320 album art",
                                    "Use 240 x 240 when disabled",
                                    status.artwork_size == lyra::media::kLargeArtworkSize,
                                    ArtworkSetting::Size320);
        lv_obj_t *reboot = make_row(body, 330, LV_SYMBOL_REFRESH, "Reboot", "Safely restart Lyra",
                                    View::SystemSettings, 62);
        lv_obj_remove_event_cb(reboot, route_cb);
        lv_obj_add_event_cb(reboot, reboot_cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_t *power_off = make_row(body, 396, LV_SYMBOL_POWER, "Power off", "Safely unmount and sleep",
                                       View::SystemSettings, 62);
        lv_obj_remove_event_cb(power_off, route_cb);
        lv_obj_add_event_cb(power_off, power_off_cb, LV_EVENT_CLICKED, nullptr);
    }
}

void render_about()
{
    make_header("About", View::Settings, true);
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
    lv_obj_t *firmware_heading = make_label(body, "FIRMWARE VERSION", kTextMuted);
    lv_obj_align(firmware_heading, LV_ALIGN_TOP_MID, 0, 112);
    lv_obj_t *firmware_version = make_label(body, "1.0.1", kTextPrimary);
    lv_obj_align(firmware_version, LV_ALIGN_TOP_MID, 0, 138);
    lv_obj_t *hardware_heading = make_label(body, "HARDWARE ID", kTextMuted);
    lv_obj_align(hardware_heading, LV_ALIGN_TOP_MID, 0, 190);
    lv_obj_t *hardware_id = make_label(body, "JC3248W535EN", kTextPrimary);
    lv_obj_align(hardware_id, LV_ALIGN_TOP_MID, 0, 216);
    lv_obj_t *update = make_button(body, 36, 270, 248, 48, kSurface, 7, true);
    lv_obj_t *update_label = make_label(update, "FIRMWARE UPDATE", kTextPrimary);
    lv_obj_center(update_label);
    lv_obj_add_event_cb(update, firmware_update_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *licenses = make_button(body, 36, 326, 248, 48, kSurface, 7, true);
    lv_obj_t *licenses_label = make_label(licenses, "LICENSES", kTextPrimary);
    lv_obj_center(licenses_label);
    add_route(licenses, View::Licenses);
}

void render_licenses()
{
    make_header("Licenses", View::Settings, true);
    lv_obj_t *body = make_scroll_body(72);
    constexpr const char *notices =
        "License notices for software included in this firmware.\n\n"
        "Lyra firmware\nCopyright 2026 Emotivate Lyra contributors\nApache License 2.0\n\n"
        "ESP-IDF\nCopyright Espressif Systems (Shanghai) Co., Ltd.\nApache License 2.0\n\n"
        "LVGL\nCopyright 2025 LVGL Kft\nMIT License\n\n"
        "Source Han Sans glyph data\nCopyright 2014 Adobe Systems Incorporated\n"
        "Apache License 2.0\n\n"
        "ESP Audio Codec\nCopyright 2025 Espressif Systems (Shanghai) Co., Ltd.\n"
        "Espressif Modified MIT License\n\n"
        "libjpeg-turbo\nCopyright 2009-2025 D. R. Commander\n"
        "Copyright 2015 Viktor Szathmáry\nIJG, BSD-3-Clause, and zlib licenses\n\n"
        "libpng\nCopyright 1995-2019 The PNG Reference Library Authors\n"
        "Copyright 2018-2019 Cosmin Truta\n"
        "Copyright 2000-2002, 2004, 2006-2018 Glenn Randers-Pehrson\n"
        "Copyright 1996-1997 Andreas Dilger\n"
        "Copyright 1995-1996 Guy Eric Schalnat, Group 42, Inc.\n"
        "PNG Reference Library License\n\n"
        "zlib\nCopyright 1995-2022 Jean-loup Gailly and Mark Adler\n"
        "zlib License";
    lv_obj_t *text = make_label(body, notices, kTextSecondary);
    lv_obj_set_pos(text, 14, 14);
    lv_obj_set_width(text, 292);
    lv_label_set_long_mode(text, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_line_space(text, 5, 0);
}

} // namespace lyra::gui::internal
