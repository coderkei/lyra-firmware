/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../lyra_gui_internal.h"

namespace lyra::gui::internal {

void play_track_from_row(size_t track_index, bool force_single)
{
    discard_pending_saved_queue();
    s_has_active_queue = true;
    if (force_single) {
        s_playback_scope = PlaybackScope::Single;
    } else if (s_view == View::LibrarySongs) {
        s_playback_scope = PlaybackScope::AllSongs;
    } else if (s_view == View::AlbumDetail || s_view == View::TrackList ||
               (s_view == View::ArtistDetail && s_artist_detail_tab == ArtistDetailTab::Songs)) {
        s_playback_scope = PlaybackScope::Group;
        s_playback_group_kind = s_view == View::AlbumDetail ?
            lyra::media::GroupKind::Album : s_selected_group_kind;
        s_playback_group = s_selected_group;
    } else if (s_view == View::PlaylistDetail) {
        if (s_selected_playlist_is_smart) {
            s_playback_scope = PlaybackScope::SmartPlaylist;
            s_playback_smart_playlist = s_selected_smart_playlist;
        } else {
            s_playback_scope = PlaybackScope::Playlist;
            s_playback_playlist = s_selected_playlist;
        }
    } else if (s_view == View::Folders || s_view == View::FolderDetail) {
        s_playback_scope = PlaybackScope::Folder;
        copy_ui_text(s_playback_folder_path, sizeof(s_playback_folder_path), s_folder_path);
    } else if (s_view == View::Search) {
        s_playback_scope = PlaybackScope::Search;
    } else {
        s_playback_scope = PlaybackScope::Single;
    }
    s_current_track = track_index;
    reset_shuffle_queue();
    s_queue_position_valid = false;
    current_queue_position(&s_queue_position);
    s_audio_eof_seen = false;
    lyra::media::Track track{};
    if (lyra::media::track_at(s_current_track, &track)) {
        const esp_err_t audio_ret = start_track_audio(track);
        if (audio_ret != ESP_OK) {
            ESP_LOGW(kTag, "cannot start %s: %s", track.path, esp_err_to_name(audio_ret));
        }
    }
    navigate_to(View::Player);
}

void track_route_cb(lv_event_t *event)
{
    play_track_from_row(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)), false);
}

void single_track_route_cb(lv_event_t *event)
{
    play_track_from_row(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)), true);
}

void add_playlist_track_cb(lv_event_t *event);

void add_track_route(lv_obj_t *row, size_t track_index, bool force_single)
{
    lv_obj_add_event_cb(row, force_single ? single_track_route_cb : track_route_cb,
                        LV_EVENT_CLICKED, reinterpret_cast<void *>(track_index));
}

void make_song_row(lv_obj_t *parent, int y, size_t track_index, int height,
                   bool force_single, bool show_play_count)
{
    lyra::media::Track track{};
    if (!lyra::media::track_at(track_index, &track)) return;
    lv_obj_t *row = make_button(parent, 7, y, 306, height, kSurface, 5, true);
    lv_obj_t *title = make_label(row, track.title, kTextPrimary);
    make_marquee(title, s_playlist_add_mode ? 244 : 282);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 12, -9);
    char subtitle[lyra::media::kMaxName + 32];
    if (show_play_count) {
        format_text_u32(lyra::i18n::StringId::ArtistPlayCount, track.artist,
                        lyra::media::track_play_count(track_index), subtitle,
                        sizeof(subtitle));
    } else {
        copy_ui_text(subtitle, sizeof(subtitle), track.artist);
    }
    lv_obj_t *artist = make_label(row, subtitle, kTextSecondary);
    make_marquee(artist, s_playlist_add_mode ? 244 : 282);
    lv_obj_align(artist, LV_ALIGN_LEFT_MID, 12, 11);
    if (s_playlist_add_mode) {
        lv_obj_t *add = make_label(row, LV_SYMBOL_PLUS, kAccent);
        lv_obj_align(add, LV_ALIGN_RIGHT_MID, -12, 0);
        lv_obj_clear_flag(add, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, add_playlist_track_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(track_index));
    } else {
        add_track_route(row, track_index, force_single);
    }
}

