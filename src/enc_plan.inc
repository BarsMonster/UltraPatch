/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 *
 * A1 host encoder module -- plan/degrade: degrade_ops_to_journal_budget, plan_encode.
 * Part of the single translation unit rooted at src/patch_generate.c, which
 * #includes the enc_*.inc modules in dependency order. NOT a standalone TU:
 * it relies on the shared prologue (typedefs, file-scope state) and on symbols
 * defined by earlier modules. Umbrella split preserves the single-TU byte-exact
 * wire and model_diff.c's #include "patch_generate.c".
 */
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
static void degrade_ops_to_journal_budget(OpVec *ops, const Buf *to, uint32_t from_size,
                                          uint32_t to_size, size_t budget) {
    int FWD = g_enc_fwd;
    uint8_t *pres = preserve_indices(ops, from_size, to_size);
    size_t total = 0;
    for (uint32_t wi = 0; wi < to_size; wi++) total += pres[wi];
    g_deg_pres_needed = total;
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
                     !a1_row_covered(a, t);
            if (!hz) { k++; continue; }
            int32_t rs = k;
            while (k < dl) {
                a = (int64_t)fp0 + k; t = (int64_t)tp0 + k;
                hz = a >= 0 &&
                     (FWD ? (a < (int64_t)to_size && a >= C)
                          : (a < (int64_t)from_size && a <= C)) &&
                     !a1_row_covered(a, t);
                if (!hz) break;
                k++;
            }
            opvec_push(&out, op_copy(rs - seg, o->diff + seg, k - rs,
                                     to->d + (size_t)tp0 + (size_t)rs, k - rs));
            g_deg_engaged = 1;
            g_deg_converted += (size_t)(k - rs);
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

/* One full op-plan -> emitted body pipeline. variant: 0 = legacy block-matched deltas;
 * 1 = + op-derived field deltas (exact under the bsdiff alignment); 2 = + BL-immediate masking
 * of the bsdiff inputs (copies extend through recompiled code). encode_a1 emits whichever
 * variant's exact body is smallest (ties keep the lowest variant), so this cannot regress. */
typedef struct { int variant, fuzz; } PlanCfg;

static Buf plan_encode(const Buf *from, const Buf *to, const Ranges *fr, const Ranges *tr,
                       PlanCfg cfg, int32_t *fp_end_out, EncStats *st_out) {
    int variant = cfg.variant;
    g_deg_engaged = 0; g_deg_pres_needed = 0; g_deg_converted = 0; g_opc_splits = 0;
    BlockVec blocks[STREAM_N] = {{0}};
    Buf from_df = {0}, to_df = {0};
    data_format_encode(from, to, fr, tr, &from_df, &to_df, blocks,
                       variant >= 2 ? 1 : 0);
    OpVec ops = bsdiff_ops(&from_df, &to_df, cfg.fuzz);
    uint32_t from_size = (uint32_t)from->n, to_size = (uint32_t)to->n;
    FieldDeltaVec fd = build_field_deltas(from, fr, blocks);
    split_nonzero_diff_runs(&ops, &from_df, &to_df);
    if (variant >= 1) merge_op_field_deltas(&fd, &ops, from->d, from_size, to->d, to_size);
    coerce_reloc_literals(&ops, from->d, from_size, to_size, &fd);
    degrade_ops_to_journal_budget(&ops, to, from_size, to_size, A1_JSLOTS);
    /* corrections-cap degradation (same philosophy as the journal budget): OPC_CAP bounds
     * corrections PER OP, so an op that needs more is SPLIT at its median-correction offset
     * (a few geometry bytes of degradation) and everything is recomputed — splitting moves
     * op boundaries, which shifts field detection and thus the corrections themselves, so
     * this iterates to a fixpoint. On the home corpus no op ever exceeds the cap and pass 0
     * computes exactly the untransformed plan (bit-identical wire). */
    uint8_t *presset, *corr; OpPC *pc;
    for (int pass = 0;; pass++) {
        presset = preserve_indices(&ops, from_size, to_size);
        corr = corrections_hybrid(&ops, from->d, to->d, &fd, from_size, to_size, presset);
        pc = preserve_corr_per_op(&ops, from_size, to_size, presset, corr);
        size_t old_n = ops.n;                               /* pc[] is sized for THIS op count */
        int split_any = 0;
        if (pass < 12) {
            int FWDD = g_enc_fwd;
            int32_t *cut = (int32_t *)xmalloc((ops.n ? ops.n : 1) * sizeof(int32_t));
            for (size_t i = 0; i < ops.n; i++) cut[i] = -1;
            for (size_t step = 0; step < ops.n; step++) {
                if (pc[step].corr.n <= A1_OPC_CAP) continue;
                size_t oi = FWDD ? step : ops.n - 1 - step;
                int32_t dl = ops.v[oi].diff_len;
                if (dl < 2) continue;                       /* cannot split further: stays infeasible */
                int32_t m = pc[step].corr.v[pc[step].corr.n / 2].off;
                if (m <= 0 || m >= dl) m = dl / 2;
                if (m <= 0 || m >= dl) continue;
                cut[oi] = m;
                g_opc_splits++;
                split_any = 1;
            }
            if (split_any) {
                OpVec out2 = {0};
                for (size_t oi = 0; oi < ops.n; oi++) {
                    Op *o = &ops.v[oi];
                    if (cut[oi] < 0) { opvec_push(&out2, *o); continue; }
                    /* split keeps the tp/fp walk exact: sub1 advances fp by cut (adj 0),
                     * sub2 carries the remaining diff + the original extras and adj. */
                    opvec_push(&out2, op_copy(cut[oi], o->diff, 0, NULL, 0));
                    opvec_push(&out2, op_copy(o->diff_len - cut[oi], o->diff + cut[oi],
                                              o->extra_len, o->extra, o->adj));
                    free(o->diff); free(o->extra);
                }
                free(ops.v);
                ops = out2;
            }
            free(cut);
        }
        if (!split_any) break;
        free(presset); free(corr);
        for (size_t i = 0; i < old_n; i++) { free(pc[i].pres.v); free(pc[i].corr.v); }
        free(pc);
    }
    int32_t fp_end_s = 0;
    for (size_t i = 0; i < ops.n; i++) fp_end_s += ops.v[i].diff_len + ops.v[i].adj;
    /* degradation snapshot: load-bearing for direction-sweep pruning and A1_DEGRADE_STATS */
    st_out->deg_engaged = g_deg_engaged;
    st_out->deg_pres_needed = g_deg_pres_needed;
    st_out->deg_converted = g_deg_converted;
    st_out->opc_splits = g_opc_splits;
    /* decoder resource-cap feasibility (mirror patch_apply OPC_CAP / JSLOTS): an over-cap plan
     * would be rejected on-device; treat as infeasible so a lower variant ships instead. */
    int feasible = 1;
    { size_t tpres = 0;
      for (size_t i = 0; i < ops.n; i++) { if (pc[i].corr.n > A1_OPC_CAP) feasible = 0; tpres += pc[i].pres.n; }
      if (tpres > A1_JSLOTS) feasible = 0; }
    Buf body = {0};
    if (feasible) {
        body = encode_body(&ops, from->d, from_size, to->d, to_size, &fd, pc);
        if (g_emit_overflow) { buf_free(&body); body = (Buf){0}; feasible = 0; }
    }
    /* An infeasible plan (any variant, INCLUDING the legacy config 0) returns an empty body
     * and the sweep tries the remaining configs — different bsdiff alignments need different
     * preserve/correction budgets, so another variant may fit the decoder caps (measured on
     * foreign firmware: config 0 over-journal while fuzz variants fit). encode_a1 dies only
     * when EVERY config is infeasible. */
    free(presset); free(corr); free(fd.v);
    for (size_t i = 0; i < ops.n; i++) { free(ops.v[i].diff); free(ops.v[i].extra); free(pc[i].pres.v); free(pc[i].corr.v); }
    free(pc); free(ops.v);
    for (int s2 = 0; s2 < STREAM_N; s2++) { for (size_t i = 0; i < blocks[s2].n; i++) free(blocks[s2].v[i].values); free(blocks[s2].v); }
    buf_free(&from_df); buf_free(&to_df);
    *fp_end_out = fp_end_s;
    return body;
}

