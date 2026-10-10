# Lyra OS platform overview

Lyra OS is an open-source reference firmware platform by Emotivate for affordable touchscreen digital audio players. It is specialised application firmware built on ESP-IDF, FreeRTOS, C++ and LVGL, intended to provide a reusable offline music-player foundation for hardware developers, hobbyists, OEMs, and manufacturers.

## The problem and goal

Budget touchscreen players pair capable processors and displays with navigation, search input, and original-language metadata workflows that can improve through a cohesive software platform. Building a complete library, input system, playback pipeline, and interface for every device also duplicates substantial work.

Lyra OS aims to make an adaptable firmware foundation available: scan and navigate a local music collection, search it with multilingual input, and play stored audio through an integrated interface. The application code uses the permissive Apache-2.0 license; dependencies and bundled data keep their own terms.

## Architecture in the current implementation

| Component | Repository paths | Current responsibility |
| --- | --- | --- |
| Startup and GUI | `main/app_main.cpp`, `main/lyra_gui.cpp`, `main/gui/` | Starts the services and LVGL interface; implements navigation, themes, search, settings, and display of library data. |
| Media catalog and storage | `main/lyra_media.cpp`, `main/media/`, `main/lyra_sd.cpp` | Mounts the reference microSD storage, scans and searches the catalog, reads metadata and artwork, manages playlists, and arbitrates SD access with audio. |
| Audio playback | `main/lyra_audio.cpp`, `main/audio/` | Decodes supported local formats, mixes PCM transitions, applies volume/EQ/ReplayGain, and sends samples to the configured I2S outputs. |
| Board integration | `components/lyra_board/`, `board/JC3248W535EN/include/lyra_board_pins.h` | Initializes the current panel, touch input, backlight, LVGL display, and reference-board pin assignments. |
| Localisation and fonts | `tools/i18n/lyra_i18n_catalog.inc`, `main/lyra_font.cpp`, `main/lyra_unicode_16.c`, `main/lyra_cjk_16.c` | Supplies translated interface text and embedded glyph coverage used for interface and music metadata. |
| Build and flash layout | `CMakeLists.txt`, `main/idf_component.yml`, `sdkconfig.defaults`, `partitions.csv` | Defines the ESP-IDF project, managed components, configured memory, and current flash partitions. |

## Design principles

- **Offline first:** the library, playlists, search, and playback operate on local microSD files.
- **Fast collection navigation:** catalog metadata, sorting, and search provide direct paths through larger libraries.
- **Original-language metadata:** Unicode display and selectable input methods support searching multilingual libraries.
- **Audio continuity under load:** audio-aware SD access and a persistent PCM/I2S output path are intended to keep playback stable while the GUI and media features are active. Physical measurements remain necessary.
- **Accessible hardware:** the project is developed on a readily available low-cost ESP32-S3 kit, while recognizing that its wiring and drivers are board-specific.
- **Theme fidelity:** existing themes and visual assets are part of the user experience and should be preserved during performance or platform work.

## Platform services and board-specific integration

The catalog, playlist logic, decoder orchestration, GUI features, localisation, and settings express the reusable software direction. The working implementation also includes hardware-specific display initialization, touch protocol and calibration, I2S GPIO setup, fixed 320 × 480 layout geometry, SD mount details, and board-dependent power assumptions. Future portability work can separate these responsibilities as additional targets are engineered.

The JC3248W535EN is the currently documented development and test configuration. It provides a convenient reference for adapting Lyra OS to other device designs. Other ESP32-S3 boards and microcontrollers begin with engineering and validation; see [hardware support](HARDWARE_SUPPORT.md) and the [porting guide](PORTING_GUIDE.md).

## Intended adoption model

A hardware developer or vendor can study the reference implementation, adapt board integration and interface geometry to its device, and retain or modify the media, playback, and localisation features that fit the product. Validate each port on its display, touch controls, storage, audio path, memory configuration, and update/recovery scheme. The repository provides a software reference; device integration and certification are part of each hardware program.
