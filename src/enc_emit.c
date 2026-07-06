/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 *
 * A1 host encoder module -- body assembly / range-coder emit: op_emit_content, emit_delta, emit_geom_pc, emit_body, encode_body.
 * Compiled as a normal internal encoder translation unit.
 */

#include "enc_internal.h"
/* One field injection (delta escaped into the LZ content stream): cc = byte cursor within its op's
 * content slice, kind = EV_BL/EV_EX, fpk = from-image field address, delta = plain (no-map) wire
 * residual. The map variant derives from delta+fpk (field_residual) without re-walking. */
typedef struct { uint32_t cc; int kind; uint32_t fpk; int64_t delta; } Inj;
typedef struct { Inj *v; size_t n, cap; } InjVec;

static void inj_push(InjVec *v, uint32_t cc, int kind, uint32_t fpk, int64_t delta) {
    v->v = (Inj *)vec_reserve(v->v, &v->cap, v->n + 1, sizeof(v->v[0]), 16);
    v->v[v->n++] = (Inj){cc, kind, fpk, delta};
}

static void injvec_array_free(InjVec *v, size_t n) {
    if (!v) return;
    for (size_t i = 0; i < n; i++) free(v[i].v);
    free(v);
}

typedef struct {
    const Op *o;
    const IVec *lits;
    Buf *tmp;
    Buf *content;
    Buf *tags;
    uint32_t cc;
    int FWD;
    int32_t base, cprev, nextpos;
    size_t li;
} EncLitCursor;

static void enc_litcur_step(EncLitCursor *lc) {
    if (lc->li < lc->lits->n) {
        int32_t p = lc->FWD ? lc->lits->v[lc->li] : lc->lits->v[lc->lits->n - 1u - lc->li];
        lc->cc += (uint32_t)rc_uleb_len((uint32_t)(lc->FWD ? p - lc->base : lc->base - p)) + 1u;
        lc->base = p;
        lc->nextpos = p;
        lc->li++;
    } else {
        lc->nextpos = -1;
    }
}

static void enc_litcur_emit(EncLitCursor *lc, int32_t k) {
    lc->tmp->n = 0;
    put_uleb(lc->tmp, (uint32_t)(lc->FWD ? k - lc->cprev : lc->cprev - k));
    buf_write(lc->content, lc->tmp->d, lc->tmp->n);
    for (size_t i = 0; i < lc->tmp->n; i++) buf_put(lc->tags, 0);
    buf_put(lc->content, lc->o->diff[k]);
    buf_put(lc->tags, 0);
    lc->cprev = k;
}

/* Single per-op decoder-order walk: emit this op's content/tags bytes (uLEB nlit + per-literal
 * uLEB-gap+byte + extras) AND record the field-injection cursors (Inj.cc) in the same pass, so the
 * two can never diverge in byte layout. Routes through the shared FieldWalk (fw_next), the one
 * mirror of the decoder sa_apply_op window skeleton. FWD/descending asymmetry is preserved exactly:
 * grow writes the extras (byte-reversed) before the dl body and gaps step back from dl; FWD writes
 * extras after and gaps measure from 0. tp0 = this op's to-image start (extras tag parity).
 * cc is a READ-AHEAD cursor: like the decoder A1LitCur, the next literal's gap+byte is consumed before
 * the field window that follows it, so cc leads content->n by the pending literal's cost. */
static void op_emit_content(const Op *o, int FWD, const uint8_t *frm, uint32_t from_size,
                            int32_t fp0, int32_t tp0, const FieldDeltaVec *fd, const IVec *ldr,
                            Buf *content, Buf *tags, InjVec *out) {
    /* Feature 7B: a zero-OUTPUT op (diff_len==0 && extra_len==0) is folded off the wire (its seek is
     * carried by a neighbor / the initial fp_start), so it contributes NOTHING to the content stream.
     * Return before emitting even the nl=0 literal-count uLEB — otherwise that stray byte would sit in
     * content with no op to consume it on decode (emit_body skips the op entirely). */
    if (o->diff_len == 0 && o->extra_len == 0) return;
    IVec lits = {0};
    for (int32_t k = 0; k < o->diff_len; k++) if (o->diff[k]) ivec_push(&lits, k);
    Buf tmp = {0};
    put_uleb(&tmp, (uint32_t)lits.n);
    buf_write(content, tmp.d, tmp.n);
    for (size_t i = 0; i < tmp.n; i++) buf_put(tags, 0);
    int32_t exstart = tp0 + o->diff_len;
    if (!FWD)   /* grow: extras precede the dl body, byte-reversed in the content stream */
        for (int32_t e = o->extra_len - 1; e >= 0; e--) { buf_put(content, o->extra[e]); buf_put(tags, (uint8_t)((exstart + e) & 1)); }
    EncLitCursor lc = { o, &lits, &tmp, content, tags,
        (uint32_t)rc_uleb_len((uint32_t)lits.n) + (FWD ? 0u : (uint32_t)o->extra_len),
        FWD, FWD ? 0 : o->diff_len, FWD ? 0 : o->diff_len, -1, 0 };
    enc_litcur_step(&lc);   /* prime: read-ahead the first literal (mirror litcur_init) */
    FieldWalk w; fw_init(&w, FWD, frm, from_size, fd, ldr, o, fp0, o->diff_len);
    while (fw_next(&w)) {
        if (w.is_field && (w.ev.type == EV_BL || w.ev.type == EV_EX)) {   /* pure field: no literals, inject delta */
            inj_push(out, lc.cc, w.ev.type, (uint32_t)(fp0 + w.pos),
                     field_residual(w.ev.type, frm, (uint32_t)(fp0 + w.pos), w.ev.delta, NULL, NULL, 0));
        } else if (w.is_field) {   /* EV_SBL still-dirty window: its bytes ship as ordinary literals */
            if (FWD) for (int b = 0; b < 4; b++) { if (w.pos + b == lc.nextpos) { enc_litcur_emit(&lc, w.pos + b); enc_litcur_step(&lc); } }
            else     for (int b = 3; b >= 0; b--) { if (w.pos + b == lc.nextpos) { enc_litcur_emit(&lc, w.pos + b); enc_litcur_step(&lc); } }
        } else if (w.pos == lc.nextpos) { enc_litcur_emit(&lc, w.pos); enc_litcur_step(&lc); }
    }
    if (FWD)
        for (int32_t e = 0; e < o->extra_len; e++) { buf_put(content, o->extra[e]); buf_put(tags, (uint8_t)((exstart + e) & 1)); }
    free(lits.v); buf_free(&tmp);
}

