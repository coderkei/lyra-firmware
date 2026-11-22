/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

// The parts of LyraOS that live around every page rather than on one: the
// mini player under browsing pages, the pull-down control centre, swipe
// navigation and the play-mode button shared by the player and the centre.
// The board has no hardware keys, so these carry what buttons would.

#include "../lyra_gui_internal.h"

namespace lyra::gui::internal {
namespace {

constexpr size_t kMaxPlayIcons = 4;
constexpr int kMiniArtSize = kRowThumbnailSize;

lv_obj_t *s_play_icons[kMaxPlayIcons]{};

lv_obj_t *s_mini_dock;
lv_obj_t *s_mini_art;
char s_mini_path[lyra::audio::kMaxPath];
bool s_mini_playing;
size_t s_mini_track = SIZE_MAX;

lv_obj_t *s_control_centre;
lv_obj_t *s_control_art;
size_t s_control_track = SIZE_MAX;

enum class ControlToggle : uintptr_t { Wifi, Speaker, Gapless, DarkMode, SleepTimer, Equalizer };

void forget_play_icon_cb(lv_event_t *event)
{
    lv_obj_t *label = lv_event_get_current_target_obj(event);
    for (lv_obj_t *&slot : s_play_icons) {
        if (slot == label) slot = nullptr;
    }
}

bool is_playing_now()
{
    const lyra::audio::Status status = lyra::audio::status();
    return status.playing && !status.paused;
}

bool view_shows_mini_player(View view)
{
    switch (view) {
        case View::Menu:
        case View::Apps:
        case View::Photos:
        case View::Ebook:
        case View::Library:
        case View::LibrarySongs:
        case View::LibraryArtists:
        case View::LibraryAlbums:
        case View::LibraryGenres:
        case View::LibraryYears:
        case View::AlbumDetail:
        case View::ArtistDetail:
        case View::TrackList:
        case View::Folders:
        case View::FolderDetail:
        case View::FolderPlaylist:
        case View::Playlists:
        case View::PlaylistDetail:
        case View::InternetRadio:
        case View::Queue:
            return true;
        default:
            return false;
    }
}

// Typing and reading views keep horizontal drags for their own controls.
bool view_allows_page_gestures(View view)
{
    switch (view) {
        case View::FileViewer:
        case View::Search:
        case View::PlaylistCreate:
        case View::WifiPassword:
        case View::RadioAdd:
        case View::FileNameInput:
        case View::SetupWelcome:
        case View::SetupLanguage:
        case View::SetupClock:
        case View::SetupAppearance:
        case View::SetupScan:
        case View::SetupComplete:
            return false;
        default:
            return !s_setup_pending;
    }
}

bool has_something_to_play()
{
    return s_has_active_queue || s_saved_queue_pending || lyra::audio::status().playing;
}

// A full-screen overlay (a dialog, a sheet) is on top; gestures must not
// navigate away underneath it.
bool overlay_is_open()
{
    const uint32_t count = lv_obj_get_child_count(s_screen);
    if (count == 0) return false;
    lv_obj_t *top = lv_obj_get_child(s_screen, static_cast<int32_t>(count) - 1);
    return lv_obj_get_height(top) >= kScreenHeight && lv_obj_get_y(top) <= 0;
}

void bind_transport(lv_obj_t *button, lv_event_cb_t callback)
{
    lv_obj_add_event_cb(button, callback, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(button, callback, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(button, callback, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(button, callback, LV_EVENT_PRESS_LOST, nullptr);
}

// Title and subtitle for whatever is loaded: the song, the radio station,
// or a gentle prompt when nothing is.
void describe_playback(const char **title, const char **subtitle, lyra::media::Track *track,
                       bool *has_track, bool *radio)
{
    const lyra::audio::Status playback = lyra::audio::status();
    *radio = playback.playing && is_radio_stream(playback.path);
    *has_track = !*radio && (s_has_active_queue || playback.playing) &&
                 lyra::media::track_at(s_current_track, track) && track->title[0];
    if (*radio) {
        *title = radio_station_name(playback.path);
        *subtitle = tr(lyra::i18n::StringId::InternetRadio);
    } else if (*has_track) {
        *title = track->title;
        *subtitle = track->artist;
    } else {
        *title = tr(lyra::i18n::StringId::NowPlaying);
        *subtitle = tr(lyra::i18n::StringId::NoTrackSelected);
    }
}

lv_obj_t *make_now_playing_art(lv_obj_t *parent, int x, int y, bool has_track, bool radio,
                               size_t *bound_track)
{
    *bound_track = SIZE_MAX;
    if (radio) {
        lv_obj_t *art = make_box(parent, x, y, kMiniArtSize, kMiniArtSize, kArtworkSurface, 8);
        lv_obj_remove_flag(art, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *icon = make_icon(art, Icon::Radio, kAccent, 24);
        lv_obj_center(icon);
        return nullptr;
    }
    lv_obj_t *image = make_row_thumbnail(parent, x, y);
    if (has_track && image) {
        *bound_track = s_current_track;
        bind_row_thumbnail(image, lyra::media::ThumbnailSource::Track, s_current_track);
        request_row_thumbnails(lyra::media::ThumbnailSource::Track, &s_current_track, 1);
    }
    return image;
}

void mini_open_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_GESTURE) {
        lv_indev_t *indev = lv_indev_active();
        if (!indev) return;
        const lv_dir_t dir = lv_indev_get_gesture_dir(indev);
        if (dir != LV_DIR_TOP && dir != LV_DIR_LEFT) return;
        lv_indev_wait_release(indev);
    }
    show_now_playing();
}

void mini_dock_deleted_cb(lv_event_t *)
{
    s_mini_dock = nullptr;
    s_mini_art = nullptr;
    s_mini_track = SIZE_MAX;
}

// ---- Control centre -------------------------------------------------------------

// The panel follows the finger: s_control_offset is how much of it shows,
// from 0 (hidden above the screen) to kControlPanelHeight (fully open).
constexpr int kControlPanelHeight = 440;
// The panel's top corners sit this far above the screen edge.
constexpr int kControlPanelOverhang = 24;
// Movement smaller than this is a tap, not a pull.
constexpr int kDragSlop = 8;

lv_obj_t *s_control_panel;
int s_control_offset;
int s_control_target;
int s_drag_start_y;
int s_drag_start_offset;
bool s_dragging;

enum class DragSource : uintptr_t { StatusBar, Panel, Overlay };

void build_control_centre(int offset);

void control_centre_deleted_cb(lv_event_t *)
{
    s_control_centre = nullptr;
    s_control_panel = nullptr;
    s_control_art = nullptr;
    s_control_track = SIZE_MAX;
    s_control_offset = 0;
    s_dragging = false;
}

void set_control_offset(int offset)
{
    s_control_offset = std::clamp(offset, 0, kControlPanelHeight);
    if (!s_control_centre || !s_control_panel) return;
    lv_obj_set_y(s_control_panel, s_control_offset - kControlPanelHeight - kControlPanelOverhang);
    // The page behind dims in step with how far the panel is open.
    lv_obj_set_style_bg_opa(s_control_centre, static_cast<lv_opa_t>(
        static_cast<int>(LV_OPA_60) * s_control_offset / kControlPanelHeight), 0);
}

void close_when_settled_cb(void *)
{
    // Runs after the closing animation; a new pull may have reopened it.
    if (s_control_centre && !s_dragging && s_control_offset == 0) close_control_centre();
}

void control_anim_exec(void *, int32_t value)
{
    set_control_offset(static_cast<int>(value));
}

void control_anim_done(lv_anim_t *)
{
    // Deleting the overlay here would delete the running animation's target,
    // so the panel is removed once LVGL has finished with it.
    if (s_control_target == 0) lv_async_call(close_when_settled_cb, nullptr);
}

void animate_control_centre(int target)
{
    if (!s_control_centre) return;
    s_control_target = target;
    lv_anim_delete(s_control_centre, control_anim_exec);
    if (s_control_offset == target) {
        control_anim_done(nullptr);
        return;
    }
    const int distance = std::abs(target - s_control_offset);
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, s_control_centre);
    lv_anim_set_values(&animation, s_control_offset, target);
    lv_anim_set_duration(&animation, static_cast<uint32_t>(80 + 180 * distance / kControlPanelHeight));
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&animation, control_anim_exec);
    lv_anim_set_completed_cb(&animation, control_anim_done);
    lv_anim_start(&animation);
}

// Pulling down on the status bar draws the panel out under the finger;
// dragging the panel (or the dimmed page) up pushes it back. Letting go
// settles it open or closed depending on how far it travelled.
void control_drag_cb(lv_event_t *event)
{
    const auto source = static_cast<DragSource>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    const lv_event_code_t code = lv_event_get_code(event);
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t point{};
    lv_indev_get_point(indev, &point);
    if (code == LV_EVENT_PRESSED) {
        s_drag_start_y = point.y;
        s_drag_start_offset = s_control_centre ? s_control_offset : 0;
        s_dragging = false;
        if (s_control_centre) lv_anim_delete(s_control_centre, control_anim_exec);
        return;
    }
    const int travel = point.y - s_drag_start_y;
    if (code == LV_EVENT_PRESSING) {
        if (!s_dragging) {
            // A tap near the top edge (say, aiming for Back) never opens it.
            if (std::abs(travel) < kDragSlop) return;
            if (source == DragSource::StatusBar && travel < 0) return;
            s_dragging = true;
            if (!s_control_centre) build_control_centre(0);
            // Start from the finger, not from where the slop was crossed.
            s_drag_start_y = point.y;
            s_drag_start_offset = s_control_offset;
            return;
        }
        set_control_offset(s_drag_start_offset + travel);
        return;
    }
    if (code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) return;
    if (!s_dragging) {
        // A tap on the dimmed page closes the panel.
        if (source == DragSource::Overlay && code == LV_EVENT_RELEASED) {
            animate_control_centre(0);
        } else if (s_control_centre) {
            // A press that stopped an animation part-way finishes the move.
            animate_control_centre(s_control_offset > kControlPanelHeight / 2 ?
                                   kControlPanelHeight : 0);
        }
        return;
    }
    s_dragging = false;
    // Opening needs a quarter of the panel; closing needs a quarter pushed back.
    const bool was_open = s_drag_start_offset > kControlPanelHeight / 2;
    const int threshold = was_open ? kControlPanelHeight * 3 / 4 : kControlPanelHeight / 4;
    animate_control_centre(s_control_offset > threshold ? kControlPanelHeight : 0);
}

void add_drag_events(lv_obj_t *object, DragSource source)
{
    void *data = reinterpret_cast<void *>(static_cast<uintptr_t>(source));
    lv_obj_add_event_cb(object, control_drag_cb, LV_EVENT_PRESSED, data);
    lv_obj_add_event_cb(object, control_drag_cb, LV_EVENT_PRESSING, data);
    lv_obj_add_event_cb(object, control_drag_cb, LV_EVENT_RELEASED, data);
    lv_obj_add_event_cb(object, control_drag_cb, LV_EVENT_PRESS_LOST, data);
}

void reopen_control_centre()
{
    close_control_centre();
    show_control_centre();
}

void control_toggle_cb(lv_event_t *event)
{
    const auto toggle = static_cast<ControlToggle>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    switch (toggle) {
        case ControlToggle::Wifi: {
            const lyra::wifi::Status wifi = lyra::wifi::status();
            const esp_err_t result = lyra::wifi::set_enabled(!wifi.enabled);
            if (result != ESP_OK) {
                ESP_LOGW(kTag, "could not switch Wi-Fi: %s", esp_err_to_name(result));
            }
            break;
        }
        case ControlToggle::Speaker: {
            const bool enabled = !s_speaker_output_enabled;
            const esp_err_t result = lyra::audio::set_speaker_output_enabled(enabled);
            if (result == ESP_OK) {
                s_speaker_output_enabled = enabled;
                save_user_settings();
            } else {
                ESP_LOGW(kTag, "could not change on-board speaker output: %s",
                         esp_err_to_name(result));
            }
            break;
        }
        case ControlToggle::Gapless:
            s_gapless = !s_gapless;
            save_user_settings();
            break;
        case ControlToggle::DarkMode:
            s_dark_mode = !s_dark_mode;
            apply_theme_palette();
            save_user_settings();
            // Rebuild the page underneath in the new colours, then come back.
            render(s_view);
            show_control_centre();
            return;
        case ControlToggle::SleepTimer: {
            constexpr uint16_t kSteps[] = {0, 15, 30, 45, 60};
            size_t next = 0;
            for (size_t i = 0; i < sizeof(kSteps) / sizeof(kSteps[0]); ++i) {
                if (kSteps[i] == s_sleep_timer_minutes) next = (i + 1) % (sizeof(kSteps) / sizeof(kSteps[0]));
            }
            s_sleep_timer_minutes = kSteps[next];
            s_sleep_timer_deadline_us = s_sleep_timer_minutes == 0 ? 0 :
                esp_timer_get_time() + static_cast<int64_t>(s_sleep_timer_minutes) * 60 * 1000 * 1000;
            break;
        }
        case ControlToggle::Equalizer:
            close_control_centre();
            navigate_to(View::Equalizer);
            return;
    }
    reopen_control_centre();
}

const char *sleep_timer_label()
{
    switch (s_sleep_timer_minutes) {
        case 15: return tr(lyra::i18n::StringId::FifteenMinutes);
        case 30: return tr(lyra::i18n::StringId::ThirtyMinutes);
        case 45: return tr(lyra::i18n::StringId::FortyFiveMinutes);
        case 60: return tr(lyra::i18n::StringId::SixtyMinutes);
        default: return tr(lyra::i18n::StringId::SleepTimer);
    }
}

void make_control_toggle(lv_obj_t *panel, int x, int y, Icon icon, const char *label,
                         bool on, ControlToggle toggle)
{
    constexpr int kWidth = (kScreenWidth - 2 * kPageInset - 10) / 2;
    lv_obj_t *button = make_button(panel, x, y, kWidth, 48, on ? kAccent : kSurfaceRaised,
                                   kRowRadius);
    const lv_color_t color = on ? kTextOnAccent : kTextPrimary;
    lv_obj_t *glyph = make_icon(button, icon, color);
    lv_obj_align(glyph, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_t *text = make_label(button, label, color);
    make_marquee(text, kWidth - 50);
    lv_obj_align(text, LV_ALIGN_LEFT_MID, 40, 0);
    lv_obj_add_event_cb(button, control_toggle_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(toggle)));
}

void format_short_date(const lyra::clock::DateTime &value, char *output, size_t capacity)
{
    switch (s_date_format) {
        case DateFormat::DayMonthYear:
            std::snprintf(output, capacity, "%02d/%02d/%04d", value.day, value.month, value.year);
            break;
        case DateFormat::MonthDayYear:
            std::snprintf(output, capacity, "%02d/%02d/%04d", value.month, value.day, value.year);
            break;
        case DateFormat::YearMonthDay:
            std::snprintf(output, capacity, "%04d-%02d-%02d", value.year, value.month, value.day);
            break;
    }
}

lv_obj_t *make_control_slider(lv_obj_t *panel, int y, Icon icon, int minimum, int value)
{
    lv_obj_t *glyph = make_icon(panel, icon, kTextSecondary, 22);
    lv_obj_set_pos(glyph, 20, y - 4);
    lv_obj_t *slider = lv_slider_create(panel);
    lv_obj_set_pos(slider, 58, y);
    lv_obj_set_size(slider, kScreenWidth - 58 - 24, 14);
    lv_slider_set_range(slider, minimum, 100);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
    style_slider(slider);
    return slider;
}

} // namespace

void track_play_icon(lv_obj_t *label)
{
    if (!label) return;
    for (lv_obj_t *&slot : s_play_icons) {
        if (!slot) {
            slot = label;
            lv_obj_add_event_cb(label, forget_play_icon_cb, LV_EVENT_DELETE, nullptr);
            return;
        }
    }
}

bool mini_player_visible()
{
    return s_show_nav && !s_setup_pending && view_shows_mini_player(s_view);
}

void make_mini_player()
{
    s_mini_dock = nullptr;
    if (!mini_player_visible()) return;
    lv_obj_t *dock = make_box(s_screen, 0, kScreenHeight - kNavHeight, kScreenWidth, kNavHeight,
                              kNavSurface);
    s_mini_dock = dock;
    lv_obj_add_event_cb(dock, mini_dock_deleted_cb, LV_EVENT_DELETE, nullptr);
    lv_obj_remove_flag(make_box(dock, 0, 0, kScreenWidth, 1, kDivider), LV_OBJ_FLAG_CLICKABLE);
    // Tap to open Now Playing, or push it up like a card.
    lv_obj_remove_flag(dock, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(dock, mini_open_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(dock, mini_open_cb, LV_EVENT_GESTURE, nullptr);

    lyra::media::Track track{};
    const char *title = nullptr;
    const char *subtitle = nullptr;
    bool has_track = false;
    bool radio = false;
    describe_playback(&title, &subtitle, &track, &has_track, &radio);
    const int art_y = (kNavHeight - kMiniArtSize) / 2;
    s_mini_art = make_now_playing_art(dock, kPageInset, art_y, has_track, radio, &s_mini_track);

    constexpr int kTextX = kPageInset + kMiniArtSize + 10;
    constexpr int kTextWidth = 222 - kTextX - 6;
    lv_obj_t *title_label = make_label(dock, title, kTextPrimary);
    make_marquee(title_label, kTextWidth);
    lv_obj_set_pos(title_label, kTextX, 9);
    lv_obj_t *subtitle_label = make_label(dock, subtitle, kTextSecondary);
    make_marquee(subtitle_label, kTextWidth);
    lv_obj_set_pos(subtitle_label, kTextX, 31);
    disable_marquee_reveal(dock);

    const bool playing = is_playing_now();
    lv_obj_t *play = make_symbol_button(dock, 222, (kNavHeight - 44) / 2, 44,
                                        playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY, kTextPrimary);
    lv_obj_add_event_cb(play, nav_play_cb, LV_EVENT_CLICKED, nullptr);
    track_play_icon(lv_obj_get_child(play, 0));
    lv_obj_t *next = make_symbol_button(dock, 268, (kNavHeight - 44) / 2, 44, LV_SYMBOL_NEXT,
                                        kTextPrimary);
    bind_transport(next, nav_next_cb);

    const lyra::audio::Status status = lyra::audio::status();
    copy_ui_text(s_mini_path, sizeof(s_mini_path), status.path);
    s_mini_playing = status.playing;
}

void update_nav_play_icon()
{
    const char *symbol = is_playing_now() ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY;
    for (lv_obj_t *label : s_play_icons) {
        if (label) set_label_text_if_changed(label, symbol);
    }
}

void update_menu_playback()
{
    update_nav_play_icon();
    const lyra::audio::Status status = lyra::audio::status();
    if (s_mini_dock && (status.playing != s_mini_playing ||
                        std::strcmp(status.path, s_mini_path) != 0)) {
        // A new song, a station, or playback starting or stopping changes
        // the whole strip: rebuild it in place.
        lv_obj_delete(s_mini_dock);
        make_mini_player();
        if (s_control_centre) lv_obj_move_foreground(s_control_centre);
    }
    // Covers load off the GUI thread; show one as soon as it is ready.
    if (s_mini_art && s_mini_track != SIZE_MAX) {
        bind_row_thumbnail(s_mini_art, lyra::media::ThumbnailSource::Track, s_mini_track);
    }
    if (s_control_art && s_control_track != SIZE_MAX) {
        bind_row_thumbnail(s_control_art, lyra::media::ThumbnailSource::Track, s_control_track);
    }
}

namespace {

void build_control_centre(int offset)
{
    if (s_control_centre || !s_screen) return;
    lv_obj_t *overlay = make_box(s_screen, 0, 0, kScreenWidth, kScreenHeight, kOverlay);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_move_foreground(overlay);
    s_control_centre = overlay;
    lv_obj_add_event_cb(overlay, control_centre_deleted_cb, LV_EVENT_DELETE, nullptr);
    add_drag_events(overlay, DragSource::Overlay);

    constexpr int kPanelHeight = kControlPanelHeight;
    constexpr int kInnerWidth = kScreenWidth - 2 * kPageInset;
    // The panel drops from the top edge, so only its lower corners round.
    lv_obj_t *panel = make_box(overlay, 0, -kControlPanelOverhang, kScreenWidth,
                               kPanelHeight + kControlPanelOverhang, kSurface, 24);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_GESTURE_BUBBLE);
    s_control_panel = panel;
    add_drag_events(panel, DragSource::Panel);
    // Drags on the panel's empty areas (not its controls) move it.
    lv_obj_t *content = make_box(panel, 0, kControlPanelOverhang, kScreenWidth, kPanelHeight,
                                 kSurface);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_CLICKABLE);

    const lyra::clock::DateTime now = lyra::clock::now();
    char time_text[16];
    format_clock_time(now, time_text, sizeof(time_text));
    lv_obj_t *clock = make_label(content, time_text, kTextPrimary);
    lv_obj_set_style_text_font(clock, display_font(), 0);
    lv_obj_set_pos(clock, 20, 14);
    char date_text[16];
    format_short_date(now, date_text, sizeof(date_text));
    lv_obj_t *date = make_label(content, date_text, kTextSecondary);
    lv_obj_set_pos(date, 22, 52);
    lv_obj_t *settings = make_icon_button(content, kScreenWidth - 58, 18, 44, Icon::Settings,
                                          kTextPrimary);
    lv_obj_add_event_cb(settings, [](lv_event_t *) {
        close_control_centre();
        navigate_to(View::Settings);
    }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *brightness = make_control_slider(content, 96, Icon::Brightness, 1, s_brightness_percent);
    lv_obj_add_event_cb(brightness, brightness_slider_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(brightness, brightness_slider_released_cb, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(brightness, brightness_slider_released_cb, LV_EVENT_PRESS_LOST, nullptr);
    const lyra::audio::Status audio = lyra::audio::status();
    lv_obj_t *volume = make_control_slider(content, 136, Icon::Sound, 0,
                                           display_volume_percent(audio.volume_percent));
    lv_obj_add_event_cb(volume, volume_slider_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(volume, volume_slider_released_cb, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(volume, volume_slider_released_cb, LV_EVENT_PRESS_LOST, nullptr);

    // What is playing, with its transport.
    constexpr int kCardY = 170;
    lv_obj_t *card = make_button(content, kPageInset, kCardY, kInnerWidth, 64, kSurfaceRaised,
                                 kCardRadius);
    lv_obj_add_event_cb(card, [](lv_event_t *) {
        close_control_centre();
        show_now_playing();
    }, LV_EVENT_CLICKED, nullptr);
    lyra::media::Track track{};
    const char *title = nullptr;
    const char *subtitle = nullptr;
    bool has_track = false;
    bool radio = false;
    describe_playback(&title, &subtitle, &track, &has_track, &radio);
    s_control_art = make_now_playing_art(card, 8, 8, has_track, radio, &s_control_track);
    constexpr int kCardTextX = 8 + kMiniArtSize + 10;
    constexpr int kCardTextWidth = kInnerWidth - kCardTextX - 132;
    lv_obj_t *card_title = make_label(card, title, kTextPrimary);
    make_marquee(card_title, kCardTextWidth);
    lv_obj_set_pos(card_title, kCardTextX, 10);
    lv_obj_t *card_subtitle = make_label(card, subtitle, kTextSecondary);
    make_marquee(card_subtitle, kCardTextWidth);
    lv_obj_set_pos(card_subtitle, kCardTextX, 32);
    disable_marquee_reveal(card);
    lv_obj_t *previous = make_symbol_button(card, kInnerWidth - 130, 10, 44, LV_SYMBOL_PREV,
                                            kTextPrimary);
    bind_transport(previous, nav_previous_cb);
    lv_obj_t *play = make_symbol_button(card, kInnerWidth - 88, 10, 44,
                                        is_playing_now() ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY,
                                        kTextPrimary);
    lv_obj_add_event_cb(play, nav_play_cb, LV_EVENT_CLICKED, nullptr);
    track_play_icon(lv_obj_get_child(play, 0));
    lv_obj_t *next = make_symbol_button(card, kInnerWidth - 46, 10, 44, LV_SYMBOL_NEXT,
                                        kTextPrimary);
    bind_transport(next, nav_next_cb);

    // Quick toggles, two to a row.
    constexpr int kToggleY = kCardY + 64 + 14;
    constexpr int kToggleColumn = kPageInset + (kInnerWidth - 10) / 2 + 10;
    const lyra::wifi::Status wifi = lyra::wifi::status();
    make_control_toggle(content, kPageInset, kToggleY, Icon::Wifi,
                        tr(lyra::i18n::StringId::WifiSettings), wifi.enabled, ControlToggle::Wifi);
    make_control_toggle(content, kToggleColumn, kToggleY, Icon::Sound,
                        tr(lyra::i18n::StringId::OnBoardSpeaker), s_speaker_output_enabled,
                        ControlToggle::Speaker);
    make_control_toggle(content, kPageInset, kToggleY + 56, Icon::Crossfade,
                        tr(lyra::i18n::StringId::GaplessPlayback), s_gapless,
                        ControlToggle::Gapless);
    make_control_toggle(content, kToggleColumn, kToggleY + 56, Icon::Theme,
                        tr(lyra::i18n::StringId::ReaderThemeDark), s_dark_mode,
                        ControlToggle::DarkMode);
    make_control_toggle(content, kPageInset, kToggleY + 112, Icon::Sleep, sleep_timer_label(),
                        s_sleep_timer_minutes != 0, ControlToggle::SleepTimer);
    make_control_toggle(content, kToggleColumn, kToggleY + 112, Icon::Equalizer,
                        tr(lyra::i18n::StringId::Equalizer),
                        s_equalizer_preset != EqualizerPreset::Flat, ControlToggle::Equalizer);

    // A grab handle under the toggles: drag up anywhere to close.
    lv_obj_remove_flag(make_box(content, (kScreenWidth - 44) / 2, kPanelHeight - 14, 44, 5,
                                kDivider, LV_RADIUS_CIRCLE), LV_OBJ_FLAG_CLICKABLE);
    set_control_offset(offset);
}

} // namespace

void show_control_centre()
{
    // Opened by code (after a toggle rebuilds it): fully open, no animation.
    build_control_centre(kControlPanelHeight);
}

void control_centre_pull_events(lv_obj_t *status_bar)
{
    add_drag_events(status_bar, DragSource::StatusBar);
}

void close_control_centre()
{
    if (!s_control_centre) return;
    lv_obj_t *centre = s_control_centre;
    s_control_centre = nullptr;
    s_control_panel = nullptr;
    s_control_offset = 0;
    s_dragging = false;
    lv_obj_delete(centre);
}

void install_gesture_navigation()
{
    static bool installed = false;
    if (installed || !s_screen) return;
    installed = true;
    lv_obj_add_event_cb(s_screen, [](lv_event_t *) {
        lv_indev_t *indev = lv_indev_active();
        if (!indev || s_control_centre || !view_allows_page_gestures(s_view) ||
            overlay_is_open()) return;
        const lv_dir_t dir = lv_indev_get_gesture_dir(indev);
        if (dir == LV_DIR_RIGHT && s_view != View::Menu) {
            lv_indev_wait_release(indev);
            navigate_back(back_view(s_view));
        } else if (dir == LV_DIR_LEFT) {
            if (s_view == View::Player) {
                lv_indev_wait_release(indev);
                open_queue_cb(nullptr);
            } else if (s_view != View::RadioPlaying && s_view != View::Queue &&
                       has_something_to_play()) {
                lv_indev_wait_release(indev);
                show_now_playing();
            }
        }
    }, LV_EVENT_GESTURE, nullptr);
}

void block_page_gestures(lv_obj_t *object)
{
    if (object) lv_obj_remove_flag(object, LV_OBJ_FLAG_GESTURE_BUBBLE);
}

void cycle_play_mode()
{
    // In order -> repeat all -> repeat one -> shuffle -> in order.
    lyra::i18n::StringId announcement = lyra::i18n::StringId::ModeInOrder;
    if (s_shuffle) {
        s_shuffle = false;
        reset_shuffle_queue();
        s_queue_position_valid = false;
        s_repeat_mode = RepeatMode::Off;
    } else if (s_repeat_mode == RepeatMode::Off) {
        s_repeat_mode = RepeatMode::All;
        announcement = lyra::i18n::StringId::ModeRepeatAll;
    } else if (s_repeat_mode == RepeatMode::All) {
        s_repeat_mode = RepeatMode::Song;
        announcement = lyra::i18n::StringId::ModeRepeatOne;
    } else {
        s_repeat_mode = RepeatMode::Off;
        s_shuffle = true;
        reset_shuffle_queue();
        if (build_shuffle_queue()) {
            announcement = lyra::i18n::StringId::ModeShuffle;
        } else {
            s_shuffle = false;
            ESP_LOGW(kTag, "could not allocate shuffle queue");
        }
    }
    // A finished queue can continue under the new mode.
    if ((s_shuffle || s_repeat_mode != RepeatMode::Off) && lyra::audio::status().eof) {
        s_audio_eof_seen = false;
    }
    render(s_view);
    show_toast(tr(announcement));
}

} // namespace lyra::gui::internal
