/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lyra_progressive_jpeg.h"

#include <algorithm>
#include <csetjmp>
#include <cstdint>

#include "esp_cpu_utils.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" {
#include "jerror.h"
#include "jpeglib.h"

void lyra_jpeg_begin_backing_store_tracking(j_common_ptr cinfo, uint32_t *flag,
                                             uint32_t allow_sd);
void lyra_jpeg_end_backing_store_tracking(j_common_ptr cinfo);
}

namespace lyra::media {
namespace {

constexpr const char *kTag = "lyra.jpeg";
// Small/medium virtual arrays remain entirely in PSRAM. For an oversized
// progressive cover, this bounds the active window while the rest uses a
// transient SD scratch file that is removed when the decoder closes.
constexpr long kProgressiveRamBudget = 512L * 1024L;
constexpr JDIMENSION kMaxSourceDimension = 16384;
constexpr uint64_t kMaxSourcePixels = 64ULL * 1024ULL * 1024ULL;

struct JpegError {
    jpeg_error_mgr base;
    jmp_buf jump;
    char message[JMSG_LENGTH_MAX];
    int message_code;
    int global_state;
    int data_precision;
    int progressive_mode;
    int arithmetic_coding;
    int input_scan_number;
    uint32_t caller_pc;
    unsigned image_width;
    unsigned image_height;
    unsigned scale_num;
    unsigned scale_denom;
    int jpeg_color_space;
    int output_color_space;
    int dct_method;
};

void jpeg_error_exit(j_common_ptr info)
{
    auto *error = reinterpret_cast<JpegError *>(info->err);
    auto *decoder = reinterpret_cast<j_decompress_ptr>(info);
    error->message_code = info->err->msg_code;
    error->global_state = decoder->global_state;
    error->data_precision = decoder->data_precision;
    error->progressive_mode = decoder->progressive_mode ? 1 : 0;
    error->arithmetic_coding = decoder->arith_code ? 1 : 0;
    error->input_scan_number = decoder->input_scan_number;
    error->caller_pc = esp_cpu_process_stack_pc(
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(__builtin_return_address(0))));
    error->image_width = static_cast<unsigned>(decoder->image_width);
    error->image_height = static_cast<unsigned>(decoder->image_height);
    error->scale_num = decoder->scale_num;
    error->scale_denom = decoder->scale_denom;
    error->jpeg_color_space = static_cast<int>(decoder->jpeg_color_space);
    error->output_color_space = static_cast<int>(decoder->out_color_space);
    error->dct_method = static_cast<int>(decoder->dct_method);
    error->message[0] = '\0';
    (*info->err->format_message)(info, error->message);
    longjmp(error->jump, 1);
}

} // namespace

