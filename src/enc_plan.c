/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 *
 * A1 host encoder module -- plan/degrade: degrade_ops_to_journal_budget, plan_encode.
 * Compiled as a normal internal encoder translation unit.
 */

#include "enc_internal.h"

/* Ordered tie priority and every preparation choice live here. Keeping the legacy variant first
 * is wire-significant: equal-size candidates retain the first entry. */
const PlanSpec PLAN_SPECS[PLAN_SPEC_N] = {
    {0, 11, PLAN_DF_UNMASK, PLAN_RAW_UNMASK_11},
    {1, 11, PLAN_DF_UNMASK, PLAN_RAW_UNMASK_11},
    {2, 11, PLAN_DF_MASK,   PLAN_RAW_MASK_11},
    {1,  6, PLAN_DF_UNMASK, PLAN_RAW_UNMASK_6},
    {1, 20, PLAN_DF_UNMASK, PLAN_RAW_UNMASK_20},
};
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
 * independent of the deployment's NVM page size. Correctness of the transformed plan is
 * still proven per blob by the reference-decoder self-verification. */
static int degrade_hazard_at(const EncCtx *ctx, int32_t fp0, int32_t tp0, int32_t k,
                             uint32_t from_size, uint32_t to_size, int32_t cutoff) {
    int64_t a = (int64_t)fp0 + k, t = (int64_t)tp0 + k;
    int unprotected = ctx->fwd ? a >= cutoff
                               : a >= RC_PACKED_POS_LIMIT || (cutoff >= 0 && a <= cutoff);
    return a >= 0 && a < (int64_t)from_size && a < (int64_t)to_size &&
           unprotected &&
           !row_covered(ctx, a, t);
}

