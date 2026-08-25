/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lyra_png.h"

#include <algorithm>
#include <csetjmp>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "png.h"

namespace lyra::media {
namespace {

constexpr const char *kTag = "lyra.png";
constexpr png_uint_32 kMaxDimension = 16384;
constexpr uint64_t kMaxPixels = 16ULL * 1024ULL * 1024ULL;

void png_error_callback(png_structp png, png_const_charp message)
{
    ESP_LOGW(kTag, "PNG decode failed: %s", message ? message : "decoder error");
    png_longjmp(png, 1);
}

void png_warning_callback(png_structp, png_const_charp message)
{
    ESP_LOGD(kTag, "PNG warning: %s", message ? message : "unknown");
}

struct PngMemoryReader {
    const uint8_t *data;
    size_t length;
    size_t position;
};

void png_memory_read(png_structp png, png_bytep output, png_size_t length)
{
    auto *reader = static_cast<PngMemoryReader *>(png_get_io_ptr(png));
    if (!reader || reader->position > reader->length ||
        length > reader->length - reader->position) {
        png_error(png, "PNG source read past end");
        return;
    }
    std::memcpy(output, reader->data + reader->position, length);
    reader->position += length;
}

png_voidp png_psram_alloc(png_structp, png_alloc_size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void png_psram_free(png_structp, png_voidp pointer)
{
    heap_caps_free(pointer);
}

uint16_t composite_rgb565(const png_byte *rgba, uint32_t transparent_background_rgb)
{
    const unsigned background_red = (transparent_background_rgb >> 16) & 0xFF;
    const unsigned background_green = (transparent_background_rgb >> 8) & 0xFF;
    const unsigned background_blue = transparent_background_rgb & 0xFF;
    const unsigned alpha = rgba[3];
    const uint8_t red = static_cast<uint8_t>((rgba[0] * alpha +
        background_red * (255 - alpha) + 127) / 255);
    const uint8_t green = static_cast<uint8_t>((rgba[1] * alpha +
        background_green * (255 - alpha) + 127) / 255);
    const uint8_t blue = static_cast<uint8_t>((rgba[2] * alpha +
        background_blue * (255 - alpha) + 127) / 255);
    return static_cast<uint16_t>(((red & 0xF8) << 8) |
                                 ((green & 0xFC) << 3) | (blue >> 3));
}

void render_row(const png_byte *row, png_uint_32 source_width,
                png_uint_32 source_height, png_uint_32 source_y,
                uint16_t *pixels, uint16_t target_width, uint16_t draw_width,
                uint16_t draw_height, uint16_t offset_x, uint16_t offset_y,
                uint16_t *next_target_y, uint32_t transparent_background_rgb)
{
    while (*next_target_y < draw_height &&
           static_cast<png_uint_32>(*next_target_y) * source_height /
               draw_height == source_y) {
        uint16_t *destination = pixels +
            static_cast<size_t>(offset_y + *next_target_y) * target_width + offset_x;
        for (uint16_t target_x = 0; target_x < draw_width; ++target_x) {
            const png_uint_32 source_x =
                static_cast<png_uint_32>(target_x) * source_width / draw_width;
            destination[target_x] = composite_rgb565(
                row + static_cast<size_t>(source_x) * 4, transparent_background_rgb);
        }
        ++*next_target_y;
    }
}

} // namespace

bool decode_png(const uint8_t *data, size_t length, uint16_t *pixels,
                uint16_t target_width, uint16_t target_height,
                bool preserve_aspect, uint32_t transparent_background_rgb)
{
    if (!data || length == 0 || !pixels || target_width == 0 || target_height == 0) return false;
    png_structp png = png_create_read_struct_2(PNG_LIBPNG_VER_STRING, nullptr,
        png_error_callback, png_warning_callback, nullptr, png_psram_alloc, png_psram_free);
    if (!png) return false;
    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_read_struct(&png, nullptr, nullptr);
        return false;
    }

    PngMemoryReader reader{data, length, 0};
    png_bytep volatile row = nullptr;
    png_bytep volatile preserved_samples = nullptr;
    bool ok = false;
    if (setjmp(png_jmpbuf(png))) {
        heap_caps_free(row);
        heap_caps_free(preserved_samples);
        png_destroy_read_struct(&png, &info, nullptr);
        return false;
    }

