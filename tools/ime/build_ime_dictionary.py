#!/usr/bin/env python3
"""Build compact, block-compressed offline Lyra IME dictionaries.

Inputs:
  * CC-CEDICT UTF-8 text wrapped in gzip.
  * Jieba's UTF-8 word-frequency dictionary.
  * EDRDG JMdict XML wrapped in gzip.

The generated files contain only readings, candidate surfaces, and ordering
scores. Candidate glyphs absent from Lyra's embedded fonts are omitted.
"""

from __future__ import annotations

import argparse
import gzip
import re
import struct
import unicodedata
import xml.etree.ElementTree as ET
import zlib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_FONTS = (
    ROOT / "main/lyra_unicode_16.c",
    ROOT / "main/lyra_cjk_16.c",
)
MAX_KEY_BYTES = 48
MAX_WORD_BYTES = 64
BLOCK_KEYS = 128
HEADER_FORMAT = "<8sIIIIII"
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)
INDEX_FORMAT = f"<B{MAX_KEY_BYTES}sIIII"
INDEX_SIZE = struct.calcsize(INDEX_FORMAT)
CEDICT_LINE = re.compile(r"^([^ ]+) ([^ ]+) \[([^\]]+)\] /")


def font_codepoints(path: Path) -> set[int]:
    text = path.read_text(encoding="utf-8")
    arrays = {
        name: [int(value, 16) for value in re.findall(r"0x([0-9A-Fa-f]+)", body)]
        for name, body in re.findall(
            r"static const uint16_t (unicode_list_\d+)\[\] = \{(.*?)\};",
            text,
            re.S,
        )
    }
    pattern = re.compile(
        r"\.range_start\s*=\s*(\d+),\s*\.range_length\s*=\s*(\d+),"
        r"\s*\.glyph_id_start\s*=\s*\d+,\s*"
        r"\.unicode_list\s*=\s*(NULL|unicode_list_\d+),\s*"
        r"\.glyph_id_ofs_list\s*=\s*(NULL|glyph_id_ofs_list_\d+),\s*"
        r"\.list_length\s*=\s*(\d+),\s*\.type\s*=\s*(\w+)"
    )
    coverage: set[int] = set()
    for entry in pattern.finditer(text):
        start, length = int(entry.group(1)), int(entry.group(2))
        unicode_list = entry.group(3)
        list_length = int(entry.group(5))
        cmap_type = entry.group(6)
        if cmap_type.startswith("LV_FONT_FMT_TXT_CMAP_FORMAT0"):
            coverage.update(range(start, start + length))
        elif unicode_list != "NULL":
            coverage.update(start + value for value in arrays[unicode_list][:list_length])
    if not coverage:
        raise ValueError(f"no LVGL font glyph coverage found in {path}")
    return coverage


def has_glyphs(text: str, coverage: set[int]) -> bool:
    return all(ord(character) in coverage for character in text)


def has_han(text: str) -> bool:
    return any(
        0x3400 <= ord(character) <= 0x9FFF or 0xF900 <= ord(character) <= 0xFAFF
        for character in text
    )


def pinyin_key(reading: str) -> str:
    reading = reading.lower().replace("ü", "v").replace("u:", "v")
    return "".join(
        character
        for character in unicodedata.normalize("NFD", reading)
        if not unicodedata.combining(character) and character.isalpha()
    )


def hiragana_key(reading: str) -> str:
    result = []
    for character in reading:
        codepoint = ord(character)
        result.append(chr(codepoint - 0x60) if 0x30A1 <= codepoint <= 0x30F6 else character)
    return "".join(result)


def load_jieba_frequencies(path: Path) -> dict[str, int]:
    frequencies: dict[str, int] = {}
    with path.open("r", encoding="utf-8") as source:
        for line in source:
            fields = line.split()
            if len(fields) >= 2:
                try:
                    frequencies[fields[0]] = int(fields[1])
                except ValueError:
                    continue
    return frequencies


def load_chinese_dictionary(
    cedict_path: Path, frequency_path: Path, coverage: set[int]
) -> dict[str, dict[str, int]]:
    frequencies = load_jieba_frequencies(frequency_path)
    candidates: dict[str, dict[str, int]] = {}
    with gzip.open(cedict_path, "rt", encoding="utf-8") as source:
        for line in source:
            match = CEDICT_LINE.match(line)
            if not match:
                continue
            traditional, simplified, reading = match.groups()
            key = pinyin_key(reading)
            key_bytes = key.encode("utf-8")
            if not key or len(key_bytes) > MAX_KEY_BYTES:
                continue
            frequency = max(1, frequencies.get(simplified, 1))
            for surface in dict.fromkeys((simplified, traditional)):
                encoded = surface.encode("utf-8")
                if (
                    len(encoded) > MAX_WORD_BYTES
                    or not has_han(surface)
                    or not has_glyphs(surface, coverage)
                ):
                    continue
                per_key = candidates.setdefault(key, {})
                per_key[surface] = max(per_key.get(surface, 0), frequency)
    return candidates


def jmdict_priority(entry: ET.Element) -> int:
    score = 0
    for item in entry.iter():
        if item.tag not in ("ke_pri", "re_pri") or not item.text:
            continue
        priority = item.text
        if priority in ("ichi1", "news1", "spec1", "gai1"):
            score = max(score, 200_000)
        elif priority.startswith("nf") and priority[2:].isdigit():
            score = max(score, 150_000 - int(priority[2:]) * 1_000)
        else:
            score = max(score, 100_000)
    return score


