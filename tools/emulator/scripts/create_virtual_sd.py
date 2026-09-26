#!/usr/bin/env python3
"""Create the emulator's FAT16 microSD image with short, synthetic WAV samples."""

from __future__ import annotations

import gzip
import math
import struct
import wave
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
OUTPUT = ROOT / "app" / "assets" / "lyra-demo-sd.img.gz"
SECTOR = 512
TOTAL_SECTORS = 524_288  # 256 MiB
SECTORS_PER_CLUSTER = 8
FAT_SECTORS = 256
ROOT_ENTRIES = 512
ROOT_SECTORS = ROOT_ENTRIES * 32 // SECTOR
DATA_START = 1 + 2 * FAT_SECTORS + ROOT_SECTORS


def make_wav(notes: tuple[float, ...], seconds: float = 3.2) -> bytes:
    sample_rate = 44_100
    frames = int(sample_rate * seconds)
    samples = bytearray()
    for index in range(frames):
        beat = int(index / sample_rate * 2) % len(notes)
        within = (index / sample_rate * 2) % 1.0
        envelope = min(1.0, within * 14.0, (1.0 - within) * 10.0)
        tone = math.sin(2 * math.pi * notes[beat] * index / sample_rate)
        value = int(max(-1.0, min(1.0, tone * envelope * 0.24)) * 32767)
        samples.extend(struct.pack("<hh", value, value))
    import io

    buffer = io.BytesIO()
    with wave.open(buffer, "wb") as wav:
        wav.setnchannels(2)
        wav.setsampwidth(2)
        wav.setframerate(sample_rate)
        wav.writeframes(samples)
    return buffer.getvalue()


def entry(name: bytes, attributes: int, first_cluster: int = 0, size: int = 0) -> bytes:
    data = bytearray(32)
    data[:11] = name.ljust(11, b" ")[:11]
    data[11] = attributes
    struct.pack_into("<HH", data, 22, 0, 0)  # time, date
    struct.pack_into("<H", data, 26, first_cluster)
    struct.pack_into("<I", data, 28, size)
    return bytes(data)


def main() -> None:
    songs = [
        (b"LYRA01  WAV", (261.63, 329.63, 392.00, 523.25, 392.00, 329.63)),
        (b"LYRA02  WAV", (293.66, 349.23, 440.00, 587.33, 440.00, 349.23)),
        (b"LYRA03  WAV", (220.00, 277.18, 329.63, 440.00, 329.63, 277.18)),
    ]
    files = [(name, make_wav(notes)) for name, notes in songs]

    fat = bytearray(FAT_SECTORS * SECTOR)
    struct.pack_into("<HH", fat, 0, 0xFFF8, 0xFFFF)
    root = bytearray(ROOT_SECTORS * SECTOR)
    root[:32] = entry(b"LYRA AUDIO", 0x08)
    image = bytearray(TOTAL_SECTORS * SECTOR)
    cluster = 2
    for index, (name, payload) in enumerate(files):
        clusters = (len(payload) + SECTORS_PER_CLUSTER * SECTOR - 1) // (SECTORS_PER_CLUSTER * SECTOR)
        first = cluster
        for _ in range(clusters):
            following = cluster + 1 if cluster + 1 < first + clusters else 0xFFFF
            struct.pack_into("<H", fat, cluster * 2, following)
            sector = DATA_START + (cluster - 2) * SECTORS_PER_CLUSTER
            start = sector * SECTOR
            image[start : start + min(len(payload), SECTORS_PER_CLUSTER * SECTOR)] = payload[: SECTORS_PER_CLUSTER * SECTOR]
            payload = payload[SECTORS_PER_CLUSTER * SECTOR :]
            cluster += 1
        root_offset = (index + 1) * 32
        root[root_offset : root_offset + 32] = entry(name, 0x20, first, len(files[index][1]))

    boot = bytearray(SECTOR)
    boot[:3] = b"\xeb\x3c\x90"
    boot[3:11] = b"MSWIN4.1"
    struct.pack_into("<HBHBHHBHHHII", boot, 11,
                     SECTOR, SECTORS_PER_CLUSTER, 1, 2, ROOT_ENTRIES,
                     TOTAL_SECTORS if TOTAL_SECTORS <= 0xFFFF else 0,
                     0xF8, FAT_SECTORS, 63, 255, 0,
                     TOTAL_SECTORS if TOTAL_SECTORS > 0xFFFF else 0)
    boot[36] = 0x80
    boot[38] = 0x29
    struct.pack_into("<I", boot, 39, 0x4C595241)
    boot[43:54] = b"LYRA AUDIO "
    boot[54:62] = b"FAT16   "
    boot[510:512] = b"\x55\xaa"
    image[:SECTOR] = boot
    image[SECTOR : SECTOR + len(fat)] = fat
    fat2 = SECTOR + len(fat)
    image[fat2 : fat2 + len(fat)] = fat
    root_start = (1 + 2 * FAT_SECTORS) * SECTOR
    image[root_start : root_start + len(root)] = root

    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_bytes(gzip.compress(image, compresslevel=9, mtime=0))
    print(f"Wrote {OUTPUT} ({len(image) / (1024 * 1024):.0f} MiB FAT16 image, {OUTPUT.stat().st_size:,} byte gzip)")


if __name__ == "__main__":
    main()
