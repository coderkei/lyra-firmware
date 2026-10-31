/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../lyra_gui_internal.h"
#include <initializer_list>
#include "lyra_gui_draw.h"

namespace lyra::gui::internal {
namespace {

// Each icon is a short list of primitives on a 20 x 20 grid, scaled to the
// requested size at draw time. LVGL's symbol font lacks a magnifier, globe,
// disc, person and similar, so these keep every menu meaning what it shows.
enum class Part : uint8_t { Line, Ring, Disc, Box, Frame, Triangle, Arc };
struct IconPart {
    Part kind;
    uint8_t width;
    int16_t v[6];
};

#define L(x1, y1, x2, y2) {Part::Line, 2, {x1, y1, x2, y2, 0, 0}}
#define LW(w, x1, y1, x2, y2) {Part::Line, w, {x1, y1, x2, y2, 0, 0}}
#define R(cx, cy, r) {Part::Ring, 2, {cx, cy, r, 0, 0, 0}}
#define D(cx, cy, r) {Part::Disc, 0, {cx, cy, r, 0, 0, 0}}
#define B(x, y, w, h, radius) {Part::Box, 0, {x, y, w, h, radius, 0}}
#define F(x, y, w, h, radius) {Part::Frame, 2, {x, y, w, h, radius, 0}}
#define T(x1, y1, x2, y2, x3, y3) {Part::Triangle, 0, {x1, y1, x2, y2, x3, y3}}
#define A(cx, cy, r, start, end) {Part::Arc, 2, {cx, cy, r, start, end, 0}}
#define AW(w, cx, cy, r, start, end) {Part::Arc, w, {cx, cy, r, start, end, 0}}

constexpr IconPart kMusic[] = {
    D(5, 15, 3), D(14, 13, 3), L(7, 15, 7, 4), L(16, 13, 16, 2), LW(3, 7, 4, 16, 2),
};
constexpr IconPart kNowPlaying[] = {R(10, 10, 9), T(8, 6, 8, 14, 15, 10)};
constexpr IconPart kSearch[] = {R(8, 8, 6), LW(3, 12, 12, 18, 18)};
constexpr IconPart kQueue[] = {
    L(2, 4, 14, 4), L(2, 9, 14, 9), L(2, 14, 8, 14), T(11, 11, 11, 19, 18, 15),
};
constexpr IconPart kApps[] = {
    B(2, 2, 7, 7, 2), B(11, 2, 7, 7, 2), B(2, 11, 7, 7, 2), B(11, 11, 7, 7, 2),
};
constexpr IconPart kFolder[] = {B(2, 3, 7, 4, 1), F(2, 6, 16, 12, 2)};
constexpr IconPart kEqualizer[] = {
    L(4, 2, 4, 18), L(10, 2, 10, 18), L(16, 2, 16, 18),
    B(1, 11, 7, 4, 1), B(7, 4, 7, 4, 1), B(13, 8, 7, 4, 1),
};
constexpr IconPart kSettings[] = {
    R(10, 10, 5), LW(3, 10, 1, 10, 4), LW(3, 10, 16, 10, 19), LW(3, 1, 10, 4, 10),
    LW(3, 16, 10, 19, 10), LW(3, 4, 4, 6, 6), LW(3, 14, 14, 16, 16),
    LW(3, 4, 16, 6, 14), LW(3, 14, 6, 16, 4),
};
constexpr IconPart kPlayback[] = {T(5, 3, 5, 17, 17, 10)};
constexpr IconPart kSound[] = {
    B(2, 7, 4, 6, 0), T(5, 10, 11, 3, 11, 17), A(11, 10, 4, 300, 60), A(11, 10, 8, 305, 55),
};
constexpr IconPart kDisplay[] = {
    D(10, 10, 4), L(10, 1, 10, 3), L(10, 17, 10, 19), L(1, 10, 3, 10), L(17, 10, 19, 10),
    L(4, 4, 5, 5), L(15, 15, 16, 16), L(4, 16, 5, 15), L(15, 5, 16, 4),
};
constexpr IconPart kSorting[] = {
    L(5, 4, 5, 18), T(1, 7, 9, 7, 5, 2), L(15, 2, 15, 16), T(11, 13, 19, 13, 15, 18),
};
constexpr IconPart kSystem[] = {
    F(5, 5, 10, 10, 1), B(8, 8, 4, 4, 0),
    L(8, 1, 8, 4), L(12, 1, 12, 4), L(8, 16, 8, 19), L(12, 16, 12, 19),
    L(1, 8, 4, 8), L(1, 12, 4, 12), L(16, 8, 19, 8), L(16, 12, 19, 12),
};
constexpr IconPart kAbout[] = {R(10, 10, 9), L(10, 9, 10, 15), B(9, 4, 3, 3, 1)};
constexpr IconPart kTheme[] = {
    R(10, 10, 9), D(6, 8, 2), D(10, 5, 2), D(14, 8, 2), D(13, 13, 2),
};
constexpr IconPart kLanguage[] = {
    R(10, 10, 9), L(2, 10, 18, 10), A(3, 10, 10, 315, 45), A(17, 10, 10, 135, 225),
};
constexpr IconPart kClock[] = {R(10, 10, 9), L(10, 10, 10, 5), L(10, 10, 14, 12)};
constexpr IconPart kWifi[] = {D(10, 16, 2), A(10, 17, 7, 225, 315), A(10, 17, 13, 228, 312)};
constexpr IconPart kSdCard[] = {
    L(8, 2, 16, 2), L(16, 2, 16, 18), L(16, 18, 4, 18), L(4, 18, 4, 6), L(4, 6, 8, 2),
    LW(1, 8, 5, 8, 8), LW(1, 11, 5, 11, 8), LW(1, 14, 5, 14, 8),
};
constexpr IconPart kStorage[] = {F(3, 2, 14, 16, 4), L(3, 8, 17, 8), L(3, 13, 17, 13)};
constexpr IconPart kSleep[] = {A(9, 10, 8, 50, 310), A(16, 10, 6, 107, 253), B(16, 2, 2, 2, 1)};
constexpr IconPart kCrossfade[] = {
    L(1, 15, 7, 15), L(7, 15, 13, 5), L(13, 5, 19, 5),
    L(1, 5, 7, 5), L(7, 5, 13, 15), L(13, 15, 19, 15),
};
constexpr IconPart kRadio[] = {
    F(2, 7, 16, 11, 2), R(7, 12, 2), L(12, 10, 15, 10), L(12, 14, 15, 14), L(5, 7, 14, 2),
};
constexpr IconPart kAlbum[] = {R(10, 10, 9), D(10, 10, 2)};
constexpr IconPart kArtist[] = {D(10, 6, 4), B(3, 12, 14, 7, 5)};
constexpr IconPart kGenre[] = {
    L(2, 2, 10, 2), L(10, 2, 18, 10), L(18, 10, 10, 18), L(10, 18, 2, 10), L(2, 10, 2, 2),
    B(5, 5, 3, 3, 1),
};
constexpr IconPart kCalendar[] = {
    F(2, 4, 16, 14, 2), L(2, 8, 18, 8), L(6, 2, 6, 5), L(14, 2, 14, 5),
    B(5, 11, 3, 3, 0), B(9, 11, 3, 3, 0), B(13, 11, 3, 3, 0),
};
constexpr IconPart kPlaylist[] = {
    L(2, 4, 12, 4), L(2, 9, 12, 9), L(2, 14, 8, 14), D(13, 16, 2), L(15, 16, 15, 6),
    L(15, 6, 19, 8),
};
constexpr IconPart kFavorite[] = {D(6, 7, 4), D(14, 7, 4), T(2, 8, 18, 8, 10, 18)};
constexpr IconPart kRecent[] = {
    A(10, 10, 8, 300, 200), T(0, 9, 6, 9, 3, 4), L(10, 10, 10, 5), L(10, 10, 13, 12),
};
constexpr IconPart kAdded[] = {LW(3, 10, 3, 10, 17), LW(3, 3, 10, 17, 10)};
constexpr IconPart kCalculator[] = {
    F(3, 1, 14, 18, 2), B(6, 4, 8, 4, 0), B(6, 11, 2, 2, 0), B(9, 11, 2, 2, 0),
    B(12, 11, 2, 2, 0), B(6, 14, 2, 2, 0), B(9, 14, 2, 2, 0), B(12, 14, 2, 2, 0),
};
constexpr IconPart kStopwatch[] = {
    R(10, 11, 7), L(10, 11, 10, 7), L(8, 1, 12, 1), L(10, 1, 10, 4), L(16, 4, 17, 5),
};
constexpr IconPart kTimer[] = {
    L(4, 2, 16, 2), L(4, 18, 16, 18), L(5, 2, 15, 18), L(15, 2, 5, 18), T(7, 17, 13, 17, 10, 13),
};
constexpr IconPart kBook[] = {
    L(10, 4, 10, 17), L(1, 3, 10, 4), L(1, 3, 1, 16), L(1, 16, 10, 17),
    L(19, 3, 10, 4), L(19, 3, 19, 16), L(19, 16, 10, 17),
};
constexpr IconPart kPhoto[] = {
    F(1, 3, 18, 14, 2), T(3, 15, 8, 8, 12, 15), T(9, 15, 13, 10, 17, 15), D(14, 7, 2),
};
constexpr IconPart kNetwork[] = {
    D(10, 4, 2), D(4, 16, 2), D(16, 16, 2), L(10, 4, 4, 16), L(10, 4, 16, 16), L(4, 16, 16, 16),
};

#undef L
#undef LW
#undef R
#undef D
#undef B
#undef F
#undef T
#undef A
#undef AW

struct IconShape {
    const IconPart *parts;
    size_t count;
};

template <size_t N>
constexpr IconShape shape(const IconPart (&parts)[N]) { return {parts, N}; }

IconShape icon_shape(Icon icon)
{
    switch (icon) {
        case Icon::Music: return shape(kMusic);
        case Icon::NowPlaying: return shape(kNowPlaying);
        case Icon::Search: return shape(kSearch);
        case Icon::Queue: return shape(kQueue);
        case Icon::Apps: return shape(kApps);
        case Icon::Folder: return shape(kFolder);
        case Icon::Equalizer: return shape(kEqualizer);
        case Icon::Settings: return shape(kSettings);
        case Icon::Playback: return shape(kPlayback);
        case Icon::Sound: return shape(kSound);
        case Icon::Display: return shape(kDisplay);
        case Icon::Sorting: return shape(kSorting);
        case Icon::System: return shape(kSystem);
        case Icon::About: return shape(kAbout);
        case Icon::Theme: return shape(kTheme);
        case Icon::Language: return shape(kLanguage);
        case Icon::Clock: return shape(kClock);
        case Icon::Wifi: return shape(kWifi);
        case Icon::SdCard: return shape(kSdCard);
        case Icon::Storage: return shape(kStorage);
        case Icon::Sleep: return shape(kSleep);
        case Icon::Crossfade: return shape(kCrossfade);
        case Icon::Radio: return shape(kRadio);
        case Icon::Album: return shape(kAlbum);
        case Icon::Artist: return shape(kArtist);
        case Icon::Genre: return shape(kGenre);
        case Icon::Calendar: return shape(kCalendar);
        case Icon::Playlist: return shape(kPlaylist);
        case Icon::Favorite: return shape(kFavorite);
        case Icon::Recent: return shape(kRecent);
        case Icon::Added: return shape(kAdded);
        case Icon::Calculator: return shape(kCalculator);
        case Icon::Stopwatch: return shape(kStopwatch);
        case Icon::Timer: return shape(kTimer);
        case Icon::Book: return shape(kBook);
        case Icon::Photo: return shape(kPhoto);
        case Icon::Network: return shape(kNetwork);
    }
    return shape(kMusic);
}

void draw_icon_cb(lv_event_t *event)
{
    lv_obj_t *object = lv_event_get_current_target_obj(event);
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_area_t area;
    lv_obj_get_coords(object, &area);
    const int size = lv_area_get_width(&area);
    // Pressed-state styles (Night City) and theme overrides resolve here, so
    // icons always follow the colour of the text beside them.
    const lv_color_t color = lv_obj_get_style_text_color(object, LV_PART_MAIN);
    const IconShape icon = icon_shape(static_cast<Icon>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(event))));
    const auto px = [&](int value) { return value * size / 20; };
    const auto x = [&](int value) { return static_cast<int>(area.x1) + px(value); };
    const auto y = [&](int value) { return static_cast<int>(area.y1) + px(value); };
    const auto stroke = [&](int width) { return std::max(1, (width * size + 10) / 20); };
    for (size_t i = 0; i < icon.count; ++i) {
        const IconPart &part = icon.parts[i];
        const int16_t *v = part.v;
        switch (part.kind) {
            case Part::Line: {
                lv_draw_line_dsc_t line;
                lv_draw_line_dsc_init(&line);
                line.p1 = {static_cast<lv_value_precise_t>(x(v[0])), static_cast<lv_value_precise_t>(y(v[1]))};
                line.p2 = {static_cast<lv_value_precise_t>(x(v[2])), static_cast<lv_value_precise_t>(y(v[3]))};
                line.color = color;
                line.width = stroke(part.width);
                line.round_start = line.round_end = true;
                gui_draw_line(layer, &line);
                break;
            }
            case Part::Ring:
            case Part::Arc: {
                lv_draw_arc_dsc_t arc;
                lv_draw_arc_dsc_init(&arc);
                arc.center = {x(v[0]), y(v[1])};
                arc.radius = static_cast<uint16_t>(std::max(1, px(v[2])));
                arc.start_angle = part.kind == Part::Ring ? 0 : v[3];
                arc.end_angle = part.kind == Part::Ring ? 360 : v[4];
                arc.width = stroke(part.width);
                arc.color = color;
                arc.rounded = part.kind == Part::Arc;
                gui_draw_arc(layer, &arc);
                break;
            }
            case Part::Disc:
            case Part::Box:
            case Part::Frame: {
                lv_area_t bounds;
                if (part.kind == Part::Disc) {
                    const int radius = std::max(1, px(v[2]));
                    bounds = {x(v[0]) - radius, y(v[1]) - radius, x(v[0]) + radius, y(v[1]) + radius};
                } else {
                    bounds = {x(v[0]), y(v[1]), x(v[0] + v[2]) - 1, y(v[1] + v[3]) - 1};
                }
                if (!gui_draw_area_visible(layer, bounds)) break;
                lv_draw_rect_dsc_t rect;
                lv_draw_rect_dsc_init(&rect);
                rect.radius = part.kind == Part::Disc ? LV_RADIUS_CIRCLE : px(v[4]);
                rect.bg_color = color;
                rect.bg_opa = part.kind == Part::Frame ? LV_OPA_TRANSP : LV_OPA_COVER;
                rect.border_color = color;
                rect.border_width = part.kind == Part::Frame ? stroke(part.width) : 0;
                rect.border_opa = LV_OPA_COVER;
                lv_draw_rect(layer, &rect, &bounds);
                break;
            }
            case Part::Triangle: {
                const lv_area_t bounds{x(std::min({v[0], v[2], v[4]})), y(std::min({v[1], v[3], v[5]})),
                                       x(std::max({v[0], v[2], v[4]})), y(std::max({v[1], v[3], v[5]}))};
                if (!gui_draw_area_visible(layer, bounds)) break;
                lv_draw_triangle_dsc_t triangle;
                lv_draw_triangle_dsc_init(&triangle);
                for (int point = 0; point < 3; ++point) {
                    triangle.p[point] = {static_cast<lv_value_precise_t>(x(v[point * 2])),
                                         static_cast<lv_value_precise_t>(y(v[point * 2 + 1]))};
                }
                triangle.color = color;
                triangle.opa = LV_OPA_COVER;
                lv_draw_triangle(layer, &triangle);
                break;
            }
        }
    }
}

} // namespace

lv_obj_t *make_icon(lv_obj_t *parent, Icon icon, lv_color_t color, int size)
{
    // A bare object (no theme styles) holds only its colour; drawing happens
    // in one callback, so an icon costs about the same as the label it replaces.
    lv_obj_t *object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_size(object, size, size);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(object, kIconFlag);
    lv_obj_set_style_text_color(object, themed_icon_color(parent, color), 0);
    lv_obj_add_event_cb(object, draw_icon_cb, LV_EVENT_DRAW_MAIN,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(icon)));
    return object;
}

} // namespace lyra::gui::internal
