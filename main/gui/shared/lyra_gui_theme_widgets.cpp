/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../lyra_gui_internal.h"

namespace lyra::gui::internal {

namespace {

// Rounds the small radii older layouts pass in up to the design's corners:
// rows and cards get the full radius, small controls a softer one.
int design_radius(int radius, int width, int height)
{
    if (radius <= 0 || radius >= LV_RADIUS_CIRCLE) return radius;
    const int shorter = std::min(width, height);
    if (shorter >= 36 && width >= 100) return std::max(radius, kRowRadius);
    return std::max(radius, std::min(8, shorter / 2));
}

lv_color_t tint(lv_color_t base, uint8_t amount)
{
    return s_dynamic_tint ? lv_color_mix(kAccent, base, amount) : base;
}

} // namespace

lv_color_t accent_color(size_t index)
{
    const AccentPalette &accent = kAccentPalettes[index < kAccentPaletteCount ? index : 0];
    return lv_color_hex(s_dark_mode ? accent.dark_rgb : accent.light_rgb);
}

void apply_theme_palette()
{
    kAccent = accent_color(s_accent_colour);
    if (s_dark_mode) {
        kBackground = lv_color_hex(0x121212);
        kSurface = lv_color_hex(0x1F1F1F);
        kSurfaceRaised = lv_color_hex(0x2B2B2B);
        kTextPrimary = lv_color_hex(0xF5F5F5);
        kTextSecondary = lv_color_hex(0xB8B8B8);
        kTextMuted = lv_color_hex(0x808080);
        kDivider = lv_color_hex(0x363636);
        kNavSurface = lv_color_hex(0x1A1A1A);
        kKeyboardSurface = lv_color_hex(0x0C0C0C);
        kArtworkSurface = lv_color_hex(0x2A2A2A);
        kDangerSurface = lv_color_hex(0x3D1717);
        kDanger = lv_color_hex(0xD64545);
        kAccentDark = lv_color_mix(lv_color_black(), kAccent, 56);
        kAccentSurface = lv_color_mix(kAccent, kBackground, 64);
    } else {
        kBackground = lv_color_hex(0xEFEFF4);
        kSurface = lv_color_hex(0xFFFFFF);
        kSurfaceRaised = lv_color_hex(0xE4E4EA);
        kTextPrimary = lv_color_hex(0x161616);
        kTextSecondary = lv_color_hex(0x4A4A4F);
        kTextMuted = lv_color_hex(0x7C7C84);
        kDivider = lv_color_hex(0xD5D5DC);
        kNavSurface = lv_color_hex(0xFFFFFF);
        kKeyboardSurface = lv_color_hex(0xD9D9E0);
        kArtworkSurface = lv_color_hex(0xDDDDE3);
        kDangerSurface = lv_color_hex(0xFBE1E1);
        kDanger = lv_color_hex(0xC62828);
        kAccentDark = lv_color_mix(lv_color_black(), kAccent, 36);
        kAccentSurface = lv_color_mix(kAccent, lv_color_white(), 44);
    }
    // Dynamic tint washes every neutral layer with the accent, deeper on the
    // raised layers so cards still separate from the page.
    const bool dark = s_dark_mode;
    kBackground = tint(kBackground, dark ? 22 : 18);
    kSurface = tint(kSurface, dark ? 30 : 12);
    kSurfaceRaised = tint(kSurfaceRaised, dark ? 38 : 26);
    kNavSurface = tint(kNavSurface, dark ? 28 : 12);
    kDivider = tint(kDivider, dark ? 40 : 34);
    kArtworkSurface = tint(kArtworkSurface, dark ? 30 : 22);
    kKeyboardSurface = tint(kKeyboardSurface, dark ? 16 : 22);
}

constexpr int16_t kEqualizerFlat[lyra::audio::kEqualizerBandCount] = {
    0, 0, 0, 0, 0,
};
constexpr int16_t kEqualizerFullBass[lyra::audio::kEqualizerBandCount] = {
    60, 60, 0, 0, 0,
};
constexpr int16_t kEqualizerFullTreble[lyra::audio::kEqualizerBandCount] = {
    0, 0, 0, 60, 60,
};
constexpr int16_t kEqualizerBassAndTreble[lyra::audio::kEqualizerBandCount] = {
    60, 60, 0, 60, 60,
};
constexpr int16_t kEqualizerRock[lyra::audio::kEqualizerBandCount] = {
    50, 30, 10, 40, 50,
};
constexpr int16_t kEqualizerPop[lyra::audio::kEqualizerBandCount] = {
    -10, 20, 40, 20, -10,
};
constexpr int16_t kEqualizerJazz[lyra::audio::kEqualizerBandCount] = {
    30, 10, 20, 30, 20,
};
constexpr int16_t kEqualizerClassic[lyra::audio::kEqualizerBandCount] = {
    40, 20, 0, 30, 40,
};

const char *equalizer_preset_name(EqualizerPreset preset)
{
    switch (preset) {
        case EqualizerPreset::Custom: return tr(lyra::i18n::StringId::Custom);
        case EqualizerPreset::Flat: return tr(lyra::i18n::StringId::Flat);
        case EqualizerPreset::FullBass: return tr(lyra::i18n::StringId::FullBass);
        case EqualizerPreset::FullTreble: return tr(lyra::i18n::StringId::FullTreble);
        case EqualizerPreset::BassAndTreble: return tr(lyra::i18n::StringId::BassAndTreble);
        case EqualizerPreset::Rock: return tr(lyra::i18n::StringId::Rock);
        case EqualizerPreset::Pop: return tr(lyra::i18n::StringId::Pop);
        case EqualizerPreset::Jazz: return tr(lyra::i18n::StringId::Jazz);
        case EqualizerPreset::Classic: return tr(lyra::i18n::StringId::Classic);
        case EqualizerPreset::Count: break;
    }
    return tr(lyra::i18n::StringId::Custom);
}

const int16_t *equalizer_preset_bands(EqualizerPreset preset)
{
    switch (preset) {
        case EqualizerPreset::Flat: return kEqualizerFlat;
        case EqualizerPreset::FullBass: return kEqualizerFullBass;
        case EqualizerPreset::FullTreble: return kEqualizerFullTreble;
        case EqualizerPreset::BassAndTreble: return kEqualizerBassAndTreble;
        case EqualizerPreset::Rock: return kEqualizerRock;
        case EqualizerPreset::Pop: return kEqualizerPop;
        case EqualizerPreset::Jazz: return kEqualizerJazz;
        case EqualizerPreset::Classic: return kEqualizerClassic;
        case EqualizerPreset::Custom: return s_equalizer_custom_bands;
        case EqualizerPreset::Count: break;
    }
    return s_equalizer_custom_bands;
}

bool equalizer_is_custom()
{
    return s_equalizer_preset == EqualizerPreset::Custom;
}

void apply_equalizer_to_audio()
{
    lyra::audio::EqualizerSettings settings{};
    const int16_t *bands = equalizer_preset_bands(s_equalizer_preset);
    std::memcpy(settings.band_tenths_db, bands, sizeof(settings.band_tenths_db));
    const esp_err_t result = lyra::audio::set_equalizer(settings);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "could not apply %s EQ preset: %s",
                 equalizer_preset_name(s_equalizer_preset), esp_err_to_name(result));
    }
}

