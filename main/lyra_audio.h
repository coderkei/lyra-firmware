/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace lyra::audio {

constexpr size_t kMaxPath = 256;
constexpr size_t kEqualizerBandCount = 5;
constexpr int16_t kEqualizerMinimumTenthsDb = -60;
constexpr int16_t kEqualizerMaximumTenthsDb = 60;
// This is the user-facing scale. The implementation applies an additional
// conservative digital output cap below full-scale PCM amplitude.
constexpr uint8_t kMaximumVolumePercent = 100;
constexpr uint8_t kDefaultVolumePercent = 50;

// Five centre-frequency gains, in tenths of a decibel. The bands are 60 Hz,
// 250 Hz, 1 kHz, 4 kHz, and 16 kHz respectively.
struct EqualizerSettings {
    int16_t band_tenths_db[kEqualizerBandCount];
};

struct Status {
    bool initialized;
    bool playing;
    bool paused;
    bool eof;
    esp_err_t last_error;
    uint32_t sample_rate;
    uint8_t channels;
    uint8_t bits_per_sample;
    uint64_t decoded_bytes;
    uint32_t position_ms;
    uint32_t duration_ms;
    uint8_t volume_percent;
    char path[kMaxPath];
};

struct Diagnostics {
    uint32_t max_sd_read_us;
    uint64_t total_sd_read_us;
    uint32_t sd_read_count;
    uint32_t average_sd_read_us;
    uint32_t max_sd_lock_wait_us;
    uint64_t total_sd_lock_wait_us;
    uint32_t average_sd_lock_wait_us;
    size_t audio_buffer_low_watermark;
    uint32_t underrun_count;
    size_t pcm_buffer_low_watermark;
    uint32_t pcm_underrun_count;
    bool read_ahead_internal;
    bool pcm_internal;
    bool stereo_internal;
};

// Initializes the native decoder registry and the PCM5102A I2S output channel.
// This does not require a mounted MicroSD card.
esp_err_t init();

// Requests asynchronous playback of a supported audio path in the VFS
// namespace. Lyra's MicroSD mount is /sdcard, so catalog paths can be passed
// directly. Native esp_audio_codec handles MP3, FLAC, AAC, M4A/ALAC, WAV, OGG,
// and Opus-in-Ogg; uncompressed AIFF/AIFC PCM is handled by Lyra.
esp_err_t play(const char *path);
esp_err_t stop();
esp_err_t toggle_pause();
esp_err_t seek(uint32_t position_ms);
esp_err_t set_volume(uint8_t volume_percent);
// Replaces all five EQ gains atomically. The new filter coefficients are
// applied at the next PCM block and smoothed over a short transition.
esp_err_t set_equalizer(const EqualizerSettings &settings);
// Applies a per-track ReplayGain adjustment in tenths of a decibel. The
// adjustment is capped by the normal maximum output level.
esp_err_t set_replay_gain_adjustment(int16_t tenths_db);
// Sets a short-lived transition gain used by the playback UI for track fades.
esp_err_t set_transition_gain(uint8_t percent);
esp_err_t save_volume();
// Reads the native format headers without starting playback. Used by the
// MicroSD catalog so duration sorting remains available after a reboot.
uint32_t probe_duration_ms(const char *path);
Status status();
Diagnostics diagnostics();

} // namespace lyra::audio
