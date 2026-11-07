# Lyra OS

Lyra OS is open-source firmware for small touchscreen music players. It's
built on ESP-IDF, FreeRTOS, C++ and LVGL, and it's meant to be a solid
starting point for anyone making an affordable digital audio player, from a
hobby build up to a product.

The idea is simple: put a music library on a microSD card, and Lyra gives you
a proper player for it. No account, no app, no internet needed. Wi-Fi is there
if you want internet radio or the correct time, but everything else works
offline.

## Hardware

Development happens on the **JC3248W535EN**, an inexpensive ESP32-S3 board
with a 320 × 480 touchscreen (AXS15231B over QSPI), 16 MB of flash, 8 MB of
PSRAM and a microSD slot. Audio goes out through an external PCM5102A I2S DAC,
and can also be mirrored to the board's own speaker if you want.

Everything in this repository is written for that board: the pins, display and
touch driver, screen layouts and audio wiring. Running Lyra on different
hardware means doing a port. The [hardware support notes](docs/HARDWARE_SUPPORT.md)
and the [JC3248W535EN wiring reference](docs/JC3248W535EN_HARDWARE.md) are the
places to start, and the [porting guide](docs/PORTING_GUIDE.md) walks through
the rest.

## What it does

### Your music library

Lyra scans the card for up to 10,000 tracks and lets you browse by song,
album, artist, genre or year, with sorting options and a jump-to-letter strip
for long lists. Lists scroll smoothly however big your library is, and you can
turn on small album covers beside songs and albums (Settings → Display). The
covers are decoded once and cached on the card, so they stay quick after the
first visit.

If you'd rather not scan at all, you can also browse folders and play straight
from them, or open `.m3u` / `.m3u8` playlists wherever they are on the card.

Playlists live in `/sdcard/Playlists` and you can create, edit and delete them
on the device. There's a Favorites list, plus smart playlists for Recently
Added, Recently Played, Most Played and Never Played.

Search covers songs, albums, artists and playlists. Type your query and press
Search; the keyboard gets out of the way so you can see the results.

### Playback

Supported formats are MP3, FLAC, AAC, M4A (AAC and ALAC), Ogg Vorbis, Opus,
WAV and AIFF.

The usual controls are all there: play/pause, skip, seek, volume, shuffle,
repeat one or all. There's also:

- gapless playback, and crossfades of 1, 2, 4 or 6 seconds
- ReplayGain
- a five-band equalizer with presets and custom settings
- a playback speed control from 0.25× to 2×
- optional Quick Seek buttons that jump 30 seconds
- a sleep timer

Tap the album art in Now Playing to see lyrics, either embedded or from a
matching `.lrc` / `.txt` file; synced lyrics follow along line by line.

The queue can be reordered, edited and saved as a playlist. It's remembered
across restarts along with where you were in the track, and playback waits
paused until you hit play again.

Album art comes from the file itself, or from `cover.jpg`, `folder.jpg` or
`cover.png` in the album folder.

### Internet radio

With Wi-Fi on, you can save and play HTTP(S) radio streams. Station lists can
be imported from `.m3u`, `.pls` or `.asx` files, and the player shows the
stream's current title when the station sends one.

### Files and apps

There's a file manager for the card, so you can rename, copy, move and delete
files and create folders without pulling the card out. Better still, **USB
file transfer** (Settings → System) turns Lyra into a USB drive for your
computer, so you can copy music over the cable. One catch: the board has a
single USB port, so serial logs and flashing pause while transfer mode is on.
Tapping Finish restarts Lyra and brings them back.

The Apps screen has a calculator, calendar, stopwatch and timer, a photo
viewer, a text and ebook reader, and two small games, Dot Link and Dice.

The photo viewer opens PNG, BMP and JPEG files full screen with zoom and pan.
The reader has three page colours (light, sepia and dark), three text sizes,
a progress bar and a bookmark per file.

### Look and feel

There are five themes: Standard, Night City, Cute Pink, Zeno and Aura.
Standard also has a dark mode and accent colours. You can pick a "listening
hub" home screen or a classic list menu, and turn on an on-screen control bar
for boards without buttons.

The interface is translated into English, French, German, Spanish, Italian,
Japanese, Korean, Russian, and Simplified and Traditional Chinese. Song
metadata in any of those scripts displays properly.

Typing works in English, Russian, Korean, Chinese (Pinyin) and Japanese
(kana flick input), with an optional T9 phone-style keypad. Chinese and
Japanese candidate conversion needs dictionaries copied to `.lyra/ime/` on the
card; see the [IME guide](tools/ime/README.md). Text fields show a cursor, and
you can slide along the space bar to move it.