/* MTF escape value: zigzag uLEB, each byte through the adaptive dval bit-tree (mirror s_bv). */
void bv_encode(A1BitTree *t, REnc *r, int64_t x) {
    uint32_t v = rc_zz32((int32_t)x);
    for (;;) {
        uint8_t b = (uint8_t)(v & 0x7fu);
        v >>= 7;
        bt_encode(t, r, v ? (uint8_t)(b | 0x80u) : b, RC_DVAL_RATE);
        if (!v) break;
    }
}

void dr_init_e(DRE *d, int64_t *dic, int cap, uint16_t hitseed) {
    d->dic = dic; d->cap = (uint16_t)cap; d->K = 1; d->dic[0] = 0; d->last = 0; d->rh = 0; d->hit = hitseed;
    for (int i = 0; i < 4; i++) d->rep[i] = RC_PHALF;
}

enum { DR_TR_REP, DR_TR_HIT, DR_TR_ESC, DR_TR_OVER };
typedef struct { uint8_t kind, ri; uint32_t idx; } DRTrans;

static DRTrans dr_transition(DRE *D, int64_t delta) {
    DRTrans tr = { DR_TR_REP, (uint8_t)(D->rh | (D->last == 0 ? 2 : 0)), 0 };
    if (delta == D->last) { D->rh = 1; return tr; }
    D->rh = 0;
    D->last = delta;
    int j = -1;
    for (int i = 0; i < D->K; i++) if (D->dic[i] == delta) { j = i; break; }
    if (j > 0) {
        int64_t t = D->dic[j];
        for (int i = j; i > 0; i--) D->dic[i] = D->dic[i - 1];
        D->dic[0] = t;
        tr.kind = DR_TR_HIT;
        tr.idx = (uint32_t)(j - 1);
    } else {
        if (j == 0) die("unexpected delta dict index 0");
        if (D->K >= D->cap) { tr.kind = DR_TR_OVER; return tr; }
        for (int i = D->K; i > 0; i--) D->dic[i] = D->dic[i - 1];
        D->dic[0] = delta;
        D->K++;
        tr.kind = DR_TR_ESC;
    }
    return tr;
}

/* Set when an emit exceeds a decoder resource cap (e.g. the MTF dict): the candidate
 * plan/parse is infeasible on the wire; callers treat the emitted size as +infinity. */
int g_emit_overflow;

void emit_delta(Models *M, REnc *r, int kind, int64_t delta) {
    if (g_emit_overflow) return;   /* stream already infeasible: state frozen, output discarded */
    DRE *D = kind == EV_BL ? &M->dr_bl : &M->dr_ex;
    A1IdxUnary *gix = kind == EV_BL ? &M->dibl : &M->diex;
    DRTrans tr = dr_transition(D, delta);
    if (tr.kind == DR_TR_REP) {
        re_bit(r, &D->rep[tr.ri], 1, RC_S_BIT_RATE);
        return;
    }
    re_bit(r, &D->rep[tr.ri], 0, RC_S_BIT_RATE);
    if (tr.kind == DR_TR_OVER) { g_emit_overflow = 1; return; }
    if (tr.kind == DR_TR_HIT) {
        re_bit(r, &D->hit, 1, RC_S_BIT_RATE);
        idx_encode(gix, r, tr.idx);
    } else {
        re_bit(r, &D->hit, 0, RC_S_BIT_RATE);
        bv_encode(&M->dval, r, delta);
    }
}

static void emit_geom_pc(REnc *r, Models *M, const Op *o, const OpPC *pc) {
    ug_encode(&M->gdl, r, (uint32_t)o->diff_len);
    ug_encode(&M->gel, r, (uint32_t)o->extra_len);
    ug_encode(&M->gadj, r, rc_zz32(o->adj));
    ug_encode(&M->pgn, r, (uint32_t)pc->pres.n);
    int32_t prev = 0;
    for (size_t i = 0; i < pc->pres.n; i++) { ug_encode(i ? &M->pg2 : &M->pg, r, (uint32_t)(pc->pres.v[i] - prev)); prev = pc->pres.v[i]; }
    ug_encode(&M->pgn, r, (uint32_t)pc->corr.n);
    prev = 0;
    for (size_t i = 0; i < pc->corr.n; i++) {
        ug_encode(i ? &M->pg2 : &M->pg, r, (uint32_t)(pc->corr.v[i].off - prev));
        prev = pc->corr.v[i].off;
        bt_encode(&M->dval, r, pc->corr.v[i].byte, RC_DVAL_RATE);
    }
}

/* Emit the full body (token geom/preserve/delta streams interleaved with the LZ content tokens)
 * for a given parse `seq` and rice parameter `kd`, finalize with the optimal flush, and return
 * the flushed Buf. The geom/preserve/delta streams are parse-independent, so this same routine
 * both measures a candidate parse's true shipped size and produces the final output. */
/* Feature 7B: fold zero-OUTPUT ops (diff_len==0 && extra_len==0 — pure source seeks that write
 * nothing) out of the wire so `nops` can be dropped and the decoder can frontier-terminate. Such an
 * op only advances the source pointer by its `adj`; that seek is absorbed WITHOUT changing any kept
 * op's source alignment: a LEADING (canonical-order) zero-op folds into the initial source-seek
 * *fp_start* (shipped in the envelope; seeds the FWD walk / is the grow landing point), and every
 * other zero-op folds its adj into the PREVIOUS kept op's effective adj (its post-copy seek grows to
 * cover the skipped seek). Sum of (dl+adj) — hence fp_end — is invariant. skip[i]=1 marks a dropped
 * op; eff_adj[i] is the adj to emit for a kept op. Returns fp_start. Pure function of `ops`, so the
 * envelope writer (plan_encode) and every emit_body call derive the identical fold. */
