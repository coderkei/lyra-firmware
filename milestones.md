# Lyra GUI localisation milestones

This plan adds these GUI languages:

- English (`en`)
- French (`fr`)
- German (`de`)
- Spanish (`es`)
- Italian (`it`)
- Japanese (`ja`)
- Korean (`ko`)
- Russian (`ru`)
- Simplified Chinese (`zh-Hans`)
- Traditional Chinese (`zh-Hant`)

The localisation must remain available before MicroSD is mounted. Translation
data and the fonts required for the core GUI should therefore be compiled into
the application image, not loaded from the MicroSD card.

Do not run builds, flash the board, monitor it, or perform device validation
from the implementation agent. The repository owner is responsible for those
steps.

## Milestone 1 — Localisation foundation and string extraction

**Status: Complete**

The localisation foundation and GUI string extraction are implemented. The
catalog completeness check passes with 317 `StringId` entries, and the GUI
literal audit passes. Build, flash, and device validation remain the
repository owner's responsibility as stated above.

### Work to do

1. Add a small `main/i18n/` module containing:

   - a `Language` enum for the ten supported languages;
   - a typed `StringId` enum for translatable GUI messages;
   - `tr(StringId)` for static strings;
   - formatting helpers for messages containing values or counts;
   - language-name helpers using native names, for example `Français`,
     `Deutsch`, `日本語`, `한국어`, `简体中文`, and `繁體中文`.

2. Establish a source-of-truth translation file or files under `tools/i18n/`.
   A generator may emit compact C++ translation tables under `main/i18n/`.
   The generated firmware data must be UTF-8 and flash-resident.

3. Extract hard-coded GUI text from every renderer, including:

   - menu, queue, library, folders, playlists, and track details;
   - Now Playing and Equalizer screens;
   - search, playlist creation, and keyboard controls;
   - Settings, sorting, database, About, and Licenses screens;
   - scan/sort progress overlays and notices;
   - power, NVS, OTA, boot-test, and debug dialogs.

4. Keep user-provided content separate from GUI translations. Track titles,
   artists, albums, playlist names, folder names, and file paths must not be
   passed through the translation catalog.

5. Replace sentence construction such as `"%u songs"`, `"Add to %s"`, and
   `"Sort %s"` with complete message keys and formatting helpers. The design
   must support language-specific plural forms, especially Russian.

6. Add a completeness check which fails or clearly reports when a language is
   missing a `StringId`. English may be the fallback during development, but
   the final catalog must contain an intentional translation or intentional
   fallback for every key.

### Acceptance criteria

- English renders the same user-visible behaviour as before.
- No user-visible GUI sentence remains as an untracked literal in `main/gui/`.
- No translation file is required at runtime and the GUI does not depend on
  MicroSD availability.
- Format strings and plural handling do not assume English word order.
- The catalog can be extended without changing every screen renderer.

## Milestone 2 — Persisted language selection

**Status: Complete**

Language selection is persisted in the existing `lyra` NVS namespace, safely
defaults to English for missing or invalid values, and is available from a
native-name picker under System settings. Selecting a language rerenders the
active GUI without resetting playback, queue, or navigation state.

### Work to do

1. Extend `lyra::gui_settings::Values` and its NVS load/save implementation
   with a validated language value, using the existing `lyra` namespace.

2. Use English when the setting is absent or invalid. Preserve existing user
   settings and remain compatible with devices upgraded from older firmware.

3. Add a `Language` item under the System settings screen. Show the language
   names in their native forms so users can identify them even before changing
   language.

4. Apply a language change immediately by rebuilding the current GUI. Preserve
   the current navigation context, selected item, playback state, and active
   queue where practical.

5. Ensure startup loads the language before the first normal GUI render. The
   boot path must still work when NVS or MicroSD is unavailable.

6. Ensure dialogs, overlays, notices, progress labels, and dynamically updated
   labels use the selected language after a language change.

### Acceptance criteria

- Selecting any supported language persists across reboot.
- A missing or corrupt language value safely falls back to English.
- Changing language does not require a factory reset, rescan, or MicroSD card.
- The language picker itself remains usable in every supported language.
- Existing NVS settings continue to load and save correctly.
- Returning to a previous screen does not restore stale English labels.

## Milestone 3 — Translation content

The localisation foundation currently contains explicit English fallbacks so
the firmware can be integrated before the translation set is complete. This
milestone replaces those development fallbacks with reviewed content for all
ten supported languages.

### Work to do

