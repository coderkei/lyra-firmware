/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../lyra_gui_internal.h"
#include "src/misc/cache/instance/lv_image_cache.h"

namespace lyra::gui::internal {
namespace {

extern const uint8_t city_png_start[] asm("_binary_neon_hud_city_png_start");
extern const uint8_t city_png_end[] asm("_binary_neon_hud_city_png_end");
uint16_t *s_city_pixels = nullptr;
lv_image_dsc_t s_city_image{};

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
    const lv_point_t points[] = {
        {left + cut, top}, {right - cut, top}, {right, top + cut},
        {right, bottom - cut}, {right - cut, bottom}, {left + cut, bottom},
        {left, bottom - cut}, {left, top + cut}, {left + cut, top},
    };
    for (size_t i = 1; i < sizeof(points) / sizeof(points[0]); ++i) {
        hud_line(layer, points[i - 1].x, points[i - 1].y,
                  points[i].x, points[i].y, color);
    }
    if (active && !octagonal && lv_area_get_width(&area) > 100) {
        hud_line(layer, left + 3, top + cut + 2, left + 3, bottom - cut - 2, kAccent);
    }
}

void hud_backdrop_cb(lv_event_t *event)
{
    lv_obj_t *object = lv_event_get_current_target_obj(event);
    lv_area_t area;
    lv_obj_get_coords(object, &area);
    lv_layer_t *layer = lv_event_get_layer(event);
    for (int x = 12; x < kScreenWidth; x += 32) {
        hud_line(layer, area.x1 + x, area.y1 + kStatusHeight,
                  area.x1 + x, area.y2, kTextSecondary, LV_OPA_10);
    }
    for (int y = kStatusHeight + 12; y < kScreenHeight; y += 32) {
        hud_line(layer, area.x1, area.y1 + y, area.x2, area.y1 + y, kAccent, LV_OPA_10);
    }
    // Short circuit traces and corner registration marks, drawn behind UI.
    const int bottom = area.y1 + content_bottom() - 2;
    hud_line(layer, area.x1 + 3, area.y1 + 76, area.x1 + 3, bottom, kDivider);
    hud_line(layer, area.x2 - 3, area.y1 + 76, area.x2 - 3, bottom, kDivider);
    hud_line(layer, area.x1 + 3, bottom - 10, area.x1 + 13, bottom, kAccent);
    hud_line(layer, area.x1 + 13, bottom, area.x1 + 37, bottom, kAccent);
    hud_line(layer, area.x2 - 37, bottom, area.x2 - 13, bottom, kAccent);
    hud_line(layer, area.x2 - 13, bottom, area.x2 - 3, bottom - 10, kAccent);
    hud_line(layer, area.x2 - 24, area.y1 + 83, area.x2 - 12, area.y1 + 83, kDivider);
    hud_line(layer, area.x2 - 12, area.y1 + 83, area.x2 - 3, area.y1 + 92, kDivider);
    for (int y = 96; y < bottom - 12; y += 48) {
        hud_line(layer, area.x1 + 3, area.y1 + y, area.x1 + 7, area.y1 + y, kAccent);
    }
}

bool load_city()
{
    if (s_city_pixels) return true;
    constexpr size_t bytes = kScreenWidth * kScreenHeight * sizeof(uint16_t);
    auto *pixels = static_cast<uint16_t *>(heap_caps_malloc(
        bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!pixels || !lyra::media::decode_png(city_png_start,
            static_cast<size_t>(city_png_end - city_png_start), pixels,
            kScreenWidth, kScreenHeight, false, 0x05080F)) {
        heap_caps_free(pixels);
        ESP_LOGW(kTag, "HUD city unavailable; using the HUD palette only");
        return false;
    }
    s_city_pixels = pixels;
    s_city_image.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_city_image.header.cf = LV_COLOR_FORMAT_RGB565;
    s_city_image.header.w = kScreenWidth;
    s_city_image.header.h = kScreenHeight;
    s_city_image.header.stride = kScreenWidth * sizeof(uint16_t);
    s_city_image.data_size = bytes;
    s_city_image.data = reinterpret_cast<const uint8_t *>(pixels);
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
    lv_obj_add_event_cb(object, hud_frame_cb, LV_EVENT_DRAW_MAIN,
                        octagonal ? reinterpret_cast<void *>(1) : nullptr);
}

void make_hud_background()
{
    // style_root has already deleted every object that referenced the image.
    if (!s_neon_hud || s_view == View::FullscreenInfoArt) {
        if (s_city_pixels) {
            lv_image_cache_drop(&s_city_image);
            heap_caps_free(s_city_pixels);
            s_city_pixels = nullptr;
            s_city_image = {};
        }
        return;
    }
    const bool major = s_view == View::Player || s_view == View::Menu ||
        s_view == View::Library || s_view == View::Settings;
    lv_obj_t *backdrop = lv_obj_create(s_screen);
    lv_obj_set_pos(backdrop, 0, 0);
    lv_obj_set_size(backdrop, kScreenWidth, kScreenHeight);
    lv_obj_set_style_pad_all(backdrop, 0, 0);
    lv_obj_set_style_radius(backdrop, 0, 0);
    lv_obj_set_style_border_width(backdrop, 0, 0);
    lv_obj_set_style_bg_color(backdrop, kBackground, 0);
    lv_obj_set_style_bg_opa(backdrop, LV_OPA_COVER, 0);
    if (load_city()) {
        // LVGL draws this before DRAW_MAIN, allowing the grid to stay on top.
        lv_obj_set_style_bg_image_src(backdrop, &s_city_image, 0);
        lv_obj_set_style_bg_image_opa(backdrop, major ? LV_OPA_80 : LV_OPA_40, 0);
    }
    lv_obj_remove_flag(backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(backdrop, hud_backdrop_cb, LV_EVENT_DRAW_MAIN, nullptr);
}

void make_hud_equalizer_grid(lv_obj_t *parent, int x, int y, int width, int height)
{
    if (!s_neon_hud) return;
    lv_obj_t *grid = make_box(parent, x, y, width, height, kBackground);
    lv_obj_set_style_bg_opa(grid, LV_OPA_80, 0);
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