int32_t fold_zero_ops(const OpVec *ops, int32_t *eff_adj, uint8_t *skip) {
    int32_t fp_start = 0;
    int last_kept = -1;
    for (size_t i = 0; i < ops->n; i++) {
        eff_adj[i] = ops->v[i].adj;
        skip[i] = 0;
        if (ops->v[i].diff_len == 0 && ops->v[i].extra_len == 0) {
            skip[i] = 1;
            if (last_kept < 0) fp_start += ops->v[i].adj;      /* leading -> initial source seek */
            else eff_adj[last_kept] += ops->v[i].adj;          /* interior/trailing -> previous kept op */
        } else {
            last_kept = (int)i;
        }
    }
    return fp_start;
}

typedef struct {
    const TokenVec *seq;
    const Buf *content;
    const Buf *tags;
    Models *M;
    REnc *rc;
    int FWD;
    int out_en;
    uint32_t oexp;
    size_t tok_i, pos, span_pos;
    int tok_mode;
    int32_t tok_left;
    int last_span;
    uint8_t prevlit;
    Token cur;
} EmitCursor;

static void emit_cursor_start_token(EmitCursor *ec) {
    Models *M = ec->M;
    REnc *rc = ec->rc;
    if (ec->tok_i >= ec->seq->n) die("content token underrun");
    ec->cur = ec->seq->v[ec->tok_i++];
    if (ec->cur.type == 'S') {
        if (ec->last_span) die("adjacent span tokens on wire");
        fl_encode(&M->flag, rc, 0);
        ug_encode(&M->gs, rc, (uint32_t)ec->cur.len - 1u);
        ec->tok_mode = 'S';
        ec->tok_left = ec->cur.len;
        ec->span_pos = 0;
        ec->last_span = 1;
    } else if (ec->cur.type == 'O') {
        if (ec->last_span) { M->flag.h = ((M->flag.h << 1) | 1) & 3; ec->last_span = 0; }
        else fl_encode(&M->flag, rc, 1);
        re_bit(rc, &M->rep0[M->rep0h], 0, RC_S_BIT_RATE);
        M->rep0h = 0;
        re_bit(rc, &M->outb, 1, RC_S_BIT_RATE);
        ug_encode(&M->go, rc, rc_outmatch_delta((uint32_t)ec->cur.dist, ec->oexp));
        ec->oexp = rc_outmatch_next_expect(ec->FWD, (uint32_t)ec->cur.dist, (uint32_t)ec->cur.len);
        ug_encode(&M->glo, rc, (uint32_t)ec->cur.len - RC_OUTMATCH_MIN);
        ec->tok_mode = 'R';
        ec->tok_left = ec->cur.len;
    } else {
        if (ec->last_span) { M->flag.h = ((M->flag.h << 1) | 1) & 3; ec->last_span = 0; }
        else fl_encode(&M->flag, rc, 1);
        if ((int32_t)ec->cur.dist == M->last_dist) {
            re_bit(rc, &M->rep0[M->rep0h], 1, RC_S_BIT_RATE);
            M->rep0h = 1;
        } else {
            re_bit(rc, &M->rep0[M->rep0h], 0, RC_S_BIT_RATE);
            M->rep0h = 0;
            if (ec->out_en) re_bit(rc, &M->outb, 0, RC_S_BIT_RATE);
            ug_encode(&M->gd, rc, (uint32_t)ec->cur.dist - 1u);
            M->last_dist = (int32_t)ec->cur.dist;
        }
        ug_encode(&M->gl, rc, (uint32_t)ec->cur.len - 1u);
        ec->tok_mode = 'R';
        ec->tok_left = ec->cur.len;
    }
}

static void emit_cursor_to(EmitCursor *ec, size_t end) {
    if (end < ec->pos || end > ec->content->n) die("invalid content cursor");
    while (ec->pos < end) {
        if (!ec->tok_mode) emit_cursor_start_token(ec);
        size_t nn = (size_t)ec->tok_left < (end - ec->pos) ? (size_t)ec->tok_left : (end - ec->pos);
        if (ec->tok_mode == 'S') {
            for (size_t i = 0; i < nn; i++) {
                uint8_t byte = ec->content->d[(size_t)ec->cur.start + ec->span_pos + i];
                int tag = ec->tags->d[ec->pos];
                bt_encode(tag ? &ec->M->lit1 : &ec->M->lit0[LIT0_SEL(ec->prevlit)],
                          ec->rc, byte, tag ? RC_LIT1_RATE : RC_LIT0_RATE);
                ec->prevlit = byte;
                ec->pos++;
            }
            ec->span_pos += nn;
        } else {
            ec->pos += nn;
            if (nn) ec->prevlit = ec->content->d[ec->pos - 1u];
        }
        ec->tok_left -= (int32_t)nn;
        if (ec->tok_left == 0) ec->tok_mode = 0;
    }
}

