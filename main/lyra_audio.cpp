/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lyra_audio.h"
#include "audio/lyra_audio_internal.h"

namespace lyra::audio {
using namespace internal;

uint32_t probe_duration_ms(const char *path)
{
    AudioFormat format{};
    if (!has_sdcard_prefix(path) || !get_audio_format(path, &format)) return 0;
    FILE *file = lyra::sd::open(path, "rb", lyra::sd::Client::Audio);
    if (!file) return 0;

    OggOpusReader ogg_opus_reader(file);
    if (format == AudioFormat::kOgg && ogg_opus_reader.is_opus_stream()) {
        format = AudioFormat::kOpus;
    }
    AiffStreamInfo aiff_info{};
    uint32_t duration_ms = 0;
    if (format == AudioFormat::kAiff && !read_aiff_stream_info(file, &aiff_info)) {
        lyra::sd::close(file, lyra::sd::Client::Audio);
        return 0;
    }
    switch (format) {
    case AudioFormat::kMp3: duration_ms = read_mp3_duration_ms(file); break;
    case AudioFormat::kFlac: duration_ms = read_flac_duration_ms(file); break;
    case AudioFormat::kAac: duration_ms = read_aac_duration_ms(file); break;
    case AudioFormat::kM4a: duration_ms = read_m4a_duration_ms(file); break;
    case AudioFormat::kWav: duration_ms = read_wav_duration_ms(file); break;
    case AudioFormat::kOgg: duration_ms = read_ogg_duration_ms(file); break;
    case AudioFormat::kOpus:
        duration_ms = read_ogg_tail_duration_ms(file, ogg_opus_reader.sample_rate,
                                                 ogg_opus_reader.pre_skip);
        break;
    case AudioFormat::kAiff: duration_ms = read_aiff_duration_ms(aiff_info); break;
    }
    lyra::sd::close(file, lyra::sd::Client::Audio);
    return duration_ms;
}

esp_err_t init()
{
    if (s_initialized) return ESP_OK;
    if (!lyra::sd::init()) return ESP_ERR_NO_MEM;

    const esp_err_t nvs_ret = ensure_nvs_ready();
    if (nvs_ret != ESP_OK) {
        ESP_LOGW(kTag, "volume persistence unavailable: %s", esp_err_to_name(nvs_ret));
    }

    s_state_mutex = xSemaphoreCreateMutex();
    if (s_state_mutex == nullptr) return ESP_ERR_NO_MEM;

    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(
        kAudioI2sPort, I2S_ROLE_MASTER);
    channel_config.dma_desc_num = kI2sDmaDescriptorCount;
    channel_config.dma_frame_num = kI2sDmaFrameCount;
    channel_config.auto_clear_after_cb = true;
    esp_err_t ret = i2s_new_channel(&channel_config, &s_i2s_tx, nullptr);
    if (ret != ESP_OK) {
        vSemaphoreDelete(s_state_mutex);
        s_state_mutex = nullptr;
        return ret;
    }

    i2s_std_config_t std_config{};
    std_config.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100);
    std_config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_128;
    std_config.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    std_config.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    std_config.gpio_cfg.bclk = static_cast<gpio_num_t>(kAudioBclkGpio);
    std_config.gpio_cfg.ws = static_cast<gpio_num_t>(kAudioLrclkGpio);
    std_config.gpio_cfg.dout = static_cast<gpio_num_t>(kAudioDataOutGpio);
    std_config.gpio_cfg.din = I2S_GPIO_UNUSED;
    ret = i2s_channel_init_std_mode(s_i2s_tx, &std_config);
    if (ret != ESP_OK) {
        i2s_del_channel(s_i2s_tx);
        s_i2s_tx = nullptr;
        vSemaphoreDelete(s_state_mutex);
        s_state_mutex = nullptr;
        return ret;
    }

