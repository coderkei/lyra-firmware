/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lyra_audio.h"

#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "decoder/impl/esp_opus_dec.h"
#include "lyra_board_pins.h"
#include "lyra_sd.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

using namespace lyra::board::jc3248w535en;

constexpr const char *kTag = "lyra.audio";
// Match the decoder's worst-case refill latency with a substantial encoded
// reserve.  This matters most for high-bitrate FLAC, where a short SD stall
// represents much less playback time than it does for a 128 kbps MP3.
constexpr size_t kReadAheadBufferBytes = 256 * 1024;
constexpr size_t kReadAheadChunkBytes = 32 * 1024;
constexpr size_t kM4aReadAheadChunkBytes = kReadAheadBufferBytes - 1;
// FLAC frames can be larger than the normal refill size. Keeping one whole
// encoded frame in a contiguous read-ahead segment avoids parser retries at
// frame boundaries (the decoder reports the encoded max-frame size).
constexpr size_t kFlacReadAheadChunkBytes = 64 * 1024;
constexpr size_t kInitialPcmBufferBytes = 8192;
constexpr size_t kMaximumPcmBufferBytes = 128 * 1024;
constexpr size_t kStereoBufferBytes = kMaximumPcmBufferBytes * 2;
constexpr size_t kPcmConvertChunkBytes = 8192;
constexpr size_t kSimpleDecoderInputChunkBytes = 4096;
constexpr size_t kMaximumOggOpusPacketBytes = 64 * 1024;
constexpr size_t kMaximumOggPageBytes = 65307;
constexpr size_t kOggSeekWindowBytes = 256 * 1024;
constexpr size_t kMaximumM4aStscEntries = 32;
constexpr size_t kMaximumM4aSttsEntries = 32;
// Keep decoded PCM ahead of the I2S deadline. This is separate from the
// encoded read-ahead ring because a complex FLAC frame can briefly occupy the
// decoder while I2S must continue draining at a fixed sample rate.
constexpr size_t kPcmOutputBufferBytes = 256 * 1024;
constexpr size_t kPcmOutputChunkBytes = 16 * 1024;
constexpr size_t kI2sDmaDescriptorCount = 16;
constexpr size_t kI2sDmaFrameCount = 512;
constexpr size_t kI2sFrameBytes = sizeof(int16_t) * 2;
constexpr size_t kI2sDmaBufferBytes = kI2sDmaDescriptorCount * kI2sDmaFrameCount *
                                      kI2sFrameBytes;
constexpr size_t kPcmOutputStartBytes = kI2sDmaBufferBytes;
constexpr size_t kDurationScanBufferBytes = 2048;
constexpr uint32_t kAudioTaskStack = 24 * 1024;
constexpr uint32_t kAudioOutputTaskStack = 8 * 1024;
constexpr UBaseType_t kAudioTaskPriority = 8;
constexpr UBaseType_t kAudioOutputTaskPriority = 9;
// Keep the user-facing volume slider useful while limiting the PCM amplitude
// sent to the speaker amplifier. 25% amplitude is -12 dB and approximately
// 1/16 of the full-scale amplifier power, before the board's analog gain.
constexpr int32_t kMaximumOutputGainQ15 = 8192;
constexpr int32_t kGainRampStepQ15 = 4;
constexpr uint16_t kEqualizerCoefficientRampFrames = 128;
constexpr float kEqualizerQ = 1.0f;
constexpr float kEqualizerPi = 3.14159265358979323846f;
constexpr float kEqualizerCenterFrequenciesHz[lyra::audio::kEqualizerBandCount] = {
    60.0f, 250.0f, 1000.0f, 4000.0f, 16000.0f,
};
constexpr const char *kSettingsNamespace = "lyra";
constexpr const char *kVolumeKey = "volume";

SemaphoreHandle_t s_state_mutex;
TaskHandle_t s_audio_task;
i2s_chan_handle_t s_i2s_tx;
bool s_i2s_started;
bool s_initialized;
bool s_nvs_ready;
int16_t s_replay_gain_tenths_db;
uint8_t s_transition_gain_percent = 100;
lyra::audio::EqualizerSettings s_equalizer{};
uint32_t s_equalizer_generation = 1;

char s_requested_path[lyra::audio::kMaxPath];
uint32_t s_request_generation;
uint32_t s_requested_seek_ms;
bool s_requested_pause_after_seek;
uint8_t s_duration_scan_buffer[kDurationScanBufferBytes];

lyra::audio::Status s_status{};
lyra::audio::Diagnostics s_diagnostics{};

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

void apply_output_processing(PcmOutput *output, uint8_t *pcm, size_t pcm_bytes);

bool fail_pcm_output(PcmOutput *output, esp_err_t error)
{
    if (!output) return false;
    output->error = true;
    output->error_code = error;
    output->stop_requested = true;
    output->drain_on_stop = false;
    return false;
}

bool preload_and_start_i2s(PcmOutput *output)
{
    if (!output || !output->stream || !s_i2s_tx) return false;

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
        apply_output_processing(output, output->staging_buffer, received);

        size_t source_offset = 0;
        while (source_offset < received) {
            size_t loaded = 0;
            const esp_err_t result = i2s_channel_preload_data(
                s_i2s_tx, output->staging_buffer + source_offset,
                received - source_offset, &loaded);
            if (result != ESP_OK || loaded == 0 || loaded % kI2sFrameBytes != 0) {
                return fail_pcm_output(output, result == ESP_OK ?
                                       ESP_ERR_INVALID_SIZE : result);
            }
            source_offset += loaded;
            preloaded += loaded;
        }
    }

    // A short final buffer is only possible while stopping a very short track;
    // the remaining DMA descriptors are intentionally cleared by the driver.
    if (preloaded == 0) return false;
    const esp_err_t enable_ret = i2s_channel_enable(s_i2s_tx);
    if (enable_ret != ESP_OK) return fail_pcm_output(output, enable_ret);
    s_i2s_started = true;
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
        volume > lyra::audio::kMaximumVolumePercent) {
        volume = lyra::audio::kDefaultVolumePercent;
    }
    nvs_close(handle);
    return volume;
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

enum class OggPacketReadResult : uint8_t {
    kPacket,
    kEnd,
    kError,
};

struct OggOpusReader {
    FILE *file = nullptr;
    uint8_t lacing[255]{};
    size_t segment_index = 0;
    size_t segment_count = 0;
    bool page_loaded = false;
    bool initialized = false;
    bool failed = false;
    uint32_t sample_rate = 48000;
    uint8_t channels = 0;
    uint16_t pre_skip = 0;
    long audio_start_offset = 0;

    explicit OggOpusReader(FILE *source) : file(source) {}

    bool read_exact(void *buffer, size_t size)
    {
        auto *destination = static_cast<uint8_t *>(buffer);
        size_t received = 0;
        while (received < size) {
            const size_t count = audio_read(file, destination + received, size - received);
            if (count == 0) return false;
            received += count;
        }
        return true;
    }

    bool read_page()
    {
        uint8_t header[27]{};
        if (!read_exact(header, sizeof(header))) return false;
        if (std::memcmp(header, "OggS", 4) != 0 || header[4] != 0) return false;
        segment_count = header[26];
        if (!read_exact(lacing, segment_count)) return false;
        segment_index = 0;
        page_loaded = true;
        return true;
    }

    OggPacketReadResult read_packet(uint8_t *output, size_t capacity, size_t *length)
    {
        if (!file || !length) return OggPacketReadResult::kError;
        *length = 0;
        const bool capture = output != nullptr;
        bool overflow = false;
        const size_t output_capacity = std::min(capacity, kMaximumOggOpusPacketBytes);
        uint8_t scratch[256]{};

        while (true) {
            if (!page_loaded || segment_index >= segment_count) {
                page_loaded = false;
                if (!read_page()) return std::feof(file) ?
                    OggPacketReadResult::kEnd : OggPacketReadResult::kError;
            }

            const size_t segment_length = lacing[segment_index++];
            size_t remaining = segment_length;
            while (remaining > 0) {
                const size_t wanted = std::min(remaining, sizeof(scratch));
                uint8_t *destination = scratch;
                if (!overflow && output && *length + wanted <= output_capacity) {
                    destination = output + *length;
                }
                const size_t count = audio_read(file, destination, wanted);
                if (count != wanted) return OggPacketReadResult::kError;
                if (destination != scratch) *length += count;
                else if (capture) overflow = true;
                remaining -= count;
            }

            if (segment_length != 255) {
                return overflow ? OggPacketReadResult::kError :
                    OggPacketReadResult::kPacket;
            }
        }
    }

    bool initialize()
    {
        if (initialized) return true;
        if (failed || !file || audio_seek(file, 0, SEEK_SET) != 0) return false;

        uint8_t identification[64]{};
        size_t length = 0;
        if (read_packet(identification, sizeof(identification), &length) !=
                OggPacketReadResult::kPacket || length < 19 ||
            std::memcmp(identification, "OpusHead", 8) != 0) {
            failed = true;
            return false;
        }
        channels = identification[9];
        pre_skip = static_cast<uint16_t>(identification[10] |
                                         (static_cast<uint16_t>(identification[11]) << 8));
        if (channels == 0) {
            failed = true;
            return false;
        }

        // OpusTags can be much larger than an audio packet. It is metadata,
        // so consume it without copying it into the realtime packet buffer.
        if (read_packet(nullptr, 0, &length) != OggPacketReadResult::kPacket) {
            failed = true;
            return false;
        }
        const long current_position = std::ftell(file);
        if (current_position >= 0) audio_start_offset = current_position;
        initialized = true;
        return true;
    }

    bool is_opus_stream()
    {
        const bool result = initialize();
        const uint8_t detected_channels = channels;
        const uint16_t detected_pre_skip = pre_skip;
        reset();
        if (result) {
            channels = detected_channels;
            pre_skip = detected_pre_skip;
        }
        return result;
    }

    void reset()
    {
        segment_index = 0;
        segment_count = 0;
        page_loaded = false;
        initialized = false;
        failed = false;
        channels = 0;
        pre_skip = 0;
        if (file) audio_seek(file, 0, SEEK_SET);
    }

    bool read_page_metadata(long offset, long file_end, int64_t *granule,
                            bool *continued, long *page_end)
    {
        if (!file || !granule || !continued || !page_end || offset < 0 ||
            offset + 27 > file_end || audio_seek(file, offset, SEEK_SET) != 0) return false;

        uint8_t header[27]{};
        uint8_t page_lacing[255]{};
        if (!read_exact(header, sizeof(header)) || std::memcmp(header, "OggS", 4) != 0 ||
            header[4] != 0) return false;
        const size_t page_segments = header[26];
        if (!read_exact(page_lacing, page_segments)) return false;
        size_t payload_bytes = 0;
        for (size_t index = 0; index < page_segments; ++index) {
            payload_bytes += page_lacing[index];
        }
        const uint64_t page_bytes = 27u + page_segments + payload_bytes;
        if (page_bytes > static_cast<uint64_t>(file_end - offset)) return false;

        const uint64_t raw_granule = read_le64(header + 6);
        *granule = raw_granule == ~static_cast<uint64_t>(0) ? -1 :
            static_cast<int64_t>(raw_granule);
        *continued = (header[5] & 0x01u) != 0;
        *page_end = offset + static_cast<long>(page_bytes);
        return true;
    }

    bool find_page_at_or_after(long start, long limit, long file_end, long *page_offset)
    {
        if (!file || !page_offset || start < 0 || limit < start) return false;
        const size_t scan_bytes = static_cast<size_t>(std::min<long>({
            static_cast<long>(kMaximumOggPageBytes + 3), file_end - start,
            limit - start + 4}));
        auto *scan_buffer = static_cast<uint8_t *>(heap_caps_malloc(
            scan_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!scan_buffer || scan_bytes < 4 || audio_seek(file, start, SEEK_SET) != 0 ||
            audio_read(file, scan_buffer, scan_bytes) != scan_bytes) {
            heap_caps_free(scan_buffer);
            return false;
        }
        for (size_t index = 0; index + 4 <= scan_bytes; ++index) {
            const long candidate = start + static_cast<long>(index);
            if (candidate > limit) break;
            if (std::memcmp(scan_buffer + index, "OggS", 4) != 0) continue;
            int64_t granule = -1;
            bool continued = false;
            long end = 0;
            if (read_page_metadata(candidate, file_end, &granule, &continued, &end)) {
                *page_offset = candidate;
                heap_caps_free(scan_buffer);
                return true;
            }
        }
        heap_caps_free(scan_buffer);
        return false;
    }

    bool activate_page(long page_offset, uint64_t base, uint64_t *base_samples)
    {
        if (!base_samples || audio_seek(file, page_offset, SEEK_SET) != 0) return false;
        segment_index = 0;
        segment_count = 0;
        page_loaded = false;
        initialized = true;
        failed = false;
        *base_samples = base;
        return true;
    }

    bool seek_to_position_linear(uint64_t target_samples, uint64_t *base_samples)
    {
        if (!file || !base_samples || audio_seek(file, 0, SEEK_SET) != 0) return false;

        long safe_page_offset = -1;
        uint64_t safe_page_base = 0;
        int64_t previous_granule = -1;

        while (true) {
            const long page_offset = std::ftell(file);
            uint8_t header[27]{};
            uint8_t page_lacing[255]{};
            if (page_offset < 0 || !read_exact(header, sizeof(header)) ||
                std::memcmp(header, "OggS", 4) != 0 || header[4] != 0) {
                break;
            }
            const size_t page_segments = header[26];
            if (!read_exact(page_lacing, page_segments)) break;
            size_t payload_bytes = 0;
            for (size_t index = 0; index < page_segments; ++index) {
                payload_bytes += page_lacing[index];
            }
            if (audio_seek(file, static_cast<long>(payload_bytes), SEEK_CUR) != 0) break;

            const int64_t granule = static_cast<int64_t>(read_le64(header + 6));
            const bool continued = (header[5] & 0x01u) != 0;
            if (granule >= 0 && static_cast<uint64_t>(granule) >= target_samples) {
                const long selected_page = continued ? safe_page_offset : page_offset;
                if (selected_page >= 0 && activate_page(selected_page,
                        continued ? safe_page_base : previous_granule >= 0 ?
                        static_cast<uint64_t>(previous_granule) : 0, base_samples)) return true;
                break;
            }

            if (!continued && granule >= 0) {
                safe_page_offset = page_offset;
                safe_page_base = previous_granule >= 0 ?
                    static_cast<uint64_t>(previous_granule) : 0;
            }
            previous_granule = granule;
        }

        reset();
        return false;
    }

    bool seek_to_position(uint32_t position_ms, uint32_t duration_ms,
                          uint64_t *base_samples)
    {
        if (!file || !base_samples || sample_rate == 0) return false;

        const uint64_t target_samples =
            (static_cast<uint64_t>(position_ms) * sample_rate) / 1000u + pre_skip;
        long file_end = -1;
        if (duration_ms > 0 && audio_seek(file, 0, SEEK_END) == 0) file_end = std::ftell(file);
        const uint64_t duration_samples = duration_ms > 0 ?
            (static_cast<uint64_t>(duration_ms) * sample_rate + 500u) / 1000u + pre_skip : 0;
        if (file_end > audio_start_offset && duration_samples > 0) {
            const uint64_t encoded_bytes = static_cast<uint64_t>(file_end - audio_start_offset);
            const uint64_t estimated_delta = encoded_bytes * target_samples / duration_samples;
            const long estimated = audio_start_offset + static_cast<long>(
                std::min<uint64_t>(estimated_delta, encoded_bytes));
            const long search_start = std::max<long>(audio_start_offset,
                estimated - static_cast<long>(kOggSeekWindowBytes));
            const long search_limit = std::min<long>(file_end - 27,
                estimated + static_cast<long>(kOggSeekWindowBytes));
            long page_offset = -1;
            if (find_page_at_or_after(search_start, search_limit, file_end, &page_offset)) {
                int64_t previous_granule = -1;
                long safe_page_offset = -1;
                uint64_t safe_page_base = 0;
                while (page_offset >= 0 && page_offset <= search_limit) {
                    int64_t granule = -1;
                    bool continued = false;
                    long next_page = 0;
                    if (!read_page_metadata(page_offset, file_end, &granule, &continued,
                                            &next_page)) break;
                    if (granule >= 0 && static_cast<uint64_t>(granule) >= target_samples) {
                        const long selected_page = continued ? safe_page_offset : page_offset;
                        if (selected_page >= 0 && activate_page(selected_page,
                                continued ? safe_page_base : previous_granule >= 0 ?
                                static_cast<uint64_t>(previous_granule) : 0, base_samples)) {
                            return true;
                        }
                        break;
                    }
                    if (!continued && granule >= 0) {
                        safe_page_offset = page_offset;
                        safe_page_base = previous_granule >= 0 ?
                            static_cast<uint64_t>(previous_granule) : 0;
                    }
                    previous_granule = granule;
                    if (next_page <= page_offset) break;
                    page_offset = next_page;
                }
            }
        }

        // Preserve correctness if the file has unusual page/bitrate layout;
        // this path is now only a fallback when the bounded estimate fails.
        return seek_to_position_linear(target_samples, base_samples);
    }

    OggPacketReadResult read_audio_packet(uint8_t *output, size_t capacity,
                                          size_t *length)
    {
        if (!initialize()) return failed ? OggPacketReadResult::kError :
            OggPacketReadResult::kEnd;
        return read_packet(output, capacity, length);
    }
};

struct M4aStscEntry {
    uint32_t first_chunk;
    uint32_t samples_per_chunk;
};

struct M4aSttsEntry {
    uint32_t sample_count;
    uint32_t sample_delta;
};

struct M4aAacReader {
    FILE *file = nullptr;
    bool initialized = false;
    bool failed = false;
    uint32_t sample_rate = 0;
    uint8_t channels = 0;
    uint32_t sample_count = 0;
    uint32_t default_sample_size = 0;
    uint64_t sample_sizes_offset = 0;
    uint64_t chunk_offsets_offset = 0;
    bool chunk_offsets_64 = false;
    uint32_t chunk_count = 0;
    uint32_t stsc_count = 0;
    M4aStscEntry stsc[kMaximumM4aStscEntries]{};
    uint32_t stts_count = 0;
    M4aSttsEntry stts[kMaximumM4aSttsEntries]{};

