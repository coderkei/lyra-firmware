/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../lyra_audio_internal.h"

namespace lyra::audio::internal {

void pcm_output_task(void *context)
{
    auto *output = static_cast<PcmOutput *>(context);
    if (!output || !output->staging_buffer) {
        if (output) {
            output->error = true;
            output->error_code = ESP_ERR_NO_MEM;
            output->stop_requested = true;
            output->drain_on_stop = false;
            output->finished = true;
            xSemaphoreGive(output->done);
        }
        vTaskDelete(nullptr);
        return;
    }

    bool channel_started = false;
    while (true) {
        if (output->stop_requested && !output->drain_on_stop) break;

        bool paused = false;
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        paused = s_status.paused;
        xSemaphoreGive(s_state_mutex);
        if (paused && !output->stop_requested) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
            continue;
        }

        const size_t available = xStreamBufferBytesAvailable(output->stream);
        if (!channel_started && available < kPcmOutputStartBytes && !output->stop_requested) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
            continue;
        }
        if (available < s_diagnostics.pcm_buffer_low_watermark) {
            s_diagnostics.pcm_buffer_low_watermark = available;
        }
        if (available == 0) {
            // On normal EOF, stop_requested means the producer is finished;
            // drain the last buffered PCM before allowing the channel to stop.
            if (output->stop_requested) break;
            ++s_diagnostics.pcm_underrun_count;
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
            continue;
        }

        if (!channel_started) {
            if (!preload_and_start_i2s(output)) break;
            channel_started = true;
            continue;
        }

        size_t wanted = std::min(available, kPcmOutputChunkBytes);
        wanted -= wanted % kI2sFrameBytes;
        if (wanted == 0) {
            fail_pcm_output(output, ESP_ERR_INVALID_SIZE);
            break;
        }

        const size_t received = xStreamBufferReceive(
            output->stream, output->staging_buffer, wanted, 0);
        if (received == 0 || received % kI2sFrameBytes != 0) {
            fail_pcm_output(output, ESP_ERR_INVALID_SIZE);
            break;
        }
        lyra::audio::pcm::apply_output_processing(
            output, output->staging_buffer, received, pcm_processing_settings());
        mirror_to_speaker(output->staging_buffer, received);
        const esp_err_t write_ret = write_i2s_data(
            s_i2s_tx, output->staging_buffer, received);
        if (write_ret != ESP_OK) fail_pcm_output(output, write_ret);
        if (output->error) break;
    }

    output->finished = true;
    xSemaphoreGive(output->done);
    vTaskDelete(nullptr);
}

