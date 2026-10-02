/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../lyra_gui_internal.h"

namespace lyra::gui::internal {

namespace {

enum class QueueRowAction : uintptr_t { MoveUp, MoveDown, Remove };

void close_queue_confirmation(lv_event_t *event)
{
    lv_event_stop_bubbling(event);
    lv_obj_t *overlay = static_cast<lv_obj_t *>(lv_event_get_user_data(event));
    if (overlay) lv_obj_delete(overlay);
}

void confirm_clear_queue_cb(lv_event_t *event)
{
    lv_event_stop_bubbling(event);
    clear_active_queue();
    render(View::Queue);
}

void show_clear_queue_confirmation()
{
    lv_obj_t *overlay = make_box(s_screen, 0, 0, kScreenWidth, kScreenHeight, kOverlay);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_90, 0);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(overlay);
    lv_obj_add_event_cb(overlay, [](lv_event_t *event) {
        if (lv_event_get_target_obj(event) == lv_event_get_current_target_obj(event)) {
            lv_obj_delete(lv_event_get_current_target_obj(event));
        }
    }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *dialog = make_box(overlay, 20, 140, 280, 200, kSurfaceRaised, 12);
    lv_obj_t *title = make_label(dialog, tr(lyra::i18n::StringId::ClearQueueQuestion),
                                 kTextPrimary);
    lv_obj_set_width(title, 248);
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 18);
    lv_obj_t *message = make_label(dialog, tr(lyra::i18n::StringId::ClearQueueMessage),
                                   kTextSecondary);
    lv_obj_set_width(message, 248);
    lv_label_set_long_mode(message, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(message, LV_ALIGN_CENTER, 0, -4);

    lv_obj_t *cancel = make_button(dialog, 14, 146, 116, 40, kSurface, 7);
    lv_obj_t *cancel_label = make_label(cancel, tr(lyra::i18n::StringId::Cancel),
                                        kTextSecondary);
    lv_obj_center(cancel_label);
    lv_obj_add_event_cb(cancel, close_queue_confirmation, LV_EVENT_CLICKED, overlay);
    lv_obj_t *clear = make_button(dialog, 150, 146, 116, 40, kDangerSurface, 7);
    lv_obj_t *clear_label = make_label(clear, tr(lyra::i18n::StringId::Clear), kTextOnAccent);
    lv_obj_center(clear_label);
    lv_obj_add_event_cb(clear, confirm_clear_queue_cb, LV_EVENT_CLICKED, nullptr);
}

void clear_queue_cb(lv_event_t *)
{
    show_clear_queue_confirmation();
}

void toggle_queue_edit_mode_cb(lv_event_t *)
{
    s_queue_edit_mode = !s_queue_edit_mode;
    render(View::Queue);
}

void make_queue_edit_mode_toggle(lv_obj_t *header)
{
    lv_obj_t *button = make_button(header, 274, 4, 40, 36,
        s_queue_edit_mode ? kAccentDark : kBackground, 5);
    lv_obj_t *icon = make_label(button,
        s_queue_edit_mode ? LV_SYMBOL_CLOSE : LV_SYMBOL_EDIT,
        s_queue_edit_mode ? kTextOnAccent : kTextSecondary);
    lv_obj_center(icon);
    lv_obj_add_event_cb(button, [](lv_event_t *event) {
        lv_event_stop_bubbling(event);
        toggle_queue_edit_mode_cb(event);
    }, LV_EVENT_CLICKED, nullptr);
}

void save_queue_as_playlist_cb(lv_event_t *)
{
    if (!s_has_active_queue || playback_queue_count() == 0) return;
    s_playlist_create_from_queue = true;
    s_playlist_name[0] = '\0';
    navigate_to(View::PlaylistCreate);
}

void queue_row_action_cb(lv_event_t *event)
{
    lv_event_stop_bubbling(event);
    const uintptr_t action_value = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    const size_t position = static_cast<size_t>(action_value >> 2);
    const QueueRowAction action = static_cast<QueueRowAction>(action_value & 0x3u);
    bool changed = false;
    if (action == QueueRowAction::MoveUp) changed = move_queue_entry(position, -1);
    else if (action == QueueRowAction::MoveDown) changed = move_queue_entry(position, 1);
    else if (action == QueueRowAction::Remove) changed = remove_queue_entry(position);
    if (changed) render(View::Queue);
}

void add_queue_row_action(lv_obj_t *row, int x, const char *symbol, lv_color_t color,
                          size_t position, QueueRowAction action)
{
    const bool remove = action == QueueRowAction::Remove;
    lv_obj_t *button = make_button(row, x, 10, 30, 34,
                                   remove ? lv_color_hex(0x991B1B) : kSurfaceRaised, 5);
    lv_obj_t *icon = make_label(button, symbol, remove ? kTextOnAccent : color);
    lv_obj_center(icon);
    const uintptr_t payload = (static_cast<uintptr_t>(position) << 2) |
                              static_cast<uintptr_t>(action);
    lv_obj_add_event_cb(button, queue_row_action_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(payload));
}

void add_queue_toolbar_button(lv_obj_t *parent, int x, const char *text,
                              lv_event_cb_t callback)
{
    lv_obj_t *button = make_button(parent, x, 0, 148, 40, kSurfaceRaised, 6, true);
    lv_obj_t *label = make_label(button, text, kAccent);
    make_marquee(label, 132);
    lv_obj_center(label);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, nullptr);
}

} // namespace

