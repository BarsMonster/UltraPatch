#!/usr/bin/env bash
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

# End-to-end regression for the Makefile's shared wire override path. The caller recursively
# rebuilds the full host tool with nondefault WIRE_CONFIG_FLAGS, compiles the public decoder
# header set for host and Cortex-M0+, and proves the alternate encoder changes the expected wire.
set -eu

: "${CC:?check_wire_config.sh: CC not set - invoke through make check-wire-config}"
: "${CFLAGS:?check_wire_config.sh: CFLAGS not set - invoke through make check-wire-config}"
: "${DECODER_CFLAGS:?check_wire_config.sh: DECODER_CFLAGS not set - invoke through make check-wire-config}"
: "${DEC_STANDALONE_SRCS:?check_wire_config.sh: DEC_STANDALONE_SRCS not set - invoke through make check-wire-config}"
: "${DEC_DEMO_DEFINES:?check_wire_config.sh: DEC_DEMO_DEFINES not set - invoke through make check-wire-config}"
: "${ARM_CC:?check_wire_config.sh: ARM_CC not set - invoke through make check-wire-config}"
: "${ARM_SIZE:?check_wire_config.sh: ARM_SIZE not set - invoke through make check-wire-config}"
: "${ARM_OBJDUMP:?check_wire_config.sh: ARM_OBJDUMP not set - invoke through make check-wire-config}"
: "${ARM_DEC_FLAGS:?check_wire_config.sh: ARM_DEC_FLAGS not set - invoke through make check-wire-config}"
: "${ARM_OBJECT_OPT:?check_wire_config.sh: ARM_OBJECT_OPT not set - invoke through make check-wire-config}"
: "${ARM_BSS_HARD_CAP:?check_wire_config.sh: ARM_BSS_HARD_CAP not set - invoke through make check-wire-config}"
: "${ARM_LINK_STUBS:?check_wire_config.sh: ARM_LINK_STUBS not set - invoke through make check-wire-config}"
: "${ARM_LINK_FLAGS:?check_wire_config.sh: ARM_LINK_FLAGS not set - invoke through make check-wire-config}"
: "${ARM_LINK_LIBS:?check_wire_config.sh: ARM_LINK_LIBS not set - invoke through make check-wire-config}"
: "${DECODER_INTEGRATION_TU:?check_wire_config.sh: DECODER_INTEGRATION_TU not set - invoke through make check-wire-config}"
: "${DEFAULT_ULTRAPATCH:?check_wire_config.sh: DEFAULT_ULTRAPATCH not set - invoke through make check-wire-config}"
: "${ULTRAPATCH:?check_wire_config.sh: ULTRAPATCH not set - invoke through make check-wire-config}"

[ -x "$DEFAULT_ULTRAPATCH" ] || {
    echo "default encoder is missing or not executable: $DEFAULT_ULTRAPATCH" >&2
    exit 2
}
[ -x "$ULTRAPATCH" ] || {
    echo "alternate encoder is missing or not executable: $ULTRAPATCH" >&2
    exit 2
}
[ "$DEFAULT_ULTRAPATCH" != "$ULTRAPATCH" ] || {
    echo "wire-config probe reused the default-profile encoder" >&2
    exit 2
}

. "$(dirname "$0")/tempdir.sh"

FIX="${FIXTURES:-test-bench/fixtures}"
base="$FIX/v0_base/watch.bin"
one="$FIX/v1_one_face/watch.bin"

cat > "$tmp/wire_config_assert.h" <<'EOF'
#if !defined(CORTEX_M0) || defined(CORTEX_M4)
#error "shared target-family override did not reach this compilation path"
#endif
#if MAX_IMAGE != 1048576u
#error "MAX_IMAGE override missing"
#endif
#if WINDOW_LOG != 11
#error "WINDOW_LOG override missing"
#endif
#if JSLOTS != 769u
#error "JSLOTS override missing"
#endif
#if OPC_CAP != 81
#error "OPC_CAP override missing"
#endif
#if OUTROW != 128u
#error "OUTROW override missing"
#endif
#if OUTROW_DEPTH != 4u
#error "OUTROW_DEPTH override missing"
#endif
#if DR_KCAP_BL != 209u || DR_KCAP_EX != 129u
#error "DR_KCAP_* override missing"
#endif
EOF

