/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../lyra_gui_internal.h"

namespace lyra::gui::internal {

void apply_theme_palette()
{
    const size_t accent_index = static_cast<size_t>(s_accent_colour) < kAccentPaletteCount ?
                                s_accent_colour : 0;
    const AccentPalette &accent = kAccentPalettes[accent_index];
    if (s_dark_mode) {
        kBackground = lv_color_hex(0x080C12);
        kSurface = lv_color_hex(0x101720);
        kSurfaceRaised = lv_color_hex(0x18212D);
        kAccent = lv_color_hex(accent.dark_rgb);
        kAccentDark = lv_color_hex(accent.dark_pressed_rgb);
        kAccentSurface = lv_color_hex(accent.dark_surface_rgb);
        kTextPrimary = lv_color_hex(0xF4F7FB);
        kTextSecondary = lv_color_hex(0xCBD5E1);
        kTextMuted = lv_color_hex(0x718096);
        kDivider = lv_color_hex(0x25303D);
        kNavSurface = lv_color_hex(0x0B111A);
        kKeyboardSurface = lv_color_hex(0x05080D);
        kArtworkSurface = lv_color_hex(0x142131);
        kDangerSurface = lv_color_hex(0x3F1118);
    } else {
        kBackground = lv_color_hex(0xF8FAFC);
        kSurface = lv_color_hex(0xFFFFFF);
        kSurfaceRaised = lv_color_hex(0xF1F5F9);
        kAccent = lv_color_hex(accent.light_rgb);
        kAccentDark = lv_color_hex(accent.light_pressed_rgb);
        kAccentSurface = lv_color_hex(accent.light_surface_rgb);
        kTextPrimary = lv_color_hex(0x172033);
        kTextSecondary = lv_color_hex(0x475569);
        kTextMuted = lv_color_hex(0x64748B);
        kDivider = lv_color_hex(0xCBD5E1);
        kNavSurface = lv_color_hex(0xFFFFFF);
        kKeyboardSurface = lv_color_hex(0xE2E8F0);
        kArtworkSurface = lv_color_hex(0xE2E8F0);
        kDangerSurface = lv_color_hex(0xFEE2E2);
    }
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
    lyra::gui_settings::Values values{
        s_gapless,
        s_replay_gain,
        s_crossfade_seconds,
        s_brightness_percent,
        s_dark_mode,
        s_accent_colour,
        lyra::i18n::current_language(),
        s_speaker_output_enabled,
        static_cast<uint8_t>(s_equalizer_preset),
        {},
        static_cast<uint8_t>(s_date_format),
        s_use_24_hour,
        s_dst_enabled,
        true,
    };
    std::memcpy(values.equalizer_custom_bands, s_equalizer_custom_bands,
                sizeof(values.equalizer_custom_bands));
    return lyra::gui_settings::save(values);
}

void load_user_settings()
{
    lyra::gui_settings::Values values{
        s_gapless,
        s_replay_gain,
        s_crossfade_seconds,
        s_brightness_percent,
        s_dark_mode,
        s_accent_colour,
        lyra::i18n::Language::English,
        s_speaker_output_enabled,
        static_cast<uint8_t>(s_equalizer_preset),
        {},
        static_cast<uint8_t>(s_date_format),
        s_use_24_hour,
        s_dst_enabled,
        false,
    };
    std::memcpy(values.equalizer_custom_bands, s_equalizer_custom_bands,
                sizeof(values.equalizer_custom_bands));
    lyra::gui_settings::load(&values, kAccentPaletteCount,
                             static_cast<uint8_t>(EqualizerPreset::Count));
    s_language_setup_pending = !values.language_selected;
    const bool clear_fallback_dst = lyra::clock::is_using_default_time() && values.dst_enabled;
    if (clear_fallback_dst) values.dst_enabled = false;
    s_gapless = values.gapless;
    s_replay_gain = values.replay_gain;
    s_crossfade_seconds = values.crossfade_seconds;
    s_brightness_percent = values.brightness_percent;
    s_dark_mode = values.dark_mode;
    s_accent_colour = values.accent_colour;
    lyra::i18n::set_language(values.language);
    s_speaker_output_enabled = values.speaker_output_enabled;
    s_equalizer_preset = static_cast<EqualizerPreset>(values.equalizer_preset);
    s_date_format = values.date_format < 3 ? static_cast<DateFormat>(values.date_format) :
                                             DateFormat::DayMonthYear;
    s_use_24_hour = values.use_24_hour;
    s_dst_enabled = values.dst_enabled;
    std::memcpy(s_equalizer_custom_bands, values.equalizer_custom_bands,
                sizeof(s_equalizer_custom_bands));
    if (clear_fallback_dst && !s_language_setup_pending) save_user_settings();
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
        s_view, s_library_tab, s_artist_detail_tab, s_list_page, s_selected_playlist,
        s_selected_playlist_is_smart, s_selected_smart_playlist, s_selected_group,
        s_selected_group_kind, s_playlists_from_library, s_playlist_add_mode, {}, {}};
    copy_ui_text(state.track_list_title, sizeof(state.track_list_title), s_track_list_title);
    copy_ui_text(state.folder_path, sizeof(state.folder_path), s_folder_path);
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
}

