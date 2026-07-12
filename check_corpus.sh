#!/usr/bin/env bash
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

# Fast parallel corpus-matrix + foreign-lineage metrics for the A1 gate.
#
# Run from the repository root through Make, or set ULTRAPATCH explicitly. Prints the gate
# metric lines so the Makefile (or a measurement run) can parse them.
#
# Two independent pair sets share ONE parallel pool so the whole leg costs one wall-clock, not
# the sum of two:
#   * home matrix : all 256 (from,to) pairs over the 16 SensorWatch images.
#   * foreign     : 17 explicitly pinned CircuitPython edges, both directions = 34 pair-directions
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
# Encoder/decoder paths:
#   ULTRAPATCH          required encoder binary (accepts <from> <to> <patch>).
#   ULTRAPATCH_DECODE   decoder binary (accepts --decode <image> <patch>); defaults to $ULTRAPATCH.
# Home per-pair size handling:
#   WIRE_BASELINE          combined C/F/G baseline; set to "" for an unpinned measurement
#   CORPUS_SIZE_BASELINE   optional three-column A/B override; set to "" to skip the split
#                          (bare per-pair measurement, e.g. when generating a fresh baseline).
#   CORPUS_SIZE_DUMP       if set, write the home per-pair "<from> <to> <size>" TSV to this path.
#   WIRE_BASELINE_DUMP     if set, write candidate C/F rows in the combined format.
# Expected home/foreign pair counts are derived from the generated job stream, so a reduced
# IMAGES/FOREIGN subset still validates that every scheduled job produced a metric line.
set -u
JOBS="${1:-$(nproc 2>/dev/null || echo 4)}"
IMG="${IMAGES:-test-bench/images}"
FGN="${FOREIGN:-test-bench/foreign}"
INVENTORY="${CORPUS_INVENTORY-test-bench/corpus-inventory.tsv}"
TOPOLOGY_HELPER="scripts/corpus_topology.py"
WIRE_BASE="${WIRE_BASELINE-test-bench/wire-baseline.tsv}"
SIZE_BASE="${CORPUS_SIZE_BASELINE-$WIRE_BASE}"
: "${ULTRAPATCH:?check_corpus.sh: ULTRAPATCH not set; invoke through make check-corpus}"
UP="$ULTRAPATCH"
UPD="${ULTRAPATCH_DECODE:-$UP}"
DUMP="${CORPUS_SIZE_DUMP:-}"
BASE_DUMP="${WIRE_BASELINE_DUMP:-}"
export UP UPD

[ -x "$UP" ] || { echo "check_corpus.sh: encoder is missing or not executable: $UP" >&2; exit 3; }
[ -x "$UPD" ] || { echo "check_corpus.sh: decoder is missing or not executable: $UPD" >&2; exit 3; }

# Release checks take their ordered home membership and explicit foreign edges from one parsed
# inventory. Bare A/B or custom measurements set CORPUS_INVENTORY="" and retain directory
# discovery plus adjacent foreign pairs for their synthetic subsets.
USE_TOPOLOGY=0
if [ -n "$INVENTORY" ]; then
  if [ ! -f "$INVENTORY" ]; then
    echo "check_corpus.sh: missing release inventory: $INVENTORY" >&2
    exit 3
  fi
  if [ ! -f "$TOPOLOGY_HELPER" ] || \
     ! python3 "$TOPOLOGY_HELPER" counts --inventory "$INVENTORY" >/dev/null; then
    echo "check_corpus.sh: malformed release inventory: $INVENTORY" >&2
    exit 3
  fi
  USE_TOPOLOGY=1