void open_queue_cb(lv_event_t *)
{
    if (s_view == View::Queue) return;
    if (!s_has_active_queue && !restore_saved_queue_if_pending()) {
        show_notice(tr(lyra::i18n::StringId::QueueEmpty),
                    tr(lyra::i18n::StringId::QueueEmptyBuild));
        return;
    }
    push_navigation_state();
    s_queue_edit_mode = false;
    const size_t page_size = library_page_size();
    size_t current = 0;
    s_list_page = current_queue_position(&current) ? current / page_size : 0;
    render(View::Queue);
}

bool show_now_playing()
{
    if (!s_has_active_queue && !restore_saved_queue_if_pending()) {
        show_notice(tr(lyra::i18n::StringId::QueueEmpty),
                    tr(lyra::i18n::StringId::QueueEmptyNowPlaying));
        return false;
    }
    resume_saved_track_if_pending();
    navigate_to(View::Player);
    return true;
}

void open_library_cb(lv_event_t *)
{
    const lyra::media::Status status = lyra::media::status();
    if (!status.mounted) {
        show_notice(tr(lyra::i18n::StringId::NoMicroSdCard),
                    tr(lyra::i18n::StringId::InsertMicroSdLibrary));
    } else if (status.track_count == 0) {
        show_notice(tr(lyra::i18n::StringId::NoScannedSongsTitle),
                    tr(lyra::i18n::StringId::NoMusicScanned));
    } else {
        navigate_to(View::Library);
    }
}

void open_folders_cb(lv_event_t *)
{
    if (!lyra::media::status().mounted) {
        show_notice(tr(lyra::i18n::StringId::NoMicroSdCard),
                    tr(lyra::i18n::StringId::InsertMicroSdFolders));
    } else {
        navigate_to(View::Folders);
    }
}

void render_menu()
{
    if (is_zeno_theme()) { render_zeno_menu(); return; }
    make_header(tr(lyra::i18n::StringId::Menu), View::Menu);
    lv_obj_t *body = make_scroll_body(72);
    struct MenuItem { const char *icon; const char *label; View view; };
    const MenuItem items[] = {
        {LV_SYMBOL_AUDIO, tr(lyra::i18n::StringId::NowPlaying), View::Player},
        {LV_SYMBOL_LIST, tr(lyra::i18n::StringId::MusicLibrary), View::Library},
        {LV_SYMBOL_DIRECTORY, tr(lyra::i18n::StringId::BrowseFolders), View::Folders},
        {LV_SYMBOL_LIST, tr(lyra::i18n::StringId::Queue), View::Queue},
        {LV_SYMBOL_SETTINGS, tr(lyra::i18n::StringId::Equalizer), View::Equalizer},
        {LV_SYMBOL_EDIT, tr(lyra::i18n::StringId::Search), View::Search},
        {LV_SYMBOL_SETTINGS, tr(lyra::i18n::StringId::Settings), View::Settings},
    };
    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); ++i) {
        lv_obj_t *row = make_row(body, static_cast<int>(i) * 56, items[i].icon, items[i].label,
                                 nullptr, items[i].view, 51);
        if (is_cute_theme()) {
            constexpr uint32_t pastel_rows[] = {
                0xFFD6ED, 0xF3EDFF, 0xEDF7FF, 0xEDF9F5,
                0xF6EEFF, 0xEEF5FF, 0xFFF0F7,
            };
            lv_obj_set_style_bg_color(row, lv_color_hex(pastel_rows[i]), 0);
            // make_row's first child is its stock font icon. Keep the label
            // geometry and replace only that decoration in Cute & Pink.
            lv_obj_add_flag(lv_obj_get_child(row, 0), LV_OBJ_FLAG_HIDDEN);
            if (items[i].view == View::Player) {
                lv_obj_set_style_border_color(row, kAccent, 0);
                lv_obj_set_style_text_color(lv_obj_get_child(row, 1), kAccent, 0);
            }
            make_cute_menu_icon(row, items[i].view);
        }
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
    play_queue_position(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)), false);
}

