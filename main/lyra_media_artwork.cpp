/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lyra_media_artwork.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <sys/types.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lyra_png.h"
#include "lyra_progressive_jpeg.h"
#include "lyra_sd.h"

namespace lyra::media::artwork {
namespace {

constexpr const char *kTag = "lyra.media.artwork";
constexpr const char *kDataDir = "/sdcard/.lyra";
constexpr const char *kArtworkDir = "/sdcard/.lyra/covers";
constexpr size_t kArtworkReadChunkBytes = 8192;
constexpr size_t kMaximumEmbeddedImageBytes = 16u * 1024u * 1024u;
constexpr size_t kMaximumOggCommentPacketBytes = 2u * 1024u * 1024u;
constexpr uint32_t kLargeArtworkCacheVersion = 1;

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

bool equals_ci_bytes(const uint8_t *bytes, size_t length, const char *text)
{
    if (!bytes || !text) return false;
    size_t index = 0;
    for (; index < length && text[index]; ++index) {
        const unsigned char left = static_cast<unsigned char>(std::tolower(bytes[index]));
        const unsigned char right = static_cast<unsigned char>(std::tolower(text[index]));
        if (left != right) return false;
    }
    return index == length && text[index] == '\0';
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
                           lyra::sd::Client::Artwork) != sizeof(page_header) ||
            std::memcmp(page_header, "OggS", 4) != 0 || page_header[4] != 0) return false;
        const size_t segment_count = page_header[26];
        if (lyra::sd::read(file, lacing, segment_count,
                           lyra::sd::Client::Artwork) != segment_count) return false;
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
                                                     lyra::sd::Client::Artwork);
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

bool ensure_directory(const char *path)
{
    if (mkdir(path, 0775) == 0) return true;
    struct stat info{};
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

} // namespace

uint32_t key(const Track &track)
{
    uint32_t hash = 2166136261u;
    const auto update = [&hash](const char *text) {
        for (const uint8_t *p = reinterpret_cast<const uint8_t *>(text); *p; ++p) {
            hash = (hash ^ *p) * 16777619u;
        }
    };
    // The Albums browser groups on Track::album, so use the same identity
    // here. This guarantees every song in an album resolves to one shared
    // decoded source and one in-memory display asset.
    update(track.album);
    return hash;
}

uint64_t request_key(const Track &track, ArtworkSize size)
{
    (void)size;
    return key(track);
}


enum class ArtworkFormat : uint8_t { Jpeg, Png };

struct ArtworkBlob {
    uint8_t *data;
    size_t length;
    size_t capacity;
    ArtworkFormat format;
    bool psram;
};

void free_artwork_blob(ArtworkBlob *blob)
{
    if (!blob) return;
    heap_caps_free(blob->data);
    *blob = {};
}

