# Lyra OS reference implementation — JC3248W535EN

This is the primary technical reference for the current native Lyra OS
implementation. It describes the checked-in ESP-IDF project for the
JC3248W535EN development kit, which is a convenient development/test reference
for vendors and developers adapting Lyra OS to their own product designs. Platform-level goals are in the
[Lyra OS platform baseline](EMOTIVATE_LYRA_OS_SPEC_BASELINE.md); exact board
information is in the [JC3248W535EN hardware reference](JC3248W535EN_HARDWARE.md).

## Reference configuration

| Item | Value |
|---|---|
| Target | ESP32-S3 |
| Board | JC3248W535EN |
| Display profile | `LYRA-320`, 320 × 480 portrait RGB565 |
| Framework | ESP-IDF 6.0.2, FreeRTOS, C++, LVGL 9.4.0 |
| Default resources | 16 MB flash and 8 MB octal PSRAM |
| Managed components | LVGL, `esp_audio_codec`, libjpeg-turbo, and libpng |
| Flash layout | NVS, OTA data, two 7.4375 MB application slots, and a 1 MB data partition named `littlefs` (SPIFFS subtype) |

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
| Audio service | `main/lyra_audio.cpp` | Decode, PCM conversion, equalizer, volume, seeking, and mirrored I2S output |
| Wi-Fi service | `main/lyra_wifi.cpp` | Station startup, network scans, saved-network connection, reconnect, and ping diagnostics |

The display is initialised with its backlight off. Lyra renders the boot
screen before enabling the panel, then starts its regular LVGL worker after
the GUI is ready. The system shell reports MicroSD and audio-service status
while keeping its settings and recovery interface available.

## Media and persistence

- The system clock is initialized before MicroSD mounting. At startup, a
  missing retained RTC/system-clock value uses the `00:00 01/01/2000` fallback
  and counts forward while powered. **Settings > System > Time & date** opens separate
  numeric **Set time** and **Set date** screens. The screens use a number pad
  with left/right cursor movement so individual digits can be corrected. The
  clock menu also stores the selected date format, 12/24-hour display format,
  and a manual daylight-saving correction. After full power loss, startup
  initializes the clock to its fallback epoch.
- The MicroSD card mounts at `/sdcard`. A user starts the library scan from
  `Settings > System > Scan music library`.
- Library scans still walk the card to discover additions and removals, but
  reuse catalog records whose path, size, and filesystem modification time are
  unchanged. Changed files are reparsed, and the replacement catalog reflects
  the files found on the card. Non-default sort caches are
  built lazily by the background indexer, keeping the GUI responsive during
  catalog publication.
- The recursive catalog accepts MP3, WAV, FLAC, AAC, M4A, OGG, Opus, AIFF,
  AIF, and AIFC files. The maximum catalog size is 10,000 tracks.
- The catalog is stored at `/sdcard/.lyra/catalog-v10.bin` and is published
  with a temporary file and backup so a failed scan retains the prior catalog.
- User playlists are portable `.m3u` or `.m3u8` files under
  `/sdcard/Playlists`. The `Favorites` playlist is created as needed.
- The Playlists screen includes generated Smart Playlists for Recently Added,
  Recently Played, Most Played, and Never Played. Playback counts and recency
  are persisted by track path in `/sdcard/.lyra/play-stats-v1.bin`.
- The active queue is stored separately at `/sdcard/.lyra/queue-v1.m3u8`.
- Settings, volume, equalizer preferences, sort choices, and artwork-cache
  choices use the `lyra` NVS namespace.
- Wi-Fi is disabled by default. Settings > System > Wi-Fi can enable the
  station, scan nearby networks, connect to open or password-protected
  networks, disconnect, and forget the saved network. The enabled state and
  one saved SSID/passphrase persist in NVS; an enabled station reconnects to
  that network after startup. Settings > About > Service Menu > Debug >
  Network tests pings `1.1.1.1` and resolves then pings `example.com`.