void make_queue_song_row(lv_obj_t *parent, int y, size_t queue_position, bool current)
{
    size_t track_index;
    lyra::media::Track track{};
    if (!queue_track_at(queue_position, &track_index) || !lyra::media::track_at(track_index, &track)) return;
    lv_obj_t *row = make_button(parent, 7, y, 306, 54,
        current ? (!is_standard_theme() ? kAccentSurface : kAccentDark) : kSurface, 5);
    char queue_number_text[16];
    format_u32(lyra::i18n::StringId::QueueNumber,
               static_cast<uint32_t>(queue_position + 1),
               queue_number_text, sizeof(queue_number_text));
    lv_obj_t *queue_number = make_label(row, queue_number_text,
                                        current ? kTextOnAccent : kTextMuted);
    lv_obj_set_width(queue_number, 40);
    lv_obj_set_style_text_align(queue_number, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(queue_number, LV_ALIGN_LEFT_MID, 0, 0);
    if (current) {
        lv_obj_t *playing = make_label(row, LV_SYMBOL_PLAY, !is_standard_theme() ? kAccent : kTextOnAccent);
        lv_obj_align(playing, LV_ALIGN_LEFT_MID, 10, 0);
        lv_obj_clear_flag(playing, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(queue_number, LV_OBJ_FLAG_HIDDEN);
    }
    const int text_x = 42;
    lv_obj_t *title = make_label(row, track.title,
        current ? (!is_standard_theme() ? kAccent : kTextOnAccent) : kTextPrimary);
    const int text_width = s_queue_edit_mode ? 132 : 250;
    make_marquee(title, text_width);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, text_x, -9);
    lv_obj_t *artist = make_label(row, track.artist,
        current && is_standard_theme() ? kTextOnAccent : kTextSecondary);
    make_marquee(artist, text_width);
    lv_obj_align(artist, LV_ALIGN_LEFT_MID, text_x, 11);
    if (s_queue_edit_mode) {
        const lv_color_t up_color = queue_position > 0 ? kTextSecondary : kTextMuted;
        const lv_color_t down_color = queue_position + 1 < playback_queue_count() ?
                                      kTextSecondary : kTextMuted;
        add_queue_row_action(row, 188, LV_SYMBOL_UP, up_color, queue_position,
                             QueueRowAction::MoveUp);
        add_queue_row_action(row, 220, LV_SYMBOL_DOWN, down_color, queue_position,
                             QueueRowAction::MoveDown);
        add_queue_row_action(row, 252, LV_SYMBOL_TRASH, kDangerSurface, queue_position,
                             QueueRowAction::Remove);
    }
    lv_obj_add_event_cb(row, queue_track_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(queue_position));
}

void render_queue()
{
    const size_t count = s_has_active_queue ? playback_queue_count() : 0;
    lv_obj_t *header = make_header(tr(lyra::i18n::StringId::Queue), View::Menu, true, "");
    make_queue_edit_mode_toggle(header);
    lv_obj_t *list = make_scroll_body(72);
    const int rows_top = s_queue_edit_mode ? 48 : 0;
    if (s_queue_edit_mode) {
        add_queue_toolbar_button(list, 7, tr(lyra::i18n::StringId::Clear), clear_queue_cb);
        add_queue_toolbar_button(list, 165,
                                 tr(lyra::i18n::StringId::SaveQueueAsPlaylist),
                                 save_queue_as_playlist_cb);
    }
    if (count == 0) {
        lv_obj_t *empty = make_label(list, tr(lyra::i18n::StringId::NoActiveQueue), kTextMuted);
        lv_obj_set_pos(empty, 12, rows_top + 2);
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
        make_queue_song_row(list, rows_top + static_cast<int>(i) * 58, first + i,
                            first + i == current);
    }
    make_page_controls(list, rows_top + static_cast<int>(shown) * 58 + 4, count, page_size);
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
