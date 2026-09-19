/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../lyra_gui_internal.h"

namespace lyra::gui::internal {

void open_track_list_cb(lv_event_t *event)
{
    const size_t selected_group = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    const lyra::media::GroupKind selected_kind = group_kind(s_library_tab);
    lyra::media::Group selected{};
    if (!lyra::media::group_at(selected_kind, selected_group, &selected)) return;
    push_navigation_state();
    s_selected_group = selected_group;
    s_selected_group_kind = selected_kind;
    copy_ui_text(s_track_list_title, sizeof(s_track_list_title), selected.name);
    s_list_page = 0;
    if (s_library_tab == LibraryTab::Albums) {
        render(View::AlbumDetail);
    } else if (s_library_tab == LibraryTab::Artists) {
        s_artist_detail_tab = ArtistDetailTab::Songs;
        render(View::ArtistDetail);
    } else {
        render(View::TrackList);
    }
}

void open_album_detail_cb(lv_event_t *event)
{
    const size_t selected_group = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    lyra::media::Group selected{};
    if (!lyra::media::group_at(lyra::media::GroupKind::Album, selected_group, &selected)) return;
    push_navigation_state();
    s_library_tab = LibraryTab::Albums;
    s_selected_group = selected_group;
    s_selected_group_kind = lyra::media::GroupKind::Album;
    copy_ui_text(s_track_list_title, sizeof(s_track_list_title), selected.name);
    s_list_page = 0;
    render(View::AlbumDetail);
}

void open_library_playlists_cb(lv_event_t *)
{
    push_navigation_state();
    s_playlists_from_library = true;
    s_playlist_manage_mode = false;
    s_list_page = 0;
    render(View::Playlists);
}

void render_library()
{
    const bool adding_to_playlist = s_playlist_add_mode;
    char add_title[lyra::media::kMaxName + 12];
    copy_ui_text(add_title, sizeof(add_title), tr(lyra::i18n::StringId::AddSongs));
    if (adding_to_playlist) {
        lyra::media::Playlist playlist{};
        if (lyra::media::playlist_at(s_selected_playlist, &playlist)) {
            format_text(lyra::i18n::StringId::AddToPlaylist, playlist.name,
                        add_title, sizeof(add_title));
        }
    }
    make_header(adding_to_playlist ? add_title : tr(lyra::i18n::StringId::MusicLibrary),
                adding_to_playlist ? View::PlaylistDetail : View::Menu, true);
    lv_obj_t *body = make_scroll_body(72);
    make_row(body, 0, nullptr, tr(lyra::i18n::StringId::Songs),
             tr(lyra::i18n::StringId::BrowseEverySong), View::LibrarySongs, 54);
    make_row(body, 58, nullptr, tr(lyra::i18n::StringId::Albums),
             tr(lyra::i18n::StringId::ArtworkAndAlbumSummaries), View::LibraryAlbums, 54);
    make_row(body, 116, nullptr, tr(lyra::i18n::StringId::Artists),
             tr(lyra::i18n::StringId::BrowseByArtist), View::LibraryArtists, 54);
    make_row(body, 174, nullptr, tr(lyra::i18n::StringId::Genres),
             tr(lyra::i18n::StringId::BrowseByGenre), View::LibraryGenres, 54);
    make_row(body, 232, nullptr, tr(lyra::i18n::StringId::Year),
             tr(lyra::i18n::StringId::BrowseByReleaseYear), View::LibraryYears, 54);
    if (adding_to_playlist) {
        lv_obj_t *hint = make_label(body, tr(lyra::i18n::StringId::SelectSongToAdd), kTextMuted);
        lv_obj_set_pos(hint, 12, 300);
        return;
    }
    lv_obj_t *playlists = make_row(body, 290, nullptr, tr(lyra::i18n::StringId::Playlists),
                                   tr(lyra::i18n::StringId::YourSavedPlaylists),
                                   View::Playlists, 54);
    lv_obj_remove_event_cb(playlists, route_cb);
    lv_obj_add_event_cb(playlists, open_library_playlists_cb, LV_EVENT_CLICKED, nullptr);
}

void make_album_row(lv_obj_t *parent, int y, size_t group_index)
{
    lyra::media::Group group{};
    if (!lyra::media::group_at(lyra::media::GroupKind::Album, group_index, &group)) return;
    // The catalog representative supplies the artist subtitle. Album rows are
    // intentionally text-only; artwork is reserved for overview and player.
    const size_t representative = group.representative_track;
    lyra::media::Track track{};
    if (!lyra::media::track_at(representative, &track)) return;
    lv_obj_t *row = make_button(parent, 7, y, 306, 60, kSurface, 5, true);
    lv_obj_t *title = make_label(row, group.name, kTextPrimary);
    make_marquee(title, 282);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 12, -10);
    char subtitle[lyra::media::kMaxName + 24];
    format_text_u32(group.track_count == 1 ? lyra::i18n::StringId::AlbumSummaryOne :
                    lyra::i18n::StringId::AlbumSummaryMany, track.artist,
                    static_cast<uint32_t>(group.track_count), subtitle, sizeof(subtitle));
    lv_obj_t *sub = make_label(row, subtitle, kTextSecondary);
    make_marquee(sub, 282);
    lv_obj_align(sub, LV_ALIGN_LEFT_MID, 12, 12);
    lv_obj_add_event_cb(row, open_album_detail_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(group_index));
}

