#!/usr/bin/env bash
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

# Degradation / direction / row-window / big-span gate: synthetic firmware-like pairs that
# deterministically FORCE each encoder path the 6-blob golden set and the home corpus never
# exercise, and ASSERT the path was actually taken (not merely that the blob round-trips):
#
#   (a) journal-budget degradation  — over-budget read-after-overwrite converted to plain
#       extras; asserts ultrapatch reports deg_journal=1 AND the decoder's journal peak lands
#       exactly on the JSLOTS budget on the degraded blob.
#   (b) OPC_CAP op-splitting         — an op needing >OPC_CAP corrections is split to fixpoint;
#       asserts ultrapatch reports opc_splits>=1.
#   (c) unnatural apply direction    — the encoder flips direction and signals it with an
#       OVERLONG size-delta uLEB; asserts the emitted envelope really carries the overlong
#       marker (multi-byte encoding whose final byte is 0x00) and round-trips.
#   (d) row-window oracle reliance   — sub-256B read-behind-frontier reads left journal-free
#       because the decoder keeps OUTROW_DEPTH uncommitted rows; asserts the blob round-trips
#       on the production D=2 decoder with a ZERO journal peak, yet REJECTS (clean CRC(to)
#       reject, no silent wrong image) on a D=1 decoder built here — the monotone-window
#       compatibility contract.
#   (e) big-span universality        — a >384 KiB-span pair with behind-frontier journal
#       reads at source >= 393216 (the old 6-page journal table refused this late in
#       selfcheck); the flat 24-bit journal spans 16 MiB, so it must encode and round-trip
#       with the over-budget preserves degraded to extras.
#   (f,g) packed-position seam       — real >16 MiB plan geometry proves descending journal
#       planning skips an unrepresentable high preserve before keeping JSLOTS lower entries,
#       and that a correction at local offset 2^24 is split/rebased. Both plans emit real
#       bodies and pass production selfcheck; direct construction avoids a >60 s suffix sort.
#
# ultrapatch self-verifies every emitted blob on the reference decoder, so a round-trip failure is
# already impossible for an accepted blob; this gate additionally pins that the SPECIFIC path
# engaged, using the wire-neutral DEGRADE_STATS stderr line (blob bytes unaffected — the
# golden gate proves it) and the decoder's existing journal-peak metric.
#
# All fixtures are generated deterministically by the shared scripts/synth_gen.py (fixed-seed
# LCG) — no committed binaries. Cases (a) and (c) request its two NAMED golden pins, so they are
# byte-identical to the check_golden manifest entries by construction.
#
# Usage: make check-degrade (supplies the compiler, source lists, and ULTRAPATCH)
set -u

CC_HOST="${CC:-cc}"
: "${IMAGES:?check_degrade.sh: IMAGES not set to the profile-scoped corpus}"
IMG="$IMAGES"
: "${CFLAGS:?check_degrade.sh: CFLAGS not set — invoke through make check-degrade}"
: "${ENC_SEAM_SRCS:?check_degrade.sh: ENC_SEAM_SRCS not set — invoke through make check-degrade}"
: "${CLANG:?check_degrade.sh: CLANG not set — invoke through make check-degrade}"
: "${ENCODER_KERNEL_BASELINE:?check_degrade.sh: encoder kernel baseline not set}"
: "${ULTRAPATCH:?check_degrade.sh: ULTRAPATCH not set; invoke through make check-degrade}"
[ -x "$ULTRAPATCH" ] || {
  echo "check_degrade.sh: ULTRAPATCH is missing or not executable: $ULTRAPATCH" >&2
  exit 2
}
. "$(dirname "$0")/tempdir.sh"

SPLIT="$tmp/split-run-probe"
if ! $CC_HOST $CFLAGS -D_POSIX_C_SOURCE=200809L -DSPLIT_WORK_PROBE \
      test-bench/split-run-probe.c $ENC_SEAM_SRCS -Wl,--gc-sections \
      -o "$SPLIT" 2>"$tmp/split-run-build.log"; then
  echo "check_degrade: split-run budget probe build failed" >&2
  sed 's/^/    /' "$tmp/split-run-build.log" >&2; exit 1
