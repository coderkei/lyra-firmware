# JC3248W535EN hardware reference

This document records the board profile used by the current Lyra OS reference
implementation and the physical checks for this hardware. The compile-time
pin definitions are
[`board/JC3248W535EN/include/lyra_board_pins.h`](../board/JC3248W535EN/include/lyra_board_pins.h).
This board serves as a development and test reference for Lyra OS. Use the
[firmware reference](EMOTIVATE_LYRA_OS_SPEC_JC3248W535EN.md)
for its software implementation and the [platform overview](PLATFORM_OVERVIEW.md)
for the broader adoption intent.

## Board profile

| Area | Specification |
|---|---|
| Module | ESP32-S3-WROOM-1, dual-core LX7 up to 240 MHz |
| Display | 320 × 480 RGB565 AXS15231B panel over QSPI |
| Touch | AXS15231B I2C interface; the firmware uses polling |
| Memory profile | 16 MB flash and 8 MB PSRAM configured by default |
| Removable storage | MicroSD socket using 1-bit SDMMC |
| Audio | I2S output to an external PCM5102A DAC and the on-board speaker/amplifier |
| Power | USB-C input, battery connector, and a battery-sense divider |

The values above come from the supplied board.

## Pin map

| Function | GPIO | Notes |
|---|---:|---|
| LCD backlight | 1 | Active-high board contract |
| LCD QSPI CS / clock | 45 / 47 | AXS15231B |
| LCD QSPI D0 / D1 / D2 / D3 | 21 / 48 / 40 / 39 | AXS15231B |
| LCD TE | 38 | Panel timing signal |
| Touch SCL / SDA | 8 / 4 | I2C port 0 |
| Touch interrupt reference | 3 | The native driver polls touch |
| MicroSD DAT0 / clock / command | 13 / 12 / 11 | 1-bit SDMMC |
| MicroSD SPI CS reference | 10 | The active 1-bit SDMMC configuration uses DAT0, clock, and command |
| External DAC I2S BCLK / LRCLK / data out | 6 / 15 / 7 | PCM5102A via the 8-pin JST |
| On-board speaker I2S BCLK / LRCLK / data out | 42 / 2 / 41 | Original speaker/amplifier path |
| Battery sense | 5 | Divider calibration required |

The board profile uses the AXS15231B panel and touch controller interfaces,
and the PCM5102A's BCK-derived internal PLL for audio clocking. Connect the
DAC's `SCK` pin to DAC ground.

## External PCM5102A wiring

| JC3248W535EN connector | PCM5102A pin |
|---|---|
| 4-pin JST `GND` | `GND` |
| 4-pin JST `3V3` | `VIN` |
| 8-pin JST `IO6` | `BCK` |
| 8-pin JST `IO7` | `DIN` |
| 8-pin JST `IO15` | `LRCK` |
| DAC `GND` | DAC `SCK` (jumper) |

Configure the DAC for standard I2S: `FLT` low, `DEMP` low, `XSMT` high, and
`FMT` low. These are normally selected using the DAC board's H1L-H4L solder
jumpers. Connect `ROUT` and `LROUT`, the line-level outputs, to an amplifier
or active speaker input.

The ESP32-S3 has two I2S peripherals. Lyra uses the second one to mirror
playback to the original on-board speaker when **Settings > Sound > On-board
speaker** is enabled. The setting is enabled by default and persists in NVS.

## Hardware validation required

Record these measurements and observations from the physical development kit:

- Detected flash size/mode and PSRAM mode/speed.
- Touch orientation, coordinate alignment, and whether GPIO3 is a reliable
  interrupt source.
- Panel orientation and the usefulness of the TE signal for the final flush
  path.
- The fitted audio device, its enable/shutdown behaviour, and speaker output
  quality.
- Battery chemistry, charger behaviour, divider ratio, ADC calibration, and
  safe thresholds.
- USB serial/JTAG enumeration and reset/boot behaviour.
- Safe use of exposed GPIOs for any external controls or accessories.
