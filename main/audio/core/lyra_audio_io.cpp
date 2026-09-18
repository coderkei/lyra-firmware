/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../lyra_audio_internal.h"

namespace lyra::audio::internal {

using lyra::audio::pcm::PcmOutput;

bool fail_pcm_output(PcmOutput *output, esp_err_t error)
{
    if (!output) return false;
    output->error = true;
    output->error_code = error;
    output->stop_requested = true;
    output->drain_on_stop = false;
    return false;
}

lyra::audio::pcm::ProcessingSettings pcm_processing_settings()
{
    lyra::audio::pcm::ProcessingSettings settings{};
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    settings.volume_percent = s_status.volume_percent;
    settings.maximum_volume_percent = s_maximum_volume_percent;
    settings.replay_gain_tenths_db = s_replay_gain_tenths_db;
    settings.transition_gain_percent = s_transition_gain_percent;
    settings.equalizer = s_equalizer;
    settings.equalizer_generation = s_equalizer_generation;
    xSemaphoreGive(s_state_mutex);
    return settings;
}

bool speaker_output_enabled()
{
    bool enabled = false;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    enabled = s_speaker_output_enabled;
    xSemaphoreGive(s_state_mutex);
    return enabled;
}

esp_err_t preload_i2s_data(i2s_chan_handle_t channel, const uint8_t *data,
                           size_t bytes)
{
    if (channel == nullptr || data == nullptr || bytes == 0 || bytes % kI2sFrameBytes != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t offset = 0;
    while (offset < bytes) {
        size_t loaded = 0;
        const esp_err_t result = i2s_channel_preload_data(
            channel, data + offset, bytes - offset, &loaded);
        if (result != ESP_OK) return result;
        if (loaded == 0 || loaded % kI2sFrameBytes != 0) return ESP_ERR_INVALID_SIZE;
        offset += loaded;
    }
    return ESP_OK;
}

esp_err_t write_i2s_data(i2s_chan_handle_t channel, const uint8_t *data,
                         size_t bytes)
{
    if (channel == nullptr || data == nullptr || bytes == 0 || bytes % kI2sFrameBytes != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t offset = 0;
    while (offset < bytes) {
        size_t written = 0;
        const esp_err_t result = i2s_channel_write(
            channel, data + offset, bytes - offset, &written, pdMS_TO_TICKS(250));
        if (result != ESP_OK) return result;
        if (written == 0 || written % kI2sFrameBytes != 0) return ESP_ERR_INVALID_SIZE;
        offset += written;
    }
    return ESP_OK;
}

void disable_speaker_i2s()
{
    if (!s_i2s_speaker_started || s_i2s_speaker_tx == nullptr) return;
    const esp_err_t result = i2s_channel_disable(s_i2s_speaker_tx);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "could not disable on-board speaker I2S: %s",
                 esp_err_to_name(result));
    }
    s_i2s_speaker_started = false;
}

bool start_speaker_i2s(const uint8_t *data, size_t bytes)
{
    if (s_i2s_speaker_tx == nullptr || s_speaker_output_faulted) return false;
    const esp_err_t preload_ret = preload_i2s_data(s_i2s_speaker_tx, data, bytes);
    if (preload_ret != ESP_OK) {
        ESP_LOGW(kTag, "could not preload on-board speaker I2S: %s",
                 esp_err_to_name(preload_ret));
        s_speaker_output_faulted = true;
        return false;
    }
    const esp_err_t enable_ret = i2s_channel_enable(s_i2s_speaker_tx);
    if (enable_ret != ESP_OK) {
        ESP_LOGW(kTag, "could not enable on-board speaker I2S: %s",
                 esp_err_to_name(enable_ret));
        s_speaker_output_faulted = true;
        return false;
    }
    s_i2s_speaker_started = true;
    return true;
}

void mirror_to_speaker(const uint8_t *data, size_t bytes)
{
    if (!speaker_output_enabled()) {
        disable_speaker_i2s();
        return;
    }
    if (!s_i2s_speaker_started) {
        start_speaker_i2s(data, bytes);
        return;
    }
    const esp_err_t result = write_i2s_data(s_i2s_speaker_tx, data, bytes);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "on-board speaker I2S write failed: %s", esp_err_to_name(result));
        disable_speaker_i2s();
        s_speaker_output_faulted = true;
    }
}

