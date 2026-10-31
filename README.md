# Lyra OS

**An open-source reference firmware platform by Emotivate for affordable touchscreen digital audio players.** Lyra OS provides an offline music experience on accessible embedded hardware, with the goal of making capable player software easier for developers and device manufacturers to adopt.

The platform is specialised application firmware built with ESP-IDF, FreeRTOS, C++ and LVGL.

## Current status and hardware

The current development and documented test reference is the **JC3248W535EN ESP32-S3 development kit**, with its 320 × 480 AXS15231B QSPI touchscreen, 16 MB flash, 8 MB PSRAM, 1-bit SDMMC microSD slot, and an external PCM5102A I2S DAC. The on-board speaker path can optionally mirror audio where fitted and enabled. This accessible kit helps developers reproduce the current implementation while Lyra OS supports adaptation to other device designs through board-specific engineering.

The checked-in implementation uses this board's pins, panel and touch driver, display geometry, storage path, and audio wiring. ESP32-S3 is the current implementation family; adapting Lyra OS to another ESP32-S3 board or microcontroller starts with an engineering port. See [hardware support](docs/HARDWARE_SUPPORT.md) and the [JC3248W535EN wiring reference](docs/JC3248W535EN_HARDWARE.md). The hardware reference and [audio transition validation notes](docs/AUDIO_TRANSITION_VALIDATION.md) describe the physical checks for this configuration.

## What the platform provides

### Offline library and media

- Recursive microSD catalog scan for up to 10,000 tracks, with metadata browsing by song, album, artist, genre, and year, configurable sorting, and sort-location jumps. Search covers songs, albums, artists, and playlists.
- Direct folder browsing and playback alongside catalog scanning, folder playback queues, and direct opening of portable `.m3u` / `.m3u8` playlists. Create, edit, and delete playlists under `/sdcard/Playlists`; Favorites and Smart Playlists include Recently Added, Recently Played, Most Played, and Never Played.
- Editable playback queue with reorder, remove, and clear actions. Queue order, current track, and elapsed position are saved on normal reboot/power-off; restored playback stays paused until resumed.
- Embedded album art with `cover.jpg`, `folder.jpg`, or `cover.png` fallback from the track folder. Artwork stays in memory; optional SD-backed decoding and caching support oversized JPEGs, and display artwork can be set to 240 or 320 pixels.
- Full-screen PNG/BMP/JPEG viewing with zoom and pan, plus a paged TXT/LRC reader with font-size and light/dark controls and saved bookmarks.
- Local playback of MP3, FLAC, AAC (`.aac`), M4A (`.m4a`, including AAC/ALAC containers), Ogg Vorbis (`.ogg`), Opus in Ogg (`.ogg`, `.opus`), WAV PCM (`.wav`), and uncompressed AIFF/AIF/AIFC PCM. Decoder behavior and format details are documented in the source and audio validation guide.

### Playback

- Play/pause, previous/next, seeking, volume, shuffle, repeat-all/repeat-song, and optional Quick Seek controls that jump backward or forward by 30 seconds.
- Per-track ReplayGain and a five-band equalizer at 60 Hz, 250 Hz, 1 kHz, 4 kHz, and 16 kHz. Presets include Flat, Full Bass, Full Treble, Bass & Treble, Rock, Pop, Jazz, and Classic, alongside custom gains.
- Gapless playback and 1, 2, 4, or 6 second crossfades, with decoded PCM joined at track boundaries. MP3/AAC priming and padding behavior is described in [audio transition validation](docs/AUDIO_TRANSITION_VALIDATION.md), alongside physical-board checks.
- Tap album art in Now Playing to show embedded lyrics or a same-stem `.lrc` / `.txt` file; timestamped lyrics highlight the current line.
- Sleep timer options stop playback after 15, 30, 45, or 60 minutes.
- File-based local playback through the external PCM5102A I2S path; optional mirrored on-board speaker output is enabled by default where fitted and can be configured in Settings → Sound.

### Interface and international libraries

