/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lyra_font.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "esp_heap_caps.h"

// Glyphs are fetched and consumed one at a time by the software renderer, so
// a cached mask is never in use when the next lookup resets the cache.
#if LV_USE_OS != LV_OS_NONE
#error "The Lyra glyph cache assumes LVGL renders on a single thread"
#endif

extern "C" {
extern const lv_font_t lyra_unicode_16;
extern const lv_font_t lyra_cjk_16;
}

namespace lyra::font {

namespace {

lv_font_t s_ui_font{};
lv_font_t s_cjk_font{};
lv_font_t s_symbol_font{};
bool s_initialized = false;

using GlyphDscCallback = bool (*)(const lv_font_t *, lv_font_glyph_dsc_t *, uint32_t, uint32_t);
using GlyphBitmapCallback = const void *(*)(lv_font_glyph_dsc_t *, lv_draw_buf_t *);

// Direct-mapped glyph lookups for one font, keyed by code point. ASCII skips
// the table: the first character map resolves it in a few instructions.
constexpr size_t kDscEntries = 512;
constexpr uint32_t kFirstCachedLetter = 0x80;

struct DscEntry {
    uint32_t letter;
    bool found;
    lv_font_glyph_dsc_t dsc;
};

struct CachedFont {
    const lv_font_t *font;
    GlyphDscCallback get_dsc;
    GlyphBitmapCallback get_bitmap;
    DscEntry *lookups;
};

constexpr size_t kMaxCachedFonts = 8;
CachedFont s_fonts[kMaxCachedFonts];
size_t s_font_count = 0;

// Decoded A8 masks share one PSRAM arena behind an open-addressed index.
// When either fills, both are cleared and refilled by the glyphs in use.
constexpr size_t kArenaBytes = 160 * 1024;
constexpr size_t kIndexSlots = 2048;
constexpr size_t kMaxEntries = kIndexSlots / 2;
constexpr uint32_t kMaxGlyphBytes = 4096;

struct GlyphMask {
    uint32_t key;
    lv_draw_buf_t buffer;
};
constexpr size_t kMaskHeaderBytes = (sizeof(GlyphMask) + 3u) & ~size_t{3};

uint8_t *s_arena = nullptr;
size_t s_arena_used = 0;
GlyphMask **s_index = nullptr;
size_t s_mask_count = 0;
bool s_mask_cache_unavailable = false;

size_t font_slot(const lv_font_t *font)
{
    for (size_t i = 0; i < s_font_count; ++i) {
        if (s_fonts[i].font == font) return i;
    }
    return kMaxCachedFonts;
}

bool ensure_mask_cache()
{
    if (s_index) return true;
    if (s_mask_cache_unavailable) return false;
    s_arena = static_cast<uint8_t *>(heap_caps_malloc(kArenaBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_index = static_cast<GlyphMask **>(heap_caps_calloc(
        kIndexSlots, sizeof(GlyphMask *), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_arena && s_index) return true;
    heap_caps_free(s_arena);
    heap_caps_free(s_index);
    s_arena = nullptr;
    s_index = nullptr;
    s_mask_cache_unavailable = true;
    return false;
}

size_t index_slot(uint32_t key)
{
    return (key * 2654435761u) >> 21;  // Top 11 bits: kIndexSlots == 2048.
}
static_assert(kIndexSlots == 2048);

GlyphMask *find_mask(uint32_t key)
{
    for (size_t slot = index_slot(key);; slot = (slot + 1) & (kIndexSlots - 1)) {
        GlyphMask *mask = s_index[slot];
        if (!mask || mask->key == key) return mask;
    }
}

GlyphMask *insert_mask(uint32_t key, uint32_t bytes)
{
    const size_t needed = kMaskHeaderBytes + ((bytes + 3u) & ~3u);
    if (s_mask_count >= kMaxEntries || s_arena_used + needed > kArenaBytes) {
        std::memset(s_index, 0, kIndexSlots * sizeof(GlyphMask *));
        s_arena_used = 0;
        s_mask_count = 0;
    }
    auto *mask = reinterpret_cast<GlyphMask *>(s_arena + s_arena_used);
    s_arena_used += needed;
    ++s_mask_count;
    mask->key = key;
    size_t slot = index_slot(key);
    while (s_index[slot]) slot = (slot + 1) & (kIndexSlots - 1);
    s_index[slot] = mask;
    return mask;
}

bool cached_glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *out,
                      uint32_t letter, uint32_t letter_next)
{
    const size_t slot = font_slot(font);
    if (slot == kMaxCachedFonts) return false;
    const CachedFont &cached = s_fonts[slot];
    if (!cached.lookups || letter < kFirstCachedLetter) {
        return cached.get_dsc(font, out, letter, letter_next);
    }
    DscEntry &entry = cached.lookups[letter & (kDscEntries - 1)];
    if (entry.letter == letter) {
        if (entry.found) *out = entry.dsc;
        return entry.found;
    }
    const bool found = cached.get_dsc(font, out, letter, letter_next);
    entry.letter = letter;
    entry.found = found;
    if (found) entry.dsc = *out;
    return found;
}

const void *cached_glyph_bitmap(lv_font_glyph_dsc_t *glyph, lv_draw_buf_t *draw_buf)
{
    const size_t slot = font_slot(glyph->resolved_font);
    if (slot == kMaxCachedFonts) return nullptr;
    const GlyphBitmapCallback decode = s_fonts[slot].get_bitmap;
    if (glyph->req_raw_bitmap || !draw_buf || !ensure_mask_cache()) return decode(glyph, draw_buf);

    const uint32_t key = (static_cast<uint32_t>(slot + 1) << 24) | (glyph->gid.index & 0xFFFFFFu);
    if (GlyphMask *mask = find_mask(key)) return &mask->buffer;

    // Only masks decoded into LVGL's own A8 buffer are kept; anything else
    // is returned untouched.
    const void *decoded = decode(glyph, draw_buf);
    if (decoded != draw_buf || draw_buf->header.cf != LV_COLOR_FORMAT_A8) return decoded;
    const uint32_t width = draw_buf->header.w;
    const uint32_t height = draw_buf->header.h;
    const uint32_t stride = draw_buf->header.stride;
    const uint32_t bytes = stride * height;
    if (bytes == 0 || bytes > kMaxGlyphBytes) return decoded;

    GlyphMask *mask = insert_mask(key, bytes);
    uint8_t *pixels = reinterpret_cast<uint8_t *>(mask) + kMaskHeaderBytes;
    std::memcpy(pixels, draw_buf->data, bytes);
    lv_draw_buf_init(&mask->buffer, width, height, LV_COLOR_FORMAT_A8, stride, pixels, bytes);
    return &mask->buffer;
}

} // namespace

void enable_glyph_cache(lv_font_t *font)
{
    if (!font || font->get_glyph_bitmap == cached_glyph_bitmap) return;
    if (s_font_count == kMaxCachedFonts) return;
    CachedFont &cached = s_fonts[s_font_count];
    cached = {font, font->get_glyph_dsc, font->get_glyph_bitmap, nullptr};
    // Kerning makes a lookup depend on the following letter, so only fonts
    // without kerning data can reuse a lookup by code point alone.
    if (font->get_glyph_dsc == lv_font_get_glyph_dsc_fmt_txt &&
        static_cast<const lv_font_fmt_txt_dsc_t *>(font->dsc)->kern_dsc == nullptr) {
        cached.lookups = static_cast<DscEntry *>(heap_caps_calloc(
            kDscEntries, sizeof(DscEntry), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    ++s_font_count;
    font->get_glyph_dsc = cached_glyph_dsc;
    font->get_glyph_bitmap = cached_glyph_bitmap;
}

void init()
{
    if (s_initialized) return;
    // LV_SYMBOL icons come from Montserrat at the end of the chain. Copies
    // let the glyph cache cover every font a label can fall back to.
    s_symbol_font = lv_font_montserrat_16;
    s_cjk_font = lyra_cjk_16;
    s_cjk_font.fallback = &s_symbol_font;
    s_ui_font = lyra_unicode_16;
    // The supplement contains only catalog-required Han glyphs. LVGL walks
    // this fallback only when the primary multilingual font has no glyph.
    s_ui_font.fallback = &s_cjk_font;
    enable_glyph_cache(&s_ui_font);
    enable_glyph_cache(&s_cjk_font);
    enable_glyph_cache(&s_symbol_font);
    s_initialized = true;
}

const lv_font_t *ui()
{
    init();
    return &s_ui_font;
}

} // namespace lyra::font
