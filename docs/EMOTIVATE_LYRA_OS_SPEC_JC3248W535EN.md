# Emotivate Lyra firmware reference — JC3248W535EN

This is the primary technical reference for the native Lyra firmware. It
describes the checked-in ESP-IDF project for the JC3248W535EN development
kit. Product-level goals are in the
[firmware product baseline](EMOTIVATE_LYRA_OS_SPEC_BASELINE.md); documented
board information in
[JC3248W535EN_HARDWARE.md](JC3248W535EN_HARDWARE.md).

## Device information

| Item | Value |
|---|---|
| Target | ESP32-S3 |
| Board | JC3248W535EN |
| Display profile | `LYRA-320`, 320 × 480 portrait RGB565 |
| Framework | ESP-IDF 6.0.2, FreeRTOS, C++, LVGL 9.4.0 |
| Default resources | 16 MB flash and 8 MB octal PSRAM |
| Managed components | LVGL, `esp_audio_codec`, libjpeg-turbo, and libpng |
| Flash layout | NVS, OTA data, two 4 MB application slots, and a 7.875 MB data partition named `littlefs` (SPIFFS subtype) |

`sdkconfig.defaults`, `partitions.csv`, `main/idf_component.yml`, and
`components/lyra_board/idf_component.yml` define the build configuration.
Generated `sdkconfig`, `build`, and `managed_components` directories are
local ESP-IDF outputs.

## Board pins

The complete pin map is maintained once in the
[hardware reference](JC3248W535EN_HARDWARE.md) and in the authoritative
[`lyra_board_pins.h`](../board/JC3248W535EN/include/lyra_board_pins.h) header.
The firmware uses polled I2C touch input and 1-bit SDMMC; GPIO3 and GPIO10 are
hardware references.

## Firmware architecture

| Area | Source | Responsibility |
|---|---|---|
| Startup | `main/app_main.cpp` | Brings up the display, boot screen, media service, audio service, GUI, and LVGL worker |
| Board support | `components/lyra_board/` | AXS15231B QSPI display, backlight, LVGL integration, and touch polling |
| User interface | `main/lyra_gui.cpp` | LVGL screens, touch routing, settings, and navigation |
| Media service | `main/lyra_media.cpp` | MicroSD mount, catalog, metadata, playlists, search, sorting, and artwork |
| Audio service | `main/lyra_audio.cpp` | Decode, PCM conversion, equalizer, volume, seeking, and I2S speaker output |

The display is initialised with its backlight off. Lyra renders the boot
screen before enabling the panel, then starts its regular LVGL worker after
the GUI is ready. A missing MicroSD card or unavailable audio service is
reported to the UI instead of preventing the system shell from starting.

## Media and persistence

- The MicroSD card mounts at `/sdcard`. A user starts the library scan from
  `Settings > System > Scan music library`.
- The recursive catalog accepts MP3, WAV, FLAC, AAC, M4A, OGG, Opus, AIFF,
  AIF, and AIFC files. The maximum catalog size is 10,000 tracks.
- The catalog is stored at `/sdcard/.lyra/catalog-v10.bin` and is published
  with a temporary file and backup so a failed scan retains the prior catalog.
- User playlists are portable `.m3u` or `.m3u8` files under
  `/sdcard/Playlists`. The `Favorites` playlist is created as needed.
- The active queue is stored separately at `/sdcard/.lyra/queue-v1.m3u8`.
- Settings, volume, equalizer preferences, sort choices, and artwork-cache
  choices use the `lyra` NVS namespace.
- Embedded album art is loaded on demand. Lyra keeps the display image in
  memory and can retain selected decoded artwork in `/sdcard/.lyra/covers`.

## Playback and UI

Audio is read from MicroSD and routed through the onboard I2S speaker path.
ESP-IDF decoders handle MP3, FLAC, AAC, M4A/ALAC, WAV, OGG, and Opus-in-Ogg;
Lyra also handles uncompressed AIFF/AIFC PCM. Playback supports play, pause,
seek, queue navigation, volume, and per-track ReplayGain adjustment.

The five EQ bands are centred at 60 Hz, 250 Hz, 1 kHz, 4 kHz, and 16 kHz.
Custom EQ and the included Flat, Full Bass, Full Treble, Bass & Treble, Rock,
Pop, Jazz, and Classic presets are available in the UI.

The interface includes Menu, Queue, Music Library, folder browsing,
playlists, search, Now Playing, Equalizer, and Settings. A configurable
bottom virtual-control bar provides Menu, Previous, Play/Pause, Next, and
Back controls for the buttonless development kit.