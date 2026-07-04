#!/usr/bin/env bash
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

# Fast parallel corpus-matrix + foreign-lineage metrics for the A1 gate.
#
# Run from the repository root; needs ./hy_enc and ./hy_dec already built. Prints the gate
# metric lines so the Makefile (or a measurement run) can parse them.
#
# Two independent pair sets share ONE parallel pool so the whole leg costs one wall-clock, not
# the sum of two:
#   * home matrix : all 256 (from,to) pairs over the 16 SensorWatch images.
#   * foreign     : 17 adjacent CircuitPython releases, both directions = 34 pair-directions
#                   (a second, unrelated Cortex-M0+ lineage; see docs/foreign-firmware-study.md).
# The single slowest job by far is the foreign cross-major pair (3.0.x <-> 10.x, effectively
# unrelated programs, ~13 s, 150-175 KB blob). It is emitted FIRST so it starts in the opening
# scheduling wave and overlaps every other pair — longest-processing-time-first, so adding the
# foreign set barely moves the leg's wall time even though it is CPU-heavy.
#
# Each worker uses its OWN mktemp dir, which keeps the deterministic encoder free of cross-run
# contamination from shared output paths.
#
# Usage: check_corpus.sh [jobs]      (jobs defaults to nproc)
# Exit 3 on a structural error (wrong pair count, or a decode produced no parseable NVM metrics).
set -u
JOBS="${1:-$(nproc 2>/dev/null || echo 4)}"
IMG="${IMAGES:-test-bench/images}"
FIX="${FIXTURES:-test-bench/fixtures}"
FGN="${FOREIGN:-test-bench/foreign}"

# Foreign lineage, pinned release order; adjacent pairs are the update steps. Two contiguous
# families (2.2.x/2.3.x/3.0.x, then 10.0.x/10.1.x) joined by the cross-major 3.0.3 -> 10.0.0 jump.
FVERS="2.2.0 2.2.1 2.2.2 2.2.3 2.2.4 2.3.0 2.3.1 3.0.0 3.0.1 3.0.2 3.0.3 10.0.0 10.0.1 10.0.2 10.0.3 10.1.1 10.1.2 10.1.3"

# One pair: encode -> blob, decode a fresh copy of `from` in place, compare to `to`, emit one line
#   "<tag> <blobsize> <roundtrip_ok?> <journal_used> <amplified> <maxrowerase> <inversions>"
# tag is C (home matrix) or F (foreign) so the aggregator can total each set separately while
# taking NVM write-safety maxima across BOTH. (A single short printf is < PIPE_BUF, so parallel
# workers' lines never interleave.)
cm_work() {
  tag=$1; from=$2; to=$3
  d=$(mktemp -d)
  ./hy_enc "$from" "$to" "$d/p.blob" >/dev/null 2>&1
  sz=$(wc -c < "$d/p.blob")
  cp "$from/watch.bin" "$d/mem.bin"
  ./hy_dec "$d/mem.bin" "$d/p.blob" 1 >/dev/null 2>"$d/log"
  if cmp -s "$d/mem.bin" "$to/watch.bin"; then ok=1; else ok=0; fi
  j=$(sed -n 's/.*journal_used=\([0-9][0-9]*\).*/\1/p' "$d/log")
  v=$(sed -n 's/.*amplified=\([0-9][0-9]*\).*maxrowerase=\([0-9][0-9]*\).*inversions=\([-0-9][0-9]*\).*/\1 \2 \3/p' "$d/log")
  printf '%s %s %s %s %s\n' "$tag" "$sz" "$ok" "${j:-NA}" "${v:-NA NA NA}"
  rm -rf "$d"
}
export -f cm_work

# Foreign job stream: cross-major boundary pair (both directions) first, then every other
# adjacent pair (both directions). Boundary detected as major<=3 on one side, major>=10 on the
# other, so it stays correct if the pinned list is retuned.
foreign_jobs() {
  prev=""; cross=""; rest=""
  for v in $FVERS; do
    if [ -n "$prev" ]; then
      pmaj=${prev%%.*}; vmaj=${v%%.*}
      if { [ "$pmaj" -le 3 ] && [ "$vmaj" -ge 10 ]; } || { [ "$pmaj" -ge 10 ] && [ "$vmaj" -le 3 ]; }; then
        cross="$prev:$v"
      else
        rest="$rest $prev:$v"
      fi
    fi
    prev=$v
  done
  for p in $cross $rest; do
    f=${p%:*}; t=${p#*:}
    printf 'F\t%s\t%s\n' "$FGN/$f" "$FGN/$t"
    printf 'F\t%s\t%s\n' "$FGN/$t" "$FGN/$f"
  done
}

agg=$(
  {
    foreign_jobs
    for from in "$IMG"/img_*; do for to in "$IMG"/img_*; do printf 'C\t%s\t%s\n' "$from" "$to"; done; done
  } \
  | xargs -P "$JOBS" -L1 bash -c 'cm_work "$0" "$1" "$2"' \
  | awk '{ tag=$1; sz=$2; ok=$3; j=$4; amp=$5; row=$6; inv=$7;
           if(j=="NA"||amp=="NA"){bad++; next}
           if(tag=="C"){cfull+=sz; cok+=ok; cn++}
           else if(tag=="F"){ffull+=sz; fok+=ok; fn++}
           if(j>mj)mj=j; if(amp>ma)ma=amp; if(row>mr)mr=row; if(inv>mi)mi=inv }
         END{ printf "%d %d %d %d %d %d %d %d %d %d %d\n", cok,cfull,cn, fok,ffull,fn, mj,ma,mr,mi, bad+0 }'
)
read cok cfull cn fok ffull fn mj ma mr mi bad <<EOF
$agg
EOF
if [ "$cn" -ne 256 ] || [ "$fn" -ne 34 ] || [ "$bad" -ne 0 ]; then
  echo "check_corpus.sh: structural error (home=$cn/256 foreign=$fn/34 bad_parse=$bad)" >&2
  exit 3
fi

# real one-face firmware update (grow + revert) — two encodes, serial (negligible).
d=$(mktemp -d)
./hy_enc "$FIX/v0_base" "$FIX/v1_one_face" "$d/grow.blob" >/dev/null 2>&1
./hy_enc "$FIX/v1_one_face" "$FIX/v0_base" "$d/revert.blob" >/dev/null 2>&1
og=$(wc -c < "$d/grow.blob"); orv=$(wc -c < "$d/revert.blob")
rm -rf "$d"

printf 'matrix_ok=%d/256\nfull_total=%d\nforeign_ok=%d/34\nforeign_total=%d\nmax_journal=%d\nmax_amplified=%d\nmax_maxrowerase=%d\nmax_inversions=%d\noneface_grow=%d\noneface_revert=%d\n' \
  "$cok" "$cfull" "$fok" "$ffull" "$mj" "$ma" "$mr" "$mi" "$og" "$orv"
