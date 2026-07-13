/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 *
 * Compile-only integration shapes used by the aggregate decoder-footprint gate.
 */
#include "patch_apply.h"

#if defined(DECODER_INTEGRATION_STATIC) == defined(DECODER_INTEGRATION_GENERIC)
#error "select exactly one decoder integration shape"
#endif

#ifdef DECODER_INTEGRATION_GENERIC
PatchApplyResult decoder_run(PatchApply *state, PatchPull next, void *ctx){ return patch_apply_run(state, next, ctx); }
#else
static PatchApply g_patch_apply_state;
PatchApplyResult decoder_run(PatchPull next, void *ctx){ return patch_apply_run(&g_patch_apply_state, next, ctx); }
#endif