void restore_navigation_state(const NavigationState &state)
{
    s_library_tab = state.library_tab;
    s_artist_detail_tab = state.artist_detail_tab;
    s_list_page = state.list_page;
    s_selected_playlist = state.selected_playlist;
    s_selected_playlist_is_smart = state.selected_playlist_is_smart;
    s_selected_smart_playlist = state.selected_smart_playlist;
    s_selected_group = state.selected_group;
    s_selected_group_kind = state.selected_group_kind;
    s_playlists_from_library = state.playlists_from_library;
    s_playlist_add_mode = state.playlist_add_mode;
    copy_ui_text(s_track_list_title, sizeof(s_track_list_title), state.track_list_title);
    copy_ui_text(s_folder_path, sizeof(s_folder_path), state.folder_path);
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
    if (s_navigation_depth) {
        restore_navigation_state(s_navigation[--s_navigation_depth]);
        return;
    }
    s_list_page = 0;
    render(fallback);
}

int content_bottom()
{
    return kScreenHeight - (s_show_nav && !s_language_setup_pending ? kNavHeight : 0);
}

int content_height(int top)
{
    return content_bottom() - top;
}

int library_keyboard_height()
{
    return s_show_nav ? kKeyboardHeightWithNav : kKeyboardHeightWithoutNav;
}

size_t library_page_size()
{
    return s_show_nav ? lyra::media::kTrackPageSize : 6;
}

size_t search_page_size()
{
    return s_show_nav ? kSearchPageSizeWithNav : kSearchPageSizeWithoutNav;
}

lv_obj_t *make_box(lv_obj_t *parent, int x, int y, int width, int height,
                   lv_color_t color, int radius, bool show_border)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, width, height);
    lv_obj_set_style_bg_color(box, color, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(box, show_border && !s_dark_mode ? 1 : 0, 0);
    lv_obj_set_style_border_color(box, kDivider, 0);
    lv_obj_set_style_radius(box, radius, 0);
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

void make_marquee(lv_obj_t *label, int width)
{
    if (!label || width <= 0) return;
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
    lv_obj_set_style_bg_color(button, kAccentDark, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(button, show_border && !s_dark_mode ? 1 : 0, 0);
    lv_obj_set_style_border_color(button, kDivider, 0);
    lv_obj_set_style_radius(button, radius, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    return button;
}

void make_equalizer_icon(lv_obj_t *parent, lv_color_t color)
{
    constexpr int heights[] = {7, 14, 10, 17};
    for (size_t i = 0; i < sizeof(heights) / sizeof(heights[0]); ++i) {
        lv_obj_t *bar = make_box(parent, 12 + static_cast<int>(i) * 6,
                                 27 - heights[i], 3, heights[i], color, 2);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    }
}

void configure_cover_aware_scroll(lv_obj_t *body)
{
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(body, [](lv_event_t *) { s_list_scrolling = true; }, LV_EVENT_SCROLL_BEGIN, nullptr);
    lv_obj_add_event_cb(body, [](lv_event_t *event) {
        s_list_scrolling = false;
        // Covers entering the viewport were deliberately skipped during the
        // swipe. Refresh once at rest to populate only the final visible rows.
        lv_obj_invalidate(lv_event_get_current_target_obj(event));
    }, LV_EVENT_SCROLL_END, nullptr);
}

lv_obj_t *make_scroll_body(int top)
{
    lv_obj_t *body = make_box(s_screen, 0, top, kScreenWidth, content_height(top), kBackground);
    configure_cover_aware_scroll(body);
    return body;
}

void style_root()
{
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
        case View::Queue: return View::Menu;
        case View::LibrarySongs:
        case View::LibraryArtists:
        case View::LibraryAlbums:
        case View::LibraryGenres:
        case View::LibraryYears: return View::Library;
        case View::AlbumDetail: return View::LibraryAlbums;
        case View::ArtistDetail: return View::LibraryArtists;
        case View::TrackList:
            return s_library_tab == LibraryTab::Artists ? View::LibraryArtists :
                   s_library_tab == LibraryTab::Genres ? View::LibraryGenres :
                   s_library_tab == LibraryTab::Years ? View::LibraryYears : View::Library;
        case View::FolderDetail: return View::Folders;
        case View::PlaylistDetail: return View::Playlists;
        case View::PlaylistCreate: return View::Playlists;
        case View::PlaylistAdd: return View::PlaylistDetail;
        case View::PlaybackSettings:
        case View::SoundSettings:
        case View::DisplaySettings:
        case View::SystemSettings:
        case View::SortingSettings: return View::Settings;
        case View::ClockSettings: return View::SystemSettings;
        case View::ClockTimeSettings:
        case View::ClockDateSettings: return View::ClockSettings;
        case View::LanguageOptions: return View::SystemSettings;
        case View::SortingOptions: return View::SortingSettings;
        case View::CrossfadeOptions:
        case View::SleepTimerOptions: return View::PlaybackSettings;
        case View::About: return View::Settings;
        case View::DebugMenu: return View::About;
        case View::Licenses: return View::Settings;
        case View::DatabaseStorage: return View::SystemSettings;
        case View::EqualizerPresets: return View::Equalizer;
        case View::Playlists: return s_playlists_from_library ? View::Library : View::Menu;
        case View::Player:
        case View::Library:
        case View::Folders:
        case View::Equalizer:
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
