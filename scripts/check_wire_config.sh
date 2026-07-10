#!/usr/bin/env bash
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

# Compile-time regression for the Makefile's shared wire override path. The caller recursively
# rebuilds this target with nondefault WIRE_CONFIG_FLAGS; these assertions must then hold in an
# actual encoder TU plus source, generated single-header, and Cortex-M0+ decoder forms.
set -eu

: "${CC:?check_wire_config.sh: CC not set - invoke through make check-wire-config}"
: "${CFLAGS:?check_wire_config.sh: CFLAGS not set - invoke through make check-wire-config}"
: "${DECODER_CFLAGS:?check_wire_config.sh: DECODER_CFLAGS not set - invoke through make check-wire-config}"
: "${SINGLE_DECODER_CFLAGS:?check_wire_config.sh: SINGLE_DECODER_CFLAGS not set - invoke through make check-wire-config}"
: "${ARM_CC:?check_wire_config.sh: ARM_CC not set - invoke through make check-wire-config}"
: "${ARM_DEC_FLAGS:?check_wire_config.sh: ARM_DEC_FLAGS not set - invoke through make check-wire-config}"

. "$(dirname "$0")/tempdir.sh"

cat > "$tmp/wire_config_assert.h" <<'EOF'
#if !defined(CORTEX_M0) || defined(CORTEX_M4)
#error "shared target-family override did not reach this compilation path"
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
EOF

cat > "$tmp/decoder_wire_config_assert.h" <<'EOF'
#include "patch_config.h"
#include "wire_config_assert.h"
#if !defined(PATCH_IMAGE_BASE) || PATCH_IMAGE_BASE != 0u
#error "repository decoder integration flags did not reach the decoder compile path"
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
static int next(void *ctx, uint8_t *out){ (void)ctx; (void)out; return 0; }
int main(void){ PatchApply state; return patch_apply_run(&state,next,0); }
EOF
# shellcheck disable=SC2086
"$CC" $DECODER_CFLAGS -I"$tmp" -include "$tmp/decoder_wire_config_assert.h" \
    -c "$tmp/source_wire_config.c" -o "$tmp/source_wire_config.o"
# shellcheck disable=SC2086
"$ARM_CC" $ARM_DEC_FLAGS -I"$tmp" -include "$tmp/decoder_wire_config_assert.h" \
    -Os -c "$tmp/source_wire_config.c" -o "$tmp/source_wire_config_arm.o"

python3 scripts/gen_single_header.py "$tmp/patch_apply_single.h" \
    src/patch_config.h src/rc_models.h src/patch_apply.h
cat > "$tmp/single_wire_config.c" <<'EOF'
#include <stdint.h>
#include "patch_apply_single.h"
#include "wire_config_assert.h"
#if !defined(PATCH_IMAGE_BASE) || PATCH_IMAGE_BASE != 0u
#error "repository decoder integration flags did not reach the generated decoder path"
#endif
uint8_t flash_read(uint32_t addr){ (void)addr; return 0xffu; }
void flash_write_page(uint32_t addr, const uint8_t page[OUTROW]){ (void)addr; (void)page; }
static int next(void *ctx, uint8_t *out){ (void)ctx; (void)out; return 0; }
int main(void){ PatchApply state; return patch_apply_run(&state,next,0); }
EOF
# shellcheck disable=SC2086
"$CC" $SINGLE_DECODER_CFLAGS -I"$tmp" "$tmp/single_wire_config.c" \
    -Wl,--gc-sections -o "$tmp/single_wire_config"

echo "wire_config_override=OK (encoder + source/generated/ARM decoders; same names/values)"
