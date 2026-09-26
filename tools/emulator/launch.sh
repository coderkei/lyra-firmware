#!/usr/bin/env sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if command -v python3 >/dev/null 2>&1; then
  python=python3
else
  printf '%s\n' 'Python 3 is required to run the emulator.' >&2
  exit 1
fi

exec "$python" "$script_dir/scripts/emulator.py" "$@"
