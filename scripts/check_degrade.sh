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
#
# ultrapatch self-verifies every emitted blob on the reference decoder, so a round-trip failure is
# already impossible for an accepted blob; this gate additionally pins that the SPECIFIC path
# engaged, using the wire-neutral A1_DEGRADE_STATS stderr line (blob bytes unaffected — the
# golden gate proves it) and the decoder's existing journal-peak metric.
#
# All fixtures are generated deterministically by the shared scripts/synth_gen.py (fixed-seed
# LCG) — no committed binaries. Cases (a) and (c) request its two NAMED golden pins, so they are
# byte-identical to the check_golden manifest entries by construction.
#
# Usage: check_degrade.sh     (needs ./ultrapatch already built)
set -u

CC_HOST="${CC:-cc}"
IMG="${IMAGES:-test-bench/images}"
. "$(dirname "$0")/tempdir.sh"

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

# enc <name> -> writes $tmp/<name>.blob, captures A1_DEGRADE line in $tmp/<name>.deg; returns encoder rc
enc() {
  name=$1
  A1_DEGRADE_STATS=1 ./ultrapatch "$tmp/${name}_from/watch.bin" "$tmp/${name}_to/watch.bin" "$tmp/$name.blob" \
    >/dev/null 2>"$tmp/$name.encerr"
  rc=$?
  grep '^A1_DEGRADE' "$tmp/$name.encerr" > "$tmp/$name.deg" 2>/dev/null || :
  return $rc
}

# dec <decoder> <name> <mode> -> round-trip a fresh copy of `from`; echoes "OK|WRONG|REJECT <journal>"
dec() {
  d=$1; name=$2; bmode=$3
  cp "$tmp/${name}_from/watch.bin" "$tmp/$name.mem"
  "$d" --decode $bmode "$tmp/$name.mem" "$tmp/$name.blob" >/dev/null 2>"$tmp/$name.declog"; drc=$?
  jp=$(sed -n 's/.*journal_used=\([0-9][0-9]*\).*/\1/p' "$tmp/$name.declog")
  if [ $drc -ne 0 ]; then echo "REJECT ${jp:-NA}"; return; fi
  if cmp -s "$tmp/$name.mem" "$tmp/${name}_to/watch.bin"; then echo "OK ${jp:-NA}"; else echo "WRONG ${jp:-NA}"; fi
}

fail=0
note() { echo "check_degrade: $*" >&2; }
bad()  { echo "DEGRADE FAILURE: $*" >&2; fail=$((fail+1)); }

# The decoder mirrors these; keep the asserts self-documenting. Derive the journal-degradation
# budget from the decoder's RC_JSLOTS_DEFAULT rather than hand-mirroring it.
JBUDGET=$(sed -n 's/^#define[[:space:]]\+RC_JSLOTS_DEFAULT[[:space:]]\+\([0-9][0-9]*\)u\?.*/\1/p' \
  "$(dirname "$0")/../src/patch_config.h" | head -1)
[ -n "$JBUDGET" ] || { echo "check_degrade: RC_JSLOTS_DEFAULT not found in src/patch_config.h" >&2; exit 2; }

# ---- variant D=1 decoder (OUTROW_DEPTH=1): a strictly smaller uncommitted window than the
# production D=2 build. Same source as the host backend, its own binary. Used only to prove
# row-window reliance rejects safely (monotone-compatibility contract). ----
D1="$tmp/ultrapatch_d1_decode"
if ! $CC_HOST -O2 -std=c99 -DCORTEX_M0 -DOUTROW_DEPTH=1 -DPATCH_APPLY_DEMO_MAIN -D_POSIX_C_SOURCE=200809L \
      -Isrc src/patch_host_backend.c src/enc_util.c -o "$D1" 2>"$tmp/d1build.log"; then
  note "could not build the D=1 variant decoder:"; sed 's/^/    /' "$tmp/d1build.log" >&2
  echo "degrade_cases=0"; echo "degrade_fail=1"; exit 1
fi

j_peak=NA; opc_n=NA; dir_flip=NA; rw=NA; bigspan=NA

