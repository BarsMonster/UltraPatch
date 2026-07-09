#!/usr/bin/env bash
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

# Fast parallel corpus-matrix + foreign-lineage metrics for the A1 gate.
#
# Run from the repository root; needs ./ultrapatch already built. Prints the gate
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
# Exit 4 on a gate-assertion failure: matrix/foreign round-trip and NVM write-safety always, plus —
# only when the BASE_FULL_TOTAL / BASE_FOREIGN_TOTAL pins are set — the home per-pair and total-size
# ratchets. Those pins stay unset for a bare measurement run on a deliberately regressing candidate.
#
# Encoder/decoder overrides (default ./ultrapatch for both, so the gate path is unchanged):
#   ULTRAPATCH          encoder binary (accepts <from> <to> <patch>).
#   ULTRAPATCH_DECODE   decoder binary (accepts --decode <image> <patch>); defaults to $ULTRAPATCH.
# Home per-pair size handling (both default off / to the pinned baseline for the gate):
#   CORPUS_SIZE_BASELINE   TSV to split home pairs against; set to "" to skip the split entirely
#                          (bare per-pair measurement, e.g. when generating a fresh baseline).
#   CORPUS_SIZE_DUMP       if set, write the home per-pair "<from> <to> <size>" TSV to this path.
# Expected home/foreign pair counts are derived from the generated job stream, so a reduced
# IMAGES/FOREIGN subset still validates that every scheduled job produced a metric line.
set -u
JOBS="${1:-$(nproc 2>/dev/null || echo 4)}"
IMG="${IMAGES:-test-bench/images}"
FGN="${FOREIGN:-test-bench/foreign}"
SIZE_BASE="${CORPUS_SIZE_BASELINE-test-bench/home-size-baseline.tsv}"
UP="${ULTRAPATCH:-./ultrapatch}"
UPD="${ULTRAPATCH_DECODE:-$UP}"
DUMP="${CORPUS_SIZE_DUMP:-}"
export UP UPD

# Foreign lineage, pinned release order; adjacent pairs are the update steps. Two contiguous
# families (2.2.x/2.3.x/3.0.x, then 10.0.x/10.1.x) joined by the cross-major 3.0.3 -> 10.0.0 jump.
FVERS="2.2.0 2.2.1 2.2.2 2.2.3 2.2.4 2.3.0 2.3.1 3.0.0 3.0.1 3.0.2 3.0.3 10.0.0 10.0.1 10.0.2 10.0.3 10.1.1 10.1.2 10.1.3"