bool decode_progressive_jpeg(const uint8_t *data, size_t length, uint16_t *pixels,
                             uint16_t target_size, bool preserve_aspect,
                             bool allow_sd_backing,
                             bool *used_sd_backing)
{
    if (used_sd_backing) *used_sd_backing = false;
    if (!data || length == 0 || !pixels || target_size == 0) return false;

    jpeg_decompress_struct decoder{};
    JpegError error{};
    decoder.err = jpeg_std_error(&error.base);
    error.base.error_exit = jpeg_error_exit;
    volatile bool created = false;

    if (setjmp(error.jump)) {
        if (error.message_code == JERR_NO_BACKING_STORE && !allow_sd_backing) {
            ESP_LOGW(kTag, "JPEG requires SD workspace but SD artwork cache is disabled: "
                     "size=%ux%u progressive=%d",
                     error.image_width, error.image_height, error.progressive_mode);
        } else {
            ESP_LOGW(kTag, "JPEG decode failed: %s code=%d state=%d caller=0x%08lx "
                     "size=%ux%u scale=%u/%u colors=%d->%d dct=%d precision=%d "
                     "progressive=%d arithmetic=%d scan=%d",
                     error.message[0] ? error.message : "decoder error",
                     error.message_code, error.global_state,
                     static_cast<unsigned long>(error.caller_pc),
                     error.image_width, error.image_height,
                     error.scale_num, error.scale_denom,
                     error.jpeg_color_space, error.output_color_space,
                     error.dct_method, error.data_precision,
                     error.progressive_mode, error.arithmetic_coding,
                     error.input_scan_number);
        }
        if (created) {
            lyra_jpeg_end_backing_store_tracking(
                reinterpret_cast<j_common_ptr>(&decoder));
            jpeg_destroy_decompress(&decoder);
        }
        return false;
    }

    jpeg_create_decompress(&decoder);
    created = true;
    uint32_t sd_backing_opened = 0;
    lyra_jpeg_begin_backing_store_tracking(
        reinterpret_cast<j_common_ptr>(&decoder), &sd_backing_opened,
        allow_sd_backing ? 1u : 0u);
    decoder.mem->max_memory_to_use = kProgressiveRamBudget;
    jpeg_mem_src(&decoder, const_cast<unsigned char *>(data), length);
    if (jpeg_read_header(&decoder, TRUE) != JPEG_HEADER_OK ||
        decoder.image_width == 0 || decoder.image_height == 0 ||
        decoder.image_width > kMaxSourceDimension ||
        decoder.image_height > kMaxSourceDimension ||
        static_cast<uint64_t>(decoder.image_width) * decoder.image_height > kMaxSourcePixels) {
        ESP_LOGW(kTag, "progressive JPEG dimensions rejected: %ux%u",
                 static_cast<unsigned>(decoder.image_width),
                 static_cast<unsigned>(decoder.image_height));
        lyra_jpeg_end_backing_store_tracking(reinterpret_cast<j_common_ptr>(&decoder));
        jpeg_destroy_decompress(&decoder);
        return false;
    }

    const JDIMENSION longest = std::max(decoder.image_width, decoder.image_height);
    decoder.scale_num = 1;
    decoder.scale_denom = 1;
    for (const unsigned denominator : {8u, 4u, 2u}) {
        if ((longest + denominator - 1) / denominator >= target_size) {
            decoder.scale_denom = denominator;
            break;
        }
    }
    // libjpeg-turbo has a packed RGB565 converter for the common YCbCr/RGB/
    // grayscale cases. Avoid expanding every source pixel to RGB888 only to
    // immediately quantize it again. CMYK/YCCK stays on the RGB fallback.
    const bool packed_rgb565 = decoder.jpeg_color_space == JCS_YCbCr ||
                               decoder.jpeg_color_space == JCS_RGB ||
                               decoder.jpeg_color_space == JCS_GRAYSCALE;
    decoder.out_color_space = packed_rgb565 ? JCS_RGB565 : JCS_RGB;
    decoder.do_fancy_upsampling = FALSE;
    // Intermediate progressive scans are never displayed. Disabling block
    // smoothing avoids retaining five iMCU rows per component and keeps the
    // decoder's PSRAM working set bounded while it produces the final image.
    decoder.do_block_smoothing = FALSE;
    decoder.dct_method = JDCT_IFAST;
    // libjpeg-turbo's packed RGB565 converter writes two bytes per pixel, but
    // intentionally reports three logical output components. The scanline
    // storage therefore needs the library-reported width while the packed
    // path below reads the actual RGB565 words from its first two bytes.
    constexpr int kReportedOutputComponents = 3;
    if (!jpeg_start_decompress(&decoder) ||
        decoder.output_components != kReportedOutputComponents ||
        decoder.output_width == 0 || decoder.output_height == 0) {
        ESP_LOGW(kTag, "JPEG output setup rejected: colorspace=%d components=%d size=%ux%u",
                 static_cast<int>(decoder.out_color_space), decoder.output_components,
                 static_cast<unsigned>(decoder.output_width),
                 static_cast<unsigned>(decoder.output_height));
        lyra_jpeg_end_backing_store_tracking(reinterpret_cast<j_common_ptr>(&decoder));
        jpeg_destroy_decompress(&decoder);
        return false;
    }

    uint16_t draw_width = target_size;
    uint16_t draw_height = target_size;
    uint16_t offset_x = 0;
    uint16_t offset_y = 0;
    if (preserve_aspect) {
        if (decoder.output_width >= decoder.output_height) {
            draw_height = static_cast<uint16_t>(std::max<uint32_t>(1,
                static_cast<uint32_t>(decoder.output_height) * target_size /
                decoder.output_width));
            offset_y = (target_size - draw_height) / 2;
        } else {
            draw_width = static_cast<uint16_t>(std::max<uint32_t>(1,
                static_cast<uint32_t>(decoder.output_width) * target_size /
                decoder.output_height));
            offset_x = (target_size - draw_width) / 2;
        }
    }

    const size_t row_bytes = static_cast<size_t>(decoder.output_width) *
        static_cast<size_t>(decoder.output_components);
    constexpr JDIMENSION kRowsPerRead = 8;
    JSAMPARRAY rows = (*decoder.mem->alloc_sarray)(
        reinterpret_cast<j_common_ptr>(&decoder), JPOOL_IMAGE, row_bytes, kRowsPerRead);
    uint16_t next_target_y = 0;
    while (decoder.output_scanline < decoder.output_height) {
        const JDIMENSION source_y_start = decoder.output_scanline;
        const JDIMENSION requested_rows = std::min<JDIMENSION>(
            kRowsPerRead, decoder.output_height - source_y_start);
        const JDIMENSION decoded_rows = jpeg_read_scanlines(&decoder, rows, requested_rows);
        if (decoded_rows == 0) break;
        for (JDIMENSION row_index = 0; row_index < decoded_rows; ++row_index) {
            const JDIMENSION source_y = source_y_start + row_index;
            while (next_target_y < draw_height &&
                   static_cast<JDIMENSION>(next_target_y) * decoder.output_height /
                       draw_height == source_y) {
                uint16_t *destination = pixels +
                    static_cast<size_t>(offset_y + next_target_y) * target_size + offset_x;
                for (uint16_t target_x = 0; target_x < draw_width; ++target_x) {
                    const JDIMENSION source_x =
                        static_cast<JDIMENSION>(target_x) * decoder.output_width / draw_width;
                    if (packed_rgb565) {
                        const auto *source_pixels = reinterpret_cast<const uint16_t *>(
                            rows[row_index]);
                        destination[target_x] = source_pixels[source_x];
                    } else {
                        const auto *rgb = rows[row_index] + static_cast<size_t>(source_x) * 3;
                        destination[target_x] = static_cast<uint16_t>(((rgb[0] & 0xF8) << 8) |
                            ((rgb[1] & 0xFC) << 3) | (rgb[2] >> 3));
                    }
                }
                ++next_target_y;
            }
        }
        if ((decoder.output_scanline & 15u) == 0) vTaskDelay(1);
    }

    const bool complete = next_target_y == draw_height && jpeg_finish_decompress(&decoder);
    if (used_sd_backing) *used_sd_backing = sd_backing_opened != 0;
    lyra_jpeg_end_backing_store_tracking(reinterpret_cast<j_common_ptr>(&decoder));
    jpeg_destroy_decompress(&decoder);
    return complete;
}

} // namespace lyra::media
