/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <atomic>
#include <cstdint>

namespace lyra::media {
enum class ImageFileFormat : uint8_t { Png, Jpeg, Bmp };
struct ImageFileInfo {
    uint32_t width{}, height{};
    ImageFileFormat format{};
};
// x/y are scroll offsets in the scaled image's canvas. Images smaller than
// the destination are centred; all other pixels are filled with black.
struct ImageViewport {
    uint32_t scaled_width{}, scaled_height{}, x{}, y{};
    uint16_t width{}, height{};
};
struct ImageReadControl {
    const std::atomic<bool> *closed{};
    const std::atomic<uint32_t> *revision{};
    uint32_t expected_revision{};
    bool cancelled() const {
        return (closed && closed->load()) ||
            (revision && revision->load() != expected_revision);
    }
};
// Caller holds media begin_file_access()/end_file_access() around background
// work. Sources stream through the audio-priority SD gate in bounded reads.
bool image_file_info(const char *path, ImageFileInfo *info, const ImageReadControl &control);
bool decode_image_file_viewport(const char *path, const ImageFileInfo &info,
                               const ImageViewport &view, uint16_t *pixels,
                               const ImageReadControl &control);
} // namespace lyra::media
