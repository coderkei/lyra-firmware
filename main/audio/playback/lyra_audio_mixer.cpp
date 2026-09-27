/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../lyra_audio_internal.h"

namespace lyra::audio::internal {
namespace {

constexpr size_t kMixFrames = 512;
constexpr size_t kPreloadFrames = 2048;

enum class FrameResult { kReady, kWaiting, kEnd };

void decoder_worker(void *argument)
{
    auto *track = static_cast<DecoderStream *>(argument);
    track->result = decode_file(track);
    ESP_LOGI(kTag, "decoder stack spare=%u bytes for %s",
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)), track->path);
    track->done.store(true, std::memory_order_release);
    // The mixer owns the task and frees its external stack after seeing done.
    vTaskSuspend(nullptr);
}

bool open_track(DecoderStream *track, const char *path, uint32_t generation,
                uint32_t start_position_ms, bool pause_after_seek,
                int16_t replay_gain_tenths_db)
{
    if (!track || !path || track->pcm.stream) return false;
    track->pcm.stop_requested = false;
    track->pcm.finished = false;
    track->pcm.error = false;
    track->pcm.error_code = ESP_OK;
    track->pcm.sample_rate = 0;
    track->done.store(false, std::memory_order_release);
    track->native_rate.store(0, std::memory_order_release);
    track->result = ESP_OK;
    track->status = {};
    track->emitted_frames = 0;
    track->cache_index = 0;
    track->cache_frames = 0;
    track->phase = 0;
    track->have_left = false;
    track->have_right = false;
    track->right_is_last = false;
    const size_t path_length = std::min(std::strlen(path), sizeof(track->path) - 1);
    std::memcpy(track->path, path, path_length);
    track->path[path_length] = '\0';
    track->generation = generation;
    track->start_position_ms = start_position_ms;
    track->pause_after_seek = pause_after_seek;
    track->replay_gain_tenths_db = replay_gain_tenths_db;
    track->linear_gain = std::pow(10.0f, replay_gain_tenths_db / 200.0f);
    track->pcm.ring_buffer = static_cast<uint8_t *>(heap_caps_malloc(
        kDecodedTrackBufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!track->pcm.ring_buffer) {
        ESP_LOGE(kTag, "decoder FIFO allocation failed for %s (PSRAM free=%u)",
                 track->path, static_cast<unsigned>(
                     heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
        return false;
    }
    track->pcm.stream = xStreamBufferCreateStatic(
        kDecodedTrackBufferBytes, kI2sFrameBytes, track->pcm.ring_buffer,
        &track->pcm.stream_storage);
    if (!track->pcm.stream) {
        ESP_LOGE(kTag, "decoder stream creation failed for %s", track->path);
        heap_caps_free(track->pcm.ring_buffer);
        track->pcm.ring_buffer = nullptr;
        return false;
    }
    // Two 24 KiB internal stacks do not fit alongside the GUI and I2S tasks.
    // Playback workers do not write flash; a PSRAM stack requires the cache
    // to stay enabled while the worker runs.
    if (xTaskCreatePinnedToCoreWithCaps(decoder_worker, "lyra_decode", kAudioTaskStack,
                                        track, kAudioTaskPriority, &track->worker_task, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(kTag, "decoder task creation failed for %s (internal largest=%u, "
                 "PSRAM largest=%u)", track->path,
                 static_cast<unsigned>(heap_caps_get_largest_free_block(
                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(
                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
        vStreamBufferDelete(track->pcm.stream);
        heap_caps_free(track->pcm.ring_buffer);
        track->pcm.stream = nullptr;
        track->pcm.ring_buffer = nullptr;
        return false;
    }
    sample_audio_memory();
    return true;
}

void close_track(DecoderStream *track)
{
    if (!track || !track->pcm.stream) return;
    track->pcm.stop_requested = true;
    const TickType_t started = xTaskGetTickCount();
    bool warned = false;
    while (!track->done.load(std::memory_order_acquire)) {
        if (!warned && xTaskGetTickCount() - started >= pdMS_TO_TICKS(2000)) {
            ESP_LOGW(kTag, "waiting for decoder to release %s", track->path);
            warned = true;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    if (track->worker_task) {
        vTaskDeleteWithCaps(track->worker_task);
        track->worker_task = nullptr;
    }
    vStreamBufferDelete(track->pcm.stream);
    heap_caps_free(track->pcm.ring_buffer);
    track->pcm.stream = nullptr;
    track->pcm.ring_buffer = nullptr;
}

FrameResult source_frame(DecoderStream *track, int16_t *frame)
{
    if (track->cache_index == track->cache_frames) {
        const size_t available = xStreamBufferBytesAvailable(track->pcm.stream);
        const size_t wanted = std::min(available - available % kI2sFrameBytes,
                                       sizeof(track->cache));
        if (wanted) {
            const size_t received = xStreamBufferReceive(
                track->pcm.stream, track->cache, wanted, 0);
            track->cache_index = 0;
            track->cache_frames = received / kI2sFrameBytes;
        }
        if (track->cache_index == track->cache_frames) {
            return track->done.load(std::memory_order_acquire) ?
                FrameResult::kEnd : FrameResult::kWaiting;
        }
    }
    frame[0] = track->cache[track->cache_index * 2];
    frame[1] = track->cache[track->cache_index * 2 + 1];
    ++track->cache_index;
    return FrameResult::kReady;
}

FrameResult next_frame(DecoderStream *track, uint32_t output_rate, int16_t *frame)
{
    const uint32_t input_rate = track->native_rate.load(std::memory_order_acquire);
    if (input_rate == 0 || output_rate == 0) return FrameResult::kWaiting;
    if (input_rate == output_rate) {
        const FrameResult result = source_frame(track, frame);
        if (result == FrameResult::kReady) ++track->emitted_frames;
        return result;
    }

    if (!track->have_left) {
        const FrameResult result = source_frame(track, track->left);
        if (result != FrameResult::kReady) return result;
        track->have_left = true;
    }
    if (!track->have_right) {
        const FrameResult result = source_frame(track, track->right);
        if (result == FrameResult::kWaiting) return result;
        if (result == FrameResult::kEnd) {
            track->right[0] = track->left[0];
            track->right[1] = track->left[1];
            track->right_is_last = true;
        }
        track->have_right = true;
    }

    while (track->phase >= output_rate) {
        if (track->right_is_last) return FrameResult::kEnd;
        track->phase -= output_rate;
        track->left[0] = track->right[0];
        track->left[1] = track->right[1];
        track->have_right = false;
        const FrameResult result = source_frame(track, track->right);
        if (result == FrameResult::kWaiting) return result;
        if (result == FrameResult::kEnd) {
            track->right[0] = track->left[0];
            track->right[1] = track->left[1];
            track->right_is_last = true;
        }
        track->have_right = true;
    }

    for (int channel = 0; channel < 2; ++channel) {
        const int32_t difference = static_cast<int32_t>(track->right[channel]) -
                                   track->left[channel];
        frame[channel] = static_cast<int16_t>(
            track->left[channel] + difference * static_cast<int64_t>(track->phase) /
            output_rate);
    }
    track->phase += input_rate;
    ++track->emitted_frames;
    return FrameResult::kReady;
}

int16_t mixed_sample(float value)
{
    return static_cast<int16_t>(std::clamp<int32_t>(
        static_cast<int32_t>(std::lround(value)), -32768, 32767));
}

void publish_track(DecoderStream *track, uint32_t output_rate, bool transitioned,
                   uint32_t queue_cookie)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (track->generation != s_request_generation) {
        xSemaphoreGive(s_state_mutex);
        return;
    }
    const uint8_t volume = s_status.volume_percent;
    const bool paused = s_status.paused;
    const uint32_t serial = s_status.playback_serial + (transitioned ? 1u : 0u);
    s_status = track->status;
    s_status.initialized = true;
    s_status.playing = true;
    s_status.eof = false;
    s_status.paused = paused;
    s_status.volume_percent = volume;
    s_status.playback_serial = serial;
    s_status.queue_cookie = queue_cookie;
    s_status.transitioned = transitioned;
    s_status.transitioning = false;
    s_status.position_ms = track->start_position_ms +
        static_cast<uint32_t>(track->emitted_frames * 1000u / output_rate);
    xSemaphoreGive(s_state_mutex);
}

void update_position(DecoderStream *track, uint32_t output_rate)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (track->generation != s_request_generation) {
        xSemaphoreGive(s_state_mutex);
        return;
    }
    s_status.decoded_bytes = track->status.decoded_bytes;
    s_status.duration_ms = track->status.duration_ms;
    s_status.position_ms = track->start_position_ms +
        static_cast<uint32_t>(track->emitted_frames * 1000u / output_rate);
    xSemaphoreGive(s_state_mutex);
}

bool track_ready(DecoderStream *track)
{
    if (!track || !track->pcm.stream) return false;
    return track->cache_index < track->cache_frames || track->have_left ||
           xStreamBufferBytesAvailable(track->pcm.stream) >=
               kPreloadFrames * kI2sFrameBytes ||
           track->done.load(std::memory_order_acquire);
}

void log_diagnostics(const Diagnostics &d)
{
    ESP_LOGI(kTag, "PCM stats: frames=%llu gapless=%u overlap=%u format=%u "
             "resampled=%u late=%u preload-fail=%u source-underruns=%u "
             "pcm-fifo-underruns=%u mix-max=%u us pcm-low=%u bytes "
             "sd-read-max=%u us sd-lock-max=%u us internal-min=%u "
             "psram-min=%u largest-internal-min=%u output=%u Hz "
             "scratch-internal=%u/%u mixer-stack-spare=%u",
             static_cast<unsigned long long>(d.mixed_frames),
             d.gapless_transitions, d.crossfade_transitions,
             d.format_transitions, d.resampled_transitions,
             d.late_preloads, d.preload_failures, d.underrun_count,
             d.pcm_underrun_count, d.max_mix_block_us,
             static_cast<unsigned>(d.pcm_buffer_low_watermark),
             d.max_sd_read_us, d.max_sd_lock_wait_us,
             static_cast<unsigned>(d.min_internal_free_bytes),
             static_cast<unsigned>(d.min_psram_free_bytes),
             static_cast<unsigned>(d.min_internal_largest_block),
             d.output_sample_rate, static_cast<unsigned>(d.pcm_internal),
             static_cast<unsigned>(d.stereo_internal),
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
}

} // namespace

bool decoder_is_current(DecoderStream *track)
{
    if (!track || track->pcm.stop_requested) return false;
    return generation_is_current(track->generation);
}

void decoder_set_error(DecoderStream *track, esp_err_t error)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    track->status.last_error = error;
    xSemaphoreGive(s_state_mutex);
}

void sample_audio_memory()
{
    const size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    auto &d = s_diagnostics;
    d.min_internal_free_bytes = d.min_internal_free_bytes ?
        std::min(d.min_internal_free_bytes, internal_free) : internal_free;
    d.min_psram_free_bytes = d.min_psram_free_bytes ?
        std::min(d.min_psram_free_bytes, psram_free) : psram_free;
    d.min_internal_largest_block = d.min_internal_largest_block ?
        std::min(d.min_internal_largest_block, largest) : largest;
    xSemaphoreGive(s_state_mutex);
}

void audio_task(void *)
{
    uint32_t handled_generation = 0;
    while (true) {
        char path[kMaxPath]{};
        uint32_t generation, start_ms;
        bool pause_after_seek;
        int16_t gain;
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        generation = s_request_generation;
        start_ms = s_requested_seek_ms;
        pause_after_seek = s_requested_pause_after_seek;
        gain = s_replay_gain_tenths_db;
        std::memcpy(path, s_requested_path, sizeof(path));
        path[sizeof(path) - 1] = '\0';
        xSemaphoreGive(s_state_mutex);
        if (generation == handled_generation) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        handled_generation = generation;
        if (!path[0]) {
            if (s_i2s_started) {
                i2s_channel_disable(s_i2s_tx);
                s_i2s_started = false;
            }
            disable_speaker_i2s();
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            s_status.playing = false;
            s_status.paused = false;
            s_status.eof = false;
            s_status.transitioning = false;
            s_status.path[0] = '\0';
            xSemaphoreGive(s_state_mutex);
            continue;
        }

        DecoderStream tracks[2]{};
        DecoderStream *current = &tracks[0];
        DecoderStream *next = &tracks[1];
        PcmOutput output{};
        uint32_t output_rate = 0;
        uint32_t next_version = 0;
        NextTrackRequest requested{};
        bool fading = false;
        uint64_t fade_frames = 0, fade_index = 0;
        bool late_counted = false;
        bool held_a = false;
        int16_t held_frame[2]{};
        int64_t next_diagnostic_log_us = esp_timer_get_time() + 30'000'000;
        esp_err_t result = ESP_OK;
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_diagnostics = {};
        s_diagnostics.audio_buffer_low_watermark = kReadAheadBufferBytes;
        s_diagnostics.pcm_buffer_low_watermark = kPcmOutputBufferBytes;
        xSemaphoreGive(s_state_mutex);
        sample_audio_memory();
        if (!open_track(current, path, generation, start_ms,
                        pause_after_seek, gain)) result = ESP_ERR_NO_MEM;

        while (result == ESP_OK && generation_is_current(generation)) {
            if (esp_timer_get_time() >= next_diagnostic_log_us) {
                sample_audio_memory();
                xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                const Diagnostics snapshot = s_diagnostics;
                xSemaphoreGive(s_state_mutex);
                log_diagnostics(snapshot);
                next_diagnostic_log_us = esp_timer_get_time() + 30'000'000;
            }
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            const bool is_paused = s_status.paused;
            const uint32_t serial = s_status.playback_serial;
            const int16_t active_gain = s_replay_gain_tenths_db;
            const NextTrackRequest snapshot = s_next_track;
            xSemaphoreGive(s_state_mutex);
            if (active_gain != current->replay_gain_tenths_db) {
                current->replay_gain_tenths_db = active_gain;
                current->linear_gain = std::pow(10.0f, active_gain / 200.0f);
            }

            if (!fading && snapshot.version != next_version) {
                if (next->pcm.stream) close_track(next);
                next_version = snapshot.version;
                requested = snapshot;
                late_counted = false;
                if (requested.path[0] && requested.parent_serial == serial &&
                    !open_track(next, requested.path, generation, 0, false,
                                requested.replay_gain_tenths_db)) {
                    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                    ++s_diagnostics.preload_failures;
                    xSemaphoreGive(s_state_mutex);
                }
            }
            if (!fading && next->pcm.stream &&
                next->done.load(std::memory_order_acquire) &&
                (next->result != ESP_OK ||
                 xStreamBufferBytesAvailable(next->pcm.stream) == 0)) {
                ESP_LOGW(kTag, "preloaded decoder failed for %s: %s, buffered=%u",
                         next->path, esp_err_to_name(next->result),
                         static_cast<unsigned>(xStreamBufferBytesAvailable(next->pcm.stream)));
                close_track(next);
                xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                ++s_diagnostics.preload_failures;
                xSemaphoreGive(s_state_mutex);
            }

            if (output_rate == 0) {
                xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                output_rate = current->status.sample_rate;
                xSemaphoreGive(s_state_mutex);
                if (output_rate == 0) {
                    if (current->done.load(std::memory_order_acquire)) {
                        result = current->result == ESP_OK ? ESP_FAIL : current->result;
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(2));
                    continue;
                }
                result = configure_i2s(output_rate);
                output.sample_rate = output_rate;
                if (result != ESP_OK || !start_pcm_output(&output)) {
                    if (result == ESP_OK) result = ESP_ERR_NO_MEM;
                    break;
                }
                xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                s_diagnostics.output_sample_rate = output_rate;
                xSemaphoreGive(s_state_mutex);
                publish_track(current, output_rate, false, 0);
                sample_audio_memory();
            }
            if (is_paused) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }

            int16_t mixed[kMixFrames * 2]{};
            size_t produced = 0;
            bool end_of_session = false;
            DecoderStream *retired = nullptr;
            const int64_t block_started = esp_timer_get_time();
            while (produced < kMixFrames && generation_is_current(generation)) {
                if (!fading && next->pcm.stream && track_ready(next)) {
                    uint32_t duration_ms;
                    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                    duration_ms = current->status.duration_ms;
                    xSemaphoreGive(s_state_mutex);
                    const uint64_t played_ms = static_cast<uint64_t>(current->start_position_ms) +
                        current->emitted_frames * 1000u / output_rate;
                    const uint32_t remaining_ms = duration_ms > played_ms ?
                        static_cast<uint32_t>(duration_ms - played_ms) : 0;
                    if (requested.crossfade_ms &&
                        (requested.immediate ||
                         (duration_ms && remaining_ms <= requested.crossfade_ms))) {
                        fading = true;
                        fade_index = 0;
                        fade_frames = std::max<uint64_t>(1,
                            static_cast<uint64_t>(requested.crossfade_ms) * output_rate / 1000u);
                        if (!requested.immediate && remaining_ms) {
                            fade_frames = std::min<uint64_t>(fade_frames,
                                static_cast<uint64_t>(remaining_ms) * output_rate / 1000u);
                        }
                        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                        s_status.transitioning = true;
                        xSemaphoreGive(s_state_mutex);
                    }
                }

                int16_t a[2]{}, b[2]{};
                const FrameResult from_a = held_a ? FrameResult::kReady :
                    next_frame(current, output_rate, a);
                if (held_a) {
                    a[0] = held_frame[0];
                    a[1] = held_frame[1];
                    held_a = false;
                }
                if (from_a == FrameResult::kWaiting) break;
                if (from_a == FrameResult::kEnd && !next->pcm.stream) {
                    if (current->done.load(std::memory_order_acquire) &&
                        current->result != ESP_OK) result = current->result;
                    end_of_session = true;
                    break;
                }
                if (from_a == FrameResult::kEnd && next->pcm.stream && !track_ready(next)) {
                    if (!late_counted) {
                        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                        ++s_diagnostics.late_preloads;
                        xSemaphoreGive(s_state_mutex);
                        late_counted = true;
                    }
                    break;
                }
                if (fading && from_a != FrameResult::kEnd) {
                    const FrameResult from_b = next_frame(next, output_rate, b);
                    if (from_b == FrameResult::kWaiting) {
                        held_a = true;
                        held_frame[0] = a[0];
                        held_frame[1] = a[1];
                        break;
                    }
                    if (from_b == FrameResult::kEnd) {
                        // The successor failed or ended during overlap. Let the
                        // original track finish at full gain.
                        fading = false;
                        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                        ++s_diagnostics.preload_failures;
                        s_status.transitioning = false;
                        xSemaphoreGive(s_state_mutex);
                        retired = next;
                    }
                }

                if (from_a == FrameResult::kEnd ||
                    (fading && fade_index >= fade_frames)) {
                    if (!next->pcm.stream || !track_ready(next)) break;
                    if (from_a == FrameResult::kEnd) {
                        const FrameResult from_b = next_frame(next, output_rate, b);
                        if (from_b != FrameResult::kReady) break;
                    }
                    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                    const Status old_status = current->status;
                    const Status new_status = next->status;
                    xSemaphoreGive(s_state_mutex);
                    const uint32_t old_rate = old_status.sample_rate;
                    const uint8_t old_channels = old_status.channels;
                    const uint8_t old_bits = old_status.bits_per_sample;
                    const bool was_fading = fading;
                    fading = false;
                    retired = current;
                    current = next;
                    next = retired;
                    next_version = 0;
                    const uint32_t cookie = requested.queue_cookie;
                    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                    if (generation != s_request_generation) {
                        xSemaphoreGive(s_state_mutex);
                        result = ESP_ERR_INVALID_STATE;
                        break;
                    }
                    ++s_diagnostics.gapless_transitions;
                    if (was_fading) ++s_diagnostics.crossfade_transitions;
                    if (old_rate != new_status.sample_rate ||
                        old_channels != new_status.channels ||
                        old_bits != new_status.bits_per_sample) {
                        ++s_diagnostics.format_transitions;
                    }
                    if (old_rate != new_status.sample_rate) {
                        ++s_diagnostics.resampled_transitions;
                    }
                    s_next_track.path[0] = '\0';
                    ++s_next_track.version;
                    s_replay_gain_tenths_db = requested.replay_gain_tenths_db;
                    xSemaphoreGive(s_state_mutex);
                    ESP_LOGI(kTag, "PCM handoff %u/%u/%u -> %u/%u/%u, output=%u Hz, overlap=%s",
                             static_cast<unsigned>(old_rate), old_channels, old_bits,
                             static_cast<unsigned>(new_status.sample_rate),
                             new_status.channels, new_status.bits_per_sample,
                             static_cast<unsigned>(output_rate), was_fading ? "yes" : "no");
                    publish_track(current, output_rate, true, cookie);
                    sample_audio_memory();
                    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                    const Diagnostics handoff_stats = s_diagnostics;
                    xSemaphoreGive(s_state_mutex);
                    log_diagnostics(handoff_stats);
                    // A frame already taken from the successor belongs to the
                    // new track; do not discard or duplicate it.
                    a[0] = b[0];
                    a[1] = b[1];
                }

                float old_weight = 1.0f, new_weight = 0.0f;
                if (fading) {
                    new_weight = static_cast<float>(fade_index) / fade_frames;
                    old_weight = 1.0f - new_weight;
                    ++fade_index;
                }
                for (int channel = 0; channel < 2; ++channel) {
                    mixed[produced * 2 + channel] = !fading &&
                        current->linear_gain == 1.0f ? a[channel] :
                        mixed_sample(a[channel] * current->linear_gain * old_weight +
                                     b[channel] * next->linear_gain * new_weight);
                }
                ++produced;
                if (retired) break;
            }

            if (produced) {
                const uint32_t mix_us = static_cast<uint32_t>(
                    esp_timer_get_time() - block_started);
                result = queue_pcm(reinterpret_cast<const uint8_t *>(mixed),
                                   produced * kI2sFrameBytes, &output, generation);
                if (result != ESP_OK) break;
                update_position(current, output_rate);
                xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                s_diagnostics.mixed_frames += produced;
                s_diagnostics.max_mix_block_us = std::max<uint32_t>(
                    s_diagnostics.max_mix_block_us, mix_us);
                xSemaphoreGive(s_state_mutex);
            }
            if (retired) {
                close_track(retired);
                if (retired == next) {
                    // Reuse the retired slot for the next queued track.
                    next = retired;
                }
                requested = {};
                sample_audio_memory();
            }
            if (end_of_session) break;
            if (!produced) vTaskDelay(pdMS_TO_TICKS(2));
        }

        const bool completed = generation_is_current(generation) && result == ESP_OK;
        if (output.stream) {
            output.drain_on_stop = completed;
            output.stop_requested = true;
            if (output.task) xTaskNotifyGive(output.task);
        }
        close_track(current);
        if (next != current) close_track(next);
        stop_pcm_output(&output, completed, generation);
        if (output.error && result == ESP_OK) result = output.error_code;
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        const Diagnostics final_stats = s_diagnostics;
        xSemaphoreGive(s_state_mutex);
        log_diagnostics(final_stats);
        if (!generation_is_current(generation)) continue;
        if (s_i2s_started) {
            i2s_channel_disable(s_i2s_tx);
            s_i2s_started = false;
        }
        disable_speaker_i2s();
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_status.playing = false;
        s_status.paused = false;
        s_status.transitioning = false;
        s_status.eof = result == ESP_OK;
        s_status.last_error = result;
        if (result == ESP_OK && s_status.duration_ms) {
            s_status.position_ms = s_status.duration_ms;
        }
        xSemaphoreGive(s_state_mutex);
    }
}

} // namespace lyra::audio::internal
