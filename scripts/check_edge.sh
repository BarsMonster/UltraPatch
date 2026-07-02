#!/usr/bin/env bash
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

# Edge-input gate: synthetic pairs the 256-pair firmware corpus never exercises —
# empty/tiny/equal images, all-0xFF/all-0x00, incompressible random data, text, page-boundary
# sizes, and a >384 KiB span that can overflow the journal page table (REJ_RESOURCE path).
#
# Acceptance model: hy_enc SELF-VERIFIES every emitted patch on the reference decoder, so for
# each pair either (a) hy_enc succeeds -> the host decoder MUST round-trip the blob
# byte-exactly (direct pull AND byte-at-a-time via the push adapter), or (b) hy_enc refuses
# cleanly (nonzero exit, no blob) -> logged as a refusal. Crashes, hangs, or wrong output
# anywhere = failure.
#
# All fixtures are generated deterministically (fixed-seed LCG) — no committed binaries.
#
# Usage: check_edge.sh [W]   (needs ./hy_enc and ./hy_dec already built)
set -u

W="${1:-10}"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# gen <path> <size> <mode>: deterministic image generator.
#   modes: rand <seed> | const <byte> | text | mutate <src> <seed> <permille> | insert <src> <size> <seed>
gen() {
  python3 - "$@" <<'EOF'
import sys
path, size, mode = sys.argv[1], int(sys.argv[2]), sys.argv[3]
def lcg(seed):
    s = seed & 0xffffffff
    while True:
        s = (s * 1664525 + 1013904223) & 0xffffffff
        yield (s >> 16) & 0xff
if mode == "rand":
    r = lcg(int(sys.argv[4])); data = bytes(next(r) for _ in range(size))
elif mode == "const":
    data = bytes([int(sys.argv[4], 0)]) * size
elif mode == "text":
    line = b"The quick brown fox jumps over the lazy dog %d.\n"
    data = b"".join(line % i for i in range(size))[:size]
elif mode == "mutate":
    src = bytearray(open(sys.argv[4], "rb").read())
    r = lcg(int(sys.argv[5])); permille = int(sys.argv[6])
    for i in range(len(src)):
        if (next(r) * 4) % 1000 < permille:
            src[i] ^= next(r) or 1
    data = bytes(src[:size]) if size else bytes(src)
elif mode == "insert":
    src = open(sys.argv[4], "rb").read()
    r = lcg(int(sys.argv[6])); pre = bytes(next(r) for _ in range(int(sys.argv[5])))
    data = (pre + src)[:size] if size else pre + src
else:
    sys.exit("bad mode")
open(path, "wb").write(data)
EOF
}

mkpair() { # mkpair <name> -> creates $tmp/<name>_from/ and $tmp/<name>_to/ dirs
  mkdir -p "$tmp/$1_from" "$tmp/$1_to"
}

cases=0; roundtrips=0; refusals=0; failures=0

run_case() { # run_case <name>  (dirs already populated with watch.bin)
  name=$1
  cases=$((cases + 1))
  from="$tmp/${name}_from/watch.bin"; to="$tmp/${name}_to/watch.bin"
  blob="$tmp/$name.blob"
  if ./hy_enc "$tmp/${name}_from" "$tmp/${name}_to" "$blob" "$W" >/dev/null 2>"$tmp/$name.encerr"; then
    ok=1
    for mode in "" "1"; do   # direct pull, then byte-at-a-time via the push adapter
      cp "$from" "$tmp/$name.mem"
      if ! ./hy_dec "$tmp/$name.mem" "$blob" $mode >/dev/null 2>&1; then ok=0; fi
      if ! cmp -s "$tmp/$name.mem" "$to"; then ok=0; fi
    done
    if [ "$ok" = 1 ]; then
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
      echo "edge refusal: $name ($(tail -1 "$tmp/$name.encerr" 2>/dev/null))" >&2
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

# --- >384 KiB span: beyond the 6-page journal table; clean REJ_RESOURCE refusal is OK ---
mkpair big_shift; gen "$tmp/big_shift_from/watch.bin" 409600 rand 88
gen "$tmp/big_shift_to/watch.bin" 0 insert "$tmp/big_shift_from/watch.bin" 4096 89
run_case big_shift

printf 'edge_cases=%u\nedge_roundtrips=%u\nedge_refusals=%u\nedge_failures=%u\n' \
  "$cases" "$roundtrips" "$refusals" "$failures"
test "$failures" -eq 0