void render_library_section(LibraryTab section)
{
    s_library_tab = section;
    const char *title = section == LibraryTab::Songs ? tr(lyra::i18n::StringId::Songs) :
                        section == LibraryTab::Artists ? tr(lyra::i18n::StringId::Artists) :
                        section == LibraryTab::Albums ? tr(lyra::i18n::StringId::Albums) :
                        section == LibraryTab::Genres ? tr(lyra::i18n::StringId::Genres) :
                        tr(lyra::i18n::StringId::Year);
    make_header(title, View::Library, true);
    lv_obj_t *body = make_scroll_body(72);
    const size_t page_size = library_page_size();
    if (section == LibraryTab::Songs) {
        const size_t count = lyra::media::track_count();
        const size_t pages = (count + page_size - 1) / page_size;
        if (pages && s_list_page >= pages) s_list_page = pages - 1;
        const size_t first = s_list_page * page_size;
        const size_t shown = std::min(page_size, count - std::min(first, count));
        for (size_t i = 0; i < shown; ++i) {
            size_t track_index;
            if (lyra::media::sorted_track_at(first + i, &track_index)) {
                make_song_row(body, static_cast<int>(i) * 58, track_index, 54);
            }
        }
        make_page_controls(body, static_cast<int>(shown) * 58 + 4, count, page_size);
        if (count == 0) make_label(body, tr(lyra::i18n::StringId::NoMusicScanned), kTextMuted);
        return;
    }

    const lyra::media::GroupKind kind = group_kind(section);
    const size_t count = lyra::media::group_count(kind);
    const size_t pages = (count + page_size - 1) / page_size;
    if (pages && s_list_page >= pages) s_list_page = pages - 1;
    const size_t first = s_list_page * page_size;
    const size_t shown = std::min(page_size, count - std::min(first, count));
    for (size_t i = 0; i < shown; ++i) {
        size_t group_index = first + i;
        if (section == LibraryTab::Albums || section == LibraryTab::Artists) {
            const lyra::media::SortSection sort_section = section == LibraryTab::Albums ?
                lyra::media::SortSection::Albums : lyra::media::SortSection::Artists;
            if (!lyra::media::sorted_group_index_at(sort_section, first + i, &group_index)) continue;
        }
        if (section == LibraryTab::Albums) {
            make_album_row(body, static_cast<int>(i) * 63, group_index);
            continue;
        }
        lyra::media::Group group{};
        if (!lyra::media::group_at(kind, group_index, &group)) continue;
        char subtitle[28];
        format_count(lyra::i18n::StringId::SongCount,
                     static_cast<uint32_t>(group.track_count), subtitle, sizeof(subtitle));
        lv_obj_t *row = make_row(body, static_cast<int>(i) * 58, nullptr,
                                 group.name, subtitle, View::TrackList, 54);
        lv_obj_remove_event_cb(row, route_cb);
        lv_obj_add_event_cb(row, open_track_list_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(group_index));
    }
    const int row_height = section == LibraryTab::Albums ? 63 : 58;
    make_page_controls(body, static_cast<int>(shown) * row_height + 4, count, page_size);
}