else
  HOME_IDS=$(for path in "$IMG"/img_*; do [ -d "$path" ] && basename "$path"; done | sort)
  FOREIGN_IDS=$(for path in "$FGN"/*; do [ -d "$path" ] && basename "$path"; done | sort -V)
  if [ -z "$HOME_IDS" ] || [ -z "$FOREIGN_IDS" ]; then
    echo "check_corpus.sh: empty home or foreign inventory" >&2
    exit 3
  fi
fi

# One pair: encode -> blob, decode a fresh copy of `from` in place, compare to `to`, emit one line
#   "<tag> <from_id> <to_id> <blobsize> <roundtrip_ok?> <journal_used> <amplified>
#    <maxpageerase> <inversions> <unaligned> <oob> <canary> <sha256>"
# tag is C (home matrix) or F (foreign) so the aggregator can total each set separately while
# taking NVM write-safety maxima across BOTH. (A single short printf is < PIPE_BUF, so parallel
# workers' lines never interleave.)
cm_work() {
  tag=$1; from=$2; to=$3
  d=$(mktemp -d)
  # Clean this worker's dir on the public-target time-cap kill path too: a mid-matrix SIGTERM would
  # otherwise leak up to JOBS of these mktemp dirs. On TERM/INT clean up, restore the
  # default disposition, and re-raise so the worker dies by the signal.
  trap 'rm -rf "$d"' EXIT
  trap 'rm -rf "$d"; trap - TERM INT EXIT; kill -s TERM "$$"' TERM
  trap 'rm -rf "$d"; trap - TERM INT EXIT; kill -s INT "$$"' INT
  "$UP" "$from/watch.bin" "$to/watch.bin" "$d/p.blob" >/dev/null 2>&1
  sz=$(wc -c < "$d/p.blob")
  hash=$(sha256sum "$d/p.blob")
  hash=${hash%% *}
  cp "$from/watch.bin" "$d/mem.bin"
  "$UPD" --decode "$d/mem.bin" "$d/p.blob" >/dev/null 2>"$d/log"
  if cmp -s "$d/mem.bin" "$to/watch.bin"; then ok=1; else ok=0; fi
  j=$(sed -n 's/.*journal_used=\([0-9][0-9]*\).*/\1/p' "$d/log")
  v=$(sed -n 's/.*amplified=\([0-9][0-9]*\).*maxpageerase=\([0-9][0-9]*\).*inversions=\([-0-9][0-9]*\).*unaligned=\([0-9][0-9]*\).*oob=\([0-9][0-9]*\).*canary=\([0-9][0-9]*\).*/\1 \2 \3 \4 \5 \6/p' "$d/log")
  printf '%s %s %s %s %s %s %s %s\n' "$tag" "${from##*/}" "${to##*/}" "$sz" "$ok" "${j:-NA}" "${v:-NA NA NA NA NA NA}" "$hash"
  rm -rf "$d"
}
export -f cm_work

