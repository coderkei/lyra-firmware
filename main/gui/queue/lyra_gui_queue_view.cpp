/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../lyra_gui_internal.h"

#include <strings.h>

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
    lv_obj_t *clear = make_button(dialog, 150, 146, 116, 40, kDanger, 7);
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
    lv_obj_t *button = make_button(header, 274, 2, 40, 40,
        s_queue_edit_mode ? kAccent : kSurfaceRaised, LV_RADIUS_CIRCLE);
    if (!s_queue_edit_mode) lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
    lv_obj_t *icon = make_label(button,
        s_queue_edit_mode ? LV_SYMBOL_CLOSE : LV_SYMBOL_EDIT,
        s_queue_edit_mode ? kTextOnAccent : kTextPrimary);
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
    auto *row = static_cast<lv_obj_t *>(lv_event_get_user_data(event));
    const size_t position = reinterpret_cast<uintptr_t>(lv_obj_get_user_data(row));
    const auto action = static_cast<QueueRowAction>(
        reinterpret_cast<uintptr_t>(lv_obj_get_user_data(lv_event_get_current_target_obj(event))));
    bool changed = false;
    if (action == QueueRowAction::MoveUp) changed = move_queue_entry(position, -1);
    else if (action == QueueRowAction::MoveDown) changed = move_queue_entry(position, 1);
    else if (action == QueueRowAction::Remove) changed = remove_queue_entry(position);
    if (!changed) return;
    if (s_scroll_body) s_pending_scroll_restore_y = lv_obj_get_scroll_y(s_scroll_body);
    render(View::Queue);
}

void add_queue_row_action(lv_obj_t *row, int x, const char *symbol, lv_color_t color,
                          QueueRowAction action)
{
    const bool remove = action == QueueRowAction::Remove;
    lv_obj_t *button = make_button(row, x, 10, 30, 34,
                                   remove ? kDanger : kSurfaceRaised, 5);
    lv_obj_t *icon = make_label(button, symbol, remove ? kTextOnAccent : color);
    lv_obj_center(icon);
    lv_obj_set_user_data(button, reinterpret_cast<void *>(static_cast<uintptr_t>(action)));
    lv_obj_add_event_cb(button, queue_row_action_cb, LV_EVENT_CLICKED, row);
}

void add_queue_toolbar_button(lv_obj_t *parent, int x, const char *text,
                              lv_event_cb_t callback)
{
    lv_obj_t *button = make_button(parent, x, 0, 146, 40, kSurface, LV_RADIUS_CIRCLE);
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
    size_t current = 0;
    // Open with the playing song near the top, one row of context above it.
    if (current_queue_position(&current)) s_pending_list_position = current ? current - 1 : 0;
    render(View::Queue);
}