bool start_pcm_output(PcmOutput *output)
{
    if (!output || !s_i2s_tx || output->sample_rate == 0) return false;

    output->ring_buffer = static_cast<uint8_t *>(heap_caps_malloc(
        kPcmOutputBufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!output->ring_buffer) {
        output->ring_buffer = static_cast<uint8_t *>(alloc_audio_buffer(
            kPcmOutputBufferBytes));
    }
    output->staging_buffer = static_cast<uint8_t *>(alloc_audio_dma_buffer(
        kPcmOutputChunkBytes));
    if (!output->ring_buffer || !output->staging_buffer) {
        heap_caps_free(output->ring_buffer);
        heap_caps_free(output->staging_buffer);
        output->ring_buffer = nullptr;
        output->staging_buffer = nullptr;
        return false;
    }

    output->stream = xStreamBufferCreateStatic(
        kPcmOutputBufferBytes, kI2sFrameBytes, output->ring_buffer,
        &output->stream_storage);
    output->done = xSemaphoreCreateBinary();
    if (!output->stream || !output->done ||
        xTaskCreatePinnedToCore(pcm_output_task, "lyra_i2s", kAudioOutputTaskStack,
                                output, kAudioOutputTaskPriority, &output->task, 0) != pdPASS) {
        if (output->stream) vStreamBufferDelete(output->stream);
        if (output->done) vSemaphoreDelete(output->done);
        heap_caps_free(output->ring_buffer);
        heap_caps_free(output->staging_buffer);
        output->ring_buffer = nullptr;
        output->staging_buffer = nullptr;
        output->stream = nullptr;
        output->done = nullptr;
        output->task = nullptr;
        return false;
    }
    return true;
}

void stop_pcm_output(PcmOutput *output, bool drain, uint32_t generation)
{
    if (!output || !output->stream) return;

    output->drain_on_stop = drain;
    output->stop_requested = true;
    if (output->task && !output->finished) xTaskNotifyGive(output->task);

    const TickType_t wait_started = xTaskGetTickCount();
    const TickType_t wait_limit = pdMS_TO_TICKS(2000);
    while (!output->finished) {
        // Do not make a newly requested track wait for the old track's entire
        // PCM backlog. A normal EOF drains cleanly; cancellation drops it.
        if (output->drain_on_stop && !generation_is_current(generation)) {
            output->drain_on_stop = false;
            if (output->task) xTaskNotifyGive(output->task);
        }
        if (xSemaphoreTake(output->done, pdMS_TO_TICKS(50)) == pdTRUE) break;
        if ((xTaskGetTickCount() - wait_started) >= wait_limit) break;
    }

    if (!output->finished && output->task) {
        ESP_LOGW(kTag, "timed out stopping I2S output task");
        vTaskDelete(output->task);
    }
    if (output->stream) vStreamBufferDelete(output->stream);
    if (output->done) vSemaphoreDelete(output->done);
    heap_caps_free(output->ring_buffer);
    heap_caps_free(output->staging_buffer);
    output->ring_buffer = nullptr;
    output->staging_buffer = nullptr;
    output->stream = nullptr;
    output->done = nullptr;
    output->task = nullptr;
}

esp_err_t queue_pcm(const uint8_t *pcm, size_t pcm_bytes, PcmOutput *output,
                    uint32_t generation)
{
    if (!pcm || pcm_bytes == 0 || !output || !output->stream) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t sent_total = 0;
    while (sent_total < pcm_bytes) {
        if (!generation_is_current(generation)) return ESP_ERR_INVALID_STATE;
        if (output->error) return output->error_code;
        if (output->finished) return ESP_ERR_INVALID_STATE;

        const size_t sent = xStreamBufferSend(
            output->stream, pcm + sent_total, pcm_bytes - sent_total,
            pdMS_TO_TICKS(20));
        if (sent > 0) {
            sent_total += sent;
            if (output->task) xTaskNotifyGive(output->task);
        }
    }
    return ESP_OK;
}

bool read_pause_state(uint32_t generation)
{
    while (true) {
        bool paused;
        bool current;
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        current = generation == s_request_generation;
        paused = s_status.paused;
        xSemaphoreGive(s_state_mutex);
        if (!current) return false;
        if (!paused) return true;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
    }
}

void set_error(esp_err_t error, const char *path)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_status.last_error = error;
    s_status.playing = false;
    s_status.paused = false;
    s_status.eof = false;
    if (path) {
        std::strncpy(s_status.path, path, sizeof(s_status.path) - 1);
        s_status.path[sizeof(s_status.path) - 1] = '\0';
    }
    xSemaphoreGive(s_state_mutex);
}

esp_err_t configure_i2s(uint32_t sample_rate)
{
    if (sample_rate == 0 || s_i2s_tx == nullptr) return ESP_ERR_INVALID_ARG;

    if (s_i2s_started) {
        const esp_err_t disable_ret = i2s_channel_disable(s_i2s_tx);
        if (disable_ret != ESP_OK) return disable_ret;
        s_i2s_started = false;
    }
    disable_speaker_i2s();

    i2s_std_clk_config_t clock_config = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    // Match the vendor speaker demo. MCLK is not routed on this board, but
    // the multiple still determines the generated BCLK accuracy.
    clock_config.mclk_multiple = I2S_MCLK_MULTIPLE_128;
    const esp_err_t clock_ret = i2s_channel_reconfig_std_clock(s_i2s_tx, &clock_config);
    if (clock_ret != ESP_OK) return clock_ret;

    s_speaker_output_faulted = s_i2s_speaker_tx == nullptr;
    if (s_i2s_speaker_tx != nullptr) {
        const esp_err_t speaker_clock_ret = i2s_channel_reconfig_std_clock(
            s_i2s_speaker_tx, &clock_config);
        if (speaker_clock_ret != ESP_OK) {
            ESP_LOGW(kTag, "could not configure on-board speaker I2S: %s",
                     esp_err_to_name(speaker_clock_ret));
            s_speaker_output_faulted = true;
        }
    }

    // Leave the channel in READY state. The PCM output task preloads the DMA
    // descriptors and enables I2S only once complete stereo frames are ready.
    return ESP_OK;
}

