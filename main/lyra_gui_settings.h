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

// Where the status bar shows the clock. Persisted IDs: append only.
enum class ClockPosition : uint8_t { Left, Centre, Right, Hidden, Count };
enum class KeyboardLayout : uint8_t {
    English, Russian, Korean, ChinesePinyin, JapaneseKana, Count,
};

struct Values {
    bool gapless = true;
    bool replay_gain = true;
    bool quick_seek = false;
    uint8_t crossfade_seconds = 0;
    uint8_t brightness_percent = 72;
    bool dark_mode = true;
    uint8_t accent_colour = 0;
    lyra::i18n::Language language = lyra::i18n::Language::English;
    bool speaker_output_enabled = false;
    uint8_t equalizer_preset = 0;
    int16_t equalizer_custom_bands[kEqualizerBandCount]{};
    uint8_t date_format = 0;
    bool use_24_hour = true;
    bool dst_enabled = false;
    // True when a valid language has been persisted.
    bool language_selected = false;
    // Global input method selection, independent of the interface language.
    KeyboardLayout keyboard_layout = KeyboardLayout::English;
    // False until the post-reset setup flow reaches its completion screen.
    bool setup_complete = false;
    ClockPosition clock_position = ClockPosition::Centre;
    // Tints backgrounds and surfaces with the accent colour.
    bool dynamic_tint = false;
    // Playback controls under the browsing pages.
    bool mini_player = true;
    // Replaces the QWERTY-style keyboards with a 12-key T9 keypad where one exists.
    bool t9_input = false;
    // Small album covers beside rows in the library, queue, playlists and search.
    bool list_artwork = true;
    // Sets the clock from network time whenever Wi-Fi connects.
    bool automatic_time = true;
    // Local standard time minus UTC, in minutes (daylight saving is separate).
    int16_t utc_offset_minutes = 0;
};

// Valid UTC offsets: UTC-12:00 to UTC+14:00 in 15-minute steps.
constexpr int16_t kMinUtcOffsetMinutes = -12 * 60;
constexpr int16_t kMaxUtcOffsetMinutes = 14 * 60;

// Loads only valid persisted values, leaving caller-provided defaults intact.
// A missing or invalid persisted language resolves to English and leaves
// language_selected false.
void load(Values *values, size_t accent_palette_count, uint8_t equalizer_preset_count);

// Persists the complete preference set atomically from the caller's point of view.
esp_err_t save(const Values &values);

} // namespace lyra::gui_settings
