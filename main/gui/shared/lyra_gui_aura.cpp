/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../lyra_gui_internal.h"
#include "lyra_gui_draw.h"
#include "src/misc/cache/instance/lv_image_cache.h"

namespace lyra::gui::internal {
namespace {
lv_obj_t *s_aura_play_icon = nullptr;
uint16_t *s_aura_pixels = nullptr;
lv_image_dsc_t s_aura_image{};
constexpr uint32_t kAuraSkyTop = 0x5DB8EE;
constexpr uint32_t kAuraSkyBottom = 0xE4F8FD;

struct Bubble { int x, y, r; };
// Soap bubbles drift across the sky, the Frutiger Aero signature.
constexpr Bubble kBubbles[] = {
    {264, 118, 26}, {38, 212, 14}, {292, 262, 10}, {72, 92, 8},
    {222, 318, 18}, {146, 44, 6}, {28, 318, 9}, {184, 200, 5},
};

void draw_aura_light(lv_layer_t *layer, const lv_area_t &area)
{
    for (int trail = 0; trail < 3; ++trail) {
        for (int segment = 0; segment < 16; ++segment) {
            const int x1 = segment * 20;
            const int x2 = x1 + 20;
            const auto curve_y = [trail](int x) {
                return 420 - trail * 26 - x / 5 - x * x / 600;
            };
            lv_draw_line_dsc_t line;
            lv_draw_line_dsc_init(&line);
            line.p1 = {static_cast<lv_value_precise_t>(area.x1 + x1),
                       static_cast<lv_value_precise_t>(area.y1 + curve_y(x1))};
            line.p2 = {static_cast<lv_value_precise_t>(area.x1 + x2),
                       static_cast<lv_value_precise_t>(area.y1 + curve_y(x2))};
            line.round_start = line.round_end = true;
            line.color = lv_color_hex(trail == 1 ? 0xF3FFD0 : 0xFFFFFF);
            line.width = 9;
            line.opa = LV_OPA_10;
            gui_draw_line(layer, &line);
            line.width = 2;
            line.opa = LV_OPA_50;
            gui_draw_line(layer, &line);
        }
    }
}

// Fallback when PSRAM cannot hold the baked wallpaper: draw the trails live.
void aura_light_cb(lv_event_t *event)
{
    lv_area_t area;
    lv_obj_get_coords(lv_event_get_current_target_obj(event), &area);
    draw_aura_light(lv_event_get_layer(event), area);
}

void aura_disc(lv_layer_t *layer, int x, int y, int r, uint32_t rgb, lv_opa_t opa,
               uint32_t border_rgb = 0, lv_opa_t border_opa = LV_OPA_TRANSP)
{
    lv_draw_rect_dsc_t disc;
    lv_draw_rect_dsc_init(&disc);
    disc.radius = LV_RADIUS_CIRCLE;
    disc.bg_color = lv_color_hex(rgb);
    disc.bg_opa = opa;
    disc.border_color = lv_color_hex(border_rgb);
    disc.border_opa = border_opa;
    disc.border_width = border_opa == LV_OPA_TRANSP ? 0 : 1;
    const lv_area_t area{x - r, y - r, x + r, y + r};
    lv_draw_rect(layer, &disc, &area);
}

struct Hill { int cx, cy, r; uint32_t top, bottom; };
// A cooler hill behind a bright grass one; only their crowns are on screen.
constexpr Hill kHills[] = {
    {420, 708, 360, 0x8FD9A8, 0x3E9C6C},
    {70, 760, 380, 0xA6E46A, 0x4FAE34},
};

uint32_t mix_rgb(uint32_t from, uint32_t to, int alpha)
{
    const auto channel = [&](int shift) {
        const int a = (from >> shift) & 0xFF, b = (to >> shift) & 0xFF;
        return static_cast<uint32_t>(a + (b - a) * alpha / 256) << shift;
    };
    return channel(16) | channel(8) | channel(0);
}

// Hills and sun glare are painted per pixel into the cached frame. Drawing
// them as LVGL circles would leave large masks in LVGL's internal-RAM circle
// cache, which the audio output task needs.
void paint_aura_scenery(uint16_t *pixels)
{
    for (int y = 0; y < kScreenHeight; ++y) {
        for (int x = 0; x < kScreenWidth; ++x) {
            uint16_t &pixel = pixels[y * kScreenWidth + x];
            uint32_t rgb = ((pixel >> 11) & 0x1F) * 255 / 31 << 16 |
                           ((pixel >> 5) & 0x3F) * 255 / 63 << 8 | (pixel & 0x1F) * 255 / 31;
            bool changed = false;
            const int gx = x - 34, gy = y - 26;
            const int glare = gx * gx + gy * gy;
            if (glare < 120 * 120) {
                const int falloff = 256 - glare * 256 / (120 * 120);
                rgb = mix_rgb(rgb, 0xFFFFFF, falloff * falloff / 256 * 200 / 256);
                changed = true;
            }
            for (const Hill &hill : kHills) {
                const int dx = x - hill.cx, dy = y - hill.cy;
                const int inside = hill.r * hill.r - (dx * dx + dy * dy);
                if (inside < 0) continue;
                const int depth = std::min(256, (y - (hill.cy - hill.r)) * 256 / (hill.r / 3));
                rgb = mix_rgb(hill.top, hill.bottom, depth);
                // About 3 px of specular sheen along the crest.
                if (inside < hill.r * 6) rgb = mix_rgb(rgb, 0xF2FFE8, 150);
                changed = true;
            }
            if (changed) {
                pixel = static_cast<uint16_t>((((rgb >> 16) & 0xFF) >> 3) << 11 |
                                              (((rgb >> 8) & 0xFF) >> 2) << 5 | ((rgb & 0xFF) >> 3));
            }
        }
    }
}

void paint_aura_wallpaper(lv_layer_t *layer, void *)
{
    lv_draw_rect_dsc_t sky;
    lv_draw_rect_dsc_init(&sky);
    sky.bg_opa = LV_OPA_COVER;
    sky.bg_color = lv_color_hex(kAuraSkyTop);
    sky.bg_grad.dir = LV_GRAD_DIR_VER;
    sky.bg_grad.stops_count = 2;
    sky.bg_grad.stops[0] = {lv_color_hex(kAuraSkyTop), LV_OPA_COVER, 0};
    sky.bg_grad.stops[1] = {lv_color_hex(kAuraSkyBottom), LV_OPA_COVER, 255};
    const lv_area_t area{0, 0, kScreenWidth - 1, kScreenHeight - 1};
    lv_draw_rect(layer, &sky, &area);
    draw_aura_light(layer, area);
    for (const Bubble &bubble : kBubbles) {
        aura_disc(layer, bubble.x, bubble.y, bubble.r, 0xFFFFFF, LV_OPA_10, 0xFFFFFF, LV_OPA_70);
        const int glint = std::max(1, bubble.r / 4);
        aura_disc(layer, bubble.x - bubble.r / 3, bubble.y - bubble.r / 3, glint, 0xFFFFFF, LV_OPA_80);
    }
}

bool load_aura_wallpaper()
{
    if (s_aura_pixels) return true;
    constexpr size_t bytes = kScreenWidth * kScreenHeight * sizeof(uint16_t);
    auto *pixels = static_cast<uint16_t *>(heap_caps_malloc(
        bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!pixels || !rasterize_rgb565(pixels, kScreenWidth, kScreenHeight, paint_aura_wallpaper)) {
        heap_caps_free(pixels);
        ESP_LOGW(kTag, "Aura wallpaper cache unavailable; drawing it live");
        return false;
    }
    paint_aura_scenery(pixels);
    s_aura_pixels = pixels;
    init_rgb565_image(&s_aura_image, pixels, kScreenWidth, kScreenHeight);
    return true;
}

void release_aura_wallpaper()
{
    if (!s_aura_pixels) return;
    lv_image_cache_drop(&s_aura_image);
    heap_caps_free(s_aura_pixels);
    s_aura_pixels = nullptr;
    s_aura_image = {};
}

void aura_wallpaper_cb(lv_event_t *event)
{
    if (!s_aura_pixels) return;
    lv_area_t area;
    lv_obj_get_coords(s_screen, &area);
    lv_draw_image_dsc_t image;
    lv_draw_image_dsc_init(&image);
    image.src = &s_aura_image;
    image.opa = LV_OPA_COVER;
    image.antialias = false;
    // One opaque RGB565 copy replaces the gradient and 96 blended line tasks
    // that used to be rasterized again under every scrolled or updated area.
    lv_draw_image(lv_event_get_layer(event), &image, &area);
}

void aura_grid_cb(lv_event_t *event)
{
    lv_area_t area;
    lv_obj_get_coords(lv_event_get_current_target_obj(event), &area);
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = lv_color_hex(0xFFFFFF);
    line.opa = LV_OPA_70;
    line.width = 1;
    for (int row = 0; row <= 6; ++row) {
        const int y = area.y1 + 14 + row * 39;
        line.p1 = {static_cast<lv_value_precise_t>(area.x1 + 36), static_cast<lv_value_precise_t>(y)};
        line.p2 = {static_cast<lv_value_precise_t>(area.x1 + 284), static_cast<lv_value_precise_t>(y)};
        gui_draw_line(lv_event_get_layer(event), &line);
    }
    for (int band = 0; band < 5; ++band) {
        const int x = area.x1 + 49 + band * 52;
        line.p1 = {static_cast<lv_value_precise_t>(x), static_cast<lv_value_precise_t>(area.y1 + 14)};
        line.p2 = {static_cast<lv_value_precise_t>(x), static_cast<lv_value_precise_t>(area.y1 + 248)};
        gui_draw_line(lv_event_get_layer(event), &line);
    }
}

void aura_rim_cb(lv_event_t *event)
{
    lv_obj_t *object = lv_event_get_current_target_obj(event);
    lv_area_t area;
    lv_obj_get_coords(object, &area);
    area.x1 += 2;
    area.y1 += 2;
    area.x2 -= 2;
    area.y2 -= 2;
    lv_draw_border_dsc_t rim;
    lv_draw_border_dsc_init(&rim);
    rim.radius = std::max<int32_t>(0, std::min<int32_t>(
        lv_obj_get_style_radius(object, LV_PART_MAIN),
        std::min(lv_area_get_width(&area), lv_area_get_height(&area)) / 2) - 2);
    rim.width = 1;
    rim.color = lv_color_hex(0xFFFFFF);
    rim.opa = LV_OPA_70;
    rim.side = static_cast<lv_border_side_t>(LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_LEFT);
    gui_draw_border(lv_event_get_layer(event), &rim, &area);
}

void gradient(lv_obj_t *object, uint32_t top, uint32_t bottom, lv_style_selector_t part)
{
    lv_obj_set_style_bg_color(object, lv_color_hex(top), part);
    lv_obj_set_style_bg_grad_color(object, lv_color_hex(bottom), part);
    lv_obj_set_style_bg_grad_dir(object, LV_GRAD_DIR_VER, part);
    lv_obj_set_style_bg_main_stop(object, 0, part);
    lv_obj_set_style_bg_grad_stop(object, 255, part);
}

// Aero glass: a pale upper half breaking sharply into a deeper lower half,
// like the gloss on Windows Media Player's controls. Same cost as gradient().
void gloss(lv_obj_t *object, uint32_t top, uint32_t bottom, lv_style_selector_t part)
{
    lv_obj_set_style_bg_color(object, lv_color_hex(top), part);
    lv_obj_set_style_bg_grad_color(object, lv_color_hex(bottom), part);
    lv_obj_set_style_bg_grad_dir(object, LV_GRAD_DIR_VER, part);
    lv_obj_set_style_bg_main_stop(object, 112, part);
    lv_obj_set_style_bg_grad_stop(object, 122, part);
}
} // namespace

void make_aura_background()
{
    s_aura_play_icon = nullptr; // style_root() has deleted the previous labels.
    lv_obj_remove_event_cb(s_screen, aura_light_cb);
    lv_obj_set_style_bg_grad_dir(s_screen, LV_GRAD_DIR_NONE, 0);
    // style_root has deleted every object that referenced the cached image.
    if (!is_aura_theme() || s_view == View::FullscreenInfoArt) {
        release_aura_wallpaper();
        return;
    }
    if (s_view == View::FileViewer) return;
    if (!load_aura_wallpaper()) {
        gradient(s_screen, kAuraSkyTop, kAuraSkyBottom, 0);
        lv_obj_add_event_cb(s_screen, aura_light_cb, LV_EVENT_DRAW_MAIN, nullptr);
        return;
    }
    lv_obj_t *backdrop = lv_obj_create(s_screen);
    lv_obj_set_pos(backdrop, 0, 0);
    lv_obj_set_size(backdrop, kScreenWidth, kScreenHeight);
    lv_obj_set_style_pad_all(backdrop, 0, 0);
    lv_obj_set_style_radius(backdrop, 0, 0);
    lv_obj_set_style_border_width(backdrop, 0, 0);
    lv_obj_set_style_outline_width(backdrop, 0, 0);
    lv_obj_set_style_shadow_width(backdrop, 0, 0);
    lv_obj_set_style_bg_color(backdrop, lv_color_hex(kAuraSkyTop), 0);
    lv_obj_set_style_bg_opa(backdrop, LV_OPA_COVER, 0);
    lv_obj_remove_flag(backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(backdrop, aura_wallpaper_cb, LV_EVENT_DRAW_MAIN, nullptr);
}

void style_aura_surface(lv_obj_t *object, lv_color_t color, int width, int height, bool button)
{
    if (!is_aura_theme() || s_view == View::FullscreenInfoArt || s_view == View::FileViewer) return;
    if (!button && width == kScreenWidth && lv_color_eq(color, kBackground)) {
        // Full page bodies expose the fixed wallpaper; headers have a frosted fill.
        if (height > 72) lv_obj_set_style_bg_opa(object, LV_OPA_TRANSP, 0);
        else gloss(object, 0xF0FAFF, 0xBFE4F8, 0);
        return;
    }
    if (button && height == kStatusHeight && lv_obj_get_style_radius(object, LV_PART_MAIN) == 0) return;
    if (lv_color_eq(color, kArtworkSurface) || width < 24 || height < 20 ||
        (!button && lv_obj_get_style_radius(object, LV_PART_MAIN) == 0)) return;
    const bool accent = lv_color_eq(color, kAccent) || lv_color_eq(color, kAccentDark) ||
                        lv_color_eq(color, kAccentSurface);
    // Danger buttons retain their semantic colour rather than becoming blue.
    if (lv_color_eq(color, kDangerSurface) || lv_color_eq(color, kDanger)) return;
    if (accent) {
        gloss(object, 0x5CC4F6, 0x0A6FC9, 0);
    } else {
        gloss(object, 0xF6FCFF, 0xC4E7F9, 0);
    }
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(object, 1, 0);
    lv_obj_set_style_border_color(object, lv_color_hex(accent ? 0xB7EDFF : 0xFFFFFF), 0);
    if (button) {
        // Default labels remain dark blue while a neutral glass button is pressed.
        gloss(object, accent ? 0x2E9FE6 : 0xD2EFFC, accent ? 0x07559F : 0x8ED2F3, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(object, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_border_color(object, kAccent, LV_STATE_PRESSED);
        if (lv_obj_get_style_radius(object, LV_PART_MAIN) == 0)
            lv_obj_set_style_radius(object, 8, 0);
    }
    lv_obj_add_event_cb(object, aura_rim_cb, LV_EVENT_DRAW_MAIN, nullptr);
}

void make_aura_equalizer_grid(lv_obj_t *parent)
{
    if (!is_aura_theme()) return;
    lv_obj_t *panel = make_box(parent, 8, 64, 304, 302, kSurface, 10);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(panel, aura_grid_cb, LV_EVENT_DRAW_MAIN, nullptr);
}

void style_aura_controls(lv_obj_t *object)
{
    if (!is_aura_theme()) return;
    if (lv_obj_has_class(object, &lv_slider_class) || lv_obj_has_class(object, &lv_bar_class)) {
        gradient(object, 0x93BDD4, 0xD7F1FF, LV_PART_MAIN);
        gloss(object, 0x8BE3FF, 0x0A7FD6, LV_PART_INDICATOR);
        for (lv_style_selector_t part : {LV_PART_MAIN, LV_PART_INDICATOR}) {
            lv_obj_set_style_radius(object, LV_RADIUS_CIRCLE, part);
            lv_obj_set_style_border_width(object, 1, part);
            lv_obj_set_style_border_color(object, kDivider, part);
        }
        if (lv_obj_has_class(object, &lv_slider_class)) {
            gradient(object, 0xFFFFFF, 0x55BDED, LV_PART_KNOB);
            lv_obj_set_style_radius(object, LV_RADIUS_CIRCLE, LV_PART_KNOB);
            lv_obj_set_style_border_width(object, 1, LV_PART_KNOB);
            lv_obj_set_style_border_color(object, kAccent, LV_PART_KNOB);
        }
        const lv_style_selector_t disabled_indicator = static_cast<lv_style_selector_t>(LV_PART_INDICATOR) |
                                                        static_cast<lv_style_selector_t>(LV_STATE_DISABLED);
        const lv_style_selector_t disabled_knob = static_cast<lv_style_selector_t>(LV_PART_KNOB) |
                                                   static_cast<lv_style_selector_t>(LV_STATE_DISABLED);
        lv_obj_set_style_bg_grad_dir(object, LV_GRAD_DIR_NONE, disabled_indicator);
        lv_obj_set_style_bg_grad_dir(object, LV_GRAD_DIR_NONE, disabled_knob);
    } else if (lv_obj_has_class(object, &lv_switch_class)) {
        lv_obj_set_style_bg_color(object, kDivider, LV_PART_MAIN);
        lv_obj_set_style_bg_color(object, kAccent, LV_PART_INDICATOR);
        gradient(object, 0xFFFFFF, 0xB2E1F8, LV_PART_KNOB);
        lv_obj_set_style_border_width(object, 1, LV_PART_KNOB);
        lv_obj_set_style_border_color(object, kAccent, LV_PART_KNOB);
    } else if (lv_obj_has_class(object, &lv_buttonmatrix_class)) {
        gradient(object, 0xE7F6FF, 0xAADAF3, LV_PART_ITEMS);
        lv_obj_set_style_text_color(object, kTextPrimary, LV_PART_ITEMS);
        lv_obj_set_style_radius(object, 7, LV_PART_ITEMS);
        lv_obj_set_style_border_width(object, 1, LV_PART_ITEMS);
        lv_obj_set_style_border_color(object, lv_color_hex(0xFFFFFF), LV_PART_ITEMS);
        const lv_style_selector_t pressed = static_cast<lv_style_selector_t>(LV_PART_ITEMS) |
                                             static_cast<lv_style_selector_t>(LV_STATE_PRESSED);
        gradient(object, 0xB5E3FA, 0x7CC8EE, pressed);
    } else if (lv_obj_has_class(object, &lv_textarea_class)) {
        lv_obj_set_style_bg_color(object, kSurface, 0);
        lv_obj_set_style_text_color(object, kTextPrimary, 0);
        lv_obj_set_style_border_color(object, kDivider, 0);
        lv_obj_set_style_border_width(object, 1, 0);
        lv_obj_set_style_radius(object, 8, 0);
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); ++i)
        style_aura_controls(lv_obj_get_child(object, i));
}

void make_aura_transport(lv_obj_t *parent, int y)
{
    // Windows Media Player 11 layout: Previous and Next sit in a smoked-glass
    // capsule, with a larger glowing play orb breaking out of its centre.
    lv_obj_t *capsule = make_box(parent, 56, y + 2, 208, 40, kSurface, LV_RADIUS_CIRCLE);
    gloss(capsule, 0x3C5D80, 0x0E2340, 0);
    lv_obj_set_style_border_color(capsule, lv_color_hex(0x8CC4EE), 0);
    lv_obj_remove_flag(capsule, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *halo = make_box(parent, 124, y - 14, 72, 72, kArtworkSurface, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_color(halo, lv_color_hex(0x5ECBFF), 0);
    lv_obj_set_style_bg_opa(halo, LV_OPA_40, 0);
    lv_obj_remove_flag(halo, LV_OBJ_FLAG_CLICKABLE);
    constexpr int positions[] = {68, 128, 208};
    for (int i = 0; i < 3; ++i) {
        const bool play = i == 1;
        const int size = play ? 64 : 44;
        lv_obj_t *button = make_button(parent, positions[i], y + (play ? -10 : 0),
            size, size, play ? kAccentSurface : kSurface, LV_RADIUS_CIRCLE);
        if (!play) {
            // Side buttons are part of the capsule: no glass of their own
            // until pressed.
            lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(button, 0, 0);
            lv_obj_remove_event_cb(button, aura_rim_cb);
        } else {
            gloss(button, 0x8FDDFF, 0x0A74CC, 0);
            lv_obj_set_style_border_width(button, 2, 0);
            lv_obj_set_style_border_color(button, lv_color_hex(0xE6F8FF), 0);
        }
        lv_obj_t *icon = make_label(button, i == 0 ? LV_SYMBOL_PREV : i == 2 ? LV_SYMBOL_NEXT : LV_SYMBOL_PLAY,
                                    kTextOnAccent);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_28, 0);
        lv_obj_center(icon);
        if (play) {
            s_aura_play_icon = icon;
            lv_obj_add_event_cb(button, nav_play_cb, LV_EVENT_CLICKED, nullptr);
        } else {
            lv_event_cb_t callback = i == 0 ? nav_previous_cb : nav_next_cb;
            for (lv_event_code_t code : {LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST})
                lv_obj_add_event_cb(button, callback, code, nullptr);
        }
    }
    update_aura_transport();
}

void update_aura_transport()
{
    if (!is_aura_theme() || !s_aura_play_icon) return;
    const lyra::audio::Status status = lyra::audio::status();
    set_label_text_if_changed(s_aura_play_icon,
                              status.playing && !status.paused ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}
} // namespace lyra::gui::internal
