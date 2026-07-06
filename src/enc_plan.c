/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 *
 * A1 host encoder module -- plan/degrade: degrade_ops_to_journal_budget, plan_encode.
 * Compiled as a normal internal encoder translation unit.
 */

#include "enc_internal.h"
/* ---- journal-budget degradation (encoder-only; wire format and decoder untouched) ----
 * The decoder's never-evict journal holds at most A1_JSLOTS overwritten source bytes. When
 * the ideal op plan needs more, convert the OVER-BUDGET read-after-overwrite copy regions
 * into plain extra bytes (the exact to-image bytes, shipped in the content stream):
 * compression degrades — those bytes code as literals/LZ instead of free copies — but the
 * plan becomes journal-feasible instead of refusing. Preserves are journaled in apply order
 * (strictly ascending output positions for FWD, descending for grow — the NVM gate pins
 * frontier monotonicity), so the first A1_JSLOTS preserves in that order stay protected and
 * every read of a later (unprotected) overwritten position is converted. A read-behind-
 * frontier within one op sits at the constant offset fp0-tp0, so the conversion range is
 * contiguous per op and each affected op splits into at most two wire ops. Every remaining
 * overwritten read still goes through the journal — the invariant that keeps the wire
 * independent of the deployment's NVM row size. Correctness of the transformed plan is
 * still proven per blob by the reference-decoder self-verification. */
