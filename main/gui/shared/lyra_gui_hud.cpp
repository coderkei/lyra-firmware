/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../lyra_gui_internal.h"
#include <initializer_list>
#include "lyra_gui_draw.h"
#include "src/misc/cache/instance/lv_image_cache.h"

namespace lyra::gui::internal {
namespace {

// Neon Sky renders a Night City skyline procedurally into one PSRAM frame.
// It is generated once per theme activation, never decoded from flash or SD,
// and blitted as an opaque RGB565 image beneath every page.
uint16_t *s_city_pixels = nullptr;
lv_image_dsc_t s_city_image{};

struct Rgb { int r, g, b; };
struct Glow { int x, y, radius; Rgb color; int strength; };
struct Sign { int x, y1, y2; Rgb color; };

constexpr Glow kCityGlows[] = {
    {160, 380, 290, {120, 18, 38}, 200},  // City glow lighting the horizon.
    {24, 500, 300, {130, 14, 22}, 190},   // Street haze rising from the left.
    {300, 430, 210, {96, 8, 46}, 140},    // Magenta spill from the right block.
    {292, 40, 190, {0, 64, 78}, 90},      // Cold cyan sky behind the status bar.
};
constexpr Rgb kWindowRed{214, 52, 44};
constexpr Rgb kWindowCyan{48, 196, 208};
constexpr Rgb kWindowYellow{206, 190, 24};
constexpr int kMaxSigns = 4;

uint32_t s_seed = 0;

uint32_t next_random()
{
    s_seed = s_seed * 1664525u + 1013904223u;
    return s_seed >> 8;
}

uint32_t hash3(uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t h = a * 0x9E3779B1u ^ b * 0x85EBCA77u ^ c * 0xC2B2AE3Du;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return h;
}

void blend(Rgb &base, const Rgb &top, int alpha)
{
    base.r += (top.r - base.r) * alpha / 256;
    base.g += (top.g - base.g) * alpha / 256;
    base.b += (top.b - base.b) * alpha / 256;
}

void paint_city(uint16_t *pixels)
{
    // Static scratch keeps ~3 KB of column tables off the GUI task stack.
    static int16_t far_top[kScreenWidth];
    static int16_t near_top[kScreenWidth];
    static int16_t near_left[kScreenWidth];
    static int16_t near_right[kScreenWidth];
    static uint16_t near_id[kScreenWidth];
    Sign signs[kMaxSigns]{};
    int sign_count = 0;

    // A fixed seed keeps the skyline identical between boots and screenshots.
    s_seed = 2077;
    for (int x = 0; x < kScreenWidth;) {
        const int width = 8 + static_cast<int>(next_random() % 18);
        const int height = 120 + static_cast<int>(next_random() % 190);
        const bool spire = next_random() % 4 == 0;
        for (int i = 0; i < width && x + i < kScreenWidth; ++i) {
            far_top[x + i] = static_cast<int16_t>(kScreenHeight - height);
        }
        if (spire && x + width / 2 < kScreenWidth) {
            far_top[x + width / 2] = static_cast<int16_t>(
                kScreenHeight - height - 20 - static_cast<int>(next_random() % 40));
        }
        x += width;
    }
    uint16_t building = 0;
    for (int x = 0; x < kScreenWidth; ++building) {
        const int width = 16 + static_cast<int>(next_random() % 30);
        const int height = 44 + static_cast<int>(next_random() % 130);
        const int right = std::min(kScreenWidth, x + width) - 1;
        for (int i = x; i <= right; ++i) {
            near_top[i] = static_cast<int16_t>(kScreenHeight - height);
            near_left[i] = static_cast<int16_t>(x);
            near_right[i] = static_cast<int16_t>(right);
            near_id[i] = building;
        }
        if (height > 110 && width > 22 && sign_count < kMaxSigns) {
            // Vertical neon signs hang from the tallest street-front towers.
            const int top = kScreenHeight - height + 10;
            const Rgb color = sign_count % 2 ? kWindowCyan : Rgb{255, 74, 61};
            signs[sign_count++] = {x + 4, top, top + 34 + static_cast<int>(next_random() % 30), color};
        }
        x += width;
    }

    for (int y = 0; y < kScreenHeight; ++y) {
        for (int x = 0; x < kScreenWidth; ++x) {
            Rgb pixel{7 + 8 * y / kScreenHeight, 6, 13 - 4 * y / kScreenHeight};
            for (const Glow &glow : kCityGlows) {
                const int dx = x - glow.x, dy = y - glow.y;
                const int square = glow.radius * glow.radius;
                const int distance = dx * dx + dy * dy;
                if (distance >= square) continue;
                int alpha = (square - distance) * 256 / square;
                alpha = alpha * alpha / 256 * glow.strength / 256;
                blend(pixel, glow.color, alpha);
            }
            if (y < far_top[x] && y < near_top[x]) {
                // A sparse targeting grid in the sky, like a HUD overlay.
                if (x % 16 == 8 && y % 16 == 8) blend(pixel, {110, 30, 34}, 120);
            } else if (y < near_top[x]) {
                // Far towers stand dark against the glow, edged by its light.
                blend(pixel, {12, 5, 10}, y == far_top[x] ? 40 : 200);
                const uint32_t lit = hash3(static_cast<uint32_t>(x / 4),
                                           static_cast<uint32_t>(y / 7), 11);
                if (x % 4 == 1 && y % 7 == 3 && lit % 19 == 0) {
                    blend(pixel, lit % 5 == 0 ? kWindowCyan : kWindowRed, 110);
                }
            } else {
                pixel = {10, 5, 8};
                const int local_x = x - near_left[x];
                const int local_y = y - near_top[x];
                if (x == near_left[x] || local_y == 0) {
                    // Neon spill catches the left edge and roofline of each block.
                    pixel = {64, 14, 18};
                } else if (local_x >= 3 && near_right[x] - x >= 3 && local_x % 5 < 2 &&
                           local_y > 5 && local_y % 7 >= 3 && local_y % 7 <= 4) {
                    const uint32_t lit = hash3(near_id[x], static_cast<uint32_t>(local_x / 5),
                                               static_cast<uint32_t>(local_y / 7));
                    if (lit % 100 < 24) {
                        const uint32_t tone = (lit >> 8) % 20;
                        blend(pixel, tone < 12 ? kWindowRed : tone < 17 ? kWindowCyan : kWindowYellow,
                              150 + static_cast<int>((lit >> 16) % 90));
                    }
                }
            }
            for (int i = 0; i < sign_count; ++i) {
                const Sign &sign = signs[i];
                if (y < sign.y1 - 3 || y > sign.y2 + 3 || x < sign.x - 4 || x > sign.x + 7) continue;
                const bool core = x >= sign.x && x <= sign.x + 3 && y >= sign.y1 && y <= sign.y2;
                const bool tick = core && (y - sign.y1) % 6 == 5;
                if (core) pixel = tick ? Rgb{20, 6, 10} : sign.color;
                else blend(pixel, sign.color, 60);
            }
            if (y > kScreenHeight - 34) {
                // Ground fog sits over the street level.
                blend(pixel, {58, 10, 16}, (y - (kScreenHeight - 34)) * 4);
            }
            if (y % 3 == 0) {
                pixel.r = pixel.r * 13 / 16;
                pixel.g = pixel.g * 13 / 16;
                pixel.b = pixel.b * 13 / 16;
            }
            const int edge = std::min(x, kScreenWidth - 1 - x);
            if (edge < 20) {
                const int shade = 176 + edge * 4;
                pixel.r = pixel.r * shade / 256;
                pixel.g = pixel.g * shade / 256;
                pixel.b = pixel.b * shade / 256;
            }
            pixel.r = std::clamp(pixel.r, 0, 255);
            pixel.g = std::clamp(pixel.g, 0, 255);
            pixel.b = std::clamp(pixel.b, 0, 255);
            pixels[y * kScreenWidth + x] = static_cast<uint16_t>(
                ((pixel.r >> 3) << 11) | ((pixel.g >> 2) << 5) | (pixel.b >> 3));
        }
    }
}

bool load_city()
{
    if (s_city_pixels) return true;
    constexpr size_t bytes = kScreenWidth * kScreenHeight * sizeof(uint16_t);
    auto *pixels = static_cast<uint16_t *>(heap_caps_malloc(
        bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!pixels) {
        ESP_LOGW(kTag, "Night City backdrop unavailable; using the HUD palette only");
        return false;
    }
    paint_city(pixels);
    s_city_pixels = pixels;
    init_rgb565_image(&s_city_image, pixels, kScreenWidth, kScreenHeight);
    return true;
}

void hud_city_cb(lv_event_t *event)
{
    if (!s_city_pixels) return;
    lv_area_t area;
    lv_obj_get_coords(s_screen, &area);
    lv_draw_image_dsc_t image;
    lv_draw_image_dsc_init(&image);
    image.src = &s_city_image;
    image.opa = LV_OPA_COVER;
    image.antialias = false;
    // LVGL clips DRAW_MAIN to the current object's bounds. Sampling a fixed
    // screen origin keeps the city aligned while rows and keyboards move.
    lv_draw_image(lv_event_get_layer(event), &image, &area);
}

void hud_rect(lv_layer_t *layer, int x1, int y1, int x2, int y2,
              lv_color_t color, lv_opa_t opacity = LV_OPA_COVER)
{
    const lv_area_t area{x1, y1, x2, y2};
    if (x2 < x1 || y2 < y1 || !gui_draw_area_visible(layer, area)) return;
    lv_draw_rect_dsc_t rect;
    lv_draw_rect_dsc_init(&rect);
    rect.bg_color = color;
    rect.bg_opa = opacity;
    rect.radius = 0;
    lv_draw_rect(layer, &rect, &area);
}

void hud_triangle(lv_layer_t *layer, lv_point_t a, lv_point_t b, lv_point_t c, lv_color_t color)
{
    const lv_area_t area{std::min({a.x, b.x, c.x}), std::min({a.y, b.y, c.y}),
                         std::max({a.x, b.x, c.x}), std::max({a.y, b.y, c.y})};
    if (!gui_draw_area_visible(layer, area)) return;
    lv_draw_triangle_dsc_t triangle;
    lv_draw_triangle_dsc_init(&triangle);
    triangle.p[0] = {static_cast<lv_value_precise_t>(a.x), static_cast<lv_value_precise_t>(a.y)};
    triangle.p[1] = {static_cast<lv_value_precise_t>(b.x), static_cast<lv_value_precise_t>(b.y)};
    triangle.p[2] = {static_cast<lv_value_precise_t>(c.x), static_cast<lv_value_precise_t>(c.y)};
    triangle.color = color;
    triangle.opa = LV_OPA_COVER;
    lv_draw_triangle(layer, &triangle);
}

void hud_frame_cb(lv_event_t *event)
{
    lv_obj_t *object = lv_event_get_current_target_obj(event);
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_area_t area;
    lv_obj_get_coords(object, &area);
    const int width = lv_area_get_width(&area);
    const int height = lv_area_get_height(&area);
    const bool hero = lv_event_get_user_data(event) != nullptr;
    const lv_color_t fill = lv_obj_get_style_bg_color(object, LV_PART_MAIN);
    const bool selected = lv_obj_has_state(object, LV_STATE_PRESSED) ||
        lv_color_eq(fill, kAccentSurface);
    const bool accent = lv_color_eq(fill, kAccentDark) || lv_color_eq(fill, kAccent);
    const lv_color_t cyan = lv_color_hex(kHudCyanRgb);
    const lv_color_t signal = selected ? cyan : kAccent;

    lv_draw_border_dsc_t border;
    lv_draw_border_dsc_init(&border);
    border.color = selected ? cyan : (hero || accent) ? kAccent : kDivider;
    border.width = 1;
    border.opa = LV_OPA_COVER;
    border.side = LV_BORDER_SIDE_FULL;
    gui_draw_border(layer, &border, &area);
    if (width < 40 || height < 22) return;

    // Every panel carries a solid clipped corner, the signature of the HUD.
    const int cut = hero ? std::min(11, height / 4) : std::min(7, height / 5);
    hud_triangle(layer, {area.x2 - cut, area.y2}, {area.x2, area.y2},
                 {area.x2, area.y2 - cut}, signal);
    if (hero) {
        hud_triangle(layer, {area.x1, area.y1}, {area.x1 + cut, area.y1},
                     {area.x1, area.y1 + cut}, signal);
        const int tab = area.x1 + cut + 5;
        hud_rect(layer, tab, area.y1, tab + std::min(42, width / 4), area.y1 + 2, signal);
    } else if (width >= 200) {
        // List rows show a short signal tab that runs full height when selected.
        const int bar_bottom = selected ? area.y2 : area.y1 + height * 2 / 5;
        hud_rect(layer, area.x1, area.y1, area.x1 + (selected ? 3 : 2), bar_bottom, signal);
    }
}

void hud_brackets_cb(lv_event_t *event)
{
    lv_area_t area;
    lv_obj_get_coords(lv_event_get_current_target_obj(event), &area);
    lv_layer_t *layer = lv_event_get_layer(event);
    const int arm = std::min<int>(18, lv_area_get_width(&area) / 5);
    const lv_color_t red = kAccent;
    const lv_color_t cyan = lv_color_hex(kHudCyanRgb);
    // Scanner-style corner brackets frame the cover without hiding it.
    hud_rect(layer, area.x1, area.y1, area.x1 + arm, area.y1 + 2, red);
    hud_rect(layer, area.x1, area.y1, area.x1 + 2, area.y1 + arm, red);
    hud_rect(layer, area.x2 - arm, area.y2 - 2, area.x2, area.y2, red);
    hud_rect(layer, area.x2 - 2, area.y2 - arm, area.x2, area.y2, red);
    hud_rect(layer, area.x2 - 6, area.y1 + 4, area.x2 - 4, area.y1 + 6, cyan);
    hud_rect(layer, area.x1 + 4, area.y2 - 6, area.x1 + arm, area.y2 - 5, cyan);
}

void hud_segments_cb(lv_event_t *event)
{
    lv_obj_t *bar = lv_event_get_current_target_obj(event);
    lv_area_t area;
    lv_obj_get_coords(bar, &area);
    lv_layer_t *layer = lv_event_get_layer(event);
    // Cut the fill into cells, like the game's health and ammo readouts.
    for (int x = area.x1 + 5; x < area.x2; x += 6) {
        hud_rect(layer, x, area.y1, x, area.y2, kBackground);
    }
}

void hud_equalizer_grid_cb(lv_event_t *event)
{
    lv_area_t area;
    lv_obj_get_coords(lv_event_get_current_target_obj(event), &area);
    lv_layer_t *layer = lv_event_get_layer(event);
    for (int step = 0; step <= 4; ++step) {
        const int y = area.y1 + step * (lv_area_get_height(&area) - 1) / 4;
        hud_rect(layer, area.x1, y, area.x2, y, step == 2 ? kAccent : kDivider,
                 step == 2 ? LV_OPA_60 : LV_OPA_COVER);
    }
    for (size_t band = 0; band < lyra::audio::kEqualizerBandCount; ++band) {
        const int x = area.x1 + 9 + static_cast<int>(band) * 52;
        hud_rect(layer, x, area.y1, x, area.y2, kDivider);
    }
}

void set_label_selection(lv_obj_t *parent, bool selected)
{
    const uint32_t count = lv_obj_get_child_count(parent);
    for (uint32_t i = 0; i < count; ++i) {
        lv_obj_t *child = lv_obj_get_child(parent, i);
        if (lv_obj_check_type(child, &lv_label_class)) {
            if (selected) lv_obj_add_state(child, LV_STATE_USER_1);
            else lv_obj_remove_state(child, LV_STATE_USER_1);
        }
        set_label_selection(child, selected);
    }
}

void hud_press_cb(lv_event_t *event)
{
    lv_obj_t *button = lv_event_get_current_target_obj(event);
    if (lv_event_get_target_obj(event) != button) return;
    set_label_selection(button, lv_event_get_code(event) == LV_EVENT_PRESSED);
}

} // namespace

const char *hud_heading_text(const char *text, char *buffer, size_t capacity)
{
    if (!text) return "";
    if (!s_neon_hud || !buffer || capacity == 0) return text;
    for (const char *p = text; *p; ++p) {
        if (static_cast<unsigned char>(*p) >= 0x80) return text;
    }
    copy_ui_text(buffer, capacity, text);
    for (char *p = buffer; *p; ++p) {
        *p = static_cast<char>(std::toupper(static_cast<unsigned char>(*p)));
    }
    return buffer;
}

void add_hud_frame(lv_obj_t *object, bool octagonal)
{
    if (!s_neon_hud) return;
    // Draw events do not create children or alter rectangular touch targets.
    lv_obj_remove_event_cb(object, hud_frame_cb);
    lv_obj_set_style_outline_width(object, 0, 0);
    lv_obj_set_style_shadow_width(object, 0, 0);
    lv_obj_add_event_cb(object, hud_frame_cb, LV_EVENT_DRAW_MAIN,
                        octagonal ? reinterpret_cast<void *>(1) : nullptr);
}

void add_hud_press_feedback(lv_obj_t *button)
{
    if (!s_neon_hud) return;
    // Pressed rows light their labels cyan, the HUD's "selected" signal.
    lv_obj_add_event_cb(button, hud_press_cb, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(button, hud_press_cb, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(button, hud_press_cb, LV_EVENT_PRESS_LOST, nullptr);
}

void add_hud_brackets(lv_obj_t *object)
{
    if (!s_neon_hud) return;
    lv_obj_add_event_cb(object, hud_brackets_cb, LV_EVENT_DRAW_POST, nullptr);
}

void make_hud_background()
{
    // style_root has already deleted every object that referenced the image.
    if (!s_neon_hud || s_view == View::FullscreenInfoArt) {
        if (!s_city_pixels) return;
        lv_image_cache_drop(&s_city_image);
        heap_caps_free(s_city_pixels);
        s_city_pixels = nullptr;
        s_city_image = {};
        return;
    }
    load_city();
    lv_obj_t *backdrop = lv_obj_create(s_screen);
    lv_obj_set_pos(backdrop, 0, 0);
    lv_obj_set_size(backdrop, kScreenWidth, kScreenHeight);
    lv_obj_set_style_pad_all(backdrop, 0, 0);
    lv_obj_set_style_radius(backdrop, 0, 0);
    lv_obj_set_style_border_width(backdrop, 0, 0);
    lv_obj_set_style_outline_width(backdrop, 0, 0);
    lv_obj_set_style_shadow_width(backdrop, 0, 0);
    lv_obj_set_style_bg_color(backdrop, kBackground, 0);
    lv_obj_set_style_bg_opa(backdrop, LV_OPA_COVER, 0);
    lv_obj_remove_flag(backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(backdrop, hud_city_cb, LV_EVENT_DRAW_MAIN, nullptr);
}

void make_hud_body_background(lv_obj_t *body)
{
    if (!s_neon_hud) return;
    if (lv_obj_get_parent(body) != s_screen) {
        // Nested search/list containers reveal the already opaque page body.
        lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
        return;
    }
    // An opaque viewport allows LVGL's cover check to skip the root backdrop
    // when it redraws a scrolled page. No per-viewport buffers or decoding.
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
    lv_obj_set_style_outline_width(body, 0, 0);
    lv_obj_set_style_shadow_width(body, 0, 0);
    lv_obj_add_event_cb(body, hud_city_cb, LV_EVENT_DRAW_MAIN, nullptr);
}

void make_hud_equalizer_grid(lv_obj_t *parent, int x, int y, int width, int height)
{
    if (!s_neon_hud && !is_cute_theme()) return;
    lv_obj_t *grid = make_box(parent, x, y, width, height, kBackground);
    lv_obj_set_style_bg_opa(grid, LV_OPA_COVER, 0);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(grid, hud_equalizer_grid_cb, LV_EVENT_DRAW_MAIN, nullptr);
}

void style_hud_progress_bar(lv_obj_t *bar)
{
    if (!s_neon_hud || !bar) return;
    lv_obj_set_style_radius(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, kSurfaceRaised, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, kAccent, LV_PART_INDICATOR);
    lv_obj_remove_event_cb(bar, hud_segments_cb);
    lv_obj_add_event_cb(bar, hud_segments_cb, LV_EVENT_DRAW_MAIN_END, nullptr);
}

void decorate_hud_status_bar(lv_obj_t *bar)
{
    if (!s_neon_hud) return;
    // A solid strip keeps the readouts crisp above the skyline.
    lv_obj_remove_event_cb(bar, hud_city_cb);
    lv_obj_set_style_bg_color(bar, kNavSurface, 0);
    make_box(bar, 0, kStatusHeight - 1, kScreenWidth, 1, kDivider);
    make_box(bar, 0, kStatusHeight - 2, 72, 2, kAccent);
    make_box(bar, 76, kStatusHeight - 2, 4, 2, kAccent);
    make_box(bar, 306, kStatusHeight - 4, 6, 4, lv_color_hex(kHudYellowRgb));
}

void decorate_hud_header(lv_obj_t *header, lv_obj_t *title)
{
    if (!s_neon_hud) return;
    if (title) lv_obj_set_style_text_letter_space(title, 1, 0);
    make_box(header, 0, 43, kScreenWidth, 1, kDivider);
    make_box(header, 8, 41, 88, 3, kAccent);
    make_box(header, 100, 41, 4, 3, lv_color_hex(kHudCyanRgb));
    make_box(header, 300, 41, 12, 3, lv_color_hex(kHudYellowRgb));
}

void style_hud_controls(lv_obj_t *object)
{
    if (!s_neon_hud) return;
    // Directly-created controls (search keyboards, sliders and text areas)
    // share the same geometry while retaining their original sizes and events.
    const lv_color_t cyan = lv_color_hex(kHudCyanRgb);
    if (lv_obj_has_class(object, &lv_slider_class) || lv_obj_has_class(object, &lv_bar_class)) {
        lv_obj_set_style_radius(object, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(object, 0, LV_PART_INDICATOR);
        lv_obj_set_style_radius(object, 0, LV_PART_KNOB);
        lv_obj_set_style_bg_color(object, kSurfaceRaised, LV_PART_MAIN);
        lv_obj_set_style_border_width(object, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(object, kDivider, LV_PART_MAIN);
        lv_obj_set_style_bg_color(object, kAccent, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(object, cyan, LV_PART_KNOB);
        lv_obj_set_style_border_width(object, 0, LV_PART_KNOB);
    } else if (lv_obj_has_class(object, &lv_arc_class)) {
        lv_obj_set_style_arc_rounded(object, false, LV_PART_MAIN);
        lv_obj_set_style_arc_rounded(object, false, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(object, kAccent, LV_PART_INDICATOR);
    } else if (lv_obj_has_class(object, &lv_buttonmatrix_class)) {
        lv_obj_set_style_radius(object, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(object, 0, LV_PART_ITEMS);
        lv_obj_set_style_bg_color(object, kSurface, LV_PART_ITEMS);
        lv_obj_set_style_text_color(object, kTextPrimary, LV_PART_ITEMS);
        lv_obj_set_style_border_width(object, 1, LV_PART_ITEMS);
        lv_obj_set_style_border_color(object, kDivider, LV_PART_ITEMS);
        const lv_style_selector_t pressed_items =
            static_cast<lv_style_selector_t>(LV_PART_ITEMS) |
            static_cast<lv_style_selector_t>(LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(object, kAccentSurface, pressed_items);
        lv_obj_set_style_border_color(object, cyan, pressed_items);
        lv_obj_set_style_text_color(object, cyan, pressed_items);
    } else if (lv_obj_has_class(object, &lv_textarea_class)) {
        lv_obj_set_style_radius(object, 0, 0);
        lv_obj_set_style_bg_color(object, kSurface, 0);
        lv_obj_set_style_border_width(object, 1, 0);
        lv_obj_set_style_border_color(object, kAccent, 0);
        lv_obj_set_style_border_color(object, cyan, LV_PART_CURSOR);
    }
    const uint32_t count = lv_obj_get_child_count(object);
    for (uint32_t i = 0; i < count; ++i) style_hud_controls(lv_obj_get_child(object, i));
}

} // namespace lyra::gui::internal