void render_track_list()
{
    char count_label[24];
    lyra::media::Group group{};
    if (!lyra::media::group_at(s_selected_group_kind, s_selected_group, &group)) return;
    const size_t matches = group.track_count;
    format_u32(lyra::i18n::StringId::TracksCount, static_cast<uint32_t>(matches),
               count_label, sizeof(count_label));
    make_header(s_track_list_title, library_section_view(s_library_tab), true, count_label);
    lv_obj_t *list = make_scroll_body(72);
    const size_t pages = (matches + lyra::media::kTrackPageSize - 1) / lyra::media::kTrackPageSize;
    if (pages && s_list_page >= pages) s_list_page = pages - 1;
    size_t indices[lyra::media::kTrackPageSize];
    const size_t shown = lyra::media::group_tracks(s_selected_group_kind, s_selected_group,
        s_list_page * lyra::media::kTrackPageSize, indices, lyra::media::kTrackPageSize);
    for (size_t i = 0; i < shown; ++i) make_song_row(list, static_cast<int>(i) * 58, indices[i], 54);
    make_page_controls(list, static_cast<int>(shown) * 58 + 4, matches);
}

void artist_detail_tab_cb(lv_event_t *event)
{
    s_artist_detail_tab = static_cast<ArtistDetailTab>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    s_list_page = 0;
    render(View::ArtistDetail);
}

void make_artist_detail_tab(lv_obj_t *parent, int x, const char *label,
                            ArtistDetailTab tab)
{
    lv_obj_t *button = make_button(parent, x, 0, 149, 40,
                                   s_artist_detail_tab == tab ? kAccentDark : kSurface, 6, true);
    lv_obj_t *text = make_label(button, label,
                                s_artist_detail_tab == tab ? kTextOnAccent : kTextSecondary);
    lv_obj_center(text);
    lv_obj_add_event_cb(button, artist_detail_tab_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(tab)));
}

struct ArtistDetailListContext {
    size_t artist_group;
    size_t entry_count;
    size_t loaded;
    ArtistDetailTab tab;
};

void append_artist_detail_rows(lv_obj_t *body, ArtistDetailListContext *context)
{
    if (!body || !context || context->loaded >= context->entry_count) return;
    constexpr size_t kArtistDetailBatchSize = 8;
    const size_t start = context->loaded;
    const size_t batch = std::min(kArtistDetailBatchSize, context->entry_count - start);
    if (context->tab == ArtistDetailTab::Songs) {
        size_t indices[kArtistDetailBatchSize];
        const size_t found = lyra::media::group_tracks(lyra::media::GroupKind::Artist,
            context->artist_group, start, indices, batch);
        for (size_t i = 0; i < found; ++i) {
            make_song_row(body, 48 + static_cast<int>(start + i) * 58, indices[i], 54);
        }
        context->loaded += found;
        return;
    }

    for (size_t i = 0; i < batch; ++i) {
        size_t album_group = 0;
        if (lyra::media::artist_album_at(context->artist_group, start + i, &album_group)) {
            make_album_row(body, 48 + static_cast<int>(start + i) * 63, album_group);
        }
    }
    context->loaded += batch;
}

void artist_detail_list_event_cb(lv_event_t *event)
{
    auto *context = static_cast<ArtistDetailListContext *>(lv_event_get_user_data(event));
    if (!context) return;
    if (lv_event_get_code(event) == LV_EVENT_DELETE) {
        lv_free(context);
        return;
    }
    lv_obj_t *body = lv_event_get_current_target_obj(event);
    if (lv_obj_get_scroll_bottom(body) < 140) append_artist_detail_rows(body, context);
}

