# Contributing to Lyra OS

Patches are welcome for playback and catalog bugs, GUI performance that preserves theme visuals, language and IME corrections, documentation, concrete board ports, and audio stability improvements backed by measurements.

## Reference configuration

The current documented target is the JC3248W535EN ESP32-S3 development kit. Build it with ESP-IDF 6.0.2 from an activated ESP-IDF shell:

```powershell
idf.py set-target esp32s3
idf.py build
```

To flash and monitor a board, use its selected serial port, for example `idf.py -p COMx flash monitor`. The user or contributor running the board owns all build, flash, and device validation. The [hardware reference](docs/JC3248W535EN_HARDWARE.md) contains the wiring and physical checks for the reference kit. The [browser emulator guide](tools/emulator/README.md) explains how to run firmware in the emulator; use the development board to evaluate audible I2S playback.

Run the relevant source checks for a change. Localisation and embedded font checks are:

```powershell
py -3 tools/i18n/check_i18n.py
py -3 tools/i18n/check_font_manifest.py
```

The font check expects the checked-in generated IME dictionary assets. Firmware build, emulator, and physical tests are distinct evidence; report exactly which were performed.

## Change reports

For each patch, identify the evidence gathered: **hardware tested**, **emulator tested**, **build-only**, or **source reviewed**. Include board model and revision, relevant peripherals, firmware commit, and reproduction steps when applicable. Report source inspection and hardware results separately.

Please call out effects on audio continuity, playlist and catalog compatibility, theme appearance, multilingual input, or responsive touch navigation. Preserve these behaviors during unrelated cleanup.

For new code, fonts, dictionaries, or image assets, include their upstream source, revision, license, attribution requirements, and any generation or modification steps. Keep third-party notices and data provenance current. Follow the repository's existing disclosure practice; review it before proposing any additional AI-assistance policy.

## Issue reports

Use the repository's Bug report and New board port templates where they fit. A useful report includes a firmware commit, exact reproduction steps, and build/emulator/hardware results. Board reports should identify the board revision, display and touch controllers, I2S codec/output, memory configuration, and test results.
