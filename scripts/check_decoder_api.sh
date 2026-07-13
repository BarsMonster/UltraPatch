#!/usr/bin/env bash
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

# Run the in-memory public-API contract against the shipped decoder header set. A deliberately
# undersized de-relocation dictionary build additionally proves both clean (pre-write) and
# recovery-required (post-write) REJ_RESOURCE outcomes without weakening production settings.
set -eu

: "${CC:?check_decoder_api.sh: CC not set — invoke through make check-decoder-contract}"
: "${CFLAGS:?check_decoder_api.sh: CFLAGS not set — invoke through make check-decoder-contract}"
: "${DECODER_PUBLIC_HDRS:?check_decoder_api.sh: DECODER_PUBLIC_HDRS not set}"
: "${ULTRAPATCH:?check_decoder_api.sh: ULTRAPATCH not set; invoke through make check-decoder-contract}"
: "${FIXTURES:?check_decoder_api.sh: FIXTURES not set to the build-local corpus}"
NM=${NM:-nm}
[ -x "$ULTRAPATCH" ] || {
    echo "check_decoder_api.sh: ULTRAPATCH is missing or not executable: $ULTRAPATCH" >&2
    exit 2
}
FIX="$FIXTURES"
base="$FIX/v0_base/watch.bin"
one="$FIX/v1_one_face/watch.bin"
. "$(dirname "$0")/tempdir.sh"

