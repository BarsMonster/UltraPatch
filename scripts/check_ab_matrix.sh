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
UP="$ROOT/ultrapatch"
JOBS="${AB_MATRIX_TEST_JOBS:-8}"

[ -x "$UP" ] || { echo "check_ab_matrix.sh: missing $UP; invoke via 'make check-ab-matrix'" >&2; exit 1; }

. "$ROOT/scripts/tempdir.sh"
mkdir -p "$tmp/candidate"
cp "$ROOT/Makefile" "$tmp/candidate/"
cp -R "$ROOT/src" "$ROOT/vendor" "$tmp/candidate/"

# RC_S_BIT_RATE is shared by the encoder and decoder. Changing it creates a valid, intentionally
# different wire without introducing a test-only production build define.
sed -i 's/^#define RC_S_BIT_RATE 4$/#define RC_S_BIT_RATE 5/' \
  "$tmp/candidate/src/rc_models.h"
grep -q '^#define RC_S_BIT_RATE 5$' "$tmp/candidate/src/rc_models.h"
make --no-print-directory -C "$tmp/candidate" ultrapatch >/dev/null
CAND="$tmp/candidate/ultrapatch"

FIX="$ROOT/test-bench/fixtures"
"$UP" "$FIX/v0_base/watch.bin" "$FIX/v1_one_face/watch.bin" "$tmp/base.blob" >/dev/null
"$CAND" "$FIX/v0_base/watch.bin" "$FIX/v1_one_face/watch.bin" "$tmp/candidate.blob" >/dev/null
if cmp -s "$tmp/base.blob" "$tmp/candidate.blob"; then
  echo "check_ab_matrix.sh: model variant did not change the probe wire" >&2
  exit 1
fi
cp "$FIX/v0_base/watch.bin" "$tmp/candidate.mem"
"$CAND" --decode "$tmp/candidate.mem" "$tmp/candidate.blob" >/dev/null 2>&1
cmp -s "$tmp/candidate.mem" "$FIX/v1_one_face/watch.bin"

# Two real firmware images produce four home pairs. Alternating the same images across the pinned
# foreign release names produces all 34 foreign directions without paying for the large corpus.
mkdir -p "$tmp/images/img_00_base" "$tmp/images/img_01_oneface" "$tmp/foreign"
ln -s "$FIX/v0_base/watch.bin" "$tmp/images/img_00_base/watch.bin"
ln -s "$FIX/v1_one_face/watch.bin" "$tmp/images/img_01_oneface/watch.bin"
foreign_versions="2.2.0 2.2.1 2.2.2 2.2.3 2.2.4 2.3.0 2.3.1 3.0.0 3.0.1 3.0.2 3.0.3 10.0.0 10.0.1 10.0.2 10.0.3 10.1.1 10.1.2 10.1.3"
i=0
for version in $foreign_versions; do
  mkdir -p "$tmp/foreign/$version"
  if [ $((i % 2)) -eq 0 ]; then image="$FIX/v0_base/watch.bin"; else image="$FIX/v1_one_face/watch.bin"; fi
  ln -s "$image" "$tmp/foreign/$version/watch.bin"
  i=$((i + 1))
done

set +e
IMAGES="$tmp/images" FOREIGN="$tmp/foreign" FIXTURES="$FIX" \
  CORPUS_WIRE_MANIFEST="$tmp/manifest-must-not-be-read" \
  BASE_FULL_TOTAL="" BASE_FOREIGN_TOTAL="" BASE_ONEFACE_GROW="" BASE_ONEFACE_REVERT="" \
  "$ROOT/scripts/ab_matrix.sh" "$UP" "$CAND" "$CAND" "$JOBS" \
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
grep -Eq '^accept_gate=(OK|REJECT)' "$tmp/ab.out"

printf 'ab_wire_change=OK (home=4/4 foreign=34/34; manifest disabled; candidate decoder and NVM checks passed)\n'
