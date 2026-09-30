/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../lyra_gui_internal.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "src/draw/lv_draw_triangle.h"

namespace lyra::gui::internal {
namespace {

extern const uint8_t cute_png_start[] asm("_binary_cute_pink_background_png_start");
extern const uint8_t cute_png_end[] asm("_binary_cute_pink_background_png_end");
uint16_t *s_cute_pixels = nullptr;
lv_image_dsc_t s_cute_image{};

void cute_plastic_cb(lv_event_t *event)
{
    lv_obj_t *object = lv_event_get_current_target_obj(event);
    lv_area_t area;
    lv_obj_get_coords(object, &area);
    if (lv_area_get_width(&area) < 12 || lv_area_get_height(&area) < 12) return;
    const int32_t radius = std::min<int32_t>(
        lv_obj_get_style_radius(object, LV_PART_MAIN),
        std::min(lv_area_get_width(&area), lv_area_get_height(&area)) / 2);
    // Two narrow native borders give the solid surface a molded rim, without
    // blurred shadows, gradients, image copies or additional child widgets.
    area.x1 += 2;
    area.y1 += 2;
    area.x2 -= 2;
    area.y2 -= 2;
    lv_draw_border_dsc_t rim;
    lv_draw_border_dsc_init(&rim);
    rim.radius = std::max<int32_t>(radius - 2, 0);
    rim.width = 1;
    rim.opa = LV_OPA_COVER;
    rim.color = lv_color_hex(0xFFFFFF);
    rim.side = LV_BORDER_SIDE_FULL;
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_draw_border(layer, &rim, &area);
    const bool pressed = lv_obj_has_state(object, LV_STATE_PRESSED);
    rim.color = lv_color_hex(pressed ? 0xE2A7D0 : 0xDCCBED);
    rim.side = static_cast<lv_border_side_t>(pressed ?
        LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_LEFT :
        LV_BORDER_SIDE_BOTTOM | LV_BORDER_SIDE_RIGHT);
    lv_draw_border(layer, &rim, &area);
}

// Coordinates are local to a 32 px sticker. Each primitive has a solid fill
// or a rounded stroke; icons introduce no image buffers or hit targets.
struct CuteIconPen {
    lv_layer_t *layer;
    int32_t x;
    int32_t y;

    void box(int32_t dx, int32_t dy, int32_t width, int32_t height,
             int32_t radius, uint32_t fill, uint32_t edge = 0) const
    {
        lv_draw_rect_dsc_t rect;
        lv_draw_rect_dsc_init(&rect);
        rect.radius = radius;
        rect.bg_color = lv_color_hex(fill);
        rect.bg_opa = LV_OPA_COVER;
        rect.border_color = lv_color_hex(edge);
        rect.border_width = edge ? 1 : 0;
        rect.border_opa = LV_OPA_COVER;
        const lv_area_t area{x + dx, y + dy, x + dx + width - 1, y + dy + height - 1};
        lv_draw_rect(layer, &rect, &area);
    }

    void line(int32_t x1, int32_t y1, int32_t x2, int32_t y2,
              uint32_t color, int32_t width = 2) const
    {
        lv_draw_line_dsc_t stroke;
        lv_draw_line_dsc_init(&stroke);
        stroke.p1 = {static_cast<lv_value_precise_t>(x + x1), static_cast<lv_value_precise_t>(y + y1)};
        stroke.p2 = {static_cast<lv_value_precise_t>(x + x2), static_cast<lv_value_precise_t>(y + y2)};
        stroke.color = lv_color_hex(color);
        stroke.width = width;
        stroke.round_start = true;
        stroke.round_end = true;
        lv_draw_line(layer, &stroke);
    }

    void arc(int32_t dx, int32_t dy, uint16_t radius, uint32_t color,
             int32_t start = 0, int32_t end = 360, int32_t width = 2) const
    {
        lv_draw_arc_dsc_t curve;
        lv_draw_arc_dsc_init(&curve);
        curve.center = {x + dx, y + dy};
        curve.radius = radius;
        curve.color = lv_color_hex(color);
        curve.start_angle = static_cast<lv_value_precise_t>(start);
        curve.end_angle = static_cast<lv_value_precise_t>(end);
        curve.width = width;
        curve.rounded = true;
        lv_draw_arc(layer, &curve);
    }

