# Porting Lyra OS to another board

This guide is a source map and engineering checklist. Current development follows the JC3248W535EN reference implementation; adapting another target starts with board-specific code changes, hardware testing, and a clear record of its differences.

## Source locations to study

| Path | What it currently contains |
| --- | --- |
| `components/lyra_board/include/lyra_board.h` | Existing display initialization, LVGL display access, brightness/backlight control, and explicit LVGL task-start interface. |
| `components/lyra_board/lyra_board.cpp` | JC3248W535EN display/touch initialization, LVGL buffers and flush callback, touch polling, backlight, and worker setup. |
| `components/lyra_board/lyra_axs15231b.cpp` | AXS15231B panel command and QSPI transfer implementation. |
| `board/JC3248W535EN/include/lyra_board_pins.h` | Current panel, touch, SD, I2S, speaker, and battery-sense GPIO definitions. |
| `main/lyra_audio.cpp` | Main I2S output initialization and optional second I2S device for mirrored speaker output. |
| `main/gui/lyra_gui_internal.h` and `main/gui/` | GUI-wide constants and screens; `kScreenWidth` and `kScreenHeight` are fixed at 320 × 480, with substantial absolute positioning. |
| `sdkconfig.defaults`, `partitions.csv`, `main/idf_component.yml`, `CMakeLists.txt` | Reference memory settings, flash layout, managed dependencies, build configuration, and component wiring. |
| `main/lyra_sd.cpp`, `main/lyra_media.cpp`, `main/media/` | Audio-aware SD access gate, microSD mounting, catalog and media services. |
| `docs/JC3248W535EN_HARDWARE.md`, `docs/EMOTIVATE_LYRA_OS_SPEC_JC3248W535EN.md` | Current board wiring and implementation behavior. |
| `docs/AUDIO_TRANSITION_VALIDATION.md` | Audio continuity counters and physical validation procedure. |

## Display driver caution

The current AXS15231B QSPI path deliberately transfers a complete frame after LVGL has rendered its invalidated areas into the persistent full-screen buffer. This behavior follows panel command and row-window constraints observed in the current driver. Establish correctness for any partial-screen SPI/QSPI approach on its specific panel and driver, including repeated updates and long-running GUI activity.

Supporting display resolutions beyond 320 × 480 includes GUI geometry and layout work alongside changes to `LYRA_BOARD_DISPLAY_WIDTH` and `LYRA_BOARD_DISPLAY_HEIGHT`, because current screens use fixed dimensions and absolute positions.

## Porting checklist

1. Record the exact board revision, processor, flash/PSRAM configuration, display, touch, storage, audio components, power path, and available controls.
2. Establish the new display driver and verify LVGL flush completion, pixel format, orientation, invalidation, and backlight sequencing.
3. Calibrate touch coordinates and validate taps, swipes, and any physical navigation controls.
4. Bring up removable storage and check scans, metadata, artwork, playlists, file browsing, and SD access while audio is playing.
5. Configure I2S pins, clocks, sample format, DAC or codec, analog output, amplifier, and any optional speaker path.
6. Verify heap/PSRAM use, task stacks, flash capacity, partition sizing, OTA slots, and the recovery path; keep the partition map consistent with update logic.
7. Preserve audio task priority and the audio-aware storage arbitration. Measure playback continuity while scanning, browsing artwork, and changing tracks.
8. Test Unicode metadata, search input methods, installed dictionary behavior, all themes, and touch navigation on the real display.
9. Exercise update, reboot, interrupted-update recovery, storage removal, and audio/output failure behavior.
10. Document board differences, firmware revision, build configuration, and the evidence gathered through source review, builds, emulator runs, and physical tests.

Use the [reference wiring table](JC3248W535EN_HARDWARE.md) for exact JC3248W535EN details. Select and document board-specific pins for another target, then record its actual hardware behavior before marking the port verified.

## Incremental board-profile direction

Future portability work should proceed against a second concrete target and preserve the current pin, timing, panel-transfer, and peripheral behavior:

1. Centralize board identity and metadata while preserving current runtime behavior.
2. Make active board name and capability information available to About and diagnostic screens from shared metadata.
3. Place board-specific drivers and GPIO assignments in board support, keeping
   GUI and media logic independent of pin details.
4. After selecting and testing a second target, add explicit board selection.
5. Treat different LCD sizes/orientations, touch protocols, and navigation controls as separate compatibility projects with regression checks.

This sequence describes future work that can proceed after a second target is selected and tested.
