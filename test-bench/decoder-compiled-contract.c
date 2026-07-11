/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 *
 * Object-level decoder contract probe.  Keep the decoder state automatic: the
 * contract gate inspects this translation unit's real -O0 object to ensure the
 * header contributes no writable storage, allocation dependency, or exported
 * implementation symbol.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef DECODER_SINGLE_HEADER
#include "patch_apply_single.h"
#else
#include "patch_apply.h"
#endif

PatchApplyResult decoder_compiled_contract_apply(PatchPull next, void *ctx){
    PatchApply state;
    return patch_apply_run(&state, next, ctx);
}