void render_artist_detail()
{
    lyra::media::Group artist{};
    if (!lyra::media::group_at(lyra::media::GroupKind::Artist, s_selected_group, &artist)) return;
    make_header(artist.name, View::LibraryArtists, true);
    lv_obj_t *body = make_scroll_body(72);
    make_artist_detail_tab(body, 7, tr(lyra::i18n::StringId::Songs), ArtistDetailTab::Songs);
    make_artist_detail_tab(body, 164, tr(lyra::i18n::StringId::Albums), ArtistDetailTab::Albums);

    const size_t total = s_artist_detail_tab == ArtistDetailTab::Songs ?
        artist.track_count : lyra::media::artist_album_count(s_selected_group);
    if (total == 0) {
        make_label(body, s_artist_detail_tab == ArtistDetailTab::Songs ?
                   tr(lyra::i18n::StringId::ArtistNoIndexedSongs) :
                   tr(lyra::i18n::StringId::ArtistNoIndexedAlbums),
                   kTextMuted);
        return;
    }

    auto *context = static_cast<ArtistDetailListContext *>(
        lv_malloc_zeroed(sizeof(ArtistDetailListContext)));
    if (context) {
        context->artist_group = s_selected_group;
        context->entry_count = total;
        context->tab = s_artist_detail_tab;
        append_artist_detail_rows(body, context);
        lv_obj_add_event_cb(body, artist_detail_list_event_cb, LV_EVENT_SCROLL, context);
        lv_obj_add_event_cb(body, artist_detail_list_event_cb, LV_EVENT_SCROLL_END, context);
        lv_obj_add_event_cb(body, artist_detail_list_event_cb, LV_EVENT_DELETE, context);
    }
}

struct AlbumListContext {
    size_t group_index;
    size_t track_count;
    size_t loaded;
};

void append_album_song_rows(lv_obj_t *body, AlbumListContext *context)
{
    if (!body || !context || context->loaded >= context->track_count) return;
    constexpr size_t kAlbumBatchSize = 8;
    size_t indices[kAlbumBatchSize];
    const size_t found = lyra::media::group_tracks(lyra::media::GroupKind::Album,
        context->group_index, context->loaded, indices, kAlbumBatchSize);
    for (size_t i = 0; i < found; ++i) {
        make_song_row(body, 130 + static_cast<int>(context->loaded + i) * 58, indices[i], 54);
    }
    context->loaded += found;
}

void album_list_event_cb(lv_event_t *event)
{
    auto *context = static_cast<AlbumListContext *>(lv_event_get_user_data(event));
    if (!context) return;
    if (lv_event_get_code(event) == LV_EVENT_DELETE) {
        lv_free(context);
        return;
    }
    lv_obj_t *body = lv_event_get_current_target_obj(event);
    if (lv_obj_get_scroll_bottom(body) < 140) append_album_song_rows(body, context);
}

void render_album_detail()
{
    lyra::media::Group album{};
    if (!lyra::media::group_at(lyra::media::GroupKind::Album, s_selected_group, &album)) return;
    const size_t representative = album.representative_track;
    lyra::media::Track track{};
    if (!lyra::media::track_at(representative, &track)) return;
    make_header(album.name, View::LibraryAlbums, true);
    lv_obj_t *body = make_scroll_body(72);
    lv_obj_t *summary = make_box(body, 7, 0, 306, 122, kSurface, 7, true);
    make_album_art(summary, 8, 8, 106, 106, track);
    lv_obj_t *name = make_label(summary, album.name, kTextPrimary);
    lv_obj_set_style_text_font(name, lyra::font::ui(), 0);
    make_marquee(name, 174);
    lv_obj_set_pos(name, 124, 12);
    lv_obj_t *artist = make_label(summary, track.artist, kTextSecondary);
    make_marquee(artist, 174);
    lv_obj_set_pos(artist, 124, 40);
    // genre can occupy kMaxName - 1 bytes and year up to 7 bytes.
    char tags[lyra::media::kMaxName + 11];
    std::snprintf(tags, sizeof(tags), "%s / %s", track.genre, track.year);
    lv_obj_t *tag_label = make_label(summary, tags, kTextMuted);
    make_marquee(tag_label, 174);
    lv_obj_set_pos(tag_label, 124, 65);
    char count[28];
    format_count(lyra::i18n::StringId::AlbumTrackCount,
                 static_cast<uint32_t>(album.track_count), count, sizeof(count));
    lv_obj_t *count_label = make_label(summary, count, kAccent);
    lv_obj_set_pos(count_label, 124, 91);

    auto *context = static_cast<AlbumListContext *>(lv_malloc_zeroed(sizeof(AlbumListContext)));
    if (context) {
        context->group_index = s_selected_group;
        context->track_count = album.track_count;
        append_album_song_rows(body, context);
        lv_obj_add_event_cb(body, album_list_event_cb, LV_EVENT_SCROLL, context);
        lv_obj_add_event_cb(body, album_list_event_cb, LV_EVENT_SCROLL_END, context);
        lv_obj_add_event_cb(body, album_list_event_cb, LV_EVENT_DELETE, context);
    }
}

