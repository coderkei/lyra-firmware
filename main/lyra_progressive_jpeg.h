/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>
#include <cstdio>

namespace lyra::media {

bool decode_progressive_jpeg(const uint8_t *data, size_t length, uint16_t *pixels,
                             uint16_t target_size, bool preserve_aspect,
                             bool allow_sd_backing,
                             bool *used_sd_backing = nullptr);

} // namespace lyra::media
