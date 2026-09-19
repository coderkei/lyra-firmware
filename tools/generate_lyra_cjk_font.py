#!/usr/bin/env python3
"""Generate Lyra's catalog-specific CJK fallback font.

The primary Lyra font contains a broad multilingual repertoire.  This small
supplement is generated from the actual translated catalog so a stale or
locale-inappropriate primary CJK subset cannot turn a translated label into a
missing-glyph box.  It is intended to be chained behind the primary font in
LVGL and uses the same 20 px line metrics as the primary UI font.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).parent / "i18n"))
from check_i18n import decode_string_argument, macro_invocations, split_arguments


FONT_SIZE_PX = 16
BPP = 2
DEFAULT_CATALOG = Path("tools/i18n/lyra_i18n_catalog.inc")
DEFAULT_OUTPUT = Path("main/lyra_cjk_16.c")


def catalog_han_symbols(catalog: Path) -> str:
    """Return sorted Han characters used by every translated catalog value."""
    points: set[int] = set()
    for macro, raw in macro_invocations(catalog.read_text(encoding="utf-8")):
        if macro not in {"LYRA_I18N_ENTRY", "LYRA_I18N_COUNT"}:
            continue
        for argument in split_arguments(raw)[1:]:
            value = decode_string_argument(argument)
            if value is None:
                raise ValueError(f"{catalog}: invalid catalog string")
            points.update(ord(character) for character in value
                          if 0x4E00 <= ord(character) <= 0x9FFF)
    if not points:
        raise RuntimeError(f"no Han characters found in {catalog}")
    return "".join(chr(point) for point in sorted(points))


def converter_command(converter: str) -> list[str]:
    executable = shutil.which(converter)
    if not executable:
        raise RuntimeError(
            "lv_font_conv 1.5.3 is required; install it with "
            "`npm install --global lv_font_conv@1.5.3` or pass --converter"
        )
    command = [executable]
    if Path(executable).suffix.lower() == ".js":
        node = shutil.which("node")
        if not node:
            raise RuntimeError("Node.js is required when --converter names a .js file")
        command = [node, executable]
    return command


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--font", type=Path, required=True,
        help="open-source Source Han Sans CJK face used for the supplement",
    )
    parser.add_argument("--catalog", type=Path, default=DEFAULT_CATALOG)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--converter", default="lv_font_conv")
    args = parser.parse_args()

    if not args.font.is_file():
        parser.error(f"font source not found: {args.font}")
    if not args.catalog.is_file():
        parser.error(f"catalog not found: {args.catalog}")

    symbols = catalog_han_symbols(args.catalog)
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    command = converter_command(args.converter) + [
        "--bpp", str(BPP),
        "--size", str(FONT_SIZE_PX),
        "--font", str(args.font.resolve()),
        "--symbols", symbols,
        "--no-kerning",
        "--lv-fallback", "lv_font_montserrat_16",
        "--format", "lvgl",
        "--lv-include", "lvgl.h",
        "--lv-font-name", "lyra_cjk_16",
        "--output", str(output),
    ]
    print(f"Generating {output} with {len(symbols)} catalog Han ideographs.")
    subprocess.run(command, check=True)

    generated = output.read_text(encoding="utf-8")
    generated = generated.split("*/", 1)[1].lstrip("\r\n")
    generated, line_height_count = re.subn(
        r"\.line_height = \d+", ".line_height = 20", generated, count=1
    )
    generated, base_line_count = re.subn(
        r"\.base_line = \d+", ".base_line = 5", generated, count=1
    )
    if line_height_count != 1 or base_line_count != 1:
        raise RuntimeError("lv_font_conv output did not contain expected font metrics")
    provenance = """/*
 * SPDX-FileCopyrightText: 2014-2021 Adobe Systems Incorporated
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: OFL-1.1
 *
 * Generated from Source Han Sans CJK SC by
 * tools/generate_lyra_cjk_font.py.  The source font is not shipped.
 * The glyph set is derived from tools/i18n/lyra_i18n_catalog.inc.
 */

"""
    output.write_text(provenance + generated, encoding="utf-8", newline="\n")
    print(f"Wrote {output.stat().st_size:,} bytes.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
