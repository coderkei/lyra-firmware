/* SPDX-License-Identifier: Apache-2.0 */
#include "../lyra_gui_internal.h"

namespace lyra::gui::internal {
char s_folder_playlist_path[lyra::media::kMaxPath]{};

void open_folder_playlist(const char *path)
{
    // Capture the old selection before replacing it, including nested history.
    push_navigation_state();
    copy_ui_text(s_folder_playlist_path, sizeof(s_folder_playlist_path), path);
    s_list_page = 0;
    render(View::FolderPlaylist);
}

void render_folder_playlist()
{
    const char *name = std::strrchr(s_folder_playlist_path, '/');
    make_header(name ? name + 1 : s_folder_playlist_path, View::Folders, true);
    lv_obj_t *list = make_scroll_body(72);
    auto *indices = static_cast<size_t *>(heap_caps_malloc(
        lyra::media::kMaxTracks * sizeof(size_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!indices) {
        make_label(list, tr(lyra::i18n::StringId::FileMemoryError), kTextMuted);
        return;
    }
    size_t count = 0;
    const esp_err_t result = lyra::media::load_playlist_file(s_folder_playlist_path,
        indices, lyra::media::kMaxTracks, &count);
    if (result != ESP_OK || !count) {
        heap_caps_free(indices);
        make_label(list, tr(result == ESP_OK ? lyra::i18n::StringId::PlaylistNoPlayableFiles :
            result == ESP_ERR_INVALID_SIZE ? lyra::i18n::StringId::PlaylistTooManyFiles :
                                            lyra::i18n::StringId::FileReadFailed), kTextMuted);
        return;
    }
    // The list owns its temporary path IDs. Playback makes its own editable
    // queue copy before navigation destroys this screen.
    lv_obj_set_user_data(list, reinterpret_cast<void *>(uintptr_t(count)));
    lv_obj_add_event_cb(list, [](lv_event_t *event) {
        heap_caps_free(lv_event_get_user_data(event));
    }, LV_EVENT_DELETE, indices);
    const size_t pages = (count + lyra::media::kTrackPageSize - 1) / lyra::media::kTrackPageSize;
    s_list_page = std::min(s_list_page, pages - 1);
    const size_t first = s_list_page * lyra::media::kTrackPageSize;
    const size_t last = std::min(count, first + lyra::media::kTrackPageSize);
    int y = 0;
    for (size_t position = first; position < last; ++position, y += 58) {
        lyra::media::Track track{};
        if (!lyra::media::track_at(indices[position], &track)) {
            lv_obj_t *error = make_label(list, tr(lyra::i18n::StringId::FileOpenFailed), kTextMuted);
            lv_obj_set_pos(error, 12, y + 16);
            continue;
        }
        lv_obj_t *row = make_button(list, 7, y, 306, 54, kSurface, 5, true);
        lv_obj_t *title = make_label(row, track.title, kTextPrimary);
        make_marquee(title, s_queue_add_mode ? 244 : 282);
        lv_obj_align(title, LV_ALIGN_LEFT_MID, 12, -9);
        lv_obj_t *artist = make_label(row, track.artist, kTextSecondary);
        make_marquee(artist, s_queue_add_mode ? 244 : 282);
        lv_obj_align(artist, LV_ALIGN_LEFT_MID, 12, 11);
        if (s_queue_add_mode) {
            lv_obj_t *plus = make_label(row, LV_SYMBOL_PLUS, kAccent);
            lv_obj_align(plus, LV_ALIGN_RIGHT_MID, -12, 0);
            lv_obj_clear_flag(plus, LV_OBJ_FLAG_CLICKABLE);
        }
        lv_obj_set_user_data(row, reinterpret_cast<void *>(uintptr_t(position)));
        lv_obj_add_event_cb(row, [](lv_event_t *event) {
            lv_event_stop_bubbling(event);
            lv_obj_t *row = lv_event_get_current_target_obj(event);
            const size_t position = reinterpret_cast<uintptr_t>(lv_obj_get_user_data(row));
            lv_obj_t *list = lv_obj_get_parent(row);
            const size_t count = reinterpret_cast<uintptr_t>(lv_obj_get_user_data(list));
            const auto *indices = static_cast<const size_t *>(lv_event_get_user_data(event));
            if (s_queue_add_mode) { show_queue_add_picker_for_track(indices[position]); return; }
            if (play_file_queue(indices, count, position)) navigate_to(View::Player);
            else show_notice(tr(lyra::i18n::StringId::Playlists), tr(lyra::i18n::StringId::FileOpenFailed));
        }, LV_EVENT_CLICKED, indices);
    }
    make_page_controls(list, y + 4, count);
}
} // namespace lyra::gui::internal
