/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "driver/i2s_std.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "decoder/impl/esp_opus_dec.h"
#include "lyra_audio.h"
#include "lyra_audio_pcm.h"
#include "lyra_board_pins.h"
#include "lyra_sd.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace lyra::audio::internal {

using namespace lyra::board::jc3248w535en;

constexpr const char *kTag = "lyra.audio";
constexpr size_t kReadAheadBufferBytes = 256 * 1024;
constexpr size_t kReadAheadChunkBytes = 32 * 1024;
constexpr size_t kM4aReadAheadChunkBytes = kReadAheadBufferBytes - 1;
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
constexpr size_t kPcmOutputBufferBytes = 256 * 1024;
constexpr size_t kPcmOutputChunkBytes = 16 * 1024;
constexpr size_t kI2sDmaDescriptorCount = 16;
constexpr size_t kI2sDmaFrameCount = 512;
constexpr size_t kI2sFrameBytes = sizeof(int16_t) * 2;
constexpr size_t kI2sDmaBufferBytes = kI2sDmaDescriptorCount * kI2sDmaFrameCount * kI2sFrameBytes;
constexpr size_t kPcmOutputStartBytes = kI2sDmaBufferBytes;
constexpr size_t kDurationScanBufferBytes = 2048;
constexpr uint32_t kAudioTaskStack = 24 * 1024;
constexpr uint32_t kAudioOutputTaskStack = 8 * 1024;
constexpr UBaseType_t kAudioTaskPriority = 8;
constexpr UBaseType_t kAudioOutputTaskPriority = 9;
constexpr const char *kSettingsNamespace = "lyra";
constexpr const char *kVolumeKey = "volume";
constexpr const char *kMaximumVolumeKey = "max_volume";

extern SemaphoreHandle_t s_state_mutex;
extern TaskHandle_t s_audio_task;
extern i2s_chan_handle_t s_i2s_tx;
extern i2s_chan_handle_t s_i2s_speaker_tx;
extern bool s_i2s_started;
extern bool s_i2s_speaker_started;
extern bool s_speaker_output_faulted;
extern bool s_initialized;
extern bool s_nvs_ready;
extern bool s_speaker_output_enabled;
extern uint8_t s_maximum_volume_percent;
extern int16_t s_replay_gain_tenths_db;
extern uint8_t s_transition_gain_percent;
extern lyra::audio::EqualizerSettings s_equalizer;
extern uint32_t s_equalizer_generation;
extern char s_requested_path[lyra::audio::kMaxPath];
extern uint32_t s_request_generation;
extern uint32_t s_requested_seek_ms;
extern bool s_requested_pause_after_seek;
extern uint8_t s_duration_scan_buffer[kDurationScanBufferBytes];
extern lyra::audio::Status s_status;
extern lyra::audio::Diagnostics s_diagnostics;

using lyra::audio::pcm::PcmOutput;

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

struct AiffStreamInfo {
    long data_start;
    uint64_t data_bytes;
    uint32_t sample_rate;
    uint32_t frame_count;
    uint8_t channels;
    uint8_t bits_per_sample;
    bool little_endian;
};

struct WavStreamInfo {
    uint32_t sample_rate = 0;
    uint32_t byte_rate = 0;
    uint64_t data_bytes = 0;
    long data_start = 0;
};

struct AacFrameInfo {
    uint32_t sample_rate = 0;
    uint32_t samples_per_frame = 0;
    uint32_t frame_length = 0;
    uint8_t header_length = 0;
};

struct M4aAtomInfo {
    uint64_t data_start = 0;
    uint64_t end = 0;
    uint8_t type[4]{};
};

struct M4aDurationCandidates {
    uint32_t movie_ms = 0;
    uint32_t track_ms = 0;
};

struct Mp3FrameInfo {
    uint32_t sample_rate;
    uint32_t samples_per_frame;
    uint32_t frame_length;
    uint32_t bitrate_bps;
    uint8_t version;
    uint8_t channel_mode;
};

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