    png_set_read_fn(png, &reader, png_memory_read);
    png_read_info(png, info);
    const png_uint_32 width = png_get_image_width(png, info);
    const png_uint_32 height = png_get_image_height(png, info);
    if (width == 0 || height == 0 || width > kMaxDimension || height > kMaxDimension ||
        static_cast<uint64_t>(width) * height > kMaxPixels) {
        png_error(png, "image dimensions exceed artwork limit");
    }

    const int color_type = png_get_color_type(png, info);
    const int bit_depth = png_get_bit_depth(png, info);
    if (bit_depth == 16) png_set_strip_16(png);
    if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
        png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);
    if (!(color_type & PNG_COLOR_MASK_ALPHA) && !png_get_valid(png, info, PNG_INFO_tRNS))
        png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    const int passes = png_set_interlace_handling(png);
    png_read_update_info(png, info);
    const size_t row_bytes = png_get_rowbytes(png, info);
    if (row_bytes != static_cast<size_t>(width) * 4)
        png_error(png, "unexpected transformed row format");
    row = static_cast<png_bytep>(heap_caps_malloc(row_bytes,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!row) png_error(png, "not enough PSRAM for PNG row");

    uint16_t draw_width = target_width;
    uint16_t draw_height = target_height;
    uint16_t offset_x = 0;
    uint16_t offset_y = 0;
    if (preserve_aspect) {
        if (width >= height) {
            draw_height = static_cast<uint16_t>(std::max<uint32_t>(1,
                static_cast<uint32_t>(height) * target_width / width));
            if (draw_height > target_height) {
                draw_height = target_height;
                draw_width = static_cast<uint16_t>(std::max<uint32_t>(1,
                    static_cast<uint32_t>(width) * target_height / height));
            }
            offset_y = (target_height - draw_height) / 2;
        } else {
            draw_width = static_cast<uint16_t>(std::max<uint32_t>(1,
                static_cast<uint32_t>(width) * target_height / height));
            if (draw_width > target_width) {
                draw_width = target_width;
                draw_height = static_cast<uint16_t>(std::max<uint32_t>(1,
                    static_cast<uint32_t>(height) * target_width / width));
            }
            offset_x = (target_width - draw_width) / 2;
        }
    }

    // Adam7 interlacing revisits each source row on every pass. Keep only the
    // source samples that can contribute to the resized output instead of
    // backing every decoded row with a MicroSD temporary file.
    if (passes > 1) {
        preserved_samples = static_cast<png_bytep>(heap_caps_calloc(
            static_cast<size_t>(draw_width) * 4, 1,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!preserved_samples) png_error(png, "not enough PSRAM for interlace samples");
    }

    uint16_t next_target_y = 0;
    for (int pass = 0; pass < passes; ++pass) {
        for (png_uint_32 source_y = 0; source_y < height; ++source_y) {
            const png_uint_32 first_target_y = static_cast<png_uint_32>(
                (static_cast<uint64_t>(source_y) * draw_height + height - 1) / height);
            const bool output_row = first_target_y < draw_height &&
                static_cast<png_uint_32>(first_target_y) * height / draw_height == source_y;
            if (passes > 1) {
                std::memset(row, 0, row_bytes);
                if (pass > 0 && output_row) {
                    for (uint16_t target_x = 0; target_x < draw_width; ++target_x) {
                        const png_uint_32 source_x = static_cast<png_uint_32>(
                            static_cast<uint32_t>(target_x) * width / draw_width);
                        std::memcpy(row + static_cast<size_t>(source_x) * 4,
                                    preserved_samples + static_cast<size_t>(target_x) * 4, 4);
                    }
                }
                png_read_row(png, nullptr, row);
                if (pass + 1 < passes && output_row) {
                    for (uint16_t target_x = 0; target_x < draw_width; ++target_x) {
                        const png_uint_32 source_x = static_cast<png_uint_32>(
                            static_cast<uint32_t>(target_x) * width / draw_width);
                        std::memcpy(preserved_samples + static_cast<size_t>(target_x) * 4,
                                    row + static_cast<size_t>(source_x) * 4, 4);
                    }
                }
            } else {
                png_read_row(png, row, nullptr);
            }
            if (pass + 1 == passes) {
                render_row(row, width, height, source_y, pixels, target_width,
                           draw_width, draw_height, offset_x, offset_y, &next_target_y,
                           transparent_background_rgb);
            }
            if ((source_y & 15u) == 15u) vTaskDelay(1);
        }
    }
    png_read_end(png, nullptr);
    ok = next_target_y == draw_height;
    heap_caps_free(preserved_samples);
    heap_caps_free(row);
    png_destroy_read_struct(&png, &info, nullptr);
    return ok;
}

} // namespace lyra::media