void *alloc_artwork_pixels(size_t size)
{
    // Artwork is read by the CPU and never submitted directly to a DMA
    // engine. Keep it exclusively in PSRAM so a large cover can never consume
    // the scarce internal heap used by the LCD transport and audio DMA.
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

bool grow_artwork_blob(ArtworkBlob *blob, size_t wanted)
{
    if (!blob || wanted > kMaximumEmbeddedImageBytes) return false;
    if (wanted <= blob->capacity) return true;
    size_t capacity = blob->capacity == 0 ? 16 * 1024 : blob->capacity;
    while (capacity < wanted) {
        if (capacity > kMaximumEmbeddedImageBytes / 2) {
            capacity = kMaximumEmbeddedImageBytes;
            break;
        }
        capacity *= 2;
    }
    // The compressed source is also CPU-only artwork memory. Do not fall
    // back to internal SRAM when PSRAM is fragmented; preserving the LCD and
    // I2S DMA heap is more important than showing a cover on that request.
    auto *next = static_cast<uint8_t *>(heap_caps_malloc(
        capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!next) return false;
    if (blob->data && blob->length) std::memcpy(next, blob->data, blob->length);
    heap_caps_free(blob->data);
    blob->data = next;
    blob->capacity = capacity;
    return true;
}

bool append_artwork_byte(ArtworkBlob *blob, uint8_t value)
{
    if (!grow_artwork_blob(blob, blob->length + 1)) return false;
    blob->data[blob->length++] = value;
    return true;
}

bool copy_embedded_image_from_memory(const uint8_t *data, size_t length,
                                     ArtworkBlob *blob)
{
    if (!data || !blob || length < 4) return false;
    constexpr uint8_t kPngSignature[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    size_t image_start = SIZE_MAX;
    ArtworkFormat format = ArtworkFormat::Jpeg;
    for (size_t index = 0; index + 3 <= length; ++index) {
        if (index + sizeof(kPngSignature) <= length &&
            std::memcmp(data + index, kPngSignature, sizeof(kPngSignature)) == 0) {
            image_start = index;
            format = ArtworkFormat::Png;
            break;
        }
        if (data[index] == 0xFF && data[index + 1] == 0xD8 && data[index + 2] == 0xFF) {
            image_start = index;
            format = ArtworkFormat::Jpeg;
            break;
        }
    }
    if (image_start == SIZE_MAX || length - image_start > kMaximumEmbeddedImageBytes) return false;
    const bool prefer_psram = blob->psram;
    free_artwork_blob(blob);
    blob->psram = prefer_psram;
    blob->format = format;
    if (!grow_artwork_blob(blob, length - image_start)) return false;
    std::memcpy(blob->data, data + image_start, length - image_start);
    blob->length = length - image_start;
    return true;
}

int base64_digit(uint8_t value)
{
    if (value >= 'A' && value <= 'Z') return value - 'A';
    if (value >= 'a' && value <= 'z') return value - 'a' + 26;
    if (value >= '0' && value <= '9') return value - '0' + 52;
    if (value == '+') return 62;
    if (value == '/') return 63;
    return -1;
}

bool decode_base64(const uint8_t *encoded, size_t length, uint8_t *decoded,
                   size_t capacity, size_t *decoded_length)
{
    if (!encoded || !decoded || !decoded_length) return false;
    size_t output = 0;
    uint32_t accumulator = 0;
    unsigned bits = 0;
    for (size_t index = 0; index < length; ++index) {
        if (encoded[index] == '=' || std::isspace(encoded[index])) continue;
        const int digit = base64_digit(encoded[index]);
        if (digit < 0) return false;
        accumulator = (accumulator << 6) | static_cast<uint32_t>(digit);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (output >= capacity) return false;
            decoded[output++] = static_cast<uint8_t>((accumulator >> bits) & 0xFFu);
        }
    }
    *decoded_length = output;
    return output > 0;
}

bool find_ogg_comment(const uint8_t *data, size_t length, size_t cursor,
                      const char *wanted_key, const uint8_t **value,
                      size_t *value_length)
{
    if (!data || !wanted_key || !value || !value_length || cursor + 8 > length) return false;
    const uint32_t vendor_length = read_le32(data + cursor);
    cursor += 4;
    if (vendor_length > length - cursor) return false;
    cursor += vendor_length;
    if (cursor + 4 > length) return false;
    const uint32_t comment_count = read_le32(data + cursor);
    cursor += 4;
    for (uint32_t index = 0; index < comment_count && cursor + 4 <= length; ++index) {
        const uint32_t comment_length = read_le32(data + cursor);
        cursor += 4;
        if (comment_length > length - cursor) return false;
        const uint8_t *equals = static_cast<const uint8_t *>(
            std::memchr(data + cursor, '=', comment_length));
        if (equals && equals_ci_bytes(data + cursor,
                                      static_cast<size_t>(equals - (data + cursor)), wanted_key)) {
            *value = equals + 1;
            *value_length = comment_length - static_cast<size_t>(equals + 1 - (data + cursor));
            return true;
        }
        cursor += comment_length;
    }
    return false;
}

bool extract_ogg_image(FILE *file, ArtworkBlob *blob)
{
    auto *packet = static_cast<uint8_t *>(heap_caps_malloc(
        kMaximumOggCommentPacketBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!packet) packet = static_cast<uint8_t *>(std::malloc(kMaximumOggCommentPacketBytes));
    if (!packet) return false;
    size_t packet_length = 0;
    bool result = false;
    if (read_ogg_comment_packet(file, packet, kMaximumOggCommentPacketBytes, &packet_length)) {
        size_t comment_start = 0;
        if (packet_length >= 8 && std::memcmp(packet, "OpusTags", 8) == 0) comment_start = 8;
        else if (packet_length >= 7 && packet[0] == 3 &&
                 std::memcmp(packet + 1, "vorbis", 6) == 0) comment_start = 7;
        if (comment_start != 0) {
            const uint8_t *encoded = nullptr;
            size_t encoded_length = 0;
            if (find_ogg_comment(packet, packet_length, comment_start,
                                 "METADATA_BLOCK_PICTURE", &encoded, &encoded_length) ||
                find_ogg_comment(packet, packet_length, comment_start,
                                 "COVERART", &encoded, &encoded_length)) {
                const size_t capacity = std::min<size_t>(
                    kMaximumEmbeddedImageBytes + 1024,
                    (encoded_length / 4u) * 3u + 4u);
                auto *decoded = static_cast<uint8_t *>(heap_caps_malloc(
                    capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                if (!decoded) decoded = static_cast<uint8_t *>(std::malloc(capacity));
                if (decoded) {
                    size_t decoded_length = 0;
                    if (decode_base64(encoded, encoded_length, decoded, capacity,
                                      &decoded_length)) {
                        result = copy_embedded_image_from_memory(decoded, decoded_length, blob);
                    }
                    heap_caps_free(decoded);
                }
            }
        }
    }
    heap_caps_free(packet);
    return result;
}

// Reads only the embedded payload's actual image. The SD gate is taken for
// each 8 KiB transaction and is fully released before any decoder runs.
bool read_embedded_image(FILE *source, uint64_t offset, uint32_t max_length,
                         ArtworkBlob *blob)
{
    if (!source || !blob || max_length < 4 || max_length > kMaximumEmbeddedImageBytes) return false;

    // A tag can contain more than one picture. Do not let a malformed first
    // candidate contaminate the next candidate's in-memory source.
    const bool prefer_psram = blob->psram;
    free_artwork_blob(blob);
    blob->psram = prefer_psram;
    if (lyra::sd::seek(source, static_cast<long>(offset), SEEK_SET,
                       lyra::sd::Client::Artwork) != 0) return false;

    enum class JpegState : uint8_t {
        MarkerCode,
        SegmentLengthHigh,
        SegmentLengthLow,
        SegmentData,
        Entropy,
        EntropyMarkerCode,
    };
    JpegState jpeg_state = JpegState::MarkerCode;
    uint8_t jpeg_marker = 0;
    uint16_t jpeg_segment_length = 0;
    size_t jpeg_segment_remaining = 0;
    bool jpeg_segment_to_entropy = false;

    uint8_t buffer[kArtworkReadChunkBytes];
    uint8_t window[8]{};
    size_t window_length = 0;
    size_t remaining = max_length;
    bool found = false;
    constexpr uint8_t png_signature[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    uint8_t png_header[8]{};
    size_t png_header_length = 0;
    uint32_t png_chunk_length = 0;
    size_t png_chunk_remaining = 0;
    size_t png_crc_remaining = 0;
    char png_chunk_type[5]{};

    while (remaining > 0) {
        const size_t wanted = std::min(remaining, sizeof(buffer));
        const size_t count = lyra::sd::read(source, buffer, wanted, lyra::sd::Client::Artwork);
        if (count == 0) break;
        remaining -= count;
        for (size_t i = 0; i < count; ++i) {
            const uint8_t value = buffer[i];
            if (!found) {
                if (window_length < sizeof(window)) window[window_length++] = value;
                else {
                    std::memmove(window, window + 1, sizeof(window) - 1);
                    window[sizeof(window) - 1] = value;
                }
                const bool jpeg = window_length >= 3 &&
                    window[window_length - 3] == 0xFF &&
                    window[window_length - 2] == 0xD8 &&
                    window[window_length - 1] == 0xFF;
                const bool png = window_length == sizeof(png_signature) &&
                    std::memcmp(window, png_signature, sizeof(png_signature)) == 0;
                if (jpeg || png) {
                    found = true;
                    blob->format = png ? ArtworkFormat::Png : ArtworkFormat::Jpeg;
                    const size_t signature_length = png ? sizeof(png_signature) : 3;
                    for (size_t j = window_length - signature_length; j < window_length; ++j) {
                        if (!append_artwork_byte(blob, window[j])) return false;
                    }
                    window_length = 0;
                }
                continue;
            }

            if (!append_artwork_byte(blob, value)) return false;
            if (blob->format == ArtworkFormat::Jpeg) {
                // EOI is only meaningful as a JPEG marker. Do not stop on
                // FF D9 bytes inside APP/EXIF/ICC payloads or entropy data.
                switch (jpeg_state) {
                case JpegState::MarkerCode:
                    if (value == 0xFF) break; // Marker fill bytes.
                    if (value == 0xD9) return true;
                    if (value == 0xD8 || value == 0x01 ||
                        (value >= 0xD0 && value <= 0xD7)) break;
                    jpeg_marker = value;
                    jpeg_state = JpegState::SegmentLengthHigh;
                    break;
                case JpegState::SegmentLengthHigh:
                    jpeg_segment_length = static_cast<uint16_t>(value) << 8;
                    jpeg_state = JpegState::SegmentLengthLow;
                    break;
                case JpegState::SegmentLengthLow:
                    jpeg_segment_length = static_cast<uint16_t>(jpeg_segment_length | value);
                    if (jpeg_segment_length < 2) return false;
                    jpeg_segment_remaining = jpeg_segment_length - 2;
                    jpeg_segment_to_entropy = jpeg_marker == 0xDA;
                    jpeg_state = jpeg_segment_remaining == 0
                        ? (jpeg_segment_to_entropy ? JpegState::Entropy : JpegState::MarkerCode)
                        : JpegState::SegmentData;
                    break;
                case JpegState::SegmentData:
                    if (jpeg_segment_remaining > 0) --jpeg_segment_remaining;
                    if (jpeg_segment_remaining == 0) {
                        jpeg_state = jpeg_segment_to_entropy ? JpegState::Entropy :
                                                                JpegState::MarkerCode;
                    }
                    break;
                case JpegState::Entropy:
                    if (value == 0xFF) jpeg_state = JpegState::EntropyMarkerCode;
                    break;
                case JpegState::EntropyMarkerCode:
                    if (value == 0x00 || (value >= 0xD0 && value <= 0xD7)) {
                        jpeg_state = JpegState::Entropy;
                    } else if (value == 0xD9) {
                        return true;
                    } else if (value == 0xFF) {
                        // Additional marker fill byte.
                    } else {
                        jpeg_marker = value;
                        jpeg_segment_to_entropy = true;
                        jpeg_state = JpegState::SegmentLengthHigh;
                    }
                    break;
                }
            } else {
                // A PNG chunk has a four-byte length, four-byte type, data,
                // and four-byte CRC. Parsing the chunk framing avoids a false
                // IEND match inside compressed IDAT data.
                if (png_header_length < sizeof(png_header)) {
                    png_header[png_header_length++] = value;
                    if (png_header_length == sizeof(png_header)) {
                        png_chunk_length = (static_cast<uint32_t>(png_header[0]) << 24) |
                            (static_cast<uint32_t>(png_header[1]) << 16) |
                            (static_cast<uint32_t>(png_header[2]) << 8) | png_header[3];
                        std::memcpy(png_chunk_type, png_header + 4, 4);
                        png_chunk_type[4] = '\0';
                        if (png_chunk_length > kMaximumEmbeddedImageBytes) return false;
                        png_chunk_remaining = png_chunk_length;
                        png_crc_remaining = 4;
                    }
                } else if (png_chunk_remaining > 0) {
                    --png_chunk_remaining;
                } else if (png_crc_remaining > 0) {
                    --png_crc_remaining;
                    if (png_crc_remaining == 0) {
                        if (std::strcmp(png_chunk_type, "IEND") == 0 && png_chunk_length == 0) {
                            return true;
                        }
                        png_header_length = 0;
                    }
                }
            }
        }
        if (remaining > 0) vTaskDelay(1);
    }
    return found && blob->length > 0;
}

bool decode_artwork_blob(const ArtworkBlob &blob, uint16_t *pixels,
                         uint16_t target_size, bool preserve_aspect,
                         bool allow_sd_backing,
                         bool *used_sd_backing)
{
    if (used_sd_backing) *used_sd_backing = false;
    if (!blob.data || blob.length == 0 || !pixels || target_size == 0) return false;
    std::fill(pixels, pixels + static_cast<size_t>(target_size) * target_size,
              static_cast<uint16_t>(0x1104));
    if (blob.format == ArtworkFormat::Png) {
        return decode_png(blob.data, blob.length, pixels, target_size, target_size,
                          preserve_aspect, 0x102020);
    }

    // libjpeg-turbo handles baseline, progressive, uncommon chroma sampling,
    // grayscale, and CMYK JPEGs consistently. TinyJPEG was faster for a narrow
    // subset but returned format errors for valid embedded covers.
    return decode_progressive_jpeg(blob.data, blob.length, pixels, target_size,
                                   preserve_aspect, allow_sd_backing, used_sd_backing);
}

void artwork_cache_path(uint32_t key, uint16_t artwork_size, char *path, size_t capacity)
{
    if (!path || capacity == 0) return;
    std::snprintf(path, capacity, "%s/%08lx.large-v%u.%u.rgb565", kArtworkDir,
                  static_cast<unsigned long>(key),
                  static_cast<unsigned>(kLargeArtworkCacheVersion),
                  static_cast<unsigned>(artwork_size));
}

bool valid_artwork_cache(const char *path, uint16_t artwork_size)
{
    struct stat info{};
    const off_t expected = static_cast<off_t>(artwork_size) * artwork_size * sizeof(uint16_t);
    return path && stat(path, &info) == 0 && S_ISREG(info.st_mode) &&
           info.st_size == expected;
}

bool read_cache(uint32_t key, uint16_t artwork_size, uint16_t **out_pixels)
{
    if (!out_pixels) return false;
    *out_pixels = nullptr;
    char path[kMaxPath];
    artwork_cache_path(key, artwork_size, path, sizeof(path));
    if (!valid_artwork_cache(path, artwork_size)) return false;

    const size_t pixel_count = static_cast<size_t>(artwork_size) * artwork_size;
    auto *pixels = static_cast<uint16_t *>(alloc_artwork_pixels(
        pixel_count * sizeof(uint16_t)));
    FILE *input = pixels ? lyra::sd::open(path, "rb", lyra::sd::Client::Artwork) : nullptr;
    const bool read = input && lyra::sd::read_exact(
        input, pixels, pixel_count * sizeof(uint16_t), lyra::sd::Client::Artwork);
    if (input) lyra::sd::close(input, lyra::sd::Client::Artwork);
    if (!read) {
        heap_caps_free(pixels);
        lyra::sd::remove(path, lyra::sd::Client::Artwork);
        return false;
    }
    *out_pixels = pixels;
    ESP_LOGI(kTag, "artwork loaded from large-image SD cache: key=%08lx",
             static_cast<unsigned long>(key));
    return true;
}

bool write_cache(uint32_t key, uint16_t artwork_size, const uint16_t *pixels)
{
    if (!pixels || !ensure_directory(kDataDir) || !ensure_directory(kArtworkDir)) return false;
    char path[kMaxPath];
    char temporary[kMaxPath];
    artwork_cache_path(key, artwork_size, path, sizeof(path));
    const int length = std::snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(temporary)) return false;

    const size_t bytes = static_cast<size_t>(artwork_size) * artwork_size * sizeof(uint16_t);
    FILE *output = lyra::sd::open(temporary, "wb", lyra::sd::Client::Artwork);
    const bool unbuffered = output && std::setvbuf(output, nullptr, _IONBF, 0) == 0;
    const bool written = unbuffered && lyra::sd::write_exact(
        output, pixels, bytes, lyra::sd::Client::Artwork);
    const bool closed = !output || lyra::sd::close(output, lyra::sd::Client::Artwork) == 0;
    if (!written || !closed) {
        lyra::sd::remove(temporary, lyra::sd::Client::Artwork);
        return false;
    }
    lyra::sd::remove(path, lyra::sd::Client::Artwork);
    if (lyra::sd::rename(temporary, path, lyra::sd::Client::Artwork) != 0) {
        lyra::sd::remove(temporary, lyra::sd::Client::Artwork);
        return false;
    }
    ESP_LOGI(kTag, "artwork persisted after SD-backed decode: key=%08lx bytes=%u",
             static_cast<unsigned long>(key), static_cast<unsigned>(bytes));
    return true;
}

bool extract_id3_image(FILE *file, const uint8_t header[10], ArtworkBlob *blob,
                       long tag_start = 0);
bool extract_flac_image(FILE *file, ArtworkBlob *blob);
bool extract_mp4_image(FILE *file, ArtworkBlob *blob);
bool extract_ogg_image(FILE *file, ArtworkBlob *blob);
bool extract_wav_image(FILE *file, ArtworkBlob *blob);
bool extract_aiff_image(FILE *file, ArtworkBlob *blob);

bool extract_embedded_image_from_track(FILE *file, const uint8_t header[10], ArtworkBlob *blob)
{
    if (!file || !blob) return false;
    if (std::memcmp(header, "ID3", 3) == 0) {
        return extract_id3_image(file, header, blob);
    }
    if (std::memcmp(header, "fLaC", 4) == 0) {
        return extract_flac_image(file, blob);
    }
    if (std::memcmp(header + 4, "ftyp", 4) == 0) {
        return extract_mp4_image(file, blob);
    }
    if (std::memcmp(header, "OggS", 4) == 0) {
        return extract_ogg_image(file, blob);
    }
    if (std::memcmp(header, "RIFF", 4) == 0) {
        return extract_wav_image(file, blob);
    }
    if (std::memcmp(header, "FORM", 4) == 0) {
        return extract_aiff_image(file, blob);
    }
    return false;
}

bool extract_id3_image(FILE *file, const uint8_t header[10], ArtworkBlob *blob,
                       long tag_start)
{
    const uint8_t version = header[3];
    if (version < 2 || version > 4) return false;
    const uint32_t tag_size = read_syncsafe32(header + 6);
    if (tag_size == 0) return false;
    uint64_t position = 10;
    const uint64_t tag_end = position + tag_size;

    while (position + (version == 2 ? 6u : 10u) <= tag_end) {
        if (lyra::sd::seek(file, tag_start + static_cast<long>(position), SEEK_SET,
                           lyra::sd::Client::Artwork) != 0) return false;
        uint8_t frame_header[10]{};
        const size_t header_size = version == 2 ? 6 : 10;
        if (lyra::sd::read(file, frame_header, header_size,
                           lyra::sd::Client::Artwork) != header_size) return false;
        if (frame_header[0] == 0) break;
        const bool picture = version == 2 ? std::memcmp(frame_header, "PIC", 3) == 0 :
                                            std::memcmp(frame_header, "APIC", 4) == 0;
        const uint32_t frame_size = version == 2 ?
            ((static_cast<uint32_t>(frame_header[3]) << 16) |
             (static_cast<uint32_t>(frame_header[4]) << 8) | frame_header[5]) :
            (version == 4 ? read_syncsafe32(frame_header + 4) : read_be32(frame_header + 4));
        position += header_size;
        if (frame_size == 0 || position + frame_size > tag_end) break;
        if (picture && read_embedded_image(file, tag_start + position, frame_size, blob)) return true;
        position += frame_size;
        if (position < tag_end) vTaskDelay(1);
    }
    return false;
}

bool extract_wav_image(FILE *file, ArtworkBlob *blob)
{
    if (!file || !blob || lyra::sd::seek(file, 0, SEEK_END, lyra::sd::Client::Artwork) != 0) {
        return false;
    }
    const long file_end = std::ftell(file);
    if (file_end < 20 || lyra::sd::seek(file, 0, SEEK_SET, lyra::sd::Client::Artwork) != 0) {
        return false;
    }
    uint8_t header[12]{};
    if (lyra::sd::read(file, header, sizeof(header), lyra::sd::Client::Artwork) != sizeof(header) ||
        std::memcmp(header, "RIFF", 4) != 0 || std::memcmp(header + 8, "WAVE", 4) != 0) return false;
    uint64_t position = 12;
    while (position + 8 <= static_cast<uint64_t>(file_end)) {
        uint8_t chunk[8]{};
        if (lyra::sd::seek(file, static_cast<long>(position), SEEK_SET,
                           lyra::sd::Client::Artwork) != 0 ||
            lyra::sd::read(file, chunk, sizeof(chunk), lyra::sd::Client::Artwork) != sizeof(chunk)) {
            return false;
        }
        const uint32_t length = read_le32(chunk + 4);
        const uint64_t data_start = position + 8;
        if (data_start + length > static_cast<uint64_t>(file_end)) return false;
        if ((std::memcmp(chunk, "id3 ", 4) == 0 || std::memcmp(chunk, "ID3 ", 4) == 0) &&
            length >= 10 && lyra::sd::seek(file, static_cast<long>(data_start), SEEK_SET,
                                             lyra::sd::Client::Artwork) == 0) {
            uint8_t id3[10]{};
            if (lyra::sd::read(file, id3, sizeof(id3), lyra::sd::Client::Artwork) == sizeof(id3) &&
                std::memcmp(id3, "ID3", 3) == 0 &&
                extract_id3_image(file, id3, blob, static_cast<long>(data_start))) return true;
        }
        position = data_start + length + (length & 1u);
    }
    return false;
}

bool extract_aiff_image(FILE *file, ArtworkBlob *blob)
{
    if (!file || !blob || lyra::sd::seek(file, 0, SEEK_END, lyra::sd::Client::Artwork) != 0) {
        return false;
    }
    const long file_end = std::ftell(file);
    if (file_end < 20 || lyra::sd::seek(file, 0, SEEK_SET, lyra::sd::Client::Artwork) != 0) {
        return false;
    }
    uint8_t form[12]{};
    if (lyra::sd::read(file, form, sizeof(form), lyra::sd::Client::Artwork) != sizeof(form) ||
        std::memcmp(form, "FORM", 4) != 0 ||
        (std::memcmp(form + 8, "AIFF", 4) != 0 && std::memcmp(form + 8, "AIFC", 4) != 0)) return false;
    uint64_t position = 12;
    while (position + 8 <= static_cast<uint64_t>(file_end)) {
        uint8_t chunk[8]{};
        if (lyra::sd::seek(file, static_cast<long>(position), SEEK_SET,
                           lyra::sd::Client::Artwork) != 0 ||
            lyra::sd::read(file, chunk, sizeof(chunk), lyra::sd::Client::Artwork) != sizeof(chunk)) {
            return false;
        }
        const uint32_t length = read_be32(chunk + 4);
        const uint64_t data_start = position + 8;
        if (data_start + length > static_cast<uint64_t>(file_end)) return false;
        if (std::memcmp(chunk, "ID3 ", 4) == 0 && length >= 10 &&
            lyra::sd::seek(file, static_cast<long>(data_start), SEEK_SET,
                           lyra::sd::Client::Artwork) == 0) {
            uint8_t id3[10]{};
            if (lyra::sd::read(file, id3, sizeof(id3), lyra::sd::Client::Artwork) == sizeof(id3) &&
                std::memcmp(id3, "ID3", 3) == 0 &&
                extract_id3_image(file, id3, blob, static_cast<long>(data_start))) return true;
        }
        position = data_start + length + (length & 1u);
    }
    return false;
}

bool extract_flac_image(FILE *file, ArtworkBlob *blob)
{
    uint64_t position = 4;
    bool last = false;
    while (!last) {
        if (lyra::sd::seek(file, static_cast<long>(position), SEEK_SET,
                           lyra::sd::Client::Artwork) != 0) return false;
        uint8_t block_header[4]{};
        if (lyra::sd::read(file, block_header, sizeof(block_header),
                           lyra::sd::Client::Artwork) != sizeof(block_header)) return false;
        last = (block_header[0] & 0x80) != 0;
        const uint8_t type = block_header[0] & 0x7F;
        const uint32_t length = (static_cast<uint32_t>(block_header[1]) << 16) |
                                (static_cast<uint32_t>(block_header[2]) << 8) | block_header[3];
        position += sizeof(block_header);
        if (type == 6 && length >= 32) {
            uint64_t cursor = position;
            uint8_t word[4];
            auto read_word = [&]() -> uint32_t {
                if (lyra::sd::seek(file, static_cast<long>(cursor), SEEK_SET,
                                   lyra::sd::Client::Artwork) != 0 ||
                    lyra::sd::read(file, word, sizeof(word), lyra::sd::Client::Artwork) != sizeof(word)) return UINT32_MAX;
                cursor += 4;
                return read_be32(word);
            };
            (void)read_word(); // Picture type.
            const uint32_t mime_length = read_word();
            if (mime_length == UINT32_MAX || cursor + mime_length > position + length) return false;
            cursor += mime_length;
            const uint32_t description_length = read_word();
            if (description_length == UINT32_MAX || cursor + description_length > position + length) return false;
            cursor += description_length;
            for (int i = 0; i < 4; ++i) if (read_word() == UINT32_MAX) return false;
            const uint32_t data_length = read_word();
            if (data_length != UINT32_MAX && cursor + data_length <= position + length &&
                read_embedded_image(file, cursor, data_length, blob)) return true;
        }
        position += length;
        if (!last) vTaskDelay(1);
    }
    return false;
}

bool extract_mp4_image(FILE *file, ArtworkBlob *blob)
{
    if (lyra::sd::seek(file, 0, SEEK_END, lyra::sd::Client::Artwork) != 0) return false;
    const long end = std::ftell(file);
    if (end < 12) return false;

    // Locate the top-level `moov` atom with header-sized reads. This seeks
    // over `mdat` instead of scanning the entire audio payload on large M4A
    // files. Cover metadata lives beneath `moov`.
    uint64_t moov_start = 0;
    uint64_t moov_end = 0;
    uint64_t position = 0;
    while (position + 8 <= static_cast<uint64_t>(end)) {
        uint8_t header[16]{};
        if (lyra::sd::seek(file, static_cast<long>(position), SEEK_SET,
                           lyra::sd::Client::Artwork) != 0 ||
            lyra::sd::read(file, header, 8, lyra::sd::Client::Artwork) != 8) return false;
        uint64_t atom_size = read_be32(header);
        uint64_t header_size = 8;
        if (atom_size == 1) {
            if (lyra::sd::read(file, header + 8, 8, lyra::sd::Client::Artwork) != 8) return false;
            atom_size = read_be64(header + 8);
            header_size = 16;
        } else if (atom_size == 0) {
            atom_size = static_cast<uint64_t>(end) - position;
        }
        if (atom_size < header_size || position + atom_size > static_cast<uint64_t>(end)) return false;
        if (std::memcmp(header + 4, "moov", 4) == 0) {
            moov_start = position + header_size;
            moov_end = position + atom_size;
            break;
        }
        position += atom_size;
        if (position + 8 <= static_cast<uint64_t>(end)) vTaskDelay(1);
    }
    if (moov_end <= moov_start || lyra::sd::seek(file, static_cast<long>(moov_start), SEEK_SET,
                                                  lyra::sd::Client::Artwork) != 0) return false;

    // Locate `covr` inside the comparatively small metadata tree, then stream
    // its nested payload directly into the in-memory decoder source.
    uint8_t buffer[2056];
    size_t carried = 0;
    uint64_t absolute = moov_start;
    while (absolute < moov_end) {
        const size_t wanted = static_cast<size_t>(std::min<uint64_t>(2048, moov_end - absolute));
        const size_t read = lyra::sd::read(file, buffer + carried, wanted,
                                           lyra::sd::Client::Artwork);
        if (read == 0) break;
        const size_t total = carried + read;
        const uint64_t buffer_start = absolute >= carried ? absolute - carried : 0;
        for (size_t i = 4; i + 4 <= total; ++i) {
            if (std::memcmp(buffer + i, "covr", 4) != 0) continue;
            const uint32_t atom_size = read_be32(buffer + i - 4);
            const uint64_t atom_start = buffer_start + i - 4;
            if (atom_size >= 16 && atom_size <= 16u * 1024u * 1024u &&
                atom_start + atom_size <= moov_end &&
                read_embedded_image(file, atom_start + 8, atom_size - 8, blob)) return true;
            if (lyra::sd::seek(file, static_cast<long>(absolute + read), SEEK_SET,
                               lyra::sd::Client::Artwork) != 0) return false;
        }
        absolute += read;
        carried = std::min<size_t>(8, total);
        std::memmove(buffer, buffer + total - carried, carried);
        if (absolute < moov_end) vTaskDelay(1);
    }
    return false;
}

bool extract_and_decode(const Track &track, uint16_t **out_pixels,
                        uint16_t artwork_size, ArtworkDiagnostics *diagnostics,
                        bool allow_sd_backing, bool *used_sd_backing)
{
    if (!out_pixels || !diagnostics) return false;
    *out_pixels = nullptr;
    if (used_sd_backing) *used_sd_backing = false;
    FILE *file = lyra::sd::open(track.path, "rb", lyra::sd::Client::Artwork);
    if (!file) return false;
    uint8_t header[10]{};
    const int64_t source_started = esp_timer_get_time();
    const size_t header_read = lyra::sd::read(file, header, sizeof(header),
                                               lyra::sd::Client::Artwork);
    ArtworkBlob blob{};
    blob.psram = true;
    const bool extracted = header_read == sizeof(header) &&
        extract_embedded_image_from_track(file, header, &blob);
    lyra::sd::close(file, lyra::sd::Client::Artwork);
    diagnostics->source_read_us = static_cast<uint32_t>(std::max<int64_t>(
        0, esp_timer_get_time() - source_started));
    diagnostics->source_psram = blob.psram;
    if (!extracted) {
        free_artwork_blob(&blob);
        return false;
    }

    const size_t player_pixels = static_cast<size_t>(artwork_size) * artwork_size;
    auto *player = static_cast<uint16_t *>(alloc_artwork_pixels(
        player_pixels * sizeof(uint16_t)));
    if (!player) {
        heap_caps_free(player);
        free_artwork_blob(&blob);
        return false;
    }

    const int64_t decode_started = esp_timer_get_time();
    const bool decoded = decode_artwork_blob(blob, player, artwork_size, true,
                                             allow_sd_backing, used_sd_backing);
    diagnostics->decode_us = static_cast<uint32_t>(std::max<int64_t>(
        0, esp_timer_get_time() - decode_started));
    ++diagnostics->decode_count;
    diagnostics->player_pixels_internal = esp_ptr_internal(player);
    if (!decoded) {
        ESP_LOGW(kTag, "artwork decode failed: format=%s bytes=%u",
                 blob.format == ArtworkFormat::Png ? "png" : "jpeg",
                 static_cast<unsigned>(blob.length));
        heap_caps_free(player);
        free_artwork_blob(&blob);
        return false;
    }
    ESP_LOGI(kTag, "artwork album decode: source=%u us decode=%u us "
             "compressed=%u bytes source=%s player=%s workspace=%s output=%ux%u",
             static_cast<unsigned>(diagnostics->source_read_us),
             static_cast<unsigned>(diagnostics->decode_us),
             static_cast<unsigned>(blob.length), blob.psram ? "psram" : "internal",
             esp_ptr_internal(player) ? "internal" : "psram",
             used_sd_backing && *used_sd_backing ? "sd" : "psram",
             static_cast<unsigned>(artwork_size), static_cast<unsigned>(artwork_size));
    free_artwork_blob(&blob);
    *out_pixels = player;
    return true;
}


} // namespace lyra::media::artwork