def is_katakana(text: str) -> bool:
    return any(0x30A1 <= ord(character) <= 0x30F6 for character in text)


def load_japanese_dictionary(path: Path, coverage: set[int]) -> dict[str, dict[str, int]]:
    candidates: dict[str, dict[str, int]] = {}
    with gzip.open(path, "rb") as source:
        for _, entry in ET.iterparse(source, events=("end",)):
            if entry.tag != "entry":
                continue
            score = jmdict_priority(entry)
            word_forms: list[tuple[str, set[str]]] = []
            for word in entry.findall("./k_ele"):
                surface = word.findtext("keb")
                if surface:
                    info = {item.text for item in word.findall("./ke_inf") if item.text}
                    word_forms.append((surface, info))

            for reading in entry.findall("./r_ele"):
                surface_reading = reading.findtext("reb")
                if not surface_reading:
                    continue
                key = hiragana_key(surface_reading)
                encoded_key = key.encode("utf-8")
                if not key or len(encoded_key) > MAX_KEY_BYTES:
                    continue
                restrictions = {item.text for item in reading.findall("./re_restr") if item.text}
                allowed_words = [
                    surface
                    for surface, info in word_forms
                    if (not restrictions or surface in restrictions)
                    and not ("uk" in info and len(word_forms) > 1)
                ]
                if not allowed_words:
                    # Keep katakana readings as conversion candidates. Plain
                    # hiragana remains available as the user's raw preedit.
                    allowed_words = [surface_reading] if is_katakana(surface_reading) else []

                for surface in allowed_words:
                    encoded = surface.encode("utf-8")
                    if (
                        len(encoded) > MAX_WORD_BYTES
                        or not has_glyphs(surface, coverage)
                        or not (has_han(surface) or is_katakana(surface))
                    ):
                        continue
                    per_key = candidates.setdefault(key, {})
                    per_key[surface] = max(per_key.get(surface, 0), score)
            entry.clear()
    return candidates


def encode_records(candidates: dict[str, dict[str, int]]) -> list[tuple[bytes, bytes]]:
    records: list[tuple[bytes, bytes]] = []
    for key in sorted(candidates, key=lambda value: value.encode("utf-8")):
        key_bytes = key.encode("utf-8")
        values = candidates[key]
        ordered = sorted(values, key=lambda word: (-values[word], -len(word), word))
        record = bytearray((len(key_bytes),))
        record.extend(struct.pack("<H", len(ordered)))
        record.extend(key_bytes)
        for word in ordered:
            encoded = word.encode("utf-8")
            record.append(len(encoded))
            record.extend(encoded)
        records.append((key_bytes, bytes(record)))
    return records


def write_dictionary(path: Path, magic: bytes, candidates: dict[str, dict[str, int]]) -> tuple[int, int]:
    records = encode_records(candidates)
    chunks: list[tuple[bytes, bytes, int, int, int]] = []
    max_raw_size = 0
    for start in range(0, len(records), BLOCK_KEYS):
        batch = records[start : start + BLOCK_KEYS]
        raw = b"".join(record for _, record in batch)
        compressed = zlib.compress(raw, level=9)
        chunks.append((batch[0][0], compressed, len(raw), zlib.crc32(raw) & 0xFFFFFFFF, 0))
        max_raw_size = max(max_raw_size, len(raw))

    index_offset = HEADER_SIZE
    data_offset = HEADER_SIZE + len(chunks) * INDEX_SIZE
    cursor = data_offset
    index_bytes = bytearray()
    payloads = bytearray()
    for first_key, compressed, raw_size, checksum, _ in chunks:
        index_bytes.extend(struct.pack(
            INDEX_FORMAT,
            len(first_key), first_key.ljust(MAX_KEY_BYTES, b"\0"),
            cursor, len(compressed), raw_size, checksum,
        ))
        payloads.extend(compressed)
        cursor += len(compressed)
    header = struct.pack(
        HEADER_FORMAT,
        magic,
        1,
        len(chunks),
        index_offset,
        data_offset,
        max_raw_size,
        cursor,
    )
    path.write_bytes(header + index_bytes + payloads)
    return len(records), path.stat().st_size


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cedict", type=Path, required=True)
    parser.add_argument("--jieba", type=Path, required=True)
    parser.add_argument("--jmdict", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, default=Path(__file__).parent)
    parser.add_argument("--font", type=Path, action="append", dest="fonts")
    args = parser.parse_args()
    fonts = tuple(args.fonts or DEFAULT_FONTS)
    try:
        coverage: set[int] = set()
        for font in fonts:
            coverage.update(font_codepoints(font))
        chinese = load_chinese_dictionary(args.cedict, args.jieba, coverage)
        japanese = load_japanese_dictionary(args.jmdict, coverage)
        args.output_dir.mkdir(parents=True, exist_ok=True)
        chinese_count, chinese_size = write_dictionary(
            args.output_dir / "lyra_ime_zh.bin", b"LYZHIME1", chinese
        )
        japanese_count, japanese_size = write_dictionary(
            args.output_dir / "lyra_ime_ja.bin", b"LYJAIME1", japanese
        )
    except (OSError, ValueError, ET.ParseError) as error:
        parser.error(str(error))
    print(f"Chinese: {chinese_count} readings, {chinese_size} bytes")
    print(f"Japanese: {japanese_count} readings, {japanese_size} bytes")
    print(f"Total: {chinese_size + japanese_size} bytes; font-filtered to {len(coverage)} glyphs")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
