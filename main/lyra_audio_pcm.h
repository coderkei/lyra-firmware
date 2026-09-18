/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "lyra_audio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

namespace lyra::audio::pcm {

struct EqualizerBiquad {
    float b0 = 1.0f;
    float b1 = 0.0f;
    float b2 = 0.0f;
    float a1 = 0.0f;
    float a2 = 0.0f;
    float target_b0 = 1.0f;
    float target_b1 = 0.0f;
    float target_b2 = 0.0f;
    float target_a1 = 0.0f;
    float target_a2 = 0.0f;
    float step_b0 = 0.0f;
    float step_b1 = 0.0f;
    float step_b2 = 0.0f;
    float step_a1 = 0.0f;
    float step_a2 = 0.0f;
    float z1_left = 0.0f;
    float z2_left = 0.0f;
    float z1_right = 0.0f;
    float z2_right = 0.0f;
    uint16_t ramp_frames = 0;
};

struct PcmOutput {
    uint8_t *ring_buffer;
    uint8_t *staging_buffer;
    StreamBufferHandle_t stream;
    StaticStreamBuffer_t stream_storage;
    SemaphoreHandle_t done;
    TaskHandle_t task;
    volatile bool stop_requested;
    volatile bool drain_on_stop;
    volatile bool finished;
    volatile bool error;
    esp_err_t error_code;
    int32_t gain_q15;
    uint32_t sample_rate;
    uint32_t equalizer_generation;
    bool equalizer_initialized;
    EqualizerBiquad equalizer[lyra::audio::kEqualizerBandCount];
};

struct ProcessingSettings {
    uint8_t volume_percent;
    uint8_t maximum_volume_percent;
    int16_t replay_gain_tenths_db;
    uint8_t transition_gain_percent;
    EqualizerSettings equalizer;
    uint32_t equalizer_generation;
};

int16_t sample_to_i16(const uint8_t *sample, uint8_t bits_per_sample);
void apply_output_processing(PcmOutput *output, uint8_t *pcm, size_t pcm_bytes,
                             const ProcessingSettings &settings);

} // namespace lyra::audio::pcm
