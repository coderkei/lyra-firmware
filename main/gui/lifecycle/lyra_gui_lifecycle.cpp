/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../lyra_gui_internal.h"

namespace lyra::gui::internal {

void render(View view)
{
    lyra::font::init();
    s_list_scrolling = false;
    s_view = view;
    s_search_label = nullptr;
    s_search_results = nullptr;
    s_search_keyboard = nullptr;
    s_search_keyboard_toggle = nullptr;
    s_search_progress_label = nullptr;
    s_scan_overlay = nullptr;
    s_scan_count_label = nullptr;
    s_scan_phase_label = nullptr;
    s_sort_overlay = nullptr;
    s_sort_progress_label = nullptr;
    style_root();
    if (view != View::FullscreenInfoArt) make_status_bar();
    switch (view) {
        case View::Menu: render_menu(); break;
        case View::Player: render_player(false); break;
        case View::FullscreenArt: render_player(true); break;
        case View::TrackInfo: render_track_info(); break;
        case View::FullscreenInfoArt: render_fullscreen_info_art(); break;
        case View::Queue: render_queue(); break;
        case View::Library: render_library(); break;
        case View::LibrarySongs: render_library_section(LibraryTab::Songs); break;
        case View::LibraryArtists: render_library_section(LibraryTab::Artists); break;
        case View::LibraryAlbums: render_library_section(LibraryTab::Albums); break;
        case View::LibraryGenres: render_library_section(LibraryTab::Genres); break;
        case View::LibraryYears: render_library_section(LibraryTab::Years); break;
        case View::AlbumDetail: render_album_detail(); break;
        case View::ArtistDetail: render_artist_detail(); break;
        case View::TrackList: render_track_list(); break;
        case View::Folders: render_folders(false); break;
        case View::FolderDetail: render_folders(true); break;
        case View::Playlists: render_playlists(false); break;
        case View::PlaylistDetail: render_playlists(true); break;
        case View::PlaylistCreate: render_playlist_create(); break;
        case View::PlaylistAdd: render_playlist_add(); break;
        case View::Equalizer: render_equalizer(); break;
        case View::EqualizerPresets: render_equalizer_presets(); break;
        case View::Search: render_search(); break;
        case View::Settings: render_settings_menu(); break;
        case View::SortingSettings: render_sorting_settings(); break;
        case View::SortingOptions: render_sorting_options(); break;
        case View::PlaybackSettings:
        case View::SoundSettings:
        case View::DisplaySettings:
        case View::SystemSettings: render_settings_page(view); break;
        case View::CrossfadeOptions: render_crossfade_options(); break;
        case View::SleepTimerOptions: render_sleep_timer_options(); break;
        case View::DatabaseStorage: render_database_storage(); break;
        case View::About: render_about(); break;
        case View::DebugMenu: render_debug_menu(); break;
        case View::Licenses: render_licenses(); break;
    }
    if (s_show_nav && view != View::FullscreenInfoArt) make_virtual_nav();
}

bool automatic_track_advance_available()
{
    if (s_repeat_mode == RepeatMode::Song || s_repeat_mode == RepeatMode::All) return true;
    const size_t count = playback_queue_count();
    if (count == 0) return false;
    if (s_shuffle) return s_shuffle_cursor + 1 < count;
    size_t position = 0;
    return current_queue_position(&position) && position + 1 < count;
}

void begin_crossfade_fade_in()
{
    if (s_crossfade_seconds == 0) return;
    s_crossfade_fade_in_started_us = esp_timer_get_time();
    s_crossfade_fade_in_ends_us = s_crossfade_fade_in_started_us +
        static_cast<int64_t>(s_crossfade_seconds) * 1000 * 1000;
    lyra::audio::set_transition_gain(0);
}

bool advance_after_track_end()
{
    const bool advanced = s_repeat_mode == RepeatMode::Song ? restart_current_track() :
                          move_in_playback_queue(1, true);
    if (advanced) begin_crossfade_fade_in();
    return advanced;
}

void update_crossfade_gain(const lyra::audio::Status &audio_status, int64_t now_us)
{
    if (s_crossfade_fade_in_ends_us != 0) {
        if (now_us >= s_crossfade_fade_in_ends_us) {
            s_crossfade_fade_in_started_us = 0;
            s_crossfade_fade_in_ends_us = 0;
            lyra::audio::set_transition_gain(100);
        } else {
            const int64_t duration_us = s_crossfade_fade_in_ends_us - s_crossfade_fade_in_started_us;
            const int64_t elapsed_us = now_us - s_crossfade_fade_in_started_us;
            const uint8_t gain = static_cast<uint8_t>(std::clamp<int64_t>(
                (elapsed_us * 100) / duration_us, 0, 100));
            lyra::audio::set_transition_gain(gain);
        }
        return;
    }
    if (s_crossfade_seconds == 0 || !automatic_track_advance_available() ||
        audio_status.duration_ms == 0 || audio_status.position_ms >= audio_status.duration_ms) {
        lyra::audio::set_transition_gain(100);
        return;
    }
    const uint32_t crossfade_ms = static_cast<uint32_t>(s_crossfade_seconds) * 1000;
    const uint32_t remaining_ms = audio_status.duration_ms - audio_status.position_ms;
    const uint8_t gain = remaining_ms >= crossfade_ms ? 100 : static_cast<uint8_t>(
        (static_cast<uint64_t>(remaining_ms) * 100u) / crossfade_ms);
    lyra::audio::set_transition_gain(gain);
}

void crossfade_poll_cb(lv_timer_t *)
{
    if (s_crossfade_seconds == 0 && s_crossfade_fade_in_ends_us == 0 &&
        s_crossfade_transition_direction == 0 && !s_crossfade_pause_pending) return;
    const int64_t now_us = esp_timer_get_time();
    if (s_crossfade_transition_direction != 0 || s_crossfade_pause_pending) {
        if (now_us >= s_crossfade_fade_out_ends_us) {
            const int direction = s_crossfade_transition_direction;
            const bool pause_pending = s_crossfade_pause_pending;
            s_crossfade_transition_direction = 0;
            s_crossfade_pause_pending = false;
            s_crossfade_fade_out_started_us = 0;
            s_crossfade_fade_out_ends_us = 0;
            if (pause_pending) {
                lyra::audio::set_transition_gain(0);
                lyra::audio::toggle_pause();
            } else if (move_in_playback_queue(direction, false)) begin_crossfade_fade_in();
            else lyra::audio::set_transition_gain(100);
        } else {
            const int64_t duration_us = s_crossfade_fade_out_ends_us -
                s_crossfade_fade_out_started_us;
            const int64_t remaining_us = s_crossfade_fade_out_ends_us - now_us;
            const uint8_t gain = static_cast<uint8_t>(std::clamp<int64_t>(
                (remaining_us * 100) / duration_us, 0, 100));
            lyra::audio::set_transition_gain(gain);
        }
        return;
    }
    const lyra::audio::Status audio_status = lyra::audio::status();
    if (!audio_status.playing || audio_status.paused || audio_status.eof) return;
    update_crossfade_gain(audio_status, now_us);
}

void player_progress_poll_cb(lv_timer_t *)
{
    const lyra::audio::Status audio_status = lyra::audio::status();
    const int64_t now_us = esp_timer_get_time();
    if (s_sleep_timer_deadline_us != 0 && now_us >= s_sleep_timer_deadline_us) {
        s_sleep_timer_deadline_us = 0;
        s_sleep_timer_minutes = 0;
        s_pending_track_advance = false;
        s_pending_track_advance_us = 0;
        s_crossfade_fade_in_started_us = 0;
        s_crossfade_fade_in_ends_us = 0;
        s_crossfade_fade_out_started_us = 0;
        s_crossfade_fade_out_ends_us = 0;
        s_crossfade_transition_direction = 0;
        s_crossfade_pause_pending = false;
        lyra::audio::set_transition_gain(100);
        lyra::audio::stop();
        show_notice(tr(lyra::i18n::StringId::SleepTimer),
                    tr(lyra::i18n::StringId::PlaybackStopped));
    } else if (!audio_status.eof) {
        s_audio_eof_seen = false;
        s_pending_track_advance = false;
        s_pending_track_advance_us = 0;
    } else if (!s_audio_eof_seen) {
        s_audio_eof_seen = true;
        lyra::audio::set_transition_gain(100);
        if (automatic_track_advance_available()) {
            if (s_gapless) {
                advance_after_track_end();
            } else {
                s_pending_track_advance = true;
                s_pending_track_advance_us = now_us +
                    static_cast<int64_t>(kNonGaplessTrackPauseMs) * 1000;
            }
        }
    } else if (s_pending_track_advance && now_us >= s_pending_track_advance_us) {
        s_pending_track_advance = false;
        s_pending_track_advance_us = 0;
        advance_after_track_end();
    }
    update_player_progress();
}

void show_sorting_overlay(const lyra::media::Status &status)
{
    if (s_sort_overlay || !s_screen) return;
    s_sort_overlay = make_box(s_screen, 0, 0, kScreenWidth, kScreenHeight,
                              kOverlay);
    lv_obj_set_style_bg_opa(s_sort_overlay, LV_OPA_80, 0);
    lv_obj_add_flag(s_sort_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(s_sort_overlay);
    lv_obj_t *dialog = make_box(s_sort_overlay, 24, 126, 272, 228,
                                kSurfaceRaised, 14);
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
    lv_obj_t *title = make_label(dialog, tr(lyra::i18n::StringId::PreparingLibrarySort), kTextPrimary);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 105);
    s_sort_progress_label = make_label(dialog, tr(lyra::i18n::StringId::BuildingSortCache), kAccent);
    lv_obj_align(s_sort_progress_label, LV_ALIGN_TOP_MID, 0, 137);
    lv_obj_t *section = make_label(dialog,
        status.sorting_section == static_cast<uint8_t>(lyra::media::SortSection::Songs) ?
            tr(lyra::i18n::StringId::Songs) : status.sorting_section == static_cast<uint8_t>(lyra::media::SortSection::Albums) ?
            tr(lyra::i18n::StringId::Albums) : tr(lyra::i18n::StringId::Artists), kTextSecondary);
    lv_obj_align(section, LV_ALIGN_TOP_MID, 0, 169);
}

void update_sorting_overlay(const lyra::media::Status &status)
{
    if (!s_sort_progress_label) return;
    char progress[48];
    format_u32_u32(lyra::i18n::StringId::PreparingProgress,
                   static_cast<uint32_t>(status.sorting_indexed),
                   static_cast<uint32_t>(status.sorting_total), progress, sizeof(progress));
    lv_label_set_text(s_sort_progress_label, progress);
}

void catalog_poll_cb(lv_timer_t *)
{
    const lyra::media::Status status = lyra::media::status();
    const lyra::media::SearchStatus search = lyra::media::search_status();
    const bool changed = status.catalog_generation != s_seen_catalog_generation ||
                         status.scanning != s_seen_scanning;
    const bool artwork_changed = status.artwork_generation != s_seen_artwork_generation;
    const bool duration_changed = status.duration_generation != s_seen_duration_generation;
    const bool sorting_changed = status.sorting_generation != s_seen_sorting_generation;
    s_seen_catalog_generation = status.catalog_generation;
    s_seen_scanning = status.scanning;
    s_seen_artwork_generation = status.artwork_generation;
    s_seen_duration_generation = status.duration_generation;
    s_seen_sorting_generation = status.sorting_generation;
    if (s_view == View::Search) {
        if (search.running && s_search_progress_label) {
            char progress[48];
            format_u32_u32(lyra::i18n::StringId::SearchingProgress,
                           static_cast<uint32_t>(search.processed),
                           static_cast<uint32_t>(search.total), progress, sizeof(progress));
            lv_label_set_text(s_search_progress_label, progress);
        }
        if (search.generation != s_seen_search_generation) {
            s_seen_search_generation = search.generation;
            if (!search.running) {
                if (search.result != ESP_OK) copy_ui_text(
                    s_search_error, sizeof(s_search_error),
                    tr(lyra::i18n::StringId::SearchCouldNotComplete));
                render(View::Search);
                return;
            }
        }
    } else {
        s_seen_search_generation = search.generation;
    }
    if (s_scan_overlay) {
        char count[48];
        format_count(lyra::i18n::StringId::ScanSongsFound,
                     static_cast<uint32_t>(status.scan_found), count, sizeof(count));
        if (s_scan_count_label) lv_label_set_text(s_scan_count_label, count);
        if (s_scan_phase_label) {
            lv_label_set_text(s_scan_phase_label,
                              status.scan_indexing ? tr(lyra::i18n::StringId::BuildingLibraryIndexes) :
                              tr(lyra::i18n::StringId::ReadingMicroSdFolders));
        }
        if (!status.scanning) render(View::SystemSettings);
        return;
    }
    if (status.sorting_indexing) {
        show_sorting_overlay(status);
        update_sorting_overlay(status);
        return;
    }
    if (s_sort_overlay) {
        lv_obj_delete(s_sort_overlay);
        s_sort_overlay = nullptr;
        s_sort_progress_label = nullptr;
    }
    if (changed && s_view == View::SystemSettings) render(View::SystemSettings);
    if (sorting_changed &&
        (s_view == View::LibrarySongs || s_view == View::LibraryAlbums ||
         s_view == View::LibraryArtists)) {
        render(s_view);
        return;
    }
    if (duration_changed && !status.duration_indexing &&
        (s_view == View::LibrarySongs || s_view == View::LibraryAlbums ||
         s_view == View::LibraryArtists)) {
        render(s_view);
        return;
    }
    if (artwork_changed) {
        if (s_view == View::Player || s_view == View::FullscreenArt ||
            s_view == View::TrackInfo || s_view == View::FullscreenInfoArt ||
            s_view == View::AlbumDetail) {
            render(s_view);
        }
        else if (s_screen) lv_obj_invalidate(s_screen);
    }
}

} // namespace lyra::gui::internal
