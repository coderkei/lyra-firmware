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
| Managed components | LVGL, `esp_audio_codec`, `esp_tinyusb`, libjpeg-turbo, and libpng |
| Flash layout | NVS, OTA data, two 7.4375 MB application slots, an 896 KiB data partition named `littlefs` (SPIFFS subtype, currently unused), and a 128 KiB `coredump` partition |
| Version | `version.txt`, embedded by ESP-IDF as the application version and shown on the About screen |

`sdkconfig.defaults`, `partitions.csv`, `main/idf_component.yml`, and
`components/lyra_board/idf_component.yml` define the build configuration.
Generated `sdkconfig`, `build`, and `managed_components` directories are
local ESP-IDF outputs. An existing `sdkconfig` keeps its values, so options
added to `sdkconfig.defaults` later (core dumps, TinyUSB mass storage) must be
enabled in menuconfig or picked up by regenerating `sdkconfig`.

## Board pins

The complete pin map is maintained once in the
[hardware reference](JC3248W535EN_HARDWARE.md) and in the authoritative
[`lyra_board_pins.h`](../board/JC3248W535EN/include/lyra_board_pins.h) header.
The firmware uses polled I2C touch input and 1-bit SDMMC; GPIO3 and GPIO10 are
hardware references.

## Firmware architecture

| Area | Source | Responsibility |
|---|---|---|
| Startup | `main/app_main.cpp` | Brings up the display, boot screen, media service, crash-report check, audio service, Wi-Fi, GUI, and LVGL worker |
| Board support | `components/lyra_board/` | AXS15231B QSPI display, backlight, LVGL integration, and touch polling |
| User interface | `main/lyra_gui.cpp`, `main/gui/` | LVGL screens, touch routing, settings, navigation, scrolling lists, and keyboards |
| Media service | `main/lyra_media.cpp`, `main/media/` | MicroSD mount, catalog, metadata, playlists, search, sorting, artwork, and list thumbnails |
| Audio service | `main/lyra_audio.cpp`, `main/audio/` | Decode, PCM conversion, equalizer, volume, seeking, and mirrored I2S output |
| Wi-Fi service | `main/lyra_wifi.cpp` | Station startup, network scans, saved-network connection, reconnect, and ping diagnostics |
| Internet radio | `main/lyra_radio.cpp` | Saved HTTP(S) stations and station-list import |
| Network time | `main/lyra_time_sync.cpp` | NTP query on each Wi-Fi connection |
| Crash reports | `main/lyra_crash_log.cpp` | Moves core dumps and reset reports to the MicroSD card |
| USB file transfer | `main/lyra_usb_transfer.cpp` | Exposes the MicroSD card as a USB mass-storage drive |

The display is initialised with its backlight off. Lyra renders the boot
screen before enabling the panel, then starts its regular LVGL worker after
the GUI is ready. The system shell reports MicroSD and audio-service status
while keeping its settings and recovery interface available.

## Media and persistence

- The MicroSD card mounts at `/sdcard`. A user starts the library scan from
  `Settings > Library > Scan music library`.
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
- Track catalog records capture each file's filesystem modification timestamp
  and refresh the size/date when file metadata is read, so files created or
  changed by Lyra receive the active clock date.
- User playlists are portable `.m3u` or `.m3u8` files under
  `/sdcard/Playlists`. The `Favorites` playlist is created as needed.
- The Playlists screen includes generated Smart Playlists for Recently Added,
  Recently Played, Most Played, and Never Played. Playback counts and recency
  are persisted by track path in `/sdcard/.lyra/play-stats-v1.bin`.
- The active queue is stored separately at `/sdcard/.lyra/queue-v1.m3u8`.
- Internet radio stations are stored at `/sdcard/.lyra/radio-stations-v1.tsv`.
  Station lists can be imported from `.m3u`, `.m3u8`, `.pls`, and `.asx` files.
- Settings, volume, equalizer preferences, sort choices, time settings, and
  artwork-cache choices use the `lyra` NVS namespace.
- Wi-Fi is disabled by default. Home > Wi-Fi (also in Settings) can enable
  the station, scan nearby networks, connect to open or password-protected
  networks, disconnect, and forget the saved network. The enabled state and
  one saved SSID/passphrase persist in NVS; an enabled station reconnects to
  that network after startup. Settings > About > Service Menu > Debug >
  Network tests pings `1.1.1.1` and resolves then pings `example.com`.

### Clock

