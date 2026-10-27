/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../lyra_gui_internal.h"
#include "lyra_radio.h"

namespace lyra::gui::internal {
namespace {

constexpr size_t kRadioFilePageSize = 5;
char s_radio_file_path[lyra::media::kMaxPath] = "/sdcard";
char s_current_station_name[lyra::radio::kStationNameSize] = "";
lv_obj_t *s_radio_status_label = nullptr;
lv_timer_t *s_radio_status_timer = nullptr;
lyra::gui_settings::KeyboardLayout s_radio_previous_keyboard_layout =
    lyra::gui_settings::KeyboardLayout::English;
bool s_radio_previous_keyboard_symbols = false;
bool s_radio_previous_keyboard_accents = false;
bool s_radio_keyboard_layout_overridden = false;

void prepare_radio_add_keyboard()
{
    if (s_radio_keyboard_layout_overridden) return;
    s_radio_previous_keyboard_layout = s_library_keyboard_layout;
    s_radio_previous_keyboard_symbols = s_library_keyboard_symbols;
    s_radio_previous_keyboard_accents = s_library_keyboard_accents;
    s_radio_keyboard_layout_overridden = true;
    s_library_keyboard_layout = lyra::gui_settings::KeyboardLayout::English;
    s_library_keyboard_symbols = false;
    s_library_keyboard_accents = false;
}

void open_radio_add_cb(lv_event_t *)
{
    s_radio_url[0] = '\0';
    render(View::RadioAdd);
}

void radio_station_cb(lv_event_t *event)
{
    const size_t index = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)) - 1;
    lyra::radio::Station station{};
    if (!lyra::radio::station_at(index, &station)) return;
    copy_ui_text(s_current_station_name, sizeof(s_current_station_name), station.name);
    s_audio_eof_seen = false;
    s_pending_track_advance = false;
    s_pending_track_advance_us = 0;
    s_crossfade_fade_in_started_us = 0;
    s_crossfade_fade_in_ends_us = 0;
    s_crossfade_fade_out_started_us = 0;
    s_crossfade_fade_out_ends_us = 0;
    s_crossfade_transition_direction = 0;
    s_crossfade_pause_pending = false;
    lyra::audio::set_transition_gain(100);
    const esp_err_t result = lyra::audio::play_stream(station.url);
    if (result != ESP_OK) {
        show_notice(tr(lyra::i18n::StringId::InternetRadio),
                    tr(lyra::i18n::StringId::RadioStreamFailed));
        return;
    }
    render(View::RadioPlaying);
}

void radio_remove_cb(lv_event_t *event)
{
    lv_event_stop_bubbling(event);
    const size_t index = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)) - 1;
    if (lyra::radio::remove_station(index) != ESP_OK) {
        show_notice(tr(lyra::i18n::StringId::InternetRadio),
                    tr(lyra::i18n::StringId::RadioSaveFailed));
        return;
    }
    render(View::InternetRadio);
}

void open_radio_files_cb(lv_event_t *)
{
    copy_ui_text(s_radio_file_path, sizeof(s_radio_file_path), "/sdcard");
    s_list_page = 0;
    render(View::RadioFiles);
}

void radio_directory_back_cb(lv_event_t *)
{
    if (std::strcmp(s_radio_file_path, "/sdcard") == 0) {
        render(View::RadioAdd);
        return;
    }
    char *slash = std::strrchr(s_radio_file_path, '/');
    if (slash && slash > s_radio_file_path + 6) *slash = '\0';
    else copy_ui_text(s_radio_file_path, sizeof(s_radio_file_path), "/sdcard");
    s_list_page = 0;
    render(View::RadioFiles);
}

void radio_browser_entry_cb(lv_event_t *event)
{
    auto *entry = static_cast<lyra::radio::BrowserEntry *>(lv_event_get_user_data(event));
    if (!entry) return;
    if (entry->directory) {
        copy_ui_text(s_radio_file_path, sizeof(s_radio_file_path), entry->path);
        s_list_page = 0;
        render(View::RadioFiles);
        return;
    }
    size_t added = 0;
    if (lyra::radio::import_playlist_file(entry->path, &added) == ESP_OK && added > 0) {
        render(View::InternetRadio);
        return;
    }
    show_notice(tr(lyra::i18n::StringId::InternetRadio),
                tr(lyra::i18n::StringId::RadioImportFailed));
}

