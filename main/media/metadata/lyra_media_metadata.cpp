/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../lyra_media_internal.h"

namespace lyra::media::internal {

void copy_text(char *destination, size_t capacity, const char *source)
{
    if (capacity == 0) return;
    if (!source) source = "";
    const size_t length = std::min(std::strlen(source), capacity - 1);
    std::memcpy(destination, source, length);
    destination[length] = '\0';
}

bool join_path(char *destination, size_t capacity, const char *parent, const char *child)
{
    if (!destination || capacity == 0 || !parent || !child) return false;
    const size_t parent_length = std::strlen(parent);
    const bool add_separator = parent_length > 0 && parent[parent_length - 1] != '/' && child[0] != '/';
    const size_t child_length = std::strlen(child);
    const size_t required = parent_length + (add_separator ? 1 : 0) + child_length + 1;
    if (required > capacity) {
        destination[0] = '\0';
        return false;
    }
    std::memcpy(destination, parent, parent_length);
    size_t offset = parent_length;
    if (add_separator) destination[offset++] = '/';
    std::memcpy(destination + offset, child, child_length);
    destination[offset + child_length] = '\0';
    return true;
}

bool equals_ci(const char *left, const char *right)
{
    while (*left && *right) {
        if (std::tolower(static_cast<unsigned char>(*left)) !=
            std::tolower(static_cast<unsigned char>(*right))) return false;
        ++left;
        ++right;
    }
    return *left == *right;
}

bool contains_ci(const char *text, const char *query)
{
    if (!query || !*query) return true;
    const size_t query_length = std::strlen(query);
    for (; *text; ++text) {
        size_t i = 0;
        while (i < query_length && text[i] &&
               std::tolower(static_cast<unsigned char>(text[i])) ==
                   std::tolower(static_cast<unsigned char>(query[i]))) ++i;
        if (i == query_length) return true;
    }
    return false;
}

uint64_t hash_path(const char *path)
{
    uint64_t hash = 14695981039346656037ull;
    for (const uint8_t *p = reinterpret_cast<const uint8_t *>(path); *p; ++p) {
        hash = (hash ^ *p) * 1099511628211ull;
    }
    return hash;
}

uint32_t checksum_update(uint32_t hash, const void *data, size_t length)
{
    const auto *bytes = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < length; ++i) hash = (hash ^ bytes[i]) * 16777619u;
    return hash;
}

bool checksum_file_payload(FILE *file, uint32_t start, uint32_t end, uint32_t *out)
{
    if (!file || !out || end < start || std::fseek(file, start, SEEK_SET) != 0) return false;
    constexpr size_t kChecksumBufferSize = 4096;
    auto *buffer = static_cast<uint8_t *>(heap_caps_malloc(kChecksumBufferSize, MALLOC_CAP_INTERNAL));
    if (!buffer) return false;
    uint32_t hash = 2166136261u;
    uint32_t remaining = end - start;
    while (remaining) {
        const size_t wanted = std::min<size_t>(kChecksumBufferSize, remaining);
        const size_t read = std::fread(buffer, 1, wanted, file);
        if (read != wanted) {
            heap_caps_free(buffer);
            return false;
        }
        hash = checksum_update(hash, buffer, read);
        remaining -= static_cast<uint32_t>(read);
    }
    heap_caps_free(buffer);
    *out = hash;
    return true;
}

const char *extension_of(const char *name)
{
    const char *dot = std::strrchr(name, '.');
    return dot && dot[1] ? dot + 1 : "";
}

bool compatible_audio(const char *name)
{
    const char *ext = extension_of(name);
    constexpr const char *formats[] = {"mp3", "wav", "flac", "aac", "m4a", "ogg", "opus",
                                       "aiff", "aif", "aifc"};
    for (const char *format : formats) if (equals_ci(ext, format)) return true;
    return false;
}

const char *base_name(const char *path)
{
    const char *slash = std::strrchr(path, '/');
    return slash ? slash + 1 : path;
}

void parent_name(const char *path, char *out, size_t capacity)
{
    char scratch[kMaxPath];
    copy_text(scratch, sizeof(scratch), path);
    char *slash = std::strrchr(scratch, '/');
    if (slash) *slash = '\0';
    copy_text(out, capacity, base_name(scratch));
}

