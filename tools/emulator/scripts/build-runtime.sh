#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
emulator_root=$(cd -- "$script_dir/.." && pwd)
runtime_root="$emulator_root/runtime"
app_wasm_root="$emulator_root/app/wasm"
revision=4ab7e900fee998d0137c2c57d59de9d05339d093
rust_flags='-Cllvm-args=-inline-threshold=2000 -Clink-arg=--export-table -Clink-arg=--growable-table'
build_root=$(mktemp -d "${TMPDIR:-/tmp}/lyra-esp32sim-build-XXXXXXXX")
trap 'rm -rf -- "$build_root"' EXIT
source_root="$build_root/esp32sim"

for tool in git cargo gzip; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    printf 'Required tool not found: %s\n' "$tool" >&2
    exit 1
  fi
done

git init "$source_root"
git -C "$source_root" config core.autocrlf false
git -C "$source_root" remote add origin https://github.com/joakimeriksson/esp32sim.git
git -C "$source_root" fetch --depth 1 origin "$revision"
git -C "$source_root" checkout --detach FETCH_HEAD
git -C "$source_root" apply --check "$runtime_root/esp32sim-lyra.patch"
git -C "$source_root" apply "$runtime_root/esp32sim-lyra.patch"
git -C "$source_root" apply --check "$runtime_root/esp32sim-storage.patch"
git -C "$source_root" apply "$runtime_root/esp32sim-storage.patch"
git -C "$source_root" apply --check "$runtime_root/esp32sim-controls.patch"
git -C "$source_root" apply "$runtime_root/esp32sim-controls.patch"

(cd "$source_root" && RUSTFLAGS="$rust_flags" cargo build --release --target wasm32-unknown-unknown -p esp32sim-wasm)

for file in worker.js jit.mjs pacing.mjs experiments.mjs; do
  cp "$source_root/web/wasm/$file" "$app_wasm_root/$file"
done
gzip -n -c "$source_root/target/wasm32-unknown-unknown/release/esp32sim_wasm.wasm" > "$app_wasm_root/esp32sim.wasm.gz"
printf 'Runtime rebuilt: %s\n' "$app_wasm_root/esp32sim.wasm.gz"