bool show_now_playing()
{
    const lyra::audio::Status audio_status = lyra::audio::status();
    if (::strncasecmp(audio_status.path, "http://", 7) == 0 ||
        ::strncasecmp(audio_status.path, "https://", 8) == 0) {
        navigate_to(View::RadioPlaying);
        return true;
    }
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
        // Re-read the card on entry; edits made elsewhere then show up.
        invalidate_file_listing();
        navigate_to(View::Folders);
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
    lv_obj_t *row = make_button(parent, kPageInset, y, kScreenWidth - 2 * kPageInset, 54,
        current ? kAccentSurface : kSurface, kRowRadius);
    char queue_number_text[16];
    format_u32(lyra::i18n::StringId::QueueNumber,
               static_cast<uint32_t>(queue_position + 1),
               queue_number_text, sizeof(queue_number_text));
    lv_obj_t *queue_number = make_label(row, queue_number_text, kTextMuted);
    lv_obj_set_width(queue_number, 40);
    lv_obj_set_style_text_align(queue_number, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(queue_number, LV_ALIGN_LEFT_MID, 0, 0);
    if (current) {
        lv_obj_t *playing = make_label(row, LV_SYMBOL_PLAY, kAccent);
        lv_obj_align(playing, LV_ALIGN_LEFT_MID, 14, 0);
        lv_obj_clear_flag(playing, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(queue_number, LV_OBJ_FLAG_HIDDEN);
    }
    const int text_x = 42;
    lv_obj_t *title = make_label(row, track.title, current ? kAccent : kTextPrimary);
    const int text_width = s_queue_edit_mode ? 132 : 250;
    make_marquee(title, text_width);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, text_x, -9);
    lv_obj_t *artist = make_label(row, track.artist, kTextSecondary);
    make_marquee(artist, text_width);
    lv_obj_align(artist, LV_ALIGN_LEFT_MID, text_x, 11);
    if (s_queue_edit_mode) {
        const lv_color_t up_color = queue_position > 0 ? kTextSecondary : kTextMuted;
        const lv_color_t down_color = queue_position + 1 < playback_queue_count() ?
                                      kTextSecondary : kTextMuted;
        lv_obj_set_user_data(row, reinterpret_cast<void *>(queue_position));
        add_queue_row_action(row, 188, LV_SYMBOL_UP, up_color, QueueRowAction::MoveUp);
        add_queue_row_action(row, 220, LV_SYMBOL_DOWN, down_color, QueueRowAction::MoveDown);
        add_queue_row_action(row, 252, LV_SYMBOL_TRASH, kDangerSurface, QueueRowAction::Remove);
    }
    lv_obj_add_event_cb(row, queue_track_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(queue_position));
}

namespace {

// The queue is frozen into this snapshot for the prefetch task, which must
// not read the live queue while the GUI edits it. Any edit re-renders.
size_t *s_queue_snapshot;
size_t s_queue_snapshot_count;
portMUX_TYPE s_queue_snapshot_lock = portMUX_INITIALIZER_UNLOCKED;
size_t s_queue_snapshot_current = SIZE_MAX;

void release_queue_snapshot(lv_event_t *)
{
    portENTER_CRITICAL(&s_queue_snapshot_lock);
    size_t *snapshot = s_queue_snapshot;
    s_queue_snapshot = nullptr;
    s_queue_snapshot_count = 0;
    portEXIT_CRITICAL(&s_queue_snapshot_lock);
    heap_caps_free(snapshot);
}

bool fetch_queue_row(size_t position, VirtualRow *row)
{
    portENTER_CRITICAL(&s_queue_snapshot_lock);
    const size_t track = position < s_queue_snapshot_count ? s_queue_snapshot[position] : SIZE_MAX;
    portEXIT_CRITICAL(&s_queue_snapshot_lock);
    lyra::media::TrackSummary summary{};
    if (track == SIZE_MAX || !lyra::media::track_summary_at(track, &summary)) {
        return false;
    }
    row->payload = position;
    copy_ui_text(row->title, sizeof(row->title), summary.title);
    copy_ui_text(row->subtitle, sizeof(row->subtitle), summary.artist);
    return true;
}

size_t queue_snapshot_track(size_t position)
{
    portENTER_CRITICAL(&s_queue_snapshot_lock);
    const size_t track = position < s_queue_snapshot_count ? s_queue_snapshot[position] : SIZE_MAX;
    portEXIT_CRITICAL(&s_queue_snapshot_lock);
    return track;
}

void bind_queue_thumbnail(lv_obj_t *art, size_t position)
{
    const size_t track = queue_snapshot_track(position);
    if (track != SIZE_MAX) bind_row_thumbnail(art, lyra::media::ThumbnailSource::Track, track);
}

void refresh_queue_thumbnail(VirtualRowWidgets &widgets)
{
    bind_queue_thumbnail(widgets.art, reinterpret_cast<uintptr_t>(lv_obj_get_user_data(widgets.button)));
}

void request_queue_thumbnails(const size_t *positions, size_t count)
{
    size_t tracks[32];
    size_t found = 0;
    for (size_t i = 0; i < count && found < sizeof(tracks) / sizeof(tracks[0]); ++i) {
        const size_t track = queue_snapshot_track(positions[i]);
        if (track != SIZE_MAX) tracks[found++] = track;
    }
    request_row_thumbnails(lyra::media::ThumbnailSource::Track, tracks, found);
}

void build_queue_row(lv_obj_t *slot, VirtualRowWidgets &widgets, size_t position,
                     const VirtualRow &data, int width)
{
    const bool current = position == s_queue_snapshot_current;
    const bool can_move_up = s_queue_edit_mode && position > 0;
    const bool can_move_down = s_queue_edit_mode && position + 1 < playback_queue_count();
    const unsigned variant = (current ? 1u : 0u) | (can_move_up ? 2u : 0u) | (can_move_down ? 4u : 0u);
    char queue_number_text[16];
    format_u32(lyra::i18n::StringId::QueueNumber, static_cast<uint32_t>(position + 1),
               queue_number_text, sizeof(queue_number_text));
    if (widgets.button && widgets.variant == variant) {
        rebind_virtual_row(widgets, data);
        set_label_text_if_changed(widgets.number, queue_number_text);
        bind_queue_thumbnail(widgets.art, position);
        return;
    }
    if (widgets.button) lv_obj_delete(widgets.button);
    lv_obj_t *row = make_button(slot, kPageInset, 0, width, 54,
        current ? kAccentSurface : kSurface, kRowRadius);
    lv_obj_set_user_data(row, reinterpret_cast<void *>(position));
    lv_obj_t *queue_number = make_label(row, queue_number_text, kTextMuted);
    lv_obj_set_width(queue_number, 40);
    lv_label_set_long_mode(queue_number, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_align(queue_number, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(queue_number, LV_ALIGN_LEFT_MID, 0, 0);
    if (current) {
        lv_obj_t *playing = make_label(row, LV_SYMBOL_PLAY, kAccent);
        lv_obj_align(playing, LV_ALIGN_LEFT_MID, 14, 0);
        lv_obj_add_flag(queue_number, LV_OBJ_FLAG_HIDDEN);
    }
    // Queue number, then the optional cover, then the text.
    constexpr int kNumberWidth = 42;
    lv_obj_t *art = s_list_artwork ?
        make_row_thumbnail(row, kNumberWidth, (54 - kRowThumbnailSize) / 2) : nullptr;
    const int text_x = s_list_artwork ? kNumberWidth + kRowThumbnailSize + kRowThumbnailInset :
                                        kNumberWidth;
    // Edit mode keeps the move and remove buttons from x = 188.
    const int text_width = s_queue_edit_mode ? 188 - 4 - text_x : width - text_x - 14;
    lv_obj_t *title = make_label(row, data.title, current ? kAccent : kTextPrimary);
    make_marquee(title, text_width);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, text_x, -9);
    lv_obj_t *artist = make_label(row, data.subtitle, kTextSecondary);
    make_marquee(artist, text_width);
    lv_obj_align(artist, LV_ALIGN_LEFT_MID, text_x, 11);
    widgets = {row, title, artist, variant, queue_number};
    widgets.art = art;
    bind_queue_thumbnail(art, position);
    if (s_queue_edit_mode) {
        add_queue_row_action(row, 188, LV_SYMBOL_UP, can_move_up ? kTextSecondary : kTextMuted,
                             QueueRowAction::MoveUp);
        add_queue_row_action(row, 220, LV_SYMBOL_DOWN, can_move_down ? kTextSecondary : kTextMuted,
                             QueueRowAction::MoveDown);
        add_queue_row_action(row, 252, LV_SYMBOL_TRASH, kDangerSurface, QueueRowAction::Remove);
    }
    lv_obj_add_event_cb(row, [](lv_event_t *event) {
        lv_obj_t *button = lv_event_get_current_target_obj(event);
        play_queue_position(reinterpret_cast<uintptr_t>(lv_obj_get_user_data(button)), false);
    }, LV_EVENT_CLICKED, nullptr);
}

} // namespace

void render_queue()
{
    const size_t count = s_has_active_queue ? playback_queue_count() : 0;
    lv_obj_t *header = make_header(tr(lyra::i18n::StringId::Queue), View::Player, true, "");
    make_queue_edit_mode_toggle(header);
    lv_obj_t *list = make_scroll_body(72);
    const int rows_top = s_queue_edit_mode ? 48 : 0;
    if (s_queue_edit_mode) {
        add_queue_toolbar_button(list, kPageInset, tr(lyra::i18n::StringId::Clear), clear_queue_cb);
        add_queue_toolbar_button(list, kScreenWidth - kPageInset - 146,
                                 tr(lyra::i18n::StringId::SaveQueueAsPlaylist),
                                 save_queue_as_playlist_cb);
    }
    if (count == 0) {
        lv_obj_t *empty = make_label(list, tr(lyra::i18n::StringId::NoActiveQueue), kTextMuted);
        lv_obj_set_pos(empty, 12, rows_top + 2);
        return;
    }
    const size_t snapshot_count = std::min(count, lyra::media::kMaxTracks);
    auto *snapshot = static_cast<size_t *>(heap_caps_malloc(
        snapshot_count * sizeof(size_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!snapshot) {
        make_label(list, tr(lyra::i18n::StringId::FileMemoryError), kTextMuted);
        return;
    }
    copy_queue_tracks(snapshot, snapshot_count);
    portENTER_CRITICAL(&s_queue_snapshot_lock);
    s_queue_snapshot = snapshot;
    s_queue_snapshot_count = snapshot_count;
    portEXIT_CRITICAL(&s_queue_snapshot_lock);
    lv_obj_add_event_cb(list, release_queue_snapshot, LV_EVENT_DELETE, nullptr);
    s_queue_snapshot_current = SIZE_MAX;
    current_queue_position(&s_queue_snapshot_current);

    VirtualListSpec spec{};
    spec.count = snapshot_count;
    spec.top = rows_top;
    spec.pitch = 58;
    spec.row_height = 54;
    spec.fetch = fetch_queue_row;
    spec.build = build_queue_row;
    enable_row_thumbnails(spec, request_queue_thumbnails, refresh_queue_thumbnail);
    make_virtual_list(list, spec);
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
