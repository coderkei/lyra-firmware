/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../lyra_gui_internal.h"

namespace lyra::gui::internal {
namespace {

constexpr int kMaxDice = 6;
constexpr int kDieSize = 84;
int s_dice_count = 2;
uint8_t s_faces[kMaxDice] = {1, 1, 1, 1, 1, 1};
bool s_rolled;

void die_draw_cb(lv_event_t *event)
{
    lv_area_t area;
    lv_obj_get_coords(lv_event_get_current_target_obj(event), &area);
    const int face = static_cast<int>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    // Pip positions on a 3 x 3 grid, indexed by face value.
    static constexpr uint16_t kPips[7] = {0, 0x010, 0x101, 0x111, 0x145, 0x155, 0x16D};
    const int pip = kDieSize / 7;
    const int step = (kDieSize - 2 * pip) / 3;
    for (int slot = 0; slot < 9; ++slot) {
        if (!(kPips[face] & (1u << slot))) continue;
        const int x = area.x1 + pip + (slot % 3) * step + (step - pip) / 2;
        const int y = area.y1 + pip + (slot / 3) * step + (step - pip) / 2;
        lv_draw_rect_dsc_t dot;
        lv_draw_rect_dsc_init(&dot);
        dot.bg_color = kTextOnAccent;
        dot.bg_opa = LV_OPA_COVER;
        dot.radius = LV_RADIUS_CIRCLE;
        const lv_area_t bounds{x, y, x + pip - 1, y + pip - 1};
        lv_draw_rect(lv_event_get_layer(event), &dot, &bounds);
    }
}

void roll_cb(lv_event_t *)
{
    for (int die = 0; die < s_dice_count; ++die) s_faces[die] = static_cast<uint8_t>(1 + esp_random() % 6);
    s_rolled = true;
    render(View::Dice);
}

void count_cb(lv_event_t *event)
{
    const int direction = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)) ? 1 : -1;
    s_dice_count = std::clamp(s_dice_count + direction, 1, kMaxDice);
    render(View::Dice);
}

} // namespace

void render_dice()
{
    make_header(tr(lyra::i18n::StringId::Dice), View::Apps, true);
    lv_obj_t *body = make_box(s_screen, 0, 72, kScreenWidth, content_height(72), kBackground);
    constexpr int kColumns = 3;
    const int rows = (s_dice_count + kColumns - 1) / kColumns;
    int total = 0;
    for (int die = 0; die < s_dice_count; ++die) {
        const int in_row = std::min(kColumns, s_dice_count - die / kColumns * kColumns);
        const int row_width = in_row * kDieSize + (in_row - 1) * 14;
        const int x = (kScreenWidth - row_width) / 2 + (die % kColumns) * (kDieSize + 14);
        const int y = 14 + (die / kColumns) * (kDieSize + 14) + (rows == 1 ? 50 : 0);
        lv_obj_t *face = make_box(body, x, y, kDieSize, kDieSize, kAccentDark, 14);
        lv_obj_add_event_cb(face, die_draw_cb, LV_EVENT_DRAW_MAIN,
                            reinterpret_cast<void *>(static_cast<uintptr_t>(s_faces[die])));
        lv_obj_add_flag(face, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(face, roll_cb, LV_EVENT_CLICKED, nullptr);
        total += s_faces[die];
    }
    char total_text[32];
    format_u32(lyra::i18n::StringId::DiceTotal, static_cast<uint32_t>(total), total_text, sizeof(total_text));
    lv_obj_t *total_label = make_label(body, s_rolled ? total_text : tr(lyra::i18n::StringId::DiceHint),
                                       s_rolled ? kTextPrimary : kTextMuted);
    lv_obj_set_style_text_font(total_label, s_rolled ? &lv_font_montserrat_28 : lyra::font::ui(), 0);
    lv_obj_align(total_label, LV_ALIGN_TOP_MID, 0, 226);

    const int controls_y = content_height(72) - 58;
    lv_obj_t *minus = make_button(body, 8, controls_y, 60, 48, kSurfaceRaised, 7, true);
    lv_obj_t *minus_label = make_label(minus, LV_SYMBOL_MINUS, s_dice_count > 1 ? kAccent : kTextMuted);
    lv_obj_center(minus_label);
    lv_obj_add_event_cb(minus, count_cb, LV_EVENT_CLICKED, reinterpret_cast<void *>(uintptr_t(0)));
    lv_obj_t *roll = make_button(body, 74, controls_y, 172, 48, kAccentDark, 7);
    lv_obj_t *roll_label = make_label(roll, tr(lyra::i18n::StringId::DiceRoll), kTextOnAccent);
    make_marquee(roll_label, 160);
    lv_obj_center(roll_label);
    lv_obj_add_event_cb(roll, roll_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *plus = make_button(body, 252, controls_y, 60, 48, kSurfaceRaised, 7, true);
    lv_obj_t *plus_label = make_label(plus, LV_SYMBOL_PLUS, s_dice_count < kMaxDice ? kAccent : kTextMuted);
    lv_obj_center(plus_label);
    lv_obj_add_event_cb(plus, count_cb, LV_EVENT_CLICKED, reinterpret_cast<void *>(uintptr_t(1)));
}

} // namespace lyra::gui::internal
