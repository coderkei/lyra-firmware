/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../lyra_audio_internal.h"

namespace lyra::audio::internal {

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
                       M4aDurationCandidates *candidates, unsigned depth)
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

uint64_t read_be64(const uint8_t *bytes)
{
    return (static_cast<uint64_t>(read_be32(bytes)) << 32) | read_be32(bytes + 4);
}

bool read_flac_stream_info(FILE *file, FlacStreamInfo *info,
                           uint64_t target_samples)
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

} // namespace lyra::audio::internal
