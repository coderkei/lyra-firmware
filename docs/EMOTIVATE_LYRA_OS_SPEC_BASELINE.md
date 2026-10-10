# Lyra OS platform baseline

This document records platform-level behavior and goals for Lyra OS, an open-source reference firmware platform maintained by Emotivate. It describes software capabilities and design intent for adaptation across touchscreen audio-player designs.

The current working implementation is documented in the [JC3248W535EN reference](EMOTIVATE_LYRA_OS_SPEC_JC3248W535EN.md). Current and future hardware support are distinguished in [hardware support](HARDWARE_SUPPORT.md); broader adoption goals are in the [platform overview](PLATFORM_OVERVIEW.md).

## Platform goals

- Provide an offline touchscreen audio-player foundation for local music collections.
- Support catalog scanning, folder browsing, Unicode metadata, search, favourites, playlists, and a persistent playback queue.
- Keep user playlists and favourites separate from the rebuildable music catalog.
- Provide local playback controls, volume, seeking, per-track ReplayGain, equalization, and audio transitions supported by the current decoder/output implementation.
- Keep settings and recovery controls available when removable media or audio
  services need attention.
- Preserve responsive navigation, audio continuity, theme visuals, and playlist/catalog compatibility as the software evolves.

These software-level expectations guide feature development; each display, codec, DAC, and processor combination is documented through its own engineering and validation record.

## Current reference behavior

The current application is built with ESP-IDF, FreeRTOS, C++ and LVGL, with the JC3248W535EN ESP32-S3 development kit as its documented hardware target. Its 320 × 480 layout, AXS15231B display/touch integration, microSD configuration, and I2S wiring are implementation details of that reference target. See the [board-specific specification](EMOTIVATE_LYRA_OS_SPEC_JC3248W535EN.md) and [hardware notes](JC3248W535EN_HARDWARE.md).

The firmware recognizes MP3, WAV, FLAC, AAC, M4A, OGG, Opus, AIFF, AIF, and AIFC inputs, subject to configured decoder support and the format behavior recorded in the [audio transition validation notes](AUDIO_TRANSITION_VALIDATION.md). File errors are reported while the interface remains responsive.

The LYRA-320 display/memory profile describes this existing implementation. Future configurations establish their own targets through the engineering work described in the [porting guide](PORTING_GUIDE.md).
