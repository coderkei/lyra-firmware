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

### Library and files

- Recursive MicroSD music-library scan for up to 10,000 tracks, with metadata,
  browsing by song, album, artist, genre, and year, configurable sorting,
  and sort-location jumps.
- Search across songs, albums, artists, and playlists.
- Direct folder browsing and playback without a library scan, folder playback
  queues, and opening portable `.m3u` / `.m3u8` files directly from folders.
- Full-screen PNG/BMP/JPEG viewing with zoom and pan, plus a paged TXT/LRC
  reader with font-size and light/dark controls and saved bookmarks.
- On-demand album art from embedded images, falling back to `cover.jpg`,
  `folder.jpg`, or `cover.png` in the track's folder. Covers are kept in memory;
  optional SD-backed decoding and caching support oversized JPEG artwork.
  Display artwork can be configured at 240 or 320 pixels.
- Create, edit, and delete portable `.m3u` / `.m3u8` playlists under
  `/sdcard/Playlists`, with a generated Favorites playlist and Smart Playlists
  for Recently Added, Recently Played, Most Played, and Never Played.
- Editable playback queue with reordering, removal, and clearing. Queue order,
  current track, and elapsed position are saved on normal reboot/power-off;
  restored playback is paused until resumed.

### Playback and sound

- Play/pause, previous/next, seeking, volume, shuffle, and repeat-all/repeat-song.
  Optional Quick Seek controls jump backward or forward by 30 seconds.
- Gapless playback and overlapping crossfades of 1, 2, 4, or 6 seconds, including
  manual next/previous transitions. Encoded MP3/AAC priming or padding may still
  produce silence; see [audio transition validation](docs/AUDIO_TRANSITION_VALIDATION.md)
  for codec limitations and on-device checks.
- Per-track ReplayGain adjustment and a five-band equalizer at 60 Hz, 250 Hz,
  1 kHz, 4 kHz, and 16 kHz. Presets include Flat, Full Bass, Full Treble,
  Bass & Treble, Rock, Pop, Jazz, and Classic, alongside custom gains.
- Now Playing lyrics: tap the album art to show embedded lyrics or a same-stem
  `.lrc` / `.txt` file. Timestamped lyrics highlight the current line.
- Sleep timer stops playback after 15, 30, 45, or 60 minutes.
- External PCM5102A I2S output with optional mirrored on-board speaker output,
  enabled by default and configurable in Settings → Sound.

### Interface and settings

- Touch-friendly LVGL screens for the 320 × 480 portrait display, with an
  optional bottom navigation/playback control bar.
- **Standard**, **Neon Sky**, **Cute Pink**, **Zeno**, and **Aura** themes.
  Standard offers dark/light mode and accent colours.
- First-start language selection and a persistent language setting for English,
  French, German, Spanish, Italian, Japanese, Korean, Russian, Simplified
  Chinese, and Traditional Chinese.
- Manual time/date setting, date-format and 12/24-hour preferences, and a manual
  daylight-saving correction.
- Persistent display, audio, and library preferences; library/database and
  artwork-cache management; safe reboot, power-off to deep sleep, and factory
  reset.

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
- [Audio transition limitations and device validation](docs/AUDIO_TRANSITION_VALIDATION.md)
- [Localisation QA matrix](docs/LOCALISATION_QA_MATRIX.md)

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