static Buf emit_body(const TokenVec *seq, int kd, int ko, const OpVec *ops, int FWD,
                     const uint8_t *frm, uint32_t from_size,
                     const OpPC *pc, const Buf *content, const Buf *tags,
                     const size_t *ends, const InjVec *inj,
                     const uint32_t *mb, const int32_t *mv, int mn) {
    Models M;
    memset(&M, 0, sizeof(M));
    for (int c = 0; c < LIT0_CTX; c++) lit_tree_seed_e(frm, from_size, 0, &M.lit0[c]);
    lit_tree_seed_e(frm, from_size, 1, &M.lit1);
    a1_fl_init(&M.flag);
    a1_bt_init(&M.dval);
    ug_init_e(&M.gd, 'r', kd);
    ug_init_e(&M.gl, 'g', 0);
    ug_seed_cont_e(&M.gl, RC_SEED_DEPTH_GL);   /* matches len>=3 => M_gl first unary bit always continue; depth in rc_models.h */
    ug_init_e(&M.gs, 'g', 0);
    ug_init_e(&M.go, 'r', ko);
    ug_init_e(&M.glo, 'g', 0);
    M.outb = RC_PHALF;
    ug_init_e(&M.pg, 'g', 0);
    ug_init_e(&M.pgn, 'g', 0);
    ug_init_e(&M.pg2, 'g', 0);
    ug_seed_cont_e(&M.pg2, RC_SEED_DEPTH_PG2);   /* rest preserve/corr gaps >=1 (strictly-increasing distinct
                                  * offsets) => M_pg2 first unary bit always continue; depth in rc_models.h. */
    ug_init_e(&M.gdl, 'g', 0);   /* neutral at top: BORROWED to code the map header below, then re-init'd
                                  * with the seed_cont apply-phase priors after the map, before the token loop. */
    ug_init_e(&M.gel, 'g', 0);
    ug_init_e(&M.gadj, 'g', 0);
    a1_idx_init(&M.dibl, RC_IDX_SEED);   /* dict-index seed from rc_models.h */
    a1_idx_init(&M.diex, RC_IDX_SEED);
    dr_init_e(&M.dr_bl, M.dic_bl, DR_KCAP_BL, DR_HIT_INIT);
    dr_init_e(&M.dr_ex, M.dic_ex, DR_KCAP_EX, DR_HIT_INIT);
    M.rep0[0] = M.rep0[1] = RC_REP0_INIT; M.rep0h = 0; M.last_dist = 0;   /* rep0 prior toward 0; mirror patch_apply */
    g_emit_overflow = 0;
    int out_en = 0;
    for (size_t i = 0; i < seq->n; i++) if (seq->v[i].type == 'O') { out_en = 1; break; }
    uint32_t oexp = 0;
    if (!FWD) { for (size_t i = 0; i < ops->n; i++) oexp += (uint32_t)(ops->v[i].diff_len + ops->v[i].extra_len); }
    REnc rc;
    re_init(&rc);
    /* piecewise shift map: ADAPTIVE-gamma count + per-entry gap (first absolute, later -1) + zz value,
     * coded through the BORROWED M.gdl (count+gaps) / M.gadj (zz values) gamma models — mirror of the
     * patch_apply decode_body map reader (bit-exact wire; s_ug_gamma == ug_encode 'g'). */
    ug_encode(&M.gdl, &rc, (uint32_t)mn);
    { uint32_t prev = 0;
      for (int i = 0; i < mn; i++) {
          uint32_t gap = mb[i] - prev;
          ug_encode(&M.gdl, &rc, i ? gap - 1u : gap);
          prev = mb[i];
          ug_encode(&M.gadj, &rc, rc_zz32(mv[i]));
      } }
    /* re-init the borrowed models to their apply-phase state (clear map pollution + apply seed_cont),
     * mirroring the ugg_init/ugg_seed_cont(&M_gdl,&M_gadj) in decode_body before its token loop. */
    ug_init_e(&M.gdl, 'g', 0);  ug_seed_cont_e(&M.gdl, RC_SEED_DEPTH_GDL);
    ug_init_e(&M.gadj, 'g', 0); ug_seed_cont_e(&M.gadj, RC_SEED_DEPTH_GADJ);
    re_raw(&rc, out_en);   /* out-match enable bit (mirror patch_apply) */
    /* token count (seq->n) is NO LONGER shipped: the decoder pulls content demand-driven and the
     * op loop bounds it (Feature 7, part A). */
    put_raw_bits(&rc, (uint32_t)kd, RC_KFIELD_BITS);
    if (out_en) put_raw_bits(&rc, (uint32_t)ko, RC_KFIELD_BITS);
    /* op count (ops->n) is NO LONGER shipped (Feature 7B): the decoder frontier-terminates the op
     * loop (FWD tp==to_size / grow tp==0). Zero-output ops are folded out below so every emitted op
     * advances the frontier by >=1 — which is exactly what makes the frontier loop bounded. */
    int32_t *eff_adj = (int32_t *)xmalloc((ops->n ? ops->n : 1) * sizeof(int32_t));
    uint8_t *zskip = (uint8_t *)xmalloc(ops->n ? ops->n : 1);
    fold_zero_ops(ops, eff_adj, zskip);
    /* true previous content-stream byte (order-1 tag0 context): updated on EVERY content byte --
     * span literal, backref, out-match copy. */
    EmitCursor ec = { seq, content, tags, &M, &rc, FWD, out_en, oexp, 0, 0, 0, 0, 0, 0, 0, {0} };
    for (size_t step = 0; step < ops->n; step++) {
        size_t oi = FWD ? step : ops->n - 1 - step;
        if (zskip[oi]) continue;                       /* zero-output op: folded out (adj carried) */
        Op oe = ops->v[oi]; oe.adj = eff_adj[oi];      /* emit with the fold-adjusted seek */
        emit_geom_pc(&rc, &M, &oe, &pc[step]);
        size_t base = step == 0 ? 0 : ends[step - 1], op_end = ends[step];
        for (size_t ii = 0; ii < inj[step].n; ii++) {
            emit_cursor_to(&ec, base + inj[step].v[ii].cc);
            emit_delta(&M, &rc, inj[step].v[ii].kind, inj[step].v[ii].delta);
        }
        emit_cursor_to(&ec, op_end);
    }
    free(eff_adj); free(zskip);
    if (ec.pos != content->n || ec.tok_i != seq->n || ec.tok_mode) die("content token cursor out of sync");
    return re_flush_opt(&rc);
}

typedef struct {
    const OpVec *ops;
    const uint8_t *frm;
    uint32_t from_size;
    const OpPC *pc;
    const Buf *content;
    const Buf *tags;
    const size_t *ends;
    int FWD;
} EmitBodyMeasure;

static size_t emit_body_size(const EmitBodyMeasure *m, const TokenVec *seq, int kd, int ko,
                             const InjVec *inj, const uint32_t *mb, const int32_t *mv, int mn) {
    Buf body = emit_body(seq, kd, ko, m->ops, m->FWD, m->frm, m->from_size, m->pc,
                         m->content, m->tags, m->ends, inj, mb, mv, mn);
    size_t n = g_emit_overflow ? (size_t)-1 : body.n;
    buf_free(&body);
    return n;
}

