/* SPDX-License-Identifier: Apache-2.0 */
#include "lyra_image_file.h"
#include "lyra_sd.h"

#include <algorithm>
#include <csetjmp>
#include <climits>
#include <cstring>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "png.h"
extern "C" {
#include "jpeglib.h"
#include "jerror.h"
}

namespace lyra::media {
namespace {
constexpr auto kClient = lyra::sd::Client::Artwork;
constexpr uint32_t kMaxDimension = 16384;
constexpr uint64_t kMaxPixels = 64ULL * 1024 * 1024;
constexpr unsigned kMaxViewport = 512;

uint16_t le16(const uint8_t *p) { return p[0] | (uint16_t(p[1]) << 8); }
uint32_t le32(const uint8_t *p) { return le16(p) | (uint32_t(le16(p + 2)) << 16); }
uint32_t be32(const uint8_t *p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
bool dimensions_valid(uint32_t w, uint32_t h, uint64_t maximum = kMaxPixels) {
    return w && h && w <= kMaxDimension && h <= kMaxDimension && uint64_t(w) * h <= maximum;
}
int32_t source_coordinate(unsigned destination, uint32_t scroll, uint32_t scaled,
                          uint32_t source, unsigned extent)
{
    const int64_t p = int64_t(scroll) + destination -
        (scaled < extent ? (extent - scaled) / 2 : 0);
    if (p < 0 || p >= scaled) return -1;
    return uint64_t(p) * source / scaled;
}
struct Mapping {
    int32_t x[kMaxViewport], y[kMaxViewport];
    Mapping(const ImageFileInfo &info, const ImageViewport &v) {
        for (unsigned i = 0; i < v.width; ++i)
            x[i] = source_coordinate(i, v.x, v.scaled_width, info.width, v.width);
        for (unsigned i = 0; i < v.height; ++i)
            y[i] = source_coordinate(i, v.y, v.scaled_height, info.height, v.height);
    }
};
uint16_t rgb565(unsigned r, unsigned g, unsigned b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
uint16_t rgba565(const uint8_t *p) {
    return rgb565((p[0] * p[3] + 127) / 255,
                  (p[1] * p[3] + 127) / 255, (p[2] * p[3] + 127) / 255);
}

struct JpegError { jpeg_error_mgr base; jmp_buf jump; };
void jpeg_fail(j_common_ptr c) {
    auto *error = reinterpret_cast<JpegError *>(c->err);
    longjmp(error->jump, 1);
}
struct JpegSource {
    jpeg_source_mgr base{};
    FILE *file{};
    const ImageReadControl *control{};
    JOCTET bytes[8192];
};
void jpeg_source_init(j_decompress_ptr) {}
boolean jpeg_source_fill(j_decompress_ptr c) {
    auto *s = reinterpret_cast<JpegSource *>(c->src);
    if (s->control->cancelled()) ERREXIT(c, JERR_FILE_READ);
    const size_t n = lyra::sd::read(s->file, s->bytes, sizeof(s->bytes), kClient);
    if (!n) ERREXIT(c, JERR_INPUT_EOF);
    s->base.next_input_byte = s->bytes;
    s->base.bytes_in_buffer = n;
    vTaskDelay(1);
    return TRUE;
}
void jpeg_source_skip(j_decompress_ptr c, long count) {
    if (count <= 0) return;
    while (static_cast<size_t>(count) > c->src->bytes_in_buffer) {
        count -= c->src->bytes_in_buffer;
        jpeg_source_fill(c);
    }
    c->src->next_input_byte += count;
    c->src->bytes_in_buffer -= count;
}
void jpeg_source_term(j_decompress_ptr) {}
void jpeg_source_setup(j_decompress_ptr c, JpegSource *s, FILE *file, const ImageReadControl &control) {
    s->file = file; s->control = &control;
    s->base.init_source = jpeg_source_init;
    s->base.fill_input_buffer = jpeg_source_fill;
    s->base.skip_input_data = jpeg_source_skip;
    s->base.resync_to_restart = jpeg_resync_to_restart;
    s->base.term_source = jpeg_source_term;
    c->src = &s->base;
}

bool jpeg_file(FILE *file, ImageFileInfo *info, const ImageViewport *view,
               uint16_t *pixels, const Mapping *mapping, const ImageReadControl &control)
{
    jpeg_decompress_struct c{};
    JpegError error{};
    JpegSource source{};
    volatile bool created = false;
    c.err = jpeg_std_error(&error.base);
    error.base.error_exit = jpeg_fail;
    if (setjmp(error.jump)) {
        if (created) {
            jpeg_destroy_decompress(&c);
        }
        return false;
    }
    jpeg_create_decompress(&c);
    created = true;
    c.mem->max_memory_to_use = 512L * 1024;
    jpeg_source_setup(&c, &source, file, control);
    bool ok = jpeg_read_header(&c, TRUE) == JPEG_HEADER_OK &&
        dimensions_valid(c.image_width, c.image_height);
    if (ok && !view) {
        *info = {c.image_width, c.image_height, ImageFileFormat::Jpeg};
    } else if (ok) {
        ok = c.image_width == info->width && c.image_height == info->height;
        // Native IDCT downsampling only when every output pixel still has at
        // least one decoder pixel. Zoom toward 1:1 automatically returns to
        // full-resolution decoding; no thumbnail-detail ceiling is retained.
        c.scale_num = 1; c.scale_denom = 1;
        for (unsigned divisor : {8u, 4u, 2u}) {
            if (uint64_t(view->scaled_width) * divisor <= info->width &&
                uint64_t(view->scaled_height) * divisor <= info->height) {
                c.scale_denom = divisor; break;
            }
        }
        const bool packed = c.jpeg_color_space == JCS_YCbCr || c.jpeg_color_space == JCS_RGB ||
                            c.jpeg_color_space == JCS_GRAYSCALE;
        const bool cmyk = c.jpeg_color_space == JCS_CMYK || c.jpeg_color_space == JCS_YCCK;
        c.out_color_space = cmyk ? JCS_CMYK : packed ? JCS_RGB565 : JCS_RGB;
        c.do_fancy_upsampling = FALSE; c.do_block_smoothing = FALSE;
        c.dct_method = JDCT_IFAST;
        if (ok) ok = jpeg_start_decompress(&c) && c.output_width && c.output_height;
        if (ok) {
            const size_t row_bytes = size_t(c.output_width) * c.output_components;
            JSAMPARRAY rows = (*c.mem->alloc_sarray)(reinterpret_cast<j_common_ptr>(&c),
                JPOOL_IMAGE, row_bytes, 1);
            unsigned dy = 0;
            while (dy < view->height && mapping->y[dy] < 0) ++dy;
            while (ok && dy < view->height && mapping->y[dy] >= 0) {
                if (control.cancelled()) { ok = false; break; }
                const uint32_t wanted = uint64_t(mapping->y[dy]) * c.output_height / info->height;
                // Discard preceding rows using a single bounded row buffer.
                // Reading incrementally makes cancellation/yield predictable.
                while (ok && c.output_scanline <= wanted) {
                    if (control.cancelled() || jpeg_read_scanlines(&c, rows, 1) != 1) ok = false;
                    if (!(c.output_scanline & 15)) vTaskDelay(1);
                }
                if (!ok) break;
                do {
                    for (unsigned dx = 0; dx < view->width; ++dx) {
                        if (mapping->x[dx] < 0) continue;
                        const uint32_t sx = uint64_t(mapping->x[dx]) * c.output_width / info->width;
                        uint16_t pixel;
                        if (packed) pixel = reinterpret_cast<const uint16_t *>(rows[0])[sx];
                        else if (cmyk) {
                            const uint8_t *p = rows[0] + sx * 4;
                            pixel = c.saw_Adobe_marker ? rgb565(p[0] * p[3] / 255,
                                p[1] * p[3] / 255, p[2] * p[3] / 255) :
                                rgb565((255 - p[0]) * (255 - p[3]) / 255,
                                    (255 - p[1]) * (255 - p[3]) / 255,
                                    (255 - p[2]) * (255 - p[3]) / 255);
                        } else {
                            const uint8_t *p = rows[0] + sx * 3;
                            pixel = rgb565(p[0], p[1], p[2]);
                        }
                        pixels[dy * view->width + dx] = pixel;
                    }
                    ++dy;
                } while (dy < view->height && mapping->y[dy] >= 0 &&
                    uint64_t(mapping->y[dy]) * c.output_height / info->height == wanted);
            }
        }
    }
    // Cropped views need not decompress the rest of the image. Abort also
    // releases progressive virtual arrays and their transient SD workspace.
    jpeg_abort_decompress(&c);
    jpeg_destroy_decompress(&c);
    return ok && !control.cancelled();
}

struct PngSource { FILE *file; const ImageReadControl *control; };
void png_read_file(png_structp png, png_bytep output, png_size_t length) {
    auto *s = static_cast<PngSource *>(png_get_io_ptr(png));
    if (s->control->cancelled() || !lyra::sd::read_exact(s->file, output, length, kClient))
        png_error(png, "file read cancelled or failed");
}
void png_fail(png_structp png, png_const_charp) { png_longjmp(png, 1); }
void png_warn(png_structp, png_const_charp) {}
png_voidp png_allocate(png_structp, png_alloc_size_t n) {
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
void png_free(png_structp, png_voidp p) { heap_caps_free(p); }

bool png_file(FILE *file, const ImageFileInfo &info, const ImageViewport &view,
              uint16_t *pixels, const Mapping &mapping, const ImageReadControl &control)
{
    png_structp png = png_create_read_struct_2(PNG_LIBPNG_VER_STRING, nullptr,
        png_fail, png_warn, nullptr, png_allocate, png_free);
    if (!png) return false;
    png_infop header = png_create_info_struct(png);
    if (!header) { png_destroy_read_struct(&png, nullptr, nullptr); return false; }
    png_bytep volatile row = nullptr;
    png_bytep volatile samples = nullptr;
    PngSource source{file, &control};
    if (setjmp(png_jmpbuf(png))) {
        heap_caps_free(row); heap_caps_free(samples);
        png_destroy_read_struct(&png, &header, nullptr);
        return false;
    }
    png_set_read_fn(png, &source, png_read_file);
    // Bound ancillary metadata allocations as well as the decoded row.
    png_set_user_limits(png, kMaxDimension, kMaxDimension);
    png_set_chunk_malloc_max(png, 64u * 1024u);
    png_set_chunk_cache_max(png, 4);
    png_read_info(png, header);
    if (png_get_image_width(png, header) != info.width ||
        png_get_image_height(png, header) != info.height ||
        !dimensions_valid(info.width, info.height, 16ULL * 1024 * 1024))
        png_error(png, "image dimensions changed or exceed limit");
    const int type = png_get_color_type(png, header);
    const int depth = png_get_bit_depth(png, header);
    if (depth == 16) png_set_strip_16(png);
    if (type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (type == PNG_COLOR_TYPE_GRAY && depth < 8) png_set_expand_gray_1_2_4_to_8(png);
    const bool transparent = png_get_valid(png, header, PNG_INFO_tRNS);
    if (transparent) png_set_tRNS_to_alpha(png);
    if (type == PNG_COLOR_TYPE_GRAY || type == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(png);
    if (!(type & PNG_COLOR_MASK_ALPHA) && !transparent) png_set_filler(png, 255, PNG_FILLER_AFTER);
    const int passes = png_set_interlace_handling(png);
    png_read_update_info(png, header);
    const size_t bytes = png_get_rowbytes(png, header);
    if (bytes != size_t(info.width) * 4) png_error(png, "unexpected row layout");
    row = static_cast<png_bytep>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!row) png_error(png, "row allocation failed");
    // Adam7 revisits rows. Preserve only viewport samples, indexed by each
    // destination row, instead of storing the entire decoded source image.
    if (passes > 1) {
        samples = static_cast<png_bytep>(heap_caps_calloc(size_t(view.width) * view.height,
            4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!samples) png_error(png, "sample allocation failed");
    }
    for (int pass = 0; pass < passes; ++pass) {
        unsigned dy = 0;
        while (dy < view.height && mapping.y[dy] < 0) ++dy;
        for (uint32_t sy = 0; sy < info.height; ++sy) {
            if (control.cancelled()) png_error(png, "cancelled");
            const bool selected = dy < view.height && mapping.y[dy] == int32_t(sy);
            if (passes > 1) {
                std::memset(row, 0, bytes);
                if (pass && selected) {
                    for (unsigned dx = 0; dx < view.width; ++dx) {
                        if (mapping.x[dx] >= 0) std::memcpy(row + mapping.x[dx] * 4,
                            samples + (dy * view.width + dx) * 4, 4);
                    }
                }
                png_read_row(png, nullptr, row);
            } else png_read_row(png, row, nullptr);
            while (dy < view.height && mapping.y[dy] == int32_t(sy)) {
                for (unsigned dx = 0; dx < view.width; ++dx) {
                    if (mapping.x[dx] < 0) continue;
                    const uint8_t *p = row + mapping.x[dx] * 4;
                    if (passes > 1 && pass + 1 < passes)
                        std::memcpy(samples + (dy * view.width + dx) * 4, p, 4);
                    if (pass + 1 == passes) pixels[dy * view.width + dx] = rgba565(p);
                }
                ++dy;
            }
            if (!(sy & 15)) vTaskDelay(1);
            // Non-interlaced crops can stop once the last required row is read.
            if (passes == 1 && (dy == view.height || mapping.y[dy] < 0)) break;
        }
    }
    heap_caps_free(row); heap_caps_free(samples);
    png_destroy_read_struct(&png, &header, nullptr);
    return !control.cancelled();
}

bool bmp_file(FILE *file, const ImageFileInfo &info, const ImageViewport &view,
              uint16_t *pixels, const Mapping &mapping, const ImageReadControl &control)
{
    uint8_t head[54], palette[1024];
    if (!lyra::sd::read_exact(file, head, sizeof(head), kClient)) return false;
    const uint32_t header = le32(head + 14), offset = le32(head + 10);
    const int32_t width = static_cast<int32_t>(le32(head + 18));
    const int32_t signed_height = static_cast<int32_t>(le32(head + 22));
    const uint16_t bits = le16(head + 28);
    const uint32_t compression = le32(head + 30);
    if (head[0] != 'B' || head[1] != 'M' || header < 40 || width != int32_t(info.width) ||
        signed_height == INT32_MIN || uint32_t(signed_height < 0 ? -signed_height : signed_height) != info.height ||
        le16(head + 26) != 1 || (compression != 0 && compression != 3) ||
        (bits != 1 && bits != 4 && bits != 8 && bits != 16 && bits != 24 && bits != 32) ||
        (compression == 3 && bits != 16 && bits != 32)) return false;
    const uint32_t stride = ((uint64_t(info.width) * bits + 31) / 32) * 4;
    if (lyra::sd::seek(file, 0, SEEK_END, kClient) != 0) return false;
    const long length = std::ftell(file);
    if (length < 0 || uint64_t(offset) + uint64_t(stride) * info.height > uint64_t(length) ||
        uint64_t(header) + 14 > offset) return false;
    uint32_t masks[3] = {bits == 16 ? 0x7C00u : 0xFF0000u,
                         bits == 16 ? 0x03E0u : 0x00FF00u,
                         bits == 16 ? 0x001Fu : 0x0000FFu};
    uint64_t palette_start = uint64_t(header) + 14;
    if (compression == 3) {
        const uint64_t start = header >= 52 ? 54 : palette_start;
        uint8_t data[12];
        if (start + 12 > offset || start > LONG_MAX ||
            lyra::sd::seek(file, start, SEEK_SET, kClient) != 0 ||
            !lyra::sd::read_exact(file, data, sizeof(data), kClient)) return false;
        for (unsigned i = 0; i < 3; ++i) masks[i] = le32(data + i * 4);
        if (header == 40) palette_start += 12;
    }
    const uint32_t colors = bits <= 8 ? (le32(head + 46) ? le32(head + 46) : 1u << bits) : 0;
    if (bits <= 8) {
        if (colors > (1u << bits) || palette_start + colors * 4 > offset || palette_start > LONG_MAX ||
            lyra::sd::seek(file, palette_start, SEEK_SET, kClient) != 0 ||
            !lyra::sd::read_exact(file, palette, colors * 4, kClient)) return false;
    }
    int32_t first_x = -1, last_x = -1;
    for (unsigned dx = 0; dx < view.width; ++dx) {
        if (mapping.x[dx] >= 0) {
            if (first_x < 0) first_x = mapping.x[dx];
            last_x = mapping.x[dx];
        }
    }
    if (first_x < 0) return !control.cancelled();
    const uint32_t first_byte = uint32_t(first_x) * bits / 8;
    const uint32_t row_bytes = (uint32_t(last_x + 1) * bits + 7) / 8 - first_byte;
    auto *row = static_cast<uint8_t *>(heap_caps_malloc(row_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!row) return false;
    bool ok = true;
    int32_t previous_y = -1;
    const auto channel = [](uint32_t value, uint32_t mask) -> unsigned {
        if (!mask) return 0;
        unsigned shift = 0;
        while (!(mask & 1)) { mask >>= 1; ++shift; }
        return (uint64_t((value >> shift) & mask) * 255) / mask;
    };
    for (unsigned dy = 0; ok && dy < view.height; ++dy) {
        if (control.cancelled()) { ok = false; break; }
        const int32_t sy = mapping.y[dy];
        if (sy < 0) continue;
        if (sy != previous_y) {
            const uint64_t position = offset + uint64_t(stride) *
                (signed_height < 0 ? sy : info.height - 1 - sy) + first_byte;
            ok = position <= LONG_MAX && lyra::sd::seek(file, position, SEEK_SET, kClient) == 0 &&
                lyra::sd::read_exact(file, row, row_bytes, kClient);
            previous_y = sy;
        }
        for (unsigned dx = 0; ok && dx < view.width; ++dx) {
            const int32_t sx = mapping.x[dx];
            if (sx < 0) continue;
            const uint32_t byte = uint32_t(sx) * bits / 8 - first_byte;
            unsigned r, g, b;
            if (bits <= 8) {
                const unsigned index = bits == 8 ? row[byte] : bits == 4 ?
                    ((row[byte] >> ((1 - sx % 2) * 4)) & 15) : ((row[byte] >> (7 - sx % 8)) & 1);
                if (index >= colors) { ok = false; break; }
                const uint8_t *p = palette + index * 4;
                b = p[0]; g = p[1]; r = p[2];
            } else if (bits == 24) {
                const uint8_t *p = row + byte; b = p[0]; g = p[1]; r = p[2];
            } else {
                const uint32_t value = bits == 16 ? le16(row + byte) : le32(row + byte);
                r = channel(value, masks[0]); g = channel(value, masks[1]); b = channel(value, masks[2]);
            }
            pixels[dy * view.width + dx] = rgb565(r, g, b);
        }
        if (!(dy & 15)) vTaskDelay(1);
    }
    heap_caps_free(row);
    return ok && !control.cancelled();
}
} // namespace

bool image_file_info(const char *path, ImageFileInfo *info, const ImageReadControl &control)
{
    if (!path || !info || control.cancelled()) return false;
    FILE *file = lyra::sd::open(path, "rb", kClient);
    if (!file) return false;
    uint8_t head[26];
    bool ok = lyra::sd::read_exact(file, head, sizeof(head), kClient);
    if (ok && !std::memcmp(head, "\x89PNG\r\n\x1a\n", 8) && !std::memcmp(head + 12, "IHDR", 4)) {
        *info = {be32(head + 16), be32(head + 20), ImageFileFormat::Png};
        ok = dimensions_valid(info->width, info->height, 16ULL * 1024 * 1024);
    } else if (ok && head[0] == 'B' && head[1] == 'M' && le32(head + 14) >= 40) {
        const int32_t w = static_cast<int32_t>(le32(head + 18));
        const int32_t h = static_cast<int32_t>(le32(head + 22));
        ok = w > 0 && h != INT32_MIN;
        if (ok) {
            *info = {uint32_t(w), uint32_t(h < 0 ? -h : h), ImageFileFormat::Bmp};
            ok = dimensions_valid(info->width, info->height);
        }
    } else if (ok && head[0] == 0xFF && head[1] == 0xD8) {
        ok = lyra::sd::seek(file, 0, SEEK_SET, kClient) == 0 &&
            jpeg_file(file, info, nullptr, nullptr, nullptr, control);
    } else ok = false;
    lyra::sd::close(file, kClient);
    return ok && !control.cancelled();
}

bool decode_image_file_viewport(const char *path, const ImageFileInfo &info,
                               const ImageViewport &view, uint16_t *pixels,
                               const ImageReadControl &control)
{
    if (!path || !pixels || !view.width || !view.height || view.width > kMaxViewport ||
        view.height > kMaxViewport || !view.scaled_width || !view.scaled_height ||
        !dimensions_valid(info.width, info.height) || control.cancelled()) return false;
    FILE *file = lyra::sd::open(path, "rb", kClient);
    if (!file) return false;
    std::memset(pixels, 0, size_t(view.width) * view.height * 2);
    const Mapping mapping(info, view);
    bool ok = false;
    switch (info.format) {
        case ImageFileFormat::Png: ok = png_file(file, info, view, pixels, mapping, control); break;
        case ImageFileFormat::Jpeg: {
            ImageFileInfo expected = info;
            ok = jpeg_file(file, &expected, &view, pixels, &mapping, control); break;
        }
        case ImageFileFormat::Bmp: ok = bmp_file(file, info, view, pixels, mapping, control); break;
    }
    lyra::sd::close(file, kClient);
    return ok && !control.cancelled();
}
} // namespace lyra::media
