/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../lyra_gui_internal.h"

namespace lyra::gui::internal {

const char *smart_playlist_title(lyra::media::SmartPlaylistKind kind)
{
    switch (kind) {
    case lyra::media::SmartPlaylistKind::RecentlyAdded:
        return tr(lyra::i18n::StringId::RecentlyAdded);
    case lyra::media::SmartPlaylistKind::RecentlyPlayed:
        return tr(lyra::i18n::StringId::RecentlyPlayed);
    case lyra::media::SmartPlaylistKind::MostPlayed:
        return tr(lyra::i18n::StringId::MostPlayed);
    case lyra::media::SmartPlaylistKind::NeverPlayed:
        return tr(lyra::i18n::StringId::NeverPlayed);
    case lyra::media::SmartPlaylistKind::Count:
        break;
    }
    return tr(lyra::i18n::StringId::SmartPlaylists);
}

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

static void open_smart_playlist(lyra::media::SmartPlaylistKind kind)
{
    push_navigation_state();
    s_selected_playlist_is_smart = true;
    s_selected_smart_playlist = kind;
    s_playlists_from_library = true;
    s_playlist_manage_mode = false;
    s_list_page = 0;
    render(View::PlaylistDetail);
}

static void open_favorites()
{
    const size_t count = lyra::media::playlist_count();
    for (size_t index = 0; index < count; ++index) {
        lyra::media::Playlist playlist{};
        if (lyra::media::playlist_at(index, &playlist) && std::strcmp(playlist.name, "Favorites") == 0) {
            push_navigation_state();
            s_selected_playlist = index;
            s_selected_playlist_is_smart = false;
            s_playlists_from_library = true;
            s_playlist_manage_mode = false;
            s_list_page = 0;
            render(View::PlaylistDetail);
            return;
        }
    }
    open_library_playlists_cb(nullptr);
}

void library_shortcut_cb(lv_event_t *event)
{
    switch (reinterpret_cast<uintptr_t>(lv_event_get_user_data(event))) {
        case 0: open_smart_playlist(lyra::media::SmartPlaylistKind::RecentlyPlayed); break;
        case 1: open_smart_playlist(lyra::media::SmartPlaylistKind::RecentlyAdded); break;
        case 2: open_favorites(); break;
        default: open_folders_cb(event); break;
    }
}

// Quick ways back into what you listen to most, before the full lists.
static void make_library_shelf(lv_obj_t *body)
{
    const lyra::i18n::StringId labels[] = {
        lyra::i18n::StringId::RecentlyPlayed,
        lyra::i18n::StringId::RecentlyAdded,
        lyra::i18n::StringId::Favorites,
    };
    constexpr Icon icons[] = {Icon::Recent, Icon::Added, Icon::Favorite};
    for (size_t i = 0; i < 3; ++i) {
        lv_obj_t *chip = make_button(body, 7 + static_cast<int>(i) * 103, 0, 100, 58,
                                     kSurfaceRaised, 8, true);
        lv_obj_t *icon = make_icon(chip, icons[i], kAccent);
        lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 8);
        lv_obj_t *label = make_label(chip, tr(labels[i]), kTextPrimary);
        make_marquee(label, 92);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -6);
        lv_obj_add_event_cb(chip, library_shortcut_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<uintptr_t>(i)));
        if (s_neon_hud) add_hud_frame(chip, true);
    }
}