/* ---- bits-based shift-map fit (D1, C6). The hit-count fit above scores a candidate map by the
 * COUNT of residuals it drives to zero, but hits have wildly different wire VALUE (a residual
 * absorbed into a rep0 run costs ~1 bit; a fresh escape costs a whole zigzag-uLEB through the dval
 * byte tree). This fit instead prices, in fractional bits, the exact map header PLUS the BL/EX
 * residual streams under a candidate map, and eliminates segments by NET bits (segment wire cost
 * vs the residual-bit increase from dropping it). The price is a PROXY (residuals are priced in
 * collect_fields order and the shared dval tree ignores the interleaved corrections, both
 * second-order); the final map choice is always settled by encode_body's exact full-body byte
 * gate, which competes this map against the hit-count map and the no-map body. ---- */

/* one adaptive range-coder bit, priced (-log2 prob, PR_SCALE units) with the real update rule */
static uint64_t px_bit(uint16_t *prob, int bit) {
    uint64_t c = bit_price(*prob, bit);
    *prob = rc_adapt(*prob, bit, RC_S_BIT_RATE);
    return c;
}
/* MTF dict-index unary (mirror idx_encode) */
static uint64_t px_idx(A1IdxUnary *g, uint32_t v) {
    uint64_t c = 0;
    for (uint32_t pos = 0; pos < v; pos++) c += px_bit(&g->u[pos < IDX_CTX ? pos : IDX_CTX - 1], 1);
    return c + px_bit(&g->u[v < IDX_CTX ? v : IDX_CTX - 1], 0);
}
/* one byte through the adaptive dval bit-tree (mirror bt_encode, dval rate = 4) */
static uint64_t px_bt(A1BitTree *t, uint8_t byte) {
    int m = 1; uint64_t c = 0;
    for (int i = 7; i >= 0; i--) {
        int bit = (byte >> i) & 1;
        uint16_t p = a1_bt_get(t, m - 1);
        c += bit_price(p, bit);
        a1_bt_set(t, m - 1, rc_adapt(p, bit, 4));
        m = (m << 1) | bit;
    }
    return c;
}
/* zigzag-uLEB escape value through dval (mirror bv_encode) */
static uint64_t px_bv(A1BitTree *t, int64_t x) {
    uint32_t v = rc_zz32((int32_t)x);
    uint64_t c = 0;
    for (;;) {
        uint8_t b = (uint8_t)(v & 0x7fu);
        v >>= 7;
        c += px_bt(t, v ? (uint8_t)(b | 0x80u) : b);
        if (!v) break;
    }
    return c;
}
/* price one residual through the MTF/rep/hit/escape machine (mirror emit_delta); overflow past the
 * decoder dict cap prices +infinity so an infeasible map can never win. */
static uint64_t px_delta(DRE *D, A1IdxUnary *gix, A1BitTree *dval, int64_t delta, int *overflow) {
    DRTrans tr = dr_transition(D, delta);
    if (tr.kind == DR_TR_REP) return px_bit(&D->rep[tr.ri], 1);
    uint64_t c = px_bit(&D->rep[tr.ri], 0);
    if (tr.kind == DR_TR_OVER) { *overflow = 1; return c; }
    if (tr.kind == DR_TR_HIT) {
        c += px_bit(&D->hit, 1);
        c += px_idx(gix, tr.idx);
    } else {
        c += px_bit(&D->hit, 0);
        c += px_bv(dval, delta);
    }
    return c;
}

/* map header bits (raw gamma, 1 bit each): count + per entry (gap gamma + zz value gamma), scaled
 * to PR_SCALE units so it adds to the residual price. Mirrors the emit_body map-header writer. */
static uint64_t px_hdr_bits(const uint32_t *mb, const int32_t *mv, int mn) {
    uint64_t bits = gammalen_u32((uint32_t)mn + 1u);   /* raw gamma proxy for count */
    uint32_t prev = 0;
    for (int i = 0; i < mn; i++) {
        uint32_t gap = mb[i] - prev;
        bits += gammalen_u32((i ? gap - 1u : gap) + 1u);
        prev = mb[i];
        bits += gammalen_u32(rc_zz32(mv[i]) + 1u);
    }
    return bits * PR_SCALE;
}

/* total priced bits (header + BL/EX residual streams) of a candidate map over the field keys */
static uint64_t px_map_total(const uint32_t *mb, const int32_t *mv, int mn,
                             const FieldKey *fk, size_t nfr, int64_t *dic_bl, int64_t *dic_ex) {
    DRE bl, ex; dr_init_e(&bl, dic_bl, DR_KCAP_BL, DR_HIT_INIT); dr_init_e(&ex, dic_ex, DR_KCAP_EX, DR_HIT_INIT);
    A1IdxUnary di_bl, di_ex; a1_idx_init(&di_bl, RC_IDX_SEED); a1_idx_init(&di_ex, RC_IDX_SEED);
    A1BitTree dval; a1_bt_init(&dval);
    uint64_t c = px_hdr_bits(mb, mv, mn);
    int overflow = 0;
    for (size_t i = 0; i < nfr; i++) {
        int32_t pred = fk[i].kind == EV_BL
            ? (int32_t)((uint32_t)rc_smap_at(mb, mv, mn, fk[i].k1) - (uint32_t)rc_smap_at(mb, mv, mn, fk[i].k2)) / 2
            : (int32_t)(0u - (uint32_t)rc_smap_at(mb, mv, mn, fk[i].k2));
        int64_t resid = (int64_t)(int32_t)((uint32_t)fk[i].need - (uint32_t)pred);
        c += px_delta(fk[i].kind == EV_BL ? &bl : &ex, fk[i].kind == EV_BL ? &di_bl : &di_ex,
                      &dval, resid, &overflow);
        if (overflow) return UINT64_MAX / 2;
    }
    return c;
}

typedef uint64_t (*SMapScoreFn)(const uint32_t *mb, const int32_t *mv, int mn, void *ctx);

typedef struct { const FieldKey *fk; size_t nfr; } SMapHitScore;
typedef struct { const FieldKey *fk; size_t nfr; int64_t *dic_bl, *dic_ex; } SMapBitScore;

