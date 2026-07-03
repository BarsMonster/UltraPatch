#!/usr/bin/env bash
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT
#
# Static-analysis leg: gcc -fanalyzer over BOTH first-party translation units
# (host encoder + device decoder) and their helpers. The decoder was already
# analyzer-clean; this extends the same tool to the previously-uncovered encoder.
#
# Curated flag set:
#   -Wno-analyzer-tainted-assertion : a batch host tool that BY DESIGN validates
#       untrusted firmware/ELF and die()s on malformed input is not a taint->sink
#       security vulnerability; this checker only produces noise here.
#   Vendored vendor/libdivsufsort is third-party and excluded.
#   Two documented false positives inside elf_ranges (allocator-wrapper modeling
#   gaps) are suppressed with an inline #pragma at the site, so the checkers stay
#   active for every other line.
#
# Clean baseline: exits nonzero on ANY analyzer finding (a NEW finding fails the
# target). Auto-skips (success) where gcc -fanalyzer is unavailable.
set -u

CC="${CC:-gcc}"
if ! "$CC" -fanalyzer -x c -c /dev/null -o /dev/null >/dev/null 2>&1; then
    echo "analyze=SKIPPED (no working '$CC -fanalyzer')"
    exit 0
fi

COMMON="-DCORTEX_M0 -std=c99 -I. -Isrc -Ivendor/libdivsufsort -fanalyzer \
        -Wno-analyzer-tainted-assertion -c -o /dev/null"
log="$(mktemp)"
trap 'rm -f "$log"' EXIT
rc=0

analyze() { # analyze <src> <extra-defines>
    echo "  analyzing $1"
    # shellcheck disable=SC2086
    "$CC" $2 $COMMON "$1" 2>>"$log" || rc=1
}

analyze src/patch_generate.c   "-DRC_V3_ENC_MAIN"          # host encoder (+main)
analyze src/arm_cortex_m4.c    ""                          # ARM reloc scanner/packers
analyze src/patch_selfcheck.c  ""                          # encoder self-verify (includes decoder)
analyze src/patch_apply_demo.c "-D_POSIX_C_SOURCE=200809L" # device decoder demo

w="$(grep -c 'warning:' "$log" 2>/dev/null || true)"
if [ "$rc" -ne 0 ] || [ "$w" -ne 0 ]; then
    echo "analyze=FAIL ($w finding(s))"
    grep -E 'warning:|error:' "$log" | head -60
    exit 1
fi
echo "analyze=OK (4 TUs, 0 findings)"
