# Emotivate Lyra firmware

Native ESP-IDF firmware for the **Emotivate Lyra** portable music player,
targeting the JC3248W535EN ESP32-S3 development kit.

The firmware uses C++, FreeRTOS, and LVGL to provide a 320 × 480 portrait
touch interface, MicroSD music library, and I2S audio playback.

## Development Hardware

- **Target:** ESP32-S3 on the JC3248W535EN board
- **Display:** 320 × 480 RGB565 AXS15231B QSPI display with I2C touch
- **Storage:** 16 MB flash, 8 MB PSRAM, and MicroSD via 1-bit SDMMC
- **Framework:** ESP-IDF 6.0.2, FreeRTOS, and LVGL 9.4.0
- **Audio:** File-based playback through an external PCM5102A I2S DAC

## Firmware features

- Touch-friendly LVGL interface with a 320 × 480 portrait layout, including Music
  Library, folder browser, search, Now Playing, queue, playlists, equalizer,
  and settings screens.
- Recursive MicroSD music-library scan (up to 10,000 tracks), with metadata,
  sorting, and embedded album-art display and caching.
- Portable `.m3u` / `.m3u8` playlists, a generated Favorites playlist, and a
  persistent playback queue.
- Playback controls for play/pause, previous/next, seeking, volume, and
  per-track ReplayGain adjustment.
- Five-band equalizer with custom settings and built-in presets; audio and
  library preferences persist across restarts.

## Supported audio formats

The MicroSD library and native playback support:

- **MP3** and **FLAC**
- **AAC** (`.aac`) and **M4A** (`.m4a`, including AAC/ALAC containers)
- **Ogg Vorbis** (`.ogg`) and **Opus in Ogg** (`.ogg`, `.opus`)
- **WAV PCM** (`.wav`)
- Uncompressed **AIFF**, **AIF**, and **AIFC PCM** (`.aiff`, `.aif`, `.aifc`)

## Documentation

Specifications and implementations are documented here:

- [Board-specific Lyra specification](docs/EMOTIVATE_LYRA_OS_SPEC_JC3248W535EN.md)
- [Product baseline specification](docs/EMOTIVATE_LYRA_OS_SPEC_BASELINE.md)
- [JC3248W535EN hardware notes](docs/JC3248W535EN_HARDWARE.md)
- [Embedded font documentation](docs/EMBEDDED_FONT.md)

## Project layout

```text
lyra-firmware/
├── main/                   Application, UI, audio, media, and storage code
├── components/lyra_board/  Display, touch, and board-support component
├── board/                  Board pin contract and retained vendor references
├── docs/                   Product, hardware, and implementation documentation
├── graphics/               Firmware image assets
├── partitions.csv          16 MB flash partition layout
├── sdkconfig.defaults      Default ESP-IDF configuration
└── dependencies.lock       Locked managed-component versions
```

## Prerequisites

Install and activate the ESP-IDF 6.0.2 environment through Espressif-IDE or
the ESP-IDF command-line tools. The first configure/build downloads the
managed dependencies declared in `main/idf_component.yml`.

## Build

From an ESP-IDF 6.0.2 shell:

```powershell
idf.py set-target esp32s3
idf.py build
```

## Flashing

Use Espressif-IDE's project build and flash actions, or run the normal IDF
command after selecting the board's serial port:

```powershell
idf.py -p COMx flash monitor
```

Use `idf.py flash` for a complete image.

To create a single image for a compatible flashing tool:

```powershell
idf.py merge-bin -o lyra_firmware_merged.bin
```

Flash the resulting merged file at offset `0x0`.

## Development note

This project was developed with assistance from AI language models. Some source code was generated or refined using AI, with generated code reviewed, modified, and integrated by the project author where appropriate.

## License

See [LICENSE](LICENSE) for the lyra-firmware source. Third-party and retained
reference materials are covered by their respective notices.
