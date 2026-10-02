/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../lyra_gui_internal.h"
#include <initializer_list>
#include "src/misc/cache/instance/lv_image_cache.h"

namespace lyra::gui::internal {
namespace {

uint16_t *s_pixels = nullptr;
lv_image_dsc_t s_background{};
lv_obj_t *s_play_icon = nullptr;

struct Glow { int x, y, radius, r, g, b, strength; };
// Fixed, soft pools of light. Rasterized once into PSRAM, never animated or
// decoded from SD, and shared by every page of this theme.
constexpr Glow kGlows[] = {
    {55, 105, 150, 154, 9, 35, 150}, {244, 135, 146, 145, 12, 105, 150},
    {288, 239, 115, 248, 65, 19, 160}, {147, 289, 110, 214, 63, 6, 130},
    {27, 416, 145, 154, 31, 10, 115}, {225, 416, 105, 97, 9, 65, 80},
    {247, 104, 36, 148, 42, 113, 75}, {218, 142, 31, 184, 43, 124, 75},
    {289, 166, 31, 206, 28, 125, 100}, {276, 209, 28, 237, 49, 113, 130},
    {214, 249, 29, 238, 49, 77, 140}, {159, 265, 21, 225, 34, 49, 110},
    {159, 319, 34, 231, 104, 15, 100}, {242, 345, 36, 187, 75, 12, 75},
    {92, 369, 30, 136, 23, 53, 70}, {156, 407, 42, 124, 13, 58, 65},
};

void logo_cb(lv_event_t *event)
{
    // A native wireframe Lyra mark echoes the reference without another image
    // allocation. It is decorative and has no hit target.
    lv_layer_t *layer = lv_event_get_layer(event);
    const lv_point_t points[] = {{270, 291}, {218, 327}, {270, 360}, {316, 328},
                                 {218, 396}, {270, 433}, {316, 398}};
    constexpr uint8_t edges[][2] = {{0,1},{0,3},{1,2},{2,3},{1,4},{4,5},{5,6},
                                   {3,6},{0,2},{2,5},{4,2},{2,6}};
    for (const auto &edge : edges) {
        lv_draw_line_dsc_t line;
        lv_draw_line_dsc_init(&line);
        line.p1 = {static_cast<lv_value_precise_t>(points[edge[0]].x),
                   static_cast<lv_value_precise_t>(points[edge[0]].y)};
        line.p2 = {static_cast<lv_value_precise_t>(points[edge[1]].x),
                   static_cast<lv_value_precise_t>(points[edge[1]].y)};
        line.color = lv_color_hex(edge[0] < 2 ? 0xEF426F : 0xCB6239);
        line.opa = LV_OPA_50;
        line.width = 5;
        line.round_start = line.round_end = true;
        lv_draw_line(layer, &line);
    }
}

void style_menu_row(lv_obj_t *row)
{
    lv_obj_set_x(row, 0);
    lv_obj_set_width(row, kScreenWidth);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(row, kDivider, 0);
}

void set_pressed_label_contrast(lv_obj_t *parent, bool pressed)
{
    const uint32_t count = lv_obj_get_child_count(parent);
    for (uint32_t i = 0; i < count; ++i) {
        lv_obj_t *child = lv_obj_get_child(parent, i);
        if (lv_obj_check_type(child, &lv_label_class)) {
            if (pressed) lv_obj_add_state(child, LV_STATE_USER_1);
            else lv_obj_remove_state(child, LV_STATE_USER_1);
        }
        set_pressed_label_contrast(child, pressed);
    }
}

void selection_press_cb(lv_event_t *event)
{
    lv_obj_t *button = lv_event_get_current_target_obj(event);
    if (lv_event_get_target_obj(event) != button) return;
    set_pressed_label_contrast(button, lv_event_get_code(event) == LV_EVENT_PRESSED);
}

void favorites_cb(lv_event_t *)
{
    const size_t count = lyra::media::playlist_count();
    for (size_t i = 0; i < count; ++i) {
        lyra::media::Playlist playlist{};
        if (lyra::media::playlist_at(i, &playlist) && std::strcmp(playlist.name, "Favorites") == 0) {
            push_navigation_state();
            s_selected_playlist = i;
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

void choice_icon_cb(lv_event_t *event)
{
    lv_area_t area;
    lv_obj_get_coords(lv_event_get_current_target_obj(event), &area);
    const int x = area.x1 + 16, y = area.y1 + 11;
    lv_layer_t *layer = lv_event_get_layer(event);
    const View target = static_cast<View>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    auto box = [&](int dx, int dy, int width, int height, int radius, bool filled) {
        lv_draw_rect_dsc_t rect;
        lv_draw_rect_dsc_init(&rect);
        rect.radius = radius;
        rect.bg_color = kTextPrimary;
        rect.bg_opa = filled ? LV_OPA_COVER : LV_OPA_TRANSP;
        rect.border_width = filled ? 0 : 2;
        rect.border_color = kTextPrimary;
        rect.border_opa = LV_OPA_COVER;
        const lv_area_t bounds{x + dx, y + dy, x + dx + width - 1, y + dy + height - 1};
        lv_draw_rect(layer, &rect, &bounds);
    };
    auto line = [&](int x1, int y1, int x2, int y2) {
        lv_draw_line_dsc_t stroke;
        lv_draw_line_dsc_init(&stroke);
        stroke.p1 = {static_cast<lv_value_precise_t>(x + x1), static_cast<lv_value_precise_t>(y + y1)};
        stroke.p2 = {static_cast<lv_value_precise_t>(x + x2), static_cast<lv_value_precise_t>(y + y2)};
        stroke.color = kTextPrimary;
        stroke.width = 2;
        lv_draw_line(layer, &stroke);
    };
    switch (target) {
        case View::LibraryAlbums:
            box(2, 2, 28, 28, LV_RADIUS_CIRCLE, false);
            box(6, 6, 20, 20, LV_RADIUS_CIRCLE, false);
            box(13, 13, 6, 6, LV_RADIUS_CIRCLE, true);
            break;
        case View::LibraryArtists:
            box(10, 1, 13, 13, LV_RADIUS_CIRCLE, true);
            box(3, 17, 27, 14, 7, true);
            break;
        case View::ThemeOptions:
            box(1, 3, 30, 21, 0, false);
            line(16, 24, 16, 30);
            line(8, 30, 24, 30);
            break;
        case View::LanguageOptions:
            box(1, 1, 30, 30, LV_RADIUS_CIRCLE, false);
            box(9, 1, 14, 30, LV_RADIUS_CIRCLE, false);
            line(2, 16, 30, 16);
            break;
        case View::DisplaySettings:
            box(9, 9, 14, 14, LV_RADIUS_CIRCLE, true);
            line(16, 0, 16, 5); line(16, 27, 16, 32);
            line(0, 16, 5, 16); line(27, 16, 32, 16);
            line(4, 4, 8, 8); line(24, 24, 28, 28);
            line(4, 28, 8, 24); line(24, 8, 28, 4);
            break;
        default: break;
    }
}

lv_obj_t *choice(lv_obj_t *body, int y, const char *icon, const char *title,
                  View target, const char *value = nullptr)
{
    lv_obj_t *row = make_button(body, 0, y, kScreenWidth, 54, kSurface, 0, true);
    lv_obj_t *glyph = make_label(row, icon, kTextPrimary);
    lv_obj_set_style_text_font(glyph, zeno_heading_font(), 0);
    lv_obj_align(glyph, LV_ALIGN_LEFT_MID, 16, 0);
    if (target == View::LibraryAlbums || target == View::LibraryArtists || target == View::ThemeOptions ||
        target == View::LanguageOptions || target == View::DisplaySettings) {
        lv_obj_add_flag(glyph, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(row, choice_icon_cb, LV_EVENT_DRAW_POST,
            reinterpret_cast<void *>(static_cast<uintptr_t>(target)));
    }
    lv_obj_t *label = make_zeno_label(row, title);
    const bool setting = target == View::ThemeOptions || target == View::LanguageOptions ||
        target == View::DisplaySettings || target == View::PlaybackSettings || target == View::SoundSettings ||
        target == View::About || target == View::SortingSettings || target == View::SystemSettings;
    if (setting) {
        lv_obj_set_style_text_font(label, lyra::font::ui(), 0);
        lv_obj_t *arrow = make_label(row, LV_SYMBOL_RIGHT, kTextSecondary);
        lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -8, 0);
    }
    make_marquee(label, value ? 167 : 238);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 60, 0);
    if (value) {
        // Values and settings remain legible in the normal multilingual font.
        lv_obj_set_style_text_font(label, lyra::font::ui(), 0);
        lv_obj_t *detail = make_label(row, value, kTextSecondary);
        make_marquee(detail, 65);
        lv_obj_set_style_text_align(detail, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(detail, LV_ALIGN_RIGHT_MID, -28, 0);
    }
    add_route(row, target);
    return row;
}

} // namespace

const lv_font_t *zeno_heading_font(bool large)
{
    // Preserve the embedded multilingual fallback for non-Latin UI languages.
    static lv_font_t heading = lv_font_montserrat_28;
    static lv_font_t menu = lv_font_montserrat_40;
    heading.fallback = menu.fallback = lyra::font::ui();
    return large ? &menu : &heading;
}

lv_obj_t *make_zeno_label(lv_obj_t *parent, const char *text, bool large)
{
    char lowercase[lyra::media::kMaxName + 32];
    copy_ui_text(lowercase, sizeof(lowercase), text);
    for (char *p = lowercase; *p; ++p) {
        if (static_cast<unsigned char>(*p) < 128) *p = static_cast<char>(std::tolower(*p));
    }
    lv_obj_t *label = make_label(parent, lowercase, kTextPrimary);
    lv_obj_set_style_text_font(label, zeno_heading_font(large), 0);
    return label;
}

void make_zeno_background()
{
    s_play_icon = nullptr; // The previous screen and its labels were deleted.
    if (!is_zeno_theme() || s_view == View::FullscreenInfoArt) {
        if (s_pixels) lv_image_cache_drop(&s_background);
        heap_caps_free(s_pixels);
        s_pixels = nullptr;
        s_background = {};
        return;
    }
    if (!s_pixels) {
        constexpr size_t bytes = kScreenWidth * kScreenHeight * sizeof(uint16_t);
        s_pixels = static_cast<uint16_t *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!s_pixels) return; // The solid dark palette remains usable.
        for (int y = 0; y < kScreenHeight; ++y) {
            for (int x = 0; x < kScreenWidth; ++x) {
                int r = 9, g = 8, b = 15;
                for (const auto &glow : kGlows) {
                    const int dx = x - glow.x, dy = y - glow.y;
                    const int square = glow.radius * glow.radius;
                    const int distance = dx * dx + dy * dy;
                    if (distance >= square) continue;
                    int alpha = (square - distance) * 256 / square;
                    alpha = glow.radius <= 42 ? std::min(256, alpha * 4) : alpha * alpha / 256;
                    alpha = alpha * glow.strength / 256;
                    r += (glow.r - r) * alpha / 256;
                    g += (glow.g - g) * alpha / 256;
                    b += (glow.b - b) * alpha / 256;
                }
                s_pixels[y * kScreenWidth + x] = static_cast<uint16_t>(
                    ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
            }
        }
        s_background.header.magic = LV_IMAGE_HEADER_MAGIC;
        s_background.header.cf = LV_COLOR_FORMAT_RGB565;
        s_background.header.w = kScreenWidth;
        s_background.header.h = kScreenHeight;
        s_background.header.stride = kScreenWidth * sizeof(uint16_t);
        s_background.data_size = bytes;
        s_background.data = reinterpret_cast<const uint8_t *>(s_pixels);
    }
    lv_obj_t *image = lv_image_create(s_screen);
    lv_image_set_src(image, &s_background);
    lv_obj_set_pos(image, 0, 0);
    lv_obj_remove_flag(image, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(image, LV_OBJ_FLAG_SCROLLABLE);
    if (s_view == View::Menu || s_view == View::Library) {
        lv_obj_add_event_cb(image, logo_cb, LV_EVENT_DRAW_POST, nullptr);
    }
}

lv_color_t zeno_label_color(lv_obj_t *parent, lv_color_t color)
{
    // Selected rows have already received their pink/orange surface in
    // make_button(). Use white for their title, subtitle and icons alike.
    for (lv_obj_t *object = parent; object; object = lv_obj_get_parent(object)) {
        if (!lv_obj_check_type(object, &lv_button_class)) continue;
        const lv_color_t background = lv_obj_get_style_bg_color(object, LV_PART_MAIN);
        if (lv_obj_get_style_bg_opa(object, LV_PART_MAIN) != LV_OPA_TRANSP &&
            (lv_color_eq(background, kAccent) || lv_color_eq(background, kAccentSurface) ||
             lv_color_eq(background, kAccentDark))) return kTextPrimary;
    }
    return color;
}

void style_zeno_box(lv_obj_t *box, lv_color_t color, int width, int height)
{
    // Read-only information rows share the menu surface and divider, while
    // retaining their ordinary object type and absence of navigation arrows.
    if (width >= 280 && height >= 40 && height <= 70 && lv_color_eq(color, kSurface)) {
        style_menu_row(box);
    }
    if ((width == kScreenWidth && lv_color_eq(color, kBackground) && height > kStatusHeight) ||
        (width >= 300 && lv_color_eq(color, kSurface) && height > 72)) {
        lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(box, 0, 0);
        lv_obj_set_style_radius(box, 0, 0);
    }
}

void style_zeno_button(lv_obj_t *button, lv_color_t color, int width, int height)
{
    const bool row = width >= 280 && height >= 40 && height <= 70;
    if (row || lv_color_eq(color, kBackground)) {
        lv_obj_set_style_radius(button, row ? 0 : LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
    }
    if (row) style_menu_row(button);
    const bool selected = lv_color_eq(color, kAccentSurface) || lv_color_eq(color, kAccentDark);
    if (selected || row) {
        const lv_style_selector_t selector = selected ? LV_STATE_DEFAULT : LV_STATE_PRESSED;
        lv_obj_set_style_bg_opa(button, LV_OPA_80, selector);
        lv_obj_set_style_bg_color(button, kAccent, selector);
        lv_obj_set_style_bg_grad_color(button, lv_color_hex(0xEF6B24), selector);
        lv_obj_set_style_bg_grad_dir(button, LV_GRAD_DIR_HOR, selector);
    }
    lv_obj_set_style_bg_opa(button, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_add_event_cb(button, selection_press_cb, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(button, selection_press_cb, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(button, selection_press_cb, LV_EVENT_PRESS_LOST, nullptr);
}

void render_zeno_menu()
{
    lv_obj_t *body = make_scroll_body(kStatusHeight);
    struct Item { lyra::i18n::StringId title; View view; lv_event_cb_t callback; };
    const Item items[] = {
        {lyra::i18n::StringId::Music, View::Library, open_library_cb},
        {lyra::i18n::StringId::NowPlaying, View::Player,
            [](lv_event_t *) { show_now_playing(); }},
        {lyra::i18n::StringId::BrowseFolders, View::Folders, open_folders_cb},
        {lyra::i18n::StringId::Queue, View::Queue, open_queue_cb},
        {lyra::i18n::StringId::Search, View::Search, nullptr},
        {lyra::i18n::StringId::Settings, View::Settings, nullptr},
        {lyra::i18n::StringId::Equalizer, View::Equalizer, nullptr},
    };
    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); ++i) {
        lv_obj_t *row = make_button(body, 0, 16 + static_cast<int>(i) * 60, 320, 58, kBackground, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_t *label = make_zeno_label(row, tr(items[i].title), i == 0);
        make_marquee(label, 282);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 23, 0);
        lv_obj_set_style_text_color(label, i == 0 ? kTextPrimary : kTextMuted, 0);
        if (i == 0) {
            lv_obj_t *stripe = make_box(row, 0, 0, 7, 58, kAccent);
            lv_obj_remove_flag(stripe, LV_OBJ_FLAG_CLICKABLE);
        }
        if (items[i].callback) lv_obj_add_event_cb(row, items[i].callback, LV_EVENT_CLICKED, nullptr);
        else add_route(row, items[i].view);
    }
    lv_obj_t *brand = make_label(body, "Emotivate Lyra", kTextSecondary);
    lv_obj_set_style_text_letter_space(brand, 2, 0);
    lv_obj_set_pos(brand, 24, 453);
}

void render_zeno_library()
{
    make_header(tr(lyra::i18n::StringId::Music), View::Menu, true);
    lv_obj_t *body = make_scroll_body(72);
    choice(body, 0, LV_SYMBOL_AUDIO, tr(lyra::i18n::StringId::AllSongs), View::LibrarySongs);
    choice(body, 56, LV_SYMBOL_BULLET, tr(lyra::i18n::StringId::Albums), View::LibraryAlbums);
    choice(body, 112, LV_SYMBOL_BULLET, tr(lyra::i18n::StringId::Artists), View::LibraryArtists);
    choice(body, 168, LV_SYMBOL_EDIT, tr(lyra::i18n::StringId::Genres), View::LibraryGenres);
    lv_obj_t *playlists = choice(body, 224, LV_SYMBOL_LIST,
        tr(lyra::i18n::StringId::Playlists), View::Playlists);
    lv_obj_remove_event_cb(playlists, route_cb);
    lv_obj_add_event_cb(playlists, open_library_playlists_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *favorites = choice(body, 280, kHeartFilled, tr(lyra::i18n::StringId::Favorites), View::PlaylistDetail);
    lv_obj_remove_event_cb(favorites, route_cb);
    lv_obj_add_event_cb(favorites, favorites_cb, LV_EVENT_CLICKED, nullptr);
    choice(body, 336, LV_SYMBOL_REFRESH, tr(lyra::i18n::StringId::Year), View::LibraryYears);
}

void make_zeno_album_song_row(lv_obj_t *parent, int y, size_t track_index, size_t ordinal)
{
    lyra::media::Track track{};
    if (!lyra::media::track_at(track_index, &track)) return;
    const bool current = track_index == s_current_track &&
        std::strcmp(lyra::audio::status().path, track.path) == 0;
    lv_obj_t *row = make_button(parent, 0, y, kScreenWidth, 44,
        current ? kAccentSurface : kSurface, 0, true);
    char number[16];
    std::snprintf(number, sizeof(number), "%u", static_cast<unsigned>(track.track_number ? track.track_number : ordinal));
    lv_obj_t *index = make_label(row, current ? LV_SYMBOL_PLAY : number, kTextPrimary);
    lv_obj_set_width(index, 24);
    lv_label_set_long_mode(index, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(index, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_t *title = make_label(row, track.title, kTextPrimary);
    make_marquee(title, 272);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 36, 0);
    add_track_route(row, track_index);
}

void render_zeno_settings()
{
    make_header(tr(lyra::i18n::StringId::Settings), View::Menu, true);
    lv_obj_t *body = make_scroll_body(72);
    choice(body, 0, LV_SYMBOL_IMAGE, tr(lyra::i18n::StringId::Theme), View::ThemeOptions, theme_name(s_theme));
    choice(body, 56, LV_SYMBOL_SETTINGS, tr(lyra::i18n::StringId::Language), View::LanguageOptions,
        lyra::i18n::language_name(lyra::i18n::current_language()));
    choice(body, 112, LV_SYMBOL_IMAGE, tr(lyra::i18n::StringId::Display), View::DisplaySettings);
    choice(body, 168, LV_SYMBOL_PLAY, tr(lyra::i18n::StringId::Playback), View::PlaybackSettings);
    choice(body, 224, LV_SYMBOL_VOLUME_MID, tr(lyra::i18n::StringId::Sound), View::SoundSettings);
    choice(body, 280, LV_SYMBOL_WARNING, tr(lyra::i18n::StringId::About), View::About);
    choice(body, 336, LV_SYMBOL_LIST, tr(lyra::i18n::StringId::Sorting), View::SortingSettings);
    choice(body, 392, LV_SYMBOL_SETTINGS, tr(lyra::i18n::StringId::System), View::SystemSettings);
}

void make_zeno_transport(lv_obj_t *parent, int y)
{
    for (int i = 0; i < 3; ++i) {
        const int size = i == 1 ? 56 : 44;
        lv_obj_t *button = make_button(parent, 77 + i * 60 - (i == 1 ? 6 : 0),
            y - (i == 1 ? 6 : 0), size, size, kBackground, LV_RADIUS_CIRCLE);
        lv_obj_set_style_border_width(button, 1, 0);
        lv_obj_set_style_border_color(button, i == 1 ? kAccent : kDivider, 0);
        lv_obj_t *icon = make_label(button, i == 0 ? LV_SYMBOL_PREV : i == 2 ? LV_SYMBOL_NEXT : LV_SYMBOL_PLAY,
            i == 1 ? kAccent : kTextPrimary);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_28, 0);
        lv_obj_center(icon);
        if (i == 1) {
            s_play_icon = icon;
            lv_obj_add_event_cb(button, nav_play_cb, LV_EVENT_CLICKED, nullptr);
        } else {
            lv_event_cb_t callback = i == 0 ? nav_previous_cb : nav_next_cb;
            for (lv_event_code_t code : {LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST})
                lv_obj_add_event_cb(button, callback, code, nullptr);
        }
    }
    update_zeno_transport();
}

void update_zeno_transport()
{
    if (!is_zeno_theme() || !s_play_icon) return;
    const lyra::audio::Status status = lyra::audio::status();
    lv_label_set_text(s_play_icon, status.playing && !status.paused ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

} // namespace lyra::gui::internal
