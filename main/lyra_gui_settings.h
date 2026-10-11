/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "lyra_audio.h"
#include "lyra_i18n.h"

namespace lyra::gui_settings {

constexpr size_t kEqualizerBandCount = lyra::audio::kEqualizerBandCount;

// Persisted IDs: append new themes without changing existing values.
enum class Theme : uint8_t { Standard, NeonSky, CutePink, Zeno, Aura, Count };
enum class KeyboardLayout : uint8_t {
    English, Russian, Korean, ChinesePinyin, JapaneseKana, Count,
};

struct Values {
    bool gapless;
    bool replay_gain;
    bool quick_seek;
    uint8_t crossfade_seconds;
    uint8_t brightness_percent;
    bool dark_mode;
    uint8_t accent_colour;
    lyra::i18n::Language language;
    bool speaker_output_enabled;
    uint8_t equalizer_preset;
    int16_t equalizer_custom_bands[kEqualizerBandCount];
    uint8_t date_format;
    bool use_24_hour;
    bool dst_enabled;
    // True when a valid language has been persisted.
    bool language_selected;
    // Independent of the classic dark/light and accent preferences.
    Theme theme = Theme::Standard;
    // Global input method selection, independent of the interface language.
    KeyboardLayout keyboard_layout = KeyboardLayout::English;
    // False until the post-reset setup flow reaches its completion screen.
    bool setup_complete = false;
    // True selects the listening hub; false restores the classic seven-row menu.
    bool listening_hub = true;
    bool timer_alarm_enabled = true;
};

// Loads only valid persisted values, leaving caller-provided defaults intact.
// A missing or invalid persisted language resolves to English and leaves
// language_selected false.
void load(Values *values, size_t accent_palette_count, uint8_t equalizer_preset_count);

// Persists the complete preference set atomically from the caller's point of view.
esp_err_t save(const Values &values);

} // namespace lyra::gui_settings