1. Translate every user-visible catalog entry into French, German, Spanish,
   Italian, Japanese, Korean, Russian, Simplified Chinese, and Traditional
   Chinese, while retaining English as the source language.

2. Keep product names, hardware identifiers, file formats, paths, technical
   identifiers, and user-provided media content unchanged unless a catalog
   entry explicitly requires a translated presentation.

3. Provide complete locale-specific plural forms for every count message.
   Include Russian one/few/many forms and verify that formatted values remain
   in the correct grammatical position for each language.

4. Review every placeholder, newline, and intentional line break in translated
   messages. Formatting arguments must remain type-compatible and must not be
   reordered by sentence construction in the renderers.

5. Replace development fallbacks in the source-of-truth catalog with reviewed
   translations, documenting any deliberately unchanged technical text or
   locale-specific fallback.

6. Extend the catalog checks to report untranslated development fallbacks,
   missing locale entries, duplicate keys, malformed format arguments, and
   invalid UTF-8 before the translated catalog is used by firmware.

### Acceptance criteria

- Every supported language has reviewed content for every user-visible
  translation key.
- No user-visible language switch leaves the GUI on development English
  fallback text unless the unchanged text is explicitly documented.
- All plural messages are correct in English, Russian, and the other supported
  locales where their grammar differs.
- Every translated format string preserves the required arguments, line
  breaks, and display-safe UTF-8 text.
- User-provided titles, artists, albums, playlists, folders, and paths remain
  separate from the translation catalog.
- Selecting a supported language produces a visibly translated GUI without a
  firmware rebuild or MicroSD dependency at runtime.

## Milestone 4 — Font, layout, input, and localisation QA

### Work to do

1. Generate a glyph manifest from all translated strings and verify that every
   code point has a usable glyph. Extend the embedded font generation process
   when necessary.

2. Check CJK presentation carefully. Japanese, Korean, Simplified Chinese,
   and Traditional Chinese can require different glyph shapes for the same
   Unicode code point. Add locale-specific fallback fonts or subsets if the
   current Korean Source Han Sans-derived font is not visually appropriate.

3. Review the fixed 320x480 layouts for longer translations. Replace fragile
   fixed-width assumptions with suitable wrapping, ellipsis, scrolling, or
   flexible widths. Pay particular attention to headers, navigation controls,
   buttons, tabs, dialogs, settings rows, and progress messages.

4. Remove English-only uppercase assumptions. Use normal translated text for
   buttons and headings, and do not require casing transformations for CJK.

5. Add locale-aware formatting helpers for counts, sizes, dates, and other
   user-visible values where the UI exposes them. Keep technical identifiers,
   file formats, paths, and firmware IDs unchanged where appropriate.

6. Decide and document the search-input scope:

   - At minimum, retain working ASCII/Latin search input for the first release.
   - If full localized search is required, add keyboard layouts and UTF-8-safe
     text insertion for Cyrillic, Japanese, Korean, and Chinese input.
   - Treat Japanese IME, Korean composition, and Chinese Pinyin/input methods
     as separate features rather than assuming translation alone provides them.

7. Add a repeatable GUI review matrix covering every screen and dialog in all
   ten languages. Check for clipped text, missing glyph boxes, incorrect
   plural forms, stale labels after switching language, and navigation regressions.

### Acceptance criteria

- Every translated string renders with the intended glyphs.
- No supported language has clipped or overlapping core controls at 320x480.
- Russian plural forms and all formatted messages are correct.
- CJK strings use acceptable locale-specific glyph shapes.
- Search behaviour is either explicitly documented as limited or passes the
  agreed localized-input requirements.
- The owner can build, flash, and manually validate the resulting firmware on
  the JC3248W535EN board.

## Relevant existing files

- `main/lyra_gui_settings.h` and `main/lyra_gui_settings.cpp` — persisted GUI preferences.
- `main/gui/lyra_gui_internal.h` — GUI state, dimensions, and shared helpers.
- `main/gui/shared/lyra_gui_theme_widgets.cpp` — common label and widget creation.
- `main/gui/` — screen renderers containing current GUI text.
- `main/lyra_font.cpp` and `main/lyra_unicode_16.c` — embedded font integration.
- `tools/generate_lyra_unicode_font.py` — embedded font generation.
- `docs/EMBEDDED_FONT.md` — current font coverage and generation requirements.
- `docs/JC3248W535EN_HARDWARE.md` — board and display constraints.
- `docs/EMOTIVATE_LYRA_OS_SPEC_JC3248W535EN.md` — firmware architecture and behaviour.