void folder_cb(lv_event_t *event)
{
    const size_t index = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    if (index >= s_folder_count) return;
    char child_path[sizeof(s_folder_path)];
    copy_ui_text(child_path, sizeof(child_path), s_folder_path);
    if (!append_ui_text(child_path, sizeof(child_path), "/") ||
        !append_ui_text(child_path, sizeof(child_path), s_folder_names[index])) return;
    push_navigation_state();
    copy_ui_text(s_folder_path, sizeof(s_folder_path), child_path);
    s_list_page = 0;
    render(View::FolderDetail);
}

void folder_back_cb(lv_event_t *)
{
    navigate_back(View::Folders);
}

void render_folders(bool detail)
{
    if (!detail) copy_ui_text(s_folder_path, sizeof(s_folder_path), "/sdcard");
    make_header(detail ? s_folder_path + std::strlen("/sdcard/") :
                         tr(lyra::i18n::StringId::BrowseFolders),
                detail ? View::Folders : View::Menu, true,
                detail ? tr(lyra::i18n::StringId::MicroSdBadge) : nullptr,
                detail ? folder_back_cb : nullptr);
    lv_obj_t *list = make_scroll_body(72);
    size_t total_folders = 0;
    s_folder_count = lyra::media::child_folders(s_folder_path, 0, s_folder_names, 1, &total_folders);
    size_t total_tracks = 0;
    size_t ignored_indices[1];
    lyra::media::folder_tracks(s_folder_path, 0, ignored_indices, 1, &total_tracks);
    const size_t total_entries = total_folders + total_tracks;
    const size_t pages = (total_entries + lyra::media::kTrackPageSize - 1) /
                         lyra::media::kTrackPageSize;
    if (pages && s_list_page >= pages) s_list_page = pages - 1;
    const size_t first_entry = s_list_page * lyra::media::kTrackPageSize;
    const size_t last_entry = std::min(first_entry + lyra::media::kTrackPageSize, total_entries);
    size_t row = 0;
    const size_t first_folder = std::min(first_entry, total_folders);
    const size_t last_folder = std::min(last_entry, total_folders);
    const size_t folder_capacity = last_folder - first_folder;
    s_folder_count = folder_capacity == 0 ? 0 : lyra::media::child_folders(
        s_folder_path, first_folder, s_folder_names, folder_capacity, &total_folders);
    for (size_t i = 0; i < s_folder_count; ++i) {
        lv_obj_t *folder = make_row(list, static_cast<int>(row++) * 58, LV_SYMBOL_DIRECTORY,
                                    s_folder_names[i], tr(lyra::i18n::StringId::Folder),
                                    View::FolderDetail, 54);
        lv_obj_remove_event_cb(folder, route_cb);
        lv_obj_add_event_cb(folder, folder_cb, LV_EVENT_CLICKED, reinterpret_cast<void *>(i));
    }
    size_t indices[lyra::media::kTrackPageSize];
    const size_t track_offset = first_entry > total_folders ? first_entry - total_folders : 0;
    const size_t track_capacity = last_entry > total_folders ?
        last_entry - std::max(first_entry, total_folders) : 0;
    const size_t tracks = track_capacity == 0 ? 0 : lyra::media::folder_tracks(
        s_folder_path, track_offset, indices, track_capacity, &total_tracks);
    for (size_t i = 0; i < tracks; ++i) make_file_row(list, static_cast<int>(row++) * 58, indices[i], 54);
    make_page_controls(list, static_cast<int>(row) * 58 + 4, total_entries);
    if (row == 0) make_label(list, tr(lyra::i18n::StringId::FolderNoScannedAudio), kTextMuted);
}

void playlist_cb(lv_event_t *event)
{
    push_navigation_state();
    s_selected_playlist = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    s_list_page = 0;
    s_playlist_manage_mode = false;
    render(View::PlaylistDetail);
}

void open_playlist_add_cb(lv_event_t *)
{
    navigate_to(View::PlaylistAdd);
}

void toggle_playlist_manage_cb(lv_event_t *)
{
    s_playlist_manage_mode = !s_playlist_manage_mode;
    render(s_view);
}