bool preload_and_start_i2s(PcmOutput *output)
{
    if (!output || !output->stream || !s_i2s_tx) return false;
    const bool mirror_speaker = speaker_output_enabled() &&
                                s_i2s_speaker_tx != nullptr &&
                                !s_speaker_output_faulted;

    // The channel is deliberately still in READY state here. Preloading the
    // complete DMA ring prevents the driver from transmitting its initial
    // empty descriptors (or stale data from the previous track) while the
    // decoder is still filling the PCM FIFO.
    size_t preloaded = 0;
    while (preloaded < kI2sDmaBufferBytes) {
        const size_t available = xStreamBufferBytesAvailable(output->stream);
        if (available == 0) {
            if (output->stop_requested) break;
            return fail_pcm_output(output, ESP_ERR_INVALID_SIZE);
        }

        const size_t wanted = std::min({available, kPcmOutputChunkBytes,
                                        kI2sDmaBufferBytes - preloaded});
        if (wanted < kI2sFrameBytes || wanted % kI2sFrameBytes != 0) {
            return fail_pcm_output(output, ESP_ERR_INVALID_SIZE);
        }
        const size_t received = xStreamBufferReceive(
            output->stream, output->staging_buffer, wanted, 0);
        if (received == 0 || received % kI2sFrameBytes != 0) {
            return fail_pcm_output(output, ESP_ERR_INVALID_SIZE);
        }
        lyra::audio::pcm::apply_output_processing(
            output, output->staging_buffer, received, pcm_processing_settings());

        const esp_err_t dac_preload_ret = preload_i2s_data(
            s_i2s_tx, output->staging_buffer, received);
        if (dac_preload_ret != ESP_OK) return fail_pcm_output(output, dac_preload_ret);
        if (mirror_speaker) {
            const esp_err_t speaker_preload_ret = preload_i2s_data(
                s_i2s_speaker_tx, output->staging_buffer, received);
            if (speaker_preload_ret != ESP_OK) {
                ESP_LOGW(kTag, "could not preload on-board speaker I2S: %s",
                         esp_err_to_name(speaker_preload_ret));
                s_speaker_output_faulted = true;
            }
        }
        preloaded += received;
    }

    // A short final buffer is only possible while stopping a very short track;
    // the remaining DMA descriptors are intentionally cleared by the driver.
    if (preloaded == 0) return false;
    const esp_err_t enable_ret = i2s_channel_enable(s_i2s_tx);
    if (enable_ret != ESP_OK) return fail_pcm_output(output, enable_ret);
    s_i2s_started = true;
    if (mirror_speaker && !s_speaker_output_faulted) {
        const esp_err_t speaker_enable_ret = i2s_channel_enable(s_i2s_speaker_tx);
        if (speaker_enable_ret != ESP_OK) {
            ESP_LOGW(kTag, "could not enable on-board speaker I2S: %s",
                     esp_err_to_name(speaker_enable_ret));
            s_speaker_output_faulted = true;
        } else {
            s_i2s_speaker_started = true;
        }
    }
    return true;
}

esp_err_t ensure_nvs_ready()
{
    if (s_nvs_ready) return ESP_OK;
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ret = nvs_flash_erase();
        if (ret == ESP_OK) ret = nvs_flash_init();
    }
    // Another component may already have initialized the default partition.
    if (ret == ESP_ERR_INVALID_STATE) ret = ESP_OK;
    if (ret == ESP_OK) s_nvs_ready = true;
    return ret;
}

uint8_t load_saved_volume()
{
    if (!s_nvs_ready) return lyra::audio::kDefaultVolumePercent;
    nvs_handle_t handle;
    if (nvs_open(kSettingsNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return lyra::audio::kDefaultVolumePercent;
    }
    uint8_t volume = lyra::audio::kDefaultVolumePercent;
    if (nvs_get_u8(handle, kVolumeKey, &volume) != ESP_OK ||
        volume > s_maximum_volume_percent) {
        volume = std::min<uint8_t>(lyra::audio::kDefaultVolumePercent,
                                   s_maximum_volume_percent);
    }
    nvs_close(handle);
    return volume;
}

void load_saved_maximum_volume()
{
    s_maximum_volume_percent = lyra::audio::kDefaultMaximumVolumePercent;
    if (!s_nvs_ready) return;
    nvs_handle_t handle;
    if (nvs_open(kSettingsNamespace, NVS_READONLY, &handle) != ESP_OK) return;
    uint8_t maximum = s_maximum_volume_percent;
    if (nvs_get_u8(handle, kMaximumVolumeKey, &maximum) == ESP_OK && maximum > 0 &&
        maximum <= lyra::audio::kMaximumVolumePercent) {
        s_maximum_volume_percent = maximum;
    }
    nvs_close(handle);
}

esp_err_t save_volume_to_nvs(uint8_t volume)
{
    if (!s_nvs_ready) return ESP_ERR_INVALID_STATE;
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(kSettingsNamespace, NVS_READWRITE, &handle);
    if (ret != ESP_OK) return ret;
    ret = nvs_set_u8(handle, kVolumeKey, volume);
    if (ret == ESP_OK) ret = nvs_commit(handle);
    nvs_close(handle);
    return ret;
}

void *alloc_audio_buffer(size_t size)
{
    // Decoder input, PCM, and the I2S staging buffer are on the realtime path.
    // Prefer internal SRAM (and DMA-capable SRAM for the final staging buffer)
    // before considering PSRAM.
    void *buffer = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buffer == nullptr) buffer = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return buffer;
}

void *alloc_audio_dma_buffer(size_t size)
{
    void *buffer = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA |
                                    MALLOC_CAP_8BIT);
    return buffer != nullptr ? buffer : alloc_audio_buffer(size);
}

bool buffer_is_internal(const void *buffer)
{
    return buffer != nullptr && esp_ptr_internal(buffer);
}

size_t audio_read(FILE *file, void *buffer, size_t size);
int audio_seek(FILE *file, long offset, int origin);
uint64_t read_le64(const uint8_t *bytes);
size_t audio_read(FILE *file, void *buffer, size_t size)
{
    return lyra::sd::read(file, buffer, size, lyra::sd::Client::Audio);
}

int audio_seek(FILE *file, long offset, int origin)
{
    return lyra::sd::seek(file, offset, origin, lyra::sd::Client::Audio);
}

bool has_sdcard_prefix(const char *path)
{
    return path != nullptr && std::strncmp(path, "/sdcard/", 8) == 0 && path[8] != '\0';
}

} // namespace lyra::audio::internal
