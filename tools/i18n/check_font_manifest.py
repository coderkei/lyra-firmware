#!/usr/bin/env python3
"""Check translated catalog glyph coverage and write a review manifest."""

from __future__ import annotations

import argparse
import binascii
from pathlib import Path
import re
import struct
import sys
import zlib

from check_i18n import decode_string_argument, macro_invocations, split_arguments


CATALOG = Path(__file__).with_name("lyra_i18n_catalog.inc")
DEFAULT_FONTS = (
    Path(__file__).parents[2] / "main/lyra_unicode_16.c",
    Path(__file__).parents[2] / "main/lyra_cjk_16.c",
)
DEFAULT_MANIFEST = Path(__file__).with_name("lyra_glyph_manifest.txt")
KEYBOARD_SOURCES = (
    Path(__file__).parents[2] / "main/gui/search/lyra_gui_search.cpp",
    Path(__file__).parents[2] / "main/gui/search/lyra_gui_ime.cpp",
)
IME_DICTIONARIES = (
    Path(__file__).parents[1] / "ime/lyra_ime_zh.bin",
    Path(__file__).parents[1] / "ime/lyra_ime_ja.bin",
)
IME_HEADER = struct.Struct("<8sIIIIII")
IME_INDEX = struct.Struct("<B48sIIII")
LOCALES = ("en", "fr", "de", "es", "it", "ja", "ko", "ru", "zh-Hans", "zh-Hant")


def catalog_locale_values(catalog: Path) -> dict[str, list[str]]:
    values = {locale: [] for locale in LOCALES}
    for macro, raw in macro_invocations(catalog.read_text(encoding="utf-8")):
        decoded = [decode_string_argument(argument)
                   for argument in split_arguments(raw)[1:]]
        if any(value is None for value in decoded):
            raise ValueError(f"{macro}: invalid catalog string")
        if macro == "LYRA_I18N_ENTRY":
            if len(decoded) != len(LOCALES):
                raise ValueError(f"{macro}: expected {len(LOCALES)} values")
            for locale, value in zip(LOCALES, decoded):
                values[locale].append(value)
        elif macro == "LYRA_I18N_COUNT":
            groups = ((0, 1), (2, 3), (4, 5), (6, 7), (8, 9),
                      (10, 11), (12, 13), (14, 15, 16), (17, 18), (19, 20))
            if len(decoded) != 21:
                raise ValueError(f"{macro}: expected 21 values")
            for locale, indexes in zip(LOCALES, groups):
                values[locale].extend(decoded[index] for index in indexes)
    return values


def font_codepoints(path: Path) -> set[int]:
    """Extract LVGL cmap coverage from generated C without compiling it."""
    text = path.read_text(encoding="utf-8")
    arrays = {
        name: [int(value, 16) for value in re.findall(r"0x([0-9A-Fa-f]+)", body)]
        for name, body in re.findall(
            r"static const uint16_t (unicode_list_\d+)\[\] = \{(.*?)\};",
            text, re.S,
        )
    }
    coverage: set[int] = set()
    entry_pattern = re.compile(
        r"\.range_start\s*=\s*(\d+),\s*\.range_length\s*=\s*(\d+),"
        r"\s*\.glyph_id_start\s*=\s*\d+,\s*"
        r"\.unicode_list\s*=\s*(NULL|unicode_list_\d+),\s*"
        r"\.glyph_id_ofs_list\s*=\s*(NULL|glyph_id_ofs_list_\d+),\s*"
        r"\.list_length\s*=\s*(\d+),\s*\.type\s*=\s*(\w+)"
    )
    entries = list(entry_pattern.finditer(text))
    if not entries:
        raise ValueError(f"no LVGL cmaps found in {path}")
    for entry in entries:
        start, length = (int(entry.group(index)) for index in (1, 2))
        unicode_list = entry.group(3)
        list_length = int(entry.group(5))
        cmap_type = entry.group(6)
        if cmap_type.startswith("LV_FONT_FMT_TXT_CMAP_FORMAT0"):
            coverage.update(range(start, start + length))
        elif unicode_list != "NULL":
            coverage.update(start + value for value in arrays[unicode_list][:list_length])
        else:
            raise ValueError(f"unsupported cmap in {path}: {entry.group(0)}")
    return coverage


