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

static FieldDeltaVec prep_fd_clone(const FieldDeltaVec *src) {
    FieldDeltaVec out = {(FieldDelta *)xmalloc(src->n * sizeof(*src->v)), src->n, src->n};
    if (src->n) memcpy(out.v, src->v, src->n * sizeof(*src->v));
    return out;
}

static OpVec prep_ops_clone(const OpVec *src, size_t payload_n) {
    OpVec out = {(Op *)xmalloc(src->n * sizeof(*src->v)), src->n, src->n,
                 (uint8_t *)xmalloc(payload_n)};
    if (src->n) memcpy(out.v, src->v, src->n * sizeof(*src->v));
    if (payload_n) memcpy(out.payload, src->payload, payload_n);
    return out;
}

void plan_prepare(PlanPrep *prep, const Buf *from, const Buf *to,
                  const Ranges *fr, const Ranges *tr) {
    memset(prep, 0, sizeof(*prep));
    ldr_target_index_build(&prep->ldr, from->d, (uint32_t)from->n);
    /* One relocation-normalized pair feeds the three bsdiff alignments named by PLAN_SPECS. */
    data_format_encode(from, to, fr, tr, &prep->from_df, &prep->to_df, &prep->fd);
    prep->raw[PLAN_RAW_11] = bsdiff_ops(&prep->from_df, &prep->to_df, 11);
    prep->raw[PLAN_RAW_6] = bsdiff_ops(&prep->from_df, &prep->to_df, 6);
    prep->raw[PLAN_RAW_20] = bsdiff_ops(&prep->from_df, &prep->to_df, 20);
}

void plan_prepare_free(PlanPrep *prep) {
    buf_free(&prep->from_df);
    buf_free(&prep->to_df);
    free(prep->fd.v);
    for (int i = 0; i < PLAN_RAW_N; i++) opvec_free(&prep->raw[i]);
    ldr_target_index_free(&prep->ldr);
    memset(prep, 0, sizeof(*prep));
}

static OpVec build_candidate_ops(EncCtx *ctx, const Buf *from, const Buf *to,
                                 const PlanPrep *prep, const PlanSpec *spec, FieldDeltaVec *fd) {
    *fd = prep_fd_clone(&prep->fd);
    OpVec ops = prep_ops_clone(&prep->raw[spec->raw_key], to->n);
    uint32_t from_size = (uint32_t)from->n, to_size = (uint32_t)to->n;
    split_nonzero_diff_runs(ctx, &ops, &prep->from_df, &prep->to_df);
    if (spec->merge_fields)
        merge_op_field_deltas(fd, &ops, from->d, from_size, to->d, to_size,
                              &prep->ldr, ctx->fwd);
    coerce_reloc_literals(ctx, &ops, from->d, from_size, fd, &prep->ldr);
    return ops;
}

/* Fold zero-output pure source seeks out of the plan before PC/body generation. A leading seek becomes
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

static int split_overfull_corrections(EncCtx *ctx, OpVec *ops, const OpPC *pc) {
    int split_any = 0;
    int FWDD = ctx->fwd;
    int32_t *cut = (int32_t *)xmalloc((ops->n ? ops->n : 1) * sizeof(int32_t));
    for (size_t i = 0; i < ops->n; i++) cut[i] = -1;
    for (size_t step = 0; step < ops->n; step++) {
        size_t nc = pc[step].corr.n;
        int high = nc && (uint32_t)pc[step].corr.v[nc - 1u].off >= RC_PACKED_POS_LIMIT;
        if (nc <= OPC_CAP && !high) continue;
        size_t oi = FWDD ? step : ops->n - 1 - step;
        int32_t dl = ops->v[oi].diff_len;
        int32_t nw = dl + ops->v[oi].extra_len;
        int32_t m;
        if (high) {
            m = (int32_t)RC_PACKED_POS_LIMIT;
        } else {
            if (dl < 2) continue;                   /* cannot split further: stays infeasible */
            m = pc[step].corr.v[nc / 2].off;
            if (m <= 0 || m >= dl) m = dl / 2;
        }
        if (m <= 0 || m >= nw) continue;
        cut[oi] = m;
        ctx->opc_splits++;
        split_any = 1;
    }
    if (split_any) {
        OpVec out2 = { .payload = ops->payload };
        for (size_t oi = 0; oi < ops->n; oi++) {
            Op *o = &ops->v[oi];
            if (cut[oi] < 0) { opvec_push(&out2, *o); continue; }
            /* Split at an arbitrary output offset. If it falls in extras, sub1 carries
             * their prefix and sub2 is extra-only; source still advances exactly dl+adj. */
            int32_t d1 = cut[oi] < o->diff_len ? cut[oi] : o->diff_len;
            int32_t e1 = cut[oi] - d1;
            opvec_push(&out2, (Op){ d1, e1, 0 });
            opvec_push(&out2, (Op){ o->diff_len - d1, o->extra_len - e1, o->adj });
        }
        free(ops->v);
        *ops = out2;
    }
    free(cut);
    return split_any;
}

