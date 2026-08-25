/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lyra_font.h"

extern "C" {
extern const lv_font_t lyra_unicode_16;
}

namespace lyra::font {

void init() {}

const lv_font_t *ui() { return &lyra_unicode_16; }

} // namespace lyra::font