bool fail_pcm_output(PcmOutput *output, esp_err_t error);
lyra::audio::pcm::ProcessingSettings pcm_processing_settings();
bool speaker_output_enabled();
esp_err_t preload_i2s_data(i2s_chan_handle_t channel, const uint8_t *data, size_t bytes);
esp_err_t write_i2s_data(i2s_chan_handle_t channel, const uint8_t *data, size_t bytes);
void disable_speaker_i2s();
bool start_speaker_i2s(const uint8_t *data, size_t bytes);
void mirror_to_speaker(const uint8_t *data, size_t bytes);
bool preload_and_start_i2s(PcmOutput *output);
esp_err_t ensure_nvs_ready();
uint8_t load_saved_volume();
void load_saved_maximum_volume();
esp_err_t save_volume_to_nvs(uint8_t volume);
void *alloc_audio_buffer(size_t size);
void *alloc_audio_dma_buffer(size_t size);
bool buffer_is_internal(const void *buffer);
size_t audio_read(FILE *file, void *buffer, size_t size);
int audio_seek(FILE *file, long offset, int origin);
bool has_sdcard_prefix(const char *path);
bool extension_is(const char *extension, const char *expected);
bool get_audio_format(const char *path, AudioFormat *format);
const char *audio_format_name(AudioFormat format);
esp_audio_simple_dec_type_t decoder_type_for_format(AudioFormat format);
uint16_t read_be16(const uint8_t *bytes);
double read_aiff_extended_rate(const uint8_t *bytes);
bool read_aiff_stream_info(FILE *file, AiffStreamInfo *info);
uint32_t read_aiff_duration_ms(const AiffStreamInfo &info);
uint16_t read_le16(const uint8_t *bytes);
uint32_t read_le32(const uint8_t *bytes);
uint64_t read_le64(const uint8_t *bytes);
bool read_wav_stream_info(FILE *file, WavStreamInfo *info);
uint32_t read_wav_duration_ms(FILE *file);
bool parse_aac_frame_header(const uint8_t *header, AacFrameInfo *info);
bool find_first_aac_frame(FILE *file, long start, long *frame_offset, AacFrameInfo *info);
uint32_t read_aac_duration_ms(FILE *file);
bool seek_aac_file(FILE *file, uint32_t position_ms, uint32_t duration_ms);
bool m4a_type(const M4aAtomInfo &atom, const char (&type)[5]);
bool read_m4a_atom(FILE *file, uint64_t position, uint64_t limit, M4aAtomInfo *atom);
bool m4a_container(const M4aAtomInfo &atom);
uint32_t m4a_time_to_ms(uint64_t duration, uint32_t timescale);
void read_m4a_time_atom(FILE *file, const M4aAtomInfo &atom, M4aDurationCandidates *candidates);
void scan_m4a_duration(FILE *file, uint64_t start, uint64_t end, M4aDurationCandidates *candidates, unsigned depth = 0);
uint32_t read_m4a_duration_ms(FILE *file);
uint32_t read_ogg_duration_ms(FILE *file);
uint32_t read_ogg_tail_duration_ms(FILE *file, uint32_t sample_rate, uint32_t pre_skip);
uint32_t read_be32(const uint8_t *bytes);
uint32_t read_be24(const uint8_t *bytes);
uint32_t read_synchsafe32(const uint8_t *bytes);
bool parse_mp3_frame_header(const uint8_t *header, Mp3FrameInfo *info);
bool find_first_mp3_frame(FILE *file, long audio_start, long *frame_offset, Mp3FrameInfo *frame_info);
uint32_t samples_to_milliseconds(uint64_t samples, uint32_t sample_rate);
bool read_xing_or_vbri_duration(FILE *file, long frame_offset, const Mp3FrameInfo &frame_info, uint32_t *duration_ms);
bool looks_like_constant_bitrate(FILE *file, long frame_offset, const Mp3FrameInfo &first_frame);
uint32_t read_mp3_duration_ms(FILE *file);
uint64_t read_be64(const uint8_t *bytes);
bool read_flac_stream_info(FILE *file, FlacStreamInfo *info, uint64_t target_samples = kNoFlacSeekSample);
bool prime_flac_decoder_input(AudioReadAhead *input, const FlacStreamInfo &info);
uint32_t pcm_bytes_to_milliseconds(uint64_t pcm_bytes, uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample);
uint32_t read_flac_duration_ms(FILE *file);
uint8_t flac_crc8(const uint8_t *bytes, size_t length);
size_t flac_utf8_value_bytes(uint8_t first);
bool valid_flac_frame_header(const uint8_t *bytes, size_t length);
bool find_flac_frame_offset(FILE *file, long estimated_offset, long first_frame_offset, long file_end, uint8_t *scan_buffer, size_t scan_capacity, long *frame_offset);
bool seek_mp3_file(FILE *file, uint32_t position_ms, uint32_t duration_ms);
bool generation_is_current(uint32_t generation);
void pcm_output_task(void *context);
bool start_pcm_output(PcmOutput *output);
void stop_pcm_output(PcmOutput *output, bool drain, uint32_t generation);
esp_err_t queue_pcm(const uint8_t *pcm, size_t pcm_bytes, PcmOutput *output, uint32_t generation);
bool read_pause_state(uint32_t generation);
void set_error(esp_err_t error, const char *path);
esp_err_t configure_i2s(uint32_t sample_rate);
esp_err_t write_pcm(const uint8_t *pcm, size_t pcm_bytes, uint8_t channels, uint8_t bits_per_sample, int16_t *stereo_buffer, size_t stereo_buffer_bytes, PcmOutput *output, uint32_t generation, bool unsigned_8bit = false);
esp_err_t play_file(const char *path, uint32_t generation, uint32_t start_position_ms, bool start_paused, bool pause_after_seek);
void audio_task(void *);

} // namespace lyra::audio::internal