The board has no battery-backed RTC. The system clock is initialized before
MicroSD mounting; a missing retained clock value uses the `00:00 01/01/2000`
fallback and counts forward while powered. The clock holds local time
directly; there is no time-zone database.

**Settings > Time & date** opens separate numeric **Set time** and
**Set date** screens. They use a number pad with left/right cursor movement so
individual digits can be corrected. The clock menu also stores the date
format, 12/24-hour display format, a manual daylight-saving correction (a
one-hour shift), and the network time settings:

- **Set time automatically** (on by default) runs a small SNTP query each time
  Wi-Fi gets an address, trying `pool.ntp.org`, then `time.google.com`, then
  `time.cloudflare.com`. The clock is set to UTC plus the **Time zone** offset
  (UTC−12:00 to UTC+14:00 in 15-minute steps) plus the daylight-saving hour.
  ESP-IDF's own SNTP client is not used, because it would set the clock to UTC.
- With automatic time off, a daylight-saving setting is cleared after power
  loss, because the manual shift was lost with the clock. With it on, the
  setting is kept and applied at the next sync.

### Artwork

- Album art is loaded on demand. Embedded artwork takes priority, and folder
  artwork can come from `cover.jpg`, `folder.jpg`, or `cover.png`. Lyra keeps
  the display image in memory and can retain selected decoded artwork in
  `/sdcard/.lyra/covers`. The player and list-thumbnail decoders share one
  JPEG decoder and take turns.
- Albums, Songs, the Queue, playlist details (including Favorites and the
  Smart Playlists), genre and year song lists, and search results show a 48x48
  thumbnail beside each row (Settings > Appearance > Album art in lists, on by
  default). Each cover is decoded once at thumbnail size by a low-priority
  worker that serves on-screen rows first, then the rows ahead of the scroll,
  and waits while the player cover is loading. Results, including "no
  artwork", are appended to a packed cache
  (`/sdcard/.lyra/covers/thumbs-v2-48.idx` and `.rgb565`). Songs share their
  album's entry, keyed by folder and album tag; a song without an album tag is
  keyed by its path, size, and modification time. Later visits need one small
  read per row. A 32-entry PSRAM cache, freed when the list closes, covers
  scrolling back. Clearing the artwork cache removes the thumbnails too;
  changed cover art is picked up only after that.

## System services

- **USB file transfer.** Settings > System > USB file transfer stops playback,
  saves the queue, unmounts the card, and exposes it to a computer as a USB
  mass-storage drive through TinyUSB. The ESP32-S3 has one USB PHY, so the USB
  serial/JTAG port (logs, monitor, flashing) is unavailable until **Finish and
  restart**. The transfer screen sits on LVGL's top layer and blocks all other
  input. The mode is never persisted: every boot, crash, or power cycle starts
  normally. The card is never formatted from this mode. Builds without
  `CONFIG_TINYUSB_MSC_ENABLED` say the feature is unavailable.
- **Crash reports.** After a panic, watchdog, brownout, or CPU-lockup reset,
  Lyra writes `/sdcard/.lyra/crash/crash-YYYYMMDD-HHMM.txt` (firmware version,
  ELF hash, reset and panic reason, task, PC, and backtrace) and, when present,
  the ELF core dump as `...-core.elf`, then erases the flash copy and tells the
  user once. Core dumps use the 128 KiB `coredump` partition at 0xFE0000.
  Devices updated with `update.bin` keep their old partition table and get
  text reports only.
- **Firmware updates** are described under Memory and performance below.

## Playback and UI

Audio is read from MicroSD and routed through the external PCM5102A I2S DAC
via GPIO6 (BCLK), GPIO15 (LRCK), and GPIO7 (data out). When enabled in
Settings, the same PCM stream is mirrored to the original on-board speaker
path via GPIO42 (BCLK), GPIO2 (LRCK), and GPIO41 (data out); the setting is
stored in NVS and enabled by default.
ESP-IDF decoders handle MP3, FLAC, AAC, M4A/ALAC, WAV, OGG, and Opus-in-Ogg;
Lyra also handles uncompressed AIFF/AIFC PCM. Playback supports play, pause,
seek, queue navigation, volume, playback speed from 0.25× to 2×, and
per-track ReplayGain adjustment.
The audio task preloads the queued successor in a second decoder and keeps one
PCM/I2S output stream across track boundaries. Configured crossfades mix both
decoded streams before output processing. See
[audio transition validation](AUDIO_TRANSITION_VALIDATION.md) for the device
measurements and codec behavior.