void render_library()
{
    if (is_zeno_theme() && !s_playlist_add_mode) { render_zeno_library(); return; }
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
    const int top = adding_to_playlist ? 0 : 66;
    if (!adding_to_playlist) make_library_shelf(body);
    make_row(body, top, Icon::Music, tr(lyra::i18n::StringId::Songs),
             tr(lyra::i18n::StringId::BrowseEverySong), View::LibrarySongs, 54);
    make_row(body, top + 58, Icon::Album, tr(lyra::i18n::StringId::Albums),
             tr(lyra::i18n::StringId::ArtworkAndAlbumSummaries), View::LibraryAlbums, 54);
    make_row(body, top + 116, Icon::Artist, tr(lyra::i18n::StringId::Artists),
             tr(lyra::i18n::StringId::BrowseByArtist), View::LibraryArtists, 54);
    make_row(body, top + 174, Icon::Genre, tr(lyra::i18n::StringId::Genres),
             tr(lyra::i18n::StringId::BrowseByGenre), View::LibraryGenres, 54);
    make_row(body, top + 232, Icon::Calendar, tr(lyra::i18n::StringId::Year),
             tr(lyra::i18n::StringId::BrowseByReleaseYear), View::LibraryYears, 54);
    if (adding_to_playlist) {
        lv_obj_t *hint = make_label(body, tr(lyra::i18n::StringId::SelectSongToAdd), kTextMuted);
        lv_obj_set_pos(hint, 12, 300);
        return;
    }
    lv_obj_t *playlists = make_row(body, top + 290, Icon::Playlist, tr(lyra::i18n::StringId::Playlists),
                                   tr(lyra::i18n::StringId::YourSavedPlaylists),
                                   View::Playlists, 54);
    lv_obj_remove_event_cb(playlists, route_cb);
    lv_obj_add_event_cb(playlists, open_library_playlists_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *folders = make_row(body, top + 348, Icon::Folder, tr(lyra::i18n::StringId::Folders),
                                 tr(lyra::i18n::StringId::BrowseByFolder), View::Folders, 54);
    lv_obj_remove_event_cb(folders, route_cb);
    lv_obj_add_event_cb(folders, library_shortcut_cb, LV_EVENT_CLICKED, reinterpret_cast<void *>(3));
    make_row(body, top + 406, Icon::Radio, tr(lyra::i18n::StringId::InternetRadio),
             tr(lyra::i18n::StringId::RadioStreamsDescription), View::InternetRadio, 54);
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
    make_marquee(title, s_queue_add_mode ? 236 : 282);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 12, -10);
    char subtitle[lyra::media::kMaxName + 24];
    format_text_u32(group.track_count == 1 ? lyra::i18n::StringId::AlbumSummaryOne :
                    lyra::i18n::StringId::AlbumSummaryMany, track.artist,
                    static_cast<uint32_t>(group.track_count), subtitle, sizeof(subtitle));
    lv_obj_t *sub = make_label(row, subtitle, kTextSecondary);
    make_marquee(sub, s_queue_add_mode ? 236 : 282);
    lv_obj_align(sub, LV_ALIGN_LEFT_MID, 12, 12);
    lv_obj_add_event_cb(row, open_album_detail_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(group_index));
    if (s_queue_add_mode) {
        lv_obj_t *queue_button = make_button(row, 260, 12, 34, 36, kAccentDark, 5);
        lv_obj_t *queue_icon = make_label(queue_button, LV_SYMBOL_PLUS, kTextOnAccent);
        lv_obj_center(queue_icon);
        lv_obj_add_event_cb(queue_button, [](lv_event_t *event) {
            lv_event_stop_bubbling(event);
            show_queue_add_picker_for_album(reinterpret_cast<uintptr_t>(
                lv_event_get_user_data(event)));
        }, LV_EVENT_CLICKED, reinterpret_cast<void *>(group_index));
    }
}

