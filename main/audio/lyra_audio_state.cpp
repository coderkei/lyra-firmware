/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lyra_audio_internal.h"

namespace lyra::audio::internal {

SemaphoreHandle_t s_state_mutex;
TaskHandle_t s_audio_task;
i2s_chan_handle_t s_i2s_tx;
i2s_chan_handle_t s_i2s_speaker_tx;
bool s_i2s_started;
bool s_i2s_speaker_started;
bool s_speaker_output_faulted;
bool s_initialized;
bool s_nvs_ready;
bool s_speaker_output_enabled = lyra::audio::kDefaultSpeakerOutputEnabled;
uint8_t s_maximum_volume_percent = lyra::audio::kDefaultMaximumVolumePercent;
int16_t s_replay_gain_tenths_db;
uint8_t s_transition_gain_percent = 100;
lyra::audio::EqualizerSettings s_equalizer{};
uint32_t s_equalizer_generation = 1;

char s_requested_path[lyra::audio::kMaxPath];
std::atomic<uint32_t> s_request_generation{0};
int64_t s_timer_alarm_started_us;
std::atomic<bool> s_timer_alarm_stream_handled{false};
uint32_t s_requested_seek_ms;
bool s_requested_pause_after_seek;
bool s_audio_session_active;
uint8_t s_duration_scan_buffer[kDurationScanBufferBytes];

lyra::audio::Status s_status{};
lyra::audio::Diagnostics s_diagnostics{};
NextTrackRequest s_next_track{};
char s_stream_title[lyra::audio::kMaxStreamTitle];

void set_stream_title(const char *title)
{
    if (s_state_mutex == nullptr) return;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    size_t length = title ? std::strlen(title) : 0;
    if (length >= sizeof(s_stream_title)) {
        length = sizeof(s_stream_title) - 1;
        // Do not leave a truncated UTF-8 sequence at the end of the title.
        while (length > 0 && (static_cast<uint8_t>(title[length]) & 0xC0) == 0x80) {
            --length;
        }
    }
    if (length > 0) std::memcpy(s_stream_title, title, length);
    s_stream_title[length] = '\0';
    xSemaphoreGive(s_state_mutex);
}

} // namespace lyra::audio::internal