The five EQ bands are centred at 60 Hz, 250 Hz, 1 kHz, 4 kHz, and 16 kHz.
Custom EQ and the included Flat, Full Bass, Full Treble, Bass & Treble, Rock,
Pop, Jazz, and Classic presets are available in the UI.

**Design.** The interface uses flat dark or light pages, rounded cards and
rows, two-column tiles with two-tone badges, pill choices and switches, bottom
sheets for actions, and one of six accent colours (blue, orange, yellow,
green, purple, pink). Dynamic tint mixes the accent into every neutral layer.
Headings use an 18 px font (Montserrat with the scaled multilingual fallback).
The shared pieces live in `main/gui/shared/lyra_gui_design.cpp`, and the
palette is built in `apply_theme_palette()`.

**Navigation.** Home shows six tiles: Music, Internet radio, Wi-Fi, Files,
More and Settings. Music opens a tile page (All songs, Albums, Artists,
Genres, Year, Favorites, Playlists, Recently played, Recently added, Most
played, Folders) with Search, Queue and Library settings in its header. More
holds Photos, Ebook, Queue, Equalizer, Search and Playlists.

The kit has no buttons, so gestures carry navigation. A swipe right goes
back, and a swipe left opens Now Playing (from Now Playing, it opens the
queue). Pulling the status bar down draws out the control centre under the
finger: the clock, brightness, volume, the playing song with its transport,
and toggles for Wi-Fi, the on-board speaker, gapless playback, dark theme,
the sleep timer and the equalizer. A tap on the status bar does nothing, so a
near miss on Back never opens it. The panel settles open once it has been
pulled a quarter of its height and closes the same way when pushed back up,
or with a tap on the dimmed page. Typing and viewer pages keep horizontal
drags for themselves.

Browsing pages end in a mini player (cover, title, play/pause, next) that
opens Now Playing when tapped or pushed up; Settings > Appearance can hide
it. Tapping a page title scrolls its list to the top. Queue additions confirm
with a short, non-blocking toast. A screen that redraws itself (a toggle, an
option, a status refresh) keeps its scroll position; navigating to other
content, including another playlist, album, or folder, starts at the top, and
Back returns to the previous position.

**Files** (Home > Files, or Music > Folders) lists every non-hidden
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

**More** lists every picture (Photos) or text file (Ebook) on the card from
a background scan, a page at a time, and opens them in the viewers.

**Viewers.** Photos open full screen. A translucent top bar shows the file
name and pixel size, and a floating pill holds zoom out, the zoom level, zoom
in, and Fit; 1:1 shows native pixels. Tapping the image hides or shows the
controls, and dragging pans on both axes. The visible area is decoded again
from the original file at the current zoom once panning settles. The text
reader pages through files in 2 KiB blocks with a reading-progress bar and
previous/next page buttons. Its options sheet sets the text size (three
sizes), the page colour (light, sepia, or dark), and one bookmark per file,
stored under `/sdcard/.lyra/bookmarks`.

**Large libraries.** Songs, Albums, Artists, Genres, Years, genre and year
song lists, playlist details, search results, and the Queue scroll as one
continuous list instead of pages. Each list keeps a fixed pool of about one
screen of row widgets. Crossing a row boundary rebinds the entering rows and
leaves the other widgets in place. The prefetch task fills a 128-entry text
cache, prioritizing the visible rows and then the direction of travel. The
first screen is fetched synchronously. Playlist and group song lists resolve
their tracks once when opened. The media service collects compact track
summaries during catalog validation and retains group headers within a PSRAM
budget. Entries outside that budget fall back to the catalog file. Recent
album and artist track orders are also cached for playback and browsing.
Songs, Albums, and Artists show a jump strip built from the sort jump index —
initials for name sorts, two-digit years for date sorts, whole minutes for
duration, and track numbers — with a bubble showing the full value while
scrolling, and a jump button that opens the page-jump screen (whose sort tab
also covers dates and durations) and returns to the exact row. The Queue
opens at the playing song. Album pages open with a large cover and the
album's details above the songs. The artist page loads its song and album
rows in batches as it scrolls.

**Search** covers songs, albums, artists, and playlist songs. A search starts
only from the keyboard's Search key, which also hides the keyboard so the
results fill the screen; changing the category re-runs the current query.

**Text input.** Keyboards are independent of the interface language: English
(with an accent page), Russian, Korean Hangul, Chinese Pinyin, and Japanese
kana flick input, plus an optional T9 keypad. Text fields show a blinking
caret. Edits happen at the caret, including input-method composition, and
swiping along the space bar of any keyboard moves the caret one character per
14 px.