# =========================================================================================
# (a) JOURNAL-BUDGET DEGRADATION — block swap: block A (first half) moves to the top and is
# read H=2048 B behind the write frontier (well past the 512 B row window), so its reads want
# H preserves — well past the budget. The encoder protects the first JBUDGET and converts the rest
# to plain extras. Assert deg_journal=1 AND journal peak == the budget on the degraded blob.
# =========================================================================================
dpin jdeg synth_journal_degrade            # == golden pin (swap 2048 88); fixture==pin by construction
if enc jdeg; then
  dj=$(sed -n 's/.*deg_journal=\([0-9]\).*/\1/p' "$tmp/jdeg.deg")
  pn=$(sed -n 's/.*pres_needed=\([0-9]*\).*/\1/p' "$tmp/jdeg.deg")
  r0=$(dec ./ultrapatch jdeg "")
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
if A1_DEGRADE_STATS=1 ./ultrapatch "$IMG/img_00_n3/watch.bin" "$IMG/img_15_n83/watch.bin" "$tmp/opc.blob" \
     >/dev/null 2>"$tmp/opc.encerr"; then
  opc_sweep=$(sed -n 's/.*opc_splits_sweep=\([0-9][0-9]*\).*/\1/p' "$tmp/opc.encerr")
  opc_win=$(sed -n 's/.* opc_splits=\([0-9][0-9]*\) .*/\1/p' "$tmp/opc.encerr")
  cp "$IMG/img_00_n3/watch.bin" "$tmp/opc.mem"
  if ./ultrapatch --decode "$tmp/opc.mem" "$tmp/opc.blob" >/dev/null 2>&1 && cmp -s "$tmp/opc.mem" "$IMG/img_15_n83/watch.bin"; then rt=OK; else rt=FAIL; fi
  opc_n=$opc_sweep
  [ "${opc_sweep:-0}" -ge 1 ] || bad "OPC op-split never engaged in the plan sweep (opc_splits_sweep=$opc_sweep)"
  [ "$rt" = OK ] || bad "OPC pair did not round-trip ($rt)"
  note "(b) opc split: opc_splits_sweep=$opc_sweep (masking plan variant, fixpoint) shipped_winner_splits=${opc_win:-0} roundtrip=$rt"
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
  ov=$(python3 - "$tmp/dir.blob" <<'EOF'
import sys
b = open(sys.argv[1], "rb").read()
p = 8                                   # skip CRC32(from)[4] | CRC32(to)[4]
def uleb(p):
    v = sh = n = 0
    while True:
        x = b[p]; p += 1; v |= (x & 0x7f) << sh; sh += 7; n += 1
        if not (x & 0x80): break
    return v, p, n, x
_, p, _, _ = uleb(p)                    # from_size
_, p, n, last = uleb(p)                 # size-delta: overlong iff multi-byte ending in 0x00
print("OVERLONG" if (n > 1 and last == 0) else "canonical")
EOF
)
  natflag=$(sed -n 's/.*natural=\([0-9]\).*/\1/p' "$tmp/dir.deg")
  r=$(dec ./ultrapatch dir "")
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
  r2=$(dec ./ultrapatch rowwin "")
  r1=$(dec "$D1" rowwin "")
  rw="D2:${r2%% *}_D1:${r1%% *}"
  [ "${r2%% *}" = OK ] || bad "row-window blob did not round-trip on the production D=2 decoder ($r2)"
  [ "${r2#* }" = 0 ] || bad "row-window blob used the journal (peak ${r2#* }); expected pure window reliance (0)"
  [ "${r1%% *}" = REJECT ] || bad "row-window blob did NOT reject on the D=1 decoder (got $r1) — window contract broken"
  note "(d) row window: D2=${r2%% *} (journal ${r2#* }) D1=${r1%% *} (CRC(to) reject) blob=$(wc -c <"$tmp/rowwin.blob")B"
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
  r=$(dec ./ultrapatch bigspan "")
  bigspan="${r%% *}_j${r#* }"
  [ "${r%% *}" = OK ] || bad "big-span blob did not round-trip ($r)"
  note "(e) big span: roundtrip=${r%% *} journal_peak=${r#* }/$JBUDGET blob=$(wc -c <"$tmp/bigspan.blob")B"
else
  bad "big-span pair was refused (rc=$?)"
fi

cases=5
printf 'degrade_journal_peak=%s\ndegrade_opc_splits=%s\ndegrade_direction=%s\ndegrade_rowwindow=%s\ndegrade_bigspan=%s\ndegrade_cases=%s\ndegrade_fail=%s\n' \
  "$j_peak" "$opc_n" "$dir_flip" "$rw" "$bigspan" "$cases" "$fail"
test "$cases" -eq 5
test "$fail" -eq 0
