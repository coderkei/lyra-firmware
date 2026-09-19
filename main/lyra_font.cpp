/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lyra_font.h"

extern "C" {
extern const lv_font_t lyra_unicode_16;
extern const lv_font_t lyra_cjk_16;
}

namespace lyra::font {

namespace {

lv_font_t s_ui_font{};
bool s_initialized = false;

} // namespace

void init()
{
    if (s_initialized) return;
    s_ui_font = lyra_unicode_16;
    // The supplement contains only catalog-required Han glyphs. LVGL walks
    // this fallback only when the primary multilingual font has no glyph.
    s_ui_font.fallback = &lyra_cjk_16;
    s_initialized = true;
}

const lv_font_t *ui()
{
    init();
    return &s_ui_font;
}

} // namespace lyra::font