esp_err_t save_user_settings()
{
    lyra::gui_settings::Values values{};
    values.gapless = s_gapless;
    values.replay_gain = s_replay_gain;
    values.quick_seek = s_quick_seek;
    values.crossfade_seconds = s_crossfade_seconds;
    values.brightness_percent = s_brightness_percent;
    values.dark_mode = s_dark_mode;
    values.accent_colour = s_accent_colour;
    values.language = lyra::i18n::current_language();
    values.speaker_output_enabled = s_speaker_output_enabled;
    values.equalizer_preset = static_cast<uint8_t>(s_equalizer_preset);
    values.date_format = static_cast<uint8_t>(s_date_format);
    values.use_24_hour = s_use_24_hour;
    values.dst_enabled = s_dst_enabled;
    values.language_selected = true;
    values.keyboard_layout = s_library_keyboard_layout;
    values.setup_complete = !s_setup_pending;
    values.clock_position = s_clock_position;
    values.dynamic_tint = s_dynamic_tint;
    values.mini_player = s_show_nav;
    values.t9_input = s_t9_input;
    values.list_artwork = s_list_artwork;
    values.automatic_time = s_automatic_time;
    values.utc_offset_minutes = s_utc_offset_minutes;
    std::memcpy(values.equalizer_custom_bands, s_equalizer_custom_bands,
                sizeof(values.equalizer_custom_bands));
    return lyra::gui_settings::save(values);
}

