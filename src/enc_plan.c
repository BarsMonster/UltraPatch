/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 *
 * A1 host encoder module -- plan/degrade: degrade_ops_to_journal_budget, plan_encode.
 * Compiled as a normal internal encoder translation unit.
 */

#include "enc_internal.h"
/* ---- journal-budget degradation (encoder-only; wire format and decoder untouched) ----
 * The decoder's never-evict journal holds at most JSLOTS overwritten source bytes. When
 * the ideal op plan needs more, convert the OVER-BUDGET read-after-overwrite copy regions
 * into plain extra bytes (the exact to-image bytes, shipped in the content stream):
 * compression degrades — those bytes code as literals/LZ instead of free copies — but the
 * plan becomes journal-feasible instead of refusing. Preserves are journaled in apply order
 * (strictly ascending output positions for FWD, descending for grow — the NVM gate pins
 * frontier monotonicity), so the first JSLOTS preserves in that order stay protected and
 * every read of a later (unprotected) overwritten position is converted. A read-behind-
 * frontier within one op sits at the constant offset fp0-tp0, so the conversion range is
 * contiguous per op and each affected op splits into at most two wire ops. Every remaining
 * overwritten read still goes through the journal — the invariant that keeps the wire
 * independent of the deployment's NVM row size. Correctness of the transformed plan is
 * still proven per blob by the reference-decoder self-verification. */
static int degrade_hazard_at(const EncCtx *ctx, int32_t fp0, int32_t tp0, int32_t k,
                             uint32_t from_size, uint32_t to_size, int32_t cutoff) {
    int64_t a = (int64_t)fp0 + k, t = (int64_t)tp0 + k;
    return a >= 0 &&
           (ctx->fwd ? (a < (int64_t)to_size && a >= cutoff)
                     : (a < (int64_t)from_size && a <= cutoff)) &&
           !a1_row_covered(ctx, a, t);
}

/* Output position of the (budget+1)-th preserve in apply order, i.e. the cutoff C that
 * degrade_hazard_at compares against. preserve_corrections_pc already accumulated the identical
 * preserve set: caps.pres_total is the count and pc[step] holds each op's preserves. Within an op,
 * FWD pushes ascending offsets (== apply order) while grow re-sorts pres ascending, so grow apply
 * order is pc->pres reversed. Each op's output base tp tiles from 0 (opwalk_build), so the position
 * of a preserve is tp + off. Only called when total > budget, so the (budget+1)-th preserve exists. */
static int32_t degrade_pres_cutoff(const EncCtx *ctx, const OpVec *ops, const OpPC *pc, size_t budget) {
    int FWD = ctx->fwd;
    size_t n = ops->n;
    int32_t *tp_of = (int32_t *)xmalloc((n ? n : 1) * sizeof(int32_t));
    int32_t tp = 0;
    for (size_t i = 0; i < n; i++) { tp_of[i] = tp; tp += ops->v[i].diff_len + ops->v[i].extra_len; }
    int32_t cutoff = 0;
    size_t seen = 0;
    for (size_t step = 0; step < n; step++) {
        const IVec *pres = &pc[step].pres;
        for (size_t j = 0; j < pres->n; j++) {
            if (seen == budget) {
                int32_t off = FWD ? pres->v[j] : pres->v[pres->n - 1u - j];
                cutoff = tp_of[opwalk_apply_index(n, FWD, step)] + off;
                free(tp_of);
                return cutoff;
            }
            seen++;
        }
    }
    free(tp_of);
    return cutoff;
}