"$ULTRAPATCH" "$base" "$one" "$tmp/grow.blob" >/dev/null
"$ULTRAPATCH" "$one" "$base" "$tmp/revert.blob" >/dev/null

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
    ' $DECODER_PUBLIC_HDRS; then
        echo "decoder private function namespace audit failed" >&2
        exit 1
    fi
    echo "decoder_private_namespace_audit=OK (static + macro-prefixed declarations)"

    # Exercise the installed-header contract without a repository include path. Integrators
    # include only patch_apply.h, while patch_config.h and rc_models.h live beside it so the
    # entrypoint's normal local includes resolve.
    header_dir="$tmp/public-headers"
    mkdir "$header_dir"
    cp $DECODER_PUBLIC_HDRS "$header_dir/"
    cp test-bench/decoder-compiled-contract.c "$header_dir/consumer.c"
    ( cd "$header_dir" && "$CC" -std=c11 -Wall -Wextra -Werror \
        -DPATCH_IMAGE_BASE=0u -DPATCH_IMAGE_CAPACITY=67108864u \
        -c consumer.c -o consumer.o )
    echo "decoder_header_install=OK (patch_apply.h + local support headers)"

    # A 256-byte prefix reaches a second de-relocation dictionary value in the first
    # output page. With a test-only cap of one this rejects before any buffered page is
    # committed; the full update reaches the same cap only after physical writes.
    dd if="$base" of="$tmp/prefix.from" bs=256 count=1 status=none
    dd if="$one" of="$tmp/prefix.to" bs=256 count=1 status=none
    "$ULTRAPATCH" "$tmp/prefix.from" "$tmp/prefix.to" "$tmp/prefix.blob" >/dev/null

    # These are behavioral harnesses; production -O2 and -Os compilation is already enforced
    # by the portable/stack/ARM legs.  -O0 materially reduces concurrent gate compile load.
    common="$CFLAGS -O0"

    # Compile the decoder exactly as an integrator can: the only PatchApply object is an
    # automatic local in decoder_compiled_contract_apply().  Auditing the resulting objects
    # closes the gap left by the lexical no-global/no-allocation checks above: macros and
    # attributes can hide storage or allocator calls from a source grep.
    "$CC" $common -c test-bench/decoder-compiled-contract.c \
        -o "$tmp/compiled-contract.o"
    "$CC" $common -DNO_GNU_EXTENSIONS -DHAND_ROLLED_MEMMOVE \
        -c test-bench/decoder-compiled-contract.c \
        -o "$tmp/compiled-contract-hand.o"

    audit_decoder_object(){
        obj=$1
        form=$2

        if ! LC_ALL=C "$NM" -a -P "$obj" > "$tmp/nm-$form-all" ||
           ! LC_ALL=C "$NM" -g --defined-only -P "$obj" > "$tmp/nm-$form-exports" ||
           ! LC_ALL=C "$NM" -u -P "$obj" > "$tmp/nm-$form-undefined"; then
            echo "could not inspect $form decoder object with NM=$NM" >&2
            exit 1
        fi

        # GNU/POSIX nm's B/C/D/G/S classes (lowercase for local symbols) are the
        # semantic writable-storage classes. TLS objects use B or D according to
        # whether their initial value is zero; weak/unique globals are caught by the
        # exported-symbol audit below. Read-only data is intentionally permitted.
        writable=$(awk '$2 ~ /^[BbCcDdGgSs]$/ { print $1 " " $2 }' \
            "$tmp/nm-$form-all")
        if [ -n "$writable" ]; then
            echo "$form decoder object defines writable/BSS/common/TLS storage:" >&2
            printf '%s\n' "$writable" >&2
            exit 1
        fi

        exports=$(awk '{ print $1 " " $2 }' "$tmp/nm-$form-exports")
        if [ "$exports" != "decoder_compiled_contract_apply T" ]; then
            echo "$form decoder object has unexpected externally visible symbols:" >&2
            printf '%s\n' "$exports" >&2
            exit 1
        fi

        undefined=$(awk '{ print $1 }' "$tmp/nm-$form-undefined" | LC_ALL=C sort -u)
        allocators=$(printf '%s\n' "$undefined" | awk '
            /^(malloc|calloc|realloc|reallocarray|free|cfree|aligned_alloc|posix_memalign|memalign|valloc|pvalloc)$/')
        if [ -n "$allocators" ]; then
            echo "$form decoder object refers to dynamic allocation:" >&2
            printf '%s\n' "$allocators" >&2
            exit 1
        fi
        unexpected=$(printf '%s\n' "$undefined" | awk '
            !/^(flash_read|flash_write_page|memcpy|memmove|memset|__stack_chk_fail|__stack_chk_fail_local)$/')
        if [ -n "$unexpected" ]; then
            echo "$form decoder object has unexpected undefined symbols:" >&2
            printf '%s\n' "$unexpected" >&2
            exit 1
        fi
    }
    audit_decoder_object "$tmp/compiled-contract.o" public-header
    audit_decoder_object "$tmp/compiled-contract-hand.o" hand-rolled
    if grep -Eq '^(memcpy|memmove)[[:space:]]' "$tmp/nm-hand-rolled-undefined"; then
        echo "hand-rolled decoder still refers to a library copy helper:" >&2
        grep -E '^(memcpy|memmove)[[:space:]]' "$tmp/nm-hand-rolled-undefined" >&2
        exit 1
    fi

    # The former multi-TU linkage macros are rejected explicitly. Without this contract an old
    # declarations-only caller could silently start compiling a private decoder copy after the
    # mode's removal.
    for mode in ULTRAPATCH_IMPLEMENTATION ULTRAPATCH_DECLARATIONS_ONLY; do
        if "$CC" $common -D"$mode" -c test-bench/decoder-compiled-contract.c \
             -o "$tmp/obsolete-$mode.o" \
             >"$tmp/obsolete-$mode.out" 2>"$tmp/obsolete-$mode.err"; then
            echo "decoder accepted removed linkage mode $mode" >&2
            exit 1
        fi
        if ! grep -q 'external decoder linkage was removed' "$tmp/obsolete-$mode.err"; then
            echo "decoder rejected $mode without the migration diagnostic" >&2
            cat "$tmp/obsolete-$mode.err" >&2
            exit 1
        fi
    done
    echo "decoder_linkage_contract=OK (internal header-only; obsolete external modes rejected)"

    # Snapshot preprocessor state with the decoder's system prerequisites and representative
    # former-private-name sentinels already loaded, then include the public entrypoint. No existing
    # consumer macro may be removed or changed.
    # The only additions are this exact public configuration/include-guard allowlist; every
    # decoder-private implementation macro must therefore be sealed by patch_apply.h.
    {
        printf '%s\n' '#include <stddef.h>'
        printf '%s\n' '#include <stdint.h>'
        printf '%s\n' '#include <string.h>'
        printf '%s\n' '#define RING 7u' '#define MASK 3u' '#define ARENA_BYTES 11u'
        printf '%s\n' '#define ULEB32_OVERFLOW(sh,b) ((sh)==(b))'
        printf '%s\n' '#define BT_PROBS 13u' '#define BT_BYTES 17u' '#define UG_CTX 19'
        printf '%s\n' '#define UG_C(x) ((x)+23)' '#define UG_GAMMA_MANT 29'
        printf '%s\n' '#define SMAP_CAP 31' '#define LIT0_CTX 37' '#define IDX_CTX 41'
        printf '%s\n' '#define DR_HIT_INIT 43u'
        printf '%s\n' '#define HAND_ROLLED_MEMMOVE'
    } > "$tmp/macro-before.c"
    cp "$tmp/macro-before.c" "$tmp/macro-after.c"
    printf '%s\n' '#include "patch_apply.h"' >> "$tmp/macro-after.c"
    {
        printf '%s\n' DR_KCAP_BL DR_KCAP_EX JSLOTS MAX_IMAGE OPC_CAP OUTROW OUTROW_DEPTH
        printf '%s\n' PATCH_WIRE_VERSION
        printf '%s\n' UP_PATCH_APPLY_H UP_PATCH_CONFIG_H UP_RC_MODELS_H WINDOW_LOG
    } | LC_ALL=C sort > "$tmp/public-macro-allowlist"

    "$CC" $common -dM -E "$tmp/macro-before.c" | LC_ALL=C sort > "$tmp/macros-before"
    "$CC" $common -dM -E "$tmp/macro-after.c" | \
        LC_ALL=C sort > "$tmp/macros-after"
    comm -23 "$tmp/macros-before" "$tmp/macros-after" > "$tmp/macros-removed"
    if [ -s "$tmp/macros-removed" ]; then
        echo "decoder header removed or changed consumer macros:" >&2
        cat "$tmp/macros-removed" >&2
        exit 1
    fi
    comm -13 "$tmp/macros-before" "$tmp/macros-after" > "$tmp/macros-added"
    awk '$1 == "#define" { name=$2; sub(/\(.*/, "", name); print name }' \
        "$tmp/macros-before" | LC_ALL=C sort -u > "$tmp/macros-before-names"
    comm -23 "$tmp/public-macro-allowlist" "$tmp/macros-before-names" \
        > "$tmp/macros-expected-names"
    awk '$1 == "#define" { name=$2; sub(/\(.*/, "", name); print name }' \
        "$tmp/macros-added" | LC_ALL=C sort -u > "$tmp/macros-added-names"
    if ! cmp -s "$tmp/macros-expected-names" "$tmp/macros-added-names"; then
        echo "decoder header macro delta is outside the public allowlist" >&2
        echo "expected added macro names:" >&2
        cat "$tmp/macros-expected-names" >&2
        echo "actual added macros:" >&2
        cat "$tmp/macros-added" >&2
        exit 1
    fi
    echo "decoder_compiled_contract=OK (O0: automatic state, symbols + macros)"

    if "$CC" $common -DWINDOW_LOG=9 -c test-bench/decoder-compiled-contract.c \
         -o "$tmp/override.o" >"$tmp/override.out" 2>"$tmp/override.err"; then
        echo "decoder accepted a production wire-constant override" >&2
        exit 1
    fi
    grep -q 'wire constants are owned by patch_config.h' "$tmp/override.err"
    echo "decoder_wire_constants=OK (shared config; production overrides rejected)"

    "$CC" $common test-bench/nvm-geometry-probe.c -o "$tmp/nvm-geometry-probe"
    "$tmp/nvm-geometry-probe"
    "$CC" $common test-bench/decoder-contract.c -Wl,--gc-sections -o "$tmp/contract"
    "$tmp/contract" $args >"$tmp/contract.out"
    "$CC" $common -DNO_GNU_EXTENSIONS -DHAND_ROLLED_MEMMOVE \
        test-bench/decoder-contract.c -Wl,--gc-sections -o "$tmp/contract-hand"
    "$tmp/contract-hand" $args >/dev/null
    echo "decoder_hand_rolled_memmove=OK (portable grow/revert + no library copy helper)"

    # A nonzero absolute base plus a one-page partition exercises address translation and the
    # oversized-envelope guard. Before the guard this crafted
    # envelope caused one out-of-range CRC read; it must now reject with zero flash accesses.
    capacityflags="-UPATCH_IMAGE_BASE -UPATCH_IMAGE_CAPACITY \
        -DPATCH_IMAGE_BASE=0x08000000u -DPATCH_IMAGE_CAPACITY=256u"
    "$CC" $common $capacityflags test-bench/decoder-contract.c \
        -Wl,--gc-sections -o "$tmp/capacity"
    "$tmp/capacity" capacity >"$tmp/capacity.out"
    "$tmp/capacity" success "$tmp/prefix.from" "$tmp/prefix.to" \
        "$tmp/prefix.blob" >"$tmp/nonzero.out"

    cat "$tmp/contract.out"
    cat "$tmp/capacity.out"
    cat "$tmp/nonzero.out"
    echo "decoder_resource_contract=OK (bounded delta caches + resident cap checks)"
fi

# The backend and byte callback are the only pointer-rich code in this contract.  One
# public-header run under ASan+UBSan catches harness/decoder boundary mistakes. It is a
# standalone target rather than concurrent with the CPU-saturated 290-pair gate.
if [ "${DECODER_API_SANITIZE:-0}" = 1 ]; then
    "$CC" $CFLAGS -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
        -fno-sanitize-recover=all test-bench/decoder-contract.c -o "$tmp/contract-sanitize"
    ASAN_OPTIONS=detect_leaks=1 "$tmp/contract-sanitize" $args >/dev/null
    "$CC" $CFLAGS -O1 -DHAND_ROLLED_MEMMOVE -fsanitize=address,undefined -fno-omit-frame-pointer \
        -fno-sanitize-recover=all test-bench/decoder-contract.c -o "$tmp/contract-sanitize-hand"
    ASAN_OPTIONS=detect_leaks=1 "$tmp/contract-sanitize-hand" $args >/dev/null
    echo "decoder_sanitizers=OK (ASan + UBSan; library + hand-rolled)"
fi