    uint32_t current_sample = 0;
    uint64_t current_offset = 0;
    uint32_t current_size = 0;
    uint32_t current_chunk_sample = 0;
    uint32_t current_chunk_samples = 0;
    bool current_valid = false;

    explicit M4aAacReader(FILE *source) : file(source) {}

    static uint16_t be16(const uint8_t *bytes)
    {
        return static_cast<uint16_t>((static_cast<uint16_t>(bytes[0]) << 8) | bytes[1]);
    }

    static uint32_t be32(const uint8_t *bytes)
    {
        return (static_cast<uint32_t>(bytes[0]) << 24) |
               (static_cast<uint32_t>(bytes[1]) << 16) |
               (static_cast<uint32_t>(bytes[2]) << 8) |
               static_cast<uint32_t>(bytes[3]);
    }

    static uint64_t be64(const uint8_t *bytes)
    {
        return (static_cast<uint64_t>(be32(bytes)) << 32) | be32(bytes + 4);
    }

    bool read_at(uint64_t offset, void *buffer, size_t size)
    {
        return file && offset <= static_cast<uint64_t>(LONG_MAX) &&
            audio_seek(file, static_cast<long>(offset), SEEK_SET) == 0 &&
            audio_read(file, buffer, size) == size;
    }

    bool read_atom(uint64_t position, uint64_t limit, uint64_t *data_start,
                   uint64_t *end, uint8_t type[4])
    {
        if (!data_start || !end || !type || position + 8 > limit) return false;
        uint8_t header[16]{};
        if (!read_at(position, header, 8)) return false;
        uint64_t size = be32(header);
        uint64_t header_size = 8;
        if (size == 1) {
            if (position + 16 > limit || !read_at(position + 8, header + 8, 8)) return false;
            size = be64(header + 8);
            header_size = 16;
        } else if (size == 0) {
            size = limit - position;
        }
        if (size < header_size || size > limit - position) return false;
        std::memcpy(type, header + 4, 4);
        *data_start = position + header_size;
        *end = position + size;
        return true;
    }

    static bool type_is(const uint8_t type[4], const char (&expected)[5])
    {
        return std::memcmp(type, expected, 4) == 0;
    }

    static bool is_container(const uint8_t type[4])
    {
        return type_is(type, "moov") || type_is(type, "trak") || type_is(type, "mdia") ||
               type_is(type, "minf") || type_is(type, "stbl") || type_is(type, "edts") ||
               type_is(type, "dinf") || type_is(type, "udta") || type_is(type, "meta");
    }

    void clear_tables()
    {
        sample_rate = 0;
        channels = 0;
        sample_count = 0;
        default_sample_size = 0;
        sample_sizes_offset = 0;
        chunk_offsets_offset = 0;
        chunk_offsets_64 = false;
        chunk_count = 0;
        stsc_count = 0;
        stts_count = 0;
        current_valid = false;
    }

    bool parse_stsd(uint64_t data_start, uint64_t end)
    {
        uint8_t header[8]{};
        if (end - data_start < sizeof(header) || !read_at(data_start, header, sizeof(header))) {
            return false;
        }
        const uint32_t entry_count = be32(header + 4);
        uint64_t position = data_start + 8;
        for (uint32_t entry = 0; entry < entry_count && position + 8 <= end; ++entry) {
            uint8_t entry_header[8]{};
            if (!read_at(position, entry_header, sizeof(entry_header))) return false;
            const uint32_t entry_size = be32(entry_header);
            if (entry_size < 36 || position + entry_size > end) return false;
            if (std::memcmp(entry_header + 4, "mp4a", 4) == 0) {
                uint8_t audio_entry[28]{};
                if (!read_at(position + 8, audio_entry, sizeof(audio_entry))) return false;
                channels = static_cast<uint8_t>(be16(audio_entry + 16));
                sample_rate = be32(audio_entry + 24) >> 16;
                return sample_rate != 0 && channels != 0;
            }
            position += entry_size;
        }
        return false;
    }

    bool parse_stts(uint64_t data_start, uint64_t end)
    {
        uint8_t header[8]{};
        if (end - data_start < sizeof(header) || !read_at(data_start, header, sizeof(header))) {
            return false;
        }
        const uint32_t count = be32(header + 4);
        if (count == 0 || count > kMaximumM4aSttsEntries ||
            data_start + 8 + static_cast<uint64_t>(count) * 8 > end) return false;
        for (uint32_t index = 0; index < count; ++index) {
            uint8_t entry[8]{};
            if (!read_at(data_start + 8 + static_cast<uint64_t>(index) * 8,
                         entry, sizeof(entry))) return false;
            stts[index] = {be32(entry), be32(entry + 4)};
            if (stts[index].sample_count == 0 || stts[index].sample_delta == 0) return false;
        }
        stts_count = count;
        return true;
    }

    bool parse_stsc(uint64_t data_start, uint64_t end)
    {
        uint8_t header[8]{};
        if (end - data_start < sizeof(header) || !read_at(data_start, header, sizeof(header))) {
            return false;
        }
        const uint32_t count = be32(header + 4);
        if (count == 0 || count > kMaximumM4aStscEntries ||
            data_start + 8 + static_cast<uint64_t>(count) * 12 > end) return false;
        for (uint32_t index = 0; index < count; ++index) {
            uint8_t entry[12]{};
            if (!read_at(data_start + 8 + static_cast<uint64_t>(index) * 12,
                         entry, sizeof(entry))) return false;
            stsc[index] = {be32(entry), be32(entry + 4)};
            if (stsc[index].first_chunk == 0 || stsc[index].samples_per_chunk == 0 ||
                (index > 0 && stsc[index].first_chunk <= stsc[index - 1].first_chunk)) {
                return false;
            }
        }
        stsc_count = count;
        return true;
    }

    bool parse_stsz(uint64_t data_start, uint64_t end)
    {
        uint8_t header[12]{};
        if (end - data_start < sizeof(header) || !read_at(data_start, header, sizeof(header))) {
            return false;
        }
        default_sample_size = be32(header + 4);
        sample_count = be32(header + 8);
        if (sample_count == 0) return false;
        sample_sizes_offset = data_start + 12;
        return default_sample_size != 0 ||
            sample_sizes_offset + static_cast<uint64_t>(sample_count) * 4 <= end;
    }

    bool parse_chunk_offsets(uint64_t data_start, uint64_t end, bool offsets_64)
    {
        uint8_t header[8]{};
        if (end - data_start < sizeof(header) || !read_at(data_start, header, sizeof(header))) {
            return false;
        }
        chunk_count = be32(header + 4);
        const uint64_t entry_bytes = offsets_64 ? 8 : 4;
        if (chunk_count == 0 || data_start + 8 + static_cast<uint64_t>(chunk_count) * entry_bytes > end) {
            return false;
        }
        chunk_offsets_offset = data_start + 8;
        chunk_offsets_64 = offsets_64;
        return true;
    }

    bool parse_stbl(uint64_t start, uint64_t end)
    {
        clear_tables();
        bool have_audio = false;
        uint64_t position = start;
        while (position + 8 <= end) {
            uint64_t data_start = 0;
            uint64_t atom_end = 0;
            uint8_t type[4]{};
            if (!read_atom(position, end, &data_start, &atom_end, type)) return false;
            if (type_is(type, "stsd")) have_audio = parse_stsd(data_start, atom_end);
            else if (type_is(type, "stts")) parse_stts(data_start, atom_end);
            else if (type_is(type, "stsc")) parse_stsc(data_start, atom_end);
            else if (type_is(type, "stsz")) parse_stsz(data_start, atom_end);
            else if (type_is(type, "stco")) parse_chunk_offsets(data_start, atom_end, false);
            else if (type_is(type, "co64")) parse_chunk_offsets(data_start, atom_end, true);
            position = atom_end;
        }
        return have_audio && stts_count > 0 && stsc_count > 0 && sample_count > 0 &&
            chunk_count > 0 && chunk_offsets_offset != 0;
    }

    bool find_tables(uint64_t start, uint64_t end, unsigned depth = 0)
    {
        if (depth > 8) return false;
        uint64_t position = start;
        while (position + 8 <= end) {
            uint64_t data_start = 0;
            uint64_t atom_end = 0;
            uint8_t type[4]{};
            if (!read_atom(position, end, &data_start, &atom_end, type)) return false;
            if (type_is(type, "stbl") && parse_stbl(data_start, atom_end)) return true;
            if (is_container(type)) {
                uint64_t child_start = data_start;
                if (type_is(type, "meta") && child_start + 4 <= atom_end) child_start += 4;
                if (find_tables(child_start, atom_end, depth + 1)) return true;
            }
            position = atom_end;
        }
        return false;
    }

    bool initialize()
    {
        if (initialized) return true;
        if (failed || !file || audio_seek(file, 0, SEEK_END) != 0) return false;
        const long file_end = std::ftell(file);
        if (file_end < 12) return false;
        clear_tables();
        uint64_t position = 0;
        while (position + 8 <= static_cast<uint64_t>(file_end)) {
            uint64_t data_start = 0;
            uint64_t atom_end = 0;
            uint8_t type[4]{};
            if (!read_atom(position, static_cast<uint64_t>(file_end), &data_start,
                           &atom_end, type)) break;
            if (type_is(type, "moov") && find_tables(data_start, atom_end)) {
                initialized = true;
                return true;
            }
            position = atom_end;
        }
        failed = true;
        return false;
    }

    bool read_sample_size(uint32_t sample, uint32_t *size)
    {
        if (!size || sample >= sample_count) return false;
        if (default_sample_size != 0) {
            *size = default_sample_size;
            return true;
        }
        uint8_t bytes[4]{};
        if (!read_at(sample_sizes_offset + static_cast<uint64_t>(sample) * 4,
                     bytes, sizeof(bytes))) return false;
        *size = be32(bytes);
        return *size != 0;
    }

    bool read_chunk_offset(uint32_t chunk, uint64_t *offset)
    {
        if (!offset || chunk == 0 || chunk > chunk_count) return false;
        const uint64_t entry_size = chunk_offsets_64 ? 8 : 4;
        uint8_t bytes[8]{};
        if (!read_at(chunk_offsets_offset + static_cast<uint64_t>(chunk - 1) * entry_size,
                     bytes, entry_size)) return false;
        *offset = chunk_offsets_64 ? be64(bytes) : be32(bytes);
        return true;
    }

    bool locate_sample(uint32_t sample)
    {
        if (sample >= sample_count || stsc_count == 0) return false;
        uint64_t remaining = sample;
        uint32_t chunk = 0;
        uint32_t sample_in_chunk = 0;
        uint32_t samples_in_chunk = 0;
        for (uint32_t index = 0; index < stsc_count; ++index) {
            const uint32_t first_chunk = stsc[index].first_chunk;
            const uint32_t next_chunk = index + 1 < stsc_count ?
                stsc[index + 1].first_chunk : chunk_count + 1;
            if (next_chunk <= first_chunk) return false;
            const uint64_t segment_samples = static_cast<uint64_t>(next_chunk - first_chunk) *
                                              stsc[index].samples_per_chunk;
            if (remaining >= segment_samples) {
                remaining -= segment_samples;
                continue;
            }
            chunk = first_chunk + static_cast<uint32_t>(remaining / stsc[index].samples_per_chunk);
            sample_in_chunk = static_cast<uint32_t>(remaining % stsc[index].samples_per_chunk);
            samples_in_chunk = stsc[index].samples_per_chunk;
            break;
        }
        if (chunk == 0) return false;

        uint64_t offset = 0;
        if (!read_chunk_offset(chunk, &offset)) return false;
        const uint32_t first_sample = sample - sample_in_chunk;
        for (uint32_t index = first_sample; index < sample; ++index) {
            uint32_t size = 0;
            if (!read_sample_size(index, &size)) return false;
            offset += size;
        }
        uint32_t size = 0;
        if (!read_sample_size(sample, &size)) return false;
        current_sample = sample;
        current_offset = offset;
        current_size = size;
        current_chunk_sample = sample_in_chunk;
        current_chunk_samples = samples_in_chunk;
        current_valid = true;
        return true;
    }

    uint32_t sample_for_time(uint64_t target_samples, uint64_t *base_samples)
    {
        uint64_t elapsed = 0;
        uint64_t sample = 0;
        for (uint32_t index = 0; index < stts_count; ++index) {
            const uint64_t entry_samples = stts[index].sample_count;
            const uint64_t entry_duration = entry_samples * stts[index].sample_delta;
            if (target_samples < elapsed + entry_duration) {
                const uint64_t within = (target_samples - elapsed) / stts[index].sample_delta;
                *base_samples = elapsed + within * stts[index].sample_delta;
                return static_cast<uint32_t>(std::min<uint64_t>(
                    sample + within, sample_count - 1));
            }
            elapsed += entry_duration;
            sample += entry_samples;
        }
        *base_samples = elapsed;
        return sample_count - 1;
    }

    bool prepare_seek(uint32_t position_ms, uint64_t *base_samples)
    {
        if (!base_samples || !initialize() || sample_rate == 0 || channels == 0) return false;
        const uint64_t target_samples =
            (static_cast<uint64_t>(position_ms) * sample_rate) / 1000u;
        const uint32_t sample = sample_for_time(target_samples, base_samples);
        return locate_sample(sample);
    }

    OggPacketReadResult read_audio_sample(uint8_t *output, size_t capacity, size_t *length)
    {
        if (!output || !length || !initialize()) return OggPacketReadResult::kError;
        if (current_sample >= sample_count) return OggPacketReadResult::kEnd;
        if (!current_valid && !locate_sample(current_sample)) return OggPacketReadResult::kError;
        if (current_size > capacity || audio_seek(file, static_cast<long>(current_offset), SEEK_SET) != 0 ||
            audio_read(file, output, current_size) != current_size) {
            return OggPacketReadResult::kError;
        }
        *length = current_size;
        ++current_sample;
        if (current_sample >= sample_count || current_chunk_sample + 1 >= current_chunk_samples) {
            current_valid = false;
        } else {
            ++current_chunk_sample;
            current_offset += current_size;
            if (!read_sample_size(current_sample, &current_size)) current_valid = false;
        }
        return OggPacketReadResult::kPacket;
    }
};

struct AudioReadAhead {
    FILE *file;
    uint8_t *buffer;
    size_t capacity;
    size_t read_index;
    size_t write_index;
    size_t available;
    size_t chunk_bytes;
    bool eof;
    bool error;
    OggOpusReader *ogg_opus = nullptr;
    M4aAacReader *m4a_aac = nullptr;
    // AIFF/AIFC is a big-endian PCM container rather than an esp_audio_codec
    // simple-decoder format. The source bytes are converted in place before
    // they enter the common PCM/I2S path.
    bool bounded_source = false;
    bool source_little_endian = false;
    uint8_t source_bytes_per_sample = 0;
    uint8_t source_channels = 0;
    uint64_t source_remaining = 0;

    size_t contiguous() const
    {
        if (available == 0) return 0;
        return std::min(available, capacity - read_index);
    }

    void update_low_watermark()
    {
        if (available < s_diagnostics.audio_buffer_low_watermark) {
            s_diagnostics.audio_buffer_low_watermark = available;
        }
    }