void add_search_playlist_route(lv_obj_t *row, size_t track_index, size_t playlist_index)
{
    // kMaxTracks is the result-buffer ceiling, so this compact payload fits
    // safely on the 32-bit ESP32 event user-data field.
    const uintptr_t payload = playlist_index * lyra::media::kMaxTracks + track_index;
    lv_obj_add_event_cb(row, [](lv_event_t *event) {
        const uintptr_t payload = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
        const size_t playlist_index = payload / lyra::media::kMaxTracks;
        const size_t track_index = payload % lyra::media::kMaxTracks;
        if (playlist_index >= lyra::media::playlist_count()) return;
        discard_pending_saved_queue();
        s_has_active_queue = true;
        s_playback_scope = PlaybackScope::Playlist;
        s_playback_playlist = playlist_index;
        s_current_track = track_index;
        reset_shuffle_queue();
        s_queue_position_valid = false;
        current_queue_position(&s_queue_position);
        s_audio_eof_seen = false;
        lyra::media::Track track{};
        if (lyra::media::track_at(s_current_track, &track)) {
            const esp_err_t audio_ret = start_track_audio(track);
            if (audio_ret != ESP_OK) {
                ESP_LOGW(kTag, "cannot start %s: %s", track.path, esp_err_to_name(audio_ret));
            }
        }
        navigate_to(View::Player);
    }, LV_EVENT_CLICKED, reinterpret_cast<void *>(payload));
}

void make_search_playlist_row(lv_obj_t *parent, int y,
                              const lyra::media::SearchResult &result)
{
    lyra::media::Track track{};
    lyra::media::Playlist playlist{};
    if (!lyra::media::track_at(result.track_index, &track) ||
        !lyra::media::playlist_at(result.playlist_index, &playlist)) return;
    lv_obj_t *row = make_button(parent, 7, y, 306, 54, kSurface, 5, true);
    lv_obj_t *title = make_label(row, track.title, kTextPrimary);
    make_marquee(title, 282);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 12, -9);
    lv_obj_t *source = make_label(row, playlist.name, kTextSecondary);
    make_marquee(source, 282);
    lv_obj_align(source, LV_ALIGN_LEFT_MID, 12, 11);
    add_search_playlist_route(row, result.track_index, result.playlist_index);
}

void search_album_result_cb(lv_event_t *event)
{
    const size_t group_index = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    lyra::media::Group group{};
    if (!lyra::media::group_at(lyra::media::GroupKind::Album, group_index, &group)) return;
    s_selected_group_kind = lyra::media::GroupKind::Album;
    s_selected_group = group_index;
    s_library_tab = LibraryTab::Albums;
    copy_ui_text(s_track_list_title, sizeof(s_track_list_title), group.name);
    navigate_to(View::AlbumDetail);
}

void search_artist_result_cb(lv_event_t *event)
{
    const size_t group_index = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    lyra::media::Group group{};
    if (!lyra::media::group_at(lyra::media::GroupKind::Artist, group_index, &group)) return;
    s_selected_group_kind = lyra::media::GroupKind::Artist;
    s_selected_group = group_index;
    s_library_tab = LibraryTab::Artists;
    s_artist_detail_tab = ArtistDetailTab::Songs;
    copy_ui_text(s_track_list_title, sizeof(s_track_list_title), group.name);
    navigate_to(View::ArtistDetail);
}

