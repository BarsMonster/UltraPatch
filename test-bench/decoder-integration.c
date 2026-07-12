/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 *
 * Compile-only integration shapes shared by the ARM size, stack, and wire-config checks.
 */
#ifdef DECODER_SINGLE_HEADER
#include DECODER_SINGLE_HEADER
#else
#include "patch_apply.h"
#endif

#if defined(DECODER_INTEGRATION_STATIC) == defined(DECODER_INTEGRATION_GENERIC)
#error "select exactly one decoder integration shape"
#endif

#ifdef DECODER_INTEGRATION_BSS_PROBE_BYTES
unsigned char decoder_integration_bss_probe[DECODER_INTEGRATION_BSS_PROBE_BYTES];
#endif

#ifdef DECODER_INTEGRATION_GENERIC
PatchApplyResult rcv3_run(PatchApply *state, PatchPull next, void *ctx){ return patch_apply_run(state, next, ctx); }
#else
static PatchApply g_patch_apply_state;
PatchApplyResult rcv3_run(PatchPull next, void *ctx){ return patch_apply_run(&g_patch_apply_state, next, ctx); }
#endif
