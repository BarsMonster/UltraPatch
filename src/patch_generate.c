/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * A1 host encoder (C) for the patch_apply decoder wire.
 *
 * This is intentionally a host-side encoder: compression-side memory/CPU are
 * allowed to be large. It emits the final A1 blob consumed by patch_apply.h.
 */
#include <unistd.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arm_cortex_m4.h"
#include "rc_models.h"

/* patch_selfcheck.c: reference-decoder self-verification of an emitted blob.
 * Returns NULL on success, else a short static error message. */
extern const char *a1_selfcheck(const uint8_t *blob, size_t blob_n,
                                const uint8_t *from, size_t from_n,
                                const uint8_t *to, size_t to_n);

int divsufsort(const uint8_t *T, int32_t *SA, int32_t n);

/* PATHE_W / A1_* encoder mirrors and DR_KCAP_* are configured in patch_config.h. */
/* DR_HIT_INIT is single-sourced in rc_models.h (shared by decoder dr_init and encoder dr_init_e). */

typedef struct {
    /* Apply direction for the CURRENT encode attempt (1 = ascending/FWD, 0 = descending).
     * Direction is an ENCODER CHOICE signaled in the envelope (natural = canonical size-delta
     * uLEB, unnatural = overlong marker) instead of being derived from the size relation. */
    int fwd;

    /* WIRE-NEUTRAL scaffold gate: when set, encode_body appends the surviving tag0/tag1
     * span-literal stream to $A1_LITDUMP.<pid>.bin for tools/lit0map_faithful.c. */
    int litdump;

    /* Degradation instrumentation (host-side, WIRE-NEUTRAL: these never touch emitted bytes).
     * plan_encode resets them per plan attempt; encode_a1 snapshots the winning plan. */
    int    deg_engaged;      /* journal-budget degradation converted >=1 over-budget read */
    size_t deg_pres_needed;  /* preserves the ideal (pre-degradation) plan required */
    size_t deg_converted;    /* source bytes converted from journal reads to plain extras */
    size_t opc_splits;       /* ops split because per-op corrections exceeded A1_OPC_CAP */
} EncCtx;

/* Row-window oracle mirrors — MUST match decoder OUTROW / OUTROW_DEPTH (encoding-affecting build
 * contract; compatibility is monotone toward larger decoder windows). Shared DEFAULT in patch_config.h;
 * kept SEPARATELY overridable from the decoder knobs (a decoder window may legitimately be a superset). */

/* Row-window oracle: is source address a — already logically overwritten — still OLD in
 * physical flash when the decoder produces output position t? The decoder keeps the last
 * OUTROW_DEPTH rows uncommitted (direct-mapped FIFO); rows are touched in write order, so
 * at read time the uncommitted set always includes rows within (depth-1) of row(t)
 * (conservative by one extra row at row starts). Covered reads return the pristine old
 * byte via plain flash_read — no journal slot, no [P] event, no conversion needed. */
static int a1_row_covered(const EncCtx *ctx, int64_t a, int64_t t) {
    if (ctx->fwd) return a / A1_OUTROW >= t / A1_OUTROW - (A1_ROW_DEPTH - 1);
    return a / A1_OUTROW <= t / A1_OUTROW + (A1_ROW_DEPTH - 1);
}

/* Decoder resource-cap mirrors — MUST match patch_apply JSLOTS / OPC_CAP (a deployment that
 * -D-retunes the decoder caps must retune these identically). Shared DEFAULT in patch_config.h.
 * A1_JSLOTS is also the journal budget for plan degradation (degrade_ops_to_journal_budget). */

enum { STREAM_DATA, STREAM_CODE, STREAM_BL, STREAM_LDR, STREAM_N };

typedef struct { uint8_t *d; size_t n, cap; } Buf;
typedef struct { int32_t diff_len, adj; uint8_t *diff; uint8_t *extra; int32_t extra_len; } Op;
typedef struct { Op *v; size_t n, cap; } OpVec;
typedef struct { int32_t from_offset, to_address, n; int64_t *values; } Block;
typedef struct { Block *v; size_t n, cap; } BlockVec;
typedef struct { uint32_t addr; int kind; int64_t delta; } FieldDelta;
typedef struct { FieldDelta *v; size_t n, cap; } FieldDeltaVec;
typedef struct { int32_t *v; size_t n, cap; } IVec;
typedef struct { int32_t off; uint8_t byte; } CorrEnt;
typedef struct { CorrEnt *v; size_t n, cap; } CorrVec;
typedef struct { IVec pres; CorrVec corr; } OpPC;
typedef struct { uint32_t data_off_begin, data_off_end, data_begin, data_end, code_begin, code_end; } Ranges;
typedef struct {
    /* degradation snapshot (filled unconditionally by plan_encode) */
    int    deg_engaged;
    size_t deg_pres_needed, deg_converted, opc_splits;
} EncStats;

/* ------------------------------------------------------------------------------------- */
/* Umbrella split (U15): the host encoder is one translation unit assembled from ordered  */
/* enc_*.inc modules included below. This keeps the single-TU whole-program optimization   */
/* and byte-exact wire, keeps test/model_diff.c's `#include "patch_generate.c"` and         */
/* scripts/check_analyze.sh working unchanged, and gives each subsystem its own file.       */
/* The modules are NOT standalone TUs: they share the prologue above, pass EncCtx for        */
/* attempt-scoped encoder state, and reference each other in the preserved include order.    */
/* ------------------------------------------------------------------------------------- */
#include "enc_util.inc"     /* util/IO, allocators, a1_sort, Buf, crc32, varints           */
#include "enc_elf.inc"      /* ELF32 range extraction                                      */
#include "enc_bsdiff.inc"   /* SequenceMatcher data blocks + suffix-sort bsdiff ops        */
#include "enc_field.inc"    /* field/delta model + apply planning + preserve/corrections   */
#include "enc_rc.inc"       /* binary range encoder + entropy models                       */
#include "enc_lz.inc"       /* LZSS DP parse + entropy pricing                             */
#include "enc_emit.inc"     /* body assembly / range-coder emit                            */
#include "enc_plan.inc"     /* plan/degrade                                                */
#include "enc_cli.inc"      /* CLI: encode_a1, main                                        */
