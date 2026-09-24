/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lyra_media_internal.h"

#include <climits>
#include <cinttypes>

namespace lyra::media {
namespace {

constexpr size_t kLyricsReadChunk = 4096;
constexpr size_t kMaximumEmbeddedFrameBytes = 64u * 1024u;
constexpr size_t kMaximumVorbisCommentBytes = 256u * 1024u;
constexpr size_t kOggCommentBufferBytes = internal::kMaximumOggCommentPacketBytes;

bool seek_to(FILE *file, uint64_t position)
{
    return file && position <= static_cast<uint64_t>(LONG_MAX) &&
           lyra::sd::seek(file, static_cast<long>(position), SEEK_SET,
                          lyra::sd::Client::Artwork) == 0;
}

bool read_exact(FILE *file, void *destination, size_t size)
{
    return lyra::sd::read_exact(file, destination, size,
                                lyra::sd::Client::Artwork, kLyricsReadChunk);
}

bool read_at(FILE *file, uint64_t position, void *destination, size_t size)
{
    return seek_to(file, position) && read_exact(file, destination, size);
}

bool append_utf8(char *destination, size_t capacity, size_t *used, uint32_t value)
{
    if (!destination || !used || capacity == 0) return false;
    uint8_t encoded[4]{};
    size_t length = 0;
    if (value <= 0x7F) {
        encoded[0] = static_cast<uint8_t>(value);
        length = 1;
    } else if (value <= 0x7FF) {
        encoded[0] = static_cast<uint8_t>(0xC0 | (value >> 6));
        encoded[1] = static_cast<uint8_t>(0x80 | (value & 0x3F));
        length = 2;
    } else if (value <= 0xFFFF) {
        encoded[0] = static_cast<uint8_t>(0xE0 | (value >> 12));
        encoded[1] = static_cast<uint8_t>(0x80 | ((value >> 6) & 0x3F));
        encoded[2] = static_cast<uint8_t>(0x80 | (value & 0x3F));
        length = 3;
    } else if (value <= 0x10FFFF) {
        encoded[0] = static_cast<uint8_t>(0xF0 | (value >> 18));
        encoded[1] = static_cast<uint8_t>(0x80 | ((value >> 12) & 0x3F));
        encoded[2] = static_cast<uint8_t>(0x80 | ((value >> 6) & 0x3F));
        encoded[3] = static_cast<uint8_t>(0x80 | (value & 0x3F));
        length = 4;
    } else {
        return true;
    }
    if (*used + length >= capacity) return false;
    std::memcpy(destination + *used, encoded, length);
    *used += length;
    return true;
}

size_t nul_terminated_text_length(const uint8_t *source, size_t length, uint8_t encoding)
{
    if (encoding == 1 || encoding == 2) {
        for (size_t i = 0; i + 1 < length; i += 2) {
            if (source[i] == 0 && source[i + 1] == 0) return i;
        }
    } else {
        const void *end = std::memchr(source, 0, length);
        if (end) return static_cast<const uint8_t *>(end) - source;
    }
    return length;
}

void decode_id3_text(const uint8_t *source, size_t length, uint8_t encoding,
                     char *destination, size_t capacity)
{
    if (!destination || capacity == 0) return;
    destination[0] = '\0';
    if (!source || length == 0) return;
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
        while (position + 1 < length) {
            uint16_t codepoint = little_endian
                ? static_cast<uint16_t>(source[position] | (source[position + 1] << 8))
                : static_cast<uint16_t>((source[position] << 8) | source[position + 1]);
            position += 2;
            if (codepoint == 0) break;
            if (codepoint >= 0xD800 && codepoint <= 0xDBFF && position + 1 < length) {
                const uint16_t low = little_endian
                    ? static_cast<uint16_t>(source[position] | (source[position + 1] << 8))
                    : static_cast<uint16_t>((source[position] << 8) | source[position + 1]);
                if (low >= 0xDC00 && low <= 0xDFFF) {
                    codepoint = 0x10000u + ((codepoint - 0xD800u) << 10) + (low - 0xDC00u);
                    position += 2;
                }
            }
            if (codepoint < 0x20 && codepoint != '\n' && codepoint != '\r' &&
                codepoint != '\t') continue;
            if (!append_utf8(destination, capacity, &used, codepoint)) break;
        }
    } else {
        for (size_t i = 0; i < length && source[i]; ++i) {
            const uint8_t value = source[i];
            if (value < 0x20 && value != '\n' && value != '\r' && value != '\t') continue;
            if (encoding == 0) {
                if (!append_utf8(destination, capacity, &used, value)) break;
            } else {
                if (used + 1 >= capacity) break;
                destination[used++] = static_cast<char>(value);
            }
        }
    }
    destination[used] = '\0';
}

void append_text(char *destination, size_t capacity, const char *text)
{
    if (!destination || capacity == 0 || !text) return;
    const size_t used = std::strlen(destination);
    if (used >= capacity - 1) return;
    const size_t copy_length = std::min(std::strlen(text), capacity - used - 1);
    std::memcpy(destination + used, text, copy_length);
    destination[used + copy_length] = '\0';
}

bool decode_id3_synced_lyrics(const uint8_t *frame, size_t frame_size,
                              char *destination, size_t capacity)
{
    if (!frame || frame_size < 8 || !destination || capacity == 0) return false;
    const uint8_t encoding = frame[0];
    const uint8_t timestamp_format = frame[4];
    size_t cursor = 6;
    const size_t descriptor_length = nul_terminated_text_length(
        frame + cursor, frame_size - cursor, encoding);
    cursor += descriptor_length + ((encoding == 1 || encoding == 2) ? 2 : 1);
    destination[0] = '\0';
    while (cursor < frame_size && cursor + 4 <= frame_size) {
        const size_t text_length = nul_terminated_text_length(
            frame + cursor, frame_size - cursor, encoding);
        if (text_length > frame_size - cursor) break;
        char line[2048]{};
        decode_id3_text(frame + cursor, text_length, encoding, line, sizeof(line));
        cursor += text_length + ((encoding == 1 || encoding == 2) ? 2 : 1);
        if (cursor + 4 > frame_size) break;
        uint32_t timestamp = internal::read_be32(frame + cursor);
        cursor += 4;
        if (timestamp_format == 1) timestamp = static_cast<uint32_t>(
            std::min<uint64_t>(static_cast<uint64_t>(timestamp) * 1000u / 75u, UINT32_MAX - 1));
        if (timestamp_format != 1 && timestamp_format != 2) return false;
        char tag[24];
        const uint32_t minutes = timestamp / 60000u;
        const uint32_t seconds = (timestamp / 1000u) % 60u;
        const uint32_t hundredths = (timestamp % 1000u) / 10u;
        std::snprintf(tag, sizeof(tag), "[%02" PRIu32 ":%02" PRIu32 ".%02" PRIu32 "]",
                      minutes, seconds, hundredths);
        append_text(destination, capacity, tag);
        append_text(destination, capacity, line);
        append_text(destination, capacity, "\n");
        if (std::strlen(destination) + 8 >= capacity) break;
    }
    return destination[0] != '\0';
}

void normalize_newlines(char *text, size_t capacity)
{
    if (!text || capacity == 0) return;
    size_t length = 0;
    while (length < capacity && text[length]) ++length;
    if (length >= 3 && static_cast<uint8_t>(text[0]) == 0xEF &&
        static_cast<uint8_t>(text[1]) == 0xBB &&
        static_cast<uint8_t>(text[2]) == 0xBF) {
        std::memmove(text, text + 3, length - 2);
        length -= 3;
    }
    size_t write = 0;
    for (size_t read = 0; read < length && write + 1 < capacity; ++read) {
        if (text[read] == '\r') {
            text[write++] = '\n';
            if (read + 1 < length && text[read + 1] == '\n') ++read;
        } else if (text[read] != '\0') {
            text[write++] = text[read];
        }
    }
    while (write > 0 && (text[write - 1] == '\n' || text[write - 1] == ' ' ||
                         text[write - 1] == '\t')) --write;
    text[write] = '\0';
}

bool extract_id3_lyrics(FILE *file, uint64_t tag_start,
                        char *destination, size_t capacity)
{
    uint8_t header[10]{};
    if (!read_at(file, tag_start, header, sizeof(header)) ||
        std::memcmp(header, "ID3", 3) != 0 || header[3] < 2 || header[3] > 4) return false;
    const uint8_t version = header[3];
    const uint32_t tag_size = internal::read_syncsafe32(header + 6);
    if (tag_size == 0) return false;
    const uint64_t tag_end = tag_start + 10u + tag_size;
    uint64_t position = tag_start + 10;
    if (header[5] & 0x40) {
        uint8_t extended[4]{};
        if (!read_at(file, position, extended, sizeof(extended))) return false;
        const uint32_t extended_size = version == 4 ? internal::read_syncsafe32(extended) :
                                                       internal::read_be32(extended);
        position += version == 4 ? extended_size : extended_size + 4u;
    }
    while (position + (version == 2 ? 6u : 10u) <= tag_end) {
        uint8_t frame_header[10]{};
        const size_t frame_header_size = version == 2 ? 6 : 10;
        if (!read_at(file, position, frame_header, frame_header_size) || frame_header[0] == 0) break;
        const uint32_t frame_size = version == 2
            ? ((static_cast<uint32_t>(frame_header[3]) << 16) |
               (static_cast<uint32_t>(frame_header[4]) << 8) | frame_header[5])
            : (version == 4 ? internal::read_syncsafe32(frame_header + 4) :
                              internal::read_be32(frame_header + 4));
        position += frame_header_size;
        if (frame_size == 0 || position + frame_size > tag_end) break;
        const bool unsynced_lyrics = (version == 2 &&
            std::memcmp(frame_header, "ULT", 3) == 0) || (version > 2 &&
            std::memcmp(frame_header, "USLT", 4) == 0);
        const bool synced_lyrics = (version == 2 &&
            std::memcmp(frame_header, "SLT", 3) == 0) || (version > 2 &&
            std::memcmp(frame_header, "SYLT", 4) == 0);
        const bool user_text = (version == 2 && std::memcmp(frame_header, "TXX", 3) == 0) ||
                               (version > 2 && std::memcmp(frame_header, "TXXX", 4) == 0);
        if ((unsynced_lyrics || synced_lyrics || user_text) &&
            frame_size <= kMaximumEmbeddedFrameBytes) {
            bool unsupported = false;
            const uint8_t frame_flags = version > 2 ? frame_header[9] : 0;
            if (version > 2) unsupported = version == 3 ? (frame_flags & 0xC0u) != 0 :
                                                         (frame_flags & 0x0Cu) != 0;
            auto *frame = static_cast<uint8_t *>(heap_caps_malloc(
                frame_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!frame) frame = static_cast<uint8_t *>(std::malloc(frame_size));
            if (!unsupported && frame && read_at(file, position, frame, frame_size) && frame_size >= 4) {
                size_t text_start = 0;
                if (version == 3 && (frame_flags & 0x20u)) ++text_start;
                if (version == 4) {
                    if (frame_flags & 0x40u) ++text_start;
                    if (frame_flags & 0x01u) text_start += 4;
                }
                size_t text_size = frame_size > text_start ? frame_size - text_start : 0;
                if ((header[5] & 0x80u) || (version == 4 && (frame_flags & 0x02u))) {
                    size_t write = 0;
                    for (size_t read = text_start; read < frame_size; ++read) {
                        frame[write++] = frame[read];
                        if (frame[read] == 0xFF && read + 1 < frame_size && frame[read + 1] == 0) ++read;
                    }
                    text_size = write;
                    text_start = 0;
                }
                const uint8_t encoding = text_size ? frame[text_start] : 0xFF;
                if ((unsynced_lyrics || user_text) && text_size > 4) {
                    size_t descriptor = text_start + 4;
                    const size_t descriptor_length = nul_terminated_text_length(
                        frame + descriptor, text_size - (descriptor - text_start), encoding);
                    if (user_text) {
                        char description[64]{};
                        decode_id3_text(frame + descriptor, descriptor_length, encoding,
                                        description, sizeof(description));
                        if (!internal::equals_ci(description, "LYRICS") &&
                            !internal::equals_ci(description, "UNSYNCED LYRICS") &&
                            !internal::equals_ci(description, "UNSYNCEDLYRICS")) {
                            descriptor = text_start + text_size;
                        }
                    }
                    descriptor += descriptor_length;
                    descriptor += (encoding == 1 || encoding == 2) ? 2 : 1;
                    if ((unsynced_lyrics || user_text) && descriptor < text_start + text_size) {
                        decode_id3_text(frame + descriptor, text_start + text_size - descriptor,
                                        encoding, destination, capacity);
                        normalize_newlines(destination, capacity);
                    }
                } else if (synced_lyrics && text_size > 6) {
                    decode_id3_synced_lyrics(frame + text_start, text_size,
                                             destination, capacity);
                    normalize_newlines(destination, capacity);
                }
            }
            heap_caps_free(frame);
            if (destination[0]) return true;
        }
        position += frame_size;
    }
    return false;
}

bool copy_vorbis_lyrics(const uint8_t *data, size_t length, size_t cursor,
                        char *destination, size_t capacity)
{
    if (!data || cursor + 8 > length) return false;
    const uint32_t vendor_length = internal::read_le32(data + cursor);
    cursor += 4;
    if (vendor_length > length - cursor) return false;
    cursor += vendor_length;
    if (cursor + 4 > length) return false;
    const uint32_t comment_count = internal::read_le32(data + cursor);
    cursor += 4;
    for (uint32_t i = 0; i < comment_count && cursor + 4 <= length; ++i) {
        const uint32_t comment_length = internal::read_le32(data + cursor);
        cursor += 4;
        if (comment_length > length - cursor) return false;
        const uint8_t *equals = static_cast<const uint8_t *>(
            std::memchr(data + cursor, '=', comment_length));
        if (equals) {
            char key[32]{};
            const size_t key_length = std::min<size_t>(
                equals - (data + cursor), sizeof(key) - 1);
            std::memcpy(key, data + cursor, key_length);
            if (internal::equals_ci(key, "LYRICS") ||
                internal::equals_ci(key, "SYNCEDLYRICS") ||
                internal::equals_ci(key, "UNSYNCEDLYRICS") ||
                internal::equals_ci(key, "LYRIC")) {
                const uint8_t *value = equals + 1;
                const size_t value_length = comment_length -
                    static_cast<size_t>(value - (data + cursor));
                const size_t copy_length = std::min(value_length, capacity - 1);
                std::memcpy(destination, value, copy_length);
                destination[copy_length] = '\0';
                normalize_newlines(destination, capacity);
                if (destination[0]) return true;
            }
        }
        cursor += comment_length;
    }
    return false;
}

bool extract_flac_lyrics(FILE *file, char *destination, size_t capacity)
{
    uint64_t position = 4;
    bool last = false;
    while (!last) {
        uint8_t block_header[4]{};
        if (!read_at(file, position, block_header, sizeof(block_header))) return false;
        last = (block_header[0] & 0x80u) != 0;
        const uint8_t type = block_header[0] & 0x7Fu;
        const uint32_t block_length = (static_cast<uint32_t>(block_header[1]) << 16) |
                                      (static_cast<uint32_t>(block_header[2]) << 8) |
                                      block_header[3];
        position += 4;
        if (type == 4 && block_length >= 8 && block_length <= kMaximumVorbisCommentBytes) {
            auto *block = static_cast<uint8_t *>(heap_caps_malloc(
                block_length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!block) block = static_cast<uint8_t *>(std::malloc(block_length));
            if (!block) return false;
            const bool read = read_at(file, position, block, block_length);
            const bool found = read && copy_vorbis_lyrics(block, block_length, 0,
                                                          destination, capacity);
            heap_caps_free(block);
            if (found) return true;
        }
        position += block_length;
    }
    return false;
}

bool extract_ogg_lyrics(FILE *file, char *destination, size_t capacity)
{
    auto *packet = static_cast<uint8_t *>(heap_caps_malloc(
        kOggCommentBufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!packet) packet = static_cast<uint8_t *>(std::malloc(kOggCommentBufferBytes));
    if (!packet) return false;
    size_t packet_length = 0;
    bool found = false;
    if (internal::read_ogg_comment_packet(file, packet, kOggCommentBufferBytes,
                                          &packet_length)) {
        if (packet_length >= 8 && std::memcmp(packet, "OpusTags", 8) == 0) {
            found = copy_vorbis_lyrics(packet, packet_length, 8, destination, capacity);
        } else if (packet_length >= 7 && packet[0] == 3 &&
                   std::memcmp(packet + 1, "vorbis", 6) == 0) {
            found = copy_vorbis_lyrics(packet, packet_length, 7, destination, capacity);
        }
    }
    heap_caps_free(packet);
    return found;
}

bool mp4_read_atom(FILE *file, uint64_t position, uint64_t limit,
                   internal::Mp4Atom *atom)
{
    if (!file || !atom || position + 8 > limit) return false;
    uint8_t header[16]{};
    if (!read_at(file, position, header, 8)) return false;
    uint64_t size = internal::read_be32(header);
    uint8_t header_size = 8;
    if (size == 1) {
        if (position + 16 > limit || !read_at(file, position + 8, header + 8, 8)) return false;
        size = internal::read_be64(header + 8);
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

bool atom_is(const internal::Mp4Atom &atom, const char (&type)[5])
{
    return std::memcmp(atom.type, type, 4) == 0;
}

bool read_mp4_lyrics_item(FILE *file, const internal::Mp4Atom &item,
                          char *destination, size_t capacity)
{
    uint64_t cursor = item.data_start;
    while (cursor + 8 <= item.end) {
        internal::Mp4Atom child{};
        if (!mp4_read_atom(file, cursor, item.end, &child)) return false;
        if (atom_is(child, "data") && child.data_size > 8) {
            const size_t value_length = std::min<size_t>(child.data_size - 8, capacity - 1);
            if (read_at(file, child.data_start + 8, destination, value_length)) {
                destination[value_length] = '\0';
                normalize_newlines(destination, capacity);
                return destination[0] != '\0';
            }
        }
        cursor = child.end;
    }
    return false;
}

bool mp4_walk_lyrics(FILE *file, uint64_t start, uint64_t end, unsigned depth,
                     char *destination, size_t capacity)
{
    if (depth > 8) return false;
    uint64_t cursor = start;
    while (cursor + 8 <= end) {
        internal::Mp4Atom atom{};
        if (!mp4_read_atom(file, cursor, end, &atom)) return false;
        if (atom_is(atom, "ilst")) {
            uint64_t item_cursor = atom.data_start;
            while (item_cursor + 8 <= atom.end) {
                internal::Mp4Atom item{};
                if (!mp4_read_atom(file, item_cursor, atom.end, &item)) break;
                if (std::memcmp(item.type, "\xA9" "lyr", 4) == 0 &&
                    read_mp4_lyrics_item(file, item, destination, capacity)) return true;
                item_cursor = item.end;
            }
        } else {
            const bool container = atom_is(atom, "moov") || atom_is(atom, "trak") ||
                atom_is(atom, "mdia") || atom_is(atom, "minf") || atom_is(atom, "stbl") ||
                atom_is(atom, "udta") || atom_is(atom, "meta") || atom_is(atom, "edts") ||
                atom_is(atom, "dinf") || atom_is(atom, "mvex") || atom_is(atom, "moof") ||
                atom_is(atom, "traf");
            if (container) {
                uint64_t child_start = atom.data_start;
                if (atom_is(atom, "meta")) child_start += 4;
                if (mp4_walk_lyrics(file, child_start, atom.end, depth + 1,
                                    destination, capacity)) return true;
            }
        }
        cursor = atom.end;
    }
    return false;
}

bool extract_mp4_lyrics(FILE *file, char *destination, size_t capacity)
{
    if (lyra::sd::seek(file, 0, SEEK_END, lyra::sd::Client::Artwork) != 0) return false;
    const long file_end = std::ftell(file);
    if (file_end < 12) return false;
    uint64_t cursor = 0;
    while (cursor + 8 <= static_cast<uint64_t>(file_end)) {
        internal::Mp4Atom atom{};
        if (!mp4_read_atom(file, cursor, file_end, &atom)) return false;
        if (atom_is(atom, "moov")) {
            return mp4_walk_lyrics(file, atom.data_start, atom.end, 0,
                                   destination, capacity);
        }
        cursor = atom.end;
    }
    return false;
}

bool extract_riff_id3(FILE *file, bool aiff, char *destination, size_t capacity)
{
    uint8_t header[12]{};
    if (!read_at(file, 0, header, sizeof(header))) return false;
    const bool valid = aiff ? std::memcmp(header, "FORM", 4) == 0 :
                              std::memcmp(header, "RIFF", 4) == 0;
    if (!valid) return false;
    uint64_t position = 12;
    for (size_t chunk_index = 0; chunk_index < 256; ++chunk_index) {
        uint8_t chunk_header[8]{};
        if (!read_at(file, position, chunk_header, sizeof(chunk_header))) return false;
        const uint32_t chunk_size = aiff ? internal::read_be32(chunk_header + 4) :
                                           internal::read_le32(chunk_header + 4);
        const bool id3_chunk = aiff
            ? std::memcmp(chunk_header, "ID3 ", 4) == 0
            : (std::memcmp(chunk_header, "id3 ", 4) == 0 ||
               std::memcmp(chunk_header, "ID3 ", 4) == 0);
        if (id3_chunk && chunk_size >= 10 &&
            extract_id3_lyrics(file, position + 8, destination, capacity)) return true;
        position += 8u + chunk_size + (chunk_size & 1u);
    }
    return false;
}

bool embedded_lyrics(const Track &track, char *destination, size_t capacity)
{
    FILE *file = lyra::sd::open(track.path, "rb", lyra::sd::Client::Artwork);
    if (!file) return false;
    const char *extension = internal::extension_of(track.path);
    uint8_t header[10]{};
    const bool header_read = read_at(file, 0, header, sizeof(header));
    bool found = false;
    if ((internal::equals_ci(extension, "m4a") || internal::equals_ci(extension, "mp4"))) {
        found = extract_mp4_lyrics(file, destination, capacity);
    } else if (internal::equals_ci(extension, "flac")) {
        found = header_read && std::memcmp(header, "fLaC", 4) == 0 &&
                extract_flac_lyrics(file, destination, capacity);
    } else if (internal::equals_ci(extension, "ogg") ||
               internal::equals_ci(extension, "opus")) {
        found = extract_ogg_lyrics(file, destination, capacity);
    } else if (internal::equals_ci(extension, "wav")) {
        found = extract_riff_id3(file, false, destination, capacity);
    } else if (internal::equals_ci(extension, "aiff") ||
               internal::equals_ci(extension, "aif") ||
               internal::equals_ci(extension, "aifc")) {
        found = extract_riff_id3(file, true, destination, capacity);
    } else if (header_read && std::memcmp(header, "ID3", 3) == 0) {
        found = extract_id3_lyrics(file, 0, destination, capacity);
    }
    lyra::sd::close(file, lyra::sd::Client::Artwork);
    return found;
}

bool make_sidecar_path(const char *audio_path, const char *extension,
                       char *destination, size_t capacity)
{
    if (!audio_path || !extension || !destination || capacity == 0) return false;
    const size_t path_length = std::strlen(audio_path);
    if (path_length + 5 >= capacity) return false;
    internal::copy_text(destination, capacity, audio_path);
    char *slash = std::strrchr(destination, '/');
    char *dot = std::strrchr(destination, '.');
    if (dot && (!slash || dot > slash)) *dot = '\0';
    const size_t stem_length = std::strlen(destination);
    const size_t extension_length = std::strlen(extension);
    if (stem_length + extension_length + 2 > capacity) return false;
    destination[stem_length] = '.';
    std::memcpy(destination + stem_length + 1, extension, extension_length + 1);
    return true;
}

bool read_text_file(const char *path, char *destination, size_t capacity)
{
    if (!path || !destination || capacity == 0) return false;
    FILE *file = lyra::sd::open(path, "rb", lyra::sd::Client::Artwork);
    if (!file) return false;
    size_t used = 0;
    while (used + 1 < capacity) {
        const size_t wanted = std::min(kLyricsReadChunk, capacity - used - 1);
        const size_t count = lyra::sd::read(file, destination + used, wanted,
                                             lyra::sd::Client::Artwork);
        used += count;
        if (count < wanted) break;
        vTaskDelay(1);
    }
    lyra::sd::close(file, lyra::sd::Client::Artwork);
    destination[used] = '\0';
    normalize_newlines(destination, capacity);
    return destination[0] != '\0';
}

} // namespace

bool load_lyrics(const Track &track, char *destination, size_t capacity,
                 LyricsSource *source)
{
    if (source) *source = LyricsSource::None;
    if (!destination || capacity == 0) return false;
    destination[0] = '\0';
    if (embedded_lyrics(track, destination, capacity)) {
        if (source) *source = LyricsSource::Embedded;
        return true;
    }

    char sidecar[kMaxPath + 8]{};
    if (make_sidecar_path(track.path, "lrc", sidecar, sizeof(sidecar)) &&
        read_text_file(sidecar, destination, capacity)) {
        if (source) *source = LyricsSource::Lrc;
        return true;
    }
    destination[0] = '\0';
    if (make_sidecar_path(track.path, "txt", sidecar, sizeof(sidecar)) &&
        read_text_file(sidecar, destination, capacity)) {
        if (source) *source = LyricsSource::Text;
        return true;
    }
    destination[0] = '\0';
    return false;
}

} // namespace lyra::media