void confirm_playlist_manage_action_cb(lv_event_t *event)
{
    lv_obj_t *overlay = static_cast<lv_obj_t *>(lv_event_get_user_data(event));
    const bool deleting_playlist =
        s_pending_playlist_manage_action == PlaylistManageAction::DeletePlaylist;
    const esp_err_t result = deleting_playlist ?
        lyra::media::delete_playlist(s_pending_playlist) :
        lyra::media::remove_from_playlist(s_pending_playlist, s_pending_playlist_track);
    if (overlay) lv_obj_delete(overlay);
    if (deleting_playlist) {
        s_playlist_manage_mode = result != ESP_OK;
        s_list_page = 0;
        render(View::Playlists);
    } else {
        render(View::PlaylistDetail);
    }
}

void show_playlist_manage_confirmation(PlaylistManageAction action, size_t playlist_index,
                                       size_t track_index)
{
    lyra::media::Playlist playlist{};
    if (!lyra::media::playlist_at(playlist_index, &playlist)) return;
    s_pending_playlist_manage_action = action;
    s_pending_playlist = playlist_index;
    s_pending_playlist_track = track_index;
    const bool favorites = std::strcmp(playlist.name, "Favorites") == 0;
    const char *title = action == PlaylistManageAction::RemoveTrack ?
        tr(lyra::i18n::StringId::RemoveSongQuestion) : favorites ?
        tr(lyra::i18n::StringId::ClearFavoritesQuestion) :
        tr(lyra::i18n::StringId::DeletePlaylistQuestion);
    const char *message = action == PlaylistManageAction::RemoveTrack ?
        tr(lyra::i18n::StringId::RemoveSongMessage) : favorites ?
        tr(lyra::i18n::StringId::ClearFavoritesMessage) :
        tr(lyra::i18n::StringId::DeletePlaylistMessage);
    lv_obj_t *overlay = make_box(s_screen, 0, 0, kScreenWidth, kScreenHeight,
                                 kOverlay);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_90, 0);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(overlay);
    lv_obj_t *dialog = make_box(overlay, 20, 126, 280, 228, kSurfaceRaised, 12);
    lv_obj_t *heading = make_label(dialog, title, kTextPrimary);
    lv_obj_align(heading, LV_ALIGN_TOP_MID, 0, 22);
    lv_obj_t *detail = make_label(dialog, message, kTextSecondary);
    lv_obj_set_style_text_align(detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(detail, 240);
    lv_label_set_long_mode(detail, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(detail, LV_ALIGN_CENTER, 0, -12);
    lv_obj_t *cancel = make_button(dialog, 14, 166, 116, 48, kSurface, 7);
    lv_obj_t *cancel_label = make_label(cancel, tr(lyra::i18n::StringId::Cancel), kTextSecondary);
    lv_obj_center(cancel_label);
    lv_obj_add_event_cb(cancel, dismiss_overlay_cb, LV_EVENT_CLICKED, overlay);
    lv_obj_t *confirm = make_button(dialog, 150, 166, 116, 48, lv_color_hex(0x991B1B), 7);
    lv_obj_t *confirm_label = make_label(confirm, action == PlaylistManageAction::RemoveTrack ?
                                          tr(lyra::i18n::StringId::Remove) : favorites ?
                                          tr(lyra::i18n::StringId::Clear) :
                                          tr(lyra::i18n::StringId::Delete), kTextOnAccent);
    lv_obj_center(confirm_label);
    lv_obj_add_event_cb(confirm, confirm_playlist_manage_action_cb, LV_EVENT_CLICKED, overlay);
}

void delete_playlist_cb(lv_event_t *event)
{
    show_playlist_manage_confirmation(PlaylistManageAction::DeletePlaylist,
                                      reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
}

void remove_playlist_track_cb(lv_event_t *event)
{
    show_playlist_manage_confirmation(PlaylistManageAction::RemoveTrack, s_selected_playlist,
                                      reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
}

void make_playlist_manage_row(lv_obj_t *parent, int y, size_t playlist_index,
                              const lyra::media::Playlist &playlist)
{
    lv_obj_t *row = make_button(parent, 7, y, 306, 58, kSurface, 6, true);
    lv_obj_t *name = make_label(row, playlist.name, kTextPrimary);
    make_marquee(name, 205);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 12, -10);
    char subtitle[28];
    format_count(lyra::i18n::StringId::SongCount,
                 static_cast<uint32_t>(playlist.track_count), subtitle, sizeof(subtitle));
    lv_obj_t *sub = make_label(row, subtitle, kTextSecondary);
    lv_obj_align(sub, LV_ALIGN_LEFT_MID, 12, 11);
    lv_obj_t *remove = make_button(row, 246, 11, 48, 36, lv_color_hex(0x991B1B), 5);
    lv_obj_t *remove_icon = make_label(remove, LV_SYMBOL_TRASH, kTextOnAccent);
    lv_obj_center(remove_icon);
    lv_obj_add_event_cb(remove, delete_playlist_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(playlist_index));
}

void make_playlist_track_manage_row(lv_obj_t *parent, int y, size_t track_index)
{
    lyra::media::Track track{};
    if (!lyra::media::track_at(track_index, &track)) return;
    lv_obj_t *row = make_button(parent, 7, y, 306, 54, kSurface, 5, true);
    lv_obj_t *title = make_label(row, track.title, kTextPrimary);
    make_marquee(title, 220);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 12, -9);
    lv_obj_t *artist = make_label(row, track.artist, kTextSecondary);
    make_marquee(artist, 220);
    lv_obj_align(artist, LV_ALIGN_LEFT_MID, 12, 11);
    lv_obj_t *remove = make_button(row, 246, 9, 48, 36, lv_color_hex(0x991B1B), 5);
    lv_obj_t *remove_icon = make_label(remove, LV_SYMBOL_TRASH, kTextOnAccent);
    lv_obj_center(remove_icon);
    lv_obj_add_event_cb(remove, remove_playlist_track_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(track_index));
}

void render_playlists(bool detail)
{
    // Returning to a playlist always ends the temporary library selection mode.
    s_playlist_add_mode = false;
    lyra::media::Playlist selected{};
    if (detail) lyra::media::playlist_at(s_selected_playlist, &selected);
    const char *title = detail ? selected.name : tr(lyra::i18n::StringId::Playlists);
    make_header(title, detail ? View::Playlists :
                (s_playlists_from_library ? View::Library : View::Menu), true, "");
    lv_obj_t *plus = make_button(s_screen, 220, 32, 44, 36, kBackground, 4);
    lv_obj_t *plus_label = make_label(plus, LV_SYMBOL_PLUS, kAccent);
    lv_obj_center(plus_label);
    if (detail) {
        lv_obj_add_event_cb(plus, open_playlist_add_cb, LV_EVENT_CLICKED, nullptr);
    } else {
        add_route(plus, View::PlaylistCreate);
    }
    lv_obj_t *manage = make_button(s_screen, 270, 32, 44, 36, kBackground, 4);
    lv_obj_t *manage_label = make_label(manage,
                                        s_playlist_manage_mode ? LV_SYMBOL_CLOSE : LV_SYMBOL_EDIT,
                                        s_playlist_manage_mode ? kTextSecondary : kAccent);
    lv_obj_center(manage_label);
    lv_obj_add_event_cb(manage, toggle_playlist_manage_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *list = make_scroll_body(72);
    if (!detail) {
        const size_t count = lyra::media::playlist_count();
        for (size_t i = 0; i < count; ++i) {
            lyra::media::Playlist playlist{};
            if (!lyra::media::playlist_at(i, &playlist)) continue;
            char subtitle[28];
            format_count(lyra::i18n::StringId::SongCount,
                         static_cast<uint32_t>(playlist.track_count), subtitle, sizeof(subtitle));
            if (s_playlist_manage_mode) {
                make_playlist_manage_row(list, static_cast<int>(i) * 62, i, playlist);
            } else {
                lv_obj_t *row = make_row(list, static_cast<int>(i) * 62, nullptr, playlist.name,
                                         subtitle, View::PlaylistDetail, 58);
                lv_obj_remove_event_cb(row, route_cb);
                lv_obj_add_event_cb(row, playlist_cb, LV_EVENT_CLICKED, reinterpret_cast<void *>(i));
            }
        }
        if (count == 0) make_label(list, tr(lyra::i18n::StringId::NoPlaylistsYetCreate), kTextMuted);
    } else {
        const size_t pages = (selected.track_count + lyra::media::kTrackPageSize - 1) /
                             lyra::media::kTrackPageSize;
        if (pages && s_list_page >= pages) s_list_page = pages - 1;
        size_t indices[lyra::media::kTrackPageSize];
        const size_t count = lyra::media::playlist_tracks(s_selected_playlist,
            s_list_page * lyra::media::kTrackPageSize, indices, lyra::media::kTrackPageSize);
        for (size_t i = 0; i < count; ++i) {
            if (s_playlist_manage_mode) {
                make_playlist_track_manage_row(list, static_cast<int>(i) * 58, indices[i]);
            } else {
                make_song_row(list, static_cast<int>(i) * 58, indices[i], 54);
            }
        }
        make_page_controls(list, static_cast<int>(count) * 58 + 4, selected.track_count);
        if (count == 0) make_label(list, tr(lyra::i18n::StringId::PlaylistEmptyAdd), kTextMuted);
    }
}

void toggle_favorite_cb(lv_event_t *event)
{
    const bool favorite = lyra::media::is_favorite(s_current_track);
    if (lyra::media::set_favorite(s_current_track, !favorite) != ESP_OK) return;
    lv_obj_t *label = static_cast<lv_obj_t *>(lv_event_get_user_data(event));
    lv_label_set_text(label, !favorite ? kHeartFilled : kHeartOutline);
    lv_obj_set_style_text_color(label, !favorite ? kAccent : kTextPrimary, 0);
}

void close_playlist_picker()
{
    if (!s_playlist_picker) return;
    lv_obj_delete(s_playlist_picker);
    s_playlist_picker = nullptr;
}

void add_current_track_to_playlist_cb(lv_event_t *event)
{
    const size_t playlist_index = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    if (lyra::media::add_to_playlist(playlist_index, s_current_track) == ESP_OK) {
        close_playlist_picker();
    }
}

void show_player_playlist_picker_cb(lv_event_t *)
{
    if (s_playlist_picker) return;

    const int picker_height = content_bottom();
    const int dialog_height = std::min(316, picker_height - 64);
    s_playlist_picker = make_box(s_screen, 0, 0, kScreenWidth, picker_height, kBackground);
    lv_obj_set_style_bg_opa(s_playlist_picker, LV_OPA_80, 0);
    lv_obj_add_flag(s_playlist_picker, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_playlist_picker, [](lv_event_t *) { close_playlist_picker(); },
                        LV_EVENT_CLICKED, nullptr);

    lv_obj_t *dialog = make_box(s_playlist_picker, 16, (picker_height - dialog_height) / 2,
                                288, dialog_height, kSurfaceRaised, 10);
    lv_obj_t *title = make_label(dialog, tr(lyra::i18n::StringId::AddToPlaylistTitle), kTextPrimary);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 12);
    lv_obj_t *close = make_button(dialog, 246, 7, 34, 34, kSurface, 17);
    lv_obj_t *close_label = make_label(close, LV_SYMBOL_CLOSE, kTextSecondary);
    lv_obj_center(close_label);
    lv_obj_add_event_cb(close, [](lv_event_t *) { close_playlist_picker(); },
                        LV_EVENT_CLICKED, nullptr);

    lv_obj_t *list = make_box(dialog, 8, 48, 272, dialog_height - 56, kSurface, 6);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    const size_t count = lyra::media::playlist_count();
    for (size_t i = 0; i < count; ++i) {
        lyra::media::Playlist playlist{};
        if (!lyra::media::playlist_at(i, &playlist)) continue;
        lv_obj_t *choice = make_button(list, 0, static_cast<int>(i) * 50,
                                        272, 46, kBackground, 5, true);
        lv_obj_t *name = make_label(choice, playlist.name, kTextPrimary);
        make_marquee(name, 196);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 12, 0);
        lv_obj_t *choice_arrow = make_label(choice, LV_SYMBOL_RIGHT, kAccent);
        lv_obj_align(choice_arrow, LV_ALIGN_RIGHT_MID, -12, 0);
        lv_obj_add_event_cb(choice, add_current_track_to_playlist_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(i));
    }
    if (count == 0) {
        lv_obj_t *empty = make_label(list, tr(lyra::i18n::StringId::NoPlaylistsCreateFirst), kTextMuted);
        lv_obj_align(empty, LV_ALIGN_TOP_LEFT, 12, 12);
    }
}

} // namespace lyra::gui::internal