    i2s_chan_config_t speaker_channel_config = I2S_CHANNEL_DEFAULT_CONFIG(
        kSpeakerI2sPort, I2S_ROLE_MASTER);
    speaker_channel_config.dma_desc_num = kI2sDmaDescriptorCount;
    speaker_channel_config.dma_frame_num = kI2sDmaFrameCount;
    speaker_channel_config.auto_clear_after_cb = true;
    ret = i2s_new_channel(&speaker_channel_config, &s_i2s_speaker_tx, nullptr);
    if (ret != ESP_OK) {
        ESP_LOGW(kTag, "on-board speaker output unavailable: %s", esp_err_to_name(ret));
        s_i2s_speaker_tx = nullptr;
    } else {
        i2s_std_config_t speaker_std_config{};
        speaker_std_config.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100);
        speaker_std_config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_128;
        speaker_std_config.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
        speaker_std_config.gpio_cfg.mclk = I2S_GPIO_UNUSED;
        speaker_std_config.gpio_cfg.bclk = static_cast<gpio_num_t>(kSpeakerBclkGpio);
        speaker_std_config.gpio_cfg.ws = static_cast<gpio_num_t>(kSpeakerLrclkGpio);
        speaker_std_config.gpio_cfg.dout = static_cast<gpio_num_t>(kSpeakerDataOutGpio);
        speaker_std_config.gpio_cfg.din = I2S_GPIO_UNUSED;
        ret = i2s_channel_init_std_mode(s_i2s_speaker_tx, &speaker_std_config);
        if (ret != ESP_OK) {
            ESP_LOGW(kTag, "could not initialize on-board speaker output: %s",
                     esp_err_to_name(ret));
            i2s_del_channel(s_i2s_speaker_tx);
            s_i2s_speaker_tx = nullptr;
        }
    }
    s_speaker_output_faulted = s_i2s_speaker_tx == nullptr;

    const esp_audio_err_t decoder_ret = esp_audio_dec_register_default();
    if (decoder_ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(kTag, "default audio decoder registration failed: %d",
                 static_cast<int>(decoder_ret));
        if (s_i2s_speaker_tx) i2s_del_channel(s_i2s_speaker_tx);
        s_i2s_speaker_tx = nullptr;
        i2s_del_channel(s_i2s_tx);
        s_i2s_tx = nullptr;
        vSemaphoreDelete(s_state_mutex);
        s_state_mutex = nullptr;
        return ESP_FAIL;
    }
    const esp_audio_err_t simple_decoder_ret = esp_audio_simple_dec_register_default();
    if (simple_decoder_ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(kTag, "audio simple decoder registration failed: %d",
                 static_cast<int>(simple_decoder_ret));
        esp_audio_dec_unregister_default();
        if (s_i2s_speaker_tx) i2s_del_channel(s_i2s_speaker_tx);
        s_i2s_speaker_tx = nullptr;
        i2s_del_channel(s_i2s_tx);
        s_i2s_tx = nullptr;
        vSemaphoreDelete(s_state_mutex);
        s_state_mutex = nullptr;
        return ESP_FAIL;
    }

    s_status = {};
    s_status.initialized = true;
    s_status.last_error = ESP_OK;
    load_saved_maximum_volume();
    s_status.volume_percent = load_saved_volume();
    s_requested_seek_ms = 0;
    s_requested_pause_after_seek = false;
    s_initialized = true;
    if (xTaskCreatePinnedToCore(audio_task, "lyra_audio", kAudioTaskStack, nullptr,
                                kAudioTaskPriority, &s_audio_task, 0) != pdPASS) {
        s_initialized = false;
        esp_audio_simple_dec_unregister_default();
        esp_audio_dec_unregister_default();
        if (s_i2s_speaker_tx) i2s_del_channel(s_i2s_speaker_tx);
        s_i2s_speaker_tx = nullptr;
        i2s_del_channel(s_i2s_tx);
        s_i2s_tx = nullptr;
        vSemaphoreDelete(s_state_mutex);
        s_state_mutex = nullptr;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(kTag, "audio playback ready (MP3/FLAC/AAC/M4A/WAV/OGG/Opus/AIFF): "
             "DAC BCLK=%d WS=%d DOUT=%d; speaker=%s",
             kAudioBclkGpio, kAudioLrclkGpio, kAudioDataOutGpio,
             s_i2s_speaker_tx ? "available" : "unavailable");
    return ESP_OK;
}

