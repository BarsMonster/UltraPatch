#!/usr/bin/env bash
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

# Golden-wire regression: encodes a fixed set of representative pairs and compares blob
# sha256 against the committed manifest. Catches ANY wire drift — including size-neutral
# encoding changes the size gates cannot see — and enforces the end-of-development wire
# freeze: once frozen, a mismatch here means an illegal wire change.
#
# On an INTENDED wire change, regenerate the manifest in the same commit:  make golden-update
#
# Usage: make check-golden (or set ULTRAPATCH to an executable and run this script directly)
set -eu

: "${ULTRAPATCH:?check_golden.sh: ULTRAPATCH not set; invoke through make check-golden}"
[ -x "$ULTRAPATCH" ] || {
  echo "check_golden.sh: ULTRAPATCH is missing or not executable: $ULTRAPATCH" >&2
  exit 2
}

MODE="${1:-check}"
FIX="${FIXTURES:-test-bench/fixtures}"
IMG="${IMAGES:-test-bench/images}"
MANIFEST="${GOLDEN_MANIFEST:-test-bench/golden.sha256}"

. "$(dirname "$0")/tempdir.sh"

# Two SYNTHETIC wire-surface pins the six in-repo blobs never exercise: a journal-BUDGET-degraded
# blob (over-budget read-after-overwrite shipped as plain extras) and an UNNATURAL-DIRECTION blob
# (apply direction flipped, signaled by an overlong size-delta uLEB). Generated deterministically
# by the shared scripts/synth_gen.py (fixed-seed LCG; check_degrade.sh requests the SAME two named
# pins, so both gates see identical pairs) — no committed binaries. Purely additive: the six
# in-repo hash lines stay byte-identical (verify in the git diff on any manifest update).
SFIX="$tmp/synthfix"
python3 "$(dirname "$0")/synth_gen.py" pins "$SFIX"

# name / from / to — one-face product patches both ways + small/large corpus pairs both ways,
# then the two synthetic degradation pins (appended -> they sort last; existing lines untouched).
while read -r name from to; do
  [ -n "$name" ] || continue
  "$ULTRAPATCH" "$from/watch.bin" "$to/watch.bin" "$tmp/$name.blob" >/dev/null
done <<EOF
oneface_grow $FIX/v0_base $FIX/v1_one_face
oneface_revert $FIX/v1_one_face $FIX/v0_base
img00_to_img15 $IMG/img_00_n3 $IMG/img_15_n83
img15_to_img00 $IMG/img_15_n83 $IMG/img_00_n3
img04_to_img02 $IMG/img_04_n24 $IMG/img_02_n14
img07_to_img08 $IMG/img_07_n40 $IMG/img_08_n46
synth_journal_degrade $SFIX/synth_journal_degrade_from $SFIX/synth_journal_degrade_to
synth_unnatural_dir $SFIX/synth_unnatural_dir_from $SFIX/synth_unnatural_dir_to
EOF

(cd "$tmp" && for f in *.blob; do
  printf '%s %s %s\n' "$(sha256sum "$f" | cut -d' ' -f1)" "$(wc -c < "$f")" "$f"
done | sort -k3) > "$tmp/actual"

if [ "$MODE" = update ]; then
  # Surface the wire delta before overwriting: identical => size/format-only change.
  if diff -u "$MANIFEST" "$tmp/actual"; then echo "no wire change"; fi
  cp "$tmp/actual" "$MANIFEST"
  echo "golden manifest updated: $MANIFEST"
elif diff -u "$MANIFEST" "$tmp/actual" >&2; then
  echo "golden_wire=OK ($(wc -l < "$MANIFEST") blobs)"
else
  echo "golden_wire=MISMATCH — wire changed. If intended, run 'make golden-update' and commit the manifest together with the change." >&2
  exit 1
fi
