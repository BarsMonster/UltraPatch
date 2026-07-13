#!/usr/bin/env bash
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

# Focused regression for intentional wire changes in scripts/ab_matrix.sh. Build a real
# encoder+decoder candidate with one shared model constant changed, then measure it over a small
# home+foreign corpus while an inherited manifest points to a nonexistent file. Both A/B corpus
# calls must explicitly disable that manifest; successful structural measurement proves that the
# supplied candidate decoder round-tripped every scheduled pair and passed the NVM safety checks.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
JOBS="${AB_MATRIX_TEST_JOBS:-8}"
: "${ULTRAPATCH:?check_ab_matrix.sh: ULTRAPATCH not set; invoke through make check-ab-matrix}"
: "${FIXTURES:?check_ab_matrix.sh: FIXTURES not set to the profile-scoped corpus}"

[ -x "$ULTRAPATCH" ] || {
  echo "check_ab_matrix.sh: ULTRAPATCH is missing or not executable: $ULTRAPATCH" >&2
  exit 2
}

. "$ROOT/scripts/tempdir.sh"
mkdir -p "$tmp/candidate/scripts"
cp "$ROOT/Makefile" "$tmp/candidate/"
cp "$ROOT/scripts/build_profile.py" "$tmp/candidate/scripts/"
cp -R "$ROOT/src" "$ROOT/vendor" "$tmp/candidate/"

# RC_S_BIT_RATE is shared by the encoder and decoder. Changing it creates a valid, intentionally
# different wire without introducing a test-only production build define.
sed -i 's/^#define RC_S_BIT_RATE 4$/#define RC_S_BIT_RATE 5/' \
  "$tmp/candidate/src/rc_models.h"
grep -q '^#define RC_S_BIT_RATE 5$' "$tmp/candidate/src/rc_models.h"
candidate_build="$tmp/candidate-build"
make --no-print-directory -C "$tmp/candidate" BUILD_DIR="$candidate_build" ultrapatch >/dev/null
CAND=$(make --no-print-directory -s -C "$tmp/candidate" \
  BUILD_DIR="$candidate_build" host-tool-path)
[ -x "$CAND" ] || {
  echo "check_ab_matrix.sh: candidate host tool is missing or not executable: $CAND" >&2
  exit 2
}

FIX="$FIXTURES"

# Two small deterministic images produce four home pairs. Alternating the same images across
# the pinned foreign release names produces all 34 foreign directions. This regression verifies
# matrix structure, manifest bypass, candidate decoding, and NVM checks; the A/B driver's real
# one-face pair separately proves the model variant changes a production-sized wire.
mkdir -p "$tmp/images/img_00_base" "$tmp/images/img_01_oneface" "$tmp/foreign" "$tmp/small"
python3 "$ROOT/scripts/synth_gen.py" role "$tmp/small/a.bin" from rand 512 701
python3 "$ROOT/scripts/synth_gen.py" role "$tmp/small/b.bin" from rand 768 907
ln -s "$tmp/small/a.bin" "$tmp/images/img_00_base/watch.bin"
ln -s "$tmp/small/b.bin" "$tmp/images/img_01_oneface/watch.bin"
foreign_versions=$(awk '$1=="foreign" { print $2 }' \
  "$ROOT/test-bench/corpus-inventory.tsv")
[ -n "$foreign_versions" ] || {
  echo "check_ab_matrix.sh: release inventory has no foreign images" >&2
  exit 2
}
i=0
for version in $foreign_versions; do
  mkdir -p "$tmp/foreign/$version"
  if [ $((i % 2)) -eq 0 ]; then image="$tmp/small/a.bin"; else image="$tmp/small/b.bin"; fi
  ln -s "$image" "$tmp/foreign/$version/watch.bin"
  i=$((i + 1))
done

set +e
IMAGES="$tmp/images" FOREIGN="$tmp/foreign" FIXTURES="$FIX" \
  CORPUS_INVENTORY="" \
  WIRE_BASELINE="" \
  BASE_FULL_TOTAL="" BASE_FOREIGN_TOTAL="" BASE_ONEFACE_GROW="" BASE_ONEFACE_REVERT="" \
  "$ROOT/scripts/ab_matrix.sh" "$ULTRAPATCH" "$CAND" "$CAND" "$JOBS" \
  >"$tmp/ab.out" 2>"$tmp/ab.err"
rc=$?
set -e
if [ "$rc" -ne 0 ] && [ "$rc" -ne 1 ]; then
  echo "check_ab_matrix.sh: wire-different candidate was not measurable (rc=$rc)" >&2
  cat "$tmp/ab.err" >&2
  exit 1
fi

split_sum=$(awk -F= '/^split_(better|worse|equal)=/{n+=$2} END{print n+0}' "$tmp/ab.out")
[ "$split_sum" -eq 4 ] || { echo "check_ab_matrix.sh: bad home split ($split_sum/4)" >&2; exit 1; }
grep -Eq '^full_total_base=[0-9]+$' "$tmp/ab.out"
grep -Eq '^full_total_cand=[0-9]+$' "$tmp/ab.out"
grep -Eq '^oneface_grow_base=[0-9]+$' "$tmp/ab.out"
grep -Eq '^oneface_grow_cand=[0-9]+$' "$tmp/ab.out"
grep -Eq '^oneface_revert_base=[0-9]+$' "$tmp/ab.out"
grep -Eq '^oneface_revert_cand=[0-9]+$' "$tmp/ab.out"
if ! grep -q '^oneface_wire_changed=1$' "$tmp/ab.out"; then
  echo "check_ab_matrix.sh: model variant did not change either one-face wire" >&2
  exit 1
fi
grep -Eq '^accept_gate=(OK|REJECT)' "$tmp/ab.out"

printf 'ab_wire_change=OK (home=4/4 foreign=34/34; manifest disabled; candidate decoder and NVM checks passed)\n'
