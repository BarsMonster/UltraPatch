#!/usr/bin/env bash
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT
#
# Static-analysis leg: gcc -fanalyzer over first-party translation units
# (host encoder CLI/modules + device decoder wrapper + helpers).
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
count=0

analyze() { # analyze <src> <extra-defines>
    echo "  analyzing $1"
    count=$((count + 1))
    # shellcheck disable=SC2086
    "$CC" $2 $COMMON "$1" 2>>"$log" || rc=1
}

analyze src/patch_generate.c   "-DULTRAPATCH_MAIN"         # unified host CLI main
analyze src/enc_util.c         ""                          # host encoder utilities
analyze src/enc_elf.c          ""                          # host ELF scanner
analyze src/enc_bsdiff.c       ""                          # host diff planner
analyze src/enc_field.c        ""                          # host field/apply planner
analyze src/enc_rc.c           ""                          # host range encoder models
analyze src/enc_lz.c           ""                          # host LZ planner/pricer
analyze src/enc_emit.c         ""                          # host body emitter
analyze src/enc_plan.c         ""                          # host plan/degrade sweep
analyze src/arm_cortex_m4.c    ""                          # ARM reloc scanner/packers
analyze src/patch_selfcheck.c  ""                          # encoder self-verify (includes decoder)
analyze src/patch_apply_demo.c "-D_POSIX_C_SOURCE=200809L" # device decoder demo

w="$(grep -c 'warning:' "$log" 2>/dev/null || true)"
if [ "$rc" -ne 0 ] || [ "$w" -ne 0 ]; then
    echo "analyze=FAIL ($w finding(s))"
    grep -E 'warning:|error:' "$log" | head -60
    exit 1
fi
echo "analyze=OK ($count TUs, 0 findings)"