esp_err_t play(const char *path)
{
    return play_from_position(path, 0, false);
}

esp_err_t play_from_position(const char *path, uint32_t position_ms, bool paused)
{
    if (!s_initialized || s_state_mutex == nullptr) return ESP_ERR_INVALID_STATE;
    if (!has_sdcard_prefix(path)) return ESP_ERR_INVALID_ARG;
    AudioFormat format{};
    if (!get_audio_format(path, &format)) return ESP_ERR_NOT_SUPPORTED;
    if (std::strlen(path) >= kMaxPath) return ESP_ERR_INVALID_SIZE;

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    std::strncpy(s_requested_path, path, sizeof(s_requested_path) - 1);
    s_requested_path[sizeof(s_requested_path) - 1] = '\0';
    s_requested_seek_ms = position_ms;
    s_requested_pause_after_seek = false;
    ++s_request_generation;
    s_status.last_error = ESP_OK;
    s_status.playing = true;
    s_status.eof = false;
    s_status.paused = paused;
    s_status.sample_rate = 0;
    s_status.channels = 0;
    s_status.bits_per_sample = 0;
    s_status.decoded_bytes = 0;
    s_status.position_ms = position_ms;
    s_status.duration_ms = 0;
    std::strncpy(s_status.path, path, sizeof(s_status.path) - 1);
    s_status.path[sizeof(s_status.path) - 1] = '\0';
    xSemaphoreGive(s_state_mutex);
    xTaskNotifyGive(s_audio_task);
    return ESP_OK;
}

esp_err_t stop()
{
    if (!s_initialized || s_state_mutex == nullptr) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_requested_path[0] = '\0';
    s_requested_seek_ms = 0;
    s_requested_pause_after_seek = false;
    ++s_request_generation;
    s_status.paused = false;
    s_status.path[0] = '\0';
    s_status.position_ms = 0;
    s_status.duration_ms = 0;
    xSemaphoreGive(s_state_mutex);
    xTaskNotifyGive(s_audio_task);
    return ESP_OK;
}