    bool fill()
    {
        if (!file || !buffer || eof || error || available == capacity) return true;

        if (ogg_opus) {
            // Raw Opus decoding requires exactly one complete packet. The
            // packet reader leaves the encoded packet contiguous in the ring,
            // so the normal decoder loop can consume it without handing an
            // entire OGG page or a partial packet to the Opus codec.
            if (available > 0) return true;
            read_index = 0;
            write_index = 0;
            size_t packet_length = 0;
            const OggPacketReadResult packet = ogg_opus->read_audio_packet(
                buffer, capacity, &packet_length);
            if (packet == OggPacketReadResult::kEnd) {
                eof = true;
                return true;
            }
            if (packet != OggPacketReadResult::kPacket || packet_length == 0) {
                error = true;
                return false;
            }
            write_index = packet_length;
            available = packet_length;
            update_low_watermark();
            return true;
        }

        if (m4a_aac) {
            // M4A sample tables provide complete AAC access units. Keep one
            // sample contiguous so the raw AAC decoder never receives a
            // partial access unit or the surrounding container bytes.
            if (available > 0) return true;
            read_index = 0;
            write_index = 0;
            size_t sample_length = 0;
            const OggPacketReadResult sample = m4a_aac->read_audio_sample(
                buffer, capacity, &sample_length);
            if (sample == OggPacketReadResult::kEnd) {
                eof = true;
                return true;
            }
            if (sample != OggPacketReadResult::kPacket || sample_length == 0) {
                error = true;
                return false;
            }
            write_index = sample_length;
            available = sample_length;
            update_low_watermark();
            return true;
        }

        if (available == 0 && chunk_bytes >= capacity - 1) {
            // A full-buffer refill avoids the many small SD transactions that
            // otherwise dominate long M4A replay seeks.
            read_index = 0;
            write_index = 0;
        }
        const size_t free_bytes = capacity - available;
        const size_t contiguous_free = std::min(free_bytes, capacity - write_index);
        size_t wanted = std::min(contiguous_free, chunk_bytes);
        if (bounded_source) {
            wanted = std::min<uint64_t>(wanted, source_remaining);
            const size_t frame_bytes = static_cast<size_t>(source_bytes_per_sample) *
                                       source_channels;
            if (frame_bytes > 0) wanted -= wanted % frame_bytes;
        }
        if (wanted == 0) return true;

        const int64_t transaction_started = esp_timer_get_time();
        if (!lyra::sd::acquire(lyra::sd::Client::Audio)) {
            ++s_diagnostics.underrun_count;
            return false;
        }
        const uint32_t lock_wait = static_cast<uint32_t>(std::max<int64_t>(
            0, esp_timer_get_time() - transaction_started));
        const size_t count = std::fread(buffer + write_index, 1, wanted, file);
        const uint32_t elapsed = static_cast<uint32_t>(std::max<int64_t>(
            0, esp_timer_get_time() - transaction_started));
        lyra::sd::release(lyra::sd::Client::Audio);
        ++s_diagnostics.sd_read_count;
        s_diagnostics.total_sd_read_us += elapsed;
        s_diagnostics.max_sd_read_us = std::max(s_diagnostics.max_sd_read_us, elapsed);
        s_diagnostics.total_sd_lock_wait_us += lock_wait;
        s_diagnostics.max_sd_lock_wait_us = std::max(
            s_diagnostics.max_sd_lock_wait_us, lock_wait);

        if (count > 0) {
            if (bounded_source && !source_little_endian && source_bytes_per_sample > 1) {
                const size_t frame_bytes = static_cast<size_t>(source_bytes_per_sample) *
                                           source_channels;
                for (size_t frame = 0; frame + frame_bytes <= count; frame += frame_bytes) {
                    for (uint8_t channel = 0; channel < source_channels; ++channel) {
                        uint8_t *sample = buffer + write_index + frame +
                                           static_cast<size_t>(channel) * source_bytes_per_sample;
                        for (size_t left = 0, right = source_bytes_per_sample - 1;
                             left < right; ++left, --right) {
                            std::swap(sample[left], sample[right]);
                        }
                    }
                }
            }
            write_index = (write_index + count) % capacity;
            available += count;
            if (bounded_source) {
                source_remaining -= std::min<uint64_t>(source_remaining, count);
                if (source_remaining == 0) eof = true;
            }
        }
        if (count < wanted && !eof) {
            eof = std::feof(file) != 0;
            error = std::ferror(file) != 0;
            if (error) return false;
        }
        return count > 0 || eof;
    }

    void consume(size_t count)
    {
        if (count > available) count = available;
        read_index = (read_index + count) % capacity;
        available -= count;
        update_low_watermark();
    }
};

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

enum class AudioFormat : uint8_t {
    kMp3,
    kFlac,
    kAac,
    kM4a,
    kWav,
    kOgg,
    kOpus,
    kAiff,
};

bool extension_is(const char *extension, const char *expected)
{
    if (!extension || !expected) return false;
    while (*extension && *expected) {
        const char left = *extension >= 'A' && *extension <= 'Z' ?
            static_cast<char>(*extension - 'A' + 'a') : *extension;
        const char right = *expected >= 'A' && *expected <= 'Z' ?
            static_cast<char>(*expected - 'A' + 'a') : *expected;
        if (left != right) return false;
        ++extension;
        ++expected;
    }
    return *extension == '\0' && *expected == '\0';
}

bool get_audio_format(const char *path, AudioFormat *format)
{
    if (!path || !format) return false;
    const char *dot = std::strrchr(path, '.');
    if (!dot) return false;
    const char *extension = dot + 1;
    if (extension_is(extension, "mp3")) {
        *format = AudioFormat::kMp3;
        return true;
    }
    if (extension_is(extension, "flac")) {
        *format = AudioFormat::kFlac;
        return true;
    }
    if (extension_is(extension, "aac")) {
        *format = AudioFormat::kAac;
        return true;
    }
    if (extension_is(extension, "m4a") || extension_is(extension, "mp4")) {
        *format = AudioFormat::kM4a;
        return true;
    }
    if (extension_is(extension, "wav")) {
        *format = AudioFormat::kWav;
        return true;
    }
    if (extension_is(extension, "ogg") || extension_is(extension, "opus")) {
        *format = AudioFormat::kOgg;
        return true;
    }
    if (extension_is(extension, "aiff") || extension_is(extension, "aif") ||
        extension_is(extension, "aifc")) {
        *format = AudioFormat::kAiff;
        return true;
    }
    return false;
}

const char *audio_format_name(AudioFormat format)
{
    switch (format) {
    case AudioFormat::kMp3: return "MP3";
    case AudioFormat::kFlac: return "FLAC";
    case AudioFormat::kAac: return "AAC";
    case AudioFormat::kM4a: return "M4A";
    case AudioFormat::kWav: return "WAV";
    case AudioFormat::kOgg: return "OGG/Opus";
    case AudioFormat::kOpus: return "OPUS";
    case AudioFormat::kAiff: return "AIFF";
    }
    return "audio";
}

esp_audio_simple_dec_type_t decoder_type_for_format(AudioFormat format)
{
    switch (format) {
    case AudioFormat::kMp3: return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    case AudioFormat::kFlac: return ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
    case AudioFormat::kAac: return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
    case AudioFormat::kM4a: return ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;
    case AudioFormat::kWav: return ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
    case AudioFormat::kOgg: return ESP_AUDIO_SIMPLE_DEC_TYPE_OGG;
    case AudioFormat::kOpus: break;
    case AudioFormat::kAiff: break;
    }
    return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
}

struct AiffStreamInfo {
    long data_start;
    uint64_t data_bytes;
    uint32_t sample_rate;
    uint32_t frame_count;
    uint8_t channels;
    uint8_t bits_per_sample;
    bool little_endian;
};

uint32_t read_be32(const uint8_t *bytes);
uint64_t read_be64(const uint8_t *bytes);
uint32_t read_synchsafe32(const uint8_t *bytes);
uint32_t samples_to_milliseconds(uint64_t samples, uint32_t sample_rate);

uint16_t read_be16(const uint8_t *bytes)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(bytes[0]) << 8) | bytes[1]);
}

double read_aiff_extended_rate(const uint8_t *bytes)
{
    const uint16_t exponent = static_cast<uint16_t>(((bytes[0] & 0x7F) << 8) | bytes[1]);
    if ((bytes[0] & 0x80) != 0 || exponent == 0 || exponent == 0x7FFF) return 0.0;
    const uint64_t mantissa = (static_cast<uint64_t>(bytes[2]) << 56) |
                              (static_cast<uint64_t>(bytes[3]) << 48) |
                              (static_cast<uint64_t>(bytes[4]) << 40) |
                              (static_cast<uint64_t>(bytes[5]) << 32) |
                              (static_cast<uint64_t>(bytes[6]) << 24) |
                              (static_cast<uint64_t>(bytes[7]) << 16) |
                              (static_cast<uint64_t>(bytes[8]) << 8) | bytes[9];
    return std::ldexp(static_cast<double>(mantissa) / 9223372036854775808.0,
                      static_cast<int>(exponent) - 16383);
}

bool read_aiff_stream_info(FILE *file, AiffStreamInfo *info)
{
    if (!file || !info || audio_seek(file, 0, SEEK_SET) != 0) return false;
    *info = {};
    uint8_t form[12]{};
    if (audio_read(file, form, sizeof(form)) != sizeof(form) ||
        std::memcmp(form, "FORM", 4) != 0 ||
        (std::memcmp(form + 8, "AIFF", 4) != 0 && std::memcmp(form + 8, "AIFC", 4) != 0)) {
        return false;
    }
    const bool aifc = std::memcmp(form + 8, "AIFC", 4) == 0;
    long file_end = -1;
    if (audio_seek(file, 0, SEEK_END) == 0) file_end = std::ftell(file);
    if (file_end < 12) return false;

    bool have_comm = false;
    bool have_ssnd = false;
    uint64_t position = 12;
    while (position + 8 <= static_cast<uint64_t>(file_end)) {
        uint8_t chunk[8]{};
        if (audio_seek(file, static_cast<long>(position), SEEK_SET) != 0 ||
            audio_read(file, chunk, sizeof(chunk)) != sizeof(chunk)) return false;
        const uint32_t length = read_be32(chunk + 4);
        const uint64_t data_start = position + 8;
        const uint64_t data_end = data_start + length;
        if (data_end > static_cast<uint64_t>(file_end)) return false;

        if (std::memcmp(chunk, "COMM", 4) == 0 && length >= 18) {
            uint8_t comm[22]{};
            const size_t wanted = std::min<size_t>(length, sizeof(comm));
            if (audio_seek(file, static_cast<long>(data_start), SEEK_SET) != 0 ||
                audio_read(file, comm, wanted) != wanted) return false;
            info->channels = static_cast<uint8_t>(read_be16(comm));
            info->frame_count = read_be32(comm + 2);
            info->bits_per_sample = static_cast<uint8_t>(read_be16(comm + 6));
            const double rate = read_aiff_extended_rate(comm + 8);
            info->sample_rate = rate > 0.0 && rate < 1000000.0 ?
                static_cast<uint32_t>(rate + 0.5) : 0;
            if (aifc && length >= 22) {
                // AIFC's `sowt` compression is little-endian PCM. `NONE`
                // and `twos` remain big-endian linear PCM.
                info->little_endian = std::memcmp(comm + 18, "sowt", 4) == 0;
                if (std::memcmp(comm + 18, "NONE", 4) != 0 &&
                    std::memcmp(comm + 18, "twos", 4) != 0 &&
                    std::memcmp(comm + 18, "sowt", 4) != 0) return false;
            }
            have_comm = true;
        } else if (std::memcmp(chunk, "SSND", 4) == 0 && length >= 8) {
            uint8_t ssnd[8]{};
            if (audio_seek(file, static_cast<long>(data_start), SEEK_SET) != 0 ||
                audio_read(file, ssnd, sizeof(ssnd)) != sizeof(ssnd)) return false;
            const uint32_t offset = read_be32(ssnd);
            if (offset > length - 8) return false;
            info->data_start = static_cast<long>(data_start + 8 + offset);
            info->data_bytes = length - 8 - offset;
            have_ssnd = true;
        }
        position = data_end + (length & 1u);
    }

    const size_t bytes_per_frame = static_cast<size_t>(info->channels) *
                                   (info->bits_per_sample / 8u);
    return have_comm && have_ssnd && info->sample_rate != 0 && info->channels > 0 &&
           info->channels <= 8 && (info->bits_per_sample == 8 ||
                                   info->bits_per_sample == 16 ||
                                   info->bits_per_sample == 24 ||
                                   info->bits_per_sample == 32) &&
           bytes_per_frame != 0 && info->data_bytes >= bytes_per_frame &&
           info->data_bytes % bytes_per_frame == 0;
}

uint32_t read_aiff_duration_ms(const AiffStreamInfo &info)
{
    const size_t bytes_per_frame = static_cast<size_t>(info.channels) *
                                   (info.bits_per_sample / 8u);
    const uint64_t frames = bytes_per_frame == 0 ? 0 : info.data_bytes / bytes_per_frame;
    const uint64_t milliseconds = info.sample_rate == 0 ? 0 :
        (frames * 1000u + info.sample_rate / 2u) / info.sample_rate;
    return milliseconds > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<uint32_t>(milliseconds);
}

uint16_t read_le16(const uint8_t *bytes)
{
    return static_cast<uint16_t>(bytes[0] | (static_cast<uint16_t>(bytes[1]) << 8));
}

uint32_t read_le32(const uint8_t *bytes)
{
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) |
           (static_cast<uint32_t>(bytes[3]) << 24);
}

uint64_t read_le64(const uint8_t *bytes)
{
    return static_cast<uint64_t>(read_le32(bytes)) |
           (static_cast<uint64_t>(read_le32(bytes + 4)) << 32);
}

struct WavStreamInfo {
    uint32_t sample_rate = 0;
    uint32_t byte_rate = 0;
    uint64_t data_bytes = 0;
    long data_start = 0;
};

bool read_wav_stream_info(FILE *file, WavStreamInfo *info)
{
    if (!file || !info || audio_seek(file, 0, SEEK_SET) != 0) return false;
    *info = {};
    uint8_t header[12]{};
    if (audio_read(file, header, sizeof(header)) != sizeof(header) ||
        std::memcmp(header, "RIFF", 4) != 0 || std::memcmp(header + 8, "WAVE", 4) != 0) {
        return false;
    }

    long file_end = -1;
    if (audio_seek(file, 0, SEEK_END) == 0) file_end = std::ftell(file);
    if (file_end < 12) return false;

    bool have_fmt = false;
    bool have_data = false;
    uint64_t position = 12;
    while (position + 8 <= static_cast<uint64_t>(file_end)) {
        uint8_t chunk[8]{};
        if (audio_seek(file, static_cast<long>(position), SEEK_SET) != 0 ||
            audio_read(file, chunk, sizeof(chunk)) != sizeof(chunk)) return false;
        const uint32_t length = read_le32(chunk + 4);
        const uint64_t data_start = position + 8;
        if (data_start + length > static_cast<uint64_t>(file_end)) return false;
        if (std::memcmp(chunk, "fmt ", 4) == 0 && length >= 16) {
            uint8_t fmt[16]{};
            if (audio_seek(file, static_cast<long>(data_start), SEEK_SET) != 0 ||
                audio_read(file, fmt, sizeof(fmt)) != sizeof(fmt)) return false;
            info->sample_rate = read_le32(fmt + 4);
            info->byte_rate = read_le32(fmt + 8);
            have_fmt = info->sample_rate != 0 && info->byte_rate != 0;
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            info->data_start = static_cast<long>(data_start);
            info->data_bytes = length;
            have_data = length > 0;
        }
        position = data_start + length + (length & 1u);
    }
    audio_seek(file, 0, SEEK_SET);
    return have_fmt && have_data;
}

uint32_t read_wav_duration_ms(FILE *file)
{
    WavStreamInfo info{};
    if (!read_wav_stream_info(file, &info)) return 0;
    const uint64_t milliseconds = info.byte_rate == 0 ? 0 :
        (info.data_bytes * 1000u + info.byte_rate / 2u) / info.byte_rate;
    audio_seek(file, 0, SEEK_SET);
    return milliseconds > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<uint32_t>(milliseconds);
}

struct AacFrameInfo {
    uint32_t sample_rate = 0;
    uint32_t samples_per_frame = 0;
    uint32_t frame_length = 0;
    uint8_t header_length = 0;
};

bool parse_aac_frame_header(const uint8_t *header, AacFrameInfo *info)
{
    if (!header || !info || header[0] != 0xFF ||
        ((header[1] & 0xF6u) != 0xF0u && (header[1] & 0xF6u) != 0xF8u)) return false;
    static constexpr uint32_t kSampleRates[] = {
        96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
        16000, 12000, 11025, 8000, 7350,
    };
    const uint8_t sample_rate_index = static_cast<uint8_t>((header[2] >> 2) & 0x0Fu);
    if (sample_rate_index >= sizeof(kSampleRates) / sizeof(kSampleRates[0])) return false;
    const uint32_t frame_length = (static_cast<uint32_t>(header[3] & 0x03u) << 11) |
                                  (static_cast<uint32_t>(header[4]) << 3) |
                                  (header[5] >> 5);
    const uint8_t header_length = (header[1] & 0x01u) != 0 ? 7 : 9;
    if (frame_length < header_length) return false;
    info->sample_rate = kSampleRates[sample_rate_index];
    info->samples_per_frame = 1024u * (1u + (header[6] & 0x03u));
    info->frame_length = frame_length;
    info->header_length = header_length;
    return true;
}

