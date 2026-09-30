/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../lyra_gui_internal.h"
#include "src/misc/cache/instance/lv_image_cache.h"

namespace lyra::gui::internal {
namespace {

extern const uint8_t city_png_start[] asm("_binary_neon_sky_city_png_start");
extern const uint8_t city_png_end[] asm("_binary_neon_sky_city_png_end");
extern const uint8_t city_lists_png_start[] asm("_binary_neon_sky_city_lists_png_start");
extern const uint8_t city_lists_png_end[] asm("_binary_neon_sky_city_lists_png_end");
uint16_t *s_city_pixels[2]{};
lv_image_dsc_t s_city_images[2]{};
size_t s_city_variant = 0;

bool major_hud_view()
{
    return s_view == View::Player || s_view == View::Menu ||
        s_view == View::Library || s_view == View::Settings;
}

void draw_city(lv_layer_t *layer, size_t variant)
{
    if (!s_city_pixels[variant]) return;
    lv_area_t area;
    lv_obj_get_coords(s_screen, &area);
    lv_draw_image_dsc_t image;
    lv_draw_image_dsc_init(&image);
    image.src = &s_city_images[variant];
    image.opa = LV_OPA_COVER;
    image.antialias = false;
    // LVGL clips DRAW_MAIN to the current object's bounds. Sampling a fixed
    // screen origin keeps the city aligned while rows and keyboards move.
    lv_draw_image(layer, &image, &area);
}

void hud_city_cb(lv_event_t *event)
{
    draw_city(lv_event_get_layer(event), s_city_variant);
}

void hud_line(lv_layer_t *layer, int x1, int y1, int x2, int y2,
              lv_color_t color, lv_opa_t opacity = LV_OPA_COVER)
{
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.p1 = {static_cast<lv_value_precise_t>(x1), static_cast<lv_value_precise_t>(y1)};
    line.p2 = {static_cast<lv_value_precise_t>(x2), static_cast<lv_value_precise_t>(y2)};
    line.color = color;
    line.opa = opacity;
    line.width = 1;
    lv_draw_line(layer, &line);
}

void hud_frame_cb(lv_event_t *event)
{
    lv_obj_t *object = lv_event_get_current_target_obj(event);
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_area_t area;
    lv_obj_get_coords(object, &area);
    const bool octagonal = lv_event_get_user_data(event) != nullptr;
    const int32_t cut = std::min<int32_t>(octagonal ? 10 : 5,
        std::min(lv_area_get_width(&area), lv_area_get_height(&area)) / 4);
    const lv_color_t fill = lv_obj_get_style_bg_color(object, LV_PART_MAIN);
    const bool active = octagonal || lv_obj_has_state(object, LV_STATE_PRESSED) ||
        lv_color_eq(fill, kAccentSurface) || lv_color_eq(fill, kAccentDark) ||
        lv_color_eq(fill, kAccent);
    const lv_color_t color = active ? kAccent : kDivider;
    const int left = area.x1 + 1, right = area.x2 - 1;
    const int top = area.y1 + 1, bottom = area.y2 - 1;
    if (!octagonal) {
        if (major_hud_view() && lv_color_eq(fill, kSurface)) {
            // Opaque pre-dimmed city replaces per-row alpha blending.
            draw_city(layer, 1);
        }
        lv_draw_border_dsc_t border;
        lv_draw_border_dsc_init(&border);
        border.color = color;
        border.width = 1;
        border.opa = LV_OPA_COVER;
        border.side = LV_BORDER_SIDE_FULL;
        lv_draw_border(layer, &border, &area);
        // Two corner accents retain the angular HUD treatment with three
        // draw tasks, instead of eight/nine line tasks on every visible row.
        hud_line(layer, left, top + cut, left + cut, top, color);
        hud_line(layer, right - cut, bottom, right, bottom - cut, color);
        return;
    }
    const lv_point_t points[] = {
        {left + cut, top}, {right - cut, top}, {right, top + cut},
        {right, bottom - cut}, {right - cut, bottom}, {left + cut, bottom},
        {left, bottom - cut}, {left, top + cut}, {left + cut, top},
    };
    for (size_t i = 1; i < sizeof(points) / sizeof(points[0]); ++i) {
        hud_line(layer, points[i - 1].x, points[i - 1].y,
                  points[i].x, points[i].y, color);
    }
}

bool load_city(size_t variant)
{
    if (s_city_pixels[variant]) return true;
    constexpr size_t bytes = kScreenWidth * kScreenHeight * sizeof(uint16_t);
    auto *pixels = static_cast<uint16_t *>(heap_caps_malloc(
        bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    const uint8_t *start = variant == 0 ? city_png_start : city_lists_png_start;
    const uint8_t *end = variant == 0 ? city_png_end : city_lists_png_end;
    if (!pixels || !lyra::media::decode_png(start,
            static_cast<size_t>(end - start), pixels,
            kScreenWidth, kScreenHeight, false, 0x05080F)) {
        heap_caps_free(pixels);
        ESP_LOGW(kTag, "HUD city unavailable; using the HUD palette only");
        return false;
    }
    s_city_pixels[variant] = pixels;
    lv_image_dsc_t &image = s_city_images[variant];
    image.header.magic = LV_IMAGE_HEADER_MAGIC;
    image.header.cf = LV_COLOR_FORMAT_RGB565;
    image.header.w = kScreenWidth;
    image.header.h = kScreenHeight;
    image.header.stride = kScreenWidth * sizeof(uint16_t);
    image.data_size = bytes;
    image.data = reinterpret_cast<const uint8_t *>(pixels);
    return true;
}

void hud_equalizer_grid_cb(lv_event_t *event)
{
    lv_area_t area;
    lv_obj_get_coords(lv_event_get_current_target_obj(event), &area);
    lv_layer_t *layer = lv_event_get_layer(event);
    for (int step = 0; step <= 4; ++step) {
        const int y = area.y1 + step * (lv_area_get_height(&area) - 1) / 4;
        hud_line(layer, area.x1, y, area.x2, y, kAccent, LV_OPA_30);
    }
    for (size_t band = 0; band < lyra::audio::kEqualizerBandCount; ++band) {
        const int x = area.x1 + 9 + static_cast<int>(band) * 52;
        hud_line(layer, x, area.y1, x, area.y2, kAccent, LV_OPA_30);
    }
}

} // namespace

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

void make_hud_background()
{
    // style_root has already deleted every object that referenced the image.
    if (!s_neon_hud || s_view == View::FullscreenInfoArt) {
        for (size_t variant = 0; variant < 2; ++variant) {
            if (!s_city_pixels[variant]) continue;
            lv_image_cache_drop(&s_city_images[variant]);
            heap_caps_free(s_city_pixels[variant]);
            s_city_pixels[variant] = nullptr;
            s_city_images[variant] = {};
        }
        return;
    }
    s_city_variant = major_hud_view() ? 0 : 1;
    load_city(s_city_variant);
    if (major_hud_view()) load_city(1);
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
    if (!s_neon_hud) return;
    lv_obj_t *grid = make_box(parent, x, y, width, height, kBackground);
    lv_obj_set_style_bg_opa(grid, LV_OPA_COVER, 0);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(grid, hud_equalizer_grid_cb, LV_EVENT_DRAW_MAIN, nullptr);
}

void style_hud_controls(lv_obj_t *object)
{
    if (!s_neon_hud) return;
    // Directly-created controls (search keyboards, sliders and text areas)
    // share the same geometry while retaining their original sizes and events.
    if (lv_obj_has_class(object, &lv_slider_class) || lv_obj_has_class(object, &lv_bar_class)) {
        lv_obj_set_style_radius(object, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(object, 0, LV_PART_INDICATOR);
        lv_obj_set_style_radius(object, 0, LV_PART_KNOB);
        lv_obj_set_style_bg_color(object, kAccent, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(object, kAccent, LV_PART_KNOB);
        lv_obj_set_style_border_width(object, 1, LV_PART_KNOB);
        lv_obj_set_style_border_color(object, kAccent, LV_PART_KNOB);
    } else if (lv_obj_has_class(object, &lv_buttonmatrix_class)) {
        lv_obj_set_style_radius(object, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(object, 0, LV_PART_ITEMS);
        lv_obj_set_style_bg_color(object, kSurface, LV_PART_ITEMS);
        lv_obj_set_style_text_color(object, kTextSecondary, LV_PART_ITEMS);
        lv_obj_set_style_border_width(object, 1, LV_PART_ITEMS);
        lv_obj_set_style_border_color(object, kDivider, LV_PART_ITEMS);
        const lv_style_selector_t pressed_items =
            static_cast<lv_style_selector_t>(LV_PART_ITEMS) |
            static_cast<lv_style_selector_t>(LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(object, kAccentSurface, pressed_items);
        lv_obj_set_style_border_color(object, kAccent, pressed_items);
    } else if (lv_obj_has_class(object, &lv_textarea_class)) {
        lv_obj_set_style_radius(object, 0, 0);
        lv_obj_set_style_border_width(object, 1, 0);
        lv_obj_set_style_border_color(object, kAccent, 0);
    }
    const uint32_t count = lv_obj_get_child_count(object);
    for (uint32_t i = 0; i < count; ++i) style_hud_controls(lv_obj_get_child(object, i));
}

} // namespace lyra::gui::internal
