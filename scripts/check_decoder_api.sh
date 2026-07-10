#!/usr/bin/env bash
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

# Run the same in-memory public-API contract against the source decoder headers and
# the generated single-header distribution.  A deliberately undersized de-relocation
# dictionary build additionally proves both clean (pre-write) and recovery-required
# (post-write) REJ_RESOURCE outcomes without weakening the production configuration.
set -eu

: "${CC:?check_decoder_api.sh: CC not set — invoke through make check-decoder-contract}"
: "${CFLAGS:?check_decoder_api.sh: CFLAGS not set — invoke through make check-decoder-contract}"

FIX="${FIXTURES:-test-bench/fixtures}"
base="$FIX/v0_base/watch.bin"
one="$FIX/v1_one_face/watch.bin"
. "$(dirname "$0")/tempdir.sh"

./ultrapatch "$base" "$one" "$tmp/grow.blob" >/dev/null
./ultrapatch "$one" "$base" "$tmp/revert.blob" >/dev/null

args="$base $one $tmp/grow.blob $one $base $tmp/revert.blob"

if [ "${DECODER_API_REGULAR:-1}" = 1 ]; then
    if ! awk '
        /^(static[[:space:]]|[A-Z][A-Z0-9_]*[[:space:]])/ && /\(/ {
            decl=$0
            sub(/\(.*/, "", decl)
            n=split(decl, word, /[[:space:]]+/)
            name=word[n]
            if(FILENAME ~ /patch_apply\.h$/)
                ok=(name ~ /^up_/ || name ~ /^patch_apply_/)
            else
                ok=(name ~ /^up_/ || name ~ /^rc_/)
            if(!ok){ print FILENAME ":" FNR ": unnamespaced private function " name; bad=1 }
        }
        END { exit bad ? 1 : 0 }
    ' src/rc_models.h src/patch_apply.h; then
        echo "decoder private function namespace audit failed" >&2
        exit 1
    fi
    echo "decoder_private_namespace_audit=OK (static + macro-prefixed declarations)"

    # A 256-byte prefix reaches a second de-relocation dictionary value in the first
    # output page. With a test-only cap of one this rejects before any buffered page is
    # committed; the full update reaches the same cap only after physical writes.
    dd if="$base" of="$tmp/prefix.from" bs=256 count=1 status=none
    dd if="$one" of="$tmp/prefix.to" bs=256 count=1 status=none
    ./ultrapatch "$tmp/prefix.from" "$tmp/prefix.to" "$tmp/prefix.blob" >/dev/null

    python3 scripts/gen_single_header.py "$tmp/patch_apply_single.h" \
        src/patch_config.h src/rc_models.h src/patch_apply.h

    # These are behavioral harnesses; production -O2 and -Os compilation is already enforced
    # by the portable/stack/ARM legs.  -O0 materially reduces concurrent gate compile load.
    common="$CFLAGS -O0"
    "$CC" $common -c test-bench/decoder-collision.c -o "$tmp/collision-source.o"
    "$CC" $common -DDECODER_SINGLE_HEADER -I"$tmp" \
        -c test-bench/decoder-collision.c -o "$tmp/collision-single.o"
    echo "decoder_namespace_contract=OK (source + single header)"
    "$CC" $common test-bench/nvm-geometry-probe.c -o "$tmp/nvm-geometry-probe"
    "$tmp/nvm-geometry-probe"
    "$CC" $common test-bench/decoder-contract.c -Wl,--gc-sections -o "$tmp/contract-source"
    "$CC" $common -DDECODER_SINGLE_HEADER -I"$tmp" test-bench/decoder-contract.c \
        -Wl,--gc-sections -o "$tmp/contract-single"

    "$tmp/contract-source" $args >"$tmp/source.out"
    "$tmp/contract-single" $args >"$tmp/single.out"
    cmp "$tmp/source.out" "$tmp/single.out"

    capflags="-UDR_KCAP_BL -UDR_KCAP_EX -DDR_KCAP_BL=1 -DDR_KCAP_EX=1"
    "$CC" $common $capflags test-bench/decoder-contract.c -Wl,--gc-sections -o "$tmp/cap-source"
    "$CC" $common $capflags -DDECODER_SINGLE_HEADER -I"$tmp" test-bench/decoder-contract.c \
        -Wl,--gc-sections -o "$tmp/cap-single"
    for decoder in "$tmp/cap-source" "$tmp/cap-single"; do
        "$decoder" resource-clean "$tmp/prefix.from" "$tmp/prefix.to" "$tmp/prefix.blob" >/dev/null
        "$decoder" resource-touched "$base" "$one" "$tmp/grow.blob" >/dev/null
    done

    # A nonzero absolute base plus a one-page partition exercises address translation and the
    # oversized-envelope guard in both distributed header forms. Before the guard this crafted
    # envelope caused one out-of-range CRC read; it must now reject with zero flash accesses.
    capacityflags="-UPATCH_IMAGE_BASE -UPATCH_IMAGE_CAPACITY \
        -DPATCH_IMAGE_BASE=0x08000000u -DPATCH_IMAGE_CAPACITY=256u"
    "$CC" $common $capacityflags test-bench/decoder-contract.c \
        -Wl,--gc-sections -o "$tmp/capacity-source"
    "$CC" $common $capacityflags -DDECODER_SINGLE_HEADER -I"$tmp" \
        test-bench/decoder-contract.c -Wl,--gc-sections -o "$tmp/capacity-single"
    "$tmp/capacity-source" capacity >"$tmp/capacity-source.out"
    "$tmp/capacity-single" capacity >"$tmp/capacity-single.out"
    cmp "$tmp/capacity-source.out" "$tmp/capacity-single.out"
    "$tmp/capacity-source" success "$tmp/prefix.from" "$tmp/prefix.to" \
        "$tmp/prefix.blob" >"$tmp/nonzero-source.out"
    "$tmp/capacity-single" success "$tmp/prefix.from" "$tmp/prefix.to" \
        "$tmp/prefix.blob" >"$tmp/nonzero-single.out"
    cmp "$tmp/nonzero-source.out" "$tmp/nonzero-single.out"

    cat "$tmp/source.out"
    cat "$tmp/capacity-source.out"
    cat "$tmp/nonzero-source.out"
    echo "decoder_resource_contract=OK (clean + touched; source + single header)"
fi

# The backend and byte callback are the only pointer-rich code in this contract.  One
# source-header run under ASan+UBSan catches harness/decoder boundary mistakes.  It is a
# standalone target rather than concurrent with the CPU-saturated 290-pair gate.
if [ "${DECODER_API_SANITIZE:-0}" = 1 ]; then
    "$CC" $CFLAGS -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
        -fno-sanitize-recover=all test-bench/decoder-contract.c -o "$tmp/contract-sanitize"
    ASAN_OPTIONS=detect_leaks=1 "$tmp/contract-sanitize" $args >/dev/null
    echo "decoder_sanitizers=OK (ASan + UBSan)"
fi
