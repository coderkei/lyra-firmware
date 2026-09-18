/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lyra_audio_pcm.h"

#include <algorithm>
#include <cmath>

namespace lyra::audio::pcm {
namespace {

constexpr size_t kStereoFrameBytes = sizeof(int16_t) * 2;
constexpr int32_t kMaximumOutputGainQ15 = 16384;
constexpr int32_t kGainRampStepQ15 = 4;
constexpr uint16_t kEqualizerCoefficientRampFrames = 128;
constexpr float kEqualizerQ = 1.0f;
constexpr float kEqualizerPi = 3.14159265358979323846f;
constexpr float kEqualizerCenterFrequenciesHz[kEqualizerBandCount] = {
    60.0f, 250.0f, 1000.0f, 4000.0f, 16000.0f,
};

int16_t scale_sample(int16_t sample, int32_t gain_q15)
{
    const int64_t scaled = (static_cast<int64_t>(sample) * gain_q15) >> 15;
    if (scaled > 32767) return 32767;
    if (scaled < -32768) return -32768;
    return static_cast<int16_t>(scaled);
}

void apply_output_gain(uint8_t *pcm, size_t pcm_bytes, int32_t *current_gain_q15,
                       const ProcessingSettings &settings)
{
    if (!pcm || !current_gain_q15 || pcm_bytes % kStereoFrameBytes != 0) return;

    const int32_t volume_gain_q15 = static_cast<int32_t>(
        (static_cast<uint32_t>(settings.volume_percent) * kMaximumOutputGainQ15) /
        std::max<uint8_t>(settings.maximum_volume_percent, 1));
    const float replay_multiplier = std::pow(
        10.0f, static_cast<float>(settings.replay_gain_tenths_db) / 200.0f);
    const int32_t replay_gain_q15 = static_cast<int32_t>(std::lround(
        static_cast<float>(volume_gain_q15) * replay_multiplier));
    const int32_t target_gain_q15 = std::min<int32_t>(kMaximumOutputGainQ15,
        std::max<int32_t>(0, (replay_gain_q15 * settings.transition_gain_percent) / 100));

    auto *samples = reinterpret_cast<int16_t *>(pcm);
    const size_t frame_count = pcm_bytes / kStereoFrameBytes;
    int32_t gain_q15 = *current_gain_q15;
    for (size_t frame = 0; frame < frame_count; ++frame) {
        if (gain_q15 < target_gain_q15) {
            gain_q15 = std::min(gain_q15 + kGainRampStepQ15, target_gain_q15);
        } else if (gain_q15 > target_gain_q15) {
            gain_q15 = std::max(gain_q15 - kGainRampStepQ15, target_gain_q15);
        }
        samples[frame * 2] = scale_sample(samples[frame * 2], gain_q15);
        samples[frame * 2 + 1] = scale_sample(samples[frame * 2 + 1], gain_q15);
    }
    *current_gain_q15 = gain_q15;
}

void set_equalizer_target(EqualizerBiquad *filter, uint32_t sample_rate,
                          float center_frequency_hz, int16_t gain_tenths_db,
                          bool immediate)
{
    if (!filter) return;

    float b0 = 1.0f;
    float b1 = 0.0f;
    float b2 = 0.0f;
    float a1 = 0.0f;
    float a2 = 0.0f;
    const float nyquist_safe_frequency = static_cast<float>(sample_rate) * 0.45f;
    if (gain_tenths_db != 0 && sample_rate != 0 &&
        center_frequency_hz < nyquist_safe_frequency) {
        const float gain_db = static_cast<float>(gain_tenths_db) / 10.0f;
        const float amplitude = std::pow(10.0f, gain_db / 40.0f);
        const float omega = 2.0f * kEqualizerPi * center_frequency_hz /
                            static_cast<float>(sample_rate);
        const float alpha = std::sin(omega) / (2.0f * kEqualizerQ);
        const float cosine = std::cos(omega);
        const float a0 = 1.0f + alpha / amplitude;
        b0 = (1.0f + alpha * amplitude) / a0;
        b1 = (-2.0f * cosine) / a0;
        b2 = (1.0f - alpha * amplitude) / a0;
        a1 = (-2.0f * cosine) / a0;
        a2 = (1.0f - alpha / amplitude) / a0;
    }

    filter->target_b0 = b0;
    filter->target_b1 = b1;
    filter->target_b2 = b2;
    filter->target_a1 = a1;
    filter->target_a2 = a2;
    if (immediate) {
        filter->b0 = b0;
        filter->b1 = b1;
        filter->b2 = b2;
        filter->a1 = a1;
        filter->a2 = a2;
        filter->ramp_frames = 0;
        return;
    }

    filter->step_b0 = (b0 - filter->b0) / kEqualizerCoefficientRampFrames;
    filter->step_b1 = (b1 - filter->b1) / kEqualizerCoefficientRampFrames;
    filter->step_b2 = (b2 - filter->b2) / kEqualizerCoefficientRampFrames;
    filter->step_a1 = (a1 - filter->a1) / kEqualizerCoefficientRampFrames;
    filter->step_a2 = (a2 - filter->a2) / kEqualizerCoefficientRampFrames;
    filter->ramp_frames = kEqualizerCoefficientRampFrames;
}

void update_equalizer_coefficients(EqualizerBiquad *filter)
{
    if (!filter || filter->ramp_frames == 0) return;
    filter->b0 += filter->step_b0;
    filter->b1 += filter->step_b1;
    filter->b2 += filter->step_b2;
    filter->a1 += filter->step_a1;
    filter->a2 += filter->step_a2;
    --filter->ramp_frames;
    if (filter->ramp_frames == 0) {
        filter->b0 = filter->target_b0;
        filter->b1 = filter->target_b1;
        filter->b2 = filter->target_b2;
        filter->a1 = filter->target_a1;
        filter->a2 = filter->target_a2;
    }
}

float process_equalizer_sample(const EqualizerBiquad &filter, float sample,
                               float *z1, float *z2)
{
    const float output = filter.b0 * sample + *z1;
    *z1 = filter.b1 * sample - filter.a1 * output + *z2;
    *z2 = filter.b2 * sample - filter.a2 * output;
    return output;
}

void configure_equalizer(PcmOutput *output, const ProcessingSettings &settings)
{
    if (!output || (output->equalizer_initialized &&
                    output->equalizer_generation == settings.equalizer_generation)) return;

    const bool immediate = !output->equalizer_initialized;
    for (size_t band = 0; band < kEqualizerBandCount; ++band) {
        set_equalizer_target(&output->equalizer[band], output->sample_rate,
                             kEqualizerCenterFrequenciesHz[band],
                             settings.equalizer.band_tenths_db[band], immediate);
    }
    output->equalizer_generation = settings.equalizer_generation;
    output->equalizer_initialized = true;
}

void apply_equalizer(PcmOutput *output, uint8_t *pcm, size_t pcm_bytes,
                     const ProcessingSettings &settings)
{
    if (!output || !pcm || pcm_bytes % kStereoFrameBytes != 0) return;
    configure_equalizer(output, settings);

    auto *samples = reinterpret_cast<int16_t *>(pcm);
    const size_t frame_count = pcm_bytes / kStereoFrameBytes;
    for (size_t frame = 0; frame < frame_count; ++frame) {
        for (size_t band = 0; band < kEqualizerBandCount; ++band) {
            update_equalizer_coefficients(&output->equalizer[band]);
        }
        float left = static_cast<float>(samples[frame * 2]) / 32768.0f;
        float right = static_cast<float>(samples[frame * 2 + 1]) / 32768.0f;
        for (size_t band = 0; band < kEqualizerBandCount; ++band) {
            EqualizerBiquad &filter = output->equalizer[band];
            left = process_equalizer_sample(filter, left, &filter.z1_left, &filter.z2_left);
            right = process_equalizer_sample(filter, right, &filter.z1_right, &filter.z2_right);
        }
        left = std::clamp(left, -1.0f, 32767.0f / 32768.0f);
        right = std::clamp(right, -1.0f, 32767.0f / 32768.0f);
        samples[frame * 2] = static_cast<int16_t>(std::lround(left * 32768.0f));
        samples[frame * 2 + 1] = static_cast<int16_t>(std::lround(right * 32768.0f));
    }
}

} // namespace

int16_t sample_to_i16(const uint8_t *sample, uint8_t bits_per_sample)
{
    if (bits_per_sample == 8) {
        return static_cast<int16_t>(static_cast<int16_t>(static_cast<int8_t>(sample[0])) << 8);
    }
    if (bits_per_sample == 16) {
        const uint16_t value = static_cast<uint16_t>(sample[0]) |
                               (static_cast<uint16_t>(sample[1]) << 8);
        return static_cast<int16_t>(value);
    }
    if (bits_per_sample == 24) {
        int32_t value = static_cast<int32_t>(sample[0]) |
                        (static_cast<int32_t>(sample[1]) << 8) |
                        (static_cast<int32_t>(sample[2]) << 16);
        if ((value & 0x00800000) != 0) value |= ~0x00FFFFFF;
        return static_cast<int16_t>(value >> 8);
    }

    const uint32_t value = static_cast<uint32_t>(sample[0]) |
                           (static_cast<uint32_t>(sample[1]) << 8) |
                           (static_cast<uint32_t>(sample[2]) << 16) |
                           (static_cast<uint32_t>(sample[3]) << 24);
    return static_cast<int16_t>(static_cast<int32_t>(value) >> 16);
}

void apply_output_processing(PcmOutput *output, uint8_t *pcm, size_t pcm_bytes,
                             const ProcessingSettings &settings)
{
    if (!output) return;
    apply_output_gain(pcm, pcm_bytes, &output->gain_q15, settings);
    apply_equalizer(output, pcm, pcm_bytes, settings);
}

} // namespace lyra::audio::pcm
