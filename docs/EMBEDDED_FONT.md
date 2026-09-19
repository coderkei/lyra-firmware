# Embedded 16 px multilingual font

`main/lyra_unicode_16.c` is the primary font used for Lyra UI text and music
metadata. `main/lyra_cjk_16.c` is a compact catalog-specific CJK supplement
chained behind it through LVGL's fallback pointer. Both are compiled into the
application image, so labels can render before MicroSD is mounted. Neither is
a MicroSD or LittleFS asset.

The generated LVGL font uses 2-bit glyph data and LVGL font compression.
`sdkconfig.defaults` enables `CONFIG_LV_USE_FONT_COMPRESSED` and the
application partition provides the required space.

The primary font remains the broad multilingual Source Han Sans-derived face.
The supplement is generated from the translated catalog with the Source Han
Sans CJK SC face distributed in the LVGL dependency tree. It covers catalog
code points absent from the primary subset, including the Simplified Chinese
characters that previously rendered as boxes. Both fonts use the same 20 px
line metrics at runtime.

## Coverage

The font supports the below:

- Latin, Greek, Cyrillic, common punctuation, currency, and music symbols.
- Hiragana, Katakana, Bopomofo, CJK punctuation/forms, and compatibility
  ideographs.
- Hangul syllables and Jamo.
- Common Simplified Chinese, Japanese, and Traditional Chinese ideographs
  selected from GB 2312, JIS X 0208, and Big5 level 1 mappings.

## Provenance and licence

The generated data is derived from the Korean face of Source Han Sans 1.001,
an Apache-2.0 release. Please see
[`THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md).

## Generating the font

1. Obtain the Source Han Sans 1.001 Korean front in OTF format.
2. Install `lv_font_conv` 1.5.3 and make it available on `PATH`, or supply
   its path with `--converter`.
3. From `lyra-firmware`, run:

   ```powershell
   py -3.12 tools/generate_lyra_unicode_font.py --font path/to/SourceHanSansKR-Normal.otf
   ```

4. Generate the catalog-specific fallback from the LVGL-bundled open-source
   Source Han Sans CJK SC face:

   ```powershell
   py -3.12 tools/generate_lyra_cjk_font.py `
       --font managed_components/lvgl__lvgl/scripts/built_in_font/SourceHanSansSC-Normal.otf
   ```

5. Check the catalog's complete glyph coverage and refresh the review manifest:

   ```powershell
   py -3.12 tools/i18n/check_font_manifest.py --write
   py -3.12 tools/i18n/check_font_manifest.py
   ```

The primary generator defines the glyph ranges, includes every Han code point
used by the catalog, applies the 16 px metrics, and writes
`main/lyra_unicode_16.c`. The fallback generator writes
`main/lyra_cjk_16.c`; the manifest checker reads the generated LVGL cmaps
without compiling the firmware and fails if any translated code point is
absent from both fonts.