// Shows the track announced by the station while playing, falling back to the
// generic playback state for stations that do not send metadata.
void set_radio_playing_text(const lyra::audio::Status &status)
{
    char track[lyra::audio::kMaxStreamTitle];
    lyra::audio::stream_title(track, sizeof(track));
    if (!status.paused && track[0]) {
        set_label_text_if_changed(s_radio_status_label, track);
        return;
    }
    set_label_text_if_changed(s_radio_status_label,
        tr(status.paused ? lyra::i18n::StringId::PlaybackPaused :
                           lyra::i18n::StringId::PlaybackPlaying));
}

void radio_status_poll_cb(lv_timer_t *timer)
{
    if (s_view != View::RadioPlaying || !s_radio_status_label) {
        lv_timer_delete(timer);
        if (s_radio_status_timer == timer) s_radio_status_timer = nullptr;
        s_radio_status_label = nullptr;
        return;
    }
    const lyra::audio::Status status = lyra::audio::status();
    if (status.last_error != ESP_OK) {
        set_label_text_if_changed(s_radio_status_label,
            tr(lyra::i18n::StringId::RadioStreamFailed));
    } else if (!status.playing || status.eof) {
        set_label_text_if_changed(s_radio_status_label,
            tr(lyra::i18n::StringId::PlaybackIdle));
    } else if (status.sample_rate != 0) {
        set_radio_playing_text(status);
    } else {
        char connecting[lyra::media::kMaxName + 32];
        format_text(lyra::i18n::StringId::ConnectingTo, s_current_station_name,
                    connecting, sizeof(connecting));
        set_label_text_if_changed(s_radio_status_label, connecting);
    }
}

void radio_pause_cb(lv_event_t *)
{
    const lyra::audio::Status status = lyra::audio::status();
    if (!status.playing || status.eof || status.last_error != ESP_OK) {
        s_audio_eof_seen = false;
        s_pending_track_advance = false;
        s_pending_track_advance_us = 0;
        lyra::audio::set_transition_gain(100);
        lyra::audio::play_stream(status.path);
    } else {
        lyra::audio::toggle_pause();
    }
    render(View::RadioPlaying);
}

void radio_stop_cb(lv_event_t *)
{
    lyra::audio::stop();
    render(View::InternetRadio);
}

void add_radio_station_row(lv_obj_t *body, int y, size_t index,
                           const lyra::radio::Station &station,
                           bool is_current)
{
    lv_obj_t *row = make_button(body, 7, y, 306, 56,
        is_current ? kAccentSurface : kSurface, 5, true);
    lv_obj_t *icon = make_label(row, is_current ? LV_SYMBOL_VOLUME_MID : LV_SYMBOL_AUDIO,
                               is_current ? kAccent : kTextSecondary);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_t *title = make_label(row, station.name, kTextPrimary);
    make_marquee(title, 218);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 38, -10);
    lv_obj_t *address = make_label(row, station.url, kTextSecondary);
    make_marquee(address, 218);
    lv_obj_align(address, LV_ALIGN_LEFT_MID, 38, 12);
    lv_obj_add_event_cb(row, radio_station_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(index + 1));

    lv_obj_t *remove = make_button(row, 266, 12, 32, 32, kSurfaceRaised, 5);
    lv_obj_t *remove_icon = make_label(remove, LV_SYMBOL_TRASH, kTextSecondary);
    lv_obj_center(remove_icon);
    lv_obj_add_event_cb(remove, radio_remove_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(index + 1));
}

void add_radio_browser_row(lv_obj_t *body, int y,
                           const lyra::radio::BrowserEntry &entry)
{
    auto *saved_entry = static_cast<lyra::radio::BrowserEntry *>(
        lv_malloc(sizeof(lyra::radio::BrowserEntry)));
    if (!saved_entry) return;
    *saved_entry = entry;
    lv_obj_t *row = make_button(body, 7, y, 306, 48, kSurface, 5, true);
    lv_obj_t *icon = make_label(row,
        entry.directory ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_LIST, kAccent);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_t *name = make_label(row, entry.name, kTextPrimary);
    make_marquee(name, 260);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 42, 0);
    lv_obj_add_event_cb(row, radio_browser_entry_cb, LV_EVENT_CLICKED, saved_entry);
    lv_obj_add_event_cb(row, [](lv_event_t *delete_event) {
        if (lv_event_get_code(delete_event) == LV_EVENT_DELETE) {
            lv_free(lv_event_get_user_data(delete_event));
        }
    }, LV_EVENT_DELETE, saved_entry);
}

} // namespace