void load_user_settings()
{
    // The current values are the defaults for anything not saved yet.
    lyra::gui_settings::Values values{};
    values.gapless = s_gapless;
    values.replay_gain = s_replay_gain;
    values.quick_seek = s_quick_seek;
    values.crossfade_seconds = s_crossfade_seconds;
    values.brightness_percent = s_brightness_percent;
    values.dark_mode = s_dark_mode;
    values.accent_colour = s_accent_colour;
    values.speaker_output_enabled = s_speaker_output_enabled;
    values.equalizer_preset = static_cast<uint8_t>(s_equalizer_preset);
    values.date_format = static_cast<uint8_t>(s_date_format);
    values.use_24_hour = s_use_24_hour;
    values.dst_enabled = s_dst_enabled;
    values.keyboard_layout = s_library_keyboard_layout;
    values.clock_position = s_clock_position;
    values.dynamic_tint = s_dynamic_tint;
    values.mini_player = s_show_nav;
    std::memcpy(values.equalizer_custom_bands, s_equalizer_custom_bands,
                sizeof(values.equalizer_custom_bands));
    lyra::gui_settings::load(&values, kAccentPaletteCount,
                             static_cast<uint8_t>(EqualizerPreset::Count));
    s_setup_pending = !values.setup_complete;
    s_t9_input = values.t9_input;
    s_list_artwork = values.list_artwork;
    s_automatic_time = values.automatic_time;
    s_utc_offset_minutes = values.utc_offset_minutes;
    // Without a retained clock the manual DST hour was lost, unless network
    // time will set the clock again with the hour included.
    const bool clear_fallback_dst = lyra::clock::is_using_default_time() && values.dst_enabled &&
                                    !values.automatic_time;
    if (clear_fallback_dst) values.dst_enabled = false;
    s_gapless = values.gapless;
    s_replay_gain = values.replay_gain;
    s_quick_seek = values.quick_seek;
    s_crossfade_seconds = values.crossfade_seconds;
    s_brightness_percent = values.brightness_percent;
    s_dark_mode = values.dark_mode;
    s_clock_position = values.clock_position;
    s_dynamic_tint = values.dynamic_tint;
    s_show_nav = values.mini_player;
    s_library_keyboard_layout = values.keyboard_layout;
    s_accent_colour = values.accent_colour;
    lyra::i18n::set_language(values.language);
    s_speaker_output_enabled = values.speaker_output_enabled;
    s_equalizer_preset = static_cast<EqualizerPreset>(values.equalizer_preset);
    s_date_format = values.date_format < 3 ? static_cast<DateFormat>(values.date_format) :
                                             DateFormat::DayMonthYear;
    s_use_24_hour = values.use_24_hour;
    s_dst_enabled = values.dst_enabled;
    apply_time_sync_settings();
    std::memcpy(s_equalizer_custom_bands, values.equalizer_custom_bands,
                sizeof(s_equalizer_custom_bands));
    if (clear_fallback_dst && !s_setup_pending) save_user_settings();
}

void copy_ui_text(char *destination, size_t capacity, const char *source)
{
    if (!destination || capacity == 0) return;
    if (!source) source = "";
    const size_t length = std::min(std::strlen(source), capacity - 1);
    std::memcpy(destination, source, length);
    destination[length] = '\0';
}

bool append_ui_text(char *destination, size_t capacity, const char *suffix)
{
    if (!destination || capacity == 0 || !suffix) return false;
    const size_t used = std::strlen(destination);
    const size_t suffix_length = std::strlen(suffix);
    if (used + suffix_length + 1 > capacity) return false;
    std::memcpy(destination + used, suffix, suffix_length + 1);
    return true;
}

void render(View view);
void folder_back_cb(lv_event_t *event);
void cancel_power_action_cb(lv_event_t *event);
bool show_now_playing();
bool restart_current_track();
void apply_replay_gain_to_current_track();
void begin_crossfade_fade_in();