# Custom-measurement fallback: cross-major boundary pair (both directions) first, then every
# other adjacent discovered pair. Release scheduling uses the inventory's explicit ordered edges.
foreign_jobs() {
  prev=""; cross=""; rest=""
  for v in $FOREIGN_IDS; do
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
if [ -n "$WIRE_BASE" ] && [ ! -f "$WIRE_BASE" ]; then
  echo "check_corpus.sh: missing wire manifest: $WIRE_BASE" >&2
  exit 3
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

if [ "$USE_TOPOLOGY" -eq 1 ]; then
  if ! python3 "$TOPOLOGY_HELPER" jobs --inventory "$INVENTORY" \
       --images-root "$IMG" --foreign-root "$FGN" > "$tmp/jobs.txt"; then
    echo "check_corpus.sh: cannot construct release topology jobs" >&2
    exit 3
  fi
else
  {
    foreign_jobs
    for from_id in $HOME_IDS; do
      for to_id in $HOME_IDS; do
        printf 'C\t%s\t%s\n' "$IMG/$from_id" "$IMG/$to_id"
      done
    done
  } > "$tmp/jobs.txt"
fi
# Expected counts come from the scheduled jobs, so a reduced subset still catches a dropped worker.
EXP_HOME=$(grep -c '^C' "$tmp/jobs.txt")
EXP_FGN=$(grep -c '^F' "$tmp/jobs.txt")
xargs -P "$JOBS" -L1 bash -c 'cm_work "$0" "$1" "$2"' < "$tmp/jobs.txt" > "$tmp/pairs.txt"

# Optional per-pair home-size dump (used by ab_matrix.sh to build an A/B baseline from any encoder).
# Sort because worker completion order is intentionally nondeterministic.
[ -n "$DUMP" ] && awk '$1=="C"{print $2"\t"$3"\t"$4}' "$tmp/pairs.txt" | sort > "$DUMP"
# Optional combined-baseline C/F dump. Sorting makes a parallel run byte-reproducible.
[ -n "$BASE_DUMP" ] && awk '$1=="C"{print $1"\t"$2"\t"$3"\t"$4"\t"$13} \
                                  $1=="F"{print $1"\t"$2"\t"$3"\t-\t"$13}' \
                            "$tmp/pairs.txt" | sort > "$BASE_DUMP"

agg=$(
  awk '{ tag=$1; sz=$4; ok=$5; j=$6; amp=$7; page=$8; inv=$9; ua=$10; oob=$11; canary=$12;
         if(j=="NA"||amp=="NA"){bad++; next}
         if(tag=="C"){cfull+=sz; cok+=ok; cn++}
         else if(tag=="F"){ffull+=sz; fok+=ok; fn++}
         if(j>mj)mj=j; if(amp>ma)ma=amp; if(page>mp)mp=page; if(inv>mi)mi=inv;
         if(ua>mu)mu=ua; if(oob>mo)mo=oob; if(canary>mc)mc=canary }
       END{ printf "%d %d %d %d %d %d %d %d %d %d %d %d %d %d\n", cok,cfull,cn, fok,ffull,fn, mj,ma,mp,mi,mu,mo,mc,bad+0 }' "$tmp/pairs.txt"
)
read cok cfull cn fok ffull fn mj ma mp mi mu mo mc bad <<EOF
$agg
EOF
# Home per-pair split vs the baseline TSV — skipped (NA) when no baseline is provided.
if [ -n "$SIZE_BASE" ]; then
  split=$(
    awk '
      FNR==NR {
        if($0 ~ /^[[:space:]]*$/ || $1 ~ /^#/) next
        if(NF==5 && $1=="C"){key=$2 SUBSEP $3; base[key]=$4}
        else if(NF==3){key=$1 SUBSEP $2; base[key]=$3}
        else next
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

# Every corpus blob is a wire artifact, not merely a byte count.  Compare hashes from the
# already-produced worker blobs, so this closes the size-neutral wire-drift gap without a
# second encode pass.
if [ -n "$WIRE_BASE" ]; then
  wire=$(
    awk '
      FNR==NR {
        if($0 ~ /^[[:space:]]*$/ || $1 ~ /^#/) next
        if(NF!=5 || ($1!="C" && $1!="F") || length($5)!=64 || $5 !~ /^[0-9a-f]+$/){next}
        key=$1 SUBSEP $2 SUBSEP $3
        if(key in want) dup++
        want[key]=$5
        bcount++
        next
      }
      $1=="C" || $1=="F" {
        key=$1 SUBSEP $2 SUBSEP $3
        if(key in seen) dup++
        seen[key]=1
        if(!(key in want)){missing++; next}
        if($13==want[key]) same++
        else mismatch++
      }
      END {
        printf "%d %d %d %d %d\n", same+0,mismatch+0,missing+0,bcount+0,dup+badfmt
      }' "$WIRE_BASE" "$tmp/pairs.txt"
  )
  read wmatch wmismatch wmissing wbase wbad <<EOF
$wire
EOF
else
  wmatch=NA; wmismatch=0; wmissing=0; wbase=$((EXP_HOME + EXP_FGN)); wbad=0
fi
wire_full_bad=0
if [ "$EXP_HOME" -eq 256 ] && [ "$EXP_FGN" -eq 34 ] && [ "$wbase" -ne 290 ]; then
  wire_full_bad=1
fi
if [ "$cn" -ne "$EXP_HOME" ] || [ "$fn" -ne "$EXP_FGN" ] || [ "$bad" -ne 0 ] \
   || [ "$hbase" -ne "$EXP_HOME" ] || [ "$hmissing" -ne 0 ] \
   || [ "$wmissing" -ne 0 ] || [ "$wbad" -ne 0 ] || [ "$wire_full_bad" -ne 0 ]; then
  echo "check_corpus.sh: structural error (home=$cn/$EXP_HOME foreign=$fn/$EXP_FGN baseline=$hbase/$EXP_HOME missing=$hmissing wire_manifest=$wbase scheduled=$((EXP_HOME + EXP_FGN)) wire_missing=$wmissing wire_bad=$wbad full_manifest_bad=$wire_full_bad bad_parse=$bad)" >&2
  exit 3
fi

printf 'matrix_ok=%d/%d\nfull_total=%d\nhome_size_better=%s\nhome_size_worse=%s\nhome_size_equal=%s\nforeign_ok=%d/%d\nforeign_total=%d\nwire_identity=%s/%d\nmax_journal=%d\nmax_amplified=%d\nmax_maxpageerase=%d\nmax_inversions=%d\nmax_unaligned=%d\nmax_oob_page_writes=%d\nmax_canary_corrupt=%d\n' \
  "$cok" "$EXP_HOME" "$cfull" "$hbetter" "$hworse" "$hequal" "$fok" "$EXP_FGN" "$ffull" "$wmatch" "$((EXP_HOME + EXP_FGN))" "$mj" "$ma" "$mp" "$mi" "$mu" "$mo" "$mc"

# ---- gate assertions (single source; the Makefile corpus leg just runs this script) ----
# Correctness / write-safety (unconditional; only a broken candidate, never a regression, trips it):
if [ "$cok" -ne "$EXP_HOME" ] || [ "$fok" -ne "$EXP_FGN" ] \
   || [ "$wmismatch" -ne 0 ] || [ "$ma" -ne 0 ] || [ "$mp" -gt 1 ] || [ "$mi" -ne 0 ] \
   || [ "$mu" -ne 0 ] || [ "$mo" -ne 0 ] || [ "$mc" -ne 0 ]; then
  echo "check_corpus.sh: correctness/wire/write-safety gate (matrix=$cok/$EXP_HOME foreign=$fok/$EXP_FGN wire_mismatch=$wmismatch amplified=$ma maxpageerase=$mp inversions=$mi unaligned=$mu oob=$mo canary=$mc)" >&2
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