void metadata_from_path(Track *track)
{
    char stem[kMaxName];
    copy_text(stem, sizeof(stem), base_name(track->path));
    char *dot = std::strrchr(stem, '.');
    if (dot) *dot = '\0';
    char *separator = std::strstr(stem, " - ");
    if (separator) {
        *separator = '\0';
        copy_text(track->artist, sizeof(track->artist), stem);
        copy_text(track->title, sizeof(track->title), separator + 3);
    } else {
        copy_text(track->title, sizeof(track->title), stem);
        copy_text(track->artist, sizeof(track->artist), "Unknown artist");
    }
    parent_name(track->path, track->album, sizeof(track->album));
    track->album_artist[0] = '\0';
    copy_text(track->composer, sizeof(track->composer), "Unknown");
    copy_text(track->genre, sizeof(track->genre), "Unknown genre");
    copy_text(track->year, sizeof(track->year), "Unknown");
    copy_text(track->format, sizeof(track->format), extension_of(track->path));
}

uint32_t read_be32(const uint8_t *bytes)
{
    return (static_cast<uint32_t>(bytes[0]) << 24) |
           (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) |
           static_cast<uint32_t>(bytes[3]);
}

uint64_t read_be64(const uint8_t *bytes)
{
    return (static_cast<uint64_t>(read_be32(bytes)) << 32) | read_be32(bytes + 4);
}

uint32_t read_syncsafe32(const uint8_t *bytes)
{
    if ((bytes[0] | bytes[1] | bytes[2] | bytes[3]) & 0x80) return 0;
    return (static_cast<uint32_t>(bytes[0]) << 21) |
           (static_cast<uint32_t>(bytes[1]) << 14) |
           (static_cast<uint32_t>(bytes[2]) << 7) |
           static_cast<uint32_t>(bytes[3]);
}

uint32_t read_le32(const uint8_t *bytes)
{
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) |
           (static_cast<uint32_t>(bytes[3]) << 24);
}

uint32_t parse_tag_number(const char *text)
{
    if (!text) return 0;
    while (*text && !std::isdigit(static_cast<unsigned char>(*text))) ++text;
    if (!*text) return 0;
    char *end = nullptr;
    const unsigned long value = std::strtoul(text, &end, 10);
    return value > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(value);
}

int16_t parse_replay_gain_tenths_db(const char *text)
{
    if (!text) return 0;
    char *end = nullptr;
    const float decibels = std::strtof(text, &end);
    if (end == text || !std::isfinite(decibels)) return 0;
    constexpr float kMinimumReplayGainDb = -12.0f;
    constexpr float kMaximumReplayGainDb = 12.0f;
    return static_cast<int16_t>(std::lround(
        std::clamp(decibels, kMinimumReplayGainDb, kMaximumReplayGainDb) * 10.0f));
}

void copy_tag_text(char *destination, size_t capacity, const uint8_t *source,
                   size_t length, uint8_t encoding)
{
    if (!destination || capacity == 0 || !source || length == 0) return;
    size_t used = 0;
    if (encoding == 1 || encoding == 2) {
        bool little_endian = encoding == 1;
        size_t position = 0;
        if (length >= 2 && source[0] == 0xFF && source[1] == 0xFE) {
            little_endian = true;
            position = 2;
        } else if (length >= 2 && source[0] == 0xFE && source[1] == 0xFF) {
            little_endian = false;
            position = 2;
        }
        while (position + 1 < length && used + 1 < capacity) {
            const uint16_t value = little_endian
                ? static_cast<uint16_t>(source[position] | (source[position + 1] << 8))
                : static_cast<uint16_t>((source[position] << 8) | source[position + 1]);
            position += 2;
            if (value == 0) break;
            if (value < 0x80) destination[used++] = static_cast<char>(value);
            else if (value < 0x800 && used + 2 < capacity) {
                destination[used++] = static_cast<char>(0xC0 | (value >> 6));
                destination[used++] = static_cast<char>(0x80 | (value & 0x3F));
            } else if (used + 3 < capacity) {
                destination[used++] = static_cast<char>(0xE0 | (value >> 12));
                destination[used++] = static_cast<char>(0x80 | ((value >> 6) & 0x3F));
                destination[used++] = static_cast<char>(0x80 | (value & 0x3F));
            }
        }
    } else {
        for (size_t i = 0; i < length && source[i] && used + 1 < capacity; ++i) {
            const uint8_t value = source[i];
            if (encoding == 0 && value >= 0x80 && used + 2 < capacity) {
                destination[used++] = static_cast<char>(0xC0 | (value >> 6));
                destination[used++] = static_cast<char>(0x80 | (value & 0x3F));
            } else if (value >= 0x20 || value == '\t') {
                destination[used++] = static_cast<char>(value);
            }
        }
    }
    while (used > 0 && std::isspace(static_cast<unsigned char>(destination[used - 1]))) --used;
    destination[used] = '\0';
}

