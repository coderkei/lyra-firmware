/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>
#include <cstdio>

namespace lyra::media {

bool decode_png(const uint8_t *data, size_t length, uint16_t *pixels,
                uint16_t target_width, uint16_t target_height,
                bool preserve_aspect, uint32_t transparent_background_rgb);

} // namespace lyra::media