# One pair: encode -> blob, decode a fresh copy of `from` in place, compare to `to`, emit one line
#   "<tag> <from_id> <to_id> <blobsize> <roundtrip_ok?> <journal_used> <amplified> <maxrowerase> <inversions>"
# tag is C (home matrix) or F (foreign) so the aggregator can total each set separately while
# taking NVM write-safety maxima across BOTH. (A single short printf is < PIPE_BUF, so parallel
# workers' lines never interleave.)
cm_work() {
  tag=$1; from=$2; to=$3
  d=$(mktemp -d)
  # Clean this worker's dir on the 60 s-cap kill path too: a mid-matrix SIGTERM would
  # otherwise leak up to JOBS of these mktemp dirs. On TERM/INT clean up, restore the
  # default disposition, and re-raise so the worker dies by the signal.
  trap 'rm -rf "$d"' EXIT
  trap 'rm -rf "$d"; trap - TERM INT EXIT; kill -s TERM "$$"' TERM
  trap 'rm -rf "$d"; trap - TERM INT EXIT; kill -s INT "$$"' INT
  "$UP" "$from/watch.bin" "$to/watch.bin" "$d/p.blob" >/dev/null 2>&1
  sz=$(wc -c < "$d/p.blob")
  cp "$from/watch.bin" "$d/mem.bin"
  "$UPD" --decode "$d/mem.bin" "$d/p.blob" >/dev/null 2>"$d/log"
  if cmp -s "$d/mem.bin" "$to/watch.bin"; then ok=1; else ok=0; fi
  j=$(sed -n 's/.*journal_used=\([0-9][0-9]*\).*/\1/p' "$d/log")
  v=$(sed -n 's/.*amplified=\([0-9][0-9]*\).*maxrowerase=\([0-9][0-9]*\).*inversions=\([-0-9][0-9]*\).*/\1 \2 \3/p' "$d/log")
  printf '%s %s %s %s %s %s %s\n' "$tag" "${from##*/}" "${to##*/}" "$sz" "$ok" "${j:-NA}" "${v:-NA NA NA}"
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

if [ -n "$SIZE_BASE" ] && [ ! -f "$SIZE_BASE" ]; then
  echo "check_corpus.sh: missing home size baseline: $SIZE_BASE" >&2
  exit 3
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

{
  foreign_jobs
  for from in "$IMG"/img_*; do for to in "$IMG"/img_*; do printf 'C\t%s\t%s\n' "$from" "$to"; done; done
} > "$tmp/jobs.txt"
# Expected counts come from the scheduled jobs, so a reduced subset still catches a dropped worker.
EXP_HOME=$(grep -c '^C' "$tmp/jobs.txt")
EXP_FGN=$(grep -c '^F' "$tmp/jobs.txt")
xargs -P "$JOBS" -L1 bash -c 'cm_work "$0" "$1" "$2"' < "$tmp/jobs.txt" > "$tmp/pairs.txt"

# Optional per-pair home-size dump (used by ab_matrix.sh to build an A/B baseline from any encoder).
[ -n "$DUMP" ] && awk '$1=="C"{print $2"\t"$3"\t"$4}' "$tmp/pairs.txt" > "$DUMP"

agg=$(
  awk '{ tag=$1; sz=$4; ok=$5; j=$6; amp=$7; row=$8; inv=$9;
         if(j=="NA"||amp=="NA"){bad++; next}
         if(tag=="C"){cfull+=sz; cok+=ok; cn++}
         else if(tag=="F"){ffull+=sz; fok+=ok; fn++}
         if(j>mj)mj=j; if(amp>ma)ma=amp; if(row>mr)mr=row; if(inv>mi)mi=inv }
       END{ printf "%d %d %d %d %d %d %d %d %d %d %d\n", cok,cfull,cn, fok,ffull,fn, mj,ma,mr,mi, bad+0 }' "$tmp/pairs.txt"
)
read cok cfull cn fok ffull fn mj ma mr mi bad <<EOF
$agg
EOF
# Home per-pair split vs the baseline TSV — skipped (NA) when no baseline is provided.
if [ -n "$SIZE_BASE" ]; then
  split=$(
    awk '
      FNR==NR {
        if($0 ~ /^[[:space:]]*$/ || $1 ~ /^#/) next
        key=$1 SUBSEP $2
        base[key]=$3
        bcount++
        next
      }
      $1=="C" {
        key=$2 SUBSEP $3
        if(!(key in base)){missing++; next}
        seen[key]=1
        sz=$4 + 0
        ref=base[key] + 0
        if(sz < ref) better++
        else if(sz > ref) worse++
        else equal++
      }
      END {
        for(key in base) if(!(key in seen)) missing++
        printf "%d %d %d %d %d\n", better+0,worse+0,equal+0,missing+0,bcount+0
      }' "$SIZE_BASE" "$tmp/pairs.txt"
  )
  read hbetter hworse hequal hmissing hbase <<EOF
$split
EOF
else
  hbetter=NA; hworse=NA; hequal=NA; hmissing=0; hbase="$EXP_HOME"
fi
if [ "$cn" -ne "$EXP_HOME" ] || [ "$fn" -ne "$EXP_FGN" ] || [ "$bad" -ne 0 ] \
   || [ "$hbase" -ne "$EXP_HOME" ] || [ "$hmissing" -ne 0 ]; then
  echo "check_corpus.sh: structural error (home=$cn/$EXP_HOME foreign=$fn/$EXP_FGN baseline=$hbase/$EXP_HOME missing=$hmissing bad_parse=$bad)" >&2
  exit 3
fi

printf 'matrix_ok=%d/%d\nfull_total=%d\nhome_size_better=%s\nhome_size_worse=%s\nhome_size_equal=%s\nforeign_ok=%d/%d\nforeign_total=%d\nmax_journal=%d\nmax_amplified=%d\nmax_maxrowerase=%d\nmax_inversions=%d\n' \
  "$cok" "$EXP_HOME" "$cfull" "$hbetter" "$hworse" "$hequal" "$fok" "$EXP_FGN" "$ffull" "$mj" "$ma" "$mr" "$mi"

# ---- gate assertions (single source; the Makefile corpus leg just runs this script) ----
# Correctness / write-safety (unconditional; only a broken candidate, never a regression, trips it):
if [ "$cok" -ne "$EXP_HOME" ] || [ "$fok" -ne "$EXP_FGN" ] \
   || [ "$ma" -ne 0 ] || [ "$mr" -gt 1 ] || [ "$mi" -ne 0 ]; then
  echo "check_corpus.sh: correctness/write-safety gate (matrix=$cok/$EXP_HOME foreign=$fok/$EXP_FGN amplified=$ma maxrowerase=$mr inversions=$mi)" >&2
  exit 4
fi
# The split must account for every home pair (only meaningful when a baseline was supplied).
if [ -n "$SIZE_BASE" ]; then
  hsum=$((hbetter + hworse + hequal))
  if [ "$hsum" -ne "$EXP_HOME" ]; then
    echo "check_corpus.sh: correctness gate (split_sum=$hsum != $EXP_HOME)" >&2
    exit 4
  fi
fi
# Ratchets (skipped unless the pin is set, and the home ratchet needs a baseline split): home
# per-pair (zero worse) + total, and foreign total.
if [ -n "$SIZE_BASE" ] && [ -n "${BASE_FULL_TOTAL:-}" ] \
   && { [ "$hworse" -ne 0 ] || [ "$cfull" -gt "$BASE_FULL_TOTAL" ]; }; then
  echo "check_corpus.sh: home ratchet (worse=$hworse full_total=$cfull > BASE_FULL_TOTAL=$BASE_FULL_TOTAL)" >&2
  exit 4
fi
if [ -n "${BASE_FOREIGN_TOTAL:-}" ] && [ "$ffull" -gt "$BASE_FOREIGN_TOTAL" ]; then
  echo "check_corpus.sh: foreign ratchet (foreign_total=$ffull > BASE_FOREIGN_TOTAL=$BASE_FOREIGN_TOTAL)" >&2
  exit 4
fi