esp_err_t seek(uint32_t position_ms)
{
    if (!s_initialized || s_state_mutex == nullptr) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (!s_status.path[0] || s_status.last_error != ESP_OK || s_status.duration_ms == 0) {
        xSemaphoreGive(s_state_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t duration_ms = s_status.duration_ms;
    const uint32_t target_ms = position_ms >= duration_ms ?
        (duration_ms > 250 ? duration_ms - 250 : 0) : position_ms;
    const bool paused = s_status.paused;
    s_requested_seek_ms = target_ms;
    s_requested_pause_after_seek = paused;
    ++s_request_generation;
    s_status.playing = true;
    s_status.eof = false;
    s_status.position_ms = target_ms;
    s_status.decoded_bytes = 0;
    // Let the audio task run while it replays/discards a FLAC stream. The
    // requested paused state is restored by play_file after it reaches the
    // target position.
    s_status.paused = false;
    xSemaphoreGive(s_state_mutex);
    xTaskNotifyGive(s_audio_task);
    return ESP_OK;
}

esp_err_t toggle_pause()
{
    if (!s_initialized || s_state_mutex == nullptr) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (!s_status.playing || !s_status.path[0] || s_status.eof || s_status.last_error != ESP_OK) {
        xSemaphoreGive(s_state_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    s_status.paused = !s_status.paused;
    xSemaphoreGive(s_state_mutex);
    xTaskNotifyGive(s_audio_task);
    return ESP_OK;
}

esp_err_t set_volume(uint8_t volume_percent)
{
    if (!s_initialized || s_state_mutex == nullptr) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (volume_percent > s_maximum_volume_percent) {
        volume_percent = s_maximum_volume_percent;
    }
    s_status.volume_percent = volume_percent;
    xSemaphoreGive(s_state_mutex);
    xTaskNotifyGive(s_audio_task);
    return ESP_OK;
}

uint8_t maximum_volume_percent()
{
    if (s_state_mutex == nullptr) return kDefaultMaximumVolumePercent;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    const uint8_t maximum = s_maximum_volume_percent;
    xSemaphoreGive(s_state_mutex);
    return maximum;
}

esp_err_t set_maximum_volume_percent(uint8_t volume_percent)
{
    if (!s_initialized || s_state_mutex == nullptr) return ESP_ERR_INVALID_STATE;
    if (volume_percent == 0 || volume_percent > kMaximumVolumePercent) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_maximum_volume_percent = volume_percent;
    if (s_status.volume_percent > s_maximum_volume_percent) {
        s_status.volume_percent = s_maximum_volume_percent;
    }
    xSemaphoreGive(s_state_mutex);
    xTaskNotifyGive(s_audio_task);
    return ESP_OK;
}

esp_err_t set_speaker_output_enabled(bool enabled)
{
    if (!s_initialized || s_state_mutex == nullptr) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_speaker_output_enabled = enabled;
    xSemaphoreGive(s_state_mutex);
    return ESP_OK;
}

esp_err_t set_equalizer(const EqualizerSettings &settings)
{
    if (!s_initialized || s_state_mutex == nullptr) return ESP_ERR_INVALID_STATE;

    EqualizerSettings clamped = settings;
    for (size_t band = 0; band < kEqualizerBandCount; ++band) {
        clamped.band_tenths_db[band] = std::clamp(
            clamped.band_tenths_db[band], kEqualizerMinimumTenthsDb,
            kEqualizerMaximumTenthsDb);
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    const bool changed = std::memcmp(&s_equalizer, &clamped, sizeof(clamped)) != 0;
    s_equalizer = clamped;
    if (changed) ++s_equalizer_generation;
    xSemaphoreGive(s_state_mutex);
    return ESP_OK;
}

esp_err_t set_replay_gain_adjustment(int16_t tenths_db)
{
    if (!s_initialized || s_state_mutex == nullptr) return ESP_ERR_INVALID_STATE;
    constexpr int16_t kMinimumReplayGainTenthsDb = -120;
    constexpr int16_t kMaximumReplayGainTenthsDb = 120;
    tenths_db = std::clamp(tenths_db, kMinimumReplayGainTenthsDb,
                           kMaximumReplayGainTenthsDb);
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_replay_gain_tenths_db = tenths_db;
    xSemaphoreGive(s_state_mutex);
    return ESP_OK;
}

esp_err_t set_transition_gain(uint8_t percent)
{
    if (!s_initialized || s_state_mutex == nullptr) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_transition_gain_percent = std::min<uint8_t>(percent, 100);
    xSemaphoreGive(s_state_mutex);
    return ESP_OK;
}

esp_err_t save_volume()
{
    if (!s_initialized || s_state_mutex == nullptr) return ESP_ERR_INVALID_STATE;
    uint8_t volume;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    volume = s_status.volume_percent;
    xSemaphoreGive(s_state_mutex);
    return save_volume_to_nvs(volume);
}

esp_err_t save_maximum_volume()
{
    if (!s_initialized || s_state_mutex == nullptr) return ESP_ERR_INVALID_STATE;
    if (!s_nvs_ready) return ESP_ERR_INVALID_STATE;
    uint8_t maximum;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    maximum = s_maximum_volume_percent;
    xSemaphoreGive(s_state_mutex);

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(kSettingsNamespace, NVS_READWRITE, &handle);
    if (ret != ESP_OK) return ret;
    ret = nvs_set_u8(handle, kMaximumVolumeKey, maximum);
    if (ret == ESP_OK) ret = nvs_commit(handle);
    nvs_close(handle);
    return ret;
}

Status status()
{
    Status copy{};
    if (s_state_mutex == nullptr) return copy;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    copy = s_status;
    xSemaphoreGive(s_state_mutex);
    return copy;
}

Diagnostics diagnostics()
{
    Diagnostics copy{};
    if (s_state_mutex == nullptr) return copy;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    copy = s_diagnostics;
    xSemaphoreGive(s_state_mutex);
    if (copy.average_sd_read_us == 0 && copy.sd_read_count != 0) {
        copy.average_sd_read_us = static_cast<uint32_t>(
            copy.total_sd_read_us / copy.sd_read_count);
    }
    return copy;
}

} // namespace lyra::audio