void normalize_year(char *year)
{
    if (!year) return;
    for (size_t i = 0; i + 3 < 8 && year[i]; ++i) {
        if (std::isdigit(static_cast<unsigned char>(year[i])) &&
            std::isdigit(static_cast<unsigned char>(year[i + 1])) &&
            std::isdigit(static_cast<unsigned char>(year[i + 2])) &&
            std::isdigit(static_cast<unsigned char>(year[i + 3]))) {
            char normalized[5] = {year[i], year[i + 1], year[i + 2], year[i + 3], '\0'};
            copy_text(year, 8, normalized);
            return;
        }
    }
    copy_text(year, 8, "Unknown");
}

void assign_tag(Track *track, const char *key, const uint8_t *value, size_t length,
                uint8_t encoding)
{
    char decoded[kMaxName]{};
    copy_tag_text(decoded, sizeof(decoded), value, length, encoding);
    if (!decoded[0]) return;
    if (equals_ci(key, "TITLE")) copy_text(track->title, sizeof(track->title), decoded);
    else if (equals_ci(key, "ARTIST")) copy_text(track->artist, sizeof(track->artist), decoded);
    else if (equals_ci(key, "ALBUM")) copy_text(track->album, sizeof(track->album), decoded);
    else if (equals_ci(key, "ALBUMARTIST") || equals_ci(key, "ALBUM ARTIST")) {
        copy_text(track->album_artist, sizeof(track->album_artist), decoded);
    } else if (equals_ci(key, "COMPOSER")) {
        copy_text(track->composer, sizeof(track->composer), decoded);
    }
    else if (equals_ci(key, "GENRE")) copy_text(track->genre, sizeof(track->genre), decoded);
    else if (equals_ci(key, "DATE") || equals_ci(key, "YEAR")) {
        copy_text(track->year, sizeof(track->year), decoded);
        normalize_year(track->year);
    } else if (equals_ci(key, "TRACKNUMBER") || equals_ci(key, "TRACK") ||
               equals_ci(key, "TRCK")) {
        track->track_number = parse_tag_number(decoded);
    } else if (equals_ci(key, "DISCNUMBER") || equals_ci(key, "DISC") ||
               equals_ci(key, "TPOS")) {
        track->disc_number = parse_tag_number(decoded);
    } else if (equals_ci(key, "REPLAYGAIN_TRACK_GAIN")) {
        track->replay_gain_tenths_db = parse_replay_gain_tenths_db(decoded);
    }
}

void assign_id3_user_text_tag(Track *track, const uint8_t *text, size_t length)
{
    if (!track || !text || length < 3) return;
    const uint8_t encoding = text[0];
    size_t separator = 1;
    if (encoding == 1 || encoding == 2) {
        while (separator + 1 < length &&
               (text[separator] != 0 || text[separator + 1] != 0)) {
            separator += 2;
        }
        if (separator + 1 >= length) return;
    } else {
        while (separator < length && text[separator] != 0) ++separator;
        if (separator >= length) return;
    }

    char key[64]{};
    copy_tag_text(key, sizeof(key), text + 1, separator - 1, encoding);
    const size_t value_start = separator + (encoding == 1 || encoding == 2 ? 2 : 1);
    if (!key[0] || value_start >= length) return;
    assign_tag(track, key, text + value_start, length - value_start, encoding);
}