    void face(int32_t dx, int32_t dy) const
    {
        box(dx, dy, 2, 2, 1, 0x665399);
        box(dx + 6, dy, 2, 2, 1, 0x665399);
        arc(dx + 4, dy + 2, 3, 0xA76CAB, 30, 150, 1);
    }
};

void cute_menu_icon_cb(lv_event_t *event)
{
    lv_area_t bounds;
    lv_obj_get_coords(lv_event_get_current_target_obj(event), &bounds);
    const CuteIconPen pen{lv_event_get_layer(event), bounds.x1, bounds.y1};
    const View view = static_cast<View>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    constexpr uint32_t purple = 0x9472BB;
    constexpr uint32_t pink = 0xE889C1;
    pen.box(1, 1, 30, 30, 10, 0xFFFFFF, 0xE7D4F3);
    switch (view) {
        case View::Player:
            // A candy-pink eighth note with a little white reflection.
            pen.box(7, 19, 9, 7, 4, 0xF3B1D8, purple);
            pen.box(20, 16, 8, 7, 4, 0xF3B1D8, purple);
            pen.line(15, 8, 15, 22, purple);
            pen.line(27, 5, 27, 19, purple);
            pen.line(15, 8, 27, 5, pink, 3);
            pen.line(10, 21, 12, 21, 0xFFFFFF, 1);
            break;
        case View::Library:
            // A lavender vinyl record, pink label and a blue glint.
            pen.box(5, 5, 23, 23, LV_RADIUS_CIRCLE, 0xD7C5F5, purple);
            pen.arc(16, 16, 8, 0xEFE5FF, 200, 320, 1);
            pen.box(12, 12, 9, 9, LV_RADIUS_CIRCLE, 0xFFC8E5, purple);
            pen.box(15, 15, 3, 3, LV_RADIUS_CIRCLE, 0xFFFFFF);
            pen.line(9, 9, 11, 7, 0xFFFFFF, 2);
            break;
        case View::Folders:
            // A smiling folder, with a raised tab and pink cheeks.
            pen.box(5, 7, 12, 8, 3, 0xD8CAF6, purple);
            pen.box(4, 11, 25, 16, 4, 0xC9DFFB, purple);
            pen.line(7, 13, 24, 13, 0xFFFFFF, 1);
            pen.face(12, 18);
            pen.box(7, 21, 3, 2, 1, 0xF4B6D8);
            pen.box(23, 21, 3, 2, 1, 0xF4B6D8);
            break;
        case View::Queue:
            // Mint playlist sticker with pink round bullet beads.
            pen.box(5, 5, 23, 23, 5, 0xCBEEE4, purple);
            for (int32_t row = 0; row < 3; ++row) {
                pen.box(8, 9 + row * 6, 4, 4, 2, 0xF0ABD4);
                pen.line(16, 11 + row * 6, 24, 11 + row * 6, purple, 2);
            }
            break;
        case View::Equalizer:
            // Toy-like sliders and round candy knobs.
            pen.box(4, 4, 25, 25, 6, 0xE3DBFA, purple);
            for (int32_t band = 0; band < 3; ++band) {
                const int32_t x = 9 + band * 7;
                pen.line(x, 8, x, 25, purple, 2);
                pen.box(x - 2, band == 1 ? 9 : 17, 5, 5, 3, 0xF3B1D8, 0xFFFFFF);
            }
            break;
        case View::Search:
            // Baby-blue magnifying glass with a tiny pink sparkle.
            pen.line(21, 21, 27, 27, purple, 4);
            pen.box(5, 5, 20, 20, LV_RADIUS_CIRCLE, 0xCDEAFF, purple);
            pen.box(8, 8, 14, 14, LV_RADIUS_CIRCLE, 0xF3FAFF);
            pen.line(11, 11, 14, 9, 0xFFFFFF, 2);
            pen.line(25, 4, 25, 8, pink, 1);
            pen.line(23, 6, 27, 6, pink, 1);
            break;
        case View::Settings: {
            // Six soft petals replace the angular stock settings cog.
            constexpr int32_t petals[][2] = {{13, 3}, {21, 8}, {21, 18},
                                             {13, 23}, {4, 18}, {4, 8}};
            for (const auto &petal : petals) {
                pen.box(petal[0], petal[1], 8, 8, 4, 0xDCCAF5, purple);
            }
            pen.box(9, 9, 15, 15, LV_RADIUS_CIRCLE, 0xFFD1E6, purple);
            pen.face(12, 14);
            break;
        }
        default: break;
    }
}

void cute_mascot_cb(lv_event_t *event)
{
    lv_area_t bounds;
    lv_obj_get_coords(lv_event_get_current_target_obj(event), &bounds);
    lv_layer_t *layer = lv_event_get_layer(event);
    const int32_t x = bounds.x1, y = bounds.y1;
    // Small cat face using native geometry, without another bitmap or font.
    for (int32_t side = 0; side < 2; ++side) {
        lv_draw_triangle_dsc_t ear;
        lv_draw_triangle_dsc_init(&ear);
        const int32_t left = x + 2 + side * 13;
        ear.p[0] = {static_cast<lv_value_precise_t>(left), static_cast<lv_value_precise_t>(y + 1)};
        ear.p[1] = {static_cast<lv_value_precise_t>(left + 7), static_cast<lv_value_precise_t>(y + 8)};
        ear.p[2] = {static_cast<lv_value_precise_t>(left), static_cast<lv_value_precise_t>(y + 11)};
        ear.color = kAccentSurface;
        ear.opa = LV_OPA_COVER;
        lv_draw_triangle(layer, &ear);
    }
    const auto dot = [&](int32_t dx, int32_t dy, int32_t width, int32_t height, lv_color_t color) {
        lv_draw_rect_dsc_t rect;
        lv_draw_rect_dsc_init(&rect);
        rect.bg_color = color;
        rect.bg_opa = LV_OPA_COVER;
        rect.radius = LV_RADIUS_CIRCLE;
        const lv_area_t area{x + dx, y + dy, x + dx + width - 1, y + dy + height - 1};
        lv_draw_rect(layer, &rect, &area);
    };
    dot(0, 6, 24, 17, kDivider);
    dot(1, 7, 22, 15, kSurface);
    dot(5, 12, 3, 2, kTextPrimary);
    dot(16, 12, 3, 2, kTextPrimary);
    dot(2, 16, 4, 2, kAccentSurface);
    dot(18, 16, 4, 2, kAccentSurface);
    dot(11, 16, 2, 2, kAccent);
}

void cute_background_cb(lv_event_t *event)
{
    if (!s_cute_pixels) return;
    lv_area_t area;
    lv_obj_get_coords(s_screen, &area);
    lv_draw_image_dsc_t image;
    lv_draw_image_dsc_init(&image);
    image.src = &s_cute_image;
    image.opa = LV_OPA_COVER;
    image.antialias = false;
    // Fixed screen origin, clipped to the opaque page viewport by LVGL.
    // Scrolling copies RGB565 pixels without decoding or alpha blending.
    lv_draw_image(lv_event_get_layer(event), &image, &area);
}

bool load_cute_background()
{
    if (s_cute_pixels) return true;
    constexpr size_t bytes = kScreenWidth * kScreenHeight * sizeof(uint16_t);
    auto *pixels = static_cast<uint16_t *>(heap_caps_malloc(
        bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!pixels || !lyra::media::decode_png(cute_png_start,
            static_cast<size_t>(cute_png_end - cute_png_start), pixels,
            kScreenWidth, kScreenHeight, false, 0xFFF5FC)) {
        heap_caps_free(pixels);
        ESP_LOGW(kTag, "Cute background unavailable; using the pastel palette only");
        return false;
    }
    s_cute_pixels = pixels;
    s_cute_image.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_cute_image.header.cf = LV_COLOR_FORMAT_RGB565;
    s_cute_image.header.w = kScreenWidth;
    s_cute_image.header.h = kScreenHeight;
    s_cute_image.header.stride = kScreenWidth * sizeof(uint16_t);
    s_cute_image.data_size = bytes;
    s_cute_image.data = reinterpret_cast<const uint8_t *>(pixels);
    return true;
}

} // namespace

void make_cute_background()
{
    // style_root has deleted all previous objects before releasing the image.
    if (!is_cute_theme() || s_view == View::FullscreenInfoArt) {
        if (s_cute_pixels) {
            lv_image_cache_drop(&s_cute_image);
            heap_caps_free(s_cute_pixels);
            s_cute_pixels = nullptr;
            s_cute_image = {};
        }
        return;
    }
    if (!load_cute_background()) return;
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
    lv_obj_add_event_cb(backdrop, cute_background_cb, LV_EVENT_DRAW_MAIN, nullptr);
}

void make_cute_body_background(lv_obj_t *body)
{
    if (!is_cute_theme()) return;
    if (lv_obj_get_parent(body) != s_screen) {
        lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
        return;
    }
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
    lv_obj_set_style_outline_width(body, 0, 0);
    lv_obj_set_style_shadow_width(body, 0, 0);
    lv_obj_add_event_cb(body, cute_background_cb, LV_EVENT_DRAW_MAIN, nullptr);
}

void add_cute_plastic_finish(lv_obj_t *object)
{
    if (!is_cute_theme()) return;
    lv_obj_set_style_outline_width(object, 1, 0);
    lv_obj_set_style_outline_pad(object, 0, 0);
    lv_obj_set_style_outline_color(object, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_outline_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(object, 0, 0);
    lv_obj_remove_event_cb(object, cute_plastic_cb);
    lv_obj_add_event_cb(object, cute_plastic_cb, LV_EVENT_DRAW_MAIN, nullptr);
}

void make_cute_menu_icon(lv_obj_t *row, View view)
{
    if (!is_cute_theme()) return;
    lv_obj_t *icon = make_box(row, 11, 9, 32, 32, kBackground);
    lv_obj_set_style_bg_opa(icon, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(icon, 0, 0);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(icon, cute_menu_icon_cb, LV_EVENT_DRAW_MAIN,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(view)));
}

void style_cute_controls(lv_obj_t *object)
{
    if (!is_cute_theme()) return;
    if (lv_obj_has_class(object, &lv_slider_class) || lv_obj_has_class(object, &lv_bar_class)) {
        lv_obj_set_style_radius(object, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_radius(object, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(object, kDivider, LV_PART_MAIN);
        lv_obj_set_style_bg_color(object, kAccent, LV_PART_INDICATOR);
        if (lv_obj_has_class(object, &lv_slider_class)) {
            lv_obj_set_style_radius(object, LV_RADIUS_CIRCLE, LV_PART_KNOB);
            lv_obj_set_style_bg_color(object, kSurface, LV_PART_KNOB);
            lv_obj_set_style_border_width(object, 2, LV_PART_KNOB);
            lv_obj_set_style_border_color(object, kAccent, LV_PART_KNOB);
        }
    } else if (lv_obj_has_class(object, &lv_buttonmatrix_class)) {
        lv_obj_set_style_radius(object, 12, LV_PART_MAIN);
        lv_obj_set_style_radius(object, 10, LV_PART_ITEMS);
        lv_obj_set_style_bg_color(object, kSurface, LV_PART_ITEMS);
        lv_obj_set_style_text_color(object, kTextPrimary, LV_PART_ITEMS);
        lv_obj_set_style_border_width(object, 1, LV_PART_ITEMS);
        lv_obj_set_style_border_color(object, kDivider, LV_PART_ITEMS);
        const lv_style_selector_t pressed = static_cast<lv_style_selector_t>(LV_PART_ITEMS) |
            static_cast<lv_style_selector_t>(LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(object, kAccentSurface, pressed);
        lv_obj_set_style_border_color(object, kAccent, pressed);
    } else if (lv_obj_has_class(object, &lv_textarea_class)) {
        lv_obj_set_style_radius(object, 12, 0);
        lv_obj_set_style_bg_color(object, kSurface, 0);
        lv_obj_set_style_text_color(object, kTextPrimary, 0);
        lv_obj_set_style_border_width(object, 1, 0);
        lv_obj_set_style_border_color(object, kDivider, 0);
    }
    const uint32_t count = lv_obj_get_child_count(object);
    for (uint32_t i = 0; i < count; ++i) style_cute_controls(lv_obj_get_child(object, i));
}

void make_cute_header_mascot(lv_obj_t *header)
{
    if (!is_cute_theme()) return;
    // The title marquee ends at x=290; this uses the remaining header gutter.
    lv_obj_t *mascot = make_box(header, 292, 10, 24, 24, kBackground);
    lv_obj_set_style_bg_opa(mascot, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(mascot, 0, 0);
    lv_obj_remove_flag(mascot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(mascot, cute_mascot_cb, LV_EVENT_DRAW_MAIN, nullptr);
}

} // namespace lyra::gui::internal