def ime_dictionary_codepoints(path: Path) -> set[int]:
    """Read every generated IME block and collect its UTF-8 output glyphs."""
    data = path.read_bytes()
    if len(data) < IME_HEADER.size:
        raise ValueError(f"truncated IME dictionary: {path}")
    magic, version, count, index_offset, data_offset, max_raw, file_size = \
        IME_HEADER.unpack_from(data)
    if (magic not in (b"LYZHIME1", b"LYJAIME1") or version != 1 or not count or
            index_offset != IME_HEADER.size or
            data_offset != index_offset + count * IME_INDEX.size or
            data_offset > len(data) or file_size != len(data) or not max_raw):
        raise ValueError(f"invalid IME dictionary header: {path}")

    codepoints: set[int] = set()
    previous_key: bytes | None = None
    for block_index in range(count):
        record = IME_INDEX.unpack_from(data, index_offset + block_index * IME_INDEX.size)
        key_length, _key_storage, offset, compressed_size, raw_size, checksum = record
        first_key = _key_storage[:key_length]
        if (not key_length or key_length > 48 or not compressed_size or not raw_size or
                raw_size > max_raw or offset < data_offset or
                offset + compressed_size > file_size or
                (previous_key is not None and first_key <= previous_key)):
            raise ValueError(f"invalid IME dictionary index in {path}, block {block_index}")
        previous_key = first_key
        raw = zlib.decompress(data[offset:offset + compressed_size])
        if len(raw) != raw_size or (binascii.crc32(raw) & 0xFFFFFFFF) != checksum:
            raise ValueError(f"IME dictionary block checksum mismatch in {path}, block {block_index}")

        cursor = 0
        while cursor < len(raw):
            if cursor + 3 > len(raw):
                raise ValueError(f"truncated IME record in {path}, block {block_index}")
            key_size, candidate_count = struct.unpack_from("<BH", raw, cursor)
            cursor += 3
            if not key_size or cursor + key_size > len(raw):
                raise ValueError(f"invalid IME record key in {path}, block {block_index}")
            key = raw[cursor:cursor + key_size].decode("utf-8")
            codepoints.update(ord(character) for character in key if ord(character) >= 0x80)
            cursor += key_size
            for _ in range(candidate_count):
                if cursor >= len(raw):
                    raise ValueError(f"truncated IME candidate in {path}, block {block_index}")
                candidate_size = raw[cursor]
                cursor += 1
                if not candidate_size or cursor + candidate_size > len(raw):
                    raise ValueError(f"invalid IME candidate in {path}, block {block_index}")
                candidate = raw[cursor:cursor + candidate_size].decode("utf-8")
                codepoints.update(ord(character) for character in candidate if ord(character) >= 0x80)
                cursor += candidate_size
    return codepoints


def manifest_text(locale_points: dict[str, set[int]], keyboard_points: set[int]) -> str:
    lines = [
        "# Generated by tools/i18n/check_font_manifest.py; do not edit.",
        "# It records translated catalog values, keyboard glyphs, and IME outputs.",
    ]
    for locale in LOCALES:
        points = " ".join(f"U+{point:04X}" for point in sorted(locale_points[locale]))
        lines.append(f"{locale}: {points}")
    points = " ".join(f"U+{point:04X}" for point in sorted(keyboard_points))
    lines.append(f"keyboard: {points}")
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--catalog", type=Path, default=CATALOG)
    parser.add_argument("--font", type=Path, action="append", dest="fonts")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--write", action="store_true",
                        help="write the manifest instead of only checking it")
    args = parser.parse_args()
    fonts = tuple(args.fonts or DEFAULT_FONTS)

    try:
        values = catalog_locale_values(args.catalog)
        coverage = set()
        for font in fonts:
            if not font.is_file():
                raise FileNotFoundError(font)
            coverage.update(font_codepoints(font))
        locale_points = {
            locale: {ord(character) for value in strings for character in value
                     if ord(character) >= 0x20}
            for locale, strings in values.items()
        }
        keyboard_points: set[int] = set()
        for source in KEYBOARD_SOURCES:
            if not source.is_file():
                raise FileNotFoundError(source)
            keyboard_points.update(
                ord(character) for character in source.read_text(encoding="utf-8")
                if ord(character) >= 0x80 and not character.isspace()
            )
        for dictionary in IME_DICTIONARIES:
            if not dictionary.is_file():
                raise FileNotFoundError(dictionary)
            keyboard_points.update(ime_dictionary_codepoints(dictionary))
        missing = {
            locale: sorted(points - coverage)
            for locale, points in locale_points.items()
            if points - coverage
        }
        if keyboard_points - coverage:
            missing["keyboard"] = sorted(keyboard_points - coverage)
        if missing:
            for locale, points in missing.items():
                formatted = " ".join(f"U+{point:04X}" for point in points)
                print(f"font: {locale} missing {formatted}", file=sys.stderr)
            return 1
        output = manifest_text(locale_points, keyboard_points)
        if args.write:
            args.manifest.write_text(output, encoding="utf-8", newline="\n")
        elif args.manifest.is_file() and args.manifest.read_text(encoding="utf-8") != output:
            print(f"font: manifest is stale: {args.manifest}", file=sys.stderr)
            return 1
    except (OSError, ValueError, KeyError) as error:
        print(f"font: {error}", file=sys.stderr)
        return 1

    print(f"font: {sum(len(points) for points in locale_points.values())} locale and "
          f"{len(keyboard_points)} keyboard code points checked")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
