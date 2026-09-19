/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lyra_gui_settings.h"

#include "esp_log.h"
#include "nvs.h"

namespace lyra::gui_settings {
namespace {

constexpr const char *kTag = "lyra.gui.settings";
constexpr const char *kSettingsNamespace = "lyra";
constexpr const char *kGaplessKey = "gapless";
constexpr const char *kReplayGainKey = "replay_gain";
constexpr const char *kCrossfadeKey = "crossfade";
constexpr const char *kBrightnessKey = "brightness";
constexpr const char *kDarkModeKey = "dark_mode";
constexpr const char *kAccentColorKey = "accent_color";
constexpr const char *kLanguageKey = "language";
constexpr const char *kSpeakerOutputKey = "speaker_output";
constexpr const char *kEqualizerPresetKey = "eq_preset";
constexpr const char *kEqualizerBandKeys[kEqualizerBandCount] = {
    "eq_band_0", "eq_band_1", "eq_band_2", "eq_band_3", "eq_band_4",
};

bool valid_crossfade(uint8_t value)
{
    return value == 0 || value == 1 || value == 2 || value == 4 || value == 6;
}

} // namespace

void load(Values *values, size_t accent_palette_count, uint8_t equalizer_preset_count)
{
    if (!values) return;

    // Language is intentionally reset before opening NVS. This gives first
    // boot, an absent key, a corrupt key, and an unavailable NVS partition
    // the same safe English default without disturbing other caller defaults.
    values->language = lyra::i18n::Language::English;

    nvs_handle_t handle;
    if (nvs_open(kSettingsNamespace, NVS_READONLY, &handle) != ESP_OK) return;

    uint8_t value = 0;
    if (nvs_get_u8(handle, kGaplessKey, &value) == ESP_OK && value <= 1) {
        values->gapless = value != 0;
    }
    if (nvs_get_u8(handle, kReplayGainKey, &value) == ESP_OK && value <= 1) {
        values->replay_gain = value != 0;
    }
    if (nvs_get_u8(handle, kCrossfadeKey, &value) == ESP_OK && valid_crossfade(value)) {
        values->crossfade_seconds = value;
    }
    if (nvs_get_u8(handle, kBrightnessKey, &value) == ESP_OK && value >= 1 && value <= 100) {
        values->brightness_percent = value;
    }
    if (nvs_get_u8(handle, kDarkModeKey, &value) == ESP_OK && value <= 1) {
        values->dark_mode = value != 0;
    }
    if (nvs_get_u8(handle, kAccentColorKey, &value) == ESP_OK &&
        static_cast<size_t>(value) < accent_palette_count) {
        values->accent_colour = value;
    }
    if (nvs_get_u8(handle, kLanguageKey, &value) == ESP_OK &&
        lyra::i18n::is_valid_language(value)) {
        values->language = static_cast<lyra::i18n::Language>(value);
    }
    if (nvs_get_u8(handle, kSpeakerOutputKey, &value) == ESP_OK && value <= 1) {
        values->speaker_output_enabled = value != 0;
    }
    if (nvs_get_u8(handle, kEqualizerPresetKey, &value) == ESP_OK &&
        value < equalizer_preset_count) {
        values->equalizer_preset = value;
    }
    for (size_t band = 0; band < kEqualizerBandCount; ++band) {
        int8_t gain = 0;
        if (nvs_get_i8(handle, kEqualizerBandKeys[band], &gain) == ESP_OK &&
            gain >= lyra::audio::kEqualizerMinimumTenthsDb &&
            gain <= lyra::audio::kEqualizerMaximumTenthsDb) {
            values->equalizer_custom_bands[band] = gain;
        }
    }
    nvs_close(handle);
}

esp_err_t save(const Values &values)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open(kSettingsNamespace, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "settings persistence unavailable: %s", esp_err_to_name(result));
        return result;
    }

    result = nvs_set_u8(handle, kGaplessKey, values.gapless ? 1 : 0);
    if (result == ESP_OK) result = nvs_set_u8(handle, kReplayGainKey, values.replay_gain ? 1 : 0);
    if (result == ESP_OK) result = nvs_set_u8(handle, kCrossfadeKey, values.crossfade_seconds);
    if (result == ESP_OK) result = nvs_set_u8(handle, kBrightnessKey, values.brightness_percent);
    if (result == ESP_OK) result = nvs_set_u8(handle, kDarkModeKey, values.dark_mode ? 1 : 0);
    if (result == ESP_OK) result = nvs_set_u8(handle, kAccentColorKey, values.accent_colour);
    if (result == ESP_OK) {
        const uint8_t language = lyra::i18n::is_valid_language(
            static_cast<uint8_t>(values.language)) ? static_cast<uint8_t>(values.language) :
            static_cast<uint8_t>(lyra::i18n::Language::English);
        result = nvs_set_u8(handle, kLanguageKey, language);
    }
    if (result == ESP_OK) {
        result = nvs_set_u8(handle, kSpeakerOutputKey, values.speaker_output_enabled ? 1 : 0);
    }
    if (result == ESP_OK) result = nvs_set_u8(handle, kEqualizerPresetKey,
                                               values.equalizer_preset);
    for (size_t band = 0; result == ESP_OK && band < kEqualizerBandCount; ++band) {
        result = nvs_set_i8(handle, kEqualizerBandKeys[band],
                            static_cast<int8_t>(values.equalizer_custom_bands[band]));
    }
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "could not save settings: %s", esp_err_to_name(result));
    }
    return result;
}

} // namespace lyra::gui_settings
