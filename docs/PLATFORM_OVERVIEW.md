# Lyra OS platform overview

Lyra OS is open-source firmware for touchscreen digital audio players, made by
Emotivate. It's built on ESP-IDF, FreeRTOS, C++ and LVGL, and it's meant to be
something other people can pick up and build on, whether that's a hobbyist
with a different board or a company making a player.

## Why it exists

Cheap touchscreen players often have decent hardware: a capable processor, a
nice screen, enough storage. Where they tend to fall down is the software.
Browsing a big library is slow, search is awkward or in many cases, simply not implemented, and music tagged in
Japanese, Korean, Chinese or Russian often shows up as boxes due to missing fonts. And every
manufacturer ends up writing their own library, input system, playback
pipeline and interface from scratch.

Lyra tries to be that missing piece, like an open standard platform, done once and shared. Scan a music
collection, find things quickly in any language, and play it well. The
application code is Apache-2.0 so it can be used freely; the libraries and
data it depends on keep their own licences (see the
[third-party notices](../THIRD_PARTY_NOTICES.md)).

## How the code is organised

| Part | Where it lives | What it does |
| --- | --- | --- |
| Startup and GUI | `main/app_main.cpp`, `main/lyra_gui.cpp`, `main/gui/` | Starts everything up and runs the LVGL interface: navigation, themes, lists, search, keyboards, settings and apps. |
| Media and storage | `main/lyra_media.cpp`, `main/media/`, `main/lyra_sd.cpp` | Mounts the microSD card, scans and searches the library, reads tags and artwork, makes list thumbnails, manages playlists, and makes sure SD reads for audio always come first. |
| Audio | `main/lyra_audio.cpp`, `main/audio/` | Decodes the supported formats, handles gapless and crossfade transitions, applies volume, EQ and ReplayGain, and feeds the I2S outputs. |
| Network | `main/lyra_wifi.cpp`, `main/lyra_radio.cpp`, `main/lyra_time_sync.cpp` | Wi-Fi, internet radio stations, and setting the clock from network time. |
| System services | `main/lyra_usb_transfer.cpp`, `main/lyra_crash_log.cpp` | USB file transfer (the card as a USB drive) and crash reports. |
| Board support | `components/lyra_board/`, `board/JC3248W535EN/include/lyra_board_pins.h` | Display, touch, backlight and LVGL setup for the reference board, plus its pin assignments. |
| Languages and fonts | `tools/i18n/lyra_i18n_catalog.inc`, `main/lyra_font.cpp`, `main/lyra_unicode_16.c`, `main/lyra_cjk_16.c` | Translated interface text and the embedded glyphs used for the interface and for song metadata. |
| Build and flash layout | `CMakeLists.txt`, `main/idf_component.yml`, `sdkconfig.defaults`, `partitions.csv` | The ESP-IDF project, its components, memory settings and flash partitions. |

## What we care about

- **It works offline.** The library, playlists, search and playback all run
  from files on the card. Wi-Fi is only for radio and the clock.
- **Big libraries stay fast.** Lists scroll smoothly with thousands of
  tracks, and sorting, jump strips and search get you anywhere quickly.
- **Music in any language.** Tags display properly in the supported scripts,
  and there are keyboards to search them.
- **Audio doesn't stutter.** SD access is arranged so audio reads win, and
  the output path stays running between tracks. This still needs measuring on
  real hardware for each board.
- **Cheap hardware.** Development happens on a low-cost ESP32-S3 kit that
  anyone can buy, even though its wiring and drivers are specific to it.
- **The themes matter.** They're part of the experience, so performance or
  porting work shouldn't quietly break how they look.

## What's reusable and what's board-specific

The catalog, playlists, decoder handling, GUI features, translations and
settings are the reusable core. Mixed in with them, for now, is code that only
makes sense on this board: the display driver, the touch protocol and
calibration, the I2S pins, the fixed 320 × 480 layouts, SD mounting details,
the USB arrangement and some power assumptions. Separating those more cleanly
is future work, and it'll go best alongside a real second target.

The JC3248W535EN is the board everything is developed and tested on. If you
want to use something else, expect some engineering first; the
[hardware support](HARDWARE_SUPPORT.md) page and the
[porting guide](PORTING_GUIDE.md) explain what's involved.

## Using it for your own device

The intended path is: study the reference implementation, adapt the board
support and screen layouts to your hardware, and keep (or change) whichever
media, playback and language features suit your product. Then test it
properly on your own display, touch, storage, audio path, memory setup and
update/recovery process. The repository gives you the software; integrating
it into a device and getting that device certified is part of your project.
