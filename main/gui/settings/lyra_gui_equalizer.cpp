/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../lyra_gui_internal.h"

namespace lyra::gui::internal {

struct EqualizerSliderContext {
    size_t band;
    lv_obj_t *value_label;
};

EqualizerSliderContext s_equalizer_slider_contexts[lyra::audio::kEqualizerBandCount]{};

void update_equalizer_gain_label(lv_obj_t *label, int16_t gain_tenths_db, lv_color_t color)
{
    if (!label) return;
    char text[8];
    std::snprintf(text, sizeof(text), "%+.1f", static_cast<float>(gain_tenths_db) / 10.0f);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, color, 0);
}

void equalizer_slider_cb(lv_event_t *event)
{
    auto *context = static_cast<EqualizerSliderContext *>(lv_event_get_user_data(event));
    if (!context || context->band >= lyra::audio::kEqualizerBandCount || !equalizer_is_custom()) return;
    const int value = lv_slider_get_value(lv_event_get_current_target_obj(event));
    const int16_t gain = static_cast<int16_t>(std::clamp(
        value, static_cast<int>(lyra::audio::kEqualizerMinimumTenthsDb),
        static_cast<int>(lyra::audio::kEqualizerMaximumTenthsDb)));
    s_equalizer_custom_bands[context->band] = gain;
    update_equalizer_gain_label(context->value_label, gain, kAccent);
    apply_equalizer_to_audio();
}

void equalizer_slider_released_cb(lv_event_t *)
{
    // Custom bands are saved only after the gesture settles, while every
    // intermediate slider position is still applied immediately to playback.
    save_user_settings();
}

void equalizer_preset_option_cb(lv_event_t *event)
{
    const auto preset = static_cast<EqualizerPreset>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    if (preset >= EqualizerPreset::Count) return;
    s_equalizer_preset = preset;
    apply_equalizer_to_audio();
    save_user_settings();
    navigate_back(View::Equalizer);
}

void make_equalizer_preset_option(lv_obj_t *parent, int y, EqualizerPreset preset)
{
    const bool active = s_equalizer_preset == preset;
    lv_obj_t *row = make_button(parent, kPageInset, y, kScreenWidth - 2 * kPageInset, 52,
                                active ? kAccentSurface : kSurface, kRowRadius);
    lv_obj_t *label = make_label(row, equalizer_preset_name(preset),
                                 active ? kAccent : kTextPrimary);
    make_marquee(label, 258);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 16, 0);
    if (active) {
        lv_obj_t *check = make_label(row, LV_SYMBOL_OK, kAccent);
        lv_obj_align(check, LV_ALIGN_RIGHT_MID, -14, 0);
    }
    lv_obj_add_event_cb(row, equalizer_preset_option_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(preset)));
}

void render_equalizer_presets()
{
    make_header(tr(lyra::i18n::StringId::EqPreset), View::Equalizer, true);
    lv_obj_t *body = make_scroll_body(72);
    constexpr EqualizerPreset presets[] = {
        EqualizerPreset::Custom,
        EqualizerPreset::Flat,
        EqualizerPreset::FullBass,
        EqualizerPreset::FullTreble,
        EqualizerPreset::BassAndTreble,
        EqualizerPreset::Rock,
        EqualizerPreset::Pop,
        EqualizerPreset::Jazz,
        EqualizerPreset::Classic,
    };
    for (size_t index = 0; index < sizeof(presets) / sizeof(presets[0]); ++index) {
        make_equalizer_preset_option(body, 4 + static_cast<int>(index) * 58, presets[index]);
    }
}