static void degrade_ops_to_journal_budget(EncCtx *ctx, OpVec *ops, const Buf *to,
                                          const uint8_t *frm, const FieldDeltaVec *fd,
                                          uint32_t from_size, uint32_t to_size, size_t budget) {
    int FWD = ctx->fwd;
    /* Derive the preserve total and cutoff from a single preserve_corrections_pc pass over the same
     * opwalk+readarr machinery (fp_start=0: degrade runs before fold_zero_ops). */
    PlanCaps caps;
    OpPC *pc = preserve_corrections_pc(ctx, ops, 0, frm, to->d, fd, from_size, to_size, &caps);
    size_t total = caps.pres_total;
    ctx->deg_pres_needed = total;
    if (total <= budget) { oppc_array_free(pc, ops->n); return; }
    int32_t C = degrade_pres_cutoff(ctx, ops, pc, budget);
    oppc_array_free(pc, ops->n);
    /* C is the output position of the first preserve past the budget, in apply order. */
    OpVec out = {0};
    int32_t tp0 = 0, fp0 = 0;
    for (size_t oi = 0; oi < ops->n; oi++) {
        Op *o = &ops->v[oi];
        int32_t dl = o->diff_len;
        int behind = FWD ? (fp0 < tp0) : (fp0 > tp0);   /* op reads behind the write frontier */
        int32_t seg = 0, k = 0;
        int split_any = 0;
        while (behind && k < dl) {
            /* hazard = behind-frontier read of an UNPROTECTED overwritten position that the
             * row window does NOT cover. Coverage is periodic within each output row, so
             * hazard runs fragment; every maximal run becomes exact extra bytes (source
             * skipped via adj), splitting the op into copy/extra alternations. */
            int hz = degrade_hazard_at(ctx, fp0, tp0, k, from_size, to_size, C);
            if (!hz) { k++; continue; }
            int32_t rs = k;
            while (k < dl) {
                hz = degrade_hazard_at(ctx, fp0, tp0, k, from_size, to_size, C);
                if (!hz) break;
                k++;
            }
            opvec_push(&out, op_copy(rs - seg, o->diff + seg, k - rs,
                                     to->d + (size_t)tp0 + (size_t)rs, k - rs));
            ctx->deg_engaged = 1;
            ctx->deg_converted += (size_t)(k - rs);
            seg = k;
            split_any = 1;
        }
        if (split_any) {
            if ((dl - seg) || o->extra_len || o->adj)
                opvec_push(&out, op_copy(dl - seg, o->diff + seg, o->extra_len, o->extra, o->adj));
            op_free_payload(o);
        } else {
            opvec_push(&out, *o);
        }
        tp0 += dl + o->extra_len;
        fp0 += dl + o->adj;
    }
    free(ops->v);
    *ops = out;
}

static OpVec build_candidate_ops(EncCtx *ctx, const Buf *from, const Buf *to,
                                 const PairAnalysis *pa, PlanCfg cfg,
                                 Buf *from_df, Buf *to_df, FieldDeltaVec *fd) {
    int variant = cfg.variant;
    data_format_encode(from, to, pa, from_df, to_df, fd,
                       variant >= 2 ? 1 : 0);
    OpVec ops = bsdiff_ops(from_df, to_df, cfg.fuzz);
    uint32_t from_size = (uint32_t)from->n, to_size = (uint32_t)to->n;
    split_nonzero_diff_runs(ctx, &ops, from_df, to_df);
    if (variant >= 1) merge_op_field_deltas(fd, &ops, from->d, from_size, to->d, to_size);
    coerce_reloc_literals(ctx, &ops, from->d, from_size, fd);
    degrade_ops_to_journal_budget(ctx, &ops, to, from->d, fd, from_size, to_size, JSLOTS);
    return ops;
}

/* Fold zero-output pure source seeks out of the plan before PC/body generation. A leading seek becomes
 * the envelope's fp_start seed; every later seek extends the previous kept op's adj. Kept op source
 * coordinates are unchanged when walks start from fp_start. */
static int32_t fold_zero_ops(OpVec *ops) {
    OpVec out = {0};
    int32_t fp_start = 0;
    for (size_t i = 0; i < ops->n; i++) {
        Op o = ops->v[i];
        if (o.diff_len == 0 && o.extra_len == 0) {
            if (out.n) out.v[out.n - 1].adj += o.adj;
            else fp_start += o.adj;
            op_free_payload(&o);
        } else {
            opvec_push(&out, o);
        }
    }
    free(ops->v);
    *ops = out;
    return fp_start;
}

static int split_overfull_corrections(EncCtx *ctx, OpVec *ops, const OpPC *pc, int pass) {
    int split_any = 0;
    if (pass >= 12) return 0;
    int FWDD = ctx->fwd;
    int32_t *cut = (int32_t *)xmalloc((ops->n ? ops->n : 1) * sizeof(int32_t));
    for (size_t i = 0; i < ops->n; i++) cut[i] = -1;
    for (size_t step = 0; step < ops->n; step++) {
        if (pc[step].corr.n <= OPC_CAP) continue;
        size_t oi = FWDD ? step : ops->n - 1 - step;
        int32_t dl = ops->v[oi].diff_len;
        if (dl < 2) continue;                       /* cannot split further: stays infeasible */
        int32_t m = pc[step].corr.v[pc[step].corr.n / 2].off;
        if (m <= 0 || m >= dl) m = dl / 2;
        if (m <= 0 || m >= dl) continue;
        cut[oi] = m;
        ctx->opc_splits++;
        split_any = 1;
    }
    if (split_any) {
        OpVec out2 = {0};
        for (size_t oi = 0; oi < ops->n; oi++) {
            Op *o = &ops->v[oi];
            if (cut[oi] < 0) { opvec_push(&out2, *o); continue; }
            /* split keeps the tp/fp walk exact: sub1 advances fp by cut (adj 0),
             * sub2 carries the remaining diff + the original extras and adj. */
            opvec_push(&out2, op_copy(cut[oi], o->diff, 0, NULL, 0));
            opvec_push(&out2, op_copy(o->diff_len - cut[oi], o->diff + cut[oi],
                                      o->extra_len, o->extra, o->adj));
            op_free_payload(o);
        }
        free(ops->v);
        *ops = out2;
    }
    free(cut);
    return split_any;
}