/* Corrections-cap degradation: OPC_CAP bounds
 * corrections PER OP, so an op that needs more is SPLIT at its median-correction offset
 * (a few geometry bytes of degradation) and everything is recomputed — splitting moves
 * op boundaries, which shifts field detection and thus the corrections themselves, so
 * this iterates to a fixpoint. On the home corpus no op ever exceeds the cap and pass 0
 * computes exactly the untransformed plan (bit-identical wire). */
static OpPC *build_pc_fixpoint(EncCtx *ctx, OpVec *ops, int32_t fp_start,
                               const Buf *from, const Buf *to,
                               const FieldDeltaVec *fd, const LdrTargetIndex *ldr,
                               PlanCaps *caps) {
    uint32_t from_size = (uint32_t)from->n, to_size = (uint32_t)to->n;
    OpPC *pc = NULL;
    for (;;) {
        pc = corrections_pc(ctx, ops, fp_start, from->d, to->d,
                            fd, from_size, to_size, ldr, caps);
        size_t old_n = ops->n;                               /* pc[] is sized for THIS op count */
        int split_any = split_overfull_corrections(ctx, ops, pc);
        if (!split_any) break;
        /* Each cut partitions one non-empty output span into two. Therefore ops->n grows
         * strictly and can do so at most to_size times, independent of correction churn. */
        if (ops->n <= old_n || ops->n > to_size)
            die("correction split progress invariant");
        oppc_array_free(pc, old_n);
    }
    return pc;
}

/* One full op-plan -> emitted body pipeline. Plans either keep legacy block-matched deltas or add
 * op-derived field deltas exact under the bsdiff alignment. encode_patch emits the smallest body. */
PlanResult plan_encode(EncCtx *ctx, const Buf *from, const Buf *to,
                       const PlanPrep *prep, const PlanSpec *spec) {
    PlanResult r = {0};
    ctx->deg_engaged = 0;
    ctx->opc_splits = 0;
    FieldDeltaVec fd = {0};
    PlanCaps caps;
    OpVec ops = build_candidate_ops(ctx, from, to, prep, spec, &fd);
    uint32_t from_size = (uint32_t)from->n, to_size = (uint32_t)to->n;
    int32_t fp_start_s = fold_zero_ops(&ops);
    literalize_hazards(ctx, &ops, to, fp_start_s, from_size, to_size);
    if (ctx->deg_engaged) fp_start_s += fold_zero_ops(&ops);
    OpPC *pc = build_pc_fixpoint(ctx, &ops, fp_start_s, from, to, &fd, &prep->ldr, &caps);
    /* The direction fallback needs to know whether hazard/correction fallbacks were used. */
    r.st = (EncStats){ ctx->deg_engaged, ctx->opc_splits };
    /* Decoder resource feasibility mirrors patch_apply OPC_CAP. Relocation-cache misses
     * remain representable escapes and therefore do not make a plan infeasible. */
    int feasible = caps.ok;
    Buf body = {0};
    if (feasible) {
        int emit_overflow = 0;
        body = encode_body(ctx, &ops, from->d, from_size, to->d, to_size,
                           &fd, &prep->ldr, pc, fp_start_s, &emit_overflow);
        if (emit_overflow) { buf_free(&body); body = (Buf){0}; feasible = 0; }
    }
    /* An infeasible plan (any variant, INCLUDING the legacy config 0) returns an empty body
     * and the sweep tries the remaining configs — different bsdiff alignments need different
     * correction budgets, so another variant may fit the decoder cap.
     * encode_patch dies only
     * when EVERY config is infeasible. */
    free(fd.v);
    oppc_array_free(pc, ops.n);
    opvec_free(&ops);
    r.body = body;
    r.fp_end = caps.fp_end;
    r.fp_start = fp_start_s;
    return r;
}
