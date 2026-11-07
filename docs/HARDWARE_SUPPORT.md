# Hardware support

Lyra is meant to run on more than one board eventually, but right now there is
one: the JC3248W535EN. This page describes it and lists what you'd need to
look at to bring Lyra up on something else.

## The reference board

| Item | JC3248W535EN |
| --- | --- |
| Status | The board Lyra is developed and tested on |
| Processor | ESP32-S3 |
| Display | 320 × 480 portrait touchscreen, AXS15231B over QSPI |
| Memory | 16 MB flash, 8 MB PSRAM |
| Storage | microSD over 1-bit SDMMC |
| Audio | External PCM5102A I2S DAC, optionally mirrored to the on-board speaker |
| USB | One USB-C port on the chip's native USB, shared between serial/JTAG and USB file transfer |
| Pins and wiring | [JC3248W535EN hardware reference](JC3248W535EN_HARDWARE.md) and [`lyra_board_pins.h`](../board/JC3248W535EN/include/lyra_board_pins.h) |

We call this combination (320 × 480, 16 MB flash, 8 MB PSRAM) the
**LYRA-320** profile. A port to other hardware should pick its own display and
memory targets based on the features it needs, and confirm them on the real
device. The [hardware reference](JC3248W535EN_HARDWARE.md) also lists the
checks on this board that still need physical measurement.

## Things to work out for a new board

- **Processor and memory:** is there enough for the codecs, themes, artwork and
  features you want?
- **Display:** controller, bus, colour format, resolution, orientation, and how
  LVGL's flush maps onto it.
- **Touch and controls:** controller, coordinates, calibration, gestures, and
  any physical buttons.
- **Storage:** microSD (or other) wiring, filesystem, mounting, and how it
  copes with audio reads at the same time.
- **Audio:** I2S controller and pins, DAC or codec, output level, amplifier,
  channel format, and whether a speaker mirror makes sense.
- **USB:** native USB, a USB-to-serial chip, or both. This decides whether USB
  file transfer can work and whether it takes the serial port away while it's
  on.
- **Power:** battery, charger, safe shutdown, and how charge is reported.
- **Flash layout:** size, partition table, update slots, the crash-dump
  partition, and a recovery path that matches the firmware updater.
- **Real-world behaviour:** audio stability and GUI responsiveness with a
  large library, artwork and storage activity going on.

## Community ports

When someone gets Lyra running on another board and writes up how it was done
and tested, it goes here.

| Board and revision | Processor/display/audio | Firmware revision | Validation reported |
| --- | --- | --- | --- |
|  |  |  |  |
