#!/usr/bin/env bash
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

# A/B per-pair matrix comparison for compression experiments.
#
# Encodes all 256 corpus pairs plus the two one-face fixtures with a BASELINE
# encoder and a CANDIDATE encoder, round-trips every CANDIDATE blob through the
# candidate decoder, and prints the per-pair better/worse/equal split, both
# corpus totals, and both one-face sizes. The candidate is judged by the
# project rule (AGENTS.md): corpus total must improve WITHOUT an overfit split
# and WITHOUT regressing the one-face product patch beyond the tracked gates.
#
# Usage: scripts/ab_matrix.sh <enc_baseline> <enc_candidate> <dec_candidate> [jobs]
# Env:   IMAGES, FIXTURES (as for check_corpus.sh),
#        BASE_ONEFACE_GROW / BASE_ONEFACE_REVERT (one-face gates; default: the Makefile pins).
# Exit:  0 on a structurally sound run (the accept decision is the caller's),
#        3 on structural error or any candidate round-trip failure.
set -u
ENC_A="${1:?baseline encoder}"; ENC_B="${2:?candidate encoder}"; DEC_B="${3:?candidate decoder}"
JOBS="${4:-$(nproc 2>/dev/null || echo 4)}"
IMG="${IMAGES:-test-bench/images}"
FIX="${FIXTURES:-test-bench/fixtures}"
# One-face gate defaults track the authoritative Makefile pins (env still overrides).
MK="$(dirname "$0")/../Makefile"
gate() {  # <make-var> <env-value> -> its ?= default from the Makefile unless overridden
  [ -n "$2" ] && { echo "$2"; return; }
  v=$(sed -n "s/^$1[[:space:]]*?=[[:space:]]*\([0-9][0-9]*\).*/\1/p" "$MK" | head -1)
  [ -n "$v" ] || { echo "ab_matrix.sh: $1 not found in $MK (stale gate?)" >&2; exit 2; }
  echo "$v"
}
GATE_G=$(gate BASE_ONEFACE_GROW "${BASE_ONEFACE_GROW:-}") || exit 2
GATE_R=$(gate BASE_ONEFACE_REVERT "${BASE_ONEFACE_REVERT:-}") || exit 2

ab_work() {
  from=$1; to=$2
  d=$(mktemp -d)
  "$AB_ENC_A" "$from" "$to" "$d/a.blob" >/dev/null 2>&1
  "$AB_ENC_B" "$from" "$to" "$d/b.blob" >/dev/null 2>&1
  sa=$(wc -c < "$d/a.blob" 2>/dev/null || echo 0)
  sb=$(wc -c < "$d/b.blob" 2>/dev/null || echo 0)
  cp "$from/watch.bin" "$d/mem.bin"
  ok=0
  "$AB_DEC_B" "$d/mem.bin" "$d/b.blob" 1 >/dev/null 2>&1 \
    && cmp -s "$d/mem.bin" "$to/watch.bin" && ok=1
  printf '%s %s %s %s %s\n' "$sa" "$sb" "$ok" "$(basename "$from")" "$(basename "$to")"
  rm -rf "$d"
}
export -f ab_work
export AB_ENC_A="$ENC_A" AB_ENC_B="$ENC_B" AB_DEC_B="$DEC_B"

lines=$(
  for from in "$IMG"/img_*; do for to in "$IMG"/img_*; do printf '%s\t%s\n' "$from" "$to"; done; done \
  | xargs -P "$JOBS" -L1 bash -c 'ab_work "$0" "$1"'
)

n=$(printf '%s\n' "$lines" | wc -l)
rt=$(printf '%s\n' "$lines" | awk '{ ok+=$3 } END { print ok+0 }')
if [ "$n" -ne 256 ] || [ "$rt" -ne 256 ]; then
  echo "ab_matrix.sh: structural error or round-trip failure (pairs=$n roundtrips=$rt/256)" >&2
  printf '%s\n' "$lines" | awk '$3==0 { printf "  RT-FAIL %s -> %s\n", $4, $5 }' >&2
  exit 3
fi

printf '%s\n' "$lines" | awk '
  { ta+=$1; tb+=$2
    if ($2<$1) b++; else if ($2>$1) { w++; printf "worse: %s -> %s  %d -> %d (+%d)\n", $4,$5,$1,$2,$2-$1 > "/dev/stderr" }
    else e++ }
  END { printf "split_better=%d\nsplit_worse=%d\nsplit_equal=%d\n", b+0, w+0, e+0
        printf "full_total_base=%d\nfull_total_cand=%d\nfull_total_delta=%d\n", ta, tb, tb-ta }'

# one-face product patch, both encoders (serial; candidate blobs round-tripped)
d=$(mktemp -d); trap 'rm -rf "$d"' EXIT
"$ENC_A" "$FIX/v0_base" "$FIX/v1_one_face" "$d/ga.blob" >/dev/null 2>&1
"$ENC_A" "$FIX/v1_one_face" "$FIX/v0_base" "$d/ra.blob" >/dev/null 2>&1
"$ENC_B" "$FIX/v0_base" "$FIX/v1_one_face" "$d/gb.blob" >/dev/null 2>&1
"$ENC_B" "$FIX/v1_one_face" "$FIX/v0_base" "$d/rb.blob" >/dev/null 2>&1
for p in g r; do
  from="$FIX/v0_base"; to="$FIX/v1_one_face"
  [ "$p" = r ] && { from="$FIX/v1_one_face"; to="$FIX/v0_base"; }
  cp "$from/watch.bin" "$d/mem.bin"
  "$DEC_B" "$d/mem.bin" "$d/${p}b.blob" 1 >/dev/null 2>&1 && cmp -s "$d/mem.bin" "$to/watch.bin" \
    || { echo "ab_matrix.sh: one-face candidate round-trip failed ($p)" >&2; exit 3; }
done
ga=$(wc -c < "$d/ga.blob"); ra=$(wc -c < "$d/ra.blob")
gb=$(wc -c < "$d/gb.blob"); rb=$(wc -c < "$d/rb.blob")
printf 'oneface_grow_base=%d\noneface_grow_cand=%d\noneface_revert_base=%d\noneface_revert_cand=%d\n' \
  "$ga" "$gb" "$ra" "$rb"
verdict=OK
[ "$gb" -gt "$GATE_G" ] && verdict="ONEFACE_GROW_REGRESSION"
[ "$rb" -gt "$GATE_R" ] && verdict="ONEFACE_REVERT_REGRESSION"
printf 'oneface_gate=%s (caps %d/%d)\n' "$verdict" "$GATE_G" "$GATE_R"