static uint64_t smap_hit_cost(const uint32_t *mb, const int32_t *mv, int mn, void *ctx) {
    SMapHitScore *s = (SMapHitScore *)ctx;
    size_t hits = 0;
    for (size_t i = 0; i < s->nfr; i++) {
        int32_t pred = s->fk[i].kind == EV_BL
            ? (int32_t)((uint32_t)rc_smap_at(mb, mv, mn, s->fk[i].k1) - (uint32_t)rc_smap_at(mb, mv, mn, s->fk[i].k2)) / 2
            : (int32_t)(0u - (uint32_t)rc_smap_at(mb, mv, mn, s->fk[i].k2));
        if (pred == s->fk[i].need) hits++;
    }
    return (uint64_t)(s->nfr - hits);
}

static uint64_t smap_bit_cost(const uint32_t *mb, const int32_t *mv, int mn, void *ctx) {
    SMapBitScore *s = (SMapBitScore *)ctx;
    return px_map_total(mb, mv, mn, s->fk, s->nfr, s->dic_bl, s->dic_ex);
}

static void smap_without(const uint32_t *tb, const int32_t *tv, int mn, int drop,
                         uint32_t *b2, int32_t *v2) {
    int w = 0;
    for (int i = 0; i < mn; i++) if (i != drop) { b2[w] = tb[i]; v2[w] = tv[i]; w++; }
}

static int smap_finish(uint32_t *tb, int32_t *tv, int mn, uint32_t *mb, int32_t *mv) {
    int w = 0;
    for (int i = 0; i < mn; i++) {
        if (w && tv[w - 1] == tv[i]) continue;
        tb[w] = tb[i]; tv[w] = tv[i]; w++;
    }
    mn = (w == 1 && tv[0] == 0) ? 0 : w;
    for (int i = 0; i < mn; i++) { mb[i] = tb[i]; mv[i] = tv[i]; }
    return mn;
}

static int fit_shift_map_scored(const OpVec *ops, uint32_t from_size, uint32_t to_size,
                                const uint8_t *frm, const FieldRef *fr, size_t nfr,
                                SMapScoreFn score, void *score_ctx,
                                uint64_t slack, int strict_improve, int sentinel_mark,
                                uint32_t *mb, int32_t *mv) {
    FieldKey *fk = (FieldKey *)xmalloc((nfr ? nfr : 1) * sizeof(*fk));
    uint32_t tb[SMAP_POOL_MAX]; int32_t tv[SMAP_POOL_MAX];
    uint32_t b2[SMAP_POOL_MAX]; int32_t v2[SMAP_POOL_MAX];
    int mark[SMAP_POOL_MAX];
    int mn = smap_build_full(ops, from_size, to_size, frm, fr, nfr, tb, tv, fk);
    if (score == smap_hit_cost) ((SMapHitScore *)score_ctx)->fk = fk;
    else ((SMapBitScore *)score_ctx)->fk = fk;
    uint64_t cur = score(tb, tv, mn, score_ctx);
    for (int round = 0; round < 8 && mn > 1; round++) {
        int removed = 0;
        for (int i = 0; i < mn; i++) {
            smap_without(tb, tv, mn, i, b2, v2);
            uint64_t t = score(b2, v2, mn - 1, score_ctx);
            uint64_t limit = cur > UINT64_MAX - slack ? UINT64_MAX : cur + slack;
            mark[i] = strict_improve ? (t < cur) : (t <= limit);
            if (mark[i]) { removed++; if (sentinel_mark) tv[i] = INT32_MIN; }
        }
        if (!removed) break;
        int w = 0;
        for (int i = 0; i < mn; i++) {
            int keep = sentinel_mark ? (tv[i] != INT32_MIN) : !mark[i];
            if (keep) { tb[w] = tb[i]; tv[w] = tv[i]; w++; }
        }
        mn = w; cur = score(tb, tv, mn, score_ctx);
    }
    while (mn > SMAP_CAP) {
        int drop = 0; uint64_t best = UINT64_MAX;
        for (int i = 0; i < mn; i++) {
            smap_without(tb, tv, mn, i, b2, v2);
            uint64_t t = score(b2, v2, mn - 1, score_ctx);
            if (t < best) { best = t; drop = i; }
        }
        memmove(tb + drop, tb + drop + 1, (size_t)(mn - drop - 1) * sizeof(*tb));
        memmove(tv + drop, tv + drop + 1, (size_t)(mn - drop - 1) * sizeof(*tv));
        mn--; cur = best;
    }
    (void)cur;
    mn = smap_finish(tb, tv, mn, mb, mv);
    free(fk);
    return mn;
}

static int fit_shift_map_hit(const OpVec *ops, uint32_t from_size, uint32_t to_size,
                             const uint8_t *frm, const FieldRef *fr, size_t nfr,
                             uint32_t *mb, int32_t *mv) {
    SMapHitScore sc = { NULL, nfr };
    return fit_shift_map_scored(ops, from_size, to_size, frm, fr, nfr,
                                smap_hit_cost, &sc, SMAP_MAX_LOSS, 0, 1, mb, mv);
}

/* Bits-based shift-map fit: same prune/cap/finish engine as the hit-count fit, but the score is
 * estimated shipped bits for the map header plus BL/EX residual streams. A segment is batch-removed
 * only when its removal strictly reduces the estimate. The final exact byte gate still chooses among
 * no map, hit-count map, and this map, so proxy over-removal cannot regress a pair. */
static int fit_shift_map_bits(const OpVec *ops, uint32_t from_size, uint32_t to_size,
                              const uint8_t *frm, const FieldRef *fr, size_t nfr,
                              uint32_t *mb, int32_t *mv) {
    int64_t *dic_bl = (int64_t *)xmalloc(DR_KCAP_BL * sizeof(int64_t));
    int64_t *dic_ex = (int64_t *)xmalloc(DR_KCAP_EX * sizeof(int64_t));
    SMapBitScore sc = { NULL, nfr, dic_bl, dic_ex };
    int mn = fit_shift_map_scored(ops, from_size, to_size, frm, fr, nfr,
                                  smap_bit_cost, &sc, 0, 1, 0, mb, mv);
    free(dic_bl); free(dic_ex);
    return mn;
}

/* Residual-space injection variant for a candidate map: re-price each plain-delta inj through the
 * map (no content re-walk — the cursors are map-independent). Per-op InjVec array; caller frees. */
