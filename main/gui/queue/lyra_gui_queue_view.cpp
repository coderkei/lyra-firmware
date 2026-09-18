/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../lyra_gui_internal.h"

namespace lyra::gui::internal {

void open_queue_cb(lv_event_t *)
{
    if (s_view == View::Queue) return;
    if (!s_has_active_queue && !restore_saved_queue_if_pending()) {
        show_notice("Queue empty", "Select a song to begin playback\nand build a queue.");
        return;
    }
    push_navigation_state();
    const size_t page_size = library_page_size();
    size_t current = 0;
    s_list_page = current_queue_position(&current) ? current / page_size : 0;
    render(View::Queue);
}

bool show_now_playing()
{
    if (!s_has_active_queue && !restore_saved_queue_if_pending()) {
        show_notice("Queue empty", "Select a song to begin playback\nbefore opening Now Playing.");
        return false;
    }
    navigate_to(View::Player);
    return true;
}

void open_library_cb(lv_event_t *)
{
    const lyra::media::Status status = lyra::media::status();
    if (!status.mounted) {
        show_notice("No MicroSD card", "Insert a MicroSD card to use\nthe music library.");
    } else if (status.track_count == 0) {
        show_notice("No scanned songs", "Use Settings > System > Scan\nmusic library first.");
    } else {
        navigate_to(View::Library);
    }
}

void open_folders_cb(lv_event_t *)
{
    if (!lyra::media::status().mounted) {
        show_notice("No MicroSD card", "Insert a MicroSD card to browse\nits folders.");
    } else {
        navigate_to(View::Folders);
    }
}

void render_menu()
{
    make_header("Menu", View::Menu);
    lv_obj_t *body = make_scroll_body(72);
    struct MenuItem { const char *icon; const char *label; View view; };
    constexpr MenuItem items[] = {
        {LV_SYMBOL_AUDIO, "Now Playing", View::Player},
        {LV_SYMBOL_LIST, "Music Library", View::Library},
        {LV_SYMBOL_DIRECTORY, "Browse Folders", View::Folders},
        {LV_SYMBOL_LIST, "Queue", View::Queue},
        {LV_SYMBOL_SETTINGS, "Equalizer", View::Equalizer},
        {LV_SYMBOL_EDIT, "Search", View::Search},
        {LV_SYMBOL_SETTINGS, "Settings", View::Settings},
    };
    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); ++i) {
        lv_obj_t *row = make_row(body, static_cast<int>(i) * 56, items[i].icon, items[i].label,
                                 nullptr, items[i].view, 51);
        if (items[i].view == View::Queue) {
            lv_obj_remove_event_cb(row, route_cb);
            lv_obj_add_event_cb(row, open_queue_cb, LV_EVENT_CLICKED, nullptr);
        } else if (items[i].view == View::Player) {
            lv_obj_remove_event_cb(row, route_cb);
            lv_obj_add_event_cb(row, [](lv_event_t *) { show_now_playing(); },
                                LV_EVENT_CLICKED, nullptr);
        } else if (items[i].view == View::Library) {
            lv_obj_remove_event_cb(row, route_cb);
            lv_obj_add_event_cb(row, open_library_cb, LV_EVENT_CLICKED, nullptr);
        } else if (items[i].view == View::Folders) {
            lv_obj_remove_event_cb(row, route_cb);
            lv_obj_add_event_cb(row, open_folders_cb, LV_EVENT_CLICKED, nullptr);
        }
    }
}

void queue_track_cb(lv_event_t *event)
{
    play_queue_position(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
}

void make_queue_song_row(lv_obj_t *parent, int y, size_t queue_position, bool current)
{
    size_t track_index;
    lyra::media::Track track{};
    if (!queue_track_at(queue_position, &track_index) || !lyra::media::track_at(track_index, &track)) return;
    lv_obj_t *row = make_button(parent, 7, y, 306, 54, current ? kAccentDark : kSurface, 5);
    const int text_x = current ? 45 : 12;
    if (current) {
        lv_obj_t *playing = make_label(row, LV_SYMBOL_PLAY, kTextOnAccent);
        lv_obj_align(playing, LV_ALIGN_LEFT_MID, 12, 0);
        lv_obj_clear_flag(playing, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_t *title = make_label(row, track.title, current ? kTextOnAccent : kTextPrimary);
    make_marquee(title, current ? 220 : 282);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, text_x, -9);
    lv_obj_t *artist = make_label(row, track.artist, current ? kTextOnAccent : kTextSecondary);
    make_marquee(artist, current ? 220 : 282);
    lv_obj_align(artist, LV_ALIGN_LEFT_MID, text_x, 11);
    lv_obj_add_event_cb(row, queue_track_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(queue_position));
}

void render_queue()
{
    const size_t count = s_has_active_queue ? playback_queue_count() : 0;
    char count_label[24];
    std::snprintf(count_label, sizeof(count_label), "%u TRACKS", static_cast<unsigned>(count));
    make_header("Queue", View::Menu, true, count_label);
    lv_obj_t *list = make_scroll_body(72);
    if (count == 0) {
        make_label(list, "No active queue. Select a song to begin playback.", kTextMuted);
        return;
    }

    const size_t page_size = library_page_size();
    const size_t pages = (count + page_size - 1) / page_size;
    if (s_list_page >= pages) s_list_page = pages - 1;
    size_t current = count;
    current_queue_position(&current);
    const size_t first = s_list_page * page_size;
    const size_t shown = std::min(page_size, count - first);
    for (size_t i = 0; i < shown; ++i) {
        make_queue_song_row(list, static_cast<int>(i) * 58, first + i, first + i == current);
    }
    make_page_controls(list, static_cast<int>(shown) * 58 + 4, count, page_size);
}

View library_section_view(LibraryTab section)
{
    switch (section) {
        case LibraryTab::Songs: return View::LibrarySongs;
        case LibraryTab::Artists: return View::LibraryArtists;
        case LibraryTab::Albums: return View::LibraryAlbums;
        case LibraryTab::Genres: return View::LibraryGenres;
        case LibraryTab::Years: return View::LibraryYears;
    }
    return View::Library;
}

lyra::media::GroupKind group_kind(LibraryTab section)
{
    switch (section) {
        case LibraryTab::Artists: return lyra::media::GroupKind::Artist;
        case LibraryTab::Albums: return lyra::media::GroupKind::Album;
        case LibraryTab::Genres: return lyra::media::GroupKind::Genre;
        case LibraryTab::Years: return lyra::media::GroupKind::Year;
        case LibraryTab::Songs: break;
    }
    return lyra::media::GroupKind::Artist;
}

} // namespace lyra::gui::internal
