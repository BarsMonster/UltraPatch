#!/usr/bin/env bash
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

# A/B per-pair matrix comparison for compression experiments.
#
# Thin driver over the shared gate machinery: it runs check_corpus.sh's 256-pair
# home pool TWICE (baseline encoder, then candidate encoder) instead of maintaining
# its own pool/split, names the home pairs the candidate REGRESSED, and applies the
# integrated one-face accept/reject verdict (via oneface_metrics.sh). The candidate is
# judged by the project rule (AGENTS.md): corpus total must improve WITHOUT an overfit
# split and WITHOUT regressing the one-face product patch beyond the tracked gates.
#
# Usage: scripts/ab_matrix.sh <enc_baseline> <enc_candidate> <dec_candidate> [jobs]
# Encoder commands must accept: <from_image> <to_image> <patch>.
# Decoder commands must accept: --decode <image> <patch>.
# The home matrix round-trips each encoder's blobs through THAT encoder (ultrapatch is
# both encoder and decoder); <dec_candidate> is used for the one-face candidate round-trip.
# Env:   IMAGES, FOREIGN, FIXTURES (as for check_corpus.sh / oneface_metrics.sh),
#        BASE_ONEFACE_GROW / BASE_ONEFACE_REVERT (one-face caps; enforced only when set,
#        skipped for bare measurement when unset — mirroring check_corpus.sh's ratchets).
# Exit:  0 on a structurally sound run that passes the no-regression accept gate,
#        1 on a structurally sound run that should be rejected for compression,
#        3 on structural error or any encoder/candidate round-trip failure.
set -u
ENC_A="${1:?baseline encoder}"; ENC_B="${2:?candidate encoder}"; DEC_B="${3:?candidate decoder}"
JOBS="${4:-$(nproc 2>/dev/null || echo 4)}"
: "${IMAGES:?ab_matrix.sh: IMAGES not set to the profile-scoped corpus}"
: "${FIXTURES:?ab_matrix.sh: FIXTURES not set to the profile-scoped corpus}"
FIX="$FIXTURES"
SDIR="$(dirname "$0")"
CC="$SDIR/../check_corpus.sh"
# One-face caps: taken straight from the env pins (no Makefile scrape); enforced only when set.
GATE_G="${BASE_ONEFACE_GROW:-}"
GATE_R="${BASE_ONEFACE_REVERT:-}"

d=$(mktemp -d); trap 'rm -rf "$d"' EXIT

# --- home matrix, two runs over the shared pool ---------------------------------------------
# Run 1 (baseline): bare per-pair measurement (no split baseline), dump its sizes as our A/B
# reference. Round-trips through ENC_A, so ENC_A must be a working decoder (ultrapatch is).
if ! CORPUS_SIZE_BASELINE="" CORPUS_SIZE_DUMP="$d/a.tsv" WIRE_BASELINE="" \
     ULTRAPATCH="$ENC_A" \
     "$CC" "$JOBS" > "$d/a.txt" 2> "$d/a.err"; then
  echo "ab_matrix.sh: baseline corpus run failed (enc=$ENC_A)" >&2
  cat "$d/a.err" >&2
  exit 3
fi
# Run 2 (candidate): split its sizes against the baseline dump; round-trip through DEC_B. No
# ratchet pins passed, so a nonzero exit here is a structural / round-trip / write-safety fault.
if ! CORPUS_SIZE_BASELINE="$d/a.tsv" CORPUS_SIZE_DUMP="$d/b.tsv" \
     WIRE_BASELINE="" ULTRAPATCH="$ENC_B" ULTRAPATCH_DECODE="$DEC_B" \
     "$CC" "$JOBS" > "$d/b.txt" 2> "$d/b.err"; then
  echo "ab_matrix.sh: candidate corpus run failed (structural / round-trip; enc=$ENC_B dec=$DEC_B)" >&2
  cat "$d/b.err" >&2
  exit 3
fi

# Split counts are single-sourced from check_corpus.sh; totals come from each run.
better=$(sed -n 's/^home_size_better=//p' "$d/b.txt")
worse=$(sed -n 's/^home_size_worse=//p' "$d/b.txt")
equal=$(sed -n 's/^home_size_equal=//p' "$d/b.txt")
full_base=$(sed -n 's/^full_total=//p' "$d/a.txt")
full_cand=$(sed -n 's/^full_total=//p' "$d/b.txt")
printf 'split_better=%s\nsplit_worse=%s\nsplit_equal=%s\n' "$better" "$worse" "$equal"
printf 'full_total_base=%d\nfull_total_cand=%d\nfull_total_delta=%d\n' \
  "$full_base" "$full_cand" "$((full_cand - full_base))"