static InjVec *build_inj_map(const InjVec *inj, size_t nops, const uint8_t *frm,
                             const uint32_t *mb, const int32_t *mv, int mn) {
    InjVec *out = (InjVec *)xcalloc(nops ? nops : 1, sizeof(*out));
    for (size_t step = 0; step < nops; step++) {
        const InjVec *s = &inj[step];
        for (size_t ii = 0; ii < s->n; ii++)
            inj_push(&out[step], s->v[ii].cc, s->v[ii].kind, s->v[ii].fpk,
                     field_residual(s->v[ii].kind, frm, s->v[ii].fpk, s->v[ii].delta, mb, mv, mn));
    }
    return out;
}

/* Is candidate map A identical (boundaries + values) to map B? */
static int smap_eq(const uint32_t *ab, const int32_t *av, int an,
                   const uint32_t *bb, const int32_t *bv, int bn) {
    if (an != bn) return 0;
    for (int i = 0; i < an; i++) if (ab[i] != bb[i] || av[i] != bv[i]) return 0;
    return 1;
}

Buf encode_body(const EncCtx *ctx, const OpVec *ops, const uint8_t *frm, uint32_t from_size,
                       const uint8_t *tob, uint32_t to_size,
                       const FieldDeltaVec *fd, const OpPC *pc) {
    int FWD = ctx->fwd;
    Buf content = {0}, tags = {0};
    size_t *ends = (size_t *)xmalloc((ops->n ? ops->n : 1) * sizeof(size_t));
    InjVec *inj = (InjVec *)xcalloc(ops->n ? ops->n : 1, sizeof(*inj));
    OpWalkEnt *walk = opwalk_build(ops);
    /* Fused build: one decoder-order walk per op emits content/tags AND records the field-injection
     * cursors (Inj) in the same pass -- byte layout and cursors can never disagree by construction. */
    for (size_t step = 0; step < ops->n; step++) {
        const OpWalkEnt *we = &walk[opwalk_apply_index(ops->n, FWD, step)];
        IVec ldr = op_ldr_set(frm, we->fp, we->o->diff_len, from_size);
        op_emit_content(we->o, FWD, frm, from_size, we->fp, we->tp, fd, &ldr, &content, &tags, &inj[step]);
        ends[step] = content.n;
        free(ldr.v);
    }
    uint8_t L0[256], L1[256];
    from_lit_proxy_bits(frm, from_size, L0, L1);
    uint16_t *litbits = (uint16_t *)xmalloc((content.n ? content.n : 1) * sizeof(uint16_t));
    for (size_t i = 0; i < content.n; i++) litbits[i] = tags.d[i] ? L1[content.d[i]] : L0[content.d[i]];
    int kd = 0;
    int ko = bitlen32(to_size ? to_size : 1); ko = ko > 2 ? ko - 2 : 0; if (ko > 15) ko = 15;
    Cand (*cands)[LZ_CAND_MAX] = NULL; uint8_t *ncand = NULL;
    TokenVec seq = lz_candidates_c(content.d, content.n, litbits, &kd, &cands, &ncand);
    /* ---- D1 shift map: fit TWO candidate maps from the op walk — the hit-count fit and the
     * bits-based fit (C6) — and build each one's residual-space injection variant. The LZ parse is
     * map-independent (inj values never touch content bytes), so the pipeline below runs on the
     * plain-delta inj; both maps then compete against the no-map body under the exact byte gate at
     * the end (ship-smallest => improve-or-tie per pair by construction). The bits fit is skipped
     * when it lands on the same map as the hit fit (no second emit_body needed). */
    uint32_t map_b[SMAP_CAP]; int32_t map_v[SMAP_CAP]; int map_n = 0;      /* hit-count fit */
    uint32_t map_b2[SMAP_CAP]; int32_t map_v2[SMAP_CAP]; int map_n2 = 0;   /* bits fit */
    InjVec *inj_m = NULL, *inj_m2 = NULL;
    int use_map = 0;   /* 0 = no map, 1 = hit-count map, 2 = bits map */
    { size_t nfr = 0;
      FieldRef *frs = collect_fields(ctx, ops, frm, from_size, fd, &nfr);
      if (nfr) {
          map_n = fit_shift_map_hit(ops, from_size, to_size, frm, frs, nfr, map_b, map_v);
          map_n2 = fit_shift_map_bits(ops, from_size, to_size, frm, frs, nfr, map_b2, map_v2);
          if (smap_eq(map_b2, map_v2, map_n2, map_b, map_v, map_n)) map_n2 = 0;   /* duplicate */
      }
      free(frs); }
    if (map_n > 0) inj_m = build_inj_map(inj, ops->n, frm, map_b, map_v, map_n);
    if (map_n2 > 0) inj_m2 = build_inj_map(inj, ops->n, frm, map_b2, map_v2, map_n2);
    /* ---- D2 out-matches: per-content-position output-window limit (fixed at the consuming op:
     * FWD window [0, tp0); grow window [tp_end, to_size)) + candidates over the to image. */
    uint32_t *olim = (uint32_t *)xmalloc((content.n ? content.n : 1) * sizeof(uint32_t));
    uint32_t *olim2 = (uint32_t *)xmalloc((content.n ? content.n : 1) * sizeof(uint32_t));
    uint32_t *ocap = (uint32_t *)xmalloc((content.n ? content.n : 1) * sizeof(uint32_t));
    { int32_t *tp0s = (int32_t *)xmalloc((ops->n ? ops->n : 1) * sizeof(int32_t));
      int32_t tpw = 0;
      for (size_t i = 0; i < ops->n; i++) { tp0s[i] = tpw; tpw += ops->v[i].diff_len + ops->v[i].extra_len; }
      size_t prev_end = 0;
      for (size_t step = 0; step < ops->n; step++) {
          size_t oi = FWD ? step : ops->n - 1 - step;
          uint32_t tpe = (uint32_t)(tp0s[oi] + ops->v[oi].diff_len + ops->v[oi].extra_len);
          uint32_t lim = FWD ? (uint32_t)tp0s[oi] : tpe;
          /* OLD window edge: FWD [tp_end, from_size) stays pristine through this op; grow
           * [0, min(tp0, from_size)) likewise (beyond from_size is erased/undefined flash). */
          uint32_t lim2 = FWD ? tpe
                              : ((uint32_t)tp0s[oi] < from_size ? (uint32_t)tp0s[oi] : from_size);
          for (size_t c = prev_end; c < ends[step]; c++) {
              olim[c] = lim; olim2[c] = lim2;
              ocap[c] = (uint32_t)(ends[step] - c);          /* OLD tokens end inside their op */
          }
          prev_end = ends[step];
      }
      free(tp0s); }
    OCand (*ocands)[OC_MAX] = NULL; uint8_t *nocand = NULL;
    out_candidates(content.d, content.n, olim, olim2, ocap, FWD, tob, to_size, frm, from_size, &ocands, &nocand);
    EmitBodyMeasure meas = { ops, frm, from_size, pc, &content, &tags, ends, FWD };
    /* Price-feedback: re-parse using bit-prices measured from the real adaptive models, and keep
     * the result only if the FULL body (geom/preserve/delta interleaved with the LZ tokens, after
     * the optimal range-coder flush) is strictly fewer bytes -- i.e. the exact shipped size. This
     * gates token selection on the quantity we actually pay, so an order-1-cheaper parse that the
     * range-coder interleave would round up by a byte is correctly rejected. Iterate to a fixpoint. */
    merge_adjacent_spans(&seq);   /* ship-shape: adjacent spans coded as one */
    if (content.n) {
        size_t cur_bytes = emit_body_size(&meas, &seq, kd, ko, inj, NULL, NULL, 0);
        /* Phase 1: ring-only price feedback to its fixpoint (the pre-out-match trajectory).
         * Phase 2: re-parse WITH out-candidates; each candidate must beat the phase-1 fixpoint
         * under the exact byte gate, so out-matches (and their per-fresh-match out-bit tax +
         * ko header) ship only when they win outright — no pair can end worse than ring-only. */
        uint8_t *nocand0 = (uint8_t *)xcalloc(content.n ? content.n : 1, 1);
        for (int phase = 0; phase < 2; phase++) {
            const uint8_t *noc = phase ? nocand : nocand0;
            for (int pass = 0; pass < 16; pass++) {
                PriceTab pt;
                pt.oexp0 = FWD ? 0u : to_size; pt.fwd = FWD;
                measure_prices(&seq, content.d, tags.d, frm, from_size, kd, ko, &pt);
                TokenVec cand_seq = lz_parse_priced(content.n, content.d, tags.d, cands, ncand, ocands, noc, &pt);
                int nk = fit_k_tokens(&cand_seq);
                int nko = fit_k_out(&cand_seq, ko, FWD ? 0u : to_size, FWD);
                merge_adjacent_spans(&cand_seq);
                size_t cand_bytes = emit_body_size(&meas, &cand_seq, nk, nko, inj, NULL, NULL, 0);
                if (cand_bytes < cur_bytes) {
                    size_t gain = cur_bytes - cand_bytes;
                    free(seq.v); seq = cand_seq; cur_bytes = cand_bytes; kd = nk; ko = nko;
                    /* converged: on large content later passes shave a byte or two at full DP
                     * cost; small patches (where every byte counts) always iterate to fixpoint. */
                    if (gain < (content.n >> 13)) break;
                } else {
                    free(cand_seq.v);
                    break;
                }
            }
        }
        free(nocand0);
        /* anneal-k probe: fit_k_tokens minimizes the rice codelen of the raw distances, but the
         * shipped cost is the ADAPTIVE ug_encode of M_gd seeded at k; the two optima can differ by
         * one. Walk kd outward in each direction under the EXACT full-body byte gate and stop as soon
         * as a step fails to help (the codelen-vs-k curve is unimodal). Encoder-only; wire unchanged. */
        for (int dir = -1; dir <= 1; dir += 2) {
            for (int nk = kd + dir; nk >= 0 && nk <= 15; nk += dir) {
                size_t bb = emit_body_size(&meas, &seq, nk, ko, inj, NULL, NULL, 0);
                if (bb < cur_bytes) { cur_bytes = bb; kd = nk; }
                else break;
            }
        }
        /* anneal-ko probe: same unimodal walk for the out-match position rice parameter. */
        for (int dir = -1; dir <= 1; dir += 2) {
            for (int nko = ko + dir; nko >= 0 && nko <= 15; nko += dir) {
                size_t bb = emit_body_size(&meas, &seq, kd, nko, inj, NULL, NULL, 0);
                if (bb < cur_bytes) { cur_bytes = bb; ko = nko; }
                else break;
            }
        }
        /* map variants: same fixed parse, residual-space inj + shipped map. Each ships only if its
         * exact emitted body beats the current best (no-map, then whichever map already won). */
        if (map_n > 0) {
            size_t mbytes = emit_body_size(&meas, &seq, kd, ko, inj_m, map_b, map_v, map_n);
            if (mbytes < cur_bytes) { cur_bytes = mbytes; use_map = 1; }
        }
        if (map_n2 > 0) {
            size_t mbytes = emit_body_size(&meas, &seq, kd, ko, inj_m2, map_b2, map_v2, map_n2);
            if (mbytes < cur_bytes) { cur_bytes = mbytes; use_map = 2; }
        }
    }
    free(cands); free(ncand);
    const InjVec *sel_inj = use_map == 2 ? inj_m2 : use_map == 1 ? inj_m : inj;
    const uint32_t *sel_b = use_map == 2 ? map_b2 : use_map == 1 ? map_b : NULL;
    const int32_t *sel_v = use_map == 2 ? map_v2 : use_map == 1 ? map_v : NULL;
    int sel_n = use_map == 2 ? map_n2 : use_map == 1 ? map_n : 0;
    Buf body = emit_body(&seq, kd, ko, ops, FWD, frm, from_size, pc, &content, &tags, ends,
                         sel_inj, sel_b, sel_v, sel_n);
    injvec_array_free(inj, ops->n);
    injvec_array_free(inj_m, ops->n);
    injvec_array_free(inj_m2, ops->n);
    free(olim); free(olim2); free(ocap); free(ocands); free(nocand);
    free(walk); free(ends); free(litbits); free(seq.v); buf_free(&content); buf_free(&tags);
    return body;
}