void render_radio_stations()
{
    lv_obj_t *header = make_header(tr(lyra::i18n::StringId::InternetRadio),
                                   View::Library, true);
    lv_obj_t *add = make_button(header, 270, 4, 42, 36, kAccentDark, 6);
    lv_obj_t *add_icon = make_label(add, LV_SYMBOL_PLUS, kTextOnAccent);
    lv_obj_center(add_icon);
    lv_obj_add_event_cb(add, open_radio_add_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *body = make_scroll_body(72);
    if (!lyra::radio::init() || lyra::radio::station_count() == 0) {
        lv_obj_t *empty = make_label(body, tr(lyra::i18n::StringId::NoRadioStations),
                                     kTextMuted);
        lv_obj_set_width(empty, 292);
        lv_label_set_long_mode(empty, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_pos(empty, 14, 18);
        return;
    }
    const lyra::audio::Status status = lyra::audio::status();
    const size_t count = lyra::radio::station_count();
    for (size_t index = 0; index < count; ++index) {
        lyra::radio::Station station{};
        if (!lyra::radio::station_at(index, &station)) continue;
        add_radio_station_row(body, static_cast<int>(index) * 60, index, station,
            status.playing && std::strcmp(status.path, station.url) == 0);
    }
}

void render_radio_add()
{
    prepare_radio_add_keyboard();
    make_header(tr(lyra::i18n::StringId::AddRadioStation), View::InternetRadio, true);
    lv_obj_t *body = make_box(s_screen, 0, 72, kScreenWidth,
                              content_height(72), kBackground);
    lv_obj_t *input = make_box(body, 8, 8, 240, 44, kSurfaceRaised, 7);
    s_search_label = make_label(input, "", kTextMuted);
    make_marquee(s_search_label, 214);
    lv_obj_align(s_search_label, LV_ALIGN_LEFT_MID, 12, 0);
    update_search_label();

    lv_obj_t *keyboard_button = make_button(body, 255, 8, 56, 44,
                                             kSurfaceRaised, 7);
    lv_obj_t *keyboard_icon = make_label(keyboard_button, LV_SYMBOL_KEYBOARD, kAccent);
    lv_obj_center(keyboard_icon);
    lv_obj_add_event_cb(keyboard_button, keyboard_selector_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *save = make_button(body, 8, 61, 94, 40, kAccentDark, 6);
    lv_obj_t *save_label = make_label(save, tr(lyra::i18n::StringId::SaveKey), kTextOnAccent);
    lv_obj_center(save_label);
    lv_obj_add_event_cb(save, [](lv_event_t *) { save_radio_address(); },
                        LV_EVENT_CLICKED, nullptr);
    lv_obj_t *import = make_button(body, 108, 61, 204, 40, kSurfaceRaised, 6);
    lv_obj_t *import_label = make_label(import,
        tr(lyra::i18n::StringId::ImportRadioPlaylist), kTextPrimary);
    lv_obj_set_width(import_label, 192);
    lv_label_set_long_mode(import_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_center(import_label);
    lv_obj_add_event_cb(import, open_radio_files_cb, LV_EVENT_CLICKED, nullptr);

    const int keyboard_height = library_keyboard_height();
    const int keyboard_y = content_height(72) - keyboard_height;
    lv_obj_t *keyboard = make_box(body, 0, keyboard_y, kScreenWidth,
                                  keyboard_height, kKeyboardSurface);
    make_library_keyboard(keyboard, tr(lyra::i18n::StringId::SaveKey));
}

void save_radio_address()
{
    commit_keyboard_preedit();
    if (lyra::radio::add_station(s_radio_url) == ESP_OK) {
        s_radio_url[0] = '\0';
        render(View::InternetRadio);
        return;
    }
    show_notice(tr(lyra::i18n::StringId::AddRadioStation),
                tr(lyra::i18n::StringId::RadioSaveFailed));
}

void render_radio_files()
{
    make_header(tr(lyra::i18n::StringId::ChooseRadioPlaylist), View::RadioAdd,
                true, nullptr, radio_directory_back_cb);
    lv_obj_t *body = make_scroll_body(72);
    lv_obj_t *path = make_label(body, s_radio_file_path, kTextMuted);
    make_marquee(path, 292);
    lv_obj_set_pos(path, 12, 2);

    const size_t page_size = kRadioFilePageSize;
    const size_t offset = s_list_page * page_size;
    lyra::radio::BrowserEntry entries[kRadioFilePageSize]{};
    size_t total = 0;
    const size_t shown = lyra::radio::browse_directory(s_radio_file_path, offset,
        entries, page_size, &total);
    if (shown == 0) {
        lv_obj_t *empty = make_label(body,
            tr(lyra::i18n::StringId::FolderNoSupportedFiles), kTextMuted);
        lv_obj_set_pos(empty, 12, 34);
    }
    for (size_t i = 0; i < shown; ++i) {
        add_radio_browser_row(body, 28 + static_cast<int>(i) * 52, entries[i]);
    }
    make_page_controls(body, 28 + static_cast<int>(shown) * 52 + 4,
                       total, page_size);
}

void render_radio_playing()
{
    stop_radio_status_timer();
    const lyra::audio::Status status = lyra::audio::status();
    if (!s_current_station_name[0]) {
        for (size_t index = 0; index < lyra::radio::station_count(); ++index) {
            lyra::radio::Station station{};
            if (lyra::radio::station_at(index, &station) &&
                std::strcmp(status.path, station.url) == 0) {
                copy_ui_text(s_current_station_name, sizeof(s_current_station_name),
                             station.name);
                break;
            }
        }
    }
    if (!s_current_station_name[0]) {
        copy_ui_text(s_current_station_name, sizeof(s_current_station_name), status.path);
    }
    make_header(s_current_station_name[0] ? s_current_station_name :
                tr(lyra::i18n::StringId::InternetRadio), View::InternetRadio, true);
    lv_obj_t *body = make_box(s_screen, 0, 72, kScreenWidth,
                              content_height(72), kBackground);
    lv_obj_t *art = make_box(body, 80, 24, 160, 160, kAccentSurface, 16);
    lv_obj_t *icon = make_label(art, LV_SYMBOL_WIFI, kAccent);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_28, 0);
    lv_obj_center(icon);

    lv_obj_t *title = make_label(body, s_current_station_name, kTextPrimary);
    lv_obj_set_width(title, 292);
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 202);
    s_radio_status_label = make_label(body, "", kTextSecondary);
    lv_obj_set_width(s_radio_status_label, 292);
    lv_label_set_long_mode(s_radio_status_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(s_radio_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_radio_status_label, LV_ALIGN_TOP_MID, 0, 231);

    if (status.last_error != ESP_OK) {
        set_label_text_if_changed(s_radio_status_label,
            tr(lyra::i18n::StringId::RadioStreamFailed));
    } else if (status.sample_rate != 0) {
        set_radio_playing_text(status);
    } else {
        char connecting[lyra::media::kMaxName + 32];
        format_text(lyra::i18n::StringId::ConnectingTo, s_current_station_name,
                    connecting, sizeof(connecting));
        set_label_text_if_changed(s_radio_status_label, connecting);
    }

    lv_obj_t *pause = make_button(body, 104, 274, 56, 56, kAccentDark,
                                  LV_RADIUS_CIRCLE);
    lv_obj_t *pause_icon = make_label(pause,
        status.playing && !status.paused && !status.eof && status.last_error == ESP_OK ?
            LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY,
        kTextOnAccent);
    lv_obj_set_style_text_font(pause_icon, &lv_font_montserrat_28, 0);
    lv_obj_center(pause_icon);
    lv_obj_add_event_cb(pause, radio_pause_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *stop = make_button(body, 174, 280, 44, 44, kSurfaceRaised,
                                 LV_RADIUS_CIRCLE);
    lv_obj_t *stop_icon = make_label(stop, LV_SYMBOL_STOP, kTextPrimary);
    lv_obj_center(stop_icon);
    lv_obj_add_event_cb(stop, radio_stop_cb, LV_EVENT_CLICKED, nullptr);

    s_radio_status_timer = lv_timer_create(radio_status_poll_cb, 500, nullptr);
}

void stop_radio_status_timer()
{
    if (s_radio_status_timer) {
        lv_timer_delete(s_radio_status_timer);
        s_radio_status_timer = nullptr;
    }
    s_radio_status_label = nullptr;
}

void leave_radio_add()
{
    if (!s_radio_keyboard_layout_overridden) return;
    s_library_keyboard_layout = s_radio_previous_keyboard_layout;
    s_library_keyboard_symbols = s_radio_previous_keyboard_symbols;
    s_library_keyboard_accents = s_radio_previous_keyboard_accents;
    s_radio_keyboard_layout_overridden = false;
}

} // namespace lyra::gui::internal