static void degrade_ops_to_journal_budget(EncCtx *ctx, OpVec *ops, const Buf *to, uint32_t from_size,
                                          uint32_t to_size, size_t budget) {
    int FWD = ctx->fwd;
    uint8_t *pres = preserve_indices(ctx, ops, from_size, to_size);
    size_t total = 0;
    for (uint32_t wi = 0; wi < to_size; wi++) total += pres[wi];
    ctx->deg_pres_needed = total;
    if (total <= budget) { free(pres); return; }
    /* cutoff C = position of the first preserve past the budget, in apply order. pres[] is
     * indexed by apply-order write index: wi == position for FWD, to_size-1-wi for grow. */
    int32_t C = 0;
    { size_t cnt = 0;
      for (uint32_t wi = 0; wi < to_size; wi++) {
          if (!pres[wi]) continue;
          if (cnt == budget) { C = FWD ? (int32_t)wi : (int32_t)(to_size - 1u - wi); break; }
          cnt++;
      } }
    free(pres);
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
            int64_t a = (int64_t)fp0 + k, t = (int64_t)tp0 + k;
            int hz = a >= 0 &&
                     (FWD ? (a < (int64_t)to_size && a >= C)
                          : (a < (int64_t)from_size && a <= C)) &&
                     !a1_row_covered(ctx, a, t);
            if (!hz) { k++; continue; }
            int32_t rs = k;
            while (k < dl) {
                a = (int64_t)fp0 + k; t = (int64_t)tp0 + k;
                hz = a >= 0 &&
                     (FWD ? (a < (int64_t)to_size && a >= C)
                          : (a < (int64_t)from_size && a <= C)) &&
                     !a1_row_covered(ctx, a, t);
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
            free(o->diff); free(o->extra);
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
                                 const Ranges *fr, const Ranges *tr, PlanCfg cfg,
                                 BlockVec blocks[STREAM_N], Buf *from_df, Buf *to_df,
                                 FieldDeltaVec *fd) {
    int variant = cfg.variant;
    data_format_encode(from, to, fr, tr, from_df, to_df, blocks,
                       variant >= 2 ? 1 : 0);
    OpVec ops = bsdiff_ops(from_df, to_df, cfg.fuzz);
    uint32_t from_size = (uint32_t)from->n, to_size = (uint32_t)to->n;
    *fd = build_field_deltas(from, fr, blocks);
    split_nonzero_diff_runs(ctx, &ops, from_df, to_df);
    if (variant >= 1) merge_op_field_deltas(fd, &ops, from->d, from_size, to->d, to_size);
    coerce_reloc_literals(ctx, &ops, from->d, from_size, to_size, fd);
    degrade_ops_to_journal_budget(ctx, &ops, to, from_size, to_size, A1_JSLOTS);
    return ops;
}

static int split_overfull_corrections(EncCtx *ctx, OpVec *ops, const OpPC *pc, int pass) {
    int split_any = 0;
    if (pass >= 12) return 0;
    int FWDD = ctx->fwd;
    int32_t *cut = (int32_t *)xmalloc((ops->n ? ops->n : 1) * sizeof(int32_t));
    for (size_t i = 0; i < ops->n; i++) cut[i] = -1;
    for (size_t step = 0; step < ops->n; step++) {
        if (pc[step].corr.n <= A1_OPC_CAP) continue;
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
            free(o->diff); free(o->extra);
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
static OpPC *build_pc_fixpoint(EncCtx *ctx, OpVec *ops, const Buf *from, const Buf *to,
                               const FieldDeltaVec *fd) {
    uint32_t from_size = (uint32_t)from->n, to_size = (uint32_t)to->n;
    uint8_t *presset = NULL;
    OpPC *pc = NULL;
    for (int pass = 0;; pass++) {
        presset = preserve_indices(ctx, ops, from_size, to_size);
        pc = preserve_corrections_pc(ctx, ops, from->d, to->d, fd, from_size, to_size, presset);
        size_t old_n = ops->n;                               /* pc[] is sized for THIS op count */
        int split_any = split_overfull_corrections(ctx, ops, pc, pass);
        free(presset);
        if (!split_any) break;
        oppc_array_free(pc, old_n);
    }
    return pc;
}

static int32_t opvec_fp_end(const OpVec *ops) {
    int32_t fp_end = 0;
    for (size_t i = 0; i < ops->n; i++) fp_end += ops->v[i].diff_len + ops->v[i].adj;
    return fp_end;
}

static int32_t opvec_fp_start(const OpVec *ops) {
    int32_t *ea = (int32_t *)xmalloc((ops->n ? ops->n : 1) * sizeof(int32_t));
    uint8_t *sk = (uint8_t *)xmalloc(ops->n ? ops->n : 1);
    int32_t fp_start = fold_zero_ops(ops, ea, sk);
    free(ea); free(sk);
    return fp_start;
}

static int plan_caps_feasible(const OpVec *ops, const OpPC *pc) {
    int feasible = 1;
    size_t tpres = 0;
    for (size_t i = 0; i < ops->n; i++) {
        if (pc[i].corr.n > A1_OPC_CAP) feasible = 0;
        tpres += pc[i].pres.n;
    }
    if (tpres > A1_JSLOTS) feasible = 0;
    return feasible;
}

static void encstats_from_ctx(const EncCtx *ctx, EncStats *st) {
    st->deg_engaged = ctx->deg_engaged;
    st->deg_pres_needed = ctx->deg_pres_needed;
    st->deg_converted = ctx->deg_converted;
    st->opc_splits = ctx->opc_splits;
}

/* One full op-plan -> emitted body pipeline. variant: 0 = legacy block-matched deltas;
 * 1 = + op-derived field deltas (exact under the bsdiff alignment); 2 = + BL-immediate masking
 * of the bsdiff inputs (copies extend through recompiled code). encode_a1 emits whichever
 * variant's exact body is smallest (ties keep the lowest variant), so this cannot regress. */
Buf plan_encode(EncCtx *ctx, const Buf *from, const Buf *to, const Ranges *fr, const Ranges *tr,
                       PlanCfg cfg, int32_t *fp_end_out, int32_t *fp_start_out, EncStats *st_out) {
    ctx->deg_engaged = 0; ctx->deg_pres_needed = 0; ctx->deg_converted = 0; ctx->opc_splits = 0;
    BlockVec blocks[STREAM_N] = {{0}};
    Buf from_df = {0}, to_df = {0};
    FieldDeltaVec fd = {0};
    OpVec ops = build_candidate_ops(ctx, from, to, fr, tr, cfg, blocks, &from_df, &to_df, &fd);
    uint32_t from_size = (uint32_t)from->n, to_size = (uint32_t)to->n;
    OpPC *pc = build_pc_fixpoint(ctx, &ops, from, to, &fd);
    int32_t fp_end_s = opvec_fp_end(&ops);
    /* Feature 7B initial source seek: leading zero-output ops fold into this shipped seed (see
     * fold_zero_ops). Derived from the SAME final ops array emit_body folds, so envelope and body
     * agree bit-for-bit. Sum(dl+adj) is fold-invariant, so fp_end_s above stays correct. */
    int32_t fp_start_s = opvec_fp_start(&ops);
    /* degradation snapshot: load-bearing for direction-sweep pruning and A1_DEGRADE_STATS */
    encstats_from_ctx(ctx, st_out);
    /* decoder resource-cap feasibility (mirror patch_apply OPC_CAP / JSLOTS): an over-cap plan
     * would be rejected on-device; treat as infeasible so a lower variant ships instead. */
    int feasible = plan_caps_feasible(&ops, pc);
    Buf body = {0};
    if (feasible) {
        body = encode_body(ctx, &ops, from->d, from_size, to->d, to_size, &fd, pc);
        if (g_emit_overflow) { buf_free(&body); body = (Buf){0}; feasible = 0; }
    }
    /* An infeasible plan (any variant, INCLUDING the legacy config 0) returns an empty body
     * and the sweep tries the remaining configs — different bsdiff alignments need different
     * preserve/correction budgets, so another variant may fit the decoder caps (measured on
     * foreign firmware: config 0 over-journal while fuzz variants fit). encode_a1 dies only
     * when EVERY config is infeasible. */
    free(fd.v);
    oppc_array_free(pc, ops.n);
    opvec_free_deep(&ops);
    blockvec_array_free(blocks);
    buf_free(&from_df); buf_free(&to_df);
    *fp_end_out = fp_end_s;
    *fp_start_out = fp_start_s;
    return body;
}