void make_file_row(lv_obj_t *parent, int y, size_t track_index, int height)
{
    lyra::media::Track track{};
    if (!lyra::media::track_at(track_index, &track)) return;
    const char *filename = std::strrchr(track.path, '/');
    filename = filename ? filename + 1 : track.path;
    lv_obj_t *row = make_button(parent, 7, y, 306, height, kSurface, 5, true);
    lv_obj_t *icon = make_label(row, LV_SYMBOL_FILE, kAccent);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_t *name = make_label(row, filename, kTextPrimary);
    make_marquee(name, 251);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 43, -9);
    char size[24];
    if (track.size_bytes < 1024u * 1024u) {
        format_float(lyra::i18n::StringId::SizeKilobytesWhole,
                     track.size_bytes / 1024.0, size, sizeof(size));
    } else {
        format_float(lyra::i18n::StringId::SizeMegabytes,
                     track.size_bytes / (1024.0 * 1024.0), size, sizeof(size));
    }
    lv_obj_t *size_label = make_label(row, size, kTextSecondary);
    lv_obj_align(size_label, LV_ALIGN_LEFT_MID, 43, 11);
    add_track_route(row, track_index);
}

void make_album_art(lv_obj_t *parent, int x, int y, int width, int height,
                    const lyra::media::Track &track, bool preserve_aspect)
{
    make_artwork(parent, x, y, width, height, track, 13, preserve_aspect);
}

void page_cb(lv_event_t *event)
{
    const uintptr_t action = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    if (action == 0 && s_list_page > 0) --s_list_page;
    else if (action == 1 && s_list_page + 1 < s_list_page_count) ++s_list_page;
    else if (action == 2) s_list_page = 0;
    else if (action == 3 && s_list_page_count) s_list_page = s_list_page_count - 1;
    render(s_view);
}

void make_page_controls(lv_obj_t *parent, int y, size_t total,
                        size_t page_size)
{
    const size_t pages = (total + page_size - 1) / page_size;
    if (pages <= 1) return;
    s_list_page_count = pages;
    lv_obj_t *first = make_button(parent, 7, y, 48, 42, kSurfaceRaised, 6);
    lv_obj_t *first_label = make_label(first, LV_SYMBOL_PREV, s_list_page ? kTextPrimary : kTextMuted);
    lv_obj_center(first_label);
    if (s_list_page) lv_obj_add_event_cb(first, page_cb, LV_EVENT_CLICKED, reinterpret_cast<void *>(2));

    lv_obj_t *previous = make_button(parent, 59, y, 48, 42, kSurfaceRaised, 6);
    lv_obj_t *previous_label = make_label(previous, LV_SYMBOL_LEFT, s_list_page ? kTextPrimary : kTextMuted);
    lv_obj_center(previous_label);
    if (s_list_page) lv_obj_add_event_cb(previous, page_cb, LV_EVENT_CLICKED, nullptr);

    char page_text[32];
    std::snprintf(page_text, sizeof(page_text), "%u / %u",
                  static_cast<unsigned>(s_list_page + 1), static_cast<unsigned>(pages));
    lv_obj_t *page = make_label(parent, page_text, kTextSecondary);
    lv_obj_align(page, LV_ALIGN_TOP_MID, 0, y + 13);

    lv_obj_t *next = make_button(parent, 213, y, 48, 42, kSurfaceRaised, 6);
    lv_obj_t *next_label = make_label(next, LV_SYMBOL_RIGHT,
                                      s_list_page + 1 < pages ? kTextPrimary : kTextMuted);
    lv_obj_center(next_label);
    if (s_list_page + 1 < pages) {
        lv_obj_add_event_cb(next, page_cb, LV_EVENT_CLICKED, reinterpret_cast<void *>(1));
    }
    lv_obj_t *last = make_button(parent, 265, y, 48, 42, kSurfaceRaised, 6);
    lv_obj_t *last_label = make_label(last, LV_SYMBOL_NEXT,
                                      s_list_page + 1 < pages ? kTextPrimary : kTextMuted);
    lv_obj_center(last_label);
    if (s_list_page + 1 < pages) {
        lv_obj_add_event_cb(last, page_cb, LV_EVENT_CLICKED, reinterpret_cast<void *>(3));
    }
}

} // namespace lyra::gui::internal