namespace {

// ---- Scrolling library sections ---------------------------------------------
// Fetchers run on the prefetch task and touch only the media service; builders
// run on the GUI thread and use only the cached text.

bool fetch_song(size_t position, VirtualRow *row)
{
    size_t track = 0;
    lyra::media::TrackSummary summary{};
    if (!lyra::media::sorted_track_at(position, &track) ||
        !lyra::media::track_summary_at(track, &summary)) return false;
    row->payload = track;
    copy_ui_text(row->title, sizeof(row->title), summary.title);
    copy_ui_text(row->subtitle, sizeof(row->subtitle), summary.artist);
    return true;
}

bool fetch_album(size_t position, VirtualRow *row)
{
    size_t group_index = position;
    lyra::media::Group group{};
    lyra::media::TrackSummary summary{};
    if (!lyra::media::sorted_group_index_at(lyra::media::SortSection::Albums, position, &group_index) ||
        !lyra::media::group_at(lyra::media::GroupKind::Album, group_index, &group)) return false;
    lyra::media::track_summary_at(group.representative_track, &summary);
    row->payload = group_index;
    copy_ui_text(row->title, sizeof(row->title), group.name);
    format_text_u32(group.track_count == 1 ? lyra::i18n::StringId::AlbumSummaryOne :
                    lyra::i18n::StringId::AlbumSummaryMany, summary.artist,
                    static_cast<uint32_t>(group.track_count), row->subtitle, sizeof(row->subtitle));
    return true;
}

bool fetch_group(lyra::media::GroupKind kind, size_t group_index, VirtualRow *row)
{
    lyra::media::Group group{};
    if (!lyra::media::group_at(kind, group_index, &group)) return false;
    row->payload = group_index;
    copy_ui_text(row->title, sizeof(row->title), group.name);
    format_count(lyra::i18n::StringId::SongCount, static_cast<uint32_t>(group.track_count),
                 row->subtitle, sizeof(row->subtitle));
    return true;
}

bool fetch_artist(size_t position, VirtualRow *row)
{
    size_t group_index = position;
    return lyra::media::sorted_group_index_at(lyra::media::SortSection::Artists, position, &group_index) &&
           fetch_group(lyra::media::GroupKind::Artist, group_index, row);
}

bool fetch_genre(size_t position, VirtualRow *row)
{
    return fetch_group(lyra::media::GroupKind::Genre, position, row);
}

bool fetch_year(size_t position, VirtualRow *row)
{
    return fetch_group(lyra::media::GroupKind::Year, position, row);
}

void make_plus_button(lv_obj_t *row, int x, int y, lv_event_cb_t callback, size_t payload)
{
    lv_obj_t *button = make_button(row, x, y, 34, 36, kAccentDark, 5);
    lv_obj_t *icon = make_label(button, LV_SYMBOL_PLUS, kTextOnAccent);
    lv_obj_center(icon);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, reinterpret_cast<void *>(payload));
}

void build_song(lv_obj_t *slot, size_t, const VirtualRow &data, int width)
{
    if (!data.title[0]) return;
    const size_t track = data.payload;
    // Matches make_song_row(); comparing indices avoids a per-row path read.
    const bool current = !is_standard_theme() && s_has_active_queue && track == s_current_track;
    lv_obj_t *row = make_button(slot, 7, 0, width, 54, current ? kAccentSurface : kSurface, 5, true);
    const int text_x = is_zeno_theme() && current ? 34 : 12;
    const int text_width = width - text_x - (s_playlist_add_mode ? 50 : s_queue_add_mode ? 58 : 12);
    lv_obj_t *title = make_label(row, data.title, current ? kAccent : kTextPrimary);
    if (is_zeno_theme()) lv_obj_set_style_text_color(title, kTextPrimary, 0);
    make_marquee(title, text_width);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, text_x, -9);
    lv_obj_t *artist = make_label(row, data.subtitle, kTextSecondary);
    make_marquee(artist, text_width);
    lv_obj_align(artist, LV_ALIGN_LEFT_MID, text_x, 11);
    if (is_zeno_theme() && current) {
        lv_obj_t *playing = make_label(row, LV_SYMBOL_PLAY, kTextPrimary);
        lv_obj_align(playing, LV_ALIGN_LEFT_MID, 10, 0);
    }
    if (s_playlist_add_mode) {
        lv_obj_t *add = make_label(row, LV_SYMBOL_PLUS, kAccent);
        lv_obj_align(add, LV_ALIGN_RIGHT_MID, -12, 0);
        lv_obj_add_event_cb(row, add_playlist_track_cb, LV_EVENT_CLICKED, reinterpret_cast<void *>(track));
        return;
    }
    add_track_route(row, track);
    if (s_queue_add_mode) {
        make_plus_button(row, width - 46, 9, [](lv_event_t *event) {
            lv_event_stop_bubbling(event);
            show_queue_add_picker_for_track(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
        }, track);
    }
}

void build_album(lv_obj_t *slot, size_t, const VirtualRow &data, int width)
{
    if (!data.title[0]) return;
    lv_obj_t *row = make_button(slot, 7, 0, width, 60, kSurface, 5, true);
    const int text_width = width - (s_queue_add_mode ? 70 : 24);
    lv_obj_t *title = make_label(row, data.title, kTextPrimary);
    make_marquee(title, text_width);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 12, -10);
    lv_obj_t *sub = make_label(row, data.subtitle, kTextSecondary);
    make_marquee(sub, text_width);
    lv_obj_align(sub, LV_ALIGN_LEFT_MID, 12, 12);
    lv_obj_add_event_cb(row, open_album_detail_cb, LV_EVENT_CLICKED, reinterpret_cast<void *>(data.payload));
    if (s_queue_add_mode) {
        make_plus_button(row, width - 46, 12, [](lv_event_t *event) {
            lv_event_stop_bubbling(event);
            show_queue_add_picker_for_album(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
        }, data.payload);
    }
}

