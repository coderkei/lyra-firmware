/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../lyra_gui_internal.h"

namespace lyra::gui::internal {

namespace {

enum class QueueAddSourceKind : uint8_t { Track, Album, Folder };

struct QueueAddSource {
    QueueAddSourceKind kind;
    size_t index;
    char folder_path[lyra::media::kMaxPath];
};

void queue_add_popup_event_cb(lv_event_t *event)
{
    auto *source = static_cast<QueueAddSource *>(lv_event_get_user_data(event));
    if (lv_event_get_code(event) == LV_EVENT_DELETE) {
        if (s_queue_add_popup == lv_event_get_current_target_obj(event)) {
            s_queue_add_popup = nullptr;
        }
        lv_free(source);
    } else if (lv_event_get_target_obj(event) == lv_event_get_current_target_obj(event)) {
        lv_obj_delete(lv_event_get_current_target_obj(event));
    }
}

bool collect_queue_add_tracks(const QueueAddSource &source, size_t *tracks,
                              size_t capacity, size_t *count)
{
    if (!tracks || !count || capacity == 0) return false;
    *count = 0;
    if (source.kind == QueueAddSourceKind::Track) {
        if (source.index >= lyra::media::track_count()) return false;
        tracks[(*count)++] = source.index;
        return true;
    }
    if (source.kind == QueueAddSourceKind::Album) {
        lyra::media::Group album{};
        if (!lyra::media::group_at(lyra::media::GroupKind::Album, source.index, &album) ||
            album.track_count > capacity) return false;
        *count = lyra::media::group_tracks(lyra::media::GroupKind::Album, source.index,
                                           0, tracks, std::min(capacity, album.track_count));
        return *count == album.track_count;
    }

    const size_t path_length = std::strlen(source.folder_path);
    if (path_length == 0) return false;
    const size_t catalog_count = lyra::media::track_count();
    for (size_t i = 0; i < catalog_count; ++i) {
        lyra::media::Track track{};
        if (!lyra::media::track_at(i, &track) ||
            std::strncmp(track.path, source.folder_path, path_length) != 0 ||
            track.path[path_length] != '/') continue;
        if (*count >= capacity) return false;
        tracks[(*count)++] = i;
    }
    return *count > 0;
}

void add_queue_source_cb(lv_event_t *event, QueueInsertMode mode)
{
    lv_event_stop_bubbling(event);
    auto *source = static_cast<QueueAddSource *>(lv_event_get_user_data(event));
    if (!source) return;
    auto *tracks = static_cast<size_t *>(heap_caps_malloc(
        lyra::media::kMaxTracks * sizeof(size_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!tracks) {
        tracks = static_cast<size_t *>(heap_caps_malloc(
            lyra::media::kMaxTracks * sizeof(size_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    size_t count = 0;
    const bool collected = tracks && collect_queue_add_tracks(
        *source, tracks, lyra::media::kMaxTracks, &count);
    const bool added = collected && add_tracks_to_queue(tracks, count, mode);
    heap_caps_free(tracks);
    if (s_queue_add_popup) lv_obj_delete(s_queue_add_popup);
    if (!added) show_notice(tr(lyra::i18n::StringId::Queue),
                            tr(lyra::i18n::StringId::QueueAddFailed));
}

void play_queue_source_next_cb(lv_event_t *event)
{
    add_queue_source_cb(event, QueueInsertMode::PlayNext);
}

void append_queue_source_cb(lv_event_t *event)
{
    add_queue_source_cb(event, QueueInsertMode::AddToEnd);
}

void show_queue_add_picker(QueueAddSourceKind kind, size_t index, const char *folder_path)
{
    if (s_queue_add_popup || !s_screen) return;
    auto *source = static_cast<QueueAddSource *>(lv_malloc_zeroed(sizeof(QueueAddSource)));
    if (!source) return;
    source->kind = kind;
    source->index = index;
    if (folder_path) copy_ui_text(source->folder_path, sizeof(source->folder_path), folder_path);

    s_queue_add_popup = make_box(s_screen, 0, 0, kScreenWidth, kScreenHeight, kOverlay);
    lv_obj_set_style_bg_opa(s_queue_add_popup, LV_OPA_90, 0);
    lv_obj_add_flag(s_queue_add_popup, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(s_queue_add_popup);
    lv_obj_add_event_cb(s_queue_add_popup, queue_add_popup_event_cb, LV_EVENT_CLICKED, source);
    lv_obj_add_event_cb(s_queue_add_popup, queue_add_popup_event_cb, LV_EVENT_DELETE, source);

    lv_obj_t *dialog = make_box(s_queue_add_popup, 20, 142, 280, 196, kSurfaceRaised, 12);
    lv_obj_t *title = make_label(dialog, tr(lyra::i18n::StringId::Queue), kTextPrimary);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 14, 12);
    lv_obj_t *close = make_button(dialog, 238, 6, 34, 34, kSurface, 17);
    lv_obj_t *close_icon = make_label(close, LV_SYMBOL_CLOSE, kTextSecondary);
    lv_obj_center(close_icon);
    lv_obj_add_event_cb(close, [](lv_event_t *event) {
        lv_event_stop_bubbling(event);
        if (s_queue_add_popup) lv_obj_delete(s_queue_add_popup);
    }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *next = make_button(dialog, 12, 52, 256, 54, kAccentDark, 7);
    lv_obj_t *next_label = make_label(next, tr(lyra::i18n::StringId::PlayNext), kTextOnAccent);
    lv_obj_center(next_label);
    lv_obj_add_event_cb(next, play_queue_source_next_cb, LV_EVENT_CLICKED, source);
    lv_obj_t *end = make_button(dialog, 12, 116, 256, 54, kSurface, 7, true);
    lv_obj_t *end_label = make_label(end, tr(lyra::i18n::StringId::AddToQueueEnd), kTextPrimary);
    lv_obj_center(end_label);
    lv_obj_add_event_cb(end, append_queue_source_cb, LV_EVENT_CLICKED, source);
}

void add_queue_source_button(lv_obj_t *row, QueueAddSourceKind kind, size_t index,
                             const char *folder_path = nullptr)
{
    QueueAddSource *source = nullptr;
    uintptr_t payload = 0;
    if (kind == QueueAddSourceKind::Folder) {
        source = static_cast<QueueAddSource *>(lv_malloc_zeroed(sizeof(QueueAddSource)));
        if (!source) return;
        source->kind = kind;
        source->index = index;
        if (folder_path) copy_ui_text(source->folder_path, sizeof(source->folder_path), folder_path);
    } else {
        payload = (static_cast<uintptr_t>(kind) << 16) | (index & 0xFFFFu);
    }
    lv_obj_t *button = make_button(row, 260, 9, 34, 36, kAccentDark, 5);
    lv_obj_t *icon = make_label(button, LV_SYMBOL_PLUS, kTextOnAccent);
    lv_obj_center(icon);
    if (kind == QueueAddSourceKind::Folder) {
        lv_obj_add_event_cb(button, [](lv_event_t *event) {
            lv_event_stop_bubbling(event);
            auto *source = static_cast<QueueAddSource *>(lv_event_get_user_data(event));
            if (source) show_queue_add_picker(source->kind, source->index, source->folder_path);
        }, LV_EVENT_CLICKED, source);
        lv_obj_add_event_cb(button, [](lv_event_t *event) {
            if (lv_event_get_code(event) == LV_EVENT_DELETE) {
                lv_free(lv_event_get_user_data(event));
            }
        }, LV_EVENT_DELETE, source);
    } else {
        lv_obj_add_event_cb(button, [](lv_event_t *event) {
            lv_event_stop_bubbling(event);
            const uintptr_t encoded = reinterpret_cast<uintptr_t>(
                lv_event_get_user_data(event));
            const auto source_kind = static_cast<QueueAddSourceKind>(encoded >> 16);
            const size_t source_index = encoded & 0xFFFFu;
            show_queue_add_picker(source_kind, source_index, nullptr);
        }, LV_EVENT_CLICKED, reinterpret_cast<void *>(payload));
    }
}

} // namespace

void show_queue_add_picker_for_track(size_t track_index)
{
    show_queue_add_picker(QueueAddSourceKind::Track, track_index, nullptr);
}

void show_queue_add_picker_for_album(size_t group_index)
{
    show_queue_add_picker(QueueAddSourceKind::Album, group_index, nullptr);
}

void show_queue_add_picker_for_folder(const char *folder_path)
{
    if (!folder_path) return;
    show_queue_add_picker(QueueAddSourceKind::Folder, 0, folder_path);
}

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
    const bool current = !is_standard_theme() && track_index == s_current_track &&
        std::strcmp(lyra::audio::status().path, track.path) == 0;
    lv_obj_t *row = make_button(parent, 7, y, 306, height,
                                current ? kAccentSurface : kSurface, 5, true);
    lv_obj_t *title = make_label(row, track.title, current ? kAccent : kTextPrimary);
    const int text_width = s_playlist_add_mode ? 244 : (s_queue_add_mode ? 236 : 282);
    make_marquee(title, text_width);
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
    make_marquee(artist, text_width);
    lv_obj_align(artist, LV_ALIGN_LEFT_MID, 12, 11);
    if (s_playlist_add_mode) {
        lv_obj_t *add = make_label(row, LV_SYMBOL_PLUS, kAccent);
        lv_obj_align(add, LV_ALIGN_RIGHT_MID, -12, 0);
        lv_obj_clear_flag(add, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, add_playlist_track_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(track_index));
    } else {
        add_track_route(row, track_index, force_single);
        if (s_queue_add_mode) {
            add_queue_source_button(row, QueueAddSourceKind::Track, track_index);
        }
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
    lv_obj_t *icon = make_label(row, LV_SYMBOL_FILE, s_neon_hud ? kTextSecondary : kAccent);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_t *name = make_label(row, filename, kTextPrimary);
    make_marquee(name, s_queue_add_mode ? 202 : 251);
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
    if (s_queue_add_mode) add_queue_source_button(row, QueueAddSourceKind::Track, track_index);
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

namespace {

struct PageJumpRequest {
    size_t total;
    size_t page_size;
    size_t page_count;
};

void page_jump_button_cb(lv_event_t *event)
{
    const auto *request = static_cast<const PageJumpRequest *>(lv_event_get_user_data(event));
    if (request) open_page_jump(request->total, request->page_size, request->page_count);
}

void release_page_jump_request_cb(lv_event_t *event)
{
    lv_free(lv_event_get_user_data(event));
}

} // namespace

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
    lv_obj_t *page_button = make_button(parent, 111, y, 98, 42, kBackground, 6);
    lv_obj_t *page = make_label(page_button, page_text, kTextSecondary);
    lv_obj_center(page);
    auto *request = static_cast<PageJumpRequest *>(lv_malloc(sizeof(PageJumpRequest)));
    if (request) {
        *request = {total, page_size, pages};
        lv_obj_add_event_cb(page_button, page_jump_button_cb, LV_EVENT_CLICKED, request);
        lv_obj_add_event_cb(page_button, release_page_jump_request_cb, LV_EVENT_DELETE,
                            request);
    }

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
