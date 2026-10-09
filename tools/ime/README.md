# Offline IME dictionaries

Chinese Pinyin and Japanese Kana conversion use small, independently compressed
dictionary blocks on the MicroSD card. The firmware loads the index and one
block at a time; the data is not embedded in the application image.

## Install

Copy both generated files to the SD card's `.lyra/ime/` directory. On Windows,
from the repository root, run:

```powershell
.\tools\ime\install-dictionaries.ps1 -SdRoot E:\
```

Replace `E:\` with the SD card's drive root. The installed paths must be:

```text
.lyra/ime/lyra_ime_zh.bin
.lyra/ime/lyra_ime_ja.bin
```

Without the Chinese file, Pinyin remains visible as unconverted text. Without
the Japanese file, Kana can still be entered and saved, but Kanji conversion
will not be available.

## Generate

Download the current CC-CEDICT gzip text, Jieba frequency dictionary, and
JMdict gzip XML from their upstream projects. Put them in a local source folder
and invoke:

```powershell
python tools/ime/build_ime_dictionary.py `
  --cedict tools/ime/source/cc-cedict.txt.gz `
  --jieba tools/ime/source/jieba_dict.txt `
  --jmdict tools/ime/source/JMdict.gz
```

The generator filters candidate surfaces against the glyphs already provided
by Lyra's embedded fonts. It groups keys in sorted blocks and compresses each
block separately so runtime lookups use bounded RAM. The checked-in assets were
generated from 86,523 Chinese readings and 219,055 Japanese readings; they are
about 4.63 MB combined. Regenerate the glyph manifest after changing either
dictionary:

```powershell
py -3 tools/i18n/check_font_manifest.py --write
py -3 tools/i18n/check_font_manifest.py
```

## Data attribution and licenses

The generated Chinese and Japanese dictionary assets are redistributed under
CC BY-SA 4.0 as adapted database data. Their source datasets and attribution
are:

* CC-CEDICT, by MDBG and contributors, CC BY-SA 4.0:
  <https://cc-cedict.org/editor/editor.php?handler=Download>
* JMdict, by the Electronic Dictionary Research and Development Group,
  CC BY-SA 4.0: <https://www.edrdg.org/edrdg/licence.html>
* Jieba word-frequency data, MIT License, used to rank Chinese candidates:
  <https://github.com/fxsjy/jieba>

Lyra transforms these sources into reading-to-candidate indexes, filters
surfaces to embedded-font coverage, and ranks Chinese candidates with Jieba
frequencies. The in-app **Licenses** screen contains the corresponding
attribution notices.