fi
if ! "$SPLIT" >"$tmp/split-run.out" 2>"$tmp/split-run.err"; then
  echo "check_degrade: split-run budget probe failed: $(cat "$tmp/split-run.err")" >&2
  exit 1
fi
cat "$tmp/split-run.out"

# Pinned semantic results replace copies of historical algorithms. The helper runs the same
# canonical streams under this compiler and the descriptor-pinned Clang, then checks all five
# record counts and SHA-256 digests. Explicit boundary assertions remain inside the probes.
CC="$CC_HOST" CLANG="$CLANG" CFLAGS="$CFLAGS" ENC_SEAM_SRCS="$ENC_SEAM_SRCS" \
  ENCODER_KERNEL_BASELINE="$ENCODER_KERNEL_BASELINE" scripts/check_encoder_kernels.sh check

# Deterministic image generator shared by every case (scripts/synth_gen.py). Roles 'from' and
# 'to' are derived from the SAME seed so a pair is reproducible from its parameters alone.
#   gen <out> <from|to> <mode> <args...>
#     rand     n seed                 : n random bytes
#     swap     H seed                 : from=base(2H); to=second-half ++ first-half (lag H)
#     rshift   n seed a b k           : region [a,b) shifted RIGHT by k (insert k at a, drop k at b)
#     highswap lo M seed              : identity below lo, top-M two halves swapped (read past lo)
gen() { python3 "$(dirname "$0")/synth_gen.py" role "$@"; }

# dpair <name> <mode> <args...> : build $tmp/<name>_{from,to}/watch.bin
dpair() {
  name=$1; shift
  mkdir -p "$tmp/${name}_from" "$tmp/${name}_to"
  gen "$tmp/${name}_from/watch.bin" from "$@"
  gen "$tmp/${name}_to/watch.bin"   to   "$@"
}

# dpin <name> <pinname> : like dpair but from a NAMED golden pin in synth_gen.py, so this fixture
# is byte-identical to the check_golden manifest entry BY CONSTRUCTION (no parameter copy to drift).
dpin() {
  name=$1; pin=$2
  mkdir -p "$tmp/${name}_from" "$tmp/${name}_to"
  python3 "$(dirname "$0")/synth_gen.py" pin "$tmp/${name}_from/watch.bin" from "$pin"
  python3 "$(dirname "$0")/synth_gen.py" pin "$tmp/${name}_to/watch.bin"   to   "$pin"
}

# enc <name> -> writes $tmp/<name>.blob, captures DEGRADE line in $tmp/<name>.deg; returns encoder rc
enc() {
  name=$1
  DEGRADE_STATS=1 "$ULTRAPATCH" "$tmp/${name}_from/watch.bin" "$tmp/${name}_to/watch.bin" "$tmp/$name.blob" \
    >/dev/null 2>"$tmp/$name.encerr"
  rc=$?
  grep '^DEGRADE' "$tmp/$name.encerr" > "$tmp/$name.deg" 2>/dev/null || :
  return $rc
}

# dec <decoder> <name> -> round-trip a fresh copy of `from`; echoes "OK|WRONG|REJECT <journal>"
dec() {
  d=$1; name=$2
  cp "$tmp/${name}_from/watch.bin" "$tmp/$name.mem"
  "$d" --decode "$tmp/$name.mem" "$tmp/$name.blob" >/dev/null 2>"$tmp/$name.declog"; drc=$?
  jp=$(sed -n 's/.*journal_used=\([0-9][0-9]*\).*/\1/p' "$tmp/$name.declog")
  if [ $drc -ne 0 ]; then echo "REJECT ${jp:-NA}"; return; fi
  if cmp -s "$tmp/$name.mem" "$tmp/${name}_to/watch.bin"; then echo "OK ${jp:-NA}"; else echo "WRONG ${jp:-NA}"; fi
}

fail=0
note() { echo "check_degrade: $*" >&2; }
bad()  { echo "DEGRADE FAILURE: $*" >&2; fail=$((fail+1)); }

