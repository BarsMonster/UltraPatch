/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 *
 * Host encoder module -- plan finalization/degrade and plan_encode.
 * Compiled as a normal internal encoder translation unit.
 */

#include "enc_internal.h"

/* Ordered plan selectors and tie priority live here. Keeping the legacy entry first is
 * wire-significant: equal-size candidates retain the earliest registry entry. */
const PlanSpec PLAN_SPECS[PLAN_SPEC_N] = {
    {0, PLAN_RAW_11},
    {1, PLAN_RAW_11},
    {1, PLAN_RAW_6},
    {1, PLAN_RAW_20},
};
/* Convert every copy that would read already-committed flash into true target literals. An
 * equal source adjustment skips the removed copy bytes, leaving every surviving source read
 * either ahead of the frontier or covered by the uncommitted output-page window. */
static int copy_hazard_at(const EncCtx *ctx, int32_t fp0, int32_t tp0, int32_t k,
                          uint32_t from_size, uint32_t to_size) {
    int64_t a = (int64_t)fp0 + k, t = (int64_t)tp0 + k;
    int behind = ctx->fwd ? a < t : a > t;
    return a >= 0 && a < (int64_t)from_size && a < (int64_t)to_size &&
           behind && !row_covered(ctx, a, t);
}

/* Pure source seeks carry no output. A leading seek seeds the envelope; every later one extends
 * the previous emitted op, preserving the source coordinate of all following output. */
static void final_op_push(OpVec *out, Op o, int32_t *fp_start) {
    if (o.diff_len == 0 && o.extra_len == 0) {
        if (out->n) out->v[out->n - 1u].adj += o.adj;
        else *fp_start += o.adj;
    } else {
        opvec_push(out, o);
    }
}

/* Finalize post-split geometry in one rebuild: hazard literalization and source-seek folding. */
static int32_t finalize_ops(EncCtx *ctx, OpVec *ops, uint32_t from_size,
                            uint32_t to_size, int32_t *fp_end) {
    OpVec out = {0};
    int32_t fp_start = 0, tp0 = 0, fp0 = 0;
    for (size_t oi = 0; oi < ops->n; oi++) {
        Op o = ops->v[oi];
        int32_t dl = o.diff_len;
        int32_t seg = 0, k = 0;
        int split_any = 0;
        while (k < dl) {
            int hz = copy_hazard_at(ctx, fp0, tp0, k, from_size, to_size);
            if (!hz) { k++; continue; }
            int32_t rs = k;
            while (k < dl) {
                hz = copy_hazard_at(ctx, fp0, tp0, k, from_size, to_size);
                if (!hz) break;
                k++;
            }
            final_op_push(&out, (Op){ rs - seg, k - rs, k - rs }, &fp_start);
            ctx->deg_engaged = 1;
            seg = k;
            split_any = 1;
        }
        if (split_any) {
            final_op_push(&out, (Op){ dl - seg, o.extra_len, o.adj }, &fp_start);
        } else {
            final_op_push(&out, o, &fp_start);
        }
        tp0 += dl + o.extra_len;
        fp0 += dl + o.adj;
    }
    free(ops->v);
    *ops = out;
    *fp_end = fp0;
    return fp_start;
}

static OpVec prep_ops_clone(const OpVec *src) {
    OpVec out = {(Op *)xmalloc(src->n * sizeof(*src->v)), src->n, src->n};
    if (src->n) memcpy(out.v, src->v, src->n * sizeof(*src->v));
    return out;
}

void plan_prepare(PlanPrep *prep, const Buf *from, const Buf *to) {
    memset(prep, 0, sizeof(*prep));
    lit_seed_trees_init(&prep->lit.seeds, from->d, from->n);
    from_lit_proxy_bits(&prep->lit.seeds, prep->lit.L0, prep->lit.L1);
    ldr_target_index_build(&prep->ldr, from->d, (uint32_t)from->n);
    bsdiff_ops_all(from, to, prep->raw);
}

void plan_prepare_free(PlanPrep *prep) {
    for (int i = 0; i < PLAN_RAW_N; i++) opvec_free(&prep->raw[i]);
    ldr_target_index_free(&prep->ldr);
    memset(prep, 0, sizeof(*prep));
}

/* Prepare direction-dependent geometry once for adjacent registry entries that share a raw plan.
 * Field merging affects only body construction, so both raw-11 modes can reuse this exact result. */
void plan_geometry_prepare(PlanGeometry *geom, EncCtx *ctx,
                           const Buf *from, const Buf *to,
                           const PlanPrep *prep, int raw_key) {
    memset(geom, 0, sizeof(*geom));
    ctx->deg_engaged = 0;
    geom->ops = prep_ops_clone(&prep->raw[raw_key]);
    split_nonzero_diff_runs(ctx, &geom->ops, from, to, &prep->lit);
    uint32_t from_size = (uint32_t)from->n, to_size = (uint32_t)to->n;
    geom->fp_start = finalize_ops(ctx, &geom->ops, from_size, to_size, &geom->fp_end);
}

void plan_geometry_free(PlanGeometry *geom) {
    opvec_free(&geom->ops);
    memset(geom, 0, sizeof(*geom));
}

/* Encode either field mode from finalized geometry. encode_patch emits the smallest body. */
Buf plan_encode(EncCtx *ctx, const Buf *from, const Buf *to,
                const PlanPrep *prep, const PlanGeometry *geom,
                int merge_fields, OutIndex *out_index) {
    int emit_overflow = 0;
    Buf body = encode_body(ctx, &geom->ops, from->d, (uint32_t)from->n,
                           to->d, (uint32_t)to->n, &prep->ldr, &prep->lit,
                           merge_fields, geom->fp_start, out_index, &emit_overflow);
    if (emit_overflow) { buf_free(&body); body = (Buf){0}; }
    /* An infeasible plan returns an empty body and the sweep tries the remaining variants.
     * encode_patch dies only when every variant is infeasible. */
    return body;
}
