/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../lyra_gui_internal.h"

namespace lyra::gui::internal {
namespace {
lv_obj_t *s_aura_play_icon = nullptr;

// Native draw tasks keep the wallpaper fixed behind scrolling bodies, with
// no decoded bitmap, canvas buffer, extra widgets or animation timers.
void aura_light_cb(lv_event_t *event)
{
    lv_obj_t *object = lv_event_get_current_target_obj(event);
    lv_area_t area;
    lv_obj_get_coords(object, &area);
    lv_layer_t *layer = lv_event_get_layer(event);
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
            lv_draw_line(layer, &line);
            line.width = 2;
            line.opa = LV_OPA_50;
            lv_draw_line(layer, &line);
        }
    }
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
        lv_draw_line(lv_event_get_layer(event), &line);
    }
    for (int band = 0; band < 5; ++band) {
        const int x = area.x1 + 49 + band * 52;
        line.p1 = {static_cast<lv_value_precise_t>(x), static_cast<lv_value_precise_t>(area.y1 + 14)};
        line.p2 = {static_cast<lv_value_precise_t>(x), static_cast<lv_value_precise_t>(area.y1 + 248)};
        lv_draw_line(lv_event_get_layer(event), &line);
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
    lv_draw_border(lv_event_get_layer(event), &rim, &area);
}

void gradient(lv_obj_t *object, uint32_t top, uint32_t bottom, lv_style_selector_t part)
{
    lv_obj_set_style_bg_color(object, lv_color_hex(top), part);
    lv_obj_set_style_bg_grad_color(object, lv_color_hex(bottom), part);
    lv_obj_set_style_bg_grad_dir(object, LV_GRAD_DIR_VER, part);
    lv_obj_set_style_bg_main_stop(object, 0, part);
    lv_obj_set_style_bg_grad_stop(object, 255, part);
}
} // namespace

void make_aura_background()
{
    s_aura_play_icon = nullptr; // style_root() has deleted the previous labels.
    lv_obj_remove_event_cb(s_screen, aura_light_cb);
    lv_obj_set_style_bg_grad_dir(s_screen, LV_GRAD_DIR_NONE, 0);
    if (!is_aura_theme() || s_view == View::FullscreenInfoArt || s_view == View::FileViewer) return;
    gradient(s_screen, 0x83CCF3, 0xDBF7F9, 0);
    lv_obj_add_event_cb(s_screen, aura_light_cb, LV_EVENT_DRAW_MAIN, nullptr);
}

void style_aura_surface(lv_obj_t *object, lv_color_t color, int width, int height, bool button)
{
    if (!is_aura_theme() || s_view == View::FullscreenInfoArt || s_view == View::FileViewer) return;
    if (!button && width == kScreenWidth && lv_color_eq(color, kBackground)) {
        // Full page bodies expose the fixed wallpaper; headers have a frosted fill.
        if (height > 72) lv_obj_set_style_bg_opa(object, LV_OPA_TRANSP, 0);
        else gradient(object, 0xD7F1FF, 0xA1D8F5, 0);
        return;
    }
    if (button && height == kStatusHeight && lv_obj_get_style_radius(object, LV_PART_MAIN) == 0) return;
    if (lv_color_eq(color, kArtworkSurface) || width < 24 || height < 20 ||
        (!button && lv_obj_get_style_radius(object, LV_PART_MAIN) == 0)) return;
    const bool accent = lv_color_eq(color, kAccent) || lv_color_eq(color, kAccentDark) ||
                        lv_color_eq(color, kAccentSurface);
    // Danger buttons retain their semantic colour rather than becoming blue.
    if (lv_color_eq(color, kDangerSurface)) return;
    if (accent) {
        gradient(object, lv_color_to_u32(color) & 0xFFFFFF, 0x0074D2, 0);
    } else {
        gradient(object, 0xF1FBFF, 0xACDCF4, 0);
    }
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(object, 1, 0);
    lv_obj_set_style_border_color(object, lv_color_hex(accent ? 0xB7EDFF : 0xFFFFFF), 0);
    if (button) {
        // Default labels remain dark blue while a neutral glass button is pressed.
        gradient(object, accent ? 0x0786CE : 0xB5E3FA, accent ? 0x0055B0 : 0x7CC8EE, LV_STATE_PRESSED);
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
        gradient(object, 0x73DEFF, 0x0084DC, LV_PART_INDICATOR);
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
    constexpr int positions[] = {72, 128, 204};
    for (int i = 0; i < 3; ++i) {
        const bool play = i == 1;
        const int size = play ? 64 : 44;
        lv_obj_t *button = make_button(parent, positions[i], y + (play ? -10 : 0),
            size, size, play ? kAccentSurface : kSurface, LV_RADIUS_CIRCLE);
        lv_obj_t *icon = make_label(button, i == 0 ? LV_SYMBOL_PREV : i == 2 ? LV_SYMBOL_NEXT : LV_SYMBOL_PLAY,
                                    play ? kTextOnAccent : kAccent);
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
    lv_label_set_text(s_aura_play_icon, status.playing && !status.paused ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}
} // namespace lyra::gui::internal
