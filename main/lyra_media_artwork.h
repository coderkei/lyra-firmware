/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "lyra_media.h"

namespace lyra::media::artwork {

// Creates the decoder gate; call once before any artwork task starts.
void init();
uint32_t key(const Track &track);
uint32_t cache_key(const Track &track);
uint64_t request_key(const Track &track, ArtworkSize size);
bool read_cache(uint32_t key, uint16_t artwork_size, uint16_t **out_pixels);
bool write_cache(uint32_t key, uint16_t artwork_size, const uint16_t *pixels);
// Serialized: only one cover decodes at a time.
bool extract_and_decode(const Track &track, uint16_t **out_pixels, uint16_t artwork_size,
                        ArtworkDiagnostics *diagnostics, bool allow_sd_backing,
                        bool *used_sd_backing);

} // namespace lyra::media::artwork