NavigationState capture_navigation_state()
{
    NavigationState state{
        s_view, s_library_tab, s_artist_detail_tab, s_list_page,
        s_scroll_body ? std::max<int32_t>(
            0, lv_obj_get_scroll_y(s_scroll_body)) : 0,
        s_selected_playlist,
        s_selected_playlist_is_smart, s_selected_smart_playlist, s_selected_group,
        s_selected_group_kind, s_playlists_from_library, s_playlist_add_mode, {}, {}, {}};
    copy_ui_text(state.track_list_title, sizeof(state.track_list_title), s_track_list_title);
    copy_ui_text(state.folder_path, sizeof(state.folder_path), s_folder_path);
    copy_ui_text(state.folder_playlist_path, sizeof(state.folder_playlist_path), s_folder_playlist_path);
    return state;
}

void push_navigation_state()
{
    const NavigationState state = capture_navigation_state();
    if (s_navigation_depth == kNavigationDepth) {
        std::move(s_navigation + 1, s_navigation + kNavigationDepth, s_navigation);
        --s_navigation_depth;
    }
    s_navigation[s_navigation_depth++] = state;
    ++s_navigation_serial;
}

void restore_navigation_state(const NavigationState &state)
{
    restore_navigation_state(state, state.list_page);
}

void restore_navigation_state(const NavigationState &state, size_t list_page)
{
    ++s_navigation_serial;
    s_library_tab = state.library_tab;
    s_artist_detail_tab = state.artist_detail_tab;
    s_list_page = list_page;
    s_selected_playlist = state.selected_playlist;
    s_selected_playlist_is_smart = state.selected_playlist_is_smart;
    s_selected_smart_playlist = state.selected_smart_playlist;
    s_selected_group = state.selected_group;
    s_selected_group_kind = state.selected_group_kind;
    s_playlists_from_library = state.playlists_from_library;
    s_playlist_add_mode = state.playlist_add_mode;
    s_pending_scroll_restore_y = std::max<int32_t>(0, state.scroll_y);
    copy_ui_text(s_track_list_title, sizeof(s_track_list_title), state.track_list_title);
    copy_ui_text(s_folder_path, sizeof(s_folder_path), state.folder_path);
    copy_ui_text(s_folder_playlist_path, sizeof(s_folder_playlist_path), state.folder_playlist_path);
    render(state.view);
}

void navigate_to(View target)
{
    if (target == s_view) return;
    push_navigation_state();
    s_list_page = 0;
    render(target);
}

void navigate_back(View fallback)
{
    if (s_view == View::PlaylistCreate) s_playlist_create_from_queue = false;
    if (s_navigation_depth) {
        restore_navigation_state(s_navigation[--s_navigation_depth]);
        return;
    }
    s_list_page = 0;
    ++s_navigation_serial;
    render(fallback);
}

int content_bottom()
{
    return kScreenHeight - (mini_player_visible() ? kNavHeight : 0);
}

int content_height(int top)
{
    return content_bottom() - top;
}

int library_keyboard_height()
{
    return mini_player_visible() ? kKeyboardHeightWithNav : kKeyboardHeightWithoutNav;
}

size_t library_page_size()
{
    return mini_player_visible() ? lyra::media::kTrackPageSize : 6;
}

size_t search_page_size()
{
    return mini_player_visible() ? kSearchPageSizeWithNav : kSearchPageSizeWithoutNav;
}