# Name WHICH home pairs regressed (candidate blob larger than baseline), to stderr.
awk 'NR==FNR { a[$1 SUBSEP $2]=$3; next }
     { k=$1 SUBSEP $2; if((k in a) && $3+0 > a[k]+0)
         printf "worse: %s -> %s  %d -> %d (+%d)\n", $1,$2,a[k],$3,$3-a[k] > "/dev/stderr" }' \
  "$d/a.tsv" "$d/b.tsv"

# --- one-face product patch, both encoders (candidate blobs round-tripped through DEC_B) -----
# Both calls run oneface_metrics.sh in BARE-MEASUREMENT mode (pins cleared): the accept/reject
# verdict below owns cap enforcement (candidate-vs-baseline binary, not the fixed gate pins), so a
# cap regression here must stay REJECT (exit 1), never oneface_metrics.sh's exit-nonzero -> our
# structural exit 3.
BASE_ONEFACE_GROW= BASE_ONEFACE_REVERT= FIXTURES="$FIX" ONEFACE_WIRE_HASHES=1 \
  "$SDIR/oneface_metrics.sh" "$ENC_A" > "$d/base_oneface.txt" \
  || { echo "ab_matrix.sh: one-face baseline encode failed" >&2; exit 3; }
BASE_ONEFACE_GROW= BASE_ONEFACE_REVERT= FIXTURES="$FIX" ONEFACE_ROUNDTRIP=1 ONEFACE_WIRE_HASHES=1 \
  "$SDIR/oneface_metrics.sh" "$ENC_B" "$DEC_B" > "$d/cand_oneface.txt" \
  || { echo "ab_matrix.sh: one-face candidate encode/round-trip failed" >&2; exit 3; }
ga=$(sed -n 's/^oneface_grow=//p' "$d/base_oneface.txt")
ra=$(sed -n 's/^oneface_revert=//p' "$d/base_oneface.txt")
gb=$(sed -n 's/^oneface_grow=//p' "$d/cand_oneface.txt")
rb=$(sed -n 's/^oneface_revert=//p' "$d/cand_oneface.txt")
gha=$(sed -n 's/^oneface_grow_sha256=//p' "$d/base_oneface.txt")
rha=$(sed -n 's/^oneface_revert_sha256=//p' "$d/base_oneface.txt")
ghb=$(sed -n 's/^oneface_grow_sha256=//p' "$d/cand_oneface.txt")
rhb=$(sed -n 's/^oneface_revert_sha256=//p' "$d/cand_oneface.txt")
printf 'oneface_grow_base=%d\noneface_grow_cand=%d\noneface_revert_base=%d\noneface_revert_cand=%d\n' \
  "$ga" "$gb" "$ra" "$rb"
wire_changed=0
if [ "$gha" != "$ghb" ] || [ "$rha" != "$rhb" ]; then wire_changed=1; fi
printf 'oneface_wire_changed=%d\n' "$wire_changed"
verdict=OK
[ -n "$GATE_G" ] && [ "$gb" -gt "$GATE_G" ] && verdict="ONEFACE_GROW_REGRESSION"
[ -n "$GATE_R" ] && [ "$rb" -gt "$GATE_R" ] && verdict="ONEFACE_REVERT_REGRESSION"
if [ -n "$GATE_G" ] || [ -n "$GATE_R" ]; then
  printf 'oneface_gate=%s (caps %s/%s)\n' "$verdict" "${GATE_G:-NA}" "${GATE_R:-NA}"
else
  printf 'oneface_gate=SKIPPED (caps unset)\n'
fi

# --- accept gate (unchanged verdict logic + exit-code contract) ------------------------------
reject=""
[ "$worse" -ne 0 ] && reject="${reject:+$reject,}worse_pairs"
[ "$full_cand" -gt "$full_base" ] && reject="${reject:+$reject,}full_total"
[ "$gb" -gt "$ga" ] && reject="${reject:+$reject,}oneface_grow"
[ "$rb" -gt "$ra" ] && reject="${reject:+$reject,}oneface_revert"
[ "$verdict" != OK ] && reject="${reject:+$reject,}oneface_gate"
if [ -n "$reject" ]; then
  printf 'accept_gate=REJECT (%s)\n' "$reject"
  exit 1
fi
printf 'accept_gate=OK\n'
