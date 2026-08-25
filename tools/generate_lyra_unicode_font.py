#!/usr/bin/env python3
"""Build Lyra's compact 16 px, embedded Unicode font.

The resulting C source is intended for the ESP32 application image, where
read-only data lives in internal flash.  It is not a MicroSD or LittleFS asset.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys


FONT_SIZE_PX = 16
BPP = 2

# Latin covers western and central/eastern European titles, Vietnamese, Greek,
# and Cyrillic.  The remaining ranges cover punctuation, currency, common
# music/UI symbols, Japanese syllabaries, Korean Jamo/syllables, and CJK forms.
BASE_RANGES = (
    "0x0020-0x02AF,0x0370-0x052F,0x1E00-0x1EFF,"
    "0x2000-0x206F,0x20A0-0x20CF,0x2100-0x214F,0x2190-0x22FF,"
    "0x2460-0x27BF,0x2E80-0x2EFF,0x3000-0x303F,0x3040-0x30FF,"
    "0x3100-0x312F,0x3130-0x318F,0x31A0-0x31EF,0x3200-0x33FF,"
    "0xA960-0xA97F,0xAC00-0xD7FF,0xF900-0xFAFF,0xFE10-0xFE6F,"
    "0xFF00-0xFFEF"
)


def encoded_han(charset: str, first_byte_range: range | None = None) -> set[int]:
    """Return Han code points provided by a standard legacy charset.

    GB 2312 and JIS X 0208 supply the high-value Simplified Chinese and
    Japanese vocabulary.  Big5 level 1 supplies the commonly used Traditional
    Chinese vocabulary without bringing in every rare extension ideograph.
    """

    points: set[int] = set()
    for point in range(0x4E00, 0xA000):
        character = chr(point)
        try:
            encoded = character.encode(charset)
        except UnicodeEncodeError:
            continue
        if len(encoded) != 2:
            continue
        if first_byte_range is not None and encoded[0] not in first_byte_range:
            continue
        points.add(point)
    return points


def compact_han_symbols() -> str:
    points = encoded_han("gb2312")
    points.update(encoded_han("euc_jp"))
    points.update(encoded_han("big5", range(0xA4, 0xC7)))
    return "".join(chr(point) for point in sorted(points))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--font",
        type=Path,
        required=True,
        help="Apache-2.0 Source Han Sans 1.001 Korean OTF source",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("main/lyra_unicode_16.c"),
        help="generated LVGL C source (default: main/lyra_unicode_16.c)",
    )
    parser.add_argument(
        "--converter",
        default="lv_font_conv",
        help="lv_font_conv 1.5.3 executable (default: resolve from PATH)",
    )
    args = parser.parse_args()

    if not args.font.is_file():
        parser.error(f"font source not found: {args.font}")
    converter = shutil.which(args.converter)
    if not converter:
        parser.error(
            "lv_font_conv 1.5.3 is required; install it with "
            "`npm install --global lv_font_conv@1.5.3` or pass --converter"
        )

    # Invoking the Windows .cmd wrapper routes the long --symbols argument
    # through cmd.exe, whose 8 KiB command-line limit is too small for the
    # selected Han repertoire.  A direct Node invocation retains the native
    # Windows command-line limit.
    converter_command = [converter]
    if Path(converter).suffix.lower() == ".js":
        node = shutil.which("node")
        if not node:
            parser.error("Node.js is required when --converter names a .js file")
        converter_command = [node, converter]

    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    han = compact_han_symbols()
    command = converter_command + [
        "--bpp",
        str(BPP),
        "--size",
        str(FONT_SIZE_PX),
        "--font",
        str(args.font.resolve()),
        "--range",
        BASE_RANGES,
        "--symbols",
        han,
        "--no-kerning",
        "--lv-fallback",
        "lv_font_montserrat_16",
        "--format",
        "lvgl",
        "--lv-include",
        "lvgl.h",
        "--lv-font-name",
        "lyra_unicode_16",
        "--output",
        str(output),
    ]
    print(f"Generating {output} with {len(han)} selected Han ideographs.")
    subprocess.run(command, check=True)

    generated = output.read_text(encoding="utf-8")
    # The converter's initial banner embeds both the machine-local source path
    # and the entire Han list.  It is not useful in a checked-in generated
    # source file; provenance below is sufficient and reproducible.
    generated = generated.split("*/", 1)[1].lstrip("\r\n")

    # The 1.001 source's vertical FontBBox makes lv_font_conv report a 31 px
    # line height even though the generated glyphs are 16 px.  A 20/5 metric
    # matches LVGL's built-in Source Han 16 px convention and preserves the
    # glyphs' baseline offsets.
    generated, line_height_count = re.subn(
        r"\.line_height = \d+", ".line_height = 20", generated, count=1
    )
    generated, base_line_count = re.subn(
        r"\.base_line = \d+", ".base_line = 5", generated, count=1
    )
    if line_height_count != 1 or base_line_count != 1:
        raise RuntimeError("lv_font_conv output did not contain expected font metrics")
    provenance = """/*
 * SPDX-FileCopyrightText: 2014 Adobe Systems Incorporated
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Generated from Apache-2.0 Source Han Sans 1.001 (Korean face) by
 * tools/generate_lyra_unicode_font.py.  The source font is not shipped.
 */

"""
    output.write_text(provenance + generated, encoding="utf-8", newline="\n")
    print(f"Wrote {output.stat().st_size:,} bytes.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
