# Emotivate Lyra firmware Device baseline

This document defines the Device-level contract for Emotivate Lyra. It is
intentionally hardware-neutral where possible. The
[JC3248W535EN firmware reference](EMOTIVATE_LYRA_OS_SPEC_JC3248W535EN.md)
defines the build target, board wiring, and implementation details.

## Device profile

| Area | Specification |
|---|---|
| Profile | `LYRA-320`: a 320 × 480 portrait touchscreen music player |
| SoC | ESP32-S3 |
| Memory | At least 16 MB flash and 8 MB PSRAM |
| Media | MicroSD-backed local music library |
| Interface | High-legibility dark UI with `#0B0E14` background and `#38BDF8` accent |
| Audio | File-backed local playback through an I2S output path |

## Device behaviour

- The system remains usable when removable media or audio output is unavailable
  and presents a clear recoverable state.
- The library supports scanning, folders, metadata, search, favourites,
  playlists, and a persistent playback queue.
- User-owned playlists and favourites are independent of the rebuildable
  library index.
- Audio playback provides volume, seeking, ReplayGain adjustment, and a
  five-band equalizer. Demo playback is always file-based; synthesized music
  is not part of the Device.
- The touch UI includes a configurable virtual navigation bar for development
  hardware without physical controls.
- Settings, sorting choices, and sound preferences persist across restarts.

## Supported media contract

The firmware recognises MP3, WAV, FLAC, AAC, M4A, OGG, Opus, AIFF, AIF, and
AIFC files. Decoder support is configured by the ESP-IDF project; a corrupt
or unsupported file must fail without destabilising the UI.
