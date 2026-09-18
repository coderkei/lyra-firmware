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

void equalizer_preset_menu_cb(lv_event_t *)
{
    navigate_to(View::EqualizerPresets);
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
    lv_obj_t *row = make_button(parent, 7, y, 306, 54,
                                active ? kAccentSurface : kSurface, 6);
    lv_obj_t *label = make_label(row, equalizer_preset_name(preset),
                                 active ? kAccent : kTextPrimary);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 12, 0);
    if (active) {
        lv_obj_t *check = make_label(row, LV_SYMBOL_OK, kAccent);
        lv_obj_align(check, LV_ALIGN_RIGHT_MID, -14, 0);
    }
    lv_obj_add_event_cb(row, equalizer_preset_option_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(preset)));
}

void render_equalizer_presets()
{
    make_header("EQ Preset", View::Equalizer, true);
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
        make_equalizer_preset_option(body, static_cast<int>(index) * 58, presets[index]);
    }
}

void render_equalizer()
{
    make_header("Equalizer", View::Menu, true, "ON");
    lv_obj_t *body = make_box(s_screen, 0, 72, kScreenWidth, content_height(72), kBackground);
    lv_obj_t *preset = make_button(body, 8, 6, 304, 48, kSurface, 6);
    lv_obj_t *preset_name = make_label(preset, equalizer_preset_name(s_equalizer_preset),
                                       kTextPrimary);
    lv_obj_align(preset_name, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_t *arrow = make_label(preset, LV_SYMBOL_RIGHT, kTextMuted);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -12, 0);
    lv_obj_add_event_cb(preset, equalizer_preset_menu_cb, LV_EVENT_CLICKED, nullptr);

    constexpr const char *frequencies[] = {"60", "250", "1k", "4k", "16k"};
    constexpr int kSliderX = 51;
    constexpr int kSliderStep = 52;
    constexpr int kSliderY = 78;
    constexpr int kSliderHeight = 234;
    const bool editable = equalizer_is_custom();
    const int16_t *bands = equalizer_preset_bands(s_equalizer_preset);

    // A 43 px scale gutter keeps the +6 dB / -6 dB labels entirely clear of
    // the first slider's knob and indicator.
    lv_obj_t *top = make_label(body, "+6dB", kTextSecondary);
    lv_obj_set_pos(top, 3, 67);
    lv_obj_t *bottom = make_label(body, "-6dB", kTextSecondary);
    lv_obj_set_pos(bottom, 3, 302);

    for (size_t band = 0; band < lyra::audio::kEqualizerBandCount; ++band) {
        lv_obj_t *slider = lv_slider_create(body);
        lv_obj_set_pos(slider, kSliderX + static_cast<int>(band) * kSliderStep, kSliderY);
        lv_obj_set_size(slider, 12, kSliderHeight);
        lv_slider_set_range(slider, lyra::audio::kEqualizerMinimumTenthsDb,
                            lyra::audio::kEqualizerMaximumTenthsDb);
        lv_slider_set_value(slider, bands[band], LV_ANIM_OFF);
        lv_obj_set_style_bg_color(slider, kDivider, LV_PART_MAIN);
        lv_obj_set_style_bg_color(slider, kAccentDark, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(slider, kTextOnAccent, LV_PART_KNOB);
        lv_obj_set_style_border_color(slider, kAccent, LV_PART_KNOB);
        lv_obj_set_style_border_width(slider, 3, LV_PART_KNOB);

        lv_obj_t *frequency = make_label(body, frequencies[band], kTextSecondary);
        lv_obj_set_width(frequency, 40);
        lv_obj_set_style_text_align(frequency, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(frequency, 37 + static_cast<int>(band) * kSliderStep, 326);
        lv_obj_t *gain = make_label(body, "", editable ? kAccent : kTextMuted);
        lv_obj_set_width(gain, 40);
        lv_obj_set_style_text_align(gain, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(gain, 37 + static_cast<int>(band) * kSliderStep, 344);
        update_equalizer_gain_label(gain, bands[band], editable ? kAccent : kTextMuted);

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
            lv_obj_set_style_bg_color(slider, kTextMuted,
                                      disabled_indicator);
            lv_obj_set_style_bg_color(slider, kTextMuted, disabled_knob);
            lv_obj_set_style_border_color(slider, kDivider, disabled_knob);
            lv_obj_add_state(slider, LV_STATE_DISABLED);
        }
    }
}

} // namespace lyra::gui::internal
