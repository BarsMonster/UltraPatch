#!/usr/bin/env bash
# Fast parallel 16x16 corpus-matrix metrics for the A1 gate.
#
# Run from the c/ build dir; needs ./hy_enc and ./hy_dec already built. Prints the nine gate
# metric lines (identical names/format to the old serial check-corpus loop) so the Makefile (or
# a measurement run) can parse them. The 256 (from,to) pairs are independent, so they run in
# parallel across all cores; each worker uses its OWN mktemp dir (no shared blob/mem path), which
# keeps the deterministic encoder free of the cross-run contamination shared /tmp paths cause.
#
# Usage: check_corpus.sh [W] [jobs]      (W defaults to 10, jobs to nproc)
# Exit 3 on a structural error (not 256 pairs, or a decode produced no parseable NVM metrics).
set -u
W="${1:-10}"
JOBS="${2:-$(nproc 2>/dev/null || echo 4)}"
IMG=../images
FIX=../fixtures

# One pair: encode -> blob, decode a fresh copy of `from` in place, compare to `to`, emit one line
#   "<blobsize> <self?> <roundtrip_ok?> <journal_used> <amplified> <maxrowerase> <inversions>"
# (a single short printf is < PIPE_BUF, so parallel workers' lines never interleave).
cm_work() {
  from=$1; to=$2
  d=$(mktemp -d)
  ./hy_enc "$from" "$to" "$d/p.blob" "$CM_W" >/dev/null 2>&1
  sz=$(wc -c < "$d/p.blob")
  cp "$from/watch.bin" "$d/mem.bin"
  ./hy_dec "$d/mem.bin" "$d/p.blob" 1 >/dev/null 2>"$d/log"
  if cmp -s "$d/mem.bin" "$to/watch.bin"; then ok=1; else ok=0; fi
  j=$(sed -n 's/.*journal_used=\([0-9][0-9]*\).*/\1/p' "$d/log")
  v=$(sed -n 's/.*amplified=\([0-9][0-9]*\).*maxrowerase=\([0-9][0-9]*\).*inversions=\([-0-9][0-9]*\).*/\1 \2 \3/p' "$d/log")
  self=0; [ "$from" = "$to" ] && self=1
  printf '%s %s %s %s %s\n' "$sz" "$self" "$ok" "${j:-NA}" "${v:-NA NA NA}"
  rm -rf "$d"
}
export -f cm_work
export CM_W="$W"

agg=$(
  for from in "$IMG"/img_*; do for to in "$IMG"/img_*; do printf '%s\t%s\n' "$from" "$to"; done; done \
  | xargs -P "$JOBS" -L1 bash -c 'cm_work "$0" "$1"' \
  | awk '{ full+=$1; if($2==0)non+=$1; ok+=$3;
           if($4=="NA"||$5=="NA")bad++;
           if($4>mj)mj=$4; if($5>ma)ma=$5; if($6>mr)mr=$6; if($7>mi)mi=$7; n++ }
         END{ printf "%d %d %d %d %d %d %d %d %d\n", ok,full,non,mj,ma,mr,mi,n,bad+0 }'
)
set -- $agg
ok=$1; full=$2; non=$3; mj=$4; ma=$5; mr=$6; mi=$7; n=$8; bad=$9
if [ "$n" -ne 256 ] || [ "$bad" -ne 0 ]; then
  echo "check_corpus.sh: structural error (pairs=$n bad_parse=$bad)" >&2
  exit 3
fi

# real one-face firmware update (grow + revert) — two encodes, serial (negligible).
d=$(mktemp -d)
./hy_enc "$FIX/v0_base" "$FIX/v1_one_face" "$d/grow.blob" "$W" >/dev/null 2>&1
./hy_enc "$FIX/v1_one_face" "$FIX/v0_base" "$d/revert.blob" "$W" >/dev/null 2>&1
og=$(wc -c < "$d/grow.blob"); orv=$(wc -c < "$d/revert.blob")
rm -rf "$d"

printf 'matrix_ok=%d/256\nfull_total=%d\nnonself_total=%d\nmax_journal=%d\nmax_amplified=%d\nmax_maxrowerase=%d\nmax_inversions=%d\noneface_grow=%d\noneface_revert=%d\n' \
  "$ok" "$full" "$non" "$mj" "$ma" "$mr" "$mi" "$og" "$orv"
