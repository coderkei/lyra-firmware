# Contributing to Lyra OS

Patches are welcome for things like:

- playback and library bugs
- GUI performance, as long as the themes still look the way they should
- translation and keyboard/input-method fixes
- documentation
- ports to specific boards
- audio stability improvements, ideally with measurements to back them up

## Building the reference firmware

Lyra is developed on the JC3248W535EN ESP32-S3 kit. Build it with ESP-IDF
6.0.2 from an activated ESP-IDF shell:

```powershell
idf.py set-target esp32s3
idf.py build
```

To flash a board and watch its log, pass its serial port, for example
`idf.py -p COMx flash monitor`. Whoever has the board does the building,
flashing and testing on it. The [hardware reference](docs/JC3248W535EN_HARDWARE.md)
has the wiring and the physical checks for the kit. The
[browser emulator](tools/emulator/README.md) runs the real firmware without
hardware, but you'll need the board to hear audio.

If you already have a local `sdkconfig`, it keeps its own values when
`sdkconfig.defaults` changes, so check menuconfig (or regenerate `sdkconfig`)
after pulling configuration changes.

## Checks to run

Run whichever checks apply to your change. For anything that touches
interface text or fonts:

```powershell
py -3 tools/i18n/check_i18n.py
py -3 tools/i18n/check_font_manifest.py
```

New or changed interface text has to be added to the translation catalog in
every supported language in the same change. The font check expects the
checked-in IME dictionary files to be present.

The build treats warnings as errors, so give your diff a careful read for
missing includes, mismatched types and wrong function signatures before you
hand it over.

## What to say in a pull request

Tell us how the change was checked, and be precise, because these are
different things:

- **hardware tested:** run on a real board
- **emulator tested:** run in the browser emulator
- **build-only:** it compiles
- **source reviewed:** read through but not built

Where it's relevant, include the board model and revision, the peripherals
involved, the firmware commit and steps to reproduce. Keep what you read in
the source separate from what you saw on hardware.

Please mention anything your change might affect in audio continuity,
playlist or catalog compatibility, theme appearance, multilingual input or
touch navigation, and avoid changing those as a side effect of unrelated
cleanup.

If you add code, fonts, dictionaries or images from somewhere else, say where
they came from: the upstream source, revision, licence, attribution
requirements and any steps you used to generate or modify them. Update
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) to match. The README's
developer note explains how AI assistance is disclosed in this project;
please read it before proposing any change to that.

## Reporting issues

There are templates for bug reports and new board ports; use them where they
fit. A good report has the firmware commit, exact steps to reproduce, and
what you tried (build, emulator or hardware). If Lyra restarted after the
problem, attach the crash report from `.lyra/crash/` on the card. Board
reports should name the board revision, display and touch controllers, audio
output, USB wiring, memory configuration and what you tested.
