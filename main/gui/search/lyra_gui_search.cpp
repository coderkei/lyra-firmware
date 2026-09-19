/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../lyra_gui_internal.h"

namespace lyra::gui::internal {

void update_search_label()
{
    if (s_search_label == nullptr) return;
    const bool creating = s_view == View::PlaylistCreate;
    const char *text = creating ? s_playlist_name : s_search_query;
    lv_label_set_text(s_search_label, text[0] == '\0' ?
                      (creating ? tr(lyra::i18n::StringId::PlaylistNamePlaceholder) :
                                   tr(lyra::i18n::StringId::SearchMusicPlaceholder)) : text);
    lv_obj_set_style_text_color(s_search_label, text[0] == '\0' ? kTextMuted : kTextPrimary, 0);
}

const char *search_category_name(lyra::media::SearchCategory category)
{
    switch (category) {
        case lyra::media::SearchCategory::Songs: return tr(lyra::i18n::StringId::Songs);
        case lyra::media::SearchCategory::Albums: return tr(lyra::i18n::StringId::Albums);
        case lyra::media::SearchCategory::Artists: return tr(lyra::i18n::StringId::Artists);
        case lyra::media::SearchCategory::Playlists: return tr(lyra::i18n::StringId::Playlists);
    }
    return tr(lyra::i18n::StringId::Songs);
}

const char *search_empty_message(lyra::media::SearchCategory category)
{
    switch (category) {
        case lyra::media::SearchCategory::Songs: return tr(lyra::i18n::StringId::NoMatchingSongs);
        case lyra::media::SearchCategory::Albums: return tr(lyra::i18n::StringId::NoMatchingAlbums);
        case lyra::media::SearchCategory::Artists: return tr(lyra::i18n::StringId::NoMatchingArtists);
        case lyra::media::SearchCategory::Playlists: return tr(lyra::i18n::StringId::NoPlaylistSongsMatch);
    }
    return tr(lyra::i18n::StringId::NoMatches);
}

void populate_search_results()
{
    if (!s_search_results) return;
    lv_obj_clean(s_search_results);
    if (!s_search_query[0]) {
        make_label(s_search_results, tr(lyra::i18n::StringId::TypeSearchThenTap), kTextMuted);
        return;
    }
    const lyra::media::SearchStatus status = lyra::media::search_status();
    if (status.running) {
        lv_obj_t *spinner = lv_spinner_create(s_search_results);
        lv_obj_set_size(spinner, 48, 48);
        lv_obj_align(spinner, LV_ALIGN_TOP_MID, 0, 12);
        lv_spinner_set_anim_params(spinner, 800, 250);
        lv_obj_set_style_arc_color(spinner, kDivider, LV_PART_MAIN);
        lv_obj_set_style_arc_color(spinner, kAccent, LV_PART_INDICATOR);
        char progress[48];
        format_u32_u32(lyra::i18n::StringId::SearchingProgress,
                       static_cast<uint32_t>(status.processed),
                       static_cast<uint32_t>(status.total), progress, sizeof(progress));
        s_search_progress_label = make_label(s_search_results, progress, kTextSecondary);
        lv_obj_align(s_search_progress_label, LV_ALIGN_TOP_MID, 0, 72);
        return;
    }
    if (s_search_error[0]) {
        make_label(s_search_results, s_search_error, kTextMuted);
        return;
    }
    if (!status.ready || std::strcmp(s_submitted_search_query, s_search_query) != 0 ||
        s_submitted_search_category != s_search_category) {
        char prompt[64];
        format_text(lyra::i18n::StringId::TapSearchToFind,
                    search_category_name(s_search_category), prompt, sizeof(prompt));
        make_label(s_search_results, prompt, kTextMuted);
        return;
    }
    const size_t page_size = search_page_size();
    lyra::media::SearchResult results[kSearchPageCapacity]{};
    size_t total = 0;
    size_t count = lyra::media::search_results(s_list_page * page_size, results, page_size, &total);
    const size_t pages = (total + page_size - 1) / page_size;
    if (pages && s_list_page >= pages) {
        s_list_page = pages - 1;
        count = lyra::media::search_results(s_list_page * page_size, results, page_size, &total);
    }
    for (size_t i = 0; i < count; ++i) {
        const int y = static_cast<int>(i) * 58;
        const lyra::media::SearchResult &result = results[i];
        if (s_search_category == lyra::media::SearchCategory::Songs) {
            make_song_row(s_search_results, y, result.track_index, 54, true);
        } else if (s_search_category == lyra::media::SearchCategory::Playlists) {
            make_search_playlist_row(s_search_results, y, result);
        } else {
            const lyra::media::GroupKind kind =
                s_search_category == lyra::media::SearchCategory::Albums ?
                lyra::media::GroupKind::Album : lyra::media::GroupKind::Artist;
            lyra::media::Group group{};
            if (!lyra::media::group_at(kind, result.group_index, &group)) continue;
            char subtitle[32];
            format_count(lyra::i18n::StringId::SongCount,
                         static_cast<uint32_t>(group.track_count), subtitle, sizeof(subtitle));
            const View target = kind == lyra::media::GroupKind::Album ?
                                View::AlbumDetail : View::TrackList;
            lv_obj_t *row = make_row(s_search_results, y, nullptr, group.name,
                                     subtitle, target, 54);
            lv_obj_remove_event_cb(row, route_cb);
            lv_obj_add_event_cb(row,
                kind == lyra::media::GroupKind::Album ? search_album_result_cb : search_artist_result_cb,
                LV_EVENT_CLICKED, reinterpret_cast<void *>(result.group_index));
        }
    }
    make_page_controls(s_search_results, static_cast<int>(count) * 58 + 4, total, page_size);
    if (count == 0) make_label(s_search_results, search_empty_message(s_search_category), kTextMuted);
}

void search_category_cb(lv_event_t *event)
{
    const auto category = static_cast<lyra::media::SearchCategory>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    if (category == s_search_category) return;
    s_search_category = category;
    s_list_page = 0;
    s_search_error[0] = '\0';
    const lyra::media::SearchStatus status = lyra::media::search_status();
    if (s_search_query[0] && !status.running) {
        const esp_err_t result = lyra::media::start_search(s_search_category, s_search_query);
        if (result == ESP_OK) {
            copy_ui_text(s_submitted_search_query, sizeof(s_submitted_search_query), s_search_query);
            s_submitted_search_category = s_search_category;
        } else {
            copy_ui_text(s_search_error, sizeof(s_search_error),
                         tr(lyra::i18n::StringId::SearchBusy));
        }
    }
    render(View::Search);
}

void keyboard_cb(lv_event_t *event)
{
    const char *key = static_cast<const char *>(lv_event_get_user_data(event));
    char *text = s_view == View::PlaylistCreate ? s_playlist_name : s_search_query;
    const size_t capacity = s_view == View::PlaylistCreate ? sizeof(s_playlist_name) : sizeof(s_search_query);
    size_t length = std::strlen(text);
    if (std::strcmp(key, tr(lyra::i18n::StringId::SearchKey)) == 0) {
        s_list_page = 0;
        s_search_error[0] = '\0';
        const esp_err_t result = lyra::media::start_search(s_search_category, s_search_query);
        if (result == ESP_OK) {
            copy_ui_text(s_submitted_search_query, sizeof(s_submitted_search_query), s_search_query);
            s_submitted_search_category = s_search_category;
        }
        else copy_ui_text(s_search_error, sizeof(s_search_error),
                          tr(lyra::i18n::StringId::SearchBusy));
        render(View::Search);
        return;
    }
    if (std::strcmp(key, tr(lyra::i18n::StringId::SaveKey)) == 0) {
        size_t created = 0;
        if (lyra::media::create_playlist(s_playlist_name, &created) == ESP_OK) {
            s_selected_playlist = created;
            s_playlist_name[0] = '\0';
            render(View::PlaylistDetail);
        }
        return;
    }
    if (std::strcmp(key, tr(lyra::i18n::StringId::NumbersKey)) == 0 ||
        std::strcmp(key, tr(lyra::i18n::StringId::LettersKey)) == 0) {
        s_library_keyboard_symbols = !s_library_keyboard_symbols;
        render(s_view);
        return;
    }
    if (std::strcmp(key, "<") == 0) {
        if (length > 0) text[length - 1] = '\0';
    } else if (std::strcmp(key, tr(lyra::i18n::StringId::SpaceKey)) == 0) {
        if (length + 1 < capacity) {
            text[length] = ' ';
            text[length + 1] = '\0';
        }
    } else if (length + 1 < capacity) {
        text[length] = key[0];
        text[length + 1] = '\0';
    }
    update_search_label();
    // Searching 10,000 MicroSD-backed records is intentionally explicit. This
    // keeps typing responsive and turns each query into one sequential read.
}

void toggle_search_keyboard_cb(lv_event_t *)
{
    s_search_keyboard_visible = !s_search_keyboard_visible;
    if (!s_search_keyboard || !s_search_results || !s_search_keyboard_toggle) return;
    if (s_search_keyboard_visible) {
        lv_obj_remove_flag(s_search_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(s_search_results,
                          content_height(kStatusHeight) - library_keyboard_height() - 84);
        lv_label_set_text(s_search_keyboard_toggle, tr(lyra::i18n::StringId::HideKeyboard));
    } else {
        lv_obj_add_flag(s_search_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(s_search_results, content_height(kStatusHeight) - 84);
        lv_label_set_text(s_search_keyboard_toggle, tr(lyra::i18n::StringId::ShowKeyboard));
    }
    lv_obj_invalidate(s_search_results);
}

void make_keyboard_row(lv_obj_t *parent, int y, const char *keys[], size_t count, int inset,
                       int height)
{
    int gap = 3;
    int width = (320 - inset * 2 - gap * static_cast<int>(count - 1)) / static_cast<int>(count);
    for (size_t i = 0; i < count; ++i) {
        lv_obj_t *key = make_button(parent, inset + static_cast<int>(i) * (width + gap), y, width, height,
                                    kSurfaceRaised, 5);
        lv_obj_t *label = make_label(key, keys[i], kTextPrimary);
        lv_obj_center(label);
        lv_obj_add_event_cb(key, keyboard_cb, LV_EVENT_CLICKED, const_cast<char *>(keys[i]));
    }
}

void make_library_keyboard(lv_obj_t *keyboard, const char *action)
{
    const char *letters_row1[] = {"q", "w", "e", "r", "t", "y", "u", "i", "o", "p"};
    const char *letters_row2[] = {"a", "s", "d", "f", "g", "h", "j", "k", "l"};
    const char *letters_row3[] = {"z", "x", "c", "v", "b", "n", "m", "<"};
    const char *symbols_row1[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"};
    const char *symbols_row2[] = {"@", "#", "$", "_", "&", "-", "+", "(", ")"};
    const char *symbols_row3[] = {".", ",", "?", "!", "'", "\"", "=", "<"};
    const char *row4[] = {s_library_keyboard_symbols ? tr(lyra::i18n::StringId::LettersKey) :
                                                         tr(lyra::i18n::StringId::NumbersKey),
                          tr(lyra::i18n::StringId::SpaceKey), action};
    const char **keys_row1 = s_library_keyboard_symbols ? symbols_row1 : letters_row1;
    const char **keys_row2 = s_library_keyboard_symbols ? symbols_row2 : letters_row2;
    const char **keys_row3 = s_library_keyboard_symbols ? symbols_row3 : letters_row3;
    // Keep the keys immediately above the dock (or display edge).  Anchoring
    // the rows from the bottom prevents empty space below them when the
    // virtual navigation setting changes.
    const int key_height =
        s_show_nav ? kKeyboardKeyHeightWithNav : kKeyboardKeyHeightWithoutNav;
    const int rows_height = key_height * 4 + kKeyboardRowGap * 3;
    const int first_row_y = library_keyboard_height() - kKeyboardBottomPadding - rows_height;
    make_keyboard_row(keyboard, first_row_y, keys_row1, 10, 3, key_height);
    make_keyboard_row(keyboard, first_row_y + (key_height + kKeyboardRowGap), keys_row2, 9, 16,
                      key_height);
    make_keyboard_row(keyboard, first_row_y + (key_height + kKeyboardRowGap) * 2, keys_row3, 8, 24,
                      key_height);
    make_keyboard_row(keyboard, first_row_y + (key_height + kKeyboardRowGap) * 3, row4, 3, 3,
                      key_height);
}

void render_search()
{
    lv_obj_t *body = make_box(s_screen, 0, kStatusHeight, 320, content_height(kStatusHeight), kBackground);
    lv_obj_t *input = make_box(body, 9, 4, 233, 43, kSurfaceRaised, 7);
    lv_obj_t *magnifier = make_label(input, LV_SYMBOL_EDIT, kTextSecondary);
    lv_obj_align(magnifier, LV_ALIGN_LEFT_MID, 10, 0);
    s_search_label = make_label(input, "", kTextMuted);
    make_marquee(s_search_label, 180);
    lv_obj_align(s_search_label, LV_ALIGN_LEFT_MID, 37, 0);
    update_search_label();
    lv_obj_t *toggle = make_button(body, 248, 4, 63, 43, kSurfaceRaised, 7);
    s_search_keyboard_toggle = make_label(toggle, s_search_keyboard_visible ?
                                          tr(lyra::i18n::StringId::HideKeyboard) :
                                          tr(lyra::i18n::StringId::ShowKeyboard), kAccent);
    lv_obj_center(s_search_keyboard_toggle);
    lv_obj_add_event_cb(toggle, toggle_search_keyboard_cb, LV_EVENT_CLICKED, nullptr);
    constexpr lyra::media::SearchCategory categories[] = {
        lyra::media::SearchCategory::Songs,
        lyra::media::SearchCategory::Albums,
        lyra::media::SearchCategory::Artists,
        lyra::media::SearchCategory::Playlists,
    };
    for (size_t i = 0; i < sizeof(categories) / sizeof(categories[0]); ++i) {
        const char *labels[] = {tr(lyra::i18n::StringId::Songs),
                                tr(lyra::i18n::StringId::Albums),
                                tr(lyra::i18n::StringId::Artists),
                                tr(lyra::i18n::StringId::Playlists)};
        lv_obj_t *tab = make_button(body, 7 + static_cast<int>(i) * 77, 52, 72, 28,
                                    categories[i] == s_search_category ? kAccentDark : kSurfaceRaised, 5);
        lv_obj_t *tab_label = make_label(tab, labels[i],
                                         categories[i] == s_search_category ? kTextOnAccent : kTextSecondary);
        lv_obj_center(tab_label);
        lv_obj_add_event_cb(tab, search_category_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<uintptr_t>(categories[i])));
    }
    const int keyboard_height = library_keyboard_height();
    const int keyboard_y = content_height(kStatusHeight) - keyboard_height;
    const int results_height = s_search_keyboard_visible ? keyboard_y - 84 :
        content_height(kStatusHeight) - 84;
    s_search_results = make_box(body, 0, 84, 320, results_height, kBackground);
    configure_cover_aware_scroll(s_search_results);
    populate_search_results();
    s_search_keyboard = make_box(body, 0, keyboard_y, 320, keyboard_height, kKeyboardSurface);
    lv_obj_t *keyboard = s_search_keyboard;
    make_library_keyboard(keyboard, tr(lyra::i18n::StringId::SearchKey));
    if (!s_search_keyboard_visible) lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
}

void render_playlist_create()
{
    make_header(tr(lyra::i18n::StringId::NewPlaylist), View::Playlists, true);
    lv_obj_t *body = make_box(s_screen, 0, 72, 320, content_height(72), kBackground);
    lv_obj_t *input = make_box(body, 9, 8, 302, 43, kSurfaceRaised, 7);
    s_search_label = make_label(input, "", kTextMuted);
    lv_obj_align(s_search_label, LV_ALIGN_LEFT_MID, 12, 0);
    update_search_label();
    const int keyboard_height = library_keyboard_height();
    const int keyboard_y = content_height(72) - keyboard_height;
    lv_obj_t *keyboard = make_box(body, 0, keyboard_y, 320, keyboard_height, kKeyboardSurface);
    make_library_keyboard(keyboard, tr(lyra::i18n::StringId::SaveKey));
}

void add_playlist_track_cb(lv_event_t *event)
{
    const size_t track_index = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    if (lyra::media::add_to_playlist(s_selected_playlist, track_index) == ESP_OK) {
        while (s_navigation_depth) {
            const NavigationState state = s_navigation[--s_navigation_depth];
            if (state.view == View::PlaylistDetail) {
                restore_navigation_state(state);
                return;
            }
        }
        render(View::PlaylistDetail);
    }
}

void render_playlist_add()
{
    s_playlist_add_mode = true;
    render_library();
}

} // namespace lyra::gui::internal
