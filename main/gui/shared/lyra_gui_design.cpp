/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

// The LyraOS design language: rounded cards on a flat page, two-tone tile
// badges, pill choices, switches and bottom sheets. Screens compose these
// rather than styling raw LVGL objects, so the whole OS stays consistent.

#include "../lyra_gui_internal.h"

namespace lyra::gui::internal {
namespace {

constexpr int kSheetTitleHeight = 52;
constexpr int kSheetBottomPadding = 14;
constexpr int kSwitchWidth = 44;
constexpr int kSwitchHeight = 26;
constexpr int kSwitchKnob = 20;
constexpr int kChipHeight = 36;
constexpr int kChipGap = 8;

void sheet_overlay_cb(lv_event_t *event)
{
    // Only a tap on the dimmed area closes the sheet, not one on the sheet.
    if (lv_event_get_target_obj(event) != lv_event_get_current_target_obj(event)) return;
    close_sheet(static_cast<lv_obj_t **>(lv_event_get_user_data(event)));
}

void sheet_deleted_cb(lv_event_t *event)
{
    // A re-render removes the sheet with the page; forget it then too.
    auto **slot = static_cast<lv_obj_t **>(lv_event_get_user_data(event));
    if (slot && *slot == lv_event_get_current_target_obj(event)) *slot = nullptr;
}

lv_obj_t *make_untouchable(lv_obj_t *object)
{
    lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
    return object;
}

} // namespace

lv_obj_t *make_card(lv_obj_t *parent, int x, int y, int width, int height)
{
    lv_obj_t *card = make_box(parent, x, y, width, height, kSurface, kCardRadius);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICKABLE);
    return card;
}

void make_tile_badge(lv_obj_t *parent, int x, int y, int size, Icon icon, uint32_t hue)
{
    const lv_color_t color = lv_color_hex(hue);
    const int offset = size / 3;
    const int radius = std::max(6, size / 4);
    lv_obj_t *back = make_untouchable(make_box(parent, x, y, size, size, color, radius));
    lv_obj_set_style_bg_opa(back, LV_OPA_50, 0);
    lv_obj_t *front = make_untouchable(make_box(parent, x + offset, y + offset, size, size,
        lv_color_mix(lv_color_white(), color, 40), radius));
    lv_obj_set_style_bg_opa(front, LV_OPA_90, 0);
    lv_obj_t *glyph = make_icon(front, icon, lv_color_white(), size * 11 / 20);
    lv_obj_center(glyph);
}

lv_obj_t *make_tile(lv_obj_t *parent, int x, int y, int width, int height,
                    Icon icon, uint32_t hue, const char *label)
{
    lv_obj_t *tile = make_button(parent, x, y, width, height, kSurface, kCardRadius);
    const lv_font_t *font = title_font();
    const int name_height = lv_font_get_line_height(font);
    const int size = std::clamp(height * 34 / 100, 26, 44);
    const int badge = size + size / 3;
    const int top = std::max(8, (height - badge - name_height - 10) / 2);
    make_tile_badge(tile, (width - badge) / 2, top, size, icon, hue);
    lv_obj_t *name = make_label(tile, label, kTextPrimary);
    lv_obj_set_style_text_font(name, font, 0);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    make_marquee(name, width - 16);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, top + badge + 10);
    return tile;
}