bool find_first_aac_frame(FILE *file, long start, long *frame_offset, AacFrameInfo *info)
{
    if (!file || !frame_offset || !info || start < 0 || audio_seek(file, start, SEEK_SET) != 0) {
        return false;
    }
    uint8_t header[7]{};
    while (audio_read(file, header, sizeof(header)) == sizeof(header)) {
        const long candidate = std::ftell(file) - static_cast<long>(sizeof(header));
        if (candidate < 0) return false;
        if (parse_aac_frame_header(header, info)) {
            *frame_offset = candidate;
            return true;
        }
        if (audio_seek(file, candidate + 1, SEEK_SET) != 0) return false;
    }
    return false;
}

uint32_t read_aac_duration_ms(FILE *file)
{
    if (!file) return 0;
    long file_end = -1;
    if (audio_seek(file, 0, SEEK_END) == 0) file_end = std::ftell(file);
    if (file_end <= 0) return 0;

    long audio_start = 0;
    uint8_t id3_header[10]{};
    if (audio_seek(file, 0, SEEK_SET) == 0 &&
        audio_read(file, id3_header, sizeof(id3_header)) == sizeof(id3_header) &&
        std::memcmp(id3_header, "ID3", 3) == 0) {
        audio_start = 10 + static_cast<long>(read_synchsafe32(id3_header + 6));
        if ((id3_header[5] & 0x10u) != 0) audio_start += 10;
    }

    long first_frame = 0;
    AacFrameInfo first_info{};
    if (!find_first_aac_frame(file, audio_start, &first_frame, &first_info)) {
        audio_seek(file, 0, SEEK_SET);
        return 0;
    }

    uint64_t total_samples = 0;
    long position = first_frame;
    uint32_t frame_count = 0;
    while (position >= 0 && position + 7 <= file_end) {
        uint8_t header[7]{};
        AacFrameInfo info{};
        if (audio_seek(file, position, SEEK_SET) != 0 ||
            audio_read(file, header, sizeof(header)) != sizeof(header) ||
            !parse_aac_frame_header(header, &info) ||
            position + info.frame_length > file_end) break;
        total_samples += info.samples_per_frame;
        position += info.frame_length;
        if ((++frame_count & 0x3Fu) == 0) vTaskDelay(1);
    }
    audio_seek(file, 0, SEEK_SET);
    return samples_to_milliseconds(total_samples, first_info.sample_rate);
}

bool seek_aac_file(FILE *file, uint32_t position_ms, uint32_t duration_ms)
{
    if (!file || duration_ms == 0) return false;
    if (position_ms == 0) return audio_seek(file, 0, SEEK_SET) == 0;
    long file_end = -1;
    if (audio_seek(file, 0, SEEK_END) == 0) file_end = std::ftell(file);
    if (file_end <= 0) return false;
    const uint64_t estimated = static_cast<uint64_t>(file_end) * position_ms / duration_ms;
    long offset = 0;
    AacFrameInfo info{};
    if (!find_first_aac_frame(file, static_cast<long>(std::min<uint64_t>(
            estimated, static_cast<uint64_t>(file_end - 1))), &offset, &info)) return false;
    return audio_seek(file, offset, SEEK_SET) == 0;
}

struct M4aAtomInfo {
    uint64_t data_start = 0;
    uint64_t end = 0;
    uint8_t type[4]{};
};

bool m4a_type(const M4aAtomInfo &atom, const char (&type)[5])
{
    return std::memcmp(atom.type, type, 4) == 0;
}

bool read_m4a_atom(FILE *file, uint64_t position, uint64_t limit, M4aAtomInfo *atom)
{
    if (!file || !atom || position + 8 > limit ||
        audio_seek(file, static_cast<long>(position), SEEK_SET) != 0) return false;
    uint8_t header[16]{};
    if (audio_read(file, header, 8) != 8) return false;
    uint64_t size = read_be32(header);
    uint64_t header_size = 8;
    if (size == 1) {
        if (position + 16 > limit || audio_read(file, header + 8, 8) != 8) return false;
        size = read_be64(header + 8);
        header_size = 16;
    } else if (size == 0) {
        size = limit - position;
    }
    if (size < header_size || size > limit - position) return false;
    atom->data_start = position + header_size;
    atom->end = position + size;
    std::memcpy(atom->type, header + 4, 4);
    return true;
}

bool m4a_container(const M4aAtomInfo &atom)
{
    return m4a_type(atom, "moov") || m4a_type(atom, "trak") || m4a_type(atom, "mdia") ||
           m4a_type(atom, "minf") || m4a_type(atom, "stbl") || m4a_type(atom, "udta") ||
           m4a_type(atom, "meta") || m4a_type(atom, "edts") || m4a_type(atom, "dinf") ||
           m4a_type(atom, "mvex") || m4a_type(atom, "moof") || m4a_type(atom, "traf");
}

uint32_t m4a_time_to_ms(uint64_t duration, uint32_t timescale)
{
    if (duration == 0 || timescale == 0) return 0;
    const uint64_t max_uint64 = ~static_cast<uint64_t>(0);
    const uint64_t milliseconds = duration > max_uint64 / 1000u ? max_uint64 :
        (duration * 1000u + timescale / 2u) / timescale;
    return milliseconds > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<uint32_t>(milliseconds);
}

struct M4aDurationCandidates {
    uint32_t movie_ms = 0;
    uint32_t track_ms = 0;
};

void read_m4a_time_atom(FILE *file, const M4aAtomInfo &atom,
                        M4aDurationCandidates *candidates)
{
    if (!file || !candidates || atom.end - atom.data_start < 20) return;
    uint8_t data[32]{};
    const size_t wanted = std::min<uint64_t>(sizeof(data), atom.end - atom.data_start);
    if (audio_seek(file, static_cast<long>(atom.data_start), SEEK_SET) != 0 ||
        audio_read(file, data, wanted) != wanted) return;
    const uint8_t version = data[0];
    uint64_t duration = 0;
    uint32_t timescale = 0;
    if (version == 0 && wanted >= 20) {
        timescale = read_be32(data + 12);
        duration = read_be32(data + 16);
    } else if (version == 1 && wanted >= 32) {
        timescale = read_be32(data + 20);
        duration = read_be64(data + 24);
    } else {
        return;
    }
    const uint32_t duration_ms = m4a_time_to_ms(duration, timescale);
    if (m4a_type(atom, "mvhd")) candidates->movie_ms = duration_ms;
    else if (m4a_type(atom, "mdhd") && candidates->track_ms == 0) candidates->track_ms = duration_ms;
}

void scan_m4a_duration(FILE *file, uint64_t start, uint64_t end,
                       M4aDurationCandidates *candidates, unsigned depth = 0)
{
    if (!file || !candidates || depth > 8) return;
    uint64_t position = start;
    while (position + 8 <= end) {
        M4aAtomInfo atom{};
        if (!read_m4a_atom(file, position, end, &atom)) return;
        if (m4a_type(atom, "mvhd") || m4a_type(atom, "mdhd")) {
            read_m4a_time_atom(file, atom, candidates);
        } else if (m4a_container(atom)) {
            uint64_t child_start = atom.data_start;
            if (m4a_type(atom, "meta") && child_start + 4 <= atom.end) child_start += 4;
            scan_m4a_duration(file, child_start, atom.end, candidates, depth + 1);
        }
        position = atom.end;
    }
}

uint32_t read_m4a_duration_ms(FILE *file)
{
    if (!file || audio_seek(file, 0, SEEK_END) != 0) return 0;
    const long file_end = std::ftell(file);
    if (file_end < 12) return 0;
    M4aDurationCandidates candidates{};
    uint64_t position = 0;
    while (position + 8 <= static_cast<uint64_t>(file_end)) {
        M4aAtomInfo atom{};
        if (!read_m4a_atom(file, position, file_end, &atom)) break;
        if (m4a_type(atom, "moov")) {
            scan_m4a_duration(file, atom.data_start, atom.end, &candidates);
            break;
        }
        position = atom.end;
    }
    audio_seek(file, 0, SEEK_SET);
    return candidates.movie_ms != 0 ? candidates.movie_ms : candidates.track_ms;
}

uint32_t read_ogg_duration_ms(FILE *file)
{
    if (!file || audio_seek(file, 0, SEEK_SET) != 0) return 0;
    uint8_t page_header[27]{};
    uint8_t lacing[255]{};
    uint8_t first_packet[64]{};
    size_t first_length = 0;
    bool first_packet_done = false;
    int64_t last_granule = -1;
    uint32_t sample_rate = 0;
    uint32_t pre_skip = 0;
    bool failed = false;
    uint32_t page_count = 0;
    while (audio_read(file, page_header, sizeof(page_header)) == sizeof(page_header)) {
        if (std::memcmp(page_header, "OggS", 4) != 0 || page_header[4] != 0) break;
        const size_t segment_count = page_header[26];
        if (audio_read(file, lacing, segment_count) != segment_count) {
            failed = true;
            break;
        }
        for (size_t segment = 0; segment < segment_count; ++segment) {
            size_t remaining = lacing[segment];
            while (remaining > 0) {
                const size_t chunk = std::min(remaining, kDurationScanBufferBytes);
                if (audio_read(file, s_duration_scan_buffer, chunk) != chunk) {
                    failed = true;
                    break;
                }
                if (!first_packet_done && first_length < sizeof(first_packet)) {
                    const size_t copy = std::min(chunk, sizeof(first_packet) - first_length);
                    std::memcpy(first_packet + first_length, s_duration_scan_buffer, copy);
                    first_length += copy;
                }
                remaining -= chunk;
            }
            if (failed) break;
            if (lacing[segment] != 255 && !first_packet_done) {
                first_packet_done = true;
                if (first_length >= 19 && std::memcmp(first_packet, "OpusHead", 8) == 0) {
                    sample_rate = 48000;
                    pre_skip = read_le16(first_packet + 10);
                } else if (first_length >= 16 && first_packet[0] == 1 &&
                           std::memcmp(first_packet + 1, "vorbis", 6) == 0) {
                    sample_rate = read_le32(first_packet + 12);
                }
            }
        }
        if (failed) break;
        const int64_t granule = static_cast<int64_t>(read_le64(page_header + 6));
        if (granule >= 0) last_granule = granule;
        if ((++page_count & 0x1Fu) == 0) vTaskDelay(1);
    }
    audio_seek(file, 0, SEEK_SET);
    if (sample_rate == 0 || last_granule <= static_cast<int64_t>(pre_skip)) return 0;
    return samples_to_milliseconds(static_cast<uint64_t>(last_granule - pre_skip), sample_rate);
}