# Derive the journal-degradation budget through the same header as the encoder and decoder.
JBUDGET=$(printf '%s\n' '#include "patch_config.h"' 'JSLOTS' | \
  $CC_HOST $CFLAGS -E -P -x c - | sed -n 's/^\([0-9][0-9]*\)u\{0,1\}$/\1/p' | tail -1)
[ -n "$JBUDGET" ] || { echo "check_degrade: JSLOTS did not preprocess to an integer" >&2; exit 2; }

# ---- variant D=1 decoder (OUTROW_DEPTH=1): a strictly smaller uncommitted window than the
# production D=2 build. Same source as the host backend, its own binary. Used only to prove
# row-window reliance rejects safely (monotone-compatibility contract). ----
D1="$tmp/ultrapatch_d1_decode"
cp -R src "$tmp/d1-src"
sed -i 's/^#define OUTROW_DEPTH 2u$/#define OUTROW_DEPTH 1u/' "$tmp/d1-src/patch_config.h"
if ! $CC_HOST $CFLAGS $DEC_DEMO_DEFINES \
    "$tmp/d1-src/patch_host_backend.c" "$tmp/d1-src/enc_util.c" \
      -o "$D1" 2>"$tmp/d1build.log"; then
  note "could not build the D=1 variant decoder:"; sed 's/^/    /' "$tmp/d1build.log" >&2
  echo "degrade_cases=0"; echo "degrade_fail=1"; exit 1
fi

j_peak=NA; opc_n=NA; dir_flip=NA; rw=NA; bigspan=NA; seam_pres=NA; seam_corr=NA

# =========================================================================================
# (f,g) 24-BIT PACKED-POSITION SEAM — this first-party probe includes the real private plan
# normalizers, constructs actual >16 MiB image spans without running bsdiff, emits production
# range-coded bodies/envelopes, and sends both through the production decoder selfcheck.
# =========================================================================================
SEAM="$tmp/packed-seam-probe"
if ! $CC_HOST $CFLAGS -D_POSIX_C_SOURCE=200809L test-bench/packed-seam-probe.c \
      $ENC_SEAM_SRCS -Wl,--gc-sections -o "$SEAM" 2>"$tmp/seam-build.log"; then
  note "could not build the packed-position seam probe:"; sed 's/^/    /' "$tmp/seam-build.log" >&2
  echo "degrade_cases=0"; echo "degrade_fail=1"; exit 1
fi
if "$SEAM" >"$tmp/seam.out" 2>"$tmp/seam.err"; then
  cat "$tmp/seam.out"
  seam_pres=$(sed -n 's/^packed_seam_preserve=\([^ ]*\).*/\1/p' "$tmp/seam.out")
  seam_corr=$(sed -n 's/^packed_seam_correction=\([^ ]*\).*/\1/p' "$tmp/seam.out")
  [ "$seam_pres" = OK ] || bad "packed-position preserve seam did not report OK"
  [ "$seam_corr" = OK ] || bad "packed-position correction seam did not report OK"
  note "(f,g) packed seam: preserve=$seam_pres correction=$seam_corr (>16 MiB, selfchecked)"
else
  bad "packed-position seam probe failed: $(cat "$tmp/seam.err")"
fi