- Album art is loaded on demand. Embedded artwork takes priority, and folder
  artwork can come from `cover.jpg`, `folder.jpg`, or `cover.png`. Lyra keeps
  the display image in memory and can retain selected decoded artwork in
  `/sdcard/.lyra/covers`.
- Track catalog records capture each file's filesystem modification timestamp
  and refresh the size/date when file metadata is read, so files created or
  changed by Lyra receive the active clock date.

## Playback and UI

Audio is read from MicroSD and routed through the external PCM5102A I2S DAC
via GPIO6 (BCLK), GPIO15 (LRCK), and GPIO7 (data out). When enabled in
Settings, the same PCM stream is mirrored to the original on-board speaker
path via GPIO42 (BCLK), GPIO2 (LRCK), and GPIO41 (data out); the setting is
stored in NVS and enabled by default.
ESP-IDF decoders handle MP3, FLAC, AAC, M4A/ALAC, WAV, OGG, and Opus-in-Ogg;
Lyra also handles uncompressed AIFF/AIFC PCM. Playback supports play, pause,
seek, queue navigation, volume, and per-track ReplayGain adjustment.
The audio task preloads the queued successor in a second decoder and keeps one
PCM/I2S output stream across track boundaries. Configured crossfades mix both
decoded streams before output processing. See
[audio transition validation](AUDIO_TRANSITION_VALIDATION.md) for the device
measurements and codec behavior.

The five EQ bands are centred at 60 Hz, 250 Hz, 1 kHz, 4 kHz, and 16 kHz.
Custom EQ and the included Flat, Full Bass, Full Treble, Bass & Treble, Rock,
Pop, Jazz, and Classic presets are available in the UI.

The interface includes Menu, Queue, Music Library, the Files manager,
playlists, search, Now Playing, Equalizer, Apps, and Settings. A configurable
bottom virtual-control bar provides Menu, Previous, Play/Pause, Next, and
Back controls for the buttonless development kit. Holding Back returns to the
Menu from any depth, tapping a page title scrolls its list to the top, and
tapping the status-bar clock opens Now Playing. Queue additions confirm with
a short, non-blocking toast.

**Files** (Menu > Browse Folders, or Apps > Files) lists every non-hidden
entry of a MicroSD folder, folders first, sortable A–Z, Z–A, or by type. A
folder is read once with `readdir()` into a PSRAM listing; sizes are read
only for rows as they scroll into view, and rows load in batches instead of
pages. Tapping opens folders, plays audio in the displayed order, opens
images, text, and playlists. The row's ••• button or a long press offers
Play, Add to queue, Rename, Move, Copy (files), Delete, and Details; the
toolbar provides Up, sort, and New folder. Move uses a same-volume rename;
Copy and recursive Delete run in a background task behind a progress dialog.
Items that are currently playing cannot be renamed, moved, or deleted. Each
filesystem step holds the SD gate only briefly, so audio reads keep priority.
Library changes made this way appear in the catalog after the next scan.

**Apps** contains Calculator, Calendar, Stopwatch, Timer, Photos, Ebook,
Files, Dice, and Dot Link. Dot Link is a path puzzle on 5×5 to 8×8 boards:
join each pair of matching dots and fill every cell. Boards are generated on
the device by shuffling a Hamiltonian path with backbite moves and cutting it
into coloured segments, so every board has a complete solution; the hint
button reveals one route from it.

Themes with decorative wallpapers rasterize them once into PSRAM (Aura's sky
and light trails, Zeno's logo tile) and blit the cached pixels on redraw.

In Now Playing, tapping album art flips to a scrollable lyrics view. Lyrics
are loaded from embedded metadata first, then a same-stem `.lrc` file, then a
same-stem `.txt` file. Timestamped LRC lyrics highlight the current line;
plain text lyrics remain unhighlighted.
The optional Quick Seek playback setting adds 30-second backward and forward
controls beside the album art.