### System

- Wi-Fi setup, with network tests in the service menu.
- The clock sets itself from the internet when Wi-Fi connects, using your
  chosen time zone and daylight-saving setting. You can still set it by hand.
  The board has no battery-backed clock, so without Wi-Fi it starts from a
  default time after power loss.
- Firmware updates from an `update.bin` file on the card.
- If Lyra ever crashes, it saves a crash report (and a core dump, when one
  was captured) to `.lyra/crash/` on the card at the next boot, and tells you.
  This is mostly useful for sending to whoever is debugging it.
- Safe reboot, power off (deep sleep), factory reset, and tools for clearing
  the library database and artwork cache.

## Building it

You'll need **ESP-IDF 6.0.2**, through Espressif-IDE or the command-line
tools. The first build downloads the managed components listed in
`main/idf_component.yml`.

From an ESP-IDF shell:

```powershell
idf.py set-target esp32s3
idf.py build
```

Then flash it and watch the log, using your board's serial port:

```powershell
idf.py -p COMx flash monitor
```

If you want a single image for other flashing tools:

```powershell
idf.py merge-bin -o lyra_firmware_merged.bin
```

A couple of things worth knowing:

- `sdkconfig.defaults` holds the reference configuration. If you already have
  a local `sdkconfig`, its existing values win, so newer defaults (crash dumps
  and USB transfer, for example) may need turning on in `idf.py menuconfig`,
  or you can delete `sdkconfig` and let it regenerate.
- The flash layout in `partitions.csv` includes a small `coredump` partition
  for crash reports. A full `idf.py flash` writes it; devices updated with
  `update.bin` keep their old layout and just get text-only crash reports.

The wiring for the board and the external DAC is in the
[JC3248W535EN hardware notes](docs/JC3248W535EN_HARDWARE.md).

### Trying it without the hardware

There's a [browser emulator](tools/emulator/README.md) that runs the real
firmware image, with a working screen, touch and virtual microSD card. It's
great for trying the interface and taking screenshots (there's a Capture PNG
button), but you'll need the actual board to hear anything.

## Using Lyra in your own device

Lyra is meant to be adopted, whether you're a hobbyist with a different board
or a manufacturer building a player. Expect some hardware-specific work for
each new target; the guides below explain what's involved.

- [Platform overview](docs/PLATFORM_OVERVIEW.md)
- [Hardware support](docs/HARDWARE_SUPPORT.md)
- [Porting guide](docs/PORTING_GUIDE.md)
- [Contributing](CONTRIBUTING.md)

## More documentation

- [JC3248W535EN implementation reference](docs/EMOTIVATE_LYRA_OS_SPEC_JC3248W535EN.md):
  the detailed behaviour of this build
- [Platform baseline](docs/EMOTIVATE_LYRA_OS_SPEC_BASELINE.md)
- [Audio transition validation](docs/AUDIO_TRANSITION_VALIDATION.md): gapless
  and crossfade details, and how to check them on hardware
- [Embedded font notes](docs/EMBEDDED_FONT.md)
- [Localisation QA matrix](docs/LOCALISATION_QA_MATRIX.md)
- [Translation catalog and checks](tools/i18n/README.md)
- [IME dictionaries](tools/ime/README.md)
- [Third-party notices](THIRD_PARTY_NOTICES.md)

## Where things are

```text
main/                   The application: GUI, audio, media, storage and services
components/lyra_board/  Display, touch and board setup for the JC3248W535EN
board/                  Pin definitions and vendor references
docs/                   Design, hardware and validation documents
graphics/               Boot images and theme artwork
tools/                  Emulator, translation checks, font and IME tools
partitions.csv          Flash layout (16 MB)
sdkconfig.defaults      Reference ESP-IDF configuration
dependencies.lock       Pinned component versions
```

## Contributing and licence

Contributions are welcome. [CONTRIBUTING.md](CONTRIBUTING.md) explains how to
build, which checks to run, and what to include with a change (hardware,
emulator or build evidence).

The Lyra OS source is under the [Apache License 2.0](LICENSE). Some
third-party code, generated font data, dictionaries and other assets have
their own terms, so please read the [third-party notices](THIRD_PARTY_NOTICES.md)
before you redistribute a firmware image.

## Developer note

This project was developed with assistance from AI language models. Some source code was generated or refined using AI, with generated code reviewed, modified, and integrated by the project author where appropriate.
