/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../lyra_gui_internal.h"

namespace lyra::gui::internal {

void format_playback_time(uint32_t milliseconds, bool unknown, char *output, size_t capacity)
{
    if (!output || capacity == 0) return;
    if (unknown) {
        copy_ui_text(output, capacity, "--:--");
        return;
    }
    const uint32_t total_seconds = milliseconds / 1000;
    const uint32_t hours = total_seconds / 3600;
    const uint32_t minutes = (total_seconds / 60) % 60;
    const uint32_t seconds = total_seconds % 60;
    if (hours > 0) {
        std::snprintf(output, capacity, "%u:%02u:%02u",
                      static_cast<unsigned>(hours), static_cast<unsigned>(minutes),
                      static_cast<unsigned>(seconds));
    } else {
        std::snprintf(output, capacity, "%02u:%02u",
                      static_cast<unsigned>(minutes), static_cast<unsigned>(seconds));
    }
}

bool text_equals_ci(const char *left, const char *right)
{
    if (!left || !right) return false;
    while (*left && *right) {
        if (std::tolower(static_cast<unsigned char>(*left)) !=
            std::tolower(static_cast<unsigned char>(*right))) return false;
        ++left;
        ++right;
    }
    return *left == '\0' && *right == '\0';
}

const char *current_track_group_name(const lyra::media::Track &track,
                                     lyra::media::GroupKind kind)
{
    switch (kind) {
        case lyra::media::GroupKind::Artist: return track.artist;
        case lyra::media::GroupKind::Album: return track.album;
        case lyra::media::GroupKind::Genre: return track.genre;
        case lyra::media::GroupKind::Year: return track.year;
    }
    return "";
}

void open_current_track_group(lyra::media::GroupKind kind)
{
    lyra::media::Track track{};
    if (!lyra::media::track_at(s_current_track, &track)) return;
    const char *name = current_track_group_name(track, kind);
    if (!name || !name[0]) return;

    const size_t count = lyra::media::group_count(kind);
    for (size_t index = 0; index < count; ++index) {
        lyra::media::Group group{};
        if (!lyra::media::group_at(kind, index, &group) || std::strcmp(group.name, name) != 0) continue;
        s_selected_group_kind = kind;
        s_selected_group = index;
        copy_ui_text(s_track_list_title, sizeof(s_track_list_title), group.name);
        switch (kind) {
            case lyra::media::GroupKind::Album:
                s_library_tab = LibraryTab::Albums;
                navigate_to(View::AlbumDetail);
                return;
            case lyra::media::GroupKind::Artist:
                s_library_tab = LibraryTab::Artists;
                s_artist_detail_tab = ArtistDetailTab::Songs;
                navigate_to(View::ArtistDetail);
                return;
            case lyra::media::GroupKind::Genre:
                s_library_tab = LibraryTab::Genres;
                navigate_to(View::TrackList);
                return;
            case lyra::media::GroupKind::Year:
                s_library_tab = LibraryTab::Years;
                navigate_to(View::TrackList);
                return;
        }
    }
}

void open_current_track_group_cb(lv_event_t *event)
{
    const auto kind = static_cast<lyra::media::GroupKind>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    open_current_track_group(kind);
}

void track_info_tab_cb(lv_event_t *event)
{
    s_track_info_tab = static_cast<TrackInfoTab>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    render(View::TrackInfo);
}

lv_obj_t *make_track_info_row(lv_obj_t *parent, int y, const char *label,
                              const char *value, bool clickable)
{
    lv_obj_t *row = clickable ? make_button(parent, 8, y, 304, 52, kSurface, 6, true) :
                                make_box(parent, 8, y, 304, 52, kSurface, 6, true);
    if (clickable) lv_obj_set_style_bg_color(row, kSurfaceRaised, LV_STATE_PRESSED);
    lv_obj_t *label_view = make_label(row, label, kTextMuted);
    lv_obj_set_pos(label_view, 12, 6);
    lv_obj_t *value_view = make_label(row, value && value[0] ? value : "Unavailable",
                                      clickable ? kAccent : kTextPrimary);
    make_marquee(value_view, clickable ? 252 : 278);
    lv_obj_set_pos(value_view, 12, 25);
    if (clickable) {
        lv_obj_t *arrow = make_label(row, LV_SYMBOL_RIGHT, kAccent);
        lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -12, 8);
        lv_obj_clear_flag(arrow, LV_OBJ_FLAG_CLICKABLE);
    }
    return row;
}

void make_track_info_tab_button(lv_obj_t *parent, int x, const char *label,
                                TrackInfoTab tab)
{
    const bool selected = s_track_info_tab == tab;
    lv_obj_t *button = make_button(parent, x, 76, 150, 36,
                                   selected ? kAccentDark : kSurface, 6, true);
    lv_obj_t *text = make_label(button, label, selected ? kTextOnAccent : kTextSecondary);
    lv_obj_center(text);
    lv_obj_add_event_cb(button, track_info_tab_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(tab)));
}

void format_track_number(uint32_t number, char *output, size_t capacity)
{
    if (!output || capacity == 0) return;
    if (number == 0) copy_ui_text(output, capacity, "Unavailable");
    else std::snprintf(output, capacity, "%u", static_cast<unsigned>(number));
}

void format_file_size(uint64_t bytes, char *output, size_t capacity)
{
    if (!output || capacity == 0) return;
    if (bytes < 1024u) std::snprintf(output, capacity, "%llu B",
                                    static_cast<unsigned long long>(bytes));
    else if (bytes < 1024u * 1024u) std::snprintf(output, capacity, "%.1f KB",
                                                   bytes / 1024.0);
    else if (bytes < 1024ull * 1024ull * 1024ull) std::snprintf(output, capacity, "%.2f MB",
                                                                 bytes / (1024.0 * 1024.0));
    else std::snprintf(output, capacity, "%.2f GB", bytes / (1024.0 * 1024.0 * 1024.0));
}

} // namespace lyra::gui::internal
