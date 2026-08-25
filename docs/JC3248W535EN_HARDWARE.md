# JC3248W535EN hardware reference

This document separates the board info used by lyra-firmware from hardware behaviour
that still requires a manual check. The compile-time pin info is
[`board/JC3248W535EN/include/lyra_board_pins.h`](../board/JC3248W535EN/include/lyra_board_pins.h).
Use the [firmware reference](EMOTIVATE_LYRA_OS_SPEC_JC3248W535EN.md) for the
software design and supported device behaviour.

## Board profile

| Area | Specification |
|---|---|
| Module | ESP32-S3-WROOM-1, dual-core LX7 up to 240 MHz |
| Display | 320 × 480 RGB565 AXS15231B panel over QSPI |
| Touch | AXS15231B I2C interface; the firmware uses polling |
| Memory profile | 16 MB flash and 8 MB PSRAM configured by default |
| Removable storage | MicroSD socket using 1-bit SDMMC |
| Audio | I2S output to the onboard speaker/amplifier path |
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
| MicroSD SPI CS reference | 10 | Not used by the SDMMC configuration |
| I2S BCLK / LRCLK / data out | 42 / 2 / 41 | Onboard speaker path |
| Battery sense | 5 | Divider calibration required |

No LCD reset, LCD DC, touch reset, or I2S MCLK GPIO is defined for this
board profile.

## Hardware validation required

These checks cannot be established by source inspection and should be
recorded from the physical development kit:

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
