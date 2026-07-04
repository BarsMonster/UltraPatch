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
#include <errno.h>
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

/* PATHE_W: compile-time window-log default mirror. The LIVE encoder window is the CLI W argument
 * (which must equal the decoder SA_W); PATHE_W just single-sources the default value. */
#ifndef PATHE_W
#define PATHE_W RC_WINDOW_LOG_DEFAULT
#endif
#ifndef DR_KCAP_BL
#define DR_KCAP_BL RC_DR_KCAP_BL_DEFAULT   /* mirrors decoder DR_KCAP_BL (default value in rc_models.h) */
#endif
#ifndef DR_KCAP_EX
#define DR_KCAP_EX RC_DR_KCAP_EX_DEFAULT   /* mirrors decoder DR_KCAP_EX (default value in rc_models.h) */
#endif
/* DR_HIT_INIT is single-sourced in rc_models.h (shared by decoder dr_init and encoder dr_init_e). */

/* Apply direction for the CURRENT encode attempt (1 = ascending/FWD, 0 = descending).
 * Direction is an ENCODER CHOICE signaled in the envelope (natural = canonical size-delta
 * uLEB, unnatural = overlong marker) instead of being derived from the size relation;
 * encode_a1 runs the plan sweep in both directions and ships the smaller total. Every
 * pipeline stage reads this instead of comparing sizes. */
static int g_enc_fwd = 1;

/* Degradation instrumentation (host-side, WIRE-NEUTRAL: these never touch emitted bytes).
 * plan_encode resets them per plan attempt; the two degradation passes bump them; encode_a1
 * snapshots the WINNING plan's values into EncStats and prints one deterministic line under
 * A1_DEGRADE_STATS. No effect on the blob — verified by the golden gate. */
static int    g_deg_engaged;      /* journal-budget degradation converted >=1 over-budget read */
static size_t g_deg_pres_needed;  /* preserves the ideal (pre-degradation) plan required */
static size_t g_deg_converted;    /* source bytes converted from journal reads to plain extras */
static size_t g_opc_splits;       /* ops split because per-op corrections exceeded A1_OPC_CAP */

/* Row-window oracle mirrors — MUST match decoder OUTROW / OUTROW_DEPTH (encoding-affecting build
 * contract; compatibility is monotone toward larger decoder windows). Shared DEFAULT in rc_models.h;
 * kept SEPARATELY overridable from the decoder knobs (a decoder window may legitimately be a superset). */
#ifndef A1_OUTROW
#define A1_OUTROW RC_OUTROW_DEFAULT
#endif
#ifndef A1_ROW_DEPTH
#define A1_ROW_DEPTH RC_ROW_DEPTH_DEFAULT
#endif

/* Row-window oracle: is source address a — already logically overwritten — still OLD in
 * physical flash when the decoder produces output position t? The decoder keeps the last
 * OUTROW_DEPTH rows uncommitted (direct-mapped FIFO); rows are touched in write order, so
 * at read time the uncommitted set always includes rows within (depth-1) of row(t)
 * (conservative by one extra row at row starts). Covered reads return the pristine old
 * byte via plain flash_read — no journal slot, no [P] event, no conversion needed. */
static int a1_row_covered(int64_t a, int64_t t) {
    if (g_enc_fwd) return a / A1_OUTROW >= t / A1_OUTROW - (A1_ROW_DEPTH - 1);
    return a / A1_OUTROW <= t / A1_OUTROW + (A1_ROW_DEPTH - 1);
}

/* Decoder resource-cap mirrors — MUST match patch_apply JSLOTS / OPC_CAP (a deployment that
 * -D-retunes the decoder caps must retune these identically). Shared DEFAULT in rc_models.h.
 * A1_JSLOTS is also the journal budget for plan degradation (degrade_ops_to_journal_budget). */
#ifndef A1_JSLOTS
#define A1_JSLOTS RC_JSLOTS_DEFAULT
#endif
#ifndef A1_OPC_CAP
#define A1_OPC_CAP RC_OPC_CAP_DEFAULT
#endif

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
/* The modules are NOT standalone TUs: they share the prologue above and reference each      */
/* other, so the include ORDER is the historical top-to-bottom order and must be preserved.  */
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
