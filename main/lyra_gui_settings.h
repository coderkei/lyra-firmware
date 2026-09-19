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

struct Values {
    bool gapless;
    bool replay_gain;
    uint8_t crossfade_seconds;
    uint8_t brightness_percent;
    bool dark_mode;
    uint8_t accent_colour;
    lyra::i18n::Language language;
    bool speaker_output_enabled;
    uint8_t equalizer_preset;
    int16_t equalizer_custom_bands[kEqualizerBandCount];
};

// Loads only valid persisted values, leaving caller-provided defaults intact.
// A missing or invalid persisted language always resolves to English.
void load(Values *values, size_t accent_palette_count, uint8_t equalizer_preset_count);

// Persists the complete preference set atomically from the caller's point of view.
esp_err_t save(const Values &values);

} // namespace lyra::gui_settings