esp_err_t write_pcm(const uint8_t *pcm, size_t pcm_bytes, uint8_t channels,
                    uint8_t bits_per_sample,
                    int16_t *stereo_buffer, size_t stereo_buffer_bytes,
                    PcmOutput *output,
                    uint32_t generation, bool unsigned_8bit)
{
    if (pcm == nullptr || pcm_bytes == 0 || channels == 0 || channels > 8 ||
        (bits_per_sample != 8 && bits_per_sample != 16 && bits_per_sample != 24 &&
         bits_per_sample != 32) ||
        stereo_buffer == nullptr || output == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t bytes_per_sample = bits_per_sample / 8;
    const size_t bytes_per_frame = bytes_per_sample * channels;
    if (bytes_per_frame == 0 || pcm_bytes % bytes_per_frame != 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    for (size_t input_offset = 0; input_offset < pcm_bytes;) {
        if (!generation_is_current(generation)) return ESP_ERR_INVALID_STATE;
        size_t chunk_bytes = std::min(pcm_bytes - input_offset, kPcmConvertChunkBytes);
        chunk_bytes -= chunk_bytes % bytes_per_frame;
        if (chunk_bytes == 0) chunk_bytes = bytes_per_frame;
        const size_t input_frame_count = chunk_bytes / bytes_per_frame;
        const size_t output_bytes = input_frame_count * 2 * sizeof(int16_t);
        if (output_bytes > stereo_buffer_bytes || output_bytes % kI2sFrameBytes != 0) {
            return ESP_ERR_INVALID_SIZE;
        }

        size_t output_index = 0;
        for (size_t frame_index = 0; frame_index < input_frame_count; ++frame_index) {
            const uint8_t *frame = pcm + input_offset + frame_index * bytes_per_frame;
            int16_t left = 0;
            int16_t right = 0;
            if (channels == 1) {
                left = right = lyra::audio::pcm::sample_to_i16(frame, bits_per_sample);
            } else if (channels == 2) {
                left = lyra::audio::pcm::sample_to_i16(frame, bits_per_sample);
                right = lyra::audio::pcm::sample_to_i16(frame + bytes_per_sample,
                                                        bits_per_sample);
            } else {
                // The I2S path is stereo. Preserve the energy of multichannel
                // FLACs by averaging all decoded channels into a centered mono
                // signal instead of rejecting otherwise valid 5.1/7.1 files.
                int32_t sum = 0;
                for (uint8_t channel = 0; channel < channels; ++channel) {
                    sum += lyra::audio::pcm::sample_to_i16(
                        frame + channel * bytes_per_sample, bits_per_sample);
                }
                left = right = static_cast<int16_t>(sum / channels);
            }
            if (bits_per_sample == 8 && unsigned_8bit) {
                if (channels == 1) {
                    left = right = static_cast<int16_t>(
                        (static_cast<int16_t>(frame[0]) - 128) << 8);
                } else if (channels == 2) {
                    left = static_cast<int16_t>((static_cast<int16_t>(frame[0]) - 128) << 8);
                    right = static_cast<int16_t>((static_cast<int16_t>(frame[1]) - 128) << 8);
                } else {
                    int32_t sum = 0;
                    for (uint8_t channel = 0; channel < channels; ++channel) {
                        sum += (static_cast<int32_t>(frame[channel]) - 128) << 8;
                    }
                    left = right = static_cast<int16_t>(sum / channels);
                }
            }
            stereo_buffer[output_index++] = left;
            stereo_buffer[output_index++] = right;
        }
        const esp_err_t queue_result = queue_pcm(
            reinterpret_cast<const uint8_t *>(stereo_buffer), output_bytes, output, generation);
        if (queue_result != ESP_OK) return queue_result;
        input_offset += chunk_bytes;
        vTaskDelay(1);
    }
    return ESP_OK;
}

} // namespace lyra::audio::internal
