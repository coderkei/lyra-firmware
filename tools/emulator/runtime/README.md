# Emulator runtime source

The Windows and Linux launchers use the prebuilt `app/wasm/esp32sim.wasm.gz`. Normal GUI
changes are read from `lyra_firmware_merged.bin` each time the emulator starts;
the WASM runtime does not contain a copy of the Lyra UI.

The runtime is based on [`joakimeriksson/esp32sim`](https://github.com/joakimeriksson/esp32sim)
at commit `4ab7e900fee998d0137c2c57d59de9d05339d093`. `esp32sim-lyra.patch`
adds the Lyra JC3248W535EN panel and touch controller, an SDHC card and DMA
model, and the browser ABI needed to insert, update, and persist sector images
and internal flash. `esp32sim-storage.patch` adds dirty-sector/page tracking
and the WebAssembly worker messages used for incremental storage saves.
`esp32sim-controls.patch` adds worker pause and resume controls with a
single-threaded execution scheduler.

## Rebuild the runtime

Runtime rebuilds are only needed when changing the emulator hardware model or
WASM engine. They need Git, Rust/Cargo and the `wasm32-unknown-unknown` target.
From this directory, install the WASM target and run the platform's build script:

```powershell
rustup target add wasm32-unknown-unknown
..\scripts\build-runtime.ps1
```

On Linux, use:

```sh
rustup target add wasm32-unknown-unknown
bash ../scripts/build-runtime.sh
```

Both scripts fetch the pinned source into a temporary directory, apply the
three patches, build the WASM module, and write the compressed runtime and worker
support files into `../app/wasm`. The emulator launcher itself needs only
Python, a supported browser, and the firmware `.bin`.
