#!/usr/bin/env bash
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

# Full-matrix qemu-arm apply leg: executes the decoder as REAL Thumb-1 code (the
# Cortex-M0+ ISA subset) under qemu-arm user-mode emulation for EVERY tracked pair —
# the 256 corpus matrix pairs (check_corpus.sh enumeration), the real one-face
# grow/revert fixtures, and the two synthetic golden pins (gen_synth_pins.py, shared
# with check_golden.sh) — encoding on the host and byte-comparing the qemu-patched
# image against the expected target. Matrix decodes use byte_mode (push adapter),
# mirroring check_corpus.sh; the one-face pair is ALSO decoded pull-mode plus a
# corrupt-body reject, preserving the old sample-level check-qemu exactly. Pairs are
# independent, so they run in parallel (private mktemp dir per worker, as in
# check_corpus.sh). Any failure names the (from,to) pair. Deterministic and offline.
# Auto-skips (successfully, with a message) without the cross-gcc or qemu-arm.
#
# Usage: check_qemu_matrix.sh [W] [jobs]      (W defaults to 10, jobs to nproc)
set -u
W="${1:-10}"
JOBS="${2:-$(nproc 2>/dev/null || echo 4)}"
IMG="${IMAGES:-test-bench/images}"
FIX="${FIXTURES:-test-bench/fixtures}"

if ! command -v arm-linux-gnueabi-gcc >/dev/null 2>&1 || ! command -v qemu-arm >/dev/null 2>&1; then
  echo "qemu_thumb_roundtrip=SKIPPED (need gcc-arm-linux-gnueabi + qemu-user)"; exit 0
fi

top=$(mktemp -d)
trap 'rm -rf "$top"' EXIT

arm-linux-gnueabi-gcc -static -mthumb -Os -std=c99 -Wall -Wextra -Werror \
  -DCORTEX_M0 -D_POSIX_C_SOURCE=200809L \
  -Isrc src/patch_apply_demo.c -o "$top/hy_dec_qemu" || exit 1
python3 "$(dirname "$0")/gen_synth_pins.py" "$top/synthfix" || exit 1

# One pair: host-encode -> blob, qemu-arm apply onto a fresh copy of `from`, byte-compare
# to `to`; emit "<ok> <from> <to>" (single short printf < PIPE_BUF, so lines never interleave).
qm_work() {
  from=$1; to=$2
  d=$(mktemp -d)
  ok=0
  ./hy_enc "$from" "$to" "$d/p.blob" "$QM_W" >/dev/null 2>&1 \
    && cp "$from/watch.bin" "$d/mem.bin" \
    && qemu-arm "$QM_DEC" "$d/mem.bin" "$d/p.blob" 1 >/dev/null 2>&1 \
    && cmp -s "$d/mem.bin" "$to/watch.bin" \
    && ok=1
  printf '%s %s %s\n' "$ok" "$from" "$to"
  rm -rf "$d"
}
export -f qm_work
export QM_W="$W" QM_DEC="$top/hy_dec_qemu"

lines=$(
  { for from in "$IMG"/img_*; do for to in "$IMG"/img_*; do printf '%s\t%s\n' "$from" "$to"; done; done; \
    printf '%s\t%s\n' "$FIX/v0_base" "$FIX/v1_one_face"; \
    printf '%s\t%s\n' "$FIX/v1_one_face" "$FIX/v0_base"; \
    printf '%s\t%s\n' "$top/synthfix/synth_journal_degrade_from" "$top/synthfix/synth_journal_degrade_to"; \
    printf '%s\t%s\n' "$top/synthfix/synth_unnatural_dir_from" "$top/synthfix/synth_unnatural_dir_to"; } \
  | xargs -P "$JOBS" -L1 bash -c 'qm_work "$0" "$1"'
)
n=$(printf '%s\n' "$lines" | wc -l)
okn=$(printf '%s\n' "$lines" | awk '{ ok+=$1 } END { print ok+0 }')
if [ "$n" -ne 260 ] || [ "$okn" -ne "$n" ]; then
  echo "check_qemu_matrix.sh: host-vs-qemu DIVERGENCE or structural error (pairs=$n ok=$okn/260)" >&2
  printf '%s\n' "$lines" | awk '$1==0 { printf "  QEMU-FAIL %s -> %s\n", $2, $3 }' >&2
  exit 1
fi

# Legacy sample-level checks (the old check-qemu body): one-face grow+revert decoded in
# PULL mode (no byte_mode arg), then a corrupt-body reject that must leave flash untouched.
d="$top/oneface"; mkdir -p "$d"
./hy_enc "$FIX/v0_base" "$FIX/v1_one_face" "$d/grow.blob" "$W" >/dev/null 2>&1
./hy_enc "$FIX/v1_one_face" "$FIX/v0_base" "$d/revert.blob" "$W" >/dev/null 2>&1
cp "$FIX/v0_base/watch.bin" "$d/mem.bin"
qemu-arm "$QM_DEC" "$d/mem.bin" "$d/grow.blob" >/dev/null 2>&1 \
  && cmp -s "$d/mem.bin" "$FIX/v1_one_face/watch.bin" \
  && qemu-arm "$QM_DEC" "$d/mem.bin" "$d/revert.blob" >/dev/null 2>&1 \
  && cmp -s "$d/mem.bin" "$FIX/v0_base/watch.bin" \
  || { echo "check_qemu_matrix.sh: QEMU-FAIL pull-mode one-face grow/revert" >&2; exit 1; }
cp "$d/grow.blob" "$d/bad.blob"
printf '\377' | dd of="$d/bad.blob" bs=1 seek=40 count=1 conv=notrunc >/dev/null 2>&1
cp "$FIX/v0_base/watch.bin" "$d/mem.bin"
if qemu-arm "$QM_DEC" "$d/mem.bin" "$d/bad.blob" >/dev/null 2>&1; then
  echo "check_qemu_matrix.sh: qemu corrupt body accepted" >&2; exit 1; fi
cmp -s "$d/mem.bin" "$FIX/v0_base/watch.bin" \
  || { echo "check_qemu_matrix.sh: qemu corrupt reject modified flash" >&2; exit 1; }

echo "qemu_matrix_ok=$okn/260"
echo "qemu_thumb_roundtrip=OK (260-pair matrix: 256 corpus + one-face both ways + 2 synthetic pins; + pull-mode one-face + corrupt reject)"