uint32_t read_ogg_tail_duration_ms(FILE *file, uint32_t sample_rate, uint32_t pre_skip)
{
    if (!file || sample_rate == 0 || audio_seek(file, 0, SEEK_END) != 0) return 0;
    const long file_end = std::ftell(file);
    if (file_end < 27) return 0;

    // Scanning every Ogg payload to find the final granule position makes
    // opening a large Opus file take seconds. The final page is normally in
    // the last few KiB, so inspect only a bounded tail and validate candidate
    // OggS signatures by checking their complete page length.
    const size_t tail_bytes = static_cast<size_t>(std::min<long>(256 * 1024, file_end));
    const long search_start = file_end - static_cast<long>(tail_bytes);
    auto *scan_buffer = static_cast<uint8_t *>(heap_caps_malloc(
        tail_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!scan_buffer) {
        audio_seek(file, 0, SEEK_SET);
        return 0;
    }
    uint8_t header[27]{};
    uint8_t lacing[255]{};
    uint64_t last_granule = 0;
    long last_page = -1;
    if (audio_seek(file, search_start, SEEK_SET) != 0 ||
        audio_read(file, scan_buffer, tail_bytes) != tail_bytes) {
        heap_caps_free(scan_buffer);
        audio_seek(file, 0, SEEK_SET);
        return 0;
    }
    for (size_t index = tail_bytes; index-- > 3;) {
            if (std::memcmp(scan_buffer + index - 3, "OggS", 4) != 0) continue;
            const long candidate = search_start + static_cast<long>(index - 3);
            if (candidate <= last_page || candidate + 27 > file_end ||
                audio_seek(file, candidate, SEEK_SET) != 0 ||
                audio_read(file, header, sizeof(header)) != sizeof(header) ||
                header[4] != 0) continue;
            const size_t segment_count = header[26];
            if (audio_read(file, lacing, segment_count) != segment_count) continue;
            size_t payload_bytes = 0;
            for (size_t segment = 0; segment < segment_count; ++segment) {
                payload_bytes += lacing[segment];
            }
            const uint64_t page_bytes = 27u + segment_count + payload_bytes;
            if (page_bytes > static_cast<uint64_t>(file_end - candidate)) continue;
            const uint64_t granule = read_le64(header + 6);
            if (granule == ~static_cast<uint64_t>(0)) continue;
            last_page = candidate;
            last_granule = granule;
            if (candidate + static_cast<long>(page_bytes) == file_end) {
                break;
            }
    }
    heap_caps_free(scan_buffer);
    audio_seek(file, 0, SEEK_SET);
    if (last_page < 0 || last_granule <= pre_skip) return 0;
    return samples_to_milliseconds(last_granule - pre_skip, sample_rate);
}

struct Mp3FrameInfo {
    uint32_t sample_rate;
    uint32_t samples_per_frame;
    uint32_t frame_length;
    uint32_t bitrate_bps;
    uint8_t version;
    uint8_t channel_mode;
};

uint32_t read_be32(const uint8_t *bytes)
{
    return (static_cast<uint32_t>(bytes[0]) << 24) |
           (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) |
           static_cast<uint32_t>(bytes[3]);
}

uint32_t read_be24(const uint8_t *bytes)
{
    return (static_cast<uint32_t>(bytes[0]) << 16) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           static_cast<uint32_t>(bytes[2]);
}

uint32_t read_synchsafe32(const uint8_t *bytes)
{
    return (static_cast<uint32_t>(bytes[0] & 0x7F) << 21) |
           (static_cast<uint32_t>(bytes[1] & 0x7F) << 14) |
           (static_cast<uint32_t>(bytes[2] & 0x7F) << 7) |
           static_cast<uint32_t>(bytes[3] & 0x7F);
}

bool parse_mp3_frame_header(const uint8_t *header, Mp3FrameInfo *info)
{
    if (!header || !info || header[0] != 0xFF || (header[1] & 0xE0) != 0xE0) return false;

    const uint8_t version = static_cast<uint8_t>((header[1] >> 3) & 0x03);
    const uint8_t layer = static_cast<uint8_t>((header[1] >> 1) & 0x03);
    const uint8_t bitrate_index = static_cast<uint8_t>((header[2] >> 4) & 0x0F);
    const uint8_t sample_rate_index = static_cast<uint8_t>((header[2] >> 2) & 0x03);
    if (version == 1 || layer != 1 || bitrate_index == 0 || bitrate_index == 15 ||
        sample_rate_index == 3) {
        return false;
    }

    static constexpr uint16_t kMpeg1Bitrates[] = {
        0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
    static constexpr uint16_t kMpeg2Bitrates[] = {
        0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
    static constexpr uint32_t kBaseSampleRates[] = {44100, 48000, 32000};

    const uint32_t base_sample_rate = kBaseSampleRates[sample_rate_index];
    const uint32_t sample_rate = version == 3 ? base_sample_rate :
                                  version == 2 ? base_sample_rate / 2 : base_sample_rate / 4;
    const uint32_t bitrate_kbps = version == 3 ? kMpeg1Bitrates[bitrate_index] :
                                  kMpeg2Bitrates[bitrate_index];
    if (sample_rate == 0 || bitrate_kbps == 0) return false;

    const uint32_t samples_per_frame = version == 3 ? 1152 : 576;
    const uint32_t slot_size = version == 3 ? 144 : 72;
    const uint32_t padding = (header[2] >> 1) & 0x01;
    const uint32_t frame_length =
        (slot_size * bitrate_kbps * 1000) / sample_rate + padding;
    if (frame_length < 4) return false;

    info->sample_rate = sample_rate;
    info->samples_per_frame = samples_per_frame;
    info->frame_length = frame_length;
    info->bitrate_bps = bitrate_kbps * 1000;
    info->version = version;
    info->channel_mode = static_cast<uint8_t>((header[3] >> 6) & 0x03);
    return true;
}

bool find_first_mp3_frame(FILE *file, long audio_start, long *frame_offset,
                          Mp3FrameInfo *frame_info)
{
    if (!file || !frame_offset || !frame_info || audio_start < 0 ||
        audio_seek(file, audio_start, SEEK_SET) != 0) {
        return false;
    }

    uint8_t header[4];
    while (audio_read(file, header, sizeof(header)) == sizeof(header)) {
        const long candidate = std::ftell(file) - static_cast<long>(sizeof(header));
        if (candidate < 0) return false;
        if (parse_mp3_frame_header(header, frame_info)) {
            *frame_offset = candidate;
            return true;
        }
        if (audio_seek(file, candidate + 1, SEEK_SET) != 0) return false;
    }
    return false;
}

uint32_t samples_to_milliseconds(uint64_t samples, uint32_t sample_rate)
{
    if (sample_rate == 0 || samples == 0) return 0;
    const uint64_t max_uint64 = ~static_cast<uint64_t>(0);
    const uint64_t rounding = sample_rate / 2u;
    const uint64_t milliseconds = samples > (max_uint64 - rounding) / 1000u ? max_uint64 :
        (samples * 1000u + rounding) / sample_rate;
    return milliseconds > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<uint32_t>(milliseconds);
}

bool read_xing_or_vbri_duration(FILE *file, long frame_offset,
                                 const Mp3FrameInfo &frame_info, uint32_t *duration_ms)
{
    if (!file || !duration_ms) return false;

    const uint32_t side_info_bytes = frame_info.version == 3 ?
        (frame_info.channel_mode == 3 ? 17u : 32u) :
        (frame_info.channel_mode == 3 ? 9u : 17u);
    const long xing_offset = frame_offset + 4 + static_cast<long>(side_info_bytes);
    if (audio_seek(file, xing_offset, SEEK_SET) == 0) {
        uint8_t marker[4];
        uint8_t flags[4];
        uint8_t frames[4];
        if (audio_read(file, marker, sizeof(marker)) == sizeof(marker) &&
            (std::memcmp(marker, "Xing", sizeof(marker)) == 0 ||
             std::memcmp(marker, "Info", sizeof(marker)) == 0) &&
            audio_read(file, flags, sizeof(flags)) == sizeof(flags) &&
            (read_be32(flags) & 0x01u) != 0 &&
            audio_read(file, frames, sizeof(frames)) == sizeof(frames)) {
            *duration_ms = samples_to_milliseconds(
                static_cast<uint64_t>(read_be32(frames)) * frame_info.samples_per_frame,
                frame_info.sample_rate);
            return *duration_ms != 0;
        }
    }

    // VBRI places its header 32 bytes after the MPEG frame header. The
    // frame-count field starts 14 bytes into that header.
    const long vbri_offset = frame_offset + 4 + 32;
    if (audio_seek(file, vbri_offset, SEEK_SET) != 0) return false;
    uint8_t marker[4];
    uint8_t frames[4];
    if (audio_read(file, marker, sizeof(marker)) != sizeof(marker) ||
        std::memcmp(marker, "VBRI", sizeof(marker)) != 0 ||
        audio_seek(file, vbri_offset + 14, SEEK_SET) != 0 ||
        audio_read(file, frames, sizeof(frames)) != sizeof(frames)) {
        return false;
    }
    *duration_ms = samples_to_milliseconds(
        static_cast<uint64_t>(read_be32(frames)) * frame_info.samples_per_frame,
        frame_info.sample_rate);
    return *duration_ms != 0;
}

bool looks_like_constant_bitrate(FILE *file, long frame_offset,
                                 const Mp3FrameInfo &first_frame)
{
    if (!file || first_frame.bitrate_bps == 0 || audio_seek(file, frame_offset, SEEK_SET) != 0) {
        return false;
    }

    long next_frame = frame_offset;
    for (int i = 0; i < 16; ++i) {
        uint8_t header[4];
        Mp3FrameInfo frame{};
        if (audio_seek(file, next_frame, SEEK_SET) != 0 ||
            audio_read(file, header, sizeof(header)) != sizeof(header) ||
            !parse_mp3_frame_header(header, &frame) ||
            frame.sample_rate != first_frame.sample_rate ||
            frame.bitrate_bps != first_frame.bitrate_bps) {
            return false;
        }
        next_frame += static_cast<long>(frame.frame_length);
    }
    return true;
}

uint32_t read_mp3_duration_ms(FILE *file)
{
    if (!file) return 0;

    uint32_t duration_ms = 0;
    long audio_start = 0;
    if (audio_seek(file, 0, SEEK_SET) != 0) return 0;

    uint8_t id3_header[10];
    if (audio_read(file, id3_header, sizeof(id3_header)) == sizeof(id3_header) &&
        std::memcmp(id3_header, "ID3", 3) == 0) {
        audio_start = 10 + static_cast<long>(read_synchsafe32(id3_header + 6));
        if ((id3_header[5] & 0x10) != 0) audio_start += 10;
    }

    long first_frame_offset = 0;
    Mp3FrameInfo first_frame{};
    if (!find_first_mp3_frame(file, audio_start, &first_frame_offset, &first_frame)) {
        audio_seek(file, 0, SEEK_SET);
        return 0;
    }

    if (read_xing_or_vbri_duration(file, first_frame_offset, first_frame, &duration_ms)) {
        audio_seek(file, 0, SEEK_SET);
        return duration_ms;
    }

    long file_end = -1;
    if (audio_seek(file, 0, SEEK_END) == 0) file_end = std::ftell(file);

    // Most files without a Xing/VBRI header are CBR. Confirm a short run of
    // frame headers, then derive their duration from the file length without
    // walking the entire card-resident file. This avoids a long cold-start
    // delay for otherwise simple MP3s while retaining the exact scan fallback
    // for VBR files that lack metadata.
    if (file_end > first_frame_offset && looks_like_constant_bitrate(
            file, first_frame_offset, first_frame)) {
        const uint64_t audio_bytes = static_cast<uint64_t>(file_end - first_frame_offset);
        duration_ms = static_cast<uint32_t>(std::min<uint64_t>(
            (audio_bytes * 8u * 1000u) / first_frame.bitrate_bps, 0xFFFFFFFFu));
        audio_seek(file, 0, SEEK_SET);
        return duration_ms;
    }

    uint64_t total_samples = 0;
    long frame_offset = first_frame_offset;
    if (audio_seek(file, frame_offset, SEEK_SET) != 0) {
        audio_seek(file, 0, SEEK_SET);
        return 0;
    }
    while (frame_offset >= 0 &&
           (file_end < 0 || frame_offset + 4 <= file_end)) {
        uint8_t header[4];
        Mp3FrameInfo frame{};
        if (audio_read(file, header, sizeof(header)) != sizeof(header) ||
            !parse_mp3_frame_header(header, &frame) ||
            frame.sample_rate != first_frame.sample_rate ||
            frame.frame_length < sizeof(header) ||
            (file_end >= 0 && frame_offset + frame.frame_length > file_end)) {
            break;
        }
        total_samples += frame.samples_per_frame;
        const size_t payload_bytes = frame.frame_length - sizeof(header);
        size_t remaining = payload_bytes;
        while (remaining > 0) {
            const size_t chunk = std::min(remaining, kDurationScanBufferBytes);
            if (audio_read(file, s_duration_scan_buffer, chunk) != chunk) {
                remaining = 0;
                total_samples -= frame.samples_per_frame;
                frame_offset = -1;
                break;
            }
            remaining -= chunk;
        }
        if (frame_offset < 0) break;
        frame_offset = std::ftell(file);
    }

    duration_ms = samples_to_milliseconds(total_samples, first_frame.sample_rate);
    audio_seek(file, 0, SEEK_SET);
    return duration_ms;
}

struct FlacStreamInfo {
    uint32_t sample_rate;
    uint8_t channels;
    uint8_t bits_per_sample;
    uint64_t total_samples;
    uint8_t stream_info_block[34];
    long first_frame_offset;
    uint64_t seek_sample;
    uint64_t seek_offset;
};

constexpr uint64_t kNoFlacSeekSample = ~static_cast<uint64_t>(0);

uint64_t read_be64(const uint8_t *bytes)
{
    return (static_cast<uint64_t>(read_be32(bytes)) << 32) | read_be32(bytes + 4);
}

bool read_flac_stream_info(FILE *file, FlacStreamInfo *info,
                           uint64_t target_samples = kNoFlacSeekSample)
{
    if (!file || !info || audio_seek(file, 0, SEEK_SET) != 0) return false;

    *info = {};
    info->seek_sample = kNoFlacSeekSample;

    uint8_t marker[4];
    if (audio_read(file, marker, sizeof(marker)) != sizeof(marker) ||
        std::memcmp(marker, "fLaC", sizeof(marker)) != 0) {
        return false;
    }

    bool found_stream_info = false;
    bool last_block = false;
    uint32_t block_count = 0;
    while (!last_block && block_count++ < 128) {
        uint8_t block_header[4];
        if (audio_read(file, block_header, sizeof(block_header)) != sizeof(block_header)) {
            return false;
        }
        last_block = (block_header[0] & 0x80u) != 0;
        const uint8_t block_type = block_header[0] & 0x7Fu;
        const uint32_t block_length = read_be24(block_header + 1);

        if (block_type == 0) {
            if (block_length != 34) return false;
            uint8_t stream_info[34];
            if (audio_read(file, stream_info, sizeof(stream_info)) != sizeof(stream_info)) {
                return false;
            }
            std::memcpy(info->stream_info_block, stream_info, sizeof(stream_info));

            const uint64_t packed =
                (static_cast<uint64_t>(stream_info[10]) << 56) |
                (static_cast<uint64_t>(stream_info[11]) << 48) |
                (static_cast<uint64_t>(stream_info[12]) << 40) |
                (static_cast<uint64_t>(stream_info[13]) << 32) |
                (static_cast<uint64_t>(stream_info[14]) << 24) |
                (static_cast<uint64_t>(stream_info[15]) << 16) |
                (static_cast<uint64_t>(stream_info[16]) << 8) |
                static_cast<uint64_t>(stream_info[17]);
            info->sample_rate = static_cast<uint32_t>(packed >> 44);
            info->channels = static_cast<uint8_t>(((packed >> 41) & 0x07u) + 1u);
            info->bits_per_sample = static_cast<uint8_t>(((packed >> 36) & 0x1Fu) + 1u);
            info->total_samples = packed & ((1ULL << 36) - 1ULL);
            found_stream_info = info->sample_rate != 0 && info->channels != 0 &&
                                info->bits_per_sample != 0;
        } else if (block_type == 3 && block_length % 18u == 0 &&
                   target_samples != kNoFlacSeekSample) {
            // A seek table stores sample numbers and byte offsets relative to
            // the first FLAC frame. Keep only the closest point before the
            // requested sample; this avoids allocating for large tables.
            uint8_t seek_point[18];
            for (uint32_t offset = 0; offset < block_length; offset += sizeof(seek_point)) {
                if (audio_read(file, seek_point, sizeof(seek_point)) != sizeof(seek_point)) {
                    return false;
                }
                const uint64_t sample = read_be64(seek_point);
                const uint64_t frame_offset = read_be64(seek_point + 8);
                if (sample == kNoFlacSeekSample || target_samples == kNoFlacSeekSample ||
                    sample > target_samples) {
                    continue;
                }
                if (info->seek_sample == kNoFlacSeekSample || sample > info->seek_sample) {
                    info->seek_sample = sample;
                    info->seek_offset = frame_offset;
                }
            }
        } else {
            // Metadata blocks can include large Vorbis comments or pictures.
            // Seek past them without copying their payload into the realtime
            // audio buffers.
            if (audio_seek(file, static_cast<long>(block_length), SEEK_CUR) != 0) {
                return false;
            }
        }
    }

    if (!found_stream_info || !last_block) return false;
    info->first_frame_offset = std::ftell(file);
    return info->first_frame_offset >= 0;
}

// esp_audio_codec's FLAC parser searches the supplied byte stream for the
// first frame, but gives up after 512 KiB.  A FLAC PICTURE block is legal
// before that frame and routinely exceeds that limit.  The STREAMINFO block
// is all the decoder needs; retain it and present the encoded frames directly
// instead of feeding the parser artwork, comments, padding, or seek tables.
bool prime_flac_decoder_input(AudioReadAhead *input, const FlacStreamInfo &info)
{
    if (!input || !input->buffer || input->capacity < 42 || info.first_frame_offset < 0) {
        return false;
    }

    static constexpr uint8_t kStreamInfoPrefix[] = {
        'f', 'L', 'a', 'C', 0x80, 0x00, 0x00, 0x22,
    };
    static_assert(sizeof(kStreamInfoPrefix) + sizeof(info.stream_info_block) == 42,
                  "A minimal FLAC STREAMINFO stream is 42 bytes");
    std::memcpy(input->buffer, kStreamInfoPrefix, sizeof(kStreamInfoPrefix));
    std::memcpy(input->buffer + sizeof(kStreamInfoPrefix), info.stream_info_block,
                sizeof(info.stream_info_block));
    input->read_index = 0;
    input->write_index = sizeof(kStreamInfoPrefix) + sizeof(info.stream_info_block);
    input->available = input->write_index;
    input->eof = false;
    input->error = false;
    return true;
}

uint32_t pcm_bytes_to_milliseconds(uint64_t pcm_bytes, uint32_t sample_rate,
                                    uint8_t channels, uint8_t bits_per_sample)
{
    if (sample_rate == 0 || channels == 0 || bits_per_sample == 0) return 0;
    const uint64_t bytes_per_second =
        static_cast<uint64_t>(sample_rate) * channels * bits_per_sample / 8u;
    if (bytes_per_second == 0) return 0;
    const uint64_t milliseconds = (pcm_bytes * 1000u) / bytes_per_second;
    return milliseconds > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<uint32_t>(milliseconds);
}

uint32_t read_flac_duration_ms(FILE *file)
{
    FlacStreamInfo info{};
    if (!read_flac_stream_info(file, &info)) {
        audio_seek(file, 0, SEEK_SET);
        return 0;
    }
    const uint32_t duration_ms = samples_to_milliseconds(info.total_samples, info.sample_rate);
    audio_seek(file, 0, SEEK_SET);
    return duration_ms;
}

uint8_t flac_crc8(const uint8_t *bytes, size_t length)
{
    uint8_t crc = 0;
    for (size_t index = 0; index < length; ++index) {
        crc ^= bytes[index];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80u) != 0 ?
                static_cast<uint8_t>((crc << 1) ^ 0x07u) :
                static_cast<uint8_t>(crc << 1);
        }
    }
    return crc;
}

size_t flac_utf8_value_bytes(uint8_t first)
{
    if ((first & 0x80u) == 0) return 1;
    if ((first & 0xE0u) == 0xC0u) return 2;
    if ((first & 0xF0u) == 0xE0u) return 3;
    if ((first & 0xF8u) == 0xF0u) return 4;
    if ((first & 0xFCu) == 0xF8u) return 5;
    if ((first & 0xFEu) == 0xFCu) return 6;
    return 0;
}

bool valid_flac_frame_header(const uint8_t *bytes, size_t length)
{
    if (!bytes || length < 6 || bytes[0] != 0xFF ||
        (bytes[1] & 0xFCu) != 0xF8u || (bytes[2] & 0x0Fu) == 0x0Fu ||
        ((bytes[3] >> 4) & 0x0Fu) > 10 || ((bytes[3] >> 1) & 0x07u) == 3 ||
        (bytes[3] & 0x01u) != 0) {
        return false;
    }

    size_t cursor = 4;
    const size_t number_bytes = flac_utf8_value_bytes(bytes[cursor]);
    if (number_bytes == 0 || cursor + number_bytes >= length) return false;
    for (size_t index = 1; index < number_bytes; ++index) {
        if ((bytes[cursor + index] & 0xC0u) != 0x80u) return false;
    }
    cursor += number_bytes;

    const uint8_t block_size_code = static_cast<uint8_t>((bytes[2] >> 4) & 0x0Fu);
    if (block_size_code == 0 || block_size_code == 15) return false;
    if (block_size_code == 6) {
        ++cursor;
    } else if (block_size_code == 7) {
        cursor += 2;
    }

    const uint8_t sample_rate_code = bytes[2] & 0x0Fu;
    if (sample_rate_code == 12) {
        ++cursor;
    } else if (sample_rate_code == 13 || sample_rate_code == 14) {
        cursor += 2;
    }
    if (cursor >= length) return false;
    return flac_crc8(bytes, cursor) == bytes[cursor];
}

bool find_flac_frame_offset(FILE *file, long estimated_offset, long first_frame_offset,
                            long file_end, uint8_t *scan_buffer, size_t scan_capacity,
                            long *frame_offset)
{
    if (!file || !scan_buffer || scan_capacity == 0 || !frame_offset ||
        first_frame_offset < 0 || file_end <= first_frame_offset) {
        return false;
    }

    const long half_window = static_cast<long>(scan_capacity / 2);
    const long scan_start = std::max(first_frame_offset, estimated_offset - half_window);
    const long scan_end = std::min(file_end, scan_start + static_cast<long>(scan_capacity));
    if (scan_end <= scan_start || audio_seek(file, scan_start, SEEK_SET) != 0) return false;

    size_t bytes_read = 0;
    const size_t wanted = static_cast<size_t>(scan_end - scan_start);
    while (bytes_read < wanted) {
        const size_t count = audio_read(file, scan_buffer + bytes_read, wanted - bytes_read);
        if (count == 0) break;
        bytes_read += count;
    }
    if (bytes_read < 6) return false;

    long before = -1;
    long after = -1;
    for (size_t index = 0; index + 6 <= bytes_read; ++index) {
        if (scan_buffer[index] != 0xFF ||
            !valid_flac_frame_header(scan_buffer + index, bytes_read - index)) {
            continue;
        }
        const long candidate = scan_start + static_cast<long>(index);
        if (candidate <= estimated_offset) {
            before = candidate;
        } else if (after < 0) {
            after = candidate;
        }
    }
    *frame_offset = before >= 0 ? before : after;
    return *frame_offset >= 0;
}

bool seek_mp3_file(FILE *file, uint32_t position_ms, uint32_t duration_ms)
{
    if (!file || duration_ms == 0) return false;
    if (position_ms == 0) return audio_seek(file, 0, SEEK_SET) == 0;

    long file_end = -1;
    if (audio_seek(file, 0, SEEK_END) == 0) file_end = std::ftell(file);
    if (file_end <= 0) return false;

    const uint64_t estimated_offset = static_cast<uint64_t>(file_end) * position_ms / duration_ms;
    const long scan_start = static_cast<long>(std::min<uint64_t>(
        estimated_offset, static_cast<uint64_t>(file_end - 1)));
    long frame_offset = 0;
    Mp3FrameInfo frame{};
    if (!find_first_mp3_frame(file, scan_start, &frame_offset, &frame)) return false;
    return audio_seek(file, frame_offset, SEEK_SET) == 0;
}

bool generation_is_current(uint32_t generation)
{
    bool current;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    current = generation == s_request_generation;
    xSemaphoreGive(s_state_mutex);
    return current;
}

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
        apply_output_processing(output, output->staging_buffer, received);

        size_t total_written = 0;
        while (total_written < received) {
            size_t written = 0;
            const esp_err_t result = i2s_channel_write(
                s_i2s_tx, output->staging_buffer + total_written,
                received - total_written, &written, pdMS_TO_TICKS(250));
            if (result != ESP_OK || written == 0 || written % kI2sFrameBytes != 0) {
                fail_pcm_output(output, result == ESP_OK ?
                                ESP_ERR_INVALID_SIZE : result);
                break;
            }
            total_written += written;
        }
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

    i2s_std_clk_config_t clock_config = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    // Match the vendor speaker demo. MCLK is not routed on this board, but
    // the multiple still determines the generated BCLK accuracy.
    clock_config.mclk_multiple = I2S_MCLK_MULTIPLE_128;
    const esp_err_t clock_ret = i2s_channel_reconfig_std_clock(s_i2s_tx, &clock_config);
    if (clock_ret != ESP_OK) return clock_ret;

    // Leave the channel in READY state. The PCM output task preloads the DMA
    // descriptors and enables I2S only once complete stereo frames are ready.
    return ESP_OK;
}