static OpPC *degrade_ops_to_journal_budget(EncCtx *ctx, OpVec *ops, const Buf *to,
                                           const uint8_t *frm, const FieldDeltaVec *fd,
                                           const LdrTargetIndex *ldr,
                                           int32_t fp_start, uint32_t from_size, uint32_t to_size,
                                           PlanCaps *caps) {
    int FWD = ctx->fwd;
    uint8_t *payload = ops->payload;
    /* Derive the preserve total and cutoff from a single preserve_corrections_pc pass over the same
     * folded opwalk+readarr geometry used by body planning. */
    OpPC *pc = preserve_corrections_pc(ctx, ops, fp_start, frm, to->d, fd,
                                       from_size, to_size, ldr, caps);
    size_t total = caps->pres_total;
    ctx->deg_pres_needed = total;
    if (total == caps->pres_kept) return pc;
    int32_t C = caps->pres_cutoff;
    oppc_array_free(pc, ops->n);
    /* FWD protects [0,C). Grow protects [C+1,2^24): high unrepresentable positions are
     * skipped before consuming its budget, then C is the first lower over-budget preserve. */
    OpVec out = { .payload = payload };
    int32_t tp0 = 0, fp0 = fp_start;
    for (size_t oi = 0; oi < ops->n; oi++) {
        Op *o = &ops->v[oi];
        int32_t dl = o->diff_len;
        int behind = FWD ? (fp0 < tp0) : (fp0 > tp0);   /* op reads behind the write frontier */
        int32_t seg = 0, k = 0;
        int split_any = 0;
        while (behind && k < dl) {
            /* hazard = behind-frontier read of an UNPROTECTED overwritten position that the
             * page window does NOT cover. Coverage is periodic within each output page, so
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
            opvec_push(&out, (Op){ rs - seg, k - rs, k - rs });
            /* Journal degradation bypasses normalized copies: extras reconstruct true `to`. */
            memcpy(payload + tp0 + rs, to->d + (size_t)tp0 + (size_t)rs,
                   (size_t)(k - rs));
            ctx->deg_engaged = 1;
            ctx->deg_converted += (size_t)(k - rs);
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
    return NULL;
}

static Buf prep_buf_clone(const Buf *src) {
    Buf out = {(uint8_t *)xmalloc(src->n), src->n, src->n};
    if (src->n) memcpy(out.d, src->d, src->n);
    return out;
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

#ifdef PLAN_PREP_ORACLE
static void plan_prepare_oracle(const PlanPrep *prep, const Buf *from, const Buf *to,
                                const PairAnalysis *pa, size_t index_builds, size_t raw_builds) {
    static const PlanSpec expected[PLAN_SPEC_N] = {
        {0, 11, PLAN_DF_UNMASK, PLAN_RAW_UNMASK_11},
        {1, 11, PLAN_DF_UNMASK, PLAN_RAW_UNMASK_11},
        {2, 11, PLAN_DF_MASK,   PLAN_RAW_MASK_11},
        {1,  6, PLAN_DF_UNMASK, PLAN_RAW_UNMASK_6},
        {1, 20, PLAN_DF_UNMASK, PLAN_RAW_UNMASK_20},
    };
    uint8_t df_seen[PLAN_DF_N] = {0}, raw_seen[PLAN_RAW_N] = {0};
    for (size_t i = 0; i < PLAN_SPEC_N; i++) {
        const PlanSpec *spec = &PLAN_SPECS[i], *want = &expected[i];
        if (spec->variant != want->variant || spec->fuzz != want->fuzz ||
            spec->df != want->df || spec->raw_key != want->raw_key)
            die("plan registry oracle mismatch");
        Buf of = {0}, ot = {0}; FieldDeltaVec fd = {0};
        data_format_encode(from, to, pa, &of, &ot, &fd, spec->df == PLAN_DF_MASK);
        /* bsdiff_ops is deliberately retained as the one-shot build/generate/free path. Its
         * output must stay identical to the cached indexed result used by plan preparation. */
        OpVec raw = bsdiff_ops(&of, &ot, spec->fuzz);
        if (of.n != prep->from_df[spec->df].n || ot.n != prep->to_df[spec->df].n ||
            (of.n && memcmp(of.d, prep->from_df[spec->df].d, of.n)) ||
            (ot.n && memcmp(ot.d, prep->to_df[spec->df].d, ot.n)) ||
            fd.n != prep->fd.n ||
            (fd.n && memcmp(fd.v, prep->fd.v, fd.n * sizeof(*fd.v))) ||
            raw.n != prep->raw[spec->raw_key].n ||
            (raw.n && memcmp(raw.v, prep->raw[spec->raw_key].v, raw.n * sizeof(*raw.v))) ||
            (to->n && memcmp(raw.payload, prep->raw[spec->raw_key].payload, to->n)))
            die("plan preparation oracle mismatch");
        df_seen[spec->df] = 1;
        raw_seen[spec->raw_key] = 1;
        buf_free(&of); buf_free(&ot); free(fd.v); opvec_free(&raw);
    }
    size_t dfs = 0, raws = 0;
    for (size_t i = 0; i < PLAN_DF_N; i++) dfs += df_seen[i];
    for (size_t i = 0; i < PLAN_RAW_N; i++) raws += raw_seen[i];
    if (dfs != PLAN_DF_N || raws != PLAN_RAW_N ||
        index_builds != PLAN_DF_N || raw_builds != PLAN_RAW_N)
        die("plan preparation coverage oracle mismatch");
    fprintf(stderr,
            "PLAN_PREP_ORACLE configs=%u registry=OK normalized=%zu indexes=%zu raw_keys=%zu indexed=OK fd=OK\n",
            (unsigned)PLAN_SPEC_N, dfs, index_builds, raw_builds);
}
#endif

void plan_prepare(PlanPrep *prep, const Buf *from, const Buf *to, const PairAnalysis *pa) {
    memset(prep, 0, sizeof(*prep));
    ldr_target_index_build(&prep->ldr, from->d, (uint32_t)from->n);
    uint16_t raw_owner[PLAN_RAW_N] = {0};
    /* All registered normalizations share the relocation pass. Derive the masked pair from that
     * immutable base before their indexed raw plans are generated. */
    data_format_encode(from, to, pa, &prep->from_df[PLAN_DF_UNMASK],
                       &prep->to_df[PLAN_DF_UNMASK], &prep->fd, 0);
    prep->from_df[PLAN_DF_MASK] = prep_buf_clone(&prep->from_df[PLAN_DF_UNMASK]);
    prep->to_df[PLAN_DF_MASK] = prep_buf_clone(&prep->to_df[PLAN_DF_UNMASK]);
    mask_bl_imms(from->d, prep->from_df[PLAN_DF_MASK].d, from->n);
    mask_bl_imms(to->d, prep->to_df[PLAN_DF_MASK].d, to->n);
#ifdef PLAN_PREP_ORACLE
    size_t index_builds = 0, raw_builds = 0;
#endif
    /* Keep one suffix array live at a time. Within each normalization, registry order drives
     * raw-plan preparation and duplicate keys reuse their first result. */
    for (size_t df = 0; df < PLAN_DF_N; df++) {
        BsdiffIndex index = {0};
        bsdiff_index_build(&index, &prep->from_df[df]);
#ifdef PLAN_PREP_ORACLE
        index_builds++;
#endif
        for (size_t i = 0; i < PLAN_SPEC_N; i++) {
            const PlanSpec *spec = &PLAN_SPECS[i];
            if (spec->df >= PLAN_DF_N || spec->raw_key >= PLAN_RAW_N)
                die("invalid plan registry");
            if (spec->df != df) continue;
            uint16_t owner = (uint16_t)(((uint16_t)spec->df + 1u) << 8) | spec->fuzz;
            uint16_t prev = raw_owner[spec->raw_key];
            if (prev) {
                if (prev != owner) die("invalid plan registry");
                continue;
            }
            raw_owner[spec->raw_key] = owner;
            prep->raw[spec->raw_key] =
                bsdiff_ops_indexed(&index, &prep->to_df[df], spec->fuzz);
#ifdef PLAN_PREP_ORACLE
            raw_builds++;
#endif
        }
        bsdiff_index_free(&index);
    }
#ifdef PLAN_PREP_ORACLE
    plan_prepare_oracle(prep, from, to, pa, index_builds, raw_builds);
#endif
}

void plan_prepare_free(PlanPrep *prep) {
    for (int i = 0; i < PLAN_DF_N; i++) { buf_free(&prep->from_df[i]); buf_free(&prep->to_df[i]); }
    free(prep->fd.v);
    for (int i = 0; i < PLAN_RAW_N; i++) opvec_free(&prep->raw[i]);
    ldr_target_index_free(&prep->ldr);
#ifdef LDR_INDEX_STATS
    ldr_target_index_stats_report();
#endif
#ifdef SPAN_DEQUE_STATS
    span_deque_stats_report();
#endif
#ifdef OUT_ENVELOPE_STATS
    out_envelope_stats_report();
#endif
#ifdef SUFFIX_LCP_STATS
    suffix_lcp_stats_report();
#endif
    memset(prep, 0, sizeof(*prep));
}

static OpVec build_candidate_ops(EncCtx *ctx, const Buf *from, const Buf *to,
                                 const PlanPrep *prep, const PlanSpec *spec, FieldDeltaVec *fd) {
    *fd = prep_fd_clone(&prep->fd);
    OpVec ops = prep_ops_clone(&prep->raw[spec->raw_key], to->n);
    const Buf *from_df = &prep->from_df[spec->df], *to_df = &prep->to_df[spec->df];
    uint32_t from_size = (uint32_t)from->n, to_size = (uint32_t)to->n;
    split_nonzero_diff_runs(ctx, &ops, from_df, to_df);
    if (spec->variant >= 1)
        merge_op_field_deltas(fd, &ops, from->d, from_size, to->d, to_size, &prep->ldr);
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

/* corrections-cap degradation (same philosophy as the journal budget): OPC_CAP bounds
 * corrections PER OP, so an op that needs more is SPLIT at its median-correction offset
 * (a few geometry bytes of degradation) and everything is recomputed — splitting moves
 * op boundaries, which shifts field detection and thus the corrections themselves, so
 * this iterates to a fixpoint. On the home corpus no op ever exceeds the cap and pass 0
 * computes exactly the untransformed plan (bit-identical wire). */
static OpPC *build_pc_fixpoint(EncCtx *ctx, OpVec *ops, int32_t fp_start,
                               const Buf *from, const Buf *to,
                               const FieldDeltaVec *fd, const LdrTargetIndex *ldr,
                               OpPC *pc, PlanCaps *caps) {
    uint32_t from_size = (uint32_t)from->n, to_size = (uint32_t)to->n;
    for (;;) {
        if (!pc)
            pc = preserve_corrections_pc(ctx, ops, fp_start, from->d, to->d,
                                         fd, from_size, to_size, ldr, caps);
        size_t old_n = ops->n;                               /* pc[] is sized for THIS op count */
        int split_any = split_overfull_corrections(ctx, ops, pc);
        if (!split_any) break;
        /* Each cut partitions one non-empty output span into two. Therefore ops->n grows
         * strictly and can do so at most to_size times, independent of correction churn. */
        if (ops->n <= old_n || ops->n > to_size)
            die("correction split progress invariant");
        oppc_array_free(pc, old_n);
        pc = NULL;
    }
    return pc;
}

/* One full op-plan -> emitted body pipeline. variant: 0 = legacy block-matched deltas;
 * 1 = + op-derived field deltas (exact under the bsdiff alignment); 2 = + BL-immediate masking
 * of the bsdiff inputs (copies extend through recompiled code). encode_a1 emits whichever
 * variant's exact body is smallest (ties keep the lowest variant), so this cannot regress. */
PlanResult plan_encode(EncCtx *ctx, const Buf *from, const Buf *to,
                       const PlanPrep *prep, const PlanSpec *spec) {
    PlanResult r = {0};
    ctx->deg_engaged = 0; ctx->deg_pres_needed = 0; ctx->deg_converted = 0; ctx->opc_splits = 0;
    FieldDeltaVec fd = {0};
    PlanCaps caps;
    OpVec ops = build_candidate_ops(ctx, from, to, prep, spec, &fd);
    uint32_t from_size = (uint32_t)from->n, to_size = (uint32_t)to->n;
    int32_t fp_start_s = fold_zero_ops(&ops);
    OpPC *pc = degrade_ops_to_journal_budget(ctx, &ops, to, from->d, &fd, &prep->ldr,
                                             fp_start_s, from_size, to_size, &caps);
    /* A fully degraded diff with a nonzero source adjustment leaves a new pure seek.
     * Normalize only the mutated path again; the reusable PC path remains untouched. */
    if (!pc) fp_start_s += fold_zero_ops(&ops);
    pc = build_pc_fixpoint(ctx, &ops, fp_start_s, from, to, &fd, &prep->ldr, pc, &caps);
    /* degradation snapshot: load-bearing for direction-sweep pruning and DEGRADE_STATS */
    r.st = (EncStats){ ctx->deg_engaged, ctx->deg_pres_needed, ctx->deg_converted, ctx->opc_splits };
    /* decoder resource-cap feasibility (mirror patch_apply OPC_CAP / JSLOTS): an over-cap plan
     * would be rejected on-device; treat as infeasible so a lower variant ships instead. */
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
     * preserve/correction budgets, so another variant may fit the decoder caps (measured on
     * foreign firmware: config 0 over-journal while fuzz variants fit). encode_a1 dies only
     * when EVERY config is infeasible. */
    free(fd.v);
    oppc_array_free(pc, ops.n);
    opvec_free(&ops);
    r.body = body;
    r.fp_end = caps.fp_end;
    r.fp_start = fp_start_s;
    return r;
}