/* corrections-cap degradation (same philosophy as the journal budget): OPC_CAP bounds
 * corrections PER OP, so an op that needs more is SPLIT at its median-correction offset
 * (a few geometry bytes of degradation) and everything is recomputed — splitting moves
 * op boundaries, which shifts field detection and thus the corrections themselves, so
 * this iterates to a fixpoint. On the home corpus no op ever exceeds the cap and pass 0
 * computes exactly the untransformed plan (bit-identical wire). */
static OpPC *build_pc_fixpoint(EncCtx *ctx, OpVec *ops, int32_t fp_start, const Buf *from, const Buf *to,
                               const FieldDeltaVec *fd, PlanCaps *caps) {
    uint32_t from_size = (uint32_t)from->n, to_size = (uint32_t)to->n;
    OpPC *pc = NULL;
    for (int pass = 0;; pass++) {
        pc = preserve_corrections_pc(ctx, ops, fp_start, from->d, to->d, fd, from_size, to_size, caps);
        size_t old_n = ops->n;                               /* pc[] is sized for THIS op count */
        int split_any = split_overfull_corrections(ctx, ops, pc, pass);
        if (!split_any) break;
        oppc_array_free(pc, old_n);
    }
    return pc;
}

/* One full op-plan -> emitted body pipeline. variant: 0 = legacy block-matched deltas;
 * 1 = + op-derived field deltas (exact under the bsdiff alignment); 2 = + BL-immediate masking
 * of the bsdiff inputs (copies extend through recompiled code). encode_a1 emits whichever
 * variant's exact body is smallest (ties keep the lowest variant), so this cannot regress. */
PlanResult plan_encode(EncCtx *ctx, const Buf *from, const Buf *to, const PairAnalysis *pa, PlanCfg cfg) {
    PlanResult r = {0};
    ctx->deg_engaged = 0; ctx->deg_pres_needed = 0; ctx->deg_converted = 0; ctx->opc_splits = 0;
    Buf from_df = {0}, to_df = {0};
    FieldDeltaVec fd = {0};
    OpVec ops = build_candidate_ops(ctx, from, to, pa, cfg, &from_df, &to_df, &fd);
    uint32_t from_size = (uint32_t)from->n, to_size = (uint32_t)to->n;
    int32_t fp_start_s = fold_zero_ops(&ops);
    PlanCaps caps;
    OpPC *pc = build_pc_fixpoint(ctx, &ops, fp_start_s, from, to, &fd, &caps);
    /* degradation snapshot: load-bearing for direction-sweep pruning and A1_DEGRADE_STATS */
    r.st = (EncStats){ ctx->deg_engaged, ctx->deg_pres_needed, ctx->deg_converted, ctx->opc_splits };
    /* decoder resource-cap feasibility (mirror patch_apply OPC_CAP / JSLOTS): an over-cap plan
     * would be rejected on-device; treat as infeasible so a lower variant ships instead. */
    int feasible = caps.ok;
    Buf body = {0};
    if (feasible) {
        int emit_overflow = 0;
        body = encode_body(ctx, &ops, from->d, from_size, to->d, to_size, &fd, pc, fp_start_s, &emit_overflow);
        if (emit_overflow) { buf_free(&body); body = (Buf){0}; feasible = 0; }
    }
    /* An infeasible plan (any variant, INCLUDING the legacy config 0) returns an empty body
     * and the sweep tries the remaining configs — different bsdiff alignments need different
     * preserve/correction budgets, so another variant may fit the decoder caps (measured on
     * foreign firmware: config 0 over-journal while fuzz variants fit). encode_a1 dies only
     * when EVERY config is infeasible. */
    free(fd.v);
    oppc_array_free(pc, ops.n);
    opvec_free_deep(&ops);
    buf_free(&from_df); buf_free(&to_df);
    r.body = body;
    r.fp_end = caps.fp_end;
    r.fp_start = fp_start_s;
    return r;
}
