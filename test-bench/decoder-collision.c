/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 *
 * Consumer namespace regression: ordinary application identifiers that the decoder
 * formerly occupied must remain usable with both public header packaging forms.
 */
#include <stdint.h>

#define RING 7u
#define MASK 3u
#define ARENA_BYTES 11u
#define ULEB32_OVERFLOW(sh, b) ((sh) == (b))
#define BT_PROBS 13u
#define BT_BYTES 17u
#define UG_CTX 19
#define UG_C(x) ((x) + 23)
#define UG_GAMMA_MANT 29
#define SMAP_CAP 31
#define LIT0_CTX 37
#define IDX_CTX 41
#define DR_HIT_INIT 43u

int image_flash_read(void){ return 1; }
int decode_body(void){ return 2; }
int next_byte(void){ return 3; }
int crc32_flash(void){ return 4; }
int out_write(void){ return 5; }
int apply_op(void){ return 6; }
int bt_get(void){ return 7; }
int fl_init(void){ return 8; }
int idx_init(void){ return 9; }
int op_next_offset(void){ return 10; }

#ifdef DECODER_SINGLE_HEADER
#include "patch_apply_single.h"
#else
#include "patch_apply.h"
#endif

#if !defined(BT_PROBS) || !defined(BT_BYTES) || !defined(UG_CTX) || \
    !defined(UG_C) || !defined(UG_GAMMA_MANT) || !defined(SMAP_CAP) || \
    !defined(LIT0_CTX) || !defined(IDX_CTX) || !defined(DR_HIT_INIT)
#error "decoder clobbered consumer model macros"
#endif

_Static_assert(RING == 7u && MASK == 3u && ARENA_BYTES == 11u,
               "decoder changed consumer macros");
_Static_assert(BT_PROBS == 13u && BT_BYTES == 17u && UG_CTX == 19 &&
               UG_C(2) == 25 && UG_GAMMA_MANT == 29 && SMAP_CAP == 31 &&
               LIT0_CTX == 37 && IDX_CTX == 41 && DR_HIT_INIT == 43u,
               "decoder changed consumer model macros");

#if defined(UP_JREGION) || defined(UP_RING) || defined(UP_MASK) || \
    defined(UP_ARENA_BYTES) || defined(UP_RC_UNARY_MAX) || \
    defined(UP_ULEB32_OVERFLOW) || defined(UP_OROW_NONE) || \
    defined(UP_OROW_SLOT) || defined(UP_PSRC_TGT_MASK) || \
    defined(UP_BT_PROBS) || defined(UP_BT_BYTES) || defined(UP_UG_CTX) || \
    defined(UP_UG_C) || defined(UP_UG_GAMMA_MANT) || defined(UP_SMAP_CAP) || \
    defined(UP_LIT0_CTX) || defined(UP_IDX_CTX) || defined(UP_DR_HIT_INIT)
#error "decoder-private UP_* macro leaked after inclusion"
#endif

#if defined(RC_ALWAYS_INLINE) || defined(RC_NOINLINE) || \
    defined(RC_WARN_UNUSED_RESULT) || defined(RC_ADD_OVERFLOW) || \
    defined(RC_SUB_OVERFLOW)
#error "decoder-private model macro leaked after inclusion"
#endif

uint8_t flash_read(uint32_t addr){ (void)addr; return 0xffu; }
void flash_write_page(uint32_t addr, const uint8_t page[OUTROW]){
    (void)addr;
    (void)page;
}

static int app_next(void *ctx, uint8_t *out){
    (void)ctx;
    (void)out;
    return PATCH_PULL_END;
}

int decoder_collision_probe(void){
    PatchApply state;
    int ordinary = image_flash_read() + decode_body() + next_byte() + crc32_flash() +
                   out_write() + apply_op() + bt_get() + fl_init() + idx_init() +
                   op_next_offset();
    return ordinary + patch_apply_run(&state, app_next, 0) + ULEB32_OVERFLOW(1, 2);
}