void read_id3_text_tags(FILE *file, Track *track, const uint8_t header[10],
                        long tag_start)
{
    const uint8_t version = header[3];
    if (version < 2 || version > 4) return;
    const uint32_t tag_size = read_syncsafe32(header + 6);
    uint64_t position = 10;
    const uint64_t tag_end = position + tag_size;
    if (header[5] & 0x40) {
        uint8_t extended[4]{};
        if (std::fseek(file, tag_start + 10, SEEK_SET) != 0 ||
            std::fread(extended, 1, 4, file) != 4) return;
        const uint32_t extended_size = version == 4 ? read_syncsafe32(extended) : read_be32(extended);
        position += version == 4 ? extended_size : extended_size + 4;
    }
    while (position + (version == 2 ? 6u : 10u) <= tag_end) {
        if (std::fseek(file, tag_start + static_cast<long>(position), SEEK_SET) != 0) break;
        uint8_t frame_header[10]{};
        const size_t header_size = version == 2 ? 6 : 10;
        if (std::fread(frame_header, 1, header_size, file) != header_size || frame_header[0] == 0) break;
        const uint32_t frame_size = version == 2
            ? ((static_cast<uint32_t>(frame_header[3]) << 16) |
               (static_cast<uint32_t>(frame_header[4]) << 8) | frame_header[5])
            : (version == 4 ? read_syncsafe32(frame_header + 4) : read_be32(frame_header + 4));
        position += header_size;
        if (frame_size == 0 || position + frame_size > tag_end) break;
        const char *key = nullptr;
        if ((version == 2 && std::memcmp(frame_header, "TT2", 3) == 0) ||
            (version > 2 && std::memcmp(frame_header, "TIT2", 4) == 0)) key = "TITLE";
        else if ((version == 2 && std::memcmp(frame_header, "TP1", 3) == 0) ||
                 (version > 2 && std::memcmp(frame_header, "TPE1", 4) == 0)) key = "ARTIST";
        else if ((version == 2 && std::memcmp(frame_header, "TP2", 3) == 0) ||
                 (version > 2 && std::memcmp(frame_header, "TPE2", 4) == 0)) key = "ALBUMARTIST";
        else if ((version == 2 && std::memcmp(frame_header, "TAL", 3) == 0) ||
                 (version > 2 && std::memcmp(frame_header, "TALB", 4) == 0)) key = "ALBUM";
        else if ((version == 2 && std::memcmp(frame_header, "TCM", 3) == 0) ||
                 (version > 2 && std::memcmp(frame_header, "TCOM", 4) == 0)) key = "COMPOSER";
        else if ((version == 2 && std::memcmp(frame_header, "TCO", 3) == 0) ||
                 (version > 2 && std::memcmp(frame_header, "TCON", 4) == 0)) key = "GENRE";
        else if ((version == 2 && std::memcmp(frame_header, "TYE", 3) == 0) ||
                 (version > 2 && (std::memcmp(frame_header, "TYER", 4) == 0 ||
                                  std::memcmp(frame_header, "TDRC", 4) == 0))) key = "YEAR";
        else if ((version == 2 && std::memcmp(frame_header, "TRK", 3) == 0) ||
                 (version > 2 && std::memcmp(frame_header, "TRCK", 4) == 0)) key = "TRACKNUMBER";
        else if ((version == 2 && std::memcmp(frame_header, "TPA", 3) == 0) ||
                 (version > 2 && std::memcmp(frame_header, "TPOS", 4) == 0)) key = "DISCNUMBER";
        const bool user_text = (version == 2 && std::memcmp(frame_header, "TXX", 3) == 0) ||
                               (version > 2 && std::memcmp(frame_header, "TXXX", 4) == 0);
        if (key || user_text) {
            uint8_t text[512]{};
            const size_t wanted = std::min<size_t>(frame_size, sizeof(text));
            if (std::fseek(file, tag_start + static_cast<long>(position), SEEK_SET) == 0 &&
                std::fread(text, 1, wanted, file) == wanted && wanted > 1) {
                if (user_text) assign_id3_user_text_tag(track, text, wanted);
                else assign_tag(track, key, text + 1, wanted - 1, text[0]);
            }
        }
        position += frame_size;
    }
}

bool read_ogg_comment_packet(FILE *file, uint8_t *packet, size_t capacity,
                             size_t *packet_length)
{
    if (!file || !packet || capacity == 0 || !packet_length ||
        std::fseek(file, 0, SEEK_SET) != 0) return false;
    *packet_length = 0;

    uint8_t page_header[27]{};
    uint8_t lacing[255]{};
    uint8_t discard[256]{};
    size_t current_length = 0;
    bool overflow = false;
    unsigned packet_index = 0;
    while (packet_index < 2) {
        if (lyra::sd::read(file, page_header, sizeof(page_header),
                           lyra::sd::Client::Filesystem) != sizeof(page_header) ||
            std::memcmp(page_header, "OggS", 4) != 0 || page_header[4] != 0) return false;
        const size_t segment_count = page_header[26];
        if (lyra::sd::read(file, lacing, segment_count,
                           lyra::sd::Client::Filesystem) != segment_count) return false;
        for (size_t segment = 0; segment < segment_count; ++segment) {
            size_t remaining = lacing[segment];
            while (remaining > 0) {
                const size_t wanted = std::min(remaining, sizeof(discard));
                uint8_t *destination = discard;
                if (!overflow && current_length + wanted <= capacity) {
                    destination = packet + current_length;
                } else {
                    overflow = true;
                }
                const size_t count = lyra::sd::read(file, destination, wanted,
                                                     lyra::sd::Client::Filesystem);
                if (count != wanted) return false;
                if (!overflow) current_length += count;
                remaining -= count;
            }
            if (lacing[segment] != 255) {
                if (packet_index == 1 && !overflow) {
                    *packet_length = current_length;
                    return true;
                }
                ++packet_index;
                current_length = 0;
                overflow = false;
            }
        }
    }
    return false;
}