lv_obj_t *make_box(lv_obj_t *parent, int x, int y, int width, int height,
                   lv_color_t color, int radius, bool show_border)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, width, height);
    lv_obj_set_style_bg_color(box, color, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    // Light pages outline cards that ask for it; dark pages separate by tone.
    lv_obj_set_style_border_width(box, show_border && !s_dark_mode ? 1 : 0, 0);
    lv_obj_set_style_border_color(box, kDivider, 0);
    lv_obj_set_style_radius(box, design_radius(radius, width, height), 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    return box;
}

lv_obj_t *make_label(lv_obj_t *parent, const char *text, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_font(label, lyra::font::ui(), 0);
    return label;
}

void set_label_text_if_changed(lv_obj_t *label, const char *text)
{
    if (!label || !text) return;
    const char *current = lv_label_get_text(label);
    if (current && std::strcmp(current, text) == 0) return;
    lv_label_set_text(label, text);
}

namespace {

// Marquee-managed labels that rest as "…" carry this flag so a long press on
// their row can find and reveal them.
constexpr lv_obj_flag_t kTruncatedMarqueeFlag = LV_OBJ_FLAG_USER_4;
lv_style_t s_focus_marquee_style;
lv_style_t s_reveal_marquee_style;
lv_anim_t s_focus_marquee_animation;
lv_anim_t s_reveal_marquee_animation;
bool s_quiet_marquee_styles_ready = false;

void marquee_rest_cb(lv_anim_t *animation)
{
    // After its passes the label settles back to its first characters.
    // Leave scroll mode first so removing the template starts no new scroll.
    auto *label = static_cast<lv_obj_t *>(animation->var);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_remove_style(label, &s_focus_marquee_style, LV_STATE_DEFAULT);
    lv_obj_remove_style(label, &s_reveal_marquee_style, LV_STATE_DEFAULT);
}

void init_quiet_marquee_styles()
{
    if (s_quiet_marquee_styles_ready) return;
    // Slower than LVGL's default 40 px/s, so a scrolling name stays readable.
    const uint32_t speed = lv_anim_speed_clamped(30, 400, 20000);
    lv_anim_init(&s_focus_marquee_animation);
    lv_anim_set_delay(&s_focus_marquee_animation, 3000);
    lv_anim_set_repeat_delay(&s_focus_marquee_animation, 2500);
    lv_anim_set_repeat_count(&s_focus_marquee_animation, 3);
    lv_anim_set_completed_cb(&s_focus_marquee_animation, marquee_rest_cb);
    lv_style_init(&s_focus_marquee_style);
    lv_style_set_anim(&s_focus_marquee_style, &s_focus_marquee_animation);
    lv_style_set_anim_duration(&s_focus_marquee_style, speed);

    lv_anim_init(&s_reveal_marquee_animation);
    lv_anim_set_delay(&s_reveal_marquee_animation, 200);
    lv_anim_set_repeat_count(&s_reveal_marquee_animation, 1);
    lv_anim_set_completed_cb(&s_reveal_marquee_animation, marquee_rest_cb);
    lv_style_init(&s_reveal_marquee_style);
    lv_style_set_anim(&s_reveal_marquee_style, &s_reveal_marquee_animation);
    lv_style_set_anim_duration(&s_reveal_marquee_style, speed);
    s_quiet_marquee_styles_ready = true;
}

void fit_single_line(lv_obj_t *label)
{
    // "…" truncation needs a one-line box; otherwise LVGL wraps the text.
    // Callers therefore set a label's font before calling make_marquee().
    const lv_font_t *font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    const int32_t height = font ? lv_font_get_line_height(font) : 0;
    if (height > 0 && lv_obj_get_style_height(label, LV_PART_MAIN) != height) {
        lv_obj_set_height(label, height);
    }
}

void reveal_truncated_labels(lv_obj_t *parent)
{
    const uint32_t count = lv_obj_get_child_count(parent);
    for (uint32_t i = 0; i < count; ++i) {
        lv_obj_t *child = lv_obj_get_child(parent, i);
        if (lv_obj_check_type(child, &lv_label_class) &&
            lv_obj_has_flag(child, kTruncatedMarqueeFlag) &&
            lv_label_get_long_mode(child) == LV_LABEL_LONG_MODE_DOTS) {
            lv_obj_remove_style(child, &s_focus_marquee_style, LV_STATE_DEFAULT);
            lv_obj_add_style(child, &s_reveal_marquee_style, LV_STATE_DEFAULT);
            lv_label_set_long_mode(child, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
        }
        reveal_truncated_labels(child);
    }
}

void marquee_reveal_cb(lv_event_t *event)
{
    // Holding a row plays its cut-off text once instead of opening it.
    lv_obj_t *row = lv_event_get_current_target_obj(event);
    lv_indev_t *indev = lv_indev_active();
    if (indev) lv_indev_wait_release(indev);
    reveal_truncated_labels(row);
}

lv_obj_t *marquee_row(lv_obj_t *label)
{
    for (lv_obj_t *object = lv_obj_get_parent(label); object; object = lv_obj_get_parent(object)) {
        if (lv_obj_check_type(object, &lv_button_class)) return object;
    }
    return nullptr;
}

} // namespace

void rebind_virtual_row(VirtualRowWidgets &widgets, const VirtualRow &row)
{
    lv_obj_set_user_data(widgets.button, reinterpret_cast<void *>(row.payload));
    for (lv_obj_t *label : {widgets.title, widgets.subtitle}) {
        if (lv_label_get_long_mode(label) != LV_LABEL_LONG_MODE_DOTS) {
            // A long-press reveal belongs to the old row only.
            lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_remove_style(label, &s_reveal_marquee_style, LV_STATE_DEFAULT);
        }
    }
    set_label_text_if_changed(widgets.title, row.title);
    set_label_text_if_changed(widgets.subtitle, row.subtitle);
}

void disable_marquee_reveal(lv_obj_t *row)
{
    if (row) lv_obj_remove_event_cb(row, marquee_reveal_cb);
}

void make_marquee(lv_obj_t *label, int width, MarqueeMode mode)
{
    if (!label || width <= 0) return;
    if (mode != MarqueeMode::Continuous) {
        init_quiet_marquee_styles();
        lv_obj_set_width(label, width);
        lv_obj_clear_flag(label, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
        lv_obj_add_flag(label, kTruncatedMarqueeFlag);
        fit_single_line(label);
        if (lv_obj_t *row = marquee_row(label)) {
            // One handler per row, however many labels it holds.
            lv_obj_remove_event_cb(row, marquee_reveal_cb);
            lv_obj_add_event_cb(row, marquee_reveal_cb, LV_EVENT_LONG_PRESSED, nullptr);
        }
        if (mode == MarqueeMode::Focus) {
            lv_obj_add_style(label, &s_focus_marquee_style, LV_STATE_DEFAULT);
            lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
        } else {
            lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
        }
        return;
    }
    if (!s_marquee_style_ready) {
        lv_anim_init(&s_marquee_animation);
        lv_anim_set_delay(&s_marquee_animation, 2000);
        // Circular mode continues in the reading direction; the repeat delay
        // gives the text a moment at its starting edge before the next loop.
        lv_anim_set_repeat_delay(&s_marquee_animation, 1500);
        lv_anim_set_repeat_count(&s_marquee_animation, LV_ANIM_REPEAT_INFINITE);
        lv_style_init(&s_marquee_style);
        lv_style_set_anim(&s_marquee_style, &s_marquee_animation);
        s_marquee_style_ready = true;
    }
    lv_obj_set_width(label, width);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_add_style(label, &s_marquee_style, LV_STATE_DEFAULT);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
}

lv_obj_t *make_button(lv_obj_t *parent, int x, int y, int width, int height,
                      lv_color_t color, int radius, bool show_border)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, height);
    lv_obj_set_style_bg_color(button, color, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    // Accent buttons darken when pressed; everything else picks up a wash of
    // the accent, so a press always reads in the user's colour.
    const bool accent_fill = lv_color_eq(color, kAccent) || lv_color_eq(color, kAccentDark);
    lv_obj_set_style_bg_color(button, accent_fill ? lv_color_mix(lv_color_black(), color, 70) :
                              lv_color_mix(kAccent, color, 56), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(button, show_border && !s_dark_mode ? 1 : 0, 0);
    lv_obj_set_style_border_color(button, kDivider, 0);
    lv_obj_set_style_radius(button, design_radius(radius, width, height), 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    return button;
}

void configure_cover_aware_scroll(lv_obj_t *body)
{
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    // A slim position indicator appears only while a list is moving, so long
    // pages show where you are without adding work to idle frames.
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_width(body, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(body, 2, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_ver(body, 6, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(body, 2, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(body, kTextMuted, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(body, LV_OPA_70, LV_PART_SCROLLBAR);
    lv_obj_set_style_border_width(body, 0, LV_PART_SCROLLBAR);
    lv_obj_set_style_shadow_width(body, 0, LV_PART_SCROLLBAR);
}

lv_obj_t *make_scroll_body(int top)
{
    lv_obj_t *body = make_box(s_screen, 0, top, kScreenWidth, content_height(top), kBackground);
    configure_cover_aware_scroll(body);
    s_scroll_body = body;
    return body;
}

void style_root()
{
    reset_player_lyrics_state();
    reset_player_action_ui();
    close_control_centre();
    s_player_progress_bar = nullptr;
    s_player_progress_touch = nullptr;
    s_player_elapsed_label = nullptr;
    s_player_duration_label = nullptr;
    s_audio_eof_seen = false;
    s_player_progress_dragging = false;
    s_player_progress_drag_value = 0;
    s_playlist_picker = nullptr;
    s_volume_popup = nullptr;
    s_status_volume_label = nullptr;
    s_status_time_label = nullptr;
    s_scroll_body = nullptr;
    lv_obj_clean(s_screen);
    lv_obj_set_style_bg_color(s_screen, kBackground, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
}

void route_cb(lv_event_t *event)
{
    const View target = static_cast<View>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    if (target != s_view) {
        if (target == View::Menu) s_playlist_add_mode = false;
        if (target == View::PlaylistCreate) s_playlist_create_from_queue = false;
        if (target == View::Playlists && s_view != View::PlaylistDetail &&
            s_view != View::PlaylistCreate) s_playlists_from_library = false;
        navigate_to(target);
    }
}

void add_route(lv_obj_t *object, View target)
{
    lv_obj_add_event_cb(object, route_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(target)));
}

View back_view(View view)
{
    switch (view) {
        case View::TrackInfo: return View::Player;
        case View::FullscreenInfoArt: return View::TrackInfo;
        case View::Queue: return View::Player;
        case View::LibrarySongs:
        case View::LibraryArtists:
        case View::LibraryAlbums:
        case View::LibraryGenres:
        case View::LibraryYears: return View::Library;
        case View::InternetRadio: return View::Menu;
        case View::RadioAdd: return View::InternetRadio;
        case View::RadioFiles: return View::RadioAdd;
        case View::RadioPlaying: return View::InternetRadio;
        case View::AlbumDetail: return View::LibraryAlbums;
        case View::ArtistDetail: return View::LibraryArtists;
        case View::TrackList:
            return s_library_tab == LibraryTab::Artists ? View::LibraryArtists :
                   s_library_tab == LibraryTab::Genres ? View::LibraryGenres :
                   s_library_tab == LibraryTab::Years ? View::LibraryYears : View::Library;
        case View::FolderPlaylist:
        case View::FileNameInput:
        case View::FolderDetail: return View::Folders;
        case View::FileViewer: return file_viewer_back_view();
        case View::Photos:
        case View::Ebook: return View::Apps;
        case View::PlaylistDetail: return View::Playlists;
        case View::PlaylistCreate: return View::Playlists;
        case View::PlaylistAdd: return View::PlaylistDetail;
        case View::PlaybackSettings:
        case View::SoundSettings:
        case View::AppearanceSettings:
        case View::LibrarySettings:
        case View::SystemSettings:
        case View::WifiSettings:
        case View::ClockSettings:
        case View::LanguageOptions:
        case View::About: return View::Settings;
        case View::SortingSettings:
        case View::DatabaseStorage: return View::LibrarySettings;
        case View::WifiPassword: return View::WifiSettings;
        case View::NetworkTests: return View::DebugMenu;
        case View::ClockTimeSettings:
        case View::ClockDateSettings: return View::ClockSettings;
        case View::SetupWelcome:
        case View::SetupLanguage:
        case View::SetupClock:
        case View::SetupAppearance:
        case View::SetupScan:
        case View::SetupComplete: return View::Menu;
        case View::SortingOptions: return View::SortingSettings;
        case View::CrossfadeOptions:
        case View::SleepTimerOptions: return View::PlaybackSettings;
        case View::DebugMenu: return View::About;
        case View::Licenses: return View::About;
        case View::PageJump: return View::Menu;
        case View::EqualizerPresets: return View::Equalizer;
        case View::Equalizer: return View::SoundSettings;
        case View::Playlists: return s_playlists_from_library ? View::Library : View::Menu;
        case View::Apps:
        case View::Player:
        case View::Library:
        case View::Folders:
        case View::Search:
        case View::Settings:
        case View::Menu: return View::Menu;
    }
    return View::Menu;
}

void nav_back_cb(lv_event_t *)
{
    navigate_back(back_view(s_view));
}

} // namespace lyra::gui::internal