cat > "$tmp/encoder_wire_config_assert.h" <<'EOF'
#include "patch_config.h"
#include "wire_config_assert.h"
#ifdef PATCH_IMAGE_BASE
#error "PATCH_IMAGE_BASE is decoder-only and leaked into the encoder compile path"
#endif
#ifdef PATCH_IMAGE_CAPACITY
#error "PATCH_IMAGE_CAPACITY is decoder-only and leaked into the encoder compile path"
#endif
EOF

cat > "$tmp/decoder_wire_config_assert.h" <<'EOF'
#include "patch_config.h"
#include "wire_config_assert.h"
#if !defined(PATCH_IMAGE_BASE) || PATCH_IMAGE_BASE != 0u
#error "repository decoder integration flags did not reach the decoder compile path"
#endif
#if !defined(PATCH_IMAGE_CAPACITY) || PATCH_IMAGE_CAPACITY != 67108864u
#error "repository decoder capacity did not reach the decoder compile path"
#endif
EOF

# shellcheck disable=SC2086
"$CC" $CFLAGS -I"$tmp" -include "$tmp/encoder_wire_config_assert.h" \
    -c src/patch_generate.c -o "$tmp/patch_generate.o"
cat > "$tmp/source_wire_config.c" <<'EOF'
#include <stdint.h>
#include "patch_apply.h"
uint8_t flash_read(uint32_t addr){ (void)addr; return 0xffu; }
void flash_write_page(uint32_t addr, const uint8_t page[OUTROW]){ (void)addr; (void)page; }
static int next(void *ctx, uint8_t *out){ (void)ctx; (void)out; return PATCH_PULL_END; }
int main(void){ PatchApply state; return patch_apply_run(&state,next,0); }
EOF
# shellcheck disable=SC2086
"$CC" $DECODER_CFLAGS -I"$tmp" -include "$tmp/decoder_wire_config_assert.h" \
    -c "$tmp/source_wire_config.c" -o "$tmp/source_wire_config.o"

# Cross-compile and link the actual product integration shape through the same tracked TU as
# check-arm/check-stack. Static PatchApply storage makes the alternate SRAM cost visible.
# shellcheck disable=SC2086
"$ARM_CC" $ARM_DEC_FLAGS $ARM_OBJECT_OPT -I"$tmp" -include "$tmp/wire_config_assert.h" \
    -DDECODER_INTEGRATION_STATIC -c "$DECODER_INTEGRATION_TU" -o "$tmp/arm_source.o"
# shellcheck disable=SC2086
"$ARM_CC" $ARM_DEC_FLAGS $ARM_OBJECT_OPT -c "$ARM_LINK_STUBS" -o "$tmp/arm_link_stubs.o"
# shellcheck disable=SC2086
"$ARM_CC" $ARM_LINK_FLAGS "$tmp/arm_source.o" "$tmp/arm_link_stubs.o" \
    $ARM_LINK_LIBS -o "$tmp/arm_source.elf"