## Memory and performance

**Display performance.** The AXS15231B accepts pixel writes only as one
stream that starts at row zero, so every invalidated area is rounded to full
rows from the top of the panel. LVGL renders that range in horizontal bands
into two internal-RAM DMA buffers (16 rows each by default,
`CONFIG_LYRA_DISPLAY_BAND_LINES`). Each band is byte-swapped in place and sent
inside a single CS-held RAMWR while LVGL renders the next band into the other
buffer, so drawing and the QSPI transfer overlap. The refresh waits only for
its final band. No PSRAM framebuffer is read or written, which keeps the
display out of the PSRAM bandwidth and data cache that audio decoding uses.
Recovery from a transfer failure redraws the full panel. The previous PSRAM
framebuffer pipeline remains selectable as
`CONFIG_LYRA_DISPLAY_LEGACY_FRAMEBUFFER` for comparison.

LVGL's heap lives in PSRAM (`components/lyra_board/lyra_lvgl_memory.cpp`), so
widgets, styles, label text and draw tasks do not consume internal RAM. Wi-Fi
and LWIP buffers and TLS sessions are also allocated in PSRAM first, and the
Wi-Fi IRAM throughput options are disabled. The Wi-Fi driver initializes at
boot even when Wi-Fi is off, because its static RX buffers and task need
contiguous internal RAM that playback later fragments. Six static RX buffers,
matching the block-ack window, keep that reservation small. Internal RAM stays
available for the audio output task,
DMA, the display bands and the GUI stack. The I2S DMA rings stay at 186 ms
each: flash writes stall the output task, and the rings must cover an NVS
sector erase during playback. Background MicroSD workers (scan, search, sort
and duration indexing, artwork and thumbnails) never write flash and run on
PSRAM stacks. A
firmware update from `/sdcard/update.bin` writes flash, so it runs on the GUI
task's internal stack after stopping playback and waiting for media work; a
failed update shows its error and restarts Lyra six seconds later.
`update.bin` may be the app image (`build/lyra_firmware.bin`) or an
`idf.py merge-bin` image, whose embedded partition table locates the app. Only
the app is installed; a merged image's bootloader and partition table are not
written.
Decoded glyph masks and glyph lookups are cached in PSRAM (`main/lyra_font.cpp`);
the compressed UI font is otherwise decompressed for every glyph on every
redraw. QIO flash and a 32 KiB instruction cache reduce the stalls that LVGL
on the GUI core and the decoders on the audio core cause each other. Audio
decode, mixing, and I2S output keep their existing core and priority
assignments. `CONFIG_LYRA_LCD_QSPI_CLOCK_80MHZ` halves panel transfer time
and needs validation on each board.

For device validation, enable `CONFIG_LYRA_GUI_FRAME_TIMING` in menuconfig.
The serial log reports the actual QSPI clock, the band height and internal
heap after display setup, and summarizes frame time, the wait for the final
band, rows sent per frame and intervals above 33.3 ms. The interval summary
omits gaps over 100 ms, so also watch for visible stalls during continuous
gestures.
Compare continuous drags and flings in Songs, Albums, and Queue with playback
idle and with playback active. Include a track handoff with artwork and
crossfade enabled, and compare the audio underrun counters against the
previous firmware. Check the boot splash and small status-bar updates for
color or row corruption, and exercise jump-strip navigation and queue edit
buttons after scrolling.

## Now Playing

Now Playing fills the top 320 × 320 px with the cover, under a translucent
status bar and a close button. The panel below takes its colour from the
average of the cover's pixels and holds the title with a favourite heart,
the artist (tap to open the artist) with the file format, the seek bar with
elapsed time, queue position and duration, and the transport: the play-mode
button (in order, repeat all, repeat one, shuffle), previous, play/pause,
next, and a "more" sheet with Queue, Add to playlist, Go to album, Go to
artist, Information, Equalizer, Playback speed, A-B repeat, Bookmarks and
Sleep timer. Holding previous or next seeks.

In Now Playing, tapping album art flips to a scrollable lyrics view. Lyrics
are loaded from embedded metadata first, then a same-stem `.lrc` file, then a
same-stem `.txt` file. Timestamped LRC lyrics highlight the current line;
plain text lyrics remain unhighlighted.
The optional Quick Seek playback setting adds 30-second backward and forward
controls over the bottom corners of the album art.