lv_obj_t *make_icon_button(lv_obj_t *parent, int x, int y, int size, Icon icon,
                           lv_color_t color)
{
    lv_obj_t *button = make_button(parent, x, y, size, size, kSurfaceRaised, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
    lv_obj_t *glyph = make_icon(button, icon, color, std::min(24, size * 11 / 20));
    lv_obj_center(glyph);
    return button;
}

lv_obj_t *make_symbol_button(lv_obj_t *parent, int x, int y, int size, const char *symbol,
                             lv_color_t color)
{
    lv_obj_t *button = make_button(parent, x, y, size, size, kSurfaceRaised, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
    lv_obj_t *glyph = make_label(button, symbol, color);
    lv_obj_set_style_text_font(glyph, title_font(), 0);
    lv_obj_center(glyph);
    return button;
}

lv_obj_t *make_switch(lv_obj_t *parent, bool checked)
{
    lv_obj_t *track = make_untouchable(make_box(parent, 0, 0, kSwitchWidth, kSwitchHeight,
                                                kDivider, LV_RADIUS_CIRCLE));
    make_untouchable(make_box(track, 0, (kSwitchHeight - kSwitchKnob) / 2, kSwitchKnob,
                              kSwitchKnob, lv_color_white(), LV_RADIUS_CIRCLE));
    set_switch(track, checked);
    return track;
}

void set_switch(lv_obj_t *track, bool checked)
{
    if (!track) return;
    lv_obj_set_style_bg_color(track, checked ? kAccent : kDivider, 0);
    lv_obj_t *knob = lv_obj_get_child(track, 0);
    const int inset = (kSwitchHeight - kSwitchKnob) / 2;
    if (knob) lv_obj_set_x(knob, checked ? kSwitchWidth - kSwitchKnob - inset : inset);
}

int make_choice_card(lv_obj_t *parent, int y, const char *title, const char *const *labels,
                     size_t count, size_t selected, lv_event_cb_t callback)
{
    constexpr int kCardWidth = kScreenWidth - 2 * kPageInset;
    constexpr int kInner = 14;
    lv_obj_t *card = make_card(parent, kPageInset, y, kCardWidth, 60);
    lv_obj_t *heading = make_label(card, title, kTextPrimary);
    make_marquee(heading, kCardWidth - 2 * kInner);
    lv_obj_set_pos(heading, kInner, 12);
    const lv_font_t *font = lyra::font::ui();
    int x = kInner;
    int chip_y = 44;
    for (size_t index = 0; index < count; ++index) {
        lv_point_t size{};
        lv_text_get_size(&size, labels[index], font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        const int width = std::clamp(static_cast<int>(size.x) + 34, 64, kCardWidth - 2 * kInner);
        if (x > kInner && x + width > kCardWidth - kInner) {
            x = kInner;
            chip_y += kChipHeight + kChipGap;
        }
        const bool active = index == selected;
        lv_obj_t *chip = make_button(card, x, chip_y, width, kChipHeight,
                                     active ? kAccent : kSurfaceRaised, LV_RADIUS_CIRCLE);
        lv_obj_t *label = make_label(chip, labels[index], active ? kTextOnAccent : kTextPrimary);
        make_marquee(label, width - 16);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(label);
        lv_obj_add_event_cb(chip, callback, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<uintptr_t>(index)));
        x += width + kChipGap;
    }
    const int height = chip_y + kChipHeight + 14;
    lv_obj_set_height(card, height);
    return height;
}

void make_section_label(lv_obj_t *parent, int y, const char *text)
{
    lv_obj_t *label = make_label(parent, text, kTextMuted);
    make_marquee(label, kScreenWidth - 2 * kPageInset - 12);
    lv_obj_set_pos(label, kPageInset + 6, y);
}

void style_slider(lv_obj_t *slider)
{
    lv_obj_set_style_bg_color(slider, kDivider, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, kAccent, LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_border_color(slider, kAccent, LV_PART_KNOB);
    lv_obj_set_style_border_width(slider, 3, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 5, LV_PART_KNOB);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_ext_click_area(slider, 10);
    block_page_gestures(slider);
}

lv_obj_t *make_sheet(lv_obj_t **slot, const char *title, int content_height,
                     lv_obj_t **content_out)
{
    if (!slot || *slot || !s_screen || !content_out) return nullptr;
    lv_obj_t *overlay = make_box(s_screen, 0, 0, kScreenWidth, kScreenHeight, kOverlay);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_60, 0);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(overlay);
    *slot = overlay;
    lv_obj_add_event_cb(overlay, sheet_overlay_cb, LV_EVENT_CLICKED, slot);
    lv_obj_add_event_cb(overlay, sheet_deleted_cb, LV_EVENT_DELETE, slot);

    const int maximum_content = kScreenHeight - 72 - kSheetTitleHeight - kSheetBottomPadding;
    const int shown_content = std::min(content_height, maximum_content);
    const int sheet_height = kSheetTitleHeight + shown_content + kSheetBottomPadding;
    // The sheet runs past the bottom edge so only its top corners round.
    lv_obj_t *sheet = make_box(overlay, 0, kScreenHeight - sheet_height, kScreenWidth,
                               sheet_height + 24, kSurface, 22);
    make_untouchable(make_box(sheet, (kScreenWidth - 40) / 2, 8, 40, 4, kDivider,
                              LV_RADIUS_CIRCLE));
    lv_obj_t *heading = make_label(sheet, title, kTextPrimary);
    lv_obj_set_style_text_font(heading, title_font(), 0);
    make_marquee(heading, kScreenWidth - 40);
    lv_obj_set_pos(heading, 20, 20);

    lv_obj_t *content = make_box(sheet, kPageInset, kSheetTitleHeight,
                                 kScreenWidth - 2 * kPageInset, shown_content, kSurface);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_CLICKABLE);
    if (content_height > shown_content) configure_cover_aware_scroll(content);
    *content_out = content;
    return overlay;
}

void close_sheet(lv_obj_t **slot)
{
    if (!slot || !*slot) return;
    lv_obj_t *sheet = *slot;
    *slot = nullptr;
    lv_obj_delete(sheet);
}

lv_obj_t *make_sheet_row(lv_obj_t *content, int y, Icon icon, const char *text,
                         lv_event_cb_t callback, uintptr_t value)
{
    constexpr int kRowWidth = kScreenWidth - 2 * kPageInset;
    lv_obj_t *row = make_button(content, 0, y, kRowWidth, 48, kSurface, kRowRadius);
    lv_obj_t *glyph = make_icon(row, icon, kAccent);
    lv_obj_align(glyph, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_t *label = make_label(row, text, kTextPrimary);
    make_marquee(label, kRowWidth - 60);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 46, 0);
    lv_obj_add_event_cb(row, callback, LV_EVENT_CLICKED, reinterpret_cast<void *>(value));
    return row;
}

} // namespace lyra::gui::internal