int16_t pcm_sample_to_i16(const uint8_t *sample, uint8_t bits_per_sample)
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
        // Sign-extend the 24-bit FLAC sample before reducing it to I2S's
        // 16-bit output format.
        if ((value & 0x00800000) != 0) value |= ~0x00FFFFFF;
        return static_cast<int16_t>(value >> 8);
    }

    const uint32_t value = static_cast<uint32_t>(sample[0]) |
                           (static_cast<uint32_t>(sample[1]) << 8) |
                           (static_cast<uint32_t>(sample[2]) << 16) |
                           (static_cast<uint32_t>(sample[3]) << 24);
    return static_cast<int16_t>(static_cast<int32_t>(value) >> 16);
}

int16_t scale_pcm_sample(int16_t sample, int32_t gain_q15)
{
    const int64_t scaled = (static_cast<int64_t>(sample) * gain_q15) >> 15;
    if (scaled > 32767) return 32767;
    if (scaled < -32768) return -32768;
    return static_cast<int16_t>(scaled);
}

void apply_output_gain(uint8_t *pcm, size_t pcm_bytes, int32_t *current_gain_q15)
{
    if (!pcm || !current_gain_q15 || pcm_bytes % kI2sFrameBytes != 0) return;

    uint8_t volume_percent;
    int16_t replay_gain_tenths_db;
    uint8_t transition_gain_percent;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    volume_percent = s_status.volume_percent;
    replay_gain_tenths_db = s_replay_gain_tenths_db;
    transition_gain_percent = s_transition_gain_percent;
    xSemaphoreGive(s_state_mutex);
    const int32_t volume_gain_q15 = static_cast<int32_t>(
        (static_cast<uint32_t>(volume_percent) * kMaximumOutputGainQ15) /
        lyra::audio::kMaximumVolumePercent);
    const float replay_multiplier = std::pow(10.0f,
        static_cast<float>(replay_gain_tenths_db) / 200.0f);
    const int32_t replay_gain_q15 = static_cast<int32_t>(std::lround(
        static_cast<float>(volume_gain_q15) * replay_multiplier));
    const int32_t target_gain_q15 = std::min<int32_t>(kMaximumOutputGainQ15,
        std::max<int32_t>(0, (replay_gain_q15 * transition_gain_percent) / 100));

    auto *samples = reinterpret_cast<int16_t *>(pcm);
    const size_t frame_count = pcm_bytes / kI2sFrameBytes;
    int32_t gain_q15 = *current_gain_q15;
    for (size_t frame = 0; frame < frame_count; ++frame) {
        if (gain_q15 < target_gain_q15) {
            gain_q15 = std::min(gain_q15 + kGainRampStepQ15, target_gain_q15);
        } else if (gain_q15 > target_gain_q15) {
            gain_q15 = std::max(gain_q15 - kGainRampStepQ15, target_gain_q15);
        }
        samples[frame * 2] = scale_pcm_sample(samples[frame * 2], gain_q15);
        samples[frame * 2 + 1] = scale_pcm_sample(samples[frame * 2 + 1], gain_q15);
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

void configure_equalizer(PcmOutput *output)
{
    if (!output) return;
    lyra::audio::EqualizerSettings settings{};
    uint32_t generation = 0;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    settings = s_equalizer;
    generation = s_equalizer_generation;
    xSemaphoreGive(s_state_mutex);
    if (output->equalizer_initialized && output->equalizer_generation == generation) return;

    const bool immediate = !output->equalizer_initialized;
    for (size_t band = 0; band < lyra::audio::kEqualizerBandCount; ++band) {
        set_equalizer_target(&output->equalizer[band], output->sample_rate,
                             kEqualizerCenterFrequenciesHz[band],
                             settings.band_tenths_db[band], immediate);
    }
    output->equalizer_generation = generation;
    output->equalizer_initialized = true;
}

void apply_equalizer(PcmOutput *output, uint8_t *pcm, size_t pcm_bytes)
{
    if (!output || !pcm || pcm_bytes % kI2sFrameBytes != 0) return;
    configure_equalizer(output);

    auto *samples = reinterpret_cast<int16_t *>(pcm);
    const size_t frame_count = pcm_bytes / kI2sFrameBytes;
    for (size_t frame = 0; frame < frame_count; ++frame) {
        for (size_t band = 0; band < lyra::audio::kEqualizerBandCount; ++band) {
            update_equalizer_coefficients(&output->equalizer[band]);
        }
        float left = static_cast<float>(samples[frame * 2]) / 32768.0f;
        float right = static_cast<float>(samples[frame * 2 + 1]) / 32768.0f;
        for (size_t band = 0; band < lyra::audio::kEqualizerBandCount; ++band) {
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

void apply_output_processing(PcmOutput *output, uint8_t *pcm, size_t pcm_bytes)
{
    if (!output) return;
    // Apply the normal output cap first so a +6 dB EQ band retains useful
    // headroom instead of clipping before the speaker safety gain is applied.
    apply_output_gain(pcm, pcm_bytes, &output->gain_q15);
    apply_equalizer(output, pcm, pcm_bytes);
}

esp_err_t write_pcm(const uint8_t *pcm, size_t pcm_bytes, uint8_t channels,
                    uint8_t bits_per_sample,
                    int16_t *stereo_buffer, size_t stereo_buffer_bytes,
                    PcmOutput *output,
                    uint32_t generation, bool unsigned_8bit = false)
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
                left = right = pcm_sample_to_i16(frame, bits_per_sample);
            } else if (channels == 2) {
                left = pcm_sample_to_i16(frame, bits_per_sample);
                right = pcm_sample_to_i16(frame + bytes_per_sample, bits_per_sample);
            } else {
                // The I2S path is stereo. Preserve the energy of multichannel
                // FLACs by averaging all decoded channels into a centered mono
                // signal instead of rejecting otherwise valid 5.1/7.1 files.
                int32_t sum = 0;
                for (uint8_t channel = 0; channel < channels; ++channel) {
                    sum += pcm_sample_to_i16(frame + channel * bytes_per_sample,
                                             bits_per_sample);
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

esp_err_t play_file(const char *path, uint32_t generation, uint32_t start_position_ms,
                    bool start_paused, bool pause_after_seek)
{
    AudioFormat format{};
    if (!get_audio_format(path, &format)) return ESP_ERR_NOT_SUPPORTED;

    FILE *file = lyra::sd::open(path, "rb", lyra::sd::Client::Audio);
    if (file == nullptr) {
        ESP_LOGE(kTag, "failed to open %s: %s", audio_format_name(format), path);
        set_error(ESP_ERR_NOT_FOUND, path);
        return ESP_ERR_NOT_FOUND;
    }

    OggOpusReader ogg_opus_reader(file);
    if (format == AudioFormat::kOgg && ogg_opus_reader.is_opus_stream()) {
        format = AudioFormat::kOpus;
    } else if (format == AudioFormat::kOgg) {
        const char *extension = std::strrchr(path, '.');
        if (extension && extension_is(extension + 1, "opus")) {
            // A .opus file must be an Ogg Opus stream here. Raw Opus has no
            // packet boundaries or stream configuration, so passing it through
            // the OGG parser makes the codec receive the whole file as one
            // packet (and produces misleading decoder-length errors).
            ESP_LOGE(kTag, "unsupported raw or corrupt OPUS stream: %s", path);
            lyra::sd::close(file, lyra::sd::Client::Audio);
            set_error(ESP_ERR_NOT_SUPPORTED, path);
            return ESP_ERR_NOT_SUPPORTED;
        }
    }

    long file_end = -1;
    if (audio_seek(file, 0, SEEK_END) == 0) file_end = std::ftell(file);
    audio_seek(file, 0, SEEK_SET);

    AiffStreamInfo aiff_info{};
    if (format == AudioFormat::kAiff && !read_aiff_stream_info(file, &aiff_info)) {
        ESP_LOGE(kTag, "unsupported or corrupt AIFF PCM stream: %s", path);
        lyra::sd::close(file, lyra::sd::Client::Audio);
        set_error(ESP_ERR_NOT_SUPPORTED, path);
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint32_t duration_ms = 0;
    switch (format) {
    case AudioFormat::kMp3: duration_ms = read_mp3_duration_ms(file); break;
    case AudioFormat::kFlac: duration_ms = read_flac_duration_ms(file); break;
    case AudioFormat::kAac: duration_ms = read_aac_duration_ms(file); break;
    case AudioFormat::kM4a: duration_ms = read_m4a_duration_ms(file); break;
    case AudioFormat::kWav: duration_ms = read_wav_duration_ms(file); break;
    case AudioFormat::kOgg: duration_ms = read_ogg_duration_ms(file); break;
    case AudioFormat::kOpus: duration_ms = read_ogg_tail_duration_ms(
        file, ogg_opus_reader.sample_rate, ogg_opus_reader.pre_skip); break;
    case AudioFormat::kAiff: duration_ms = read_aiff_duration_ms(aiff_info); break;
    }
    audio_seek(file, 0, SEEK_SET);

    if (duration_ms > 0 && start_position_ms > duration_ms) start_position_ms = duration_ms;
    M4aAacReader m4a_aac_reader(file);
    bool m4a_frame_decoder = false;
    uint64_t m4a_seek_base_samples = 0;
    if (format == AudioFormat::kM4a && start_position_ms > 0) {
        m4a_frame_decoder = m4a_aac_reader.prepare_seek(
            start_position_ms, &m4a_seek_base_samples);
        if (!m4a_frame_decoder) audio_seek(file, 0, SEEK_SET);
    }

    esp_audio_simple_dec_handle_t decoder = nullptr;
    esp_audio_dec_handle_t opus_decoder = nullptr;
    esp_audio_dec_handle_t aac_decoder = nullptr;
    union SimpleDecoderConfig {
        esp_aac_dec_cfg_t aac;
        esp_m4a_dec_cfg_t m4a;
        esp_opus_dec_cfg_t opus;
    } simple_decoder_config{};
    if (format != AudioFormat::kAiff) {
        esp_audio_err_t decoder_ret = ESP_AUDIO_ERR_OK;
        if (m4a_frame_decoder) {
            simple_decoder_config.aac.sample_rate =
                static_cast<int32_t>(m4a_aac_reader.sample_rate);
            simple_decoder_config.aac.channel = m4a_aac_reader.channels;
            simple_decoder_config.aac.bits_per_sample = 16;
            simple_decoder_config.aac.no_adts_header = true;
            simple_decoder_config.aac.aac_plus_enable = true;
            decoder_ret = esp_aac_dec_open(&simple_decoder_config.aac,
                                           sizeof(simple_decoder_config.aac),
                                           &aac_decoder);
        } else if (format == AudioFormat::kOpus) {
            // Ogg pages have already been packetized by OggOpusReader.  The
            // simple decoder has no RAW_OPUS parser, so call the Opus frame
            // decoder directly with exactly one complete packet.
            simple_decoder_config.opus.sample_rate = ogg_opus_reader.sample_rate;
            simple_decoder_config.opus.channel = ogg_opus_reader.channels;
            simple_decoder_config.opus.frame_duration =
                ESP_OPUS_DEC_FRAME_DURATION_20_MS;
            simple_decoder_config.opus.self_delimited = false;
            decoder_ret = esp_opus_dec_open(&simple_decoder_config.opus,
                                            sizeof(simple_decoder_config.opus),
                                            &opus_decoder);
        } else {
            esp_audio_simple_dec_cfg_t decoder_config{};
            decoder_config.dec_type = decoder_type_for_format(format);
            decoder_config.use_frame_dec = false;
            if (format == AudioFormat::kAac) {
                simple_decoder_config.aac.aac_plus_enable = true;
                decoder_config.dec_cfg = &simple_decoder_config.aac;
                decoder_config.cfg_size = sizeof(simple_decoder_config.aac);
            } else if (format == AudioFormat::kM4a) {
                simple_decoder_config.m4a.aac_plus_enable = true;
                decoder_config.dec_cfg = &simple_decoder_config.m4a;
                decoder_config.cfg_size = sizeof(simple_decoder_config.m4a);
            }
            decoder_ret = esp_audio_simple_dec_open(&decoder_config, &decoder);
        }
        if (decoder_ret != ESP_AUDIO_ERR_OK ||
            (format == AudioFormat::kOpus ? opus_decoder == nullptr :
             m4a_frame_decoder ? aac_decoder == nullptr : decoder == nullptr)) {
            ESP_LOGE(kTag, "failed to open %s decoder: %d", audio_format_name(format),
                     static_cast<int>(decoder_ret));
            if (decoder) esp_audio_simple_dec_close(decoder);
            if (opus_decoder) esp_opus_dec_close(opus_decoder);
            if (aac_decoder) esp_aac_dec_close(aac_decoder);
            lyra::sd::close(file, lyra::sd::Client::Audio);
            set_error(ESP_FAIL, path);
            return ESP_FAIL;
        }
    }

    // The read-ahead ring is CPU-only; keep the smaller internal/DMA heap
    // available for the I2S staging buffer and the LCD transport allocator.
    auto *read_ahead_buffer = static_cast<uint8_t *>(heap_caps_malloc(
        kReadAheadBufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (read_ahead_buffer == nullptr) {
        read_ahead_buffer = static_cast<uint8_t *>(heap_caps_malloc(
            kReadAheadBufferBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    auto *pcm = format == AudioFormat::kAiff ? nullptr :
        static_cast<uint8_t *>(alloc_audio_buffer(kInitialPcmBufferBytes));
    auto *stereo = static_cast<int16_t *>(alloc_audio_dma_buffer(kStereoBufferBytes));
    size_t pcm_capacity = kInitialPcmBufferBytes;
    if (read_ahead_buffer == nullptr || (format != AudioFormat::kAiff && pcm == nullptr) ||
        stereo == nullptr) {
        ESP_LOGE(kTag, "not enough memory for %s buffers", audio_format_name(format));
        heap_caps_free(read_ahead_buffer);
        heap_caps_free(pcm);
        heap_caps_free(stereo);
        if (decoder) esp_audio_simple_dec_close(decoder);
        if (opus_decoder) esp_opus_dec_close(opus_decoder);
        if (aac_decoder) esp_aac_dec_close(aac_decoder);
        lyra::sd::close(file, lyra::sd::Client::Audio);
        set_error(ESP_ERR_NO_MEM, path);
        return ESP_ERR_NO_MEM;
    }

    const size_t read_ahead_chunk_bytes = format == AudioFormat::kOpus || m4a_frame_decoder ? 1 :
        format == AudioFormat::kM4a ? kM4aReadAheadChunkBytes :
        format == AudioFormat::kFlac ? kFlacReadAheadChunkBytes : kReadAheadChunkBytes;
    AudioReadAhead input{};
    input.file = file;
    input.buffer = read_ahead_buffer;
    input.capacity = kReadAheadBufferBytes;
    input.chunk_bytes = read_ahead_chunk_bytes;
    if (format == AudioFormat::kOpus) input.ogg_opus = &ogg_opus_reader;
    if (m4a_frame_decoder) input.m4a_aac = &m4a_aac_reader;
    if (format == AudioFormat::kFlac) {
        FlacStreamInfo stream_info{};
        if (read_flac_stream_info(file, &stream_info) &&
            prime_flac_decoder_input(&input, stream_info)) {
            // read_flac_stream_info leaves the file immediately before the
            // first audio frame, so the normal read-ahead loop now appends
            // frames to the synthetic, minimal FLAC header above.
            ESP_LOGD(kTag, "FLAC metadata skipped (%ld bytes before first frame)",
                     stream_info.first_frame_offset);
        } else {
            // Preserve the decoder's existing error path for malformed FLAC
            // files rather than attempting playback from an unknown offset.
            audio_seek(file, 0, SEEK_SET);
        }
    }
    if (format == AudioFormat::kAiff) {
        const size_t bytes_per_sample = aiff_info.bits_per_sample / 8u;
        const size_t bytes_per_frame = bytes_per_sample * aiff_info.channels;
        if (bytes_per_frame == 0 || audio_seek(file, aiff_info.data_start, SEEK_SET) != 0) {
            heap_caps_free(read_ahead_buffer);
            heap_caps_free(stereo);
            lyra::sd::close(file, lyra::sd::Client::Audio);
            set_error(ESP_ERR_INVALID_SIZE, path);
            return ESP_ERR_INVALID_SIZE;
        }
        input.bounded_source = true;
        input.source_little_endian = aiff_info.little_endian;
        input.source_bytes_per_sample = static_cast<uint8_t>(bytes_per_sample);
        input.source_channels = aiff_info.channels;
        input.source_remaining = aiff_info.data_bytes;
    }
    PcmOutput pcm_sink{};

    uint64_t discard_pcm_bytes = 0;
    bool replay_seek_pending = false;
    uint32_t replay_seek_position_ms = 0;
    uint32_t pcm_position_base_ms = start_position_ms;
    const uint64_t opus_pre_skip_bytes = format == AudioFormat::kOpus ?
        static_cast<uint64_t>(ogg_opus_reader.pre_skip) * ogg_opus_reader.channels *
        sizeof(int16_t) : 0;
    discard_pcm_bytes = opus_pre_skip_bytes;
    if (m4a_frame_decoder) {
        const uint64_t target_samples =
            (static_cast<uint64_t>(start_position_ms) * m4a_aac_reader.sample_rate) / 1000u;
        const uint64_t discard_samples = target_samples > m4a_seek_base_samples ?
            target_samples - m4a_seek_base_samples : 0;
        discard_pcm_bytes = discard_samples * m4a_aac_reader.channels * sizeof(int16_t);
        const uint64_t base_ms = (m4a_seek_base_samples * 1000u +
                                  m4a_aac_reader.sample_rate / 2u) /
                                 m4a_aac_reader.sample_rate;
        pcm_position_base_ms = base_ms > 0xFFFFFFFFu ? 0xFFFFFFFFu :
            static_cast<uint32_t>(base_ms);
        ESP_LOGI(kTag, "M4A seek %u ms using AAC sample table (base=%u ms discard=%llu bytes)",
                 static_cast<unsigned>(start_position_ms),
                 static_cast<unsigned>(pcm_position_base_ms),
                 static_cast<unsigned long long>(discard_pcm_bytes));
    }
    if (start_position_ms > 0) {
        if (format == AudioFormat::kMp3) {
            if (!seek_mp3_file(file, start_position_ms, duration_ms)) {
                audio_seek(file, 0, SEEK_SET);
                replay_seek_pending = true;
                replay_seek_position_ms = start_position_ms;
            }
        } else if (format == AudioFormat::kFlac) {
            // The simple FLAC decoder is a non-frame decoder, so provide it
            // with a minimal STREAMINFO header followed by a real frame near
            // the target. This avoids replaying the entire file for every seek.
            FlacStreamInfo stream_info{};
            if (read_flac_stream_info(file, &stream_info) && stream_info.sample_rate != 0 &&
                stream_info.channels != 0 &&
                (stream_info.bits_per_sample == 16 || stream_info.bits_per_sample == 24 ||
                 stream_info.bits_per_sample == 32)) {
                const uint64_t target_samples = std::min<uint64_t>(
                    stream_info.total_samples,
                    (static_cast<uint64_t>(start_position_ms) * stream_info.sample_rate) / 1000u);
                const uint8_t bytes_per_sample = stream_info.bits_per_sample / 8;
                long file_end = -1;
                long seek_frame = -1;
                uint64_t seek_base_samples = 0;
                bool have_fast_seek = false;

                if (audio_seek(file, 0, SEEK_END) == 0) file_end = std::ftell(file);
                if (stream_info.seek_sample != kNoFlacSeekSample && file_end > 0 &&
                    stream_info.seek_offset <= static_cast<uint64_t>(
                        file_end - stream_info.first_frame_offset)) {
                    seek_frame = stream_info.first_frame_offset +
                                 static_cast<long>(stream_info.seek_offset);
                    seek_base_samples = stream_info.seek_sample;
                    have_fast_seek = true;
                } else if (file_end > stream_info.first_frame_offset &&
                           stream_info.total_samples > 0) {
                    const uint64_t encoded_bytes = static_cast<uint64_t>(
                        file_end - stream_info.first_frame_offset);
                    const uint64_t estimated_delta = static_cast<uint64_t>(
                        (static_cast<double>(encoded_bytes) * target_samples) /
                        stream_info.total_samples);
                    const long estimated_frame = stream_info.first_frame_offset +
                        static_cast<long>(std::min<uint64_t>(estimated_delta, encoded_bytes));
                    have_fast_seek = find_flac_frame_offset(
                        file, estimated_frame, stream_info.first_frame_offset, file_end,
                        read_ahead_buffer, kReadAheadBufferBytes, &seek_frame);
                    // Without a seek table the frame scanner gives us a
                    // close encoded position, but not its exact sample number.
                    // Starting at that frame is preferable to replaying minutes
                    // of audio; the result is within one FLAC frame of target.
                    seek_base_samples = target_samples;
                }

                if (have_fast_seek && seek_frame >= stream_info.first_frame_offset &&
                    file_end > seek_frame && audio_seek(file, seek_frame, SEEK_SET) == 0 &&
                    prime_flac_decoder_input(&input, stream_info)) {
                    if (seek_base_samples < target_samples) {
                        discard_pcm_bytes = (target_samples - seek_base_samples) *
                                            stream_info.channels * bytes_per_sample;
                    }
                    ESP_LOGI(kTag, "FLAC seek %u ms using frame at %ld (discard=%llu bytes)",
                             static_cast<unsigned>(start_position_ms), seek_frame,
                             static_cast<unsigned long long>(discard_pcm_bytes));
                } else {
                    // Fallback for files with no usable seek point. This
                    // replays encoded frames from the beginning. Keep the
                    // synthetic STREAMINFO header so large metadata blocks do
                    // not re-enter the parser during that replay.
                    discard_pcm_bytes = target_samples * stream_info.channels *
                                        bytes_per_sample;
                    if (prime_flac_decoder_input(&input, stream_info) &&
                        audio_seek(file, stream_info.first_frame_offset, SEEK_SET) == 0) {
                        ESP_LOGI(kTag, "FLAC seek %u ms replaying from first frame",
                                 static_cast<unsigned>(start_position_ms));
                    } else {
                        start_position_ms = 0;
                        audio_seek(file, 0, SEEK_SET);
                    }
                }
            } else {
                start_position_ms = 0;
            }
        } else if (format == AudioFormat::kAiff) {
            const uint64_t target_frames = std::min<uint64_t>(
                aiff_info.data_bytes / (static_cast<size_t>(aiff_info.channels) *
                                        (aiff_info.bits_per_sample / 8u)),
                (static_cast<uint64_t>(start_position_ms) * aiff_info.sample_rate) / 1000u);
            const uint64_t target_bytes = target_frames * aiff_info.channels *
                                          (aiff_info.bits_per_sample / 8u);
            if (audio_seek(file, aiff_info.data_start + static_cast<long>(target_bytes),
                           SEEK_SET) == 0) {
                input.read_index = 0;
                input.write_index = 0;
                input.available = 0;
                input.eof = false;
                input.error = false;
                input.source_remaining = aiff_info.data_bytes - target_bytes;
                input.eof = input.source_remaining == 0;
            } else {
                start_position_ms = 0;
                audio_seek(file, aiff_info.data_start, SEEK_SET);
                input.source_remaining = aiff_info.data_bytes;
                input.eof = false;
            }
        } else if (format == AudioFormat::kAac) {
            if (!seek_aac_file(file, start_position_ms, duration_ms)) {
                audio_seek(file, 0, SEEK_SET);
                replay_seek_pending = true;
                replay_seek_position_ms = start_position_ms;
            }
        } else if (format == AudioFormat::kOpus) {
            const uint64_t target_samples =
                (static_cast<uint64_t>(start_position_ms) * ogg_opus_reader.sample_rate) /
                1000u + ogg_opus_reader.pre_skip;
            uint64_t base_samples = 0;
            if (ogg_opus_reader.seek_to_position(start_position_ms, duration_ms, &base_samples) &&
                target_samples >= base_samples) {
                const uint64_t base_playable_samples = base_samples > ogg_opus_reader.pre_skip ?
                    base_samples - ogg_opus_reader.pre_skip : 0;
                const uint64_t base_ms = (base_playable_samples * 1000u +
                                          ogg_opus_reader.sample_rate / 2u) /
                                         ogg_opus_reader.sample_rate;
                pcm_position_base_ms = base_ms > 0xFFFFFFFFu ? 0xFFFFFFFFu :
                    static_cast<uint32_t>(base_ms);
                discard_pcm_bytes = (target_samples - base_samples) *
                                    ogg_opus_reader.channels * sizeof(int16_t);
                ESP_LOGI(kTag, "OPUS seek %u ms using OGG page at %llu samples "
                         "(base=%u ms discard=%llu bytes)",
                         static_cast<unsigned>(start_position_ms),
                         static_cast<unsigned long long>(base_samples),
                         static_cast<unsigned>(pcm_position_base_ms),
                         static_cast<unsigned long long>(discard_pcm_bytes));
            } else {
                ogg_opus_reader.reset();
                audio_seek(file, 0, SEEK_SET);
                replay_seek_pending = true;
                replay_seek_position_ms = start_position_ms;
            }
        } else if ((format == AudioFormat::kM4a && !m4a_frame_decoder) ||
                   format == AudioFormat::kWav ||
                   format == AudioFormat::kOgg) {
            // Espressif's simple decoders are streaming parsers and do not
            // expose a seek API. Restarting the parser and discarding decoded
            // PCM keeps seeking correct for containers whose headers must be
            // replayed from byte zero.
            audio_seek(file, 0, SEEK_SET);
            replay_seek_pending = true;
            replay_seek_position_ms = start_position_ms;
        }
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_diagnostics = {};
    s_diagnostics.audio_buffer_low_watermark = input.capacity;
    s_diagnostics.pcm_buffer_low_watermark = kPcmOutputBufferBytes;
    s_diagnostics.read_ahead_internal = buffer_is_internal(input.buffer);
    s_diagnostics.pcm_internal = buffer_is_internal(pcm);
    s_diagnostics.stereo_internal = buffer_is_internal(stereo);
    s_status.playing = true;
    s_status.paused = start_paused;
    s_status.eof = false;
    s_status.last_error = ESP_OK;
    s_status.sample_rate = 0;
    s_status.channels = 0;
    s_status.bits_per_sample = 0;
    s_status.decoded_bytes = 0;
    s_status.position_ms = start_position_ms;
    s_status.duration_ms = duration_ms;
    std::strncpy(s_status.path, path, sizeof(s_status.path) - 1);
    s_status.path[sizeof(s_status.path) - 1] = '\0';
    xSemaphoreGive(s_state_mutex);

    bool have_audio_info = false;
    esp_err_t result = ESP_OK;
    uint32_t no_progress_count = 0;
    uint32_t replay_yield_count = 0;
    bool pause_after_seek_pending = pause_after_seek;

    while (generation_is_current(generation)) {
        if (!read_pause_state(generation)) break;

        // Keep the ring mostly full while the decoder/I2S path is active.
        // Every iteration is still one bounded SD transaction, so artwork
        // gets opportunities between audio refills but cannot run ahead of it.
        while (!input.eof &&
               input.available < input.capacity - input.chunk_bytes) {
            const size_t before = input.available;
            if (!input.fill()) {
                if (input.error) {
                    result = ESP_FAIL;
                    ESP_LOGE(kTag, "read error while playing %s", path);
                    break;
                }
                break;
            }
            if (input.available == before) break;
        }
        if (result != ESP_OK) break;
        if (input.available == 0 && input.eof) break;
        if (input.available == 0) {
            ++s_diagnostics.underrun_count;
            vTaskDelay(1);
            continue;
        }

        esp_audio_simple_dec_raw_t raw{};
        raw.buffer = input.buffer + input.read_index;
        raw.len = static_cast<uint32_t>(format == AudioFormat::kOpus || m4a_frame_decoder ?
            input.contiguous() : std::min(input.contiguous(), kSimpleDecoderInputChunkBytes));
        raw.eos = input.eof && input.available == input.contiguous() &&
                  input.available <= kSimpleDecoderInputChunkBytes;

        while (raw.len > 0 && generation_is_current(generation)) {
            if (format == AudioFormat::kAiff) {
                const size_t bytes_per_frame = static_cast<size_t>(aiff_info.channels) *
                                               (aiff_info.bits_per_sample / 8u);
                size_t pcm_bytes = raw.len;
                pcm_bytes -= pcm_bytes % bytes_per_frame;
                if (pcm_bytes == 0) break;

                if (!have_audio_info) {
                    result = configure_i2s(aiff_info.sample_rate);
                    pcm_sink.sample_rate = aiff_info.sample_rate;
                    if (result != ESP_OK || !start_pcm_output(&pcm_sink)) {
                        ESP_LOGE(kTag, "AIFF I2S output setup failed for %s", path);
                        result = result == ESP_OK ? ESP_ERR_NO_MEM : result;
                        break;
                    }
                    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                    s_status.sample_rate = aiff_info.sample_rate;
                    s_status.channels = aiff_info.channels;
                    s_status.bits_per_sample = aiff_info.bits_per_sample;
                    xSemaphoreGive(s_state_mutex);
                    ESP_LOGI(kTag, "playing %s (%u Hz, %u channel, %u-bit)", path,
                             static_cast<unsigned>(aiff_info.sample_rate),
                             static_cast<unsigned>(aiff_info.channels),
                             static_cast<unsigned>(aiff_info.bits_per_sample));
                    have_audio_info = true;
                }

                result = write_pcm(raw.buffer, pcm_bytes, aiff_info.channels,
                                   aiff_info.bits_per_sample, stereo, kStereoBufferBytes,
                                   &pcm_sink, generation);
                if (result != ESP_OK) break;
                input.consume(pcm_bytes);
                raw.buffer += pcm_bytes;
                raw.len -= static_cast<uint32_t>(pcm_bytes);
                xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                s_status.decoded_bytes += pcm_bytes;
                const uint32_t decoded_position_ms = pcm_bytes_to_milliseconds(
                    s_status.decoded_bytes, s_status.sample_rate, s_status.channels,
                    s_status.bits_per_sample);
                const uint64_t absolute_position_ms =
                    static_cast<uint64_t>(pcm_position_base_ms) + decoded_position_ms;
                s_status.position_ms = absolute_position_ms > 0xFFFFFFFFu ?
                    0xFFFFFFFFu : static_cast<uint32_t>(absolute_position_ms);
                if (s_status.duration_ms > 0 && s_status.position_ms > s_status.duration_ms) {
                    s_status.position_ms = s_status.duration_ms;
                }
                xSemaphoreGive(s_state_mutex);
                if (pause_after_seek_pending) {
                    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                    s_status.paused = true;
                    xSemaphoreGive(s_state_mutex);
                    pause_after_seek_pending = false;
                    break;
                }
                no_progress_count = 0;
                continue;
            }

            esp_audio_simple_dec_out_t frame{};
            frame.buffer = pcm;
            frame.len = static_cast<uint32_t>(pcm_capacity);
            esp_audio_dec_info_t direct_info{};
            esp_audio_err_t decode_ret = ESP_AUDIO_ERR_OK;
            if (format == AudioFormat::kOpus) {
                esp_audio_dec_in_raw_t opus_raw{};
                opus_raw.buffer = raw.buffer;
                opus_raw.len = raw.len;
                esp_audio_dec_out_frame_t opus_frame{};
                opus_frame.buffer = pcm;
                opus_frame.len = static_cast<uint32_t>(pcm_capacity);
                decode_ret = esp_opus_dec_decode(opus_decoder, &opus_raw,
                                                 &opus_frame, &direct_info);
                raw.consumed = opus_raw.consumed;
                frame.needed_size = opus_frame.needed_size;
                frame.decoded_size = opus_frame.decoded_size;
            } else if (m4a_frame_decoder) {
                esp_audio_dec_in_raw_t aac_raw{};
                aac_raw.buffer = raw.buffer;
                aac_raw.len = raw.len;
                esp_audio_dec_out_frame_t aac_frame{};
                aac_frame.buffer = pcm;
                aac_frame.len = static_cast<uint32_t>(pcm_capacity);
                decode_ret = esp_aac_dec_decode(aac_decoder, &aac_raw,
                                                &aac_frame, &direct_info);
                raw.consumed = aac_raw.consumed;
                frame.needed_size = aac_frame.needed_size;
                frame.decoded_size = aac_frame.decoded_size;
            } else {
                decode_ret = esp_audio_simple_dec_process(decoder, &raw, &frame);
            }
            if (decode_ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH && frame.needed_size > pcm_capacity &&
                frame.needed_size <= kMaximumPcmBufferBytes) {
                auto *larger = static_cast<uint8_t *>(alloc_audio_buffer(frame.needed_size));
                if (larger == nullptr) {
                    result = ESP_ERR_NO_MEM;
                    break;
                }
                heap_caps_free(pcm);
                pcm = larger;
                pcm_capacity = frame.needed_size;
                s_diagnostics.pcm_internal = buffer_is_internal(pcm);
                continue;
            }
            if (decode_ret != ESP_AUDIO_ERR_OK) {
                ESP_LOGE(kTag, "%s decode error %d in %s", audio_format_name(format),
                         static_cast<int>(decode_ret), path);
                result = ESP_FAIL;
                break;
            }

            if (raw.consumed > raw.len) {
                result = ESP_FAIL;
                ESP_LOGE(kTag, "%s decoder consumed beyond input buffer",
                         audio_format_name(format));
                break;
            }

            if (raw.consumed > 0) input.consume(raw.consumed);

            if (frame.decoded_size > 0) {
                esp_audio_simple_dec_info_t info{};
                if (!have_audio_info) {
                    esp_audio_err_t info_ret = ESP_AUDIO_ERR_OK;
                    if (format == AudioFormat::kOpus || m4a_frame_decoder) {
                        info.sample_rate = direct_info.sample_rate;
                        info.bits_per_sample = direct_info.bits_per_sample;
                        info.channel = direct_info.channel;
                        info.bitrate = direct_info.bitrate;
                        info.frame_size = direct_info.frame_size;
                        if (info.sample_rate == 0 || info.channel == 0) {
                            info_ret = ESP_AUDIO_ERR_NOT_FOUND;
                        }
                    } else {
                        info_ret = esp_audio_simple_dec_get_info(decoder, &info);
                    }
                    const bool supported_bits = info.bits_per_sample == 8 ||
                                                 info.bits_per_sample == 16 ||
                                                 info.bits_per_sample == 24 ||
                                                 info.bits_per_sample == 32;
                    if (info_ret != ESP_AUDIO_ERR_OK || !supported_bits ||
                        info.channel == 0 || info.channel > 8) {
                        ESP_LOGE(kTag, "unsupported %s PCM format in %s: ret=%d rate=%u "
                                 "channels=%u bits=%u frame=%u",
                                 audio_format_name(format), path, static_cast<int>(info_ret),
                                 static_cast<unsigned>(info.sample_rate),
                                 static_cast<unsigned>(info.channel),
                                 static_cast<unsigned>(info.bits_per_sample),
                                 static_cast<unsigned>(info.frame_size));
                        result = ESP_ERR_NOT_SUPPORTED;
                        break;
                    }
                    result = configure_i2s(info.sample_rate);
                    if (result != ESP_OK) {
                        ESP_LOGE(kTag, "I2S setup failed: %s", esp_err_to_name(result));
                        break;
                    }
                    pcm_sink.sample_rate = info.sample_rate;
                    if (!start_pcm_output(&pcm_sink)) {
                        ESP_LOGE(kTag, "I2S PCM output buffer allocation failed");
                        result = ESP_ERR_NO_MEM;
                        break;
                    }
                    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                    s_status.sample_rate = info.sample_rate;
                    s_status.channels = info.channel;
                    s_status.bits_per_sample = info.bits_per_sample;
                    if (duration_ms == 0 && file_end > 0 && info.bitrate > 0) {
                        const uint64_t estimated_duration =
                            (static_cast<uint64_t>(file_end) * 8u * 1000u) / info.bitrate;
                        duration_ms = estimated_duration > 0xFFFFFFFFu ? 0xFFFFFFFFu :
                            static_cast<uint32_t>(estimated_duration);
                        s_status.duration_ms = duration_ms;
                    }
                    xSemaphoreGive(s_state_mutex);
                    if (replay_seek_pending) {
                        uint64_t target_samples =
                            (static_cast<uint64_t>(replay_seek_position_ms) * info.sample_rate) / 1000u;
                        if (format == AudioFormat::kOpus) target_samples += ogg_opus_reader.pre_skip;
                        discard_pcm_bytes = target_samples * info.channel *
                                            (info.bits_per_sample / 8u);
                        replay_seek_pending = false;
                        ESP_LOGI(kTag, "%s seek %u ms replaying and discarding %llu bytes",
                                 audio_format_name(format),
                                 static_cast<unsigned>(replay_seek_position_ms),
                                 static_cast<unsigned long long>(discard_pcm_bytes));
                    }
                    ESP_LOGI(kTag, "playing %s (%u Hz, %u channel, %u-bit)", path,
                             static_cast<unsigned>(info.sample_rate),
                             static_cast<unsigned>(info.channel),
                             static_cast<unsigned>(info.bits_per_sample));
                    have_audio_info = true;
                } else {
                    if (format == AudioFormat::kOpus || m4a_frame_decoder) {
                        info.sample_rate = direct_info.sample_rate;
                        info.bits_per_sample = direct_info.bits_per_sample;
                        info.channel = direct_info.channel;
                        info.bitrate = direct_info.bitrate;
                        info.frame_size = direct_info.frame_size;
                    } else {
                        if (esp_audio_simple_dec_get_info(decoder, &info) != ESP_AUDIO_ERR_OK) {
                            result = ESP_FAIL;
                            break;
                        }
                    }
                }

                const uint8_t *pcm_output = frame.buffer;
                size_t pcm_output_bytes = frame.decoded_size;
                if (discard_pcm_bytes > 0) {
                    const size_t discard = static_cast<size_t>(std::min<uint64_t>(
                        discard_pcm_bytes, pcm_output_bytes));
                    discard_pcm_bytes -= discard;
                    pcm_output += discard;
                    pcm_output_bytes -= discard;
                }
                if (pcm_output_bytes > 0) {
                    result = write_pcm(pcm_output, pcm_output_bytes, info.channel,
                                       info.bits_per_sample,
                                       stereo, kStereoBufferBytes,
                                       &pcm_sink, generation, format == AudioFormat::kWav);
                    if (result != ESP_OK) break;
                    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                    s_status.decoded_bytes += pcm_output_bytes;
                    const uint32_t decoded_position_ms = pcm_bytes_to_milliseconds(
                        s_status.decoded_bytes, s_status.sample_rate, s_status.channels,
                        s_status.bits_per_sample);
                    const uint64_t absolute_position_ms =
                        static_cast<uint64_t>(pcm_position_base_ms) + decoded_position_ms;
                    s_status.position_ms = absolute_position_ms > 0xFFFFFFFFu ?
                        0xFFFFFFFFu : static_cast<uint32_t>(absolute_position_ms);
                    if (s_status.duration_ms > 0 && s_status.position_ms > s_status.duration_ms) {
                        s_status.position_ms = s_status.duration_ms;
                    }
                    xSemaphoreGive(s_state_mutex);
                }
                if (pause_after_seek_pending && discard_pcm_bytes == 0) {
                    // A seek issued while paused must decode through the
                    // requested position before pausing again. Otherwise the
                    // normal pause gate stops the FLAC replay at byte zero.
                    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                    s_status.paused = true;
                    xSemaphoreGive(s_state_mutex);
                    pause_after_seek_pending = false;
                    break;
                }
                if (replay_seek_pending || discard_pcm_bytes > 0) {
                    // AAC replay can require millions of bytes to be decoded
                    // before the requested position.  Yield often enough for
                    // LVGL, but not once per compressed frame.
                    if ((++replay_yield_count & 0x0Fu) == 0) vTaskDelay(1);
                }
                no_progress_count = 0;
            } else if (raw.consumed == 0) {
                ++no_progress_count;
                // A decoder may need the next read-ahead segment to finish a
                // frame. Leave the inner loop and let the outer loop refill
                // the ring instead of spinning on a short SD read.
                if (!input.eof && input.available < input.capacity) {
                    if (!input.fill()) {
                        if (input.error) {
                            result = ESP_FAIL;
                            ESP_LOGE(kTag, "read error while playing %s", path);
                            break;
                        }
                    }
                    break;
                }
                if (no_progress_count > 3) {
                    ESP_LOGE(kTag, "%s decoder made no progress in %s",
                             audio_format_name(format), path);
                    result = ESP_FAIL;
                    break;
                }
                vTaskDelay(1);
            } else {
                no_progress_count = 0;
                if (replay_seek_pending) vTaskDelay(1);
            }

            raw.buffer += raw.consumed;
            raw.len -= raw.consumed;
        }

        if (result != ESP_OK) break;
    }

    const bool request_still_current = generation_is_current(generation);
    stop_pcm_output(&pcm_sink, request_still_current && result == ESP_OK, generation);
    if (pcm_sink.error && result == ESP_OK) result = pcm_sink.error_code;

    if (!generation_is_current(generation)) {
        if (s_i2s_started) {
            i2s_channel_disable(s_i2s_tx);
            s_i2s_started = false;
        }
        result = ESP_OK;
    } else if (result == ESP_OK) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_status.playing = false;
        s_status.paused = false;
        s_status.eof = true;
        if (s_status.duration_ms > 0) s_status.position_ms = s_status.duration_ms;
        xSemaphoreGive(s_state_mutex);
        ESP_LOGI(kTag, "finished %s", path);
    } else {
        if (s_i2s_started) {
            i2s_channel_disable(s_i2s_tx);
            s_i2s_started = false;
        }
        set_error(result, path);
    }

    s_diagnostics.average_sd_read_us = s_diagnostics.sd_read_count == 0 ? 0 :
        static_cast<uint32_t>(s_diagnostics.total_sd_read_us /
                              s_diagnostics.sd_read_count);
    s_diagnostics.average_sd_lock_wait_us = s_diagnostics.sd_read_count == 0 ? 0 :
        static_cast<uint32_t>(s_diagnostics.total_sd_lock_wait_us /
                              s_diagnostics.sd_read_count);
    ESP_LOGI(kTag, "read-ahead: max=%u us avg=%u us lock-max=%u us lock-avg=%u us "
             "low=%u bytes underruns=%u; pcm-low=%u bytes pcm-underruns=%u; "
             "buffers read-ahead=%s pcm=%s stereo=%s",
             static_cast<unsigned>(s_diagnostics.max_sd_read_us),
             static_cast<unsigned>(s_diagnostics.average_sd_read_us),
             static_cast<unsigned>(s_diagnostics.max_sd_lock_wait_us),
             static_cast<unsigned>(s_diagnostics.average_sd_lock_wait_us),
             static_cast<unsigned>(s_diagnostics.audio_buffer_low_watermark),
             static_cast<unsigned>(s_diagnostics.underrun_count),
             static_cast<unsigned>(s_diagnostics.pcm_buffer_low_watermark),
             static_cast<unsigned>(s_diagnostics.pcm_underrun_count),
             s_diagnostics.read_ahead_internal ? "internal" : "psram",
             s_diagnostics.pcm_internal ? "internal" : "psram",
             s_diagnostics.stereo_internal ? "internal" : "psram");

    heap_caps_free(read_ahead_buffer);
    heap_caps_free(pcm);
    heap_caps_free(stereo);
    if (decoder) esp_audio_simple_dec_close(decoder);
    if (opus_decoder) esp_opus_dec_close(opus_decoder);
    if (aac_decoder) esp_aac_dec_close(aac_decoder);
    lyra::sd::close(file, lyra::sd::Client::Audio);
    return result;
}

void audio_task(void *)
{
    uint32_t handled_generation = 0;
    while (true) {
        char path[lyra::audio::kMaxPath];
        uint32_t generation;
        uint32_t seek_position_ms;
        bool paused;
        bool pause_after_seek;
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        generation = s_request_generation;
        seek_position_ms = s_requested_seek_ms;
        paused = s_status.paused;
        pause_after_seek = s_requested_pause_after_seek;
        std::strncpy(path, s_requested_path, sizeof(path) - 1);
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
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            s_status.playing = false;
            s_status.paused = false;
            s_status.eof = false;
            s_status.position_ms = 0;
            s_status.duration_ms = 0;
            s_status.path[0] = '\0';
            xSemaphoreGive(s_state_mutex);
            continue;
        }
        play_file(path, generation, seek_position_ms, paused, pause_after_seek);
    }
}

} // namespace

namespace lyra::audio {

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

    const esp_audio_err_t decoder_ret = esp_audio_dec_register_default();
    if (decoder_ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(kTag, "default audio decoder registration failed: %d",
                 static_cast<int>(decoder_ret));
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
        i2s_del_channel(s_i2s_tx);
        s_i2s_tx = nullptr;
        vSemaphoreDelete(s_state_mutex);
        s_state_mutex = nullptr;
        return ESP_FAIL;
    }

    s_status = {};
    s_status.initialized = true;
    s_status.last_error = ESP_OK;
    s_status.volume_percent = load_saved_volume();
    s_requested_seek_ms = 0;
    s_requested_pause_after_seek = false;
    s_initialized = true;
    if (xTaskCreatePinnedToCore(audio_task, "lyra_audio", kAudioTaskStack, nullptr,
                                kAudioTaskPriority, &s_audio_task, 0) != pdPASS) {
        s_initialized = false;
        esp_audio_simple_dec_unregister_default();
        esp_audio_dec_unregister_default();
        i2s_del_channel(s_i2s_tx);
        s_i2s_tx = nullptr;
        vSemaphoreDelete(s_state_mutex);
        s_state_mutex = nullptr;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(kTag, "audio playback ready (MP3/FLAC/AAC/M4A/WAV/OGG/Opus/AIFF): "
             "I2S BCLK=%d WS=%d DOUT=%d",
             kAudioBclkGpio, kAudioLrclkGpio, kAudioDataOutGpio);
    return ESP_OK;
}

esp_err_t play(const char *path)
{
    if (!s_initialized || s_state_mutex == nullptr) return ESP_ERR_INVALID_STATE;
    if (!has_sdcard_prefix(path)) return ESP_ERR_INVALID_ARG;
    AudioFormat format{};
    if (!get_audio_format(path, &format)) return ESP_ERR_NOT_SUPPORTED;
    if (std::strlen(path) >= kMaxPath) return ESP_ERR_INVALID_SIZE;

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    std::strncpy(s_requested_path, path, sizeof(s_requested_path) - 1);
    s_requested_path[sizeof(s_requested_path) - 1] = '\0';
    s_requested_seek_ms = 0;
    s_requested_pause_after_seek = false;
    ++s_request_generation;
    s_status.last_error = ESP_OK;
    s_status.playing = true;
    s_status.eof = false;
    s_status.paused = false;
    s_status.sample_rate = 0;
    s_status.channels = 0;
    s_status.bits_per_sample = 0;
    s_status.decoded_bytes = 0;
    s_status.position_ms = 0;
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
    if (volume_percent > kMaximumVolumePercent) {
        volume_percent = kMaximumVolumePercent;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_status.volume_percent = volume_percent;
    xSemaphoreGive(s_state_mutex);
    xTaskNotifyGive(s_audio_task);
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