void parse_vorbis_comments(const uint8_t *data, size_t length, size_t cursor,
                           Track *track)
{
    if (!data || !track || cursor + 8 > length) return;
    const uint32_t vendor_length = read_le32(data + cursor);
    cursor += 4;
    if (vendor_length > length - cursor) return;
    cursor += vendor_length;
    if (cursor + 4 > length) return;
    const uint32_t comment_count = read_le32(data + cursor);
    cursor += 4;
    for (uint32_t index = 0; index < comment_count && cursor + 4 <= length; ++index) {
        const uint32_t comment_length = read_le32(data + cursor);
        cursor += 4;
        if (comment_length > length - cursor) break;
        const uint8_t *equals = static_cast<const uint8_t *>(
            std::memchr(data + cursor, '=', comment_length));
        if (equals) {
            char key[32]{};
            const size_t key_length = std::min<size_t>(
                equals - (data + cursor), sizeof(key) - 1);
            std::memcpy(key, data + cursor, key_length);
            assign_tag(track, key, equals + 1,
                       comment_length - static_cast<size_t>(equals + 1 - (data + cursor)));
        }
        cursor += comment_length;
    }
}

void read_ogg_text_tags(FILE *file, Track *track)
{
    auto *packet = static_cast<uint8_t *>(heap_caps_malloc(
        kMaximumOggCommentPacketBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!packet) packet = static_cast<uint8_t *>(std::malloc(kMaximumOggCommentPacketBytes));
    if (!packet) return;
    size_t length = 0;
    if (read_ogg_comment_packet(file, packet, kMaximumOggCommentPacketBytes, &length)) {
        if (length >= 8 && std::memcmp(packet, "OpusTags", 8) == 0) {
            parse_vorbis_comments(packet, length, 8, track);
        } else if (length >= 7 && packet[0] == 3 &&
                   std::memcmp(packet + 1, "vorbis", 6) == 0) {
            parse_vorbis_comments(packet, length, 7, track);
        }
    }
    heap_caps_free(packet);
}

bool read_mp4_atom(FILE *file, uint64_t position, uint64_t limit, Mp4Atom *atom)
{
    if (!file || !atom || position + 8 > limit ||
        std::fseek(file, static_cast<long>(position), SEEK_SET) != 0) return false;
    uint8_t header[16]{};
    if (std::fread(header, 1, 8, file) != 8) return false;
    uint64_t size = read_be32(header);
    uint8_t header_size = 8;
    if (size == 1) {
        if (position + 16 > limit || std::fread(header + 8, 1, 8, file) != 8) return false;
        size = read_be64(header + 8);
        header_size = 16;
    } else if (size == 0) {
        size = limit - position;
    }
    if (size < header_size || size > limit - position) return false;
    atom->start = position;
    atom->data_start = position + header_size;
    atom->end = position + size;
    atom->data_size = size - header_size;
    atom->header_size = header_size;
    std::memcpy(atom->type, header + 4, 4);
    return true;
}

bool mp4_type(const Mp4Atom &atom, const char (&type)[5])
{
    return std::memcmp(atom.type, type, 4) == 0;
}

bool read_mp4_text_value(FILE *file, const Mp4Atom &item, const char *key, Track *track)
{
    uint64_t cursor = item.data_start;
    while (cursor + 8 <= item.end) {
        Mp4Atom child{};
        if (!read_mp4_atom(file, cursor, item.end, &child)) return false;
        if (mp4_type(child, "data") && child.data_size >= 8) {
            const uint64_t value_start = child.data_start + 8;
            const size_t value_length = static_cast<size_t>(child.data_size - 8);
            auto *value = static_cast<uint8_t *>(heap_caps_malloc(
                std::min<size_t>(value_length, kMaxName), MALLOC_CAP_INTERNAL));
            if (!value) return false;
            const size_t wanted = std::min<size_t>(value_length, kMaxName);
            const bool read = std::fseek(file, static_cast<long>(value_start), SEEK_SET) == 0 &&
                              std::fread(value, 1, wanted, file) == wanted;
            if (read) assign_tag(track, key, value, wanted);
            heap_caps_free(value);
            return read;
        }
        cursor = child.end;
    }
    return false;
}

bool read_mp4_position_value(FILE *file, const Mp4Atom &item, bool disc, Track *track)
{
    if (!file || !track) return false;
    uint64_t cursor = item.data_start;
    while (cursor + 8 <= item.end) {
        Mp4Atom child{};
        if (!read_mp4_atom(file, cursor, item.end, &child)) return false;
        if (mp4_type(child, "data") && child.data_size >= 12) {
            uint8_t value[8]{};
            const uint64_t value_start = child.data_start + 8;
            if (std::fseek(file, static_cast<long>(value_start), SEEK_SET) != 0 ||
                std::fread(value, 1, sizeof(value), file) != sizeof(value)) return false;
            const uint32_t number = (static_cast<uint32_t>(value[2]) << 8) | value[3];
            if (disc) track->disc_number = number;
            else track->track_number = number;
            return true;
        }
        cursor = child.end;
    }
    return false;
}

void read_mp4_ilst(FILE *file, const Mp4Atom &ilst, Track *track)
{
    uint64_t cursor = ilst.data_start;
    while (cursor + 8 <= ilst.end) {
        Mp4Atom item{};
        if (!read_mp4_atom(file, cursor, ilst.end, &item)) return;
        const char *key = nullptr;
        if (std::memcmp(item.type, "\xA9" "nam", 4) == 0) key = "TITLE";
        else if (std::memcmp(item.type, "\xA9" "ART", 4) == 0) key = "ARTIST";
        else if (std::memcmp(item.type, "aART", 4) == 0) key = "ALBUMARTIST";
        else if (std::memcmp(item.type, "\xA9" "alb", 4) == 0) key = "ALBUM";
        else if (std::memcmp(item.type, "\xA9" "wrt", 4) == 0) key = "COMPOSER";
        else if (std::memcmp(item.type, "\xA9" "gen", 4) == 0) key = "GENRE";
        else if (std::memcmp(item.type, "\xA9" "day", 4) == 0) key = "YEAR";
        if (key) read_mp4_text_value(file, item, key, track);
        else if (std::memcmp(item.type, "trkn", 4) == 0) {
            read_mp4_position_value(file, item, false, track);
        } else if (std::memcmp(item.type, "disk", 4) == 0) {
            read_mp4_position_value(file, item, true, track);
        }
        cursor = item.end;
    }
}

bool mp4_container(const Mp4Atom &atom)
{
    return mp4_type(atom, "moov") || mp4_type(atom, "trak") || mp4_type(atom, "mdia") ||
           mp4_type(atom, "minf") || mp4_type(atom, "stbl") || mp4_type(atom, "udta") ||
           mp4_type(atom, "meta") || mp4_type(atom, "ilst") || mp4_type(atom, "edts") ||
           mp4_type(atom, "dinf") || mp4_type(atom, "mvex") || mp4_type(atom, "moof") ||
           mp4_type(atom, "traf");
}

void read_mp4_metadata_tree(FILE *file, uint64_t start, uint64_t end, Track *track,
                            unsigned depth)
{
    if (!file || !track || depth > 8) return;
    uint64_t cursor = start;
    while (cursor + 8 <= end) {
        Mp4Atom atom{};
        if (!read_mp4_atom(file, cursor, end, &atom)) return;
        if (mp4_type(atom, "ilst")) {
            read_mp4_ilst(file, atom, track);
        } else if (mp4_container(atom)) {
            uint64_t child_start = atom.data_start;
            if (mp4_type(atom, "meta") && child_start + 4 <= atom.end) child_start += 4;
            read_mp4_metadata_tree(file, child_start, atom.end, track, depth + 1);
        }
        cursor = atom.end;
    }
}

bool find_mp4_moov(FILE *file, Mp4Atom *moov)
{
    if (!file || !moov || std::fseek(file, 0, SEEK_END) != 0) return false;
    const long file_end = std::ftell(file);
    if (file_end < 12) return false;
    uint64_t cursor = 0;
    while (cursor + 8 <= static_cast<uint64_t>(file_end)) {
        Mp4Atom atom{};
        if (!read_mp4_atom(file, cursor, file_end, &atom)) return false;
        if (mp4_type(atom, "moov")) {
            *moov = atom;
            return true;
        }
        cursor = atom.end;
    }
    return false;
}

void read_mp4_text_tags(FILE *file, Track *track)
{
    Mp4Atom moov{};
    if (!find_mp4_moov(file, &moov)) return;
    read_mp4_metadata_tree(file, moov.data_start, moov.end, track);
}

void read_flac_text_tags(FILE *file, Track *track)
{
    uint64_t position = 4;
    bool last = false;
    while (!last) {
        if (std::fseek(file, static_cast<long>(position), SEEK_SET) != 0) return;
        uint8_t block_header[4]{};
        if (std::fread(block_header, 1, 4, file) != 4) return;
        last = (block_header[0] & 0x80) != 0;
        const uint8_t type = block_header[0] & 0x7F;
        const uint32_t length = (static_cast<uint32_t>(block_header[1]) << 16) |
                                (static_cast<uint32_t>(block_header[2]) << 8) | block_header[3];
        position += 4;
        if (type == 4 && length >= 8 && length <= 64u * 1024u) {
            auto *data = static_cast<uint8_t *>(heap_caps_malloc(length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!data) data = static_cast<uint8_t *>(std::malloc(length));
            if (!data) return;
            const bool read = std::fread(data, 1, length, file) == length;
            if (read) {
                size_t cursor = 0;
                const uint32_t vendor = read_le32(data);
                if (vendor > length - 8) {
                    heap_caps_free(data);
                    return;
                }
                cursor = 4u + static_cast<size_t>(vendor);
                if (cursor + 4 <= length) {
                    const uint32_t comments = read_le32(data + cursor);
                    cursor += 4;
                    for (uint32_t i = 0; i < comments && cursor + 4 <= length; ++i) {
                        const uint32_t comment_length = read_le32(data + cursor);
                        cursor += 4;
                        if (comment_length > length - cursor) break;
                        const uint8_t *equals = static_cast<const uint8_t *>(
                            std::memchr(data + cursor, '=', comment_length));
                        if (equals) {
                            char key[16]{};
                            const size_t key_length = std::min<size_t>(equals - (data + cursor), sizeof(key) - 1);
                            std::memcpy(key, data + cursor, key_length);
                            const uint8_t *value = equals + 1;
                            assign_tag(track, key, value,
                                       comment_length - static_cast<size_t>(value - (data + cursor)));
                        }
                        cursor += comment_length;
                    }
                }
            }
            heap_caps_free(data);
            return;
        }
        position += length;
    }
}

const char *riff_info_key(const uint8_t *id)
{
    if (std::memcmp(id, "INAM", 4) == 0) return "TITLE";
    if (std::memcmp(id, "IART", 4) == 0) return "ARTIST";
    if (std::memcmp(id, "IPRD", 4) == 0) return "ALBUM";
    if (std::memcmp(id, "IGNR", 4) == 0) return "GENRE";
    if (std::memcmp(id, "ICRD", 4) == 0) return "YEAR";
    return nullptr;
}

void read_wav_text_tags(FILE *file, Track *track)
{
    if (!file || !track || std::fseek(file, 0, SEEK_SET) != 0) return;
    uint8_t header[12]{};
    if (std::fread(header, 1, sizeof(header), file) != sizeof(header) ||
        std::memcmp(header, "RIFF", 4) != 0 || std::memcmp(header + 8, "WAVE", 4) != 0) return;
    if (std::fseek(file, 0, SEEK_END) != 0) return;
    const long file_end = std::ftell(file);
    uint64_t position = 12;
    while (position + 8 <= static_cast<uint64_t>(file_end)) {
        uint8_t chunk[8]{};
        if (std::fseek(file, static_cast<long>(position), SEEK_SET) != 0 ||
            std::fread(chunk, 1, sizeof(chunk), file) != sizeof(chunk)) return;
        const uint32_t length = read_le32(chunk + 4);
        const uint64_t data_start = position + 8;
        if (data_start + length > static_cast<uint64_t>(file_end)) return;
        if (std::memcmp(chunk, "LIST", 4) == 0 && length >= 4 &&
            std::fseek(file, static_cast<long>(data_start), SEEK_SET) == 0) {
            uint8_t list_type[4]{};
            if (std::fread(list_type, 1, 4, file) == 4 && std::memcmp(list_type, "INFO", 4) == 0) {
                uint64_t cursor = data_start + 4;
                const uint64_t list_end = data_start + length;
                while (cursor + 8 <= list_end) {
                    uint8_t item[8]{};
                    if (std::fseek(file, static_cast<long>(cursor), SEEK_SET) != 0 ||
                        std::fread(item, 1, sizeof(item), file) != sizeof(item)) break;
                    const uint32_t item_length = read_le32(item + 4);
                    if (cursor + 8 + item_length > list_end) break;
                    const char *key = riff_info_key(item);
                    if (key && item_length > 0) {
                        uint8_t value[512]{};
                        const size_t wanted = std::min<size_t>(item_length, sizeof(value));
                        if (std::fread(value, 1, wanted, file) == wanted) {
                            assign_tag(track, key, value, wanted);
                        }
                    }
                    cursor += 8 + item_length + (item_length & 1u);
                }
            }
        } else if ((std::memcmp(chunk, "id3 ", 4) == 0 ||
                    std::memcmp(chunk, "ID3 ", 4) == 0) && length >= 10) {
            uint8_t id3[10]{};
            if (std::fseek(file, static_cast<long>(data_start), SEEK_SET) == 0 &&
                std::fread(id3, 1, sizeof(id3), file) == sizeof(id3) &&
                std::memcmp(id3, "ID3", 3) == 0) {
                read_id3_text_tags(file, track, id3, static_cast<long>(data_start));
            }
        }
        position = data_start + length + (length & 1u);
    }
}

void read_aiff_text_tags(FILE *file, Track *track)
{
    if (!file || !track || std::fseek(file, 0, SEEK_SET) != 0) return;
    uint8_t form[12]{};
    if (std::fread(form, 1, sizeof(form), file) != sizeof(form) ||
        std::memcmp(form, "FORM", 4) != 0 ||
        (std::memcmp(form + 8, "AIFF", 4) != 0 && std::memcmp(form + 8, "AIFC", 4) != 0)) return;
    if (std::fseek(file, 0, SEEK_END) != 0) return;
    const long file_end = std::ftell(file);
    uint64_t position = 12;
    while (position + 8 <= static_cast<uint64_t>(file_end)) {
        uint8_t chunk[8]{};
        if (std::fseek(file, static_cast<long>(position), SEEK_SET) != 0 ||
            std::fread(chunk, 1, sizeof(chunk), file) != sizeof(chunk)) return;
        const uint32_t length = read_be32(chunk + 4);
        const uint64_t data_start = position + 8;
        if (data_start + length > static_cast<uint64_t>(file_end)) return;
        const char *key = nullptr;
        if (std::memcmp(chunk, "NAME", 4) == 0) key = "TITLE";
        else if (std::memcmp(chunk, "AUTH", 4) == 0) key = "ARTIST";
        else if (std::memcmp(chunk, "ANNO", 4) == 0) key = "GENRE";
        if (key && length > 0) {
            uint8_t value[512]{};
            const size_t wanted = std::min<size_t>(length, sizeof(value));
            if (std::fread(value, 1, wanted, file) == wanted) assign_tag(track, key, value, wanted);
        } else if (std::memcmp(chunk, "ID3 ", 4) == 0 && length >= 10) {
            uint8_t id3[10]{};
            if (std::fseek(file, static_cast<long>(data_start), SEEK_SET) == 0 &&
                std::fread(id3, 1, sizeof(id3), file) == sizeof(id3) &&
                std::memcmp(id3, "ID3", 3) == 0) {
                read_id3_text_tags(file, track, id3, static_cast<long>(data_start));
            }
        }
        position = data_start + length + (length & 1u);
    }
}

void read_fast_metadata(Track *track)
{
    if (!track) return;
    const char *extension = extension_of(track->path);
    if (!equals_ci(extension, "mp3") && !equals_ci(extension, "flac") &&
        !equals_ci(extension, "m4a") && !equals_ci(extension, "mp4") &&
        !equals_ci(extension, "ogg") && !equals_ci(extension, "opus") &&
        !equals_ci(extension, "wav") && !equals_ci(extension, "aiff") &&
        !equals_ci(extension, "aif") && !equals_ci(extension, "aifc")) return;
    FILE *file = std::fopen(track->path, "rb");
    if (!file) return;
    uint8_t header[10]{};
    const size_t read = std::fread(header, 1, sizeof(header), file);
    if (equals_ci(extension, "m4a") || equals_ci(extension, "mp4")) {
        read_mp4_text_tags(file, track);
    } else if (equals_ci(extension, "ogg") || equals_ci(extension, "opus")) {
        read_ogg_text_tags(file, track);
    } else if (equals_ci(extension, "wav")) {
        read_wav_text_tags(file, track);
    } else if (equals_ci(extension, "aiff") || equals_ci(extension, "aif") ||
               equals_ci(extension, "aifc")) {
        read_aiff_text_tags(file, track);
    } else if (read == sizeof(header) && std::memcmp(header, "ID3", 3) == 0) {
        read_id3_text_tags(file, track, header);
    } else if (read >= 4 && std::memcmp(header, "fLaC", 4) == 0) {
        read_flac_text_tags(file, track);
    }
    std::fclose(file);
}

} // namespace lyra::media::internal
