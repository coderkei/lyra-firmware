# Embedded 16 px multilingual font

`main/lyra_unicode_16.c` is the font used for Lyra UI text and music metadata.
It is compiled into the application image, so labels can render before
MicroSD is mounted. It is not a MicroSD or LittleFS asset.

The generated LVGL font uses 2-bit glyph data and LVGL font compression.
`sdkconfig.defaults` enables `CONFIG_LV_USE_FONT_COMPRESSED` and the
application partition provides the required space.

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

The generator defines the glyph ranges, applies the 16 px metrics, and writes
`main/lyra_unicode_16.c`.
