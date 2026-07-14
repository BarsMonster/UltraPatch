/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 *
 * Host encoder module -- plan/degrade: literalize_hazards, plan_encode.
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

static void literalize_hazards(EncCtx *ctx, OpVec *ops, const Buf *to,
                               int32_t fp_start, uint32_t from_size, uint32_t to_size) {
    uint8_t *payload = ops->payload;
    OpVec out = { .payload = payload };
    int32_t tp0 = 0, fp0 = fp_start;
    for (size_t oi = 0; oi < ops->n; oi++) {
        Op *o = &ops->v[oi];
        int32_t dl = o->diff_len;
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
            opvec_push(&out, (Op){ rs - seg, k - rs, k - rs });
            memcpy(payload + tp0 + rs, to->d + (size_t)tp0 + (size_t)rs,
                   (size_t)(k - rs));
            ctx->deg_engaged = 1;
            seg = k;
            split_any = 1;
        }
        if (split_any) {
            if ((dl - seg) || o->extra_len || o->adj)
                opvec_push(&out, (Op){ dl - seg, o->extra_len, o->adj });
        } else {
            opvec_push(&out, *o);
        }
        tp0 += dl + o->extra_len;
        fp0 += dl + o->adj;
    }
    free(ops->v);
    *ops = out;
}

static OpVec prep_ops_clone(const OpVec *src, size_t payload_n) {
    OpVec out = {(Op *)xmalloc(src->n * sizeof(*src->v)), src->n, src->n,
                 (uint8_t *)xmalloc(payload_n)};
    if (src->n) memcpy(out.v, src->v, src->n * sizeof(*src->v));
    if (payload_n) memcpy(out.payload, src->payload, payload_n);
    return out;
}

void plan_prepare(PlanPrep *prep, const Buf *from, const Buf *to) {
    memset(prep, 0, sizeof(*prep));
    ldr_target_index_build(&prep->ldr, from->d, (uint32_t)from->n);
    prep->raw[PLAN_RAW_11] = bsdiff_ops(from, to, 11);
    prep->raw[PLAN_RAW_6] = bsdiff_ops(from, to, 6);
    prep->raw[PLAN_RAW_20] = bsdiff_ops(from, to, 20);
}

void plan_prepare_free(PlanPrep *prep) {
    for (int i = 0; i < PLAN_RAW_N; i++) opvec_free(&prep->raw[i]);
    ldr_target_index_free(&prep->ldr);
    memset(prep, 0, sizeof(*prep));
}

static OpVec build_candidate_ops(EncCtx *ctx, const Buf *from, const Buf *to,
                                 const PlanPrep *prep, const PlanSpec *spec) {
    OpVec ops = prep_ops_clone(&prep->raw[spec->raw_key], to->n);
    uint32_t from_size = (uint32_t)from->n, to_size = (uint32_t)to->n;
    split_nonzero_diff_runs(ctx, &ops, from, to);
    if (spec->merge_fields)
        coerce_reloc_literals(ctx, &ops, from->d, from_size, to->d, to_size, &prep->ldr);
    return ops;
}

/* Fold zero-output pure source seeks out of the plan before payload/body generation. A leading seek becomes
 * the envelope's fp_start seed; every later seek extends the previous kept op's adj. Kept op source
 * coordinates are unchanged when walks start from fp_start. */
static int32_t fold_zero_ops(OpVec *ops) {
    OpVec out = { .payload = ops->payload };
    int32_t fp_start = 0;
    for (size_t i = 0; i < ops->n; i++) {
        Op o = ops->v[i];
        if (o.diff_len == 0 && o.extra_len == 0) {
            if (out.n) out.v[out.n - 1].adj += o.adj;
            else fp_start += o.adj;
        } else {
            opvec_push(&out, o);
        }
    }
    free(ops->v);
    *ops = out;
    return fp_start;
}

/* One full op-plan -> emitted body pipeline. Plans either keep raw byte deltas or add op-derived
 * field deltas exact under the bsdiff alignment. encode_patch emits the smallest body. */
PlanResult plan_encode(EncCtx *ctx, const Buf *from, const Buf *to,
                       const PlanPrep *prep, const PlanSpec *spec) {
    PlanResult r = {0};
    ctx->deg_engaged = 0;
    PlanCaps caps;
    OpVec ops = build_candidate_ops(ctx, from, to, prep, spec);
    uint32_t from_size = (uint32_t)from->n, to_size = (uint32_t)to->n;
    int32_t fp_start_s = fold_zero_ops(&ops);
    literalize_hazards(ctx, &ops, to, fp_start_s, from_size, to_size);
    if (ctx->deg_engaged) fp_start_s += fold_zero_ops(&ops);
    fold_payload(ctx, &ops, fp_start_s, from->d, to->d,
                 from_size, to_size, &prep->ldr, &caps);
    /* The direction fallback needs to know whether hazard literalization was used. */
    r.st = (EncStats){ ctx->deg_engaged };
    int feasible = caps.ok;
    Buf body = {0};
    if (feasible) {
        int emit_overflow = 0;
        body = encode_body(ctx, &ops, from->d, from_size, to->d, to_size,
                           &prep->ldr, fp_start_s, &emit_overflow);
        if (emit_overflow) { buf_free(&body); body = (Buf){0}; feasible = 0; }
    }
    /* An infeasible plan returns an empty body and the sweep tries the remaining variants.
     * encode_patch dies only when every variant is infeasible. */
    opvec_free(&ops);
    r.body = body;
    r.fp_end = caps.fp_end;
    r.fp_start = fp_start_s;
    return r;
}
