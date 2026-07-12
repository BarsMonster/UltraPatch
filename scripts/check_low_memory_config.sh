#!/usr/bin/env bash
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

# Behavioral contract for a supported wire profile whose literal-seed workspace is larger than
# the apply workspace. The matching-profile encoder produces real one-face patches, then both
# public decoder packaging forms apply them. The assertions make this specifically a regression
# for the arena union's seed-dominant branch rather than just another override smoke.
set -eu

: "${CC:?check_low_memory_config.sh: CC not set - invoke through make check-wire-config}"
: "${DECODER_CFLAGS:?check_low_memory_config.sh: DECODER_CFLAGS not set}"
: "${SINGLE_DECODER_CFLAGS:?check_low_memory_config.sh: SINGLE_DECODER_CFLAGS not set}"
: "${DECODER_SINGLE_HDR:?check_low_memory_config.sh: DECODER_SINGLE_HDR not set}"
: "${ULTRAPATCH:?check_low_memory_config.sh: ULTRAPATCH not set}"

[ -x "$ULTRAPATCH" ] || {
    echo "low-memory encoder is missing or not executable: $ULTRAPATCH" >&2
    exit 2
}
case "$DECODER_SINGLE_HDR" in
    /*) ;;
    *) echo "check_low_memory_config.sh: DECODER_SINGLE_HDR must be absolute" >&2; exit 2 ;;
esac
[ -f "$DECODER_SINGLE_HDR" ] || {
    echo "low-memory canonical decoder header is missing: $DECODER_SINGLE_HDR" >&2
    exit 2
}
case " $SINGLE_DECODER_CFLAGS " in
    *" -Isrc "*|*" -I src "*)
        echo "low-memory single-header flags must not search src" >&2
        exit 2
        ;;
esac
single_header_define="-DDECODER_SINGLE_HEADER=\"$DECODER_SINGLE_HDR\""

. "$(dirname "$0")/tempdir.sh"

cat > "$tmp/low_memory_assert.h" <<'EOF'
#if WINDOW_LOG != 9
#error "low-memory WINDOW_LOG override missing"
#endif
#ifdef DECODER_SINGLE_HEADER
#include DECODER_SINGLE_HEADER
#else
#include "patch_apply.h"
#endif
_Static_assert(sizeof(((up_Arena *)0)->seed) > sizeof(((up_Arena *)0)->apply),
               "low-memory test configuration must keep the arena seed-dominant");
_Static_assert(sizeof(up_Arena) == sizeof(((up_Arena *)0)->seed),
               "seed-dominant arena must use the seed member size");
EOF

# O0 keeps this behavioral leg cheap; production ARM code generation remains covered by
# check-arm/check-stack and the existing nondefault Cortex-M0+ wire-config build.
# shellcheck disable=SC2086
"$CC" $DECODER_CFLAGS -O0 -include "$tmp/low_memory_assert.h" \
    test-bench/decoder-contract.c -Wl,--gc-sections -o "$tmp/contract-source"
# shellcheck disable=SC2086
"$CC" $SINGLE_DECODER_CFLAGS -O0 "$single_header_define" \
    -include "$tmp/low_memory_assert.h" test-bench/decoder-contract.c \
    -Wl,--gc-sections -o "$tmp/contract-single"

FIX="${FIXTURES:-test-bench/fixtures}"
base="$FIX/v0_base/watch.bin"
one="$FIX/v1_one_face/watch.bin"
"$ULTRAPATCH" "$base" "$one" "$tmp/grow.blob" >/dev/null
"$ULTRAPATCH" "$one" "$base" "$tmp/revert.blob" >/dev/null

roundtrips=0
for direction in grow revert; do
    if [ "$direction" = grow ]; then from="$base"; to="$one"; else from="$one"; to="$base"; fi
    "$tmp/contract-source" success "$from" "$to" "$tmp/$direction.blob" \
        >"$tmp/$direction.source.out"
    "$tmp/contract-single" success "$from" "$to" "$tmp/$direction.blob" \
        >"$tmp/$direction.single.out"
    cmp "$tmp/$direction.source.out" "$tmp/$direction.single.out"
    roundtrips=$((roundtrips + 1))
done

[ "$roundtrips" -eq 2 ]
echo "wire_config_low_memory_roundtrips=$roundtrips"
echo "wire_config_low_memory=OK (WINDOW_LOG=9 seed-dominant arena; one-face x2; source + single)"
