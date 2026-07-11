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
: "${SINGLE_DECODER_CFLAGS:?check_decoder_api.sh: SINGLE_DECODER_CFLAGS not set}"
: "${DECODER_SINGLE_HDR:?check_decoder_api.sh: DECODER_SINGLE_HDR not set}"
: "${DECODER_PUBLIC_HDRS:?check_decoder_api.sh: DECODER_PUBLIC_HDRS not set}"
: "${ULTRAPATCH:?check_decoder_api.sh: ULTRAPATCH not set; invoke through make check-decoder-contract}"
NM=${NM:-nm}
[ -x "$ULTRAPATCH" ] || {
    echo "check_decoder_api.sh: ULTRAPATCH is missing or not executable: $ULTRAPATCH" >&2
    exit 2
}
case "$DECODER_SINGLE_HDR" in
    /*) ;;
    *) echo "check_decoder_api.sh: DECODER_SINGLE_HDR must be absolute" >&2; exit 2 ;;
esac
[ -f "$DECODER_SINGLE_HDR" ] || {
    echo "check_decoder_api.sh: canonical decoder header is missing: $DECODER_SINGLE_HDR" >&2
    exit 2
}
case " $SINGLE_DECODER_CFLAGS " in
    *" -Isrc "*|*" -I src "*)
        echo "check_decoder_api.sh: single-header flags must not search src" >&2
        exit 2
        ;;
esac
single_header_define="-DDECODER_SINGLE_HEADER=\"$DECODER_SINGLE_HDR\""

FIX="${FIXTURES:-test-bench/fixtures}"
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

    # A 256-byte prefix reaches a second de-relocation dictionary value in the first
    # output page. With a test-only cap of one this rejects before any buffered page is
    # committed; the full update reaches the same cap only after physical writes.
    dd if="$base" of="$tmp/prefix.from" bs=256 count=1 status=none
    dd if="$one" of="$tmp/prefix.to" bs=256 count=1 status=none
    "$ULTRAPATCH" "$tmp/prefix.from" "$tmp/prefix.to" "$tmp/prefix.blob" >/dev/null

    # These are behavioral harnesses; production -O2 and -Os compilation is already enforced
    # by the portable/stack/ARM legs.  -O0 materially reduces concurrent gate compile load.
    common="$CFLAGS -O0"
    single_common="$SINGLE_DECODER_CFLAGS -O0"

    # Compile the decoder exactly as an integrator can: the only PatchApply object is an
    # automatic local in decoder_compiled_contract_apply().  Auditing the resulting objects
    # closes the gap left by the lexical no-global/no-allocation checks above: macros and
    # attributes can hide storage or allocator calls from a source grep.
    "$CC" $common -c test-bench/decoder-compiled-contract.c \
        -o "$tmp/compiled-contract-source.o"
    "$CC" $single_common "$single_header_define" \
        -c test-bench/decoder-compiled-contract.c \
        -o "$tmp/compiled-contract-single.o"

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
    audit_decoder_object "$tmp/compiled-contract-source.o" source-header
    audit_decoder_object "$tmp/compiled-contract-single.o" single-header

    # Exercise all three supported linkage modes against both packaging forms.  The
    # declarations-only TUs compile under the project -Werror policy and must refer to,
    # rather than define, patch_apply_run.  The implementation object must be the sole
    # decoder export, and linking a second copy must fail on that strong external symbol.
    keep_inline_flag=
    if printf '%s\n' 'int decoder_keep_inline_probe;' | \
         "$CC" $CFLAGS -fkeep-inline-functions -x c -c -o /dev/null - \
         >/dev/null 2>&1; then
        keep_inline_flag=-fkeep-inline-functions
    fi

    audit_declarations_object(){
        decl_obj=$1
        decl_label=$2
        decl_exports=$(LC_ALL=C "$NM" -g --defined-only -P "$decl_obj" | \
            awk '{ print $1 " " $2 }')
        if [ "$decl_exports" != "decoder_linkage_call_one T" ]; then
            echo "$decl_label declarations object has unexpected exports:" >&2
            printf '%s\n' "$decl_exports" >&2
            exit 1
        fi
        decl_private=$(LC_ALL=C "$NM" -a -P "$decl_obj" | \
            awk '$2 ~ /^[Tt]$/ && ($1 ~ /^(up_|rc_)/ || $1 == "patch_apply_run") { print $1 " " $2 }')
        if [ -n "$decl_private" ]; then
            echo "$decl_label declarations object emitted decoder-private text:" >&2
            printf '%s\n' "$decl_private" >&2
            exit 1
        fi
        decl_writable=$(LC_ALL=C "$NM" -a -P "$decl_obj" | \
            awk '$2 ~ /^[BbCcDdGgSs]$/ { print $1 " " $2 }')
        if [ -n "$decl_writable" ]; then
            echo "$decl_label declarations object defines writable storage:" >&2
            printf '%s\n' "$decl_writable" >&2
            exit 1
        fi
        decl_undefined=$(LC_ALL=C "$NM" -u -P "$decl_obj" | awk '{ print $1 }' | LC_ALL=C sort -u)
        if [ "$decl_undefined" != patch_apply_run ]; then
            echo "$decl_label declarations object has unexpected dependencies:" >&2
            printf '%s\n' "$decl_undefined" >&2
            exit 1
        fi
    }

    for form in source single; do
        if [ "$form" = source ]; then
            linkage_flags="$common"
        else
            linkage_flags="$single_common $single_header_define"
        fi
        linkage_src=test-bench/decoder-linkage-contract.c
        "$CC" $linkage_flags -DDECODER_LINKAGE_STATIC "$linkage_src" \
            -Wl,--gc-sections -o "$tmp/linkage-static-$form"
        "$tmp/linkage-static-$form"
        "$CC" $linkage_flags -DDECODER_LINKAGE_IMPLEMENTATION -c "$linkage_src" \
            -o "$tmp/linkage-implementation-$form.o"
        "$CC" $linkage_flags -DDECODER_LINKAGE_CALLER_ONE -c "$linkage_src" \
            -o "$tmp/linkage-caller-one-$form.o"
        "$CC" $linkage_flags -DDECODER_LINKAGE_CALLER_TWO -c "$linkage_src" \
            -o "$tmp/linkage-caller-two-$form.o"
        "$CC" $linkage_flags $keep_inline_flag -DNO_GNU_EXTENSIONS \
            -DDECODER_LINKAGE_CALLER_ONE \
            -c "$linkage_src" -o "$tmp/linkage-caller-one-$form-portable.o"
        "$CC" $linkage_flags -DDECODER_LINKAGE_IMPLEMENTATION_TWICE -c "$linkage_src" \
            -o "$tmp/linkage-implementation-twice-$form.o"
        "$CC" $linkage_flags -DDECODER_LINKAGE_DECLARATIONS_TWICE -c "$linkage_src" \
            -o "$tmp/linkage-declarations-twice-$form.o"
        "$CC" $linkage_flags -UPATCH_IMAGE_BASE -UPATCH_IMAGE_CAPACITY \
            -DPATCH_IMAGE_BASE=0xffffff00u -DPATCH_IMAGE_CAPACITY=256u \
            -DDECODER_LINKAGE_CALLER_ONE -c "$linkage_src" \
            -o "$tmp/linkage-top-page-$form.o"
        # Compile an otherwise identical valid capacity immediately before the
        # negative probe.  In the project's C99 compatibility mode some libc/compiler
        # combinations implement _Static_assert with a negative-width declaration and
        # therefore do not preserve its message in diagnostics.
        "$CC" $linkage_flags -UPATCH_IMAGE_CAPACITY -DPATCH_IMAGE_CAPACITY=256u \
            -DDECODER_LINKAGE_CALLER_ONE -c "$linkage_src" \
            -o "$tmp/linkage-valid-geometry-$form.o"
        if "$CC" $linkage_flags -UPATCH_IMAGE_CAPACITY -DPATCH_IMAGE_CAPACITY=257u \
             -DDECODER_LINKAGE_CALLER_ONE -c "$linkage_src" \
             -o "$tmp/linkage-invalid-geometry-$form.o" \
             >"$tmp/linkage-invalid-geometry-$form.out" \
             2>"$tmp/linkage-invalid-geometry-$form.err"; then
            echo "$form declarations-only mode accepted invalid image geometry" >&2
            exit 1
        fi
        for caller in one two; do
            role=$(printf '%s' "$caller" | tr '[:lower:]' '[:upper:]')
            "$CC" $linkage_flags $keep_inline_flag \
                -DDECODER_LINKAGE_CALLER_$role -c "$linkage_src" \
                -o "$tmp/linkage-caller-$caller-$form-keep.o"
        done

        audit_declarations_object "$tmp/linkage-caller-one-$form.o" "$form"
        audit_declarations_object "$tmp/linkage-caller-one-$form-keep.o" "$form keep-inline"
        audit_declarations_object "$tmp/linkage-caller-one-$form-portable.o" "$form portable"

        implementation_exports=$(LC_ALL=C "$NM" -g --defined-only -P \
            "$tmp/linkage-implementation-$form.o" | awk '{ print $1 " " $2 }')
        if [ "$implementation_exports" != "patch_apply_run T" ]; then
            echo "$form external implementation has unexpected exports:" >&2
            printf '%s\n' "$implementation_exports" >&2
            exit 1
        fi
        implementation_writable=$(LC_ALL=C "$NM" -a -P \
            "$tmp/linkage-implementation-$form.o" | \
            awk '$2 ~ /^[BbCcDdGgSs]$/ { print $1 " " $2 }')
        if [ -n "$implementation_writable" ]; then
            echo "$form external implementation defines writable storage:" >&2
            printf '%s\n' "$implementation_writable" >&2
            exit 1
        fi
        implementation_allocators=$(LC_ALL=C "$NM" -u -P \
            "$tmp/linkage-implementation-$form.o" | awk \
            '$1 ~ /^(malloc|calloc|realloc|reallocarray|free|cfree|aligned_alloc|posix_memalign|memalign|valloc|pvalloc)$/ { print $1 }')
        if [ -n "$implementation_allocators" ]; then
            echo "$form external implementation refers to dynamic allocation:" >&2
            printf '%s\n' "$implementation_allocators" >&2
            exit 1
        fi
        for caller in one two; do
            for suffix in '' -keep; do
                private_text=$(LC_ALL=C "$NM" -a -P \
                    "$tmp/linkage-caller-$caller-$form$suffix.o" | \
                    awk '$2 ~ /^[Tt]$/ && ($1 ~ /^(up_|rc_)/ || $1 == "patch_apply_run") { print $1 " " $2 }')
                if [ -n "$private_text" ]; then
                    echo "$form declarations-only caller $caller emitted decoder-private text$suffix:" >&2
                    printf '%s\n' "$private_text" >&2
                    exit 1
                fi
            done
            if ! LC_ALL=C "$NM" -u -P "$tmp/linkage-caller-$caller-$form.o" | \
                 awk '$1 == "patch_apply_run" { found=1 } END { exit found ? 0 : 1 }'; then
                echo "$form declarations-only caller $caller does not reference patch_apply_run" >&2
                exit 1
            fi
            if LC_ALL=C "$NM" -g --defined-only -P "$tmp/linkage-caller-$caller-$form.o" | \
                 awk '$1 == "patch_apply_run" { found=1 } END { exit found ? 0 : 1 }'; then
                echo "$form declarations-only caller $caller defines patch_apply_run" >&2
                exit 1
            fi
        done

        "$CC" $linkage_flags "$tmp/linkage-implementation-$form.o" \
            "$tmp/linkage-caller-one-$form.o" "$tmp/linkage-caller-two-$form.o" \
            -Wl,--gc-sections -o "$tmp/linkage-external-$form"
        "$tmp/linkage-external-$form"
        "$CC" $linkage_flags -DULTRAPATCH_DECLARATIONS_ONLY \
            -DDECODER_DECLARATIONS_CONTRACT -c test-bench/decoder-contract.c \
            -o "$tmp/contract-external-$form.o"
        "$CC" $linkage_flags "$tmp/linkage-implementation-$form.o" \
            "$tmp/contract-external-$form.o" -Wl,--gc-sections \
            -o "$tmp/contract-external-$form"
        "$tmp/contract-external-$form" $args > "$tmp/contract-external-$form.out"
        "$CC" $linkage_flags -DDECODER_LINKAGE_IMPLEMENTATION -c "$linkage_src" \
            -o "$tmp/linkage-implementation-duplicate-$form.o"
        duplicate_exports=$(LC_ALL=C "$NM" -g --defined-only -P \
            "$tmp/linkage-implementation-duplicate-$form.o" | awk '{ print $1 " " $2 }')
        if [ "$duplicate_exports" != "patch_apply_run T" ]; then
            echo "$form second external implementation has unexpected exports:" >&2
            printf '%s\n' "$duplicate_exports" >&2
            exit 1
        fi
        if "$CC" $linkage_flags "$tmp/linkage-implementation-$form.o" \
             "$tmp/linkage-implementation-duplicate-$form.o" \
             "$tmp/linkage-caller-one-$form.o" "$tmp/linkage-caller-two-$form.o" \
             -Wl,--gc-sections -o "$tmp/linkage-duplicate-$form" \
             >"$tmp/linkage-duplicate-$form.out" 2>"$tmp/linkage-duplicate-$form.err"; then
            echo "$form accepted two ULTRAPATCH_IMPLEMENTATION translation units" >&2
            exit 1
        fi
        if ! grep -q 'patch_apply_run' "$tmp/linkage-duplicate-$form.err"; then
            echo "$form duplicate-implementation link failed for an unexpected reason:" >&2
            cat "$tmp/linkage-duplicate-$form.err" >&2
            exit 1
        fi
        if "$CC" $linkage_flags -DDECODER_LINKAGE_CONFLICT -c "$linkage_src" \
             -o "$tmp/linkage-conflict-$form.o" \
             >"$tmp/linkage-conflict-$form.out" 2>"$tmp/linkage-conflict-$form.err"; then
            echo "$form accepted both decoder linkage modes in one translation unit" >&2
            exit 1
        fi
        if ! grep -q 'ULTRAPATCH_DECLARATIONS_ONLY and ULTRAPATCH_IMPLEMENTATION are mutually exclusive' \
             "$tmp/linkage-conflict-$form.err"; then
            echo "$form linkage-mode conflict failed for an unexpected reason:" >&2
            cat "$tmp/linkage-conflict-$form.err" >&2
            exit 1
        fi
    done
    cmp "$tmp/contract-external-source.out" "$tmp/contract-external-single.out"
    echo "decoder_linkage_contract=OK (static + declarations-only + one external implementation; source + single)"

    # Snapshot preprocessor state with the decoder's system prerequisites already loaded,
    # then include each packaging form. No existing consumer macro may be removed or changed.
    # The only additions are this exact public configuration/include-guard allowlist; every
    # decoder-private implementation macro must therefore be sealed by patch_apply.h.
    {
        printf '%s\n' '#include <stddef.h>'
        printf '%s\n' '#include <stdint.h>'
        printf '%s\n' '#include <string.h>'
    } > "$tmp/macro-before.c"
    {
        printf '%s\n' DR_KCAP_BL DR_KCAP_EX JSLOTS MAX_IMAGE OPC_CAP OUTROW OUTROW_DEPTH
        printf '%s\n' UP_PATCH_APPLY_H UP_PATCH_CONFIG_H UP_RC_MODELS_H WINDOW_LOG
    } | LC_ALL=C sort > "$tmp/public-macro-allowlist"

    "$CC" $common -dM -E "$tmp/macro-before.c" | LC_ALL=C sort > "$tmp/macros-source-before"
    "$CC" $common -dM -E test-bench/decoder-compiled-contract.c | \
        LC_ALL=C sort > "$tmp/macros-source-after"
    "$CC" $single_common "$single_header_define" -dM -E "$tmp/macro-before.c" | \
        LC_ALL=C sort > "$tmp/macros-single-before"
    "$CC" $single_common "$single_header_define" -dM -E \
        test-bench/decoder-compiled-contract.c | LC_ALL=C sort > "$tmp/macros-single-after"

    for form in source single; do
        comm -23 "$tmp/macros-$form-before" "$tmp/macros-$form-after" \
            > "$tmp/macros-$form-removed"
        if [ -s "$tmp/macros-$form-removed" ]; then
            echo "$form decoder header removed or changed consumer macros:" >&2
            cat "$tmp/macros-$form-removed" >&2
            exit 1
        fi
        comm -13 "$tmp/macros-$form-before" "$tmp/macros-$form-after" \
            > "$tmp/macros-$form-added"
        awk '$1 == "#define" { name=$2; sub(/\(.*/, "", name); print name }' \
            "$tmp/macros-$form-before" | LC_ALL=C sort -u > "$tmp/macros-$form-before-names"
        comm -23 "$tmp/public-macro-allowlist" "$tmp/macros-$form-before-names" \
            > "$tmp/macros-$form-expected-names"
        awk '$1 == "#define" { name=$2; sub(/\(.*/, "", name); print name }' \
            "$tmp/macros-$form-added" | LC_ALL=C sort -u > "$tmp/macros-$form-added-names"
        if ! cmp -s "$tmp/macros-$form-expected-names" "$tmp/macros-$form-added-names"; then
            echo "$form decoder header macro delta is outside the public allowlist" >&2
            echo "expected added macro names:" >&2
            cat "$tmp/macros-$form-expected-names" >&2
            echo "actual added macros:" >&2
            cat "$tmp/macros-$form-added" >&2
            exit 1
        fi
    done
    if ! cmp -s "$tmp/macros-source-added" "$tmp/macros-single-added"; then
        echo "source and single-header decoder macro deltas differ" >&2
        diff -u "$tmp/macros-source-added" "$tmp/macros-single-added" >&2 || :
        exit 1
    fi
    echo "decoder_compiled_contract=OK (O0 source + single: automatic state, symbols + macros)"

    "$CC" $common -c test-bench/decoder-collision.c -o "$tmp/collision-source.o"
    "$CC" $single_common "$single_header_define" \
        -c test-bench/decoder-collision.c -o "$tmp/collision-single.o"
    echo "decoder_namespace_contract=OK (source + single header)"
    "$CC" $common test-bench/nvm-geometry-probe.c -o "$tmp/nvm-geometry-probe"
    "$tmp/nvm-geometry-probe"
    "$CC" $common test-bench/decoder-contract.c -Wl,--gc-sections -o "$tmp/contract-source"
    "$CC" $single_common "$single_header_define" test-bench/decoder-contract.c \
        -Wl,--gc-sections -o "$tmp/contract-single"

    "$tmp/contract-source" $args >"$tmp/source.out"
    "$tmp/contract-single" $args >"$tmp/single.out"
    cmp "$tmp/source.out" "$tmp/single.out"
    grep -vE '^decoder_(src|ldr)_window=' "$tmp/source.out" > "$tmp/default-public.out"
    cmp "$tmp/default-public.out" "$tmp/contract-external-source.out"

    capflags="-UDR_KCAP_BL -UDR_KCAP_EX -DDR_KCAP_BL=1 -DDR_KCAP_EX=1"
    "$CC" $common $capflags test-bench/decoder-contract.c -Wl,--gc-sections -o "$tmp/cap-source"
    "$CC" $single_common $capflags "$single_header_define" test-bench/decoder-contract.c \
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
    "$CC" $single_common $capacityflags "$single_header_define" \
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
