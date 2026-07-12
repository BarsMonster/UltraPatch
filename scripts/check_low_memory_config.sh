#!/usr/bin/env bash
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

# Behavioral contract for a supported wire profile whose literal-seed workspace is larger than
# the apply workspace. The matching-profile encoder produces real one-face patches and the public
# decoder applies them. The assertions target the arena union's seed-dominant branch.
set -eu

: "${CC:?check_low_memory_config.sh: CC not set - invoke through make check-wire-config}"
: "${DECODER_CFLAGS:?check_low_memory_config.sh: DECODER_CFLAGS not set}"
: "${ULTRAPATCH:?check_low_memory_config.sh: ULTRAPATCH not set}"

[ -x "$ULTRAPATCH" ] || {
    echo "low-memory encoder is missing or not executable: $ULTRAPATCH" >&2
    exit 2
}
. "$(dirname "$0")/tempdir.sh"

cat > "$tmp/low_memory_assert.h" <<'EOF'
#if WINDOW_LOG != 9
#error "low-memory WINDOW_LOG override missing"
#endif
#if JSLOTS != 600u
#error "low-memory JSLOTS override missing"
#endif
#include "patch_apply.h"
_Static_assert(sizeof(((up_Arena *)0)->seed) > sizeof(((up_Arena *)0)->apply),
               "low-memory test configuration must keep the arena seed-dominant");
_Static_assert(sizeof(up_Arena) == sizeof(((up_Arena *)0)->seed),
               "seed-dominant arena must use the seed member size");
EOF

# O0 keeps this behavioral leg cheap; production ARM code generation remains covered by
# check-arm/check-stack and the existing nondefault Cortex-M0+ wire-config build.
# shellcheck disable=SC2086
"$CC" $DECODER_CFLAGS -O0 -include "$tmp/low_memory_assert.h" \
    test-bench/decoder-contract.c -Wl,--gc-sections -o "$tmp/contract"

FIX="${FIXTURES:-test-bench/fixtures}"
base="$FIX/v0_base/watch.bin"
one="$FIX/v1_one_face/watch.bin"
"$ULTRAPATCH" "$base" "$one" "$tmp/grow.blob" >/dev/null
"$ULTRAPATCH" "$one" "$base" "$tmp/revert.blob" >/dev/null

roundtrips=0
for direction in grow revert; do
    if [ "$direction" = grow ]; then from="$base"; to="$one"; else from="$one"; to="$base"; fi
    "$tmp/contract" success "$from" "$to" "$tmp/$direction.blob" \
        >"$tmp/$direction.out"
    roundtrips=$((roundtrips + 1))
done

[ "$roundtrips" -eq 2 ]
echo "wire_config_low_memory_roundtrips=$roundtrips"
echo "wire_config_low_memory=OK (WINDOW_LOG=9 JSLOTS=600 seed-dominant arena; one-face x2)"