# =========================================================================================
# (a) JOURNAL-BUDGET DEGRADATION — block swap: block A (first half) moves to the top and is
# read H=2048 B behind the write frontier (well past the 512 B page window), so its reads want
# H preserves — well past the budget. The encoder protects the first JBUDGET and converts the rest
# to plain extras. Assert deg_journal=1 AND journal peak == the budget on the degraded blob.
# =========================================================================================
dpin jdeg synth_journal_degrade            # == golden pin (swap 2048 88); fixture==pin by construction
if enc jdeg; then
  dj=$(sed -n 's/.*deg_journal=\([0-9]\).*/\1/p' "$tmp/jdeg.deg")
  pn=$(sed -n 's/.*pres_needed=\([0-9]*\).*/\1/p' "$tmp/jdeg.deg")
  r0=$(dec "$ULTRAPATCH" jdeg)
  j_peak=${r0#* }
  [ "$dj" = 1 ] || bad "journal degradation did not engage (deg_journal=$dj, pres_needed=$pn)"
  [ "${r0%% *}" = OK ] || bad "journal-degraded blob did not round-trip ($r0)"
  [ "$j_peak" = "$JBUDGET" ] || bad "journal peak $j_peak != budget $JBUDGET on the degraded blob"
  note "(a) journal degrade: deg_journal=$dj pres_needed=$pn journal_peak=$j_peak/$JBUDGET blob=$(wc -c <"$tmp/jdeg.blob")B"
else
  bad "journal-degradation pair was refused (rc=$?)"
fi

# =========================================================================================
# (b) OPC_CAP OP-SPLIT — an op needing >OPC_CAP corrections is split at its median-correction
# offset, iterated to a fixpoint. A "correction" is the RESIDUAL where the ARM field-delta model
# (create_patch_block / merge_op_field_deltas) fails to predict a relocated BL/LDR field. That
# model predicts any CLEAN transform EXACTLY (op-derived deltas are tautologically exact at the
# bsdiff op-walk position), so purely synthetic (fixed-seed LCG) pairs cannot concentrate >80
# residuals in one op — only the dense field churn of a real recompiled image makes the MASKING
# plan variant (mask BL immediates) copy through mixed-size Thumb code and generate hundreds of
# corrections in one op. This drives it deterministically with a committed corpus firmware pair
# (no new binary): the masking variant hits >80 corrections in one op, so the fixpoint split loop
# runs (opc_splits_sweep). Assert the split machinery engaged and that the blob round-trips. The
# corpus round-trip gate only checks round-trips (never asserts the split) and no WINNING plan
# here ships a split (a cleaner variant wins) — this gate is what actually pins the op-split path.
if DEGRADE_STATS=1 "$ULTRAPATCH" "$IMG/img_00_n3/watch.bin" "$IMG/img_15_n83/watch.bin" "$tmp/opc.blob" \
     >/dev/null 2>"$tmp/opc.encerr"; then
  opc_sweep=$(sed -n 's/.*opc_splits_sweep=\([0-9][0-9]*\).*/\1/p' "$tmp/opc.encerr")
  opc_win=$(sed -n 's/.* opc_splits=\([0-9][0-9]*\) .*/\1/p' "$tmp/opc.encerr")
  opc_evals=$(sed -n 's/.*plans_evaluated=\([0-9][0-9]*\).*/\1/p' "$tmp/opc.encerr")
  cp "$IMG/img_00_n3/watch.bin" "$tmp/opc.mem"
  if "$ULTRAPATCH" --decode "$tmp/opc.mem" "$tmp/opc.blob" >/dev/null 2>&1 && cmp -s "$tmp/opc.mem" "$IMG/img_15_n83/watch.bin"; then rt=OK; else rt=FAIL; fi
  opc_n=$opc_sweep
  [ "${opc_sweep:-0}" -ge 1 ] || bad "OPC op-split never engaged in the plan sweep (opc_splits_sweep=$opc_sweep)"
  [ "$opc_evals" = 5 ] || bad "clean OPC pair evaluated ${opc_evals:-0} plans (expected natural-only 5)"
  [ "$rt" = OK ] || bad "OPC pair did not round-trip ($rt)"
  note "(b) opc split: opc_splits_sweep=$opc_sweep plans_evaluated=$opc_evals (masking plan variant, fixpoint) shipped_winner_splits=${opc_win:-0} roundtrip=$rt"
else
  bad "OPC pair was refused (rc=$?)"
fi

# =========================================================================================
# (c) UNNATURAL APPLY DIRECTION — equal-size pair with a large rightward internal shift (lag
# k=600 B): the natural (ascending) direction would journal the whole shifted region, so the
# encoder flips to descending and signals it with an OVERLONG size-delta uLEB. Assert the
# emitted envelope really carries the overlong marker, and that it round-trips.
# =========================================================================================
dpin dir synth_unnatural_dir               # == golden pin (rshift 4096 444 256 3400 600)
if enc dir; then
  # Header uleb #1 is the size-delta; the shared wire helper reports the overlong marker.
  ov=$(python3 "$(dirname "$0")/wire_envelope.py" detect "$tmp/dir.blob" 1)
  natflag=$(sed -n 's/.*natural=\([0-9]\).*/\1/p' "$tmp/dir.deg")
  r=$(dec "$ULTRAPATCH" dir)
  dir_flip=$ov
  [ "$ov" = OVERLONG ] || bad "direction pair did not emit the overlong size-delta uLEB ($ov)"
  [ "$natflag" = 0 ] || bad "encoder reports natural=$natflag for the flipped pair (expected 0)"
  [ "${r%% *}" = OK ] || bad "direction-flip blob did not round-trip ($r)"
  note "(c) direction flip: size-delta=$ov natural=$natflag blob=$(wc -c <"$tmp/dir.blob")B"
else
  bad "direction-flip pair was refused (rc=$?)"
fi

# =========================================================================================
# (d) ROW-WINDOW ORACLE RELIANCE — equal-size pair with a SMALL rightward shift (lag 32 B):
# every shifted read sits < 256 B behind the frontier, so the encoder leaves them journal-free
# (relies on the decoder's 2-row uncommitted window). Assert: round-trips on the production
# D=2 decoder with a ZERO journal peak, but REJECTS on the D=1 decoder (CRC(to) reject, no
# silent wrong image). That regression-locks the monotone larger-window compatibility contract.
# =========================================================================================
dpair rowwin rshift 8192 555 128 6000 32
if enc rowwin; then
  r2=$(dec "$ULTRAPATCH" rowwin)
  r1=$(dec "$D1" rowwin)
  rw="D2:${r2%% *}_D1:${r1%% *}"
  [ "${r2%% *}" = OK ] || bad "row-window blob did not round-trip on the production D=2 decoder ($r2)"
  [ "${r2#* }" = 0 ] || bad "row-window blob used the journal (peak ${r2#* }); expected pure window reliance (0)"
  [ "${r1%% *}" = REJECT ] || bad "row-window blob did NOT reject on the D=1 decoder (got $r1) — window contract broken"
  note "(d) page window: D2=${r2%% *} (journal ${r2#* }) D1=${r1%% *} (CRC(to) reject) blob=$(wc -c <"$tmp/rowwin.blob")B"
else
  bad "row-window pair was refused (rc=$?)"
fi

# =========================================================================================
# (e) BIG-SPAN UNIVERSALITY — >384 KiB span with a behind-frontier journal read at a source
# position >= 393216: identity below 384 KiB, the top 4 KiB two halves swapped. The old paged
# journal could not represent those positions and the pair died late in selfcheck; the flat
# 24-bit journal spans 16 MiB, so it must encode (over-budget preserves degraded to extras)
# and round-trip.
# =========================================================================================
dpair bigspan highswap 393216 4096 88
if enc bigspan; then
  r=$(dec "$ULTRAPATCH" bigspan)
  bigspan="${r%% *}_j${r#* }"
  [ "${r%% *}" = OK ] || bad "big-span blob did not round-trip ($r)"
  note "(e) big span: roundtrip=${r%% *} journal_peak=${r#* }/$JBUDGET blob=$(wc -c <"$tmp/bigspan.blob")B"
else
  bad "big-span pair was refused (rc=$?)"
fi

cases=7
printf 'degrade_journal_peak=%s\ndegrade_opc_splits=%s\ndegrade_direction=%s\ndegrade_rowwindow=%s\ndegrade_bigspan=%s\ndegrade_packed_preserve=%s\ndegrade_packed_correction=%s\ndegrade_cases=%s\ndegrade_fail=%s\n' \
  "$j_peak" "$opc_n" "$dir_flip" "$rw" "$bigspan" "$seam_pres" "$seam_corr" "$cases" "$fail"
test "$cases" -eq 7
test "$fail" -eq 0
