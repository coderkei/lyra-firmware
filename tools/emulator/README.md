# Lyra firmware emulator

On Windows, run `Open Lyra GUI emulator.cmd`. On Linux, run `bash launch.sh`.
Both launchers open the same local browser emulator and load
`lyra_firmware_merged.bin` from the repository root when it is present. If the
image is missing, the emulator opens and waits for you to choose a merged `.bin`
with **Open .bin**. You can pass a different merged `.bin` as an argument to
either launcher; on Windows, you can also drag it onto the `.cmd` file. The
browser reads the image locally; it is not uploaded.

The launchers share `scripts/emulator.py` and need Python 3.8 or later. The
server and launcher use only the Python standard library. Windows uses Python's
`py -3` launcher when available; Linux uses `python3`.

The hidden `.build-src` and `.tools` folders are local build caches: the former
is the patched esp32sim source and build tree, and the latter contains a
Windows Rust/Cargo toolchain. They were used for a local runtime build, but are
not needed to run the emulator. The rebuild scripts use the pinned source and
the host's installed Rust toolchain instead. These folders are ignored by Git.

The emulator executes the ESP32-S3 image in a bundled WebAssembly machine and
renders the actual 320 × 480 frames written by the firmware. Click and drag on
the display to send touch input to the emulated AXS15231B controller. The .bin
is the firmware input, so firmware UI changes appear on the next run without
editing an emulator screen model. The emulator's **Restart** button and the
firmware's own reboot both perform an ESP32-S3 reset in place, retaining NVS
and virtual MicroSD contents. The SDMMC controller is reinitialized so the
card mounts again after either reset. Reopening the same firmware image restores
its saved flash; choosing a `.bin` or changing the auto-loaded image initializes
flash from that image.

**Manage Flash** opens a modal for the firmware's `nvs`, `otadata`, `ota_0`,
`ota_1`, and `littlefs` partitions. Select a partition to clear it to erased
flash (`0xFF`), export its raw bytes as `.bin` or `.img`, or import a raw image
of the partition's exact size. A partition change restarts the firmware from
the updated saved flash. **Reset full 16 MiB flash** rebuilds all internal flash
from the currently loaded merged firmware image, clearing saved NVS, OTA, and
LittleFS state for a fresh firmware setup. The panel can also export or import
the complete 16 MiB flash as a raw `.bin` or `.img`; imported full-flash images
must be exactly 16 MiB. The virtual MicroSD is retained.

Use **Fit** to scale the display to the available preview width or **1×** to
map each display pixel to one physical screen pixel, accounting for browser
zoom and operating system display scaling. The scale choice is remembered in
the browser. **Capture PNG** saves the current framebuffer, and **Pause** /
**Resume** freezes and continues firmware execution without resetting it. The
preview reports emulation speed, CPU throughput, and clock resynchronizations.
Use **Save log** in the serial console to download the recent emulator and
firmware output.

## Virtual MicroSD

The firmware sees a 256 MiB FAT16 card through its SDMMC driver. The bundled
image contains three short, synthetic WAV samples so its music library has
files to scan. **Manage MicroSD** opens a modal with a card preview and FAT16
file manager for uploads, folders, and deletion. Firmware sector writes and
file manager changes are saved to `storage/virtual-sd.img` in this emulator
folder. **Save card** exports an image; **Open card image** replaces the
persistent card. Restart firmware after changing the inserted card.

## Requirements and limits

- Windows 10/11 or Linux with Python 3.8 or later and a current Edge, Chrome,
  Chromium, or Firefox browser.
- No ESP-IDF, Rust, Node.js, or .NET installation is required to run it.
- The launcher uses a loopback-only local server for the browser's WebAssembly
  worker and keeps it in the visible launcher command window. Leave that window
  open while using the emulator; press **Ctrl+C** there to stop the server.
  Closing the browser tab alone leaves the server running.
- The 16 MiB internal flash image, including NVS and OTA partitions, is saved
  to `storage/internal-flash.bin` in this emulator folder. Flash and MicroSD
  writes are saved while the firmware runs and survive closing the browser.
- Screen, touch, and card storage are emulated. The firmware's I²S audio output
  is not routed to Windows speakers.

The WebAssembly runtime is based on [esp32sim](https://github.com/joakimeriksson/esp32sim),
an MIT-licensed ESP32-S3 emulator. Its source revision, local board/SDMMC patch,
and rebuild steps are in `runtime/README.md`.
