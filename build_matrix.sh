#!/bin/bash
set -e

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ARMGCC=${ARMGCC:-"$ROOT/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi"}
SENSOR_WATCH_DIR=${SENSOR_WATCH_DIR:-"$ROOT/test-bench/Sensor-Watch"}
OUT=${OUT:-"$ROOT/artifacts/pic"}

export PATH="$ARMGCC/bin:$PATH"
cd "$SENSOR_WATCH_DIR/movement/make"
CFG="$SENSOR_WATCH_DIR/movement/movement_config.h"
mkdir -p "$OUT"

# config-name | CC | EXTRA_CFLAGS
CONFIGS=(
  "gcc-nopic|arm-none-eabi-gcc|"
  "gcc-pic|arm-none-eabi-gcc|-fPIC -msingle-pic-base -mno-pic-data-is-text-relative"
  "clang-nopic|$ROOT/cc-clang.sh|"
  "clang-pic|$ROOT/cc-clang.sh|-fPIC"
  "clang-ropi|$ROOT/cc-clang.sh|-fropi"
)

build() { # state(base|mod)
  local state=$1
  for c in "${CONFIGS[@]}"; do
    IFS='|' read -r name cc extra <<<"$c"
    local bdir=./build-$name-$state
    rm -rf "$bdir"
    make -s COLOR=PRO CC="$cc" BUILD="$bdir" EXTRA_CFLAGS="$extra" >/dev/null 2>>"$OUT/build.log" || { echo "FAIL $name/$state"; continue; }
    mkdir -p "$OUT/$name"
    cp "$bdir/watch.bin" "$OUT/$name/$state.bin"
    cp "$bdir/watch.elf" "$OUT/$name/$state.elf"
    printf "  built %-12s %-4s bin=%s\n" "$name" "$state" "$(stat -c%s "$bdir/watch.bin")"
  done
}

echo "### MODIFIED builds (counter_face present) ###"
build mod

echo "### toggling source to BASELINE (remove counter_face) ###"
sed -i '/^    counter_face,$/d' "$CFG"
grep -q "counter_face," "$CFG" && { echo "ERROR: still present"; exit 1; } || echo "  removed."

echo "### BASELINE builds ###"
build base

echo "### restoring source ###"
sed -i 's/^    stopwatch_face,$/    stopwatch_face,\n    counter_face,/' "$CFG"
grep -q "counter_face," "$CFG" && echo "  restored." || echo "ERROR restore failed"
