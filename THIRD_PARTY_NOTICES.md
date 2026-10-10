# Third-party notices

The native lyra-firmware source in `main/`, `components/lyra_board/`, and
`board/JC3248W535EN/include/` is released under the Apache
License 2.0 in the accompanying `LICENSE` file. This notice records the
third-party software and reference material that must retain its own terms.

## Compiled dependencies

The versions below are the versions locked in `dependencies.lock`:

- **ESP-IDF 6.0.2** — Apache-2.0 for ESP-IDF code. ESP-IDF also contains
  third-party code under its own licenses; retain the ESP-IDF copyright and
  license information when redistributing a firmware or source bundle.
  See <https://github.com/espressif/esp-idf/blob/master/LICENSE> and
  <https://github.com/espressif/esp-idf/blob/master/docs/en/COPYRIGHT.rst>.
- **LVGL 9.4.0** — MIT. LVGL's bundled fonts and other third-party assets
  carry additional notices; check the LVGL distribution when those
  assets are enabled or redistributed. See
  <https://github.com/lvgl/lvgl/blob/master/docs/src/introduction/license.mdx>.
- **Source Han Sans 1.001 selected glyph data** — Apache-2.0. The generated
  `main/lyra_unicode_16.c` file is derived from the 1.001 release,
  whose Apache-2.0 terms match the firmware's existing source licence and
  retain the Adobe attribution in the generated file.
- **Source Han Sans CJK SC selected glyph data** — SIL Open Font License 1.1.
  The generated `main/lyra_cjk_16.c` catalog fallback is derived from the
  Source Han Sans CJK SC face distributed with LVGL. Retain the Adobe
  attribution and the OFL-1.1 terms when redistributing the firmware.
- **espressif/esp_audio_codec 2.6.2** — Espressif Modified MIT,
  `LicenseRef-Espressif-Modified-MIT`. Its terms require use with Espressif
  Systems products and prohibit source or binary redistribution for use with
  non-Espressif products. The exact license text is in the component's
  `LICENSE` file in an ESP-IDF component-manager checkout. See the
  [versioned component license](https://components.espressif.com/components/espressif/esp_audio_codec/versions/2.6.2/license)
  and the [upstream source](https://github.com/espressif/esp-adf-libs).
- **espressif/libjpeg-turbo 3.1.1~2** — Independent JPEG Group license,
  modified three-clause BSD license, and zlib license, as applicable to the
  included portions. Retain all upstream notices. Binary distributions must
  also include the required Independent JPEG Group attribution. See
  <https://github.com/libjpeg-turbo/libjpeg-turbo/blob/main/LICENSE.md>.
- **espressif/libpng 1.6.58** — PNG Reference Library License. Retain the
  upstream notice and identify modified source if this dependency is changed.
  See <https://github.com/pnggroup/libpng>.
- **espressif/zlib 1.3.2~1** — zlib license. Retain the copyright and license
  notice, and identify modified source if this dependency is changed. See
  <https://github.com/madler/zlib>.

## Bundled data assets

- **Offline IME dictionaries** — `tools/ime/lyra_ime_zh.bin` and
  `tools/ime/lyra_ime_ja.bin` are adapted database data distributed under
  [CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0/). The Chinese
  dictionary uses CC-CEDICT by MDBG and contributors (CC BY-SA 4.0):
  <https://cc-cedict.org/editor/editor.php?handler=Download>
  Jieba word-frequency data (MIT) ranks candidates; retain its source and
  <https://github.com/fxsjy/jieba/blob/master/LICENSE> notice. The Japanese
  dictionary uses JMdict, copyright James William Breen and EDRDG
  (CC BY-SA 4.0): <https://www.edrdg.org/edrdg/licence.html>. Source and
  generation details are in the
  [IME dictionary guide](tools/ime/README.md).

  Commercial redistribution is permitted under CC BY-SA 4.0 subject to its
  terms. Include appropriate attribution, a link to the license, and an
  indication that Lyra converts the source datasets into reading-to-candidate
  indexes, filters surfaces to embedded-font coverage, and ranks Chinese
  candidates with Jieba frequencies. Adapted dictionary data follows the
  license's ShareAlike condition. Keep its terms visible alongside the
  Apache-2.0 application source and other bundled component licenses.
