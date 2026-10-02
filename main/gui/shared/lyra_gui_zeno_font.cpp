/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../lyra_gui_internal.h"

namespace lyra::gui::internal {
namespace {

constexpr int kSourcePixels = 16;
const int kHeadingPixels = 28;
const int kMenuPixels = 40;

int scale_metric(int value, int pixels)
{
    // Round negative bearings symmetrically as well as positive dimensions.
    return (value * pixels + (value < 0 ? -kSourcePixels / 2 : kSourcePixels / 2)) / kSourcePixels;
}

bool scaled_glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *out,
                      uint32_t letter, uint32_t next)
{
    lv_font_glyph_dsc_t source{};
    if (!lv_font_get_glyph_dsc(lyra::font::ui(), &source, letter, next) || source.is_placeholder) return false;
    const int pixels = *static_cast<const int *>(font->dsc);
    *out = source;
    out->adv_w = scale_metric(source.adv_w, pixels);
    out->box_w = scale_metric(source.box_w, pixels);
    out->box_h = scale_metric(source.box_h, pixels);
    out->ofs_x = scale_metric(source.ofs_x, pixels);
    out->ofs_y = scale_metric(source.ofs_y, pixels);
    out->format = LV_FONT_GLYPH_FORMAT_A8;
    out->stride = 0;
    out->entry = nullptr;
    out->resolved_font = font;
    // The bitmap callback resolves the original glyph through the complete
    // multilingual chain again; no pointer into a temporary descriptor lives on.
    out->gid.index = letter;
    return true;
}

const void *scaled_glyph_bitmap(lv_font_glyph_dsc_t *glyph, lv_draw_buf_t *output)
{
    if (!output || glyph->box_w == 0 || glyph->box_h == 0) return nullptr;
    lv_font_glyph_dsc_t source{};
    if (!lv_font_get_glyph_dsc(lyra::font::ui(), &source, glyph->gid.index, 0) ||
        source.is_placeholder || source.box_w == 0 || source.box_h == 0) return nullptr;
    lv_draw_buf_t *scratch = lv_draw_buf_create(source.box_w, source.box_h, LV_COLOR_FORMAT_A8, 0);
    if (!scratch) return nullptr;
    const auto *bitmap = static_cast<const lv_draw_buf_t *>(lv_font_get_glyph_bitmap(&source, scratch));
    if (!bitmap) {
        lv_font_glyph_release_draw_data(&source);
        lv_draw_buf_destroy(scratch);
        return nullptr;
    }

    // Decode just this glyph with LVGL, then interpolate its alpha mask. Only
    // fallback glyphs take this path; native Montserrat glyphs stay untouched.
    // LVGL owns the output mask, and the small source mask is freed below.
    const int width = glyph->box_w, height = glyph->box_h;
    for (int y = 0; y < height; ++y) {
        const int sy = std::clamp(((2 * y + 1) * source.box_h * 128) / height - 128,
                                  0, (source.box_h - 1) * 256);
        const int y0 = sy / 256, y1 = std::min(y0 + 1, source.box_h - 1), fy = sy % 256;
        uint8_t *row = output->data + y * output->header.stride;
        std::memset(row, 0, output->header.stride);
        for (int x = 0; x < width; ++x) {
            const int sx = std::clamp(((2 * x + 1) * source.box_w * 128) / width - 128,
                                      0, (source.box_w - 1) * 256);
            const int x0 = sx / 256, x1 = std::min(x0 + 1, source.box_w - 1), fx = sx % 256;
            const uint8_t *top = bitmap->data + y0 * bitmap->header.stride;
            const uint8_t *bottom = bitmap->data + y1 * bitmap->header.stride;
            const int upper = top[x0] * (256 - fx) + top[x1] * fx;
            const int lower = bottom[x0] * (256 - fx) + bottom[x1] * fx;
            row[x] = static_cast<uint8_t>((upper * (256 - fy) + lower * fy + 32768) / 65536);
        }
    }
    lv_font_glyph_release_draw_data(&source);
    lv_draw_buf_destroy(scratch);
    lv_draw_buf_flush_cache(output, nullptr);
    return output;
}

lv_font_t make_scaled_font(const int *pixels)
{
    lv_font_t font{};
    font.get_glyph_dsc = scaled_glyph_dsc;
    font.get_glyph_bitmap = scaled_glyph_bitmap;
    font.line_height = scale_metric(lyra::font::ui()->line_height, *pixels);
    font.base_line = scale_metric(lyra::font::ui()->base_line, *pixels);
    font.underline_position = scale_metric(lyra::font::ui()->underline_position, *pixels);
    font.underline_thickness = std::max(1, scale_metric(lyra::font::ui()->underline_thickness, *pixels));
    font.dsc = pixels;
    return font;
}

} // namespace

const lv_font_t *zeno_heading_font(bool large)
{
    static const lv_font_t unicode_heading = make_scaled_font(&kHeadingPixels);
    static const lv_font_t unicode_menu = make_scaled_font(&kMenuPixels);
    static lv_font_t heading = lv_font_montserrat_28;
    static lv_font_t menu = lv_font_montserrat_40;
    // Font packs need common line metrics: allow the taller multilingual
    // glyphs to fit while retaining the original English glyph bitmaps.
    heading.line_height = unicode_heading.line_height;
    heading.base_line = unicode_heading.base_line;
    heading.fallback = &unicode_heading;
    menu.line_height = unicode_menu.line_height;
    menu.base_line = unicode_menu.base_line;
    menu.fallback = &unicode_menu;
    return large ? &menu : &heading;
}

} // namespace lyra::gui::internal