void build_group(lv_obj_t *slot, size_t, const VirtualRow &data, int width)
{
    if (!data.title[0]) return;
    lv_obj_t *row = make_button(slot, 7, 0, width, 54, kSurface, 6, true);
    if (!is_aura_theme()) {
        lv_obj_set_style_bg_color(row, !is_standard_theme() ? kAccentSurface : kSurfaceRaised,
                                  LV_STATE_PRESSED);
    }
    lv_obj_t *title = make_label(row, data.title, kTextPrimary);
    make_marquee(title, width - 47);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 12, -10);
    lv_obj_t *sub = make_label(row, data.subtitle, kTextSecondary);
    make_marquee(sub, width - 47);
    lv_obj_align(sub, LV_ALIGN_LEFT_MID, 12, 11);
    lv_obj_t *chevron = make_label(row, LV_SYMBOL_RIGHT, kTextMuted);
    lv_obj_align(chevron, LV_ALIGN_RIGHT_MID, -12, 0);
    lv_obj_add_event_cb(row, open_track_list_cb, LV_EVENT_CLICKED, reinterpret_cast<void *>(data.payload));
}

} // namespace

void render_library_section(LibraryTab section)
{
    s_library_tab = section;
    const char *title = section == LibraryTab::Songs ? tr(is_zeno_theme() ?
                        lyra::i18n::StringId::AllSongs : lyra::i18n::StringId::Songs) :
                        section == LibraryTab::Artists ? tr(lyra::i18n::StringId::Artists) :
                        section == LibraryTab::Albums ? tr(lyra::i18n::StringId::Albums) :
                        section == LibraryTab::Genres ? tr(lyra::i18n::StringId::Genres) :
                        tr(lyra::i18n::StringId::Year);
    const size_t count = section == LibraryTab::Songs ? lyra::media::track_count() :
                         lyra::media::group_count(group_kind(section));
    char count_text[28];
    format_count(lyra::i18n::StringId::SongCount, static_cast<uint32_t>(count),
                 count_text, sizeof(count_text));
    make_header(title, View::Library, true, section == LibraryTab::Songs ? count_text : nullptr);
    lv_obj_t *body = make_scroll_body(72);
    if (count == 0) {
        lv_obj_t *empty = make_label(body, tr(lyra::i18n::StringId::NoMusicScanned), kTextMuted);
        lv_obj_set_width(empty, 296);
        lv_label_set_long_mode(empty, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_pos(empty, 12, 8);
        return;
    }
    VirtualListSpec spec{};
    spec.count = count;
    spec.top = 0;
    spec.pitch = section == LibraryTab::Albums ? 63 : 58;
    spec.row_height = section == LibraryTab::Albums ? 60 : 54;
    switch (section) {
        case LibraryTab::Songs: spec.fetch = fetch_song; spec.build = build_song; break;
        case LibraryTab::Albums: spec.fetch = fetch_album; spec.build = build_album; break;
        case LibraryTab::Artists: spec.fetch = fetch_artist; spec.build = build_group; break;
        case LibraryTab::Genres: spec.fetch = fetch_genre; spec.build = build_group; break;
        case LibraryTab::Years: spec.fetch = fetch_year; spec.build = build_group; break;
    }
    const bool sortable = section == LibraryTab::Songs || section == LibraryTab::Albums ||
                          section == LibraryTab::Artists;
    spec.letter_strip = sortable;
    spec.jump_button = sortable;
    spec.letter_section = section == LibraryTab::Albums ? lyra::media::SortSection::Albums :
                          section == LibraryTab::Artists ? lyra::media::SortSection::Artists :
                                                           lyra::media::SortSection::Songs;
    make_virtual_list(body, spec);
}

void render_track_list()
{
    char count_label[24];
    lyra::media::Group group{};
    if (!lyra::media::group_at(s_selected_group_kind, s_selected_group, &group)) return;
    const size_t matches = group.track_count;
    format_count(lyra::i18n::StringId::TracksCount, static_cast<uint32_t>(matches),
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

// group_tracks() sorts and reads the entire group even for an eight-row slice.
// Retain only its compact index array while this lazy list exists; all row
// creation, ordering and scroll thresholds stay the same.
struct DetailTrackOrder {
    size_t *indices;
    uint32_t catalog_generation;
    uint32_t sorting_generation;
    bool attempted;
};

size_t detail_group_tracks(DetailTrackOrder *order, lyra::media::GroupKind kind,
                           size_t group, size_t total, size_t offset,
                           size_t *output, size_t capacity)
{
    if (offset >= total) return 0;
    const lyra::media::Status before = lyra::media::status();
    if (order->attempted &&
        (order->catalog_generation != before.catalog_generation ||
         order->sorting_generation != before.sorting_generation)) {
        heap_caps_free(order->indices);
        *order = {};
    }
    if (!order->attempted) {
        order->attempted = true;
        order->catalog_generation = before.catalog_generation;
        order->sorting_generation = before.sorting_generation;
        // Optional PSRAM cache: allocation failure keeps the original batch path.
        if (total <= lyra::media::kMaxTracks) {
            order->indices = static_cast<size_t *>(heap_caps_malloc(
                total * sizeof(size_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        }
        if (order->indices) {
            const size_t found = lyra::media::group_tracks(kind, group, 0, order->indices, total);
            const lyra::media::Status after = lyra::media::status();
            if (found != total || before.catalog_generation != after.catalog_generation ||
                before.sorting_generation != after.sorting_generation) {
                heap_caps_free(order->indices);
                order->indices = nullptr;
            }
        }
    }
    if (!order->indices) return lyra::media::group_tracks(kind, group, offset, output, capacity);
    const size_t count = std::min(capacity, total - offset);
    std::copy_n(order->indices + offset, count, output);
    return count;
}

struct ArtistDetailListContext {
    size_t artist_group;
    size_t entry_count;
    size_t loaded;
    ArtistDetailTab tab;
    DetailTrackOrder order;
};

void append_artist_detail_rows(lv_obj_t *body, ArtistDetailListContext *context)
{
    if (!body || !context || context->loaded >= context->entry_count) return;
    constexpr size_t kArtistDetailBatchSize = 8;
    const size_t start = context->loaded;
    const size_t batch = std::min(kArtistDetailBatchSize, context->entry_count - start);
    if (context->tab == ArtistDetailTab::Songs) {
        size_t indices[kArtistDetailBatchSize];
        const size_t found = detail_group_tracks(&context->order, lyra::media::GroupKind::Artist,
            context->artist_group, context->entry_count, start, indices, batch);
        for (size_t i = 0; i < found; ++i) {
            make_song_row(body, 48 + static_cast<int>(start + i) * 58, indices[i], 54);
        }
        context->loaded += found;
        if (context->loaded >= context->entry_count) {
            heap_caps_free(context->order.indices);
            context->order.indices = nullptr;
        }
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
        heap_caps_free(context->order.indices);
        lv_free(context);
        return;
    }
    // No need to walk every child for scroll_bottom once all rows are present.
    if (context->loaded >= context->entry_count) return;
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
        if (s_pending_scroll_restore_y > 0) {
            lv_obj_update_layout(body);
            while (context->loaded < context->entry_count &&
                   lv_obj_get_scroll_bottom(body) < s_pending_scroll_restore_y) {
                const size_t loaded = context->loaded;
                append_artist_detail_rows(body, context);
                if (context->loaded == loaded) break;
                lv_obj_update_layout(body);
            }
        }
        lv_obj_add_event_cb(body, artist_detail_list_event_cb, LV_EVENT_SCROLL, context);
        lv_obj_add_event_cb(body, artist_detail_list_event_cb, LV_EVENT_SCROLL_END, context);
        lv_obj_add_event_cb(body, artist_detail_list_event_cb, LV_EVENT_DELETE, context);
    }
}

struct AlbumListContext {
    size_t group_index;
    size_t track_count;
    size_t loaded;
    DetailTrackOrder order;
};

void append_album_song_rows(lv_obj_t *body, AlbumListContext *context)
{
    if (!body || !context || context->loaded >= context->track_count) return;
    constexpr size_t kAlbumBatchSize = 8;
    size_t indices[kAlbumBatchSize];
    const size_t found = detail_group_tracks(&context->order, lyra::media::GroupKind::Album,
        context->group_index, context->track_count, context->loaded, indices, kAlbumBatchSize);
    for (size_t i = 0; i < found; ++i) {
        if (is_zeno_theme() && !s_queue_add_mode && !s_playlist_add_mode) {
            make_zeno_album_song_row(body, 130 + static_cast<int>(context->loaded + i) * 44,
                indices[i], context->loaded + i + 1);
        } else {
            make_song_row(body, 130 + static_cast<int>(context->loaded + i) * 58, indices[i], 54);
        }
    }
    context->loaded += found;
    if (context->loaded >= context->track_count) {
        heap_caps_free(context->order.indices);
        context->order.indices = nullptr;
    }
}

void album_list_event_cb(lv_event_t *event)
{
    auto *context = static_cast<AlbumListContext *>(lv_event_get_user_data(event));
    if (!context) return;
    if (lv_event_get_code(event) == LV_EVENT_DELETE) {
        heap_caps_free(context->order.indices);
        lv_free(context);
        return;
    }
    // No need to walk every child for scroll_bottom once all rows are present.
    if (context->loaded >= context->track_count) return;
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
    make_header(is_zeno_theme() ? tr(lyra::i18n::StringId::Album) : album.name, View::LibraryAlbums, true);
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
    if (s_queue_add_mode) {
        lv_obj_t *queue_button = make_button(summary, 259, 80, 38, 34, kAccentDark, 5);
        lv_obj_t *queue_icon = make_label(queue_button, LV_SYMBOL_PLUS, kTextOnAccent);
        lv_obj_center(queue_icon);
        lv_obj_add_event_cb(queue_button, [](lv_event_t *event) {
            lv_event_stop_bubbling(event);
            show_queue_add_picker_for_album(reinterpret_cast<uintptr_t>(
                lv_event_get_user_data(event)));
        }, LV_EVENT_CLICKED, reinterpret_cast<void *>(s_selected_group));
    }

    auto *context = static_cast<AlbumListContext *>(lv_malloc_zeroed(sizeof(AlbumListContext)));
    if (context) {
        context->group_index = s_selected_group;
        context->track_count = album.track_count;
        append_album_song_rows(body, context);
        if (s_pending_scroll_restore_y > 0) {
            lv_obj_update_layout(body);
            while (context->loaded < context->track_count &&
                   lv_obj_get_scroll_bottom(body) < s_pending_scroll_restore_y) {
                const size_t loaded = context->loaded;
                append_album_song_rows(body, context);
                if (context->loaded == loaded) break;
                lv_obj_update_layout(body);
            }
        }
        lv_obj_add_event_cb(body, album_list_event_cb, LV_EVENT_SCROLL, context);
        lv_obj_add_event_cb(body, album_list_event_cb, LV_EVENT_SCROLL_END, context);
        lv_obj_add_event_cb(body, album_list_event_cb, LV_EVENT_DELETE, context);
    }
}

void playlist_cb(lv_event_t *event)
{
    push_navigation_state();
    s_selected_playlist = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    s_selected_playlist_is_smart = false;
    s_list_page = 0;
    s_playlist_manage_mode = false;
    render(View::PlaylistDetail);
}

void smart_playlist_cb(lv_event_t *event)
{
    push_navigation_state();
    s_selected_playlist_is_smart = true;
    s_selected_smart_playlist = static_cast<lyra::media::SmartPlaylistKind>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
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
    lv_obj_t *confirm = make_button(dialog, 150, 166, 116, 48, kDanger, 7);
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
    lv_obj_t *remove = make_button(row, 246, 11, 48, 36, kDanger, 5);
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
    lv_obj_t *remove = make_button(row, 246, 9, 48, 36, kDanger, 5);
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
    const bool smart_detail = detail && s_selected_playlist_is_smart;
    if (detail && !smart_detail && !lyra::media::playlist_at(s_selected_playlist, &selected)) {
        return;
    }
    const char *title = !detail ? tr(lyra::i18n::StringId::Playlists) : smart_detail ?
        smart_playlist_title(s_selected_smart_playlist) : selected.name;
    make_header(title, detail ? View::Playlists :
                (s_playlists_from_library ? View::Library : View::Menu), true, "");
    if (!detail || !smart_detail) {
        lv_obj_t *plus = make_button(s_screen, 220, 32, 44, 36, kBackground, 4);
        lv_obj_t *plus_label = make_label(plus, LV_SYMBOL_PLUS, kAccent);
        lv_obj_center(plus_label);
        if (detail) {
            lv_obj_add_event_cb(plus, open_playlist_add_cb, LV_EVENT_CLICKED, nullptr);
        } else {
            add_route(plus, View::PlaylistCreate);
        }
    }
    if (!detail) {
        lv_obj_t *manage = make_button(s_screen, 270, 32, 44, 36, kBackground, 4);
        lv_obj_t *manage_label = make_label(manage,
                                            s_playlist_manage_mode ? LV_SYMBOL_CLOSE : LV_SYMBOL_EDIT,
                                            s_playlist_manage_mode ? kTextSecondary : kAccent);
        lv_obj_center(manage_label);
        lv_obj_add_event_cb(manage, toggle_playlist_manage_cb, LV_EVENT_CLICKED, nullptr);
    }
    lv_obj_t *list = make_scroll_body(72);
    if (!detail) {
        const size_t normal_count = lyra::media::playlist_count();
        for (size_t i = 0; i < normal_count; ++i) {
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
        if (normal_count == 0) {
            lv_obj_t *empty = make_label(list, tr(lyra::i18n::StringId::NoPlaylistsYetCreate), kTextMuted);
            lv_obj_set_pos(empty, 12, 0);
        }

        const int smart_header_y = normal_count == 0 ? 34 : static_cast<int>(normal_count) * 62;
        lv_obj_t *smart_header = make_label(list, tr(lyra::i18n::StringId::SmartPlaylists), kTextMuted);
        lv_obj_set_pos(smart_header, 12, smart_header_y + 6);
        const size_t smart_count = lyra::media::smart_playlist_count();
        for (size_t i = 0; i < smart_count; ++i) {
            lyra::media::SmartPlaylist smart{};
            if (!lyra::media::smart_playlist_at(i, &smart)) continue;
            char subtitle[28];
            format_count(lyra::i18n::StringId::SongCount,
                         static_cast<uint32_t>(smart.track_count), subtitle, sizeof(subtitle));
            const int y = smart_header_y + 30 + static_cast<int>(i) * 62;
            lv_obj_t *row = make_row(list, y, nullptr, smart_playlist_title(smart.kind),
                                     subtitle, View::PlaylistDetail, 58);
            lv_obj_remove_event_cb(row, route_cb);
            lv_obj_add_event_cb(row, smart_playlist_cb, LV_EVENT_CLICKED,
                                reinterpret_cast<void *>(static_cast<uintptr_t>(smart.kind)));
        }
    } else {
        const size_t total = smart_detail ?
            lyra::media::smart_playlist_track_count(s_selected_smart_playlist) : selected.track_count;
        const size_t pages = (total + lyra::media::kTrackPageSize - 1) /
                             lyra::media::kTrackPageSize;
        if (pages && s_list_page >= pages) s_list_page = pages - 1;
        size_t indices[lyra::media::kTrackPageSize];
        const size_t count = smart_detail ?
            lyra::media::smart_playlist_tracks(s_selected_smart_playlist,
                s_list_page * lyra::media::kTrackPageSize, indices, lyra::media::kTrackPageSize) :
            lyra::media::playlist_tracks(s_selected_playlist,
                s_list_page * lyra::media::kTrackPageSize, indices, lyra::media::kTrackPageSize);
        for (size_t i = 0; i < count; ++i) {
            if (!smart_detail && s_playlist_manage_mode) {
                make_playlist_track_manage_row(list, static_cast<int>(i) * 58, indices[i]);
            } else {
                make_song_row(list, static_cast<int>(i) * 58, indices[i], 54, false,
                              smart_detail && s_selected_smart_playlist ==
                                  lyra::media::SmartPlaylistKind::MostPlayed);
            }
        }
        make_page_controls(list, static_cast<int>(count) * 58 + 4, total);
        if (count == 0) make_label(list, smart_detail ?
            tr(lyra::i18n::StringId::SmartPlaylistEmpty) :
            tr(lyra::i18n::StringId::PlaylistEmptyAdd), kTextMuted);
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
