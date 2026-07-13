#!/usr/bin/env bash
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

# Golden-wire regression for the four wires not already covered by the full corpus baseline.
#
# On an INTENDED wire change, regenerate the manifest in the same commit:  make golden-update
#
# Usage: make check-golden (direct runs must set ULTRAPATCH and profile-scoped FIXTURES)
set -eu

: "${ULTRAPATCH:?check_golden.sh: ULTRAPATCH not set; invoke through make check-golden}"
: "${FIXTURES:?check_golden.sh: FIXTURES not set to the profile-scoped corpus}"
[ -x "$ULTRAPATCH" ] || {
  echo "check_golden.sh: ULTRAPATCH is missing or not executable: $ULTRAPATCH" >&2
  exit 2
}

MODE="${1:-check}"
FIX="$FIXTURES"
BASELINE="${WIRE_BASELINE:-test-bench/wire-baseline.tsv}"
GOLDEN_DUMP="${GOLDEN_DUMP:-}"

. "$(dirname "$0")/tempdir.sh"

# Two SYNTHETIC wire-surface pins the corpus blobs never exercise: a journal-BUDGET-degraded
# blob (over-budget read-after-overwrite shipped as plain extras) and an UNNATURAL-DIRECTION blob
# (apply direction flipped, signaled by an overlong size-delta uLEB). Generated deterministically
# by the shared scripts/synth_gen.py (fixed-seed LCG; check_degrade.sh requests the SAME two named
# pins, so both gates see identical pairs) — no committed binaries.
SFIX="$tmp/synthfix"
python3 "$(dirname "$0")/synth_gen.py" pins "$SFIX"

# name / from / to — one-face product patches both ways plus the two synthetic degradation pins.
while read -r name from to; do
  [ -n "$name" ] || continue
  "$ULTRAPATCH" "$from/watch.bin" "$to/watch.bin" "$tmp/$name.blob" >/dev/null
done <<EOF
oneface_grow $FIX/v0_base $FIX/v1_one_face
oneface_revert $FIX/v1_one_face $FIX/v0_base
synth_journal_degrade $SFIX/synth_journal_degrade_from $SFIX/synth_journal_degrade_to
synth_unnatural_dir $SFIX/synth_unnatural_dir_from $SFIX/synth_unnatural_dir_to
EOF

for name in oneface_grow.blob oneface_revert.blob \
            synth_journal_degrade.blob synth_unnatural_dir.blob; do
  printf 'G\t%s\t-\t%s\t%s\n' "$name" "$(wc -c < "$tmp/$name")" \
    "$(sha256sum "$tmp/$name" | cut -d' ' -f1)"
done > "$tmp/actual"

if [ "$MODE" = update ]; then
  [ -n "$GOLDEN_DUMP" ] || { echo "check_golden.sh: GOLDEN_DUMP is required for update" >&2; exit 2; }
  cp "$tmp/actual" "$GOLDEN_DUMP"
  echo "golden candidate written: $GOLDEN_DUMP"
elif grep '^G[[:space:]]' "$BASELINE" | diff -u - "$tmp/actual" >&2; then
  echo "golden_wire=OK ($(wc -l < "$tmp/actual") blobs)"
else
  echo "golden_wire=MISMATCH — wire changed. If intended, run 'make golden-update' and commit the manifest together with the change." >&2
  exit 1
fi
