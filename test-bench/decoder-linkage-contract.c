/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 *
 * Multi-TU linkage contract.  The test script compiles this source in four roles
 * against both distributed header forms: the legacy static decoder, one external
 * implementation, and two declarations-only callers.
 */
#include <stddef.h>
#include <stdint.h>

#if defined(DECODER_LINKAGE_IMPLEMENTATION) || defined(DECODER_LINKAGE_IMPLEMENTATION_TWICE)
#define ULTRAPATCH_IMPLEMENTATION
#elif defined(DECODER_LINKAGE_CALLER_ONE) || defined(DECODER_LINKAGE_CALLER_TWO) || \
      defined(DECODER_LINKAGE_DECLARATIONS_TWICE)
#define ULTRAPATCH_DECLARATIONS_ONLY
#elif defined(DECODER_LINKAGE_CONFLICT)
#define ULTRAPATCH_DECLARATIONS_ONLY
#define ULTRAPATCH_IMPLEMENTATION
#elif !defined(DECODER_LINKAGE_STATIC)
#error "select one decoder linkage contract role"
#endif

#ifdef DECODER_SINGLE_HEADER
#include DECODER_SINGLE_HEADER
#else
#include "patch_apply.h"
#endif

#if defined(DECODER_LINKAGE_IMPLEMENTATION_TWICE) || \
    defined(DECODER_LINKAGE_DECLARATIONS_TWICE)
#ifdef DECODER_SINGLE_HEADER
#include DECODER_SINGLE_HEADER
#else
#include "patch_apply.h"
#endif
#endif

#if defined(DECODER_LINKAGE_CALLER_ONE)

PatchApplyResult decoder_linkage_call_one(PatchApply *state, PatchPull next, void *ctx){
    return patch_apply_run(state, next, ctx);
}

#elif defined(DECODER_LINKAGE_DECLARATIONS_TWICE)

PatchApplyResult decoder_linkage_repeated_declaration(PatchApply *state,
                                                       PatchPull next, void *ctx){
    return patch_apply_run(state, next, ctx);
}

#elif defined(DECODER_LINKAGE_CALLER_TWO)

PatchApplyResult decoder_linkage_call_one(PatchApply *state, PatchPull next, void *ctx);

static PatchApply decoder_linkage_state;

uint8_t flash_read(uint32_t addr){
    (void)addr;
    return 0xffu;
}

void flash_write_page(uint32_t addr, const uint8_t page[OUTROW]){
    (void)addr;
    (void)page;
}

static int no_bytes(void *ctx, uint8_t *out){
    (void)ctx;
    (void)out;
    return PATCH_PULL_END;
}

static PatchApplyResult decoder_linkage_call_two(PatchApply *state,
                                                  PatchPull next, void *ctx){
    return patch_apply_run(state, next, ctx);
}

int main(void){
    PatchApplyResult first = decoder_linkage_call_one(&decoder_linkage_state, no_bytes, NULL);
    PatchApplyResult second = decoder_linkage_call_two(&decoder_linkage_state, no_bytes, NULL);
    return first == PATCH_APPLY_ERROR && second == PATCH_APPLY_ERROR &&
           patch_apply_reject(&decoder_linkage_state) == REJ_CORRUPT &&
           patch_apply_flash_touched(&decoder_linkage_state) == 0 ? 0 : 1;
}

#elif defined(DECODER_LINKAGE_STATIC)

uint8_t flash_read(uint32_t addr){
    (void)addr;
    return 0xffu;
}

void flash_write_page(uint32_t addr, const uint8_t page[OUTROW]){
    (void)addr;
    (void)page;
}

static int no_bytes(void *ctx, uint8_t *out){
    (void)ctx;
    (void)out;
    return PATCH_PULL_END;
}

int main(void){
    PatchApply state;
    PatchApplyResult result = patch_apply_run(&state, no_bytes, NULL);
    return result == PATCH_APPLY_ERROR && patch_apply_reject(&state) == REJ_CORRUPT &&
           patch_apply_flash_touched(&state) == 0 ? 0 : 1;
}

#endif
