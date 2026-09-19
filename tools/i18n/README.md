# Lyra GUI catalog

`lyra_i18n_catalog.inc` is the source-of-truth catalog for the embedded GUI.
It is an X-macro file so the `StringId` enum and the flash-resident lookup
table are generated from the same list; an ID cannot be present in one and
missing from the other.

Milestone 1 uses `LYRA_I18N_FALLBACK` for untranslated locales. The fallback
is intentional and explicit. Use `LYRA_I18N_ENTRY` for a row with separate
English, French, German, Spanish, Italian, Japanese, Korean, Russian,
Simplified Chinese, and Traditional Chinese values. Use `LYRA_I18N_COUNT` or
`LYRA_I18N_COUNT_RU` for messages requiring one/few/many plural forms.
`LYRA_I18N_COUNT_RU` demonstrates the Russian one/few/many forms while
retaining explicit English fallback for the other locales.

Run the completeness check from the repository root:

```text
python tools/i18n/check_i18n.py
```

The check also scans `main/gui/` for direct string literals passed to
`make_label()`. Only fixed product/board identifiers, symbols, and display
placeholders are allowlisted; new user-facing label text must be added to the
catalog.

The catalog is included in the application image at compile time. It is not
loaded from MicroSD or any other runtime storage.