void render_equalizer()
{
    make_header(tr(lyra::i18n::StringId::Equalizer), View::SoundSettings, true);
    lv_obj_t *body = make_box(s_screen, 0, kContentTop, kScreenWidth, content_height(kContentTop),
                              kBackground);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_CLICKABLE);
    make_row(body, 4, Icon::Equalizer, tr(lyra::i18n::StringId::EqPreset),
             equalizer_preset_name(s_equalizer_preset), View::EqualizerPresets, 62);

    // Five vertical faders on one card, the 0 dB line drawn across them.
    constexpr int kCardY = 76;
    constexpr int kCardHeight = 316;
    constexpr const char *frequencies[] = {"60", "250", "1k", "4k", "16k"};
    constexpr int kSliderX = 70;
    constexpr int kSliderStep = 50;
    constexpr int kSliderY = 30;
    constexpr int kSliderHeight = 210;
    lv_obj_t *card = make_card(body, kPageInset, kCardY, kScreenWidth - 2 * kPageInset, kCardHeight);
    const bool editable = equalizer_is_custom();
    const int16_t *bands = equalizer_preset_bands(s_equalizer_preset);

    lv_obj_t *zero = make_box(card, 48, kSliderY + kSliderHeight / 2, kScreenWidth - 2 * kPageInset - 64,
                              1, kDivider);
    lv_obj_remove_flag(zero, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *top = make_label(card, "+6dB", kTextMuted);
    lv_obj_set_pos(top, 8, kSliderY - 10);
    lv_obj_t *middle = make_label(card, "0", kTextMuted);
    lv_obj_set_pos(middle, 22, kSliderY + kSliderHeight / 2 - 10);
    lv_obj_t *bottom = make_label(card, "-6dB", kTextMuted);
    lv_obj_set_pos(bottom, 8, kSliderY + kSliderHeight - 10);

    for (size_t band = 0; band < lyra::audio::kEqualizerBandCount; ++band) {
        const int column = kSliderX + static_cast<int>(band) * kSliderStep;
        lv_obj_t *slider = lv_slider_create(card);
        lv_obj_set_pos(slider, column, kSliderY);
        lv_obj_set_size(slider, 8, kSliderHeight);
        lv_slider_set_range(slider, lyra::audio::kEqualizerMinimumTenthsDb,
                            lyra::audio::kEqualizerMaximumTenthsDb);
        lv_slider_set_value(slider, bands[band], LV_ANIM_OFF);
        style_slider(slider);
        lv_obj_set_style_pad_all(slider, 7, LV_PART_KNOB);

        lv_obj_t *frequency = make_label(card, frequencies[band], kTextSecondary);
        lv_obj_set_width(frequency, 44);
        lv_obj_set_style_text_align(frequency, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(frequency, column - 18, kSliderY + kSliderHeight + 18);
        const lv_color_t gain_color = editable ? kAccent : kTextMuted;
        lv_obj_t *gain = make_label(card, "", gain_color);
        lv_obj_set_width(gain, 44);
        lv_obj_set_style_text_align(gain, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(gain, column - 18, kSliderY + kSliderHeight + 40);
        update_equalizer_gain_label(gain, bands[band], gain_color);

        if (editable) {
            s_equalizer_slider_contexts[band] = {band, gain};
            lv_obj_add_event_cb(slider, equalizer_slider_cb, LV_EVENT_VALUE_CHANGED,
                                &s_equalizer_slider_contexts[band]);
            lv_obj_add_event_cb(slider, equalizer_slider_released_cb, LV_EVENT_RELEASED, nullptr);
            lv_obj_add_event_cb(slider, equalizer_slider_released_cb, LV_EVENT_PRESS_LOST, nullptr);
        } else {
            const lv_style_selector_t disabled_indicator =
                static_cast<lv_style_selector_t>(LV_PART_INDICATOR) |
                static_cast<lv_style_selector_t>(LV_STATE_DISABLED);
            const lv_style_selector_t disabled_knob =
                static_cast<lv_style_selector_t>(LV_PART_KNOB) |
                static_cast<lv_style_selector_t>(LV_STATE_DISABLED);
            lv_obj_set_style_bg_color(slider, kTextMuted, disabled_indicator);
            lv_obj_set_style_border_color(slider, kTextMuted, disabled_knob);
            lv_obj_add_state(slider, LV_STATE_DISABLED);
        }
    }
}

} // namespace lyra::gui::internal
