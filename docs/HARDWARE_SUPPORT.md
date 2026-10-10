# Hardware support

Lyra OS has a broader platform intent than its present board support. The following table describes the current development and documented test configuration.

## Documented development and test configuration

| Item | JC3248W535EN reference configuration |
| --- | --- |
| Status | Current development and documented test target for the Lyra OS reference implementation |
| Processor | ESP32-S3 |
| GUI | 320 × 480 portrait touch display, AXS15231B over QSPI |
| Memory | 16 MB flash and 8 MB PSRAM in the current board specification/configuration |
| Storage | microSD via 1-bit SDMMC |
| Audio | External PCM5102A I2S DAC; optional on-board speaker mirror where fitted and enabled |
| Detailed pins and wiring | [JC3248W535EN hardware reference](JC3248W535EN_HARDWARE.md) and [`lyra_board_pins.h`](../board/JC3248W535EN/include/lyra_board_pins.h) |

The **LYRA-320** profile (320 × 480, 16 MB flash, and 8 MB PSRAM) describes the current implementation. Each future port should set its own display and memory targets based on selected features and confirm them through engineering and physical validation. The [hardware reference](JC3248W535EN_HARDWARE.md) lists checks for this board that call for physical measurements.

## What a future port must assess

Porters can assess these engineering categories for each device:

- Processor and memory budget for the selected codecs, themes, artwork, and enabled features.
- Display controller, transport, color format, resolution, orientation, and LVGL flush semantics.
- Touch controller, coordinates, calibration, gestures, and any physical input controls.
- microSD or other storage wiring, filesystem format, mount behavior, and contention with audio reads.
- I2S controller and pin assignments, DAC/codec requirements, output level, amplifier path, and channel format.
- Power, battery, charger, safe shutdown, and any product-specific reporting.
- Flash size, partition table, OTA slots, recovery path, and consistency with the firmware updater.
- Optional speaker/amplifier behavior and whether mirroring is supported.
- Audio stability and GUI responsiveness under representative storage, artwork, and library loads.

## Verified community ports

Add a community port here after documenting its implementation and validation evidence.

| Board and revision | Processor/display/audio | Firmware revision | Validation reported |
| --- | --- | --- | --- |
|  |  |  |  |