- Touch-focused LVGL interface and five themes: Standard, Night City, Cute Pink, Zeno, and Aura. Standard includes light/dark mode and accent colours. The current layouts target 320 × 480 portrait geometry, with an optional bottom navigation/playback control bar.
- UI translations for English, French, German, Spanish, Italian, Japanese, Korean, Russian, Simplified Chinese, and Traditional Chinese.
- Manual time/date setting, date-format and 12/24-hour preferences, a manual daylight-saving correction, and persistent display/audio/library preferences. System settings include library/database and artwork-cache management, safe reboot, power-off to deep sleep, and factory reset.
- Unicode metadata display, library browsing/search, and selectable keyboard input are separate capabilities. Input layouts are independent of UI language and include English, Russian, Korean Hangul, Chinese Pinyin, and Japanese Kana flick input with modifiers. Chinese Pinyin entry supports offline candidate conversion when its optional dictionary is installed under `.lyra/ime/` on microSD. Japanese Kana input supports flick gestures and modifiers, with Kana-to-Kanji conversion available when its optional dictionary is installed there. See the [IME dictionary guide](tools/ime/README.md), [localisation QA matrix](docs/LOCALISATION_QA_MATRIX.md), and [i18n contributor guide](tools/i18n/README.md).

## Screenshots and demonstrations

Capture the current interface by launching the [browser emulator](tools/emulator/README.md) with a merged firmware image and using its **Capture PNG** control. The emulator renders actual firmware frames and accepts touch input; use the development kit to evaluate audible I2S playback and physical board behavior.

## Build and try the reference configuration

Install and activate **ESP-IDF 6.0.2** using Espressif-IDE or Espressif's command-line tools. The first configure/build resolves managed components declared in `main/idf_component.yml`.

From an ESP-IDF 6.0.2 shell, build for the current reference board:

```powershell
idf.py set-target esp32s3
idf.py build
```

The board pin map and external PCM5102A wiring are in [JC3248W535EN hardware notes](docs/JC3248W535EN_HARDWARE.md). To flash and monitor, select the board's serial port:

```powershell
idf.py -p COMx flash monitor
```

For a single image accepted by compatible flashing tools:

```powershell
idf.py merge-bin -o lyra_firmware_merged.bin
```

The [browser emulator](tools/emulator/README.md) runs the actual ESP32-S3 firmware image, emulates its display, touch, and microSD, and can save PNG captures. Evaluate audible I2S playback on the development kit.

## Adopt or port Lyra OS

Lyra OS is intended as a software base for hobbyists, developers, OEMs, and manufacturers. Adapting a new target begins with hardware-specific integration and testing, as described in the porting guide.

- [Platform overview](docs/PLATFORM_OVERVIEW.md)
- [Hardware support status](docs/HARDWARE_SUPPORT.md)
- [Porting guide](docs/PORTING_GUIDE.md)
- [Contributing](CONTRIBUTING.md)

## Documentation

- [JC3248W535EN implementation reference](docs/EMOTIVATE_LYRA_OS_SPEC_JC3248W535EN.md)
- [Lyra OS platform baseline](docs/EMOTIVATE_LYRA_OS_SPEC_BASELINE.md)
- [Embedded font notes](docs/EMBEDDED_FONT.md)
- [Audio transition validation](docs/AUDIO_TRANSITION_VALIDATION.md)
- [Localisation QA matrix](docs/LOCALISATION_QA_MATRIX.md)
- [IME dictionary generation and attribution](tools/ime/README.md)
- [Translation catalog and checks](tools/i18n/README.md)
- [Third-party notices](THIRD_PARTY_NOTICES.md)

## Repository layout

```text
main/                   Application, GUI, audio, media, and storage services
components/lyra_board/  Current display, touch, and board integration
board/                  JC3248W535EN pin contract and vendor references
docs/                   Platform, hardware, validation, and implementation docs
graphics/               Firmware image and theme assets
partitions.csv          Current 16 MB flash partition layout
sdkconfig.defaults      Reference ESP-IDF configuration
dependencies.lock       Locked managed-component versions
```

## Contribution and licensing

See [CONTRIBUTING.md](CONTRIBUTING.md) for reference build instructions, available source checks, and the hardware/emulator/build evidence to include with a change. Lyra OS application source is licensed under [Apache License 2.0](LICENSE). Third-party code, generated font data, dictionaries, and other assets have their own license terms; review [Third-party notices](THIRD_PARTY_NOTICES.md) before redistributing a firmware image.

## Developer note

This project was developed with assistance from AI language models. Some source code was generated or refined using AI, with generated code reviewed, modified, and integrated by the project author where appropriate.
