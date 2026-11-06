/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "lvgl.h"

namespace lyra::font {

// The 16 px multilingual font is compiled into the application image, so it
// is available before the MicroSD card is mounted.
void init();
const lv_font_t *ui();

// Keeps decoded A8 glyph masks for `font` in PSRAM. LVGL otherwise expands
// (and, for compressed fonts, decompresses) every glyph on every redraw.
// fmt_txt fonts without kerning also remember glyph lookups, including
// misses, so fallback chains stop rescanning their character maps. Call
// from the LVGL task before the font is drawn; the font must stay alive.
void enable_glyph_cache(lv_font_t *font);

} // namespace lyra::font
