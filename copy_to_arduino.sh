#!/bin/bash
#------------------------------------------------------
# Copyright (c) 2026, Elehobica
# Released under the BSD-2-Clause
# refer to https://opensource.org/licenses/BSD-2-Clause
#------------------------------------------------------
#
# Copy the library sources into the Arduino library folder.
#
# The repository root holds the single source of truth for pico_battery_op.h / .cpp.
# arduino/pico_battery_op/src/ needs exact copies of them, so those copies are not tracked
# by git (see .gitignore); run this script to (re)generate them after editing the originals.
#
# The vendored pico-extras sources under arduino/pico_battery_op/src/pbo_vendor/ are NOT
# touched here: they are patched copies (pbov_ prefixed) and are tracked by git.
#
# Usage:
#   ./copy_to_arduino.sh

set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
DEST="$ROOT/arduino/pico_battery_op/src"

SOURCES=(
  pico_battery_op.h
  pico_battery_op.cpp
)

if [[ ! -d "$DEST" ]]; then
  echo "error: destination not found: $DEST" >&2
  exit 1
fi

for f in "${SOURCES[@]}"; do
  if [[ ! -f "$ROOT/$f" ]]; then
    echo "error: source not found: $ROOT/$f" >&2
    exit 1
  fi
  cp "$ROOT/$f" "$DEST/$f"
  echo "copied $f -> arduino/pico_battery_op/src/$f"
done

echo "done."
