# Lyra GUI catalog

`lyra_i18n_catalog.inc` is the source-of-truth catalog for the embedded GUI.
It is an X-macro file so the `StringId` enum and the flash-resident lookup
table are generated from the same list; an ID cannot be present in one and
missing from the other.

Each completed row uses `LYRA_I18N_ENTRY` with separate English, French,
German, Spanish, Italian, Japanese, Korean, Russian, Simplified Chinese, and
Traditional Chinese values. `LYRA_I18N_COUNT` is used for count messages and
stores singular/plural forms for every locale plus Russian one/few/many forms.
Technical identifiers, codec names, units, copyright notices, and product
names are explicitly kept unchanged where translating them would be incorrect;
those rows are documented in the catalog comments.

Run the completeness check from the repository root:

```text
python tools/i18n/check_i18n.py
```

Check the generated embedded font coverage with:

```text
python tools/i18n/check_font_manifest.py --write
python tools/i18n/check_font_manifest.py
```

The font check validates the generated LVGL cmaps used by the primary font and
the catalog-specific CJK fallback. `lyra_glyph_manifest.txt` records the
locale code points used for the repeatable GUI review.

The check scans `main/gui/` for direct string literals passed to `make_label()`
and validates every catalog locale, placeholder signature, and line-break
signature. New user-facing label text must be added to the catalog.

The catalog is included in the application image at compile time. It is not
loaded from MicroSD or any other runtime storage.
