#!/usr/bin/env bash
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

# Edge-input gate: synthetic pairs the 256-pair firmware corpus never exercises —
# empty/tiny/equal images, all-0xFF/all-0x00, incompressible random data, text, page-boundary
# sizes, and a >384 KiB span (well past the home-corpus 216 KiB maximum).
#
# Acceptance model: ultrapatch SELF-VERIFIES every emitted patch on the reference decoder, so for
# each pair either (a) encode succeeds -> the host decoder MUST round-trip the blob
# byte-exactly, or (b) encode refuses
# cleanly (nonzero exit, no blob) -> logged as a refusal. Crashes, hangs, or wrong output
# anywhere = failure.
#
# All fixtures are generated deterministically (fixed-seed LCG) — no committed binaries.
#
# Usage: check_edge.sh   (needs ./ultrapatch already built)
set -euo pipefail

EXPECTED_CASES=12
EXPECTED_ROUNDTRIPS=11
EXPECTED_REFUSALS=1

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

if [ ! -x ./ultrapatch ]; then
  echo "edge infrastructure failure: ./ultrapatch is missing or not executable" >&2
  exit 2
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# gen <path> <size> <mode> [args...]: deterministic image generator (shared scripts/synth_gen.py).
#   modes: rand <seed> | const <byte> | text | mutate <src> <seed> <permille> | insert <src> <size> <seed>
gen() { python3 "$script_dir/synth_gen.py" img "$@"; }

mkpair() { # mkpair <name> -> creates $tmp/<name>_from/ and $tmp/<name>_to/ dirs
  mkdir -p "$tmp/$1_from" "$tmp/$1_to"
}

cases=0; roundtrips=0; refusals=0; failures=0

run_case() { # run_case <name>  (dirs already populated with watch.bin)
  name=$1
  cases=$((cases + 1))
  from="$tmp/${name}_from/watch.bin"; to="$tmp/${name}_to/watch.bin"
  blob="$tmp/$name.blob"
  if [ ! -f "$from" ] || [ ! -f "$to" ]; then
    echo "edge infrastructure failure: $name fixture is incomplete" >&2
    exit 2
  fi
  if ./ultrapatch "$from" "$to" "$blob" >/dev/null 2>"$tmp/$name.encerr"; then
    cp "$from" "$tmp/$name.mem"
    if ./ultrapatch --decode "$tmp/$name.mem" "$blob" >/dev/null 2>&1 && cmp -s "$tmp/$name.mem" "$to"; then
      roundtrips=$((roundtrips + 1))
    else
      echo "edge FAILURE: $name encoded but did not round-trip" >&2
      failures=$((failures + 1))
    fi
  else
    if [ -f "$blob" ]; then
      echo "edge FAILURE: $name refused but left a blob file" >&2
      failures=$((failures + 1))
    else
      encerr_tail="$(tail -n 1 "$tmp/$name.encerr" 2>/dev/null || true)"
      echo "edge refusal: $name ($encerr_tail)" >&2
      refusals=$((refusals + 1))
    fi
  fi
}

# --- tiny and degenerate sizes ---
mkpair empty_to_small; : > "$tmp/empty_to_small_from/watch.bin"; gen "$tmp/empty_to_small_to/watch.bin" 512 rand 11
run_case empty_to_small

mkpair small_to_empty; gen "$tmp/small_to_empty_from/watch.bin" 512 rand 11; : > "$tmp/small_to_empty_to/watch.bin"
run_case small_to_empty

mkpair one_byte; printf 'A' > "$tmp/one_byte_from/watch.bin"; printf 'B' > "$tmp/one_byte_to/watch.bin"
run_case one_byte

mkpair tiny_grow; printf 'xyz' > "$tmp/tiny_grow_from/watch.bin"; printf 'xyzab' > "$tmp/tiny_grow_to/watch.bin"
run_case tiny_grow

# --- identical images (patch ~ header only) ---
mkpair equal; gen "$tmp/equal_from/watch.bin" 32768 rand 22; cp "$tmp/equal_from/watch.bin" "$tmp/equal_to/watch.bin"
run_case equal

# --- constant fills (erased-flash-like) ---
mkpair fills; gen "$tmp/fills_from/watch.bin" 8192 const 0xFF; gen "$tmp/fills_to/watch.bin" 8192 const 0x00
run_case fills

# --- constant fill, 0x00 -> 0xFF: regression-locks the exact-equivalent LZ chain pruning
# (this shape was ~18 s before the fix, ~2 s after; a quadratic regression would blow the
# gate's 60 s execution cap) ---
mkpair fills_grow; gen "$tmp/fills_grow_from/watch.bin" 8192 const 0x00; gen "$tmp/fills_grow_to/watch.bin" 8192 const 0xFF
run_case fills_grow

# --- incompressible random: near-identical and unrelated ---
mkpair rand_mut; gen "$tmp/rand_mut_from/watch.bin" 65536 rand 33
gen "$tmp/rand_mut_to/watch.bin" 0 mutate "$tmp/rand_mut_from/watch.bin" 34 10
run_case rand_mut

mkpair rand_unrel; gen "$tmp/rand_unrel_from/watch.bin" 49152 rand 44; gen "$tmp/rand_unrel_to/watch.bin" 49152 rand 55
run_case rand_unrel

# --- non-ARM structured data (text) ---
mkpair text; gen "$tmp/text_from/watch.bin" 40000 text
gen "$tmp/text_to/watch.bin" 0 mutate "$tmp/text_from/watch.bin" 66 5
run_case text

# --- journal page-table boundary sizes (64 KiB pages) ---
mkpair page64; gen "$tmp/page64_from/watch.bin" 65535 rand 77
gen "$tmp/page64_to/watch.bin" 65537 insert "$tmp/page64_from/watch.bin" 2 78
run_case page64

# --- >384 KiB span (the flat 24-bit journal spans 16 MiB) ---
mkpair big_shift; gen "$tmp/big_shift_from/watch.bin" 409600 rand 88
gen "$tmp/big_shift_to/watch.bin" 0 insert "$tmp/big_shift_from/watch.bin" 4096 89
run_case big_shift

if [ "$cases" -ne "$EXPECTED_CASES" ]; then
  echo "edge FAILURE: expected $EXPECTED_CASES cases, got $cases" >&2
  failures=$((failures + 1))
fi
if [ "$roundtrips" -ne "$EXPECTED_ROUNDTRIPS" ]; then
  echo "edge FAILURE: expected $EXPECTED_ROUNDTRIPS accepted round-trips, got $roundtrips" >&2
  failures=$((failures + 1))
fi
if [ "$refusals" -ne "$EXPECTED_REFUSALS" ]; then
  echo "edge FAILURE: expected $EXPECTED_REFUSALS refusals, got $refusals" >&2
  failures=$((failures + 1))
fi

printf 'edge_cases=%u\nedge_roundtrips=%u\nedge_refusals=%u\nedge_failures=%u\n' \
  "$cases" "$roundtrips" "$refusals" "$failures"

test "$failures" -eq 0