arm_size_triplet(){ "$ARM_SIZE" "$1" | awk 'NR==2 { print $1 "/" $2 "/" $3 }'; }
arm_obj_source=$(arm_size_triplet "$tmp/arm_source.o")
arm_link_source=$(arm_size_triplet "$tmp/arm_source.elf")
check_arm_storage(){
    arm_form=$1
    arm_kind=$2
    arm_triplet=$3
    arm_data=${arm_triplet#*/}; arm_data=${arm_data%%/*}
    arm_bss=${arm_triplet##*/}
    if [ "$arm_data" -ne 0 ]; then
        echo "alternate ARM $arm_form $arm_kind .data must be zero: actual=$arm_data" >&2
        exit 1
    fi
    if [ "$arm_bss" -gt "$ARM_BSS_HARD_CAP" ]; then
        echo "alternate ARM $arm_form $arm_kind .bss cap exceeded: actual=$arm_bss cap=$ARM_BSS_HARD_CAP" >&2
        exit 1
    fi
}
check_arm_storage source object "$arm_obj_source"
check_arm_storage source linked "$arm_link_source"

"$ARM_OBJDUMP" -d "$tmp/arm_source.o" > "$tmp/arm_source.dump"
if grep -Eq '\b(udiv|sdiv)\b' "$tmp/arm_source.dump"; then
    echo "alternate ARM decoder contains a hardware divide instruction" >&2
    exit 1
fi
arm_soft_div=$(grep -Ec '__aeabi_.*div|__aeabi_.*mod' "$tmp/arm_source.dump" || true)
if [ "$arm_soft_div" -ne 0 ]; then
    echo "alternate ARM decoder has software divide/mod references: actual=$arm_soft_div" >&2
    exit 1
fi
arm_obj_bss=${arm_obj_source##*/}
arm_link_bss=${arm_link_source##*/}
arm_bss_margin=$((ARM_BSS_HARD_CAP - arm_link_bss))

# Behavioral decoders use the same public API harness as check-decoder-contract. Keep these at
# -O0: production code generation is covered by check-arm/check-stack, while this leg is about
# wire behavior and runs concurrently with the corpus gate.
# shellcheck disable=SC2086
"$CC" $DECODER_CFLAGS -O0 test-bench/decoder-contract.c \
    -Wl,--gc-sections -o "$tmp/contract-source"

gen_img(){ python3 scripts/synth_gen.py img "$@"; }
gen_role(){ python3 scripts/synth_gen.py role "$@"; }

# The three-byte repetition is exactly 1025 bytes behind the output frontier: one byte outside
# the default 1 KiB ring but inside this build's 2 KiB ring. An empty source prevents a source
# copy from masking that boundary. Besides the default-wire comparison below, an otherwise
# identical WINDOW_LOG=10 decoder must reject the alternate blob at this exact cap.
: > "$tmp/long.from"
gen_img "$tmp/long.prefix" 1025 rand 305419896
cp "$tmp/long.prefix" "$tmp/long.to"
dd if="$tmp/long.prefix" bs=1 count=3 status=none >> "$tmp/long.to"

# A 160-byte read-behind is outside two 128-byte rows but inside four, so the row fixture proves
# OUTROW_DEPTH=4 behavior rather than merely compiling it. The journal-degrade pair exercises the
# nondefault 769-slot cap.
gen_role "$tmp/row.from" from rshift 8192 555 128 6000 160
gen_role "$tmp/row.to"   to   rshift 8192 555 128 6000 160
python3 scripts/synth_gen.py pin "$tmp/degrade.from" from synth_journal_degrade
python3 scripts/synth_gen.py pin "$tmp/degrade.to"   to   synth_journal_degrade

roundtrips=0
encode_and_apply(){
    name=$1
    from=$2
    to=$3
    if [ "$name" = degrade ]; then
        DEGRADE_STATS=1 "$ULTRAPATCH" "$from" "$to" "$tmp/$name.blob" \
            >/dev/null 2>"$tmp/$name.encoder.log"
        grep -Eq '^DEGRADE .*deg_journal=1([[:space:]]|$)' "$tmp/$name.encoder.log"
    else
        "$ULTRAPATCH" "$from" "$to" "$tmp/$name.blob" >/dev/null
    fi
    "$tmp/contract-source" success "$from" "$to" "$tmp/$name.blob" \
        >"$tmp/$name.out"
    roundtrips=$((roundtrips + 1))
}

encode_and_apply oneface_grow "$base" "$one"
encode_and_apply oneface_revert "$one" "$base"
encode_and_apply long_window "$tmp/long.from" "$tmp/long.to"
encode_and_apply row "$tmp/row.from" "$tmp/row.to"
encode_and_apply degrade "$tmp/degrade.from" "$tmp/degrade.to"

# These source-header decoders differ from the full alternate build in exactly one cap each.
# Their clean cross-config rejection makes the long-window and four-row activation definitive.
# shellcheck disable=SC2086
"$CC" $DECODER_CFLAGS -O0 -UWINDOW_LOG -DWINDOW_LOG=10 $DEC_DEMO_DEFINES \
    $DEC_STANDALONE_SRCS -Wl,--gc-sections -o "$tmp/dec-w10"
# shellcheck disable=SC2086
"$CC" $DECODER_CFLAGS -O0 -UOUTROW_DEPTH -DOUTROW_DEPTH=2u $DEC_DEMO_DEFINES \
    $DEC_STANDALONE_SRCS -Wl,--gc-sections -o "$tmp/dec-d2"

expect_cross_config_reject(){
    decoder=$1
    name=$2
    from=$3
    blob=$4
    expected=$5
    cp "$from" "$tmp/$name.cross.mem"
    cross_rc=0
    "$decoder" --decode "$tmp/$name.cross.mem" "$blob" \
        >/dev/null 2>"$tmp/$name.cross.err" || cross_rc=$?
    [ "$cross_rc" -eq 1 ]
    cmp "$tmp/$name.cross.mem" "$from"
    grep -Eq "$expected" "$tmp/$name.cross.err"
}
expect_cross_config_reject "$tmp/dec-w10" long "$tmp/long.from" "$tmp/long_window.blob" \
    '^decode error - rejected \(reason=2: corrupt/truncated patch\)$'
expect_cross_config_reject "$tmp/dec-d2" row "$tmp/row.from" "$tmp/row.blob" \
    '^decode error - rejected \(reason=(1: resource cap exceeded - firmware larger than build sizing|2: corrupt/truncated patch)\)$'

# The row fixture must use the configured four 128-byte output rows without falling back to
# journal preservation. This also runs the full alternate host backend, not only the API harness.
cp "$tmp/row.from" "$tmp/row.mem"
"$ULTRAPATCH" --decode "$tmp/row.mem" "$tmp/row.blob" \
    >/dev/null 2>"$tmp/row.decoder.log"
cmp "$tmp/row.mem" "$tmp/row.to"
grep -Eq 'journal_used=0 slots \(cap=769\)' "$tmp/row.decoder.log"

cp "$tmp/degrade.from" "$tmp/degrade.mem"
"$ULTRAPATCH" --decode "$tmp/degrade.mem" "$tmp/degrade.blob" \
    >/dev/null 2>"$tmp/degrade.decoder.log"
cmp "$tmp/degrade.mem" "$tmp/degrade.to"
grep -Eq 'journal_used=769 slots \(cap=769\)' "$tmp/degrade.decoder.log"

"$DEFAULT_ULTRAPATCH" "$tmp/long.from" "$tmp/long.to" "$tmp/long.default.blob" \
    >/dev/null
if cmp -s "$tmp/long.default.blob" "$tmp/long_window.blob"; then
    echo "nondefault configuration emitted the default long-window wire" >&2
    exit 1
fi
default_long=$(wc -c < "$tmp/long.default.blob")
alternate_long=$(wc -c < "$tmp/long_window.blob")
[ "$alternate_long" -lt "$default_long" ] || {
    echo "2 KiB long-window fixture did not improve: $alternate_long >= $default_long" >&2
    exit 1
}

# Prove MAX_IMAGE is behavioral, not just a macro assertion: this build must reject an image one
# byte beyond its 1 MiB envelope cap before publishing a blob.
gen_img "$tmp/too-large.bin" 1048577 const 0xff
too_large_rc=0
"$ULTRAPATCH" "$tmp/too-large.bin" "$one" "$tmp/too-large.blob" \
    >/dev/null 2>"$tmp/too-large.err" || too_large_rc=$?
[ "$too_large_rc" -eq 2 ]
[ ! -e "$tmp/too-large.blob" ]
grep -Eq 'from image too large for this decoder build: 1048577 > 1048576$' \
    "$tmp/too-large.err"

# A test-only one-entry de-relocation dictionary gives both resource outcomes: the 256-byte
# prefix rejects while all four output rows are still buffered, whereas the full one-face update
# rejects after physical writes.
dd if="$base" of="$tmp/resource.from" bs=256 count=1 status=none
dd if="$one" of="$tmp/resource.to" bs=256 count=1 status=none
"$ULTRAPATCH" "$tmp/resource.from" "$tmp/resource.to" "$tmp/resource.blob" >/dev/null
capflags='-UDR_KCAP_BL -UDR_KCAP_EX -DDR_KCAP_BL=1 -DDR_KCAP_EX=1'
# shellcheck disable=SC2086
"$CC" $DECODER_CFLAGS -O0 $capflags test-bench/decoder-contract.c \
    -Wl,--gc-sections -o "$tmp/cap-source"
for mode in resource-clean resource-touched; do
    if [ "$mode" = resource-clean ]; then
        rfrom="$tmp/resource.from"; rto="$tmp/resource.to"; rblob="$tmp/resource.blob"
    else
        rfrom="$base"; rto="$one"; rblob="$tmp/oneface_grow.blob"
    fi
    "$tmp/cap-source" "$mode" "$rfrom" "$rto" "$rblob" >"$tmp/$mode.out"
done

[ "$roundtrips" -eq 5 ]
echo "wire_config_roundtrips=$roundtrips (one-face x2 + long-window + row + degrade)"
echo "wire_config_long_window=$alternate_long/$default_long (alternate/default; W10=REJECT)"
echo "wire_config_row_depth=OK (D4 journal=0; D2=REJECT)"
echo "wire_config_max_image=OK (1048577 rejected by 1048576 cap)"
echo "wire_config_resource=OK (clean + touched)"
echo "wire_config_arm=$arm_obj_source object $arm_link_source linked (divide=0; bss_margin=$arm_bss_margin)"
echo "wire_config_override=OK (full encoder + host/ARM public-header decoders; behavioral)"
