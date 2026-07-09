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
 * residual, k2 = shift-map lookup key (BL branch target / EX word value) derived ONCE here from the
 * pristine from-image bytes. The mapped residual derives from delta+k2 via smap_resid() without
 * re-walking those bytes. */
typedef struct { uint32_t cc; int kind; uint32_t fpk; int32_t delta; uint32_t k2; } Inj;
typedef struct { Inj *v; size_t n, cap; } InjVec;

static void inj_push(InjVec *v, uint32_t cc, int kind, uint32_t fpk, int32_t delta, uint32_t k2) {
    v->v = (Inj *)vec_reserve(v->v, &v->cap, v->n + 1, sizeof(v->v[0]), 16);
    v->v[v->n++] = (Inj){cc, kind, fpk, delta, k2};
}

static void injvec_array_free(InjVec *v, size_t n) {
    if (!v) return;
    for (size_t i = 0; i < n; i++) free(v[i].v);
    free(v);
}

static size_t inj_field_count(const InjVec *inj, size_t nops) {
    size_t n = 0;
    for (size_t i = 0; i < nops; i++) n += inj[i].n;
    return n;
}

static void field_keys_from_inj(const InjVec *inj, size_t nops, int FWD, FieldKey *fk) {
    size_t n = 0;
    for (size_t oi = 0; oi < nops; oi++) {
        size_t step = FWD ? oi : nops - 1u - oi;
        const InjVec *iv = &inj[step];
        for (size_t q = 0; q < iv->n; q++) {
            size_t ii = FWD ? q : iv->n - 1u - q;
            const Inj *ij = &iv->v[ii];
            fk[n].kind = ij->kind;
            fk[n].k1 = ij->fpk;
            fk[n].need = (int32_t)(uint32_t)ij->delta;
            fk[n].k2 = ij->k2;
            n++;
        }
    }
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
                            int32_t fp0, int32_t tp0, const FieldDeltaVec *fd,
                            Buf *content, Buf *tags, InjVec *out) {
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
    FieldWalk w; fw_init(&w, FWD, frm, from_size, fd, o, fp0, o->diff_len);
    while (fw_next(&w)) {
        if (w.is_field && (w.ev.type == EV_BL || w.ev.type == EV_EX)) {   /* pure field: no literals, inject delta */
            uint32_t fpk = (uint32_t)(fp0 + w.pos), k2;
            if (w.ev.type == EV_BL) {
                uint16_t up = rc_u16le(frm + fpk), lo = rc_u16le(frm + fpk + 2);
                k2 = rc_bl_target(fpk, up, lo);
            } else {
                k2 = rc_u32le(frm + fpk);
            }
            inj_push(out, lc.cc, w.ev.type, fpk, w.ev.delta, k2);
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
static uint64_t bv_xfer(A1BitTree *t, REnc *r, int32_t x) {
    uint32_t v = rc_zz32(x);
    uint64_t c = 0;
    for (;;) {
        uint8_t b = (uint8_t)(v & 0x7fu);
        v >>= 7;
        uint8_t wb = v ? (uint8_t)(b | 0x80u) : b;
        if (r) bt_encode(t, r, wb, RC_DVAL_RATE);
        else c += bt_price_update(t, wb, RC_DVAL_RATE);
        if (!v) break;
    }
    return c;
}

static void dr_init_e(DRE *d, int32_t *dic, int cap, uint16_t hitseed) {
    d->dic = dic; d->cap = (uint16_t)cap;
    rc_dr_init(&d->s, d->dic, hitseed);
}

enum { DR_TR_REP, DR_TR_HIT, DR_TR_ESC, DR_TR_OVER };
typedef struct { uint8_t kind, ri; uint32_t idx; } DRTrans;

static DRTrans dr_transition(DRE *D, int32_t delta) {
    DRTrans tr = { DR_TR_REP, rc_dr_rep_ctx(D->s.rh, D->dic[0]), 0 };
    if (delta == D->dic[0]) { D->s.rh = 1; return tr; }
    D->s.rh = 0;
    int j = -1;
    for (int i = 0; i < D->s.K; i++) if (D->dic[i] == delta) { j = i; break; }
    if (j > 0) {
        rc_mtf_promote_i32(D->dic, (uint32_t)j);
        tr.kind = DR_TR_HIT;
        tr.idx = (uint32_t)(j - 1);
    } else {
        if (j == 0) die("unexpected delta dict index 0");
        if (D->s.K >= D->cap) { tr.kind = DR_TR_OVER; return tr; }
        rc_mtf_insert_i32(D->dic, &D->s.K, delta);
        tr.kind = DR_TR_ESC;
    }
    return tr;
}

static uint64_t delta_xfer(DRE *D, A1IdxUnary *gix, A1BitTree *dval,
                           REnc *r, int32_t delta, int *overflow) {
    DRTrans tr = dr_transition(D, delta);
    if (tr.kind == DR_TR_REP) {
        return ug_bit_xfer(r, &D->s.rep[tr.ri], 1, 1);
    }
    uint64_t c = ug_bit_xfer(r, &D->s.rep[tr.ri], 0, 1);
    if (tr.kind == DR_TR_OVER) { *overflow = 1; return c; }
    if (tr.kind == DR_TR_HIT) {
        c += ug_bit_xfer(r, &D->s.hit, 1, 1);
        c += unary_xfer(r, gix->u, IDX_CTX - 1, tr.idx, 1);
    } else {
        c += ug_bit_xfer(r, &D->s.hit, 0, 1);
        c += bv_xfer(dval, r, delta);
    }
    return c;
}

static void emit_delta(Models *M, REnc *r, int kind, int32_t delta, int *overflow) {
    if (*overflow) return;   /* stream already infeasible: state frozen, output discarded */
    DRE *D = kind == EV_BL ? &M->dr_bl : &M->dr_ex;
    A1IdxUnary *gix = kind == EV_BL ? &M->pre.dibl : &M->pre.diex;
    (void)delta_xfer(D, gix, &M->pre.dval, r, delta, overflow);
}

static void emit_geom_pc(REnc *r, Models *M, const Op *o, const OpPC *pc) {
    ugg_encode(&M->pre.gdl, r, (uint32_t)o->diff_len);
    ugg_encode(&M->pre.gel, r, (uint32_t)o->extra_len);
    ugg_encode(&M->pre.gadj, r, rc_zz32(o->adj));
    ugg_encode(&M->pre.pgn, r, (uint32_t)pc->pres.n);
    int32_t prev = 0;
    for (size_t i = 0; i < pc->pres.n; i++) { ugg_encode(i ? &M->pre.pg2 : &M->pre.pg, r, (uint32_t)(pc->pres.v[i] - prev)); prev = pc->pres.v[i]; }
    ugg_encode(&M->pre.pgn, r, (uint32_t)pc->corr.n);
    prev = 0;
    for (size_t i = 0; i < pc->corr.n; i++) {
        ugg_encode(i ? &M->pre.pg2 : &M->pre.pg, r, (uint32_t)(pc->corr.v[i].off - prev));
        prev = pc->corr.v[i].off;
        bt_encode(&M->pre.dval, r, pc->corr.v[i].byte, RC_DVAL_RATE);
    }
}

/* residual = need - pred under a candidate shift map, wrapped mod 2^32 exactly like the decoder
 * recombines them. BL pred is (shift(k1=pc) - shift(k2=target)) / 2; EX pred is -shift(k2=word).
 * mn==0 degenerates to residual == need (no-map wire); safe with mb/mv NULL (rc_smap_at never
 * dereferences at n==0). Single source for all four shift-map prediction/residual sites. */
static int32_t smap_resid(const uint32_t *mb, const int32_t *mv, int mn,
                          int kind, uint32_t k1, uint32_t k2, int32_t need) {
    int32_t pred = kind == EV_BL ? rc_smap_pred_bl(mb, mv, mn, k1, k2)
                                 : rc_smap_pred_ex(mb, mv, mn, k2);
    return (int32_t)((uint32_t)need - (uint32_t)pred);
}

/* Emit the full body (token geom/preserve/delta streams interleaved with the LZ content tokens)
 * for a given parse `seq` and rice parameter `kd`, finalize with the optimal flush, and return
 * the flushed Buf. The geom/preserve/delta streams are parse-independent, so this same routine
 * both measures a candidate parse's true shipped size and produces the final output. */
static Buf emit_body(const TokenVec *seq, int kd, int ko, const OpVec *ops, int FWD,
                     const uint8_t *frm, uint32_t from_size,
                     const OpPC *pc, const Buf *content, const Buf *tags,
                     const size_t *ends, const InjVec *inj,
                     const uint32_t *mb, const int32_t *mv, int mn,
                     int *overflow) {
    Models M;
    memset(&M, 0, sizeof(M));
    models_init_content(&M, frm, from_size, kd, ko);   /* literal trees + token-loop models */
    dr_init_e(&M.dr_bl, M.dic_bl, DR_KCAP_BL, DR_HIT_INIT);
    dr_init_e(&M.dr_ex, M.dic_ex, DR_KCAP_EX, DR_HIT_INIT);
    ugg_init_e(&M.pre.gdl); ugg_init_e(&M.pre.gadj);   /* borrowed NEUTRAL to code the map header below;
                                  * rc_init_prekd re-inits them (with seed_cont) after the map. */
    *overflow = 0;
    int out_en = 0;
    for (size_t i = 0; i < seq->n; i++) if (seq->v[i].type == 'O') { out_en = 1; break; }
    uint32_t oexp = 0;
    if (!FWD) { for (size_t i = 0; i < ops->n; i++) oexp += (uint32_t)(ops->v[i].diff_len + ops->v[i].extra_len); }
    REnc rc;
    re_init(&rc);
    /* piecewise shift map: ADAPTIVE-gamma count + per-entry gap (first absolute, later -1) + zz value,
     * coded through the BORROWED M.pre.gdl (count+gaps) / M.pre.gadj (zz values) gamma models — mirror of the
     * patch_apply decode_body map reader (bit-exact wire; s_ug_gamma == ugg_encode). */
    ugg_encode(&M.pre.gdl, &rc, (uint32_t)mn);
    { uint32_t prev = 0;
      for (int i = 0; i < mn; i++) {
          uint32_t gap = mb[i] - prev;
          ugg_encode(&M.pre.gdl, &rc, i ? gap - 1u : gap);
          prev = mb[i];
          ugg_encode(&M.pre.gadj, &rc, rc_zz32(mv[i]));
      } }
    /* now init the full apply-phase pre-kd state (re-seeds the borrowed gdl/gadj, inits dval/dibl/diex/
     * pg/pgn/pg2/gel), mirroring rc_init_prekd() in decode_body before its token loop. */
    rc_init_prekd(&M.pre);
    re_raw(&rc, out_en);   /* out-match enable bit (mirror patch_apply) */
    /* token count (seq->n) is NO LONGER shipped: the decoder pulls content demand-driven and the
     * op loop bounds it (Feature 7, part A). */
    put_raw_bits(&rc, (uint32_t)kd, RC_KFIELD_BITS);
    if (out_en) put_raw_bits(&rc, (uint32_t)ko, RC_KFIELD_BITS);
    /* true previous content-stream byte (order-1 tag0 context): updated on EVERY content byte --
     * span literal, backref, out-match copy. */
    ContentCursor ec;
    content_cursor_init(&ec, seq, content->d, tags->d, content->n, &M, &rc, FWD, out_en, oexp);
    for (size_t step = 0; step < ops->n; step++) {
        size_t oi = FWD ? step : ops->n - 1 - step;
        emit_geom_pc(&rc, &M, &ops->v[oi], &pc[step]);
        size_t base = step == 0 ? 0 : ends[step - 1], op_end = ends[step];
        for (size_t ii = 0; ii < inj[step].n; ii++) {
            const Inj *ij = &inj[step].v[ii];
            int32_t delta = mn ? smap_resid(mb, mv, mn, ij->kind, ij->fpk, ij->k2, ij->delta) : ij->delta;
            content_cursor_to(&ec, base + ij->cc, NULL);
            emit_delta(&M, &rc, ij->kind, delta, overflow);
        }
        content_cursor_to(&ec, op_end, NULL);
    }
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
    int overflow = 0;
    Buf body = emit_body(seq, kd, ko, m->ops, m->FWD, m->frm, m->from_size, m->pc,
                         m->content, m->tags, m->ends, inj, mb, mv, mn, &overflow);
    size_t n = overflow ? (size_t)-1 : body.n;
    buf_free(&body);
    return n;
}

/* ---- bits-based shift-map fit (D1, C6). The hit-count fit above scores a candidate map by the
 * COUNT of residuals it drives to zero, but hits have wildly different wire VALUE (a residual
 * absorbed into a rep0 run costs ~1 bit; a fresh escape costs a whole zigzag-uLEB through the dval
 * byte tree). This fit instead prices, in fractional bits, the exact map header PLUS the BL/EX
 * residual streams under a candidate map, and eliminates segments by NET bits (segment wire cost
 * vs the residual-bit increase from dropping it). The price is a PROXY (residuals are priced in
 * field-reference order and the shared dval tree ignores the interleaved corrections, both
 * second-order); the final map choice is always settled by encode_body's exact full-body byte
 * gate, which competes this map against the hit-count map and the no-map body. ---- */

/* price one residual through the MTF/rep/hit/escape machine (mirror emit_delta); overflow past the
 * decoder dict cap prices +infinity so an infeasible map can never win. */
static uint64_t px_delta(DRE *D, A1IdxUnary *gix, A1BitTree *dval, int32_t delta, int *overflow) {
    return delta_xfer(D, gix, dval, NULL, delta, overflow);
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
                             const FieldKey *fk, size_t nfr, int32_t *dic_bl, int32_t *dic_ex) {
    DRE bl, ex; dr_init_e(&bl, dic_bl, DR_KCAP_BL, DR_HIT_INIT); dr_init_e(&ex, dic_ex, DR_KCAP_EX, DR_HIT_INIT);
    A1IdxUnary di_bl, di_ex; a1_idx_init(&di_bl, RC_IDX_SEED); a1_idx_init(&di_ex, RC_IDX_SEED);
    A1BitTree dval; a1_bt_init(&dval);
    uint64_t c = px_hdr_bits(mb, mv, mn);
    int overflow = 0;
    for (size_t i = 0; i < nfr; i++) {
        int32_t resid = smap_resid(mb, mv, mn, fk[i].kind, fk[i].k1, fk[i].k2, fk[i].need);
        c += px_delta(fk[i].kind == EV_BL ? &bl : &ex, fk[i].kind == EV_BL ? &di_bl : &di_ex,
                      &dval, resid, &overflow);
        if (overflow) return UINT64_MAX / 2;
    }
    return c;
}

typedef uint64_t (*SMapScoreFn)(const uint32_t *mb, const int32_t *mv, int mn, void *ctx);

typedef struct { const FieldKey *fk; size_t nfr; } SMapHitScore;
typedef struct { const FieldKey *fk; size_t nfr; int32_t *dic_bl, *dic_ex; } SMapBitScore;

static uint64_t smap_hit_cost(const uint32_t *mb, const int32_t *mv, int mn, void *ctx) {
    SMapHitScore *s = (SMapHitScore *)ctx;
    size_t hits = 0;
    for (size_t i = 0; i < s->nfr; i++)
        hits += smap_resid(mb, mv, mn, s->fk[i].kind, s->fk[i].k1, s->fk[i].k2, s->fk[i].need) == 0;
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

static int fit_shift_map_scored(const uint32_t *fb, const int32_t *fv, int fn,
                                SMapScoreFn score, void *score_ctx,
                                uint64_t slack, int strict_improve, int sentinel_mark,
                                uint32_t *mb, int32_t *mv) {
    uint32_t tb[SMAP_POOL_MAX]; int32_t tv[SMAP_POOL_MAX];
    uint32_t b2[SMAP_POOL_MAX]; int32_t v2[SMAP_POOL_MAX];
    int mark[SMAP_POOL_MAX] = {0};
    memcpy(tb, fb, (size_t)fn * sizeof(*tb));
    memcpy(tv, fv, (size_t)fn * sizeof(*tv));
    int mn = fn;
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
    return mn;
}

static int fit_shift_map_hit(const uint32_t *fb, const int32_t *fv, int fn,
                             const FieldKey *fk, size_t nfr,
                             uint32_t *mb, int32_t *mv) {
    SMapHitScore sc = { fk, nfr };
    return fit_shift_map_scored(fb, fv, fn, smap_hit_cost, &sc, SMAP_MAX_LOSS, 0, 1, mb, mv);
}

/* Bits-based shift-map fit: same prune/cap/finish engine as the hit-count fit, but the score is
 * estimated shipped bits for the map header plus BL/EX residual streams. A segment is batch-removed
 * only when its removal strictly reduces the estimate. The final exact byte gate still chooses among
 * no map, hit-count map, and this map, so proxy over-removal cannot regress a pair. */
static int fit_shift_map_bits(const uint32_t *fb, const int32_t *fv, int fn,
                              const FieldKey *fk, size_t nfr,
                              uint32_t *mb, int32_t *mv) {
    int32_t *dic_bl = (int32_t *)xmalloc(DR_KCAP_BL * sizeof(int32_t));
    int32_t *dic_ex = (int32_t *)xmalloc(DR_KCAP_EX * sizeof(int32_t));
    SMapBitScore sc = { fk, nfr, dic_bl, dic_ex };
    int mn = fit_shift_map_scored(fb, fv, fn, smap_bit_cost, &sc, 0, 1, 0, mb, mv);
    free(dic_bl); free(dic_ex);
    return mn;
}

Buf encode_body(const EncCtx *ctx, const OpVec *ops, const uint8_t *frm, uint32_t from_size,
                       const uint8_t *tob, uint32_t to_size,
                       const FieldDeltaVec *fd, const OpPC *pc, int32_t fp_start,
                       int *overflow_out) {
    int FWD = ctx->fwd;
    Buf content = {0}, tags = {0};
    size_t *ends = (size_t *)xmalloc((ops->n ? ops->n : 1) * sizeof(size_t));
    InjVec *inj = (InjVec *)xcalloc(ops->n ? ops->n : 1, sizeof(*inj));
    OpWalkEnt *walk = opwalk_build(ops, fp_start);
    /* Fused build: one decoder-order walk per op emits content/tags AND records the field-injection
     * cursors (Inj) in the same pass -- byte layout and cursors can never disagree by construction. */
    const OpWalkEnt *we;
    for (size_t step = 0; step < ops->n; step++) {
        we = &walk[opwalk_apply_index(ops->n, FWD, step)];
        op_emit_content(we->o, FWD, frm, from_size, we->fp, we->tp, fd, &content, &tags, &inj[step]);
        ends[step] = content.n;
    }
    uint8_t L0[256], L1[256];
    from_lit_proxy_bits(frm, from_size, L0, L1);
    int kd = 0;
    int ko = bitlen32(to_size ? to_size : 1); ko = ko > 2 ? ko - 2 : 0; if (ko > 15) ko = 15;
    Cand (*cands)[LZ_CAND_MAX] = NULL; uint8_t *ncand = NULL;
    TokenVec seq = lz_candidates_c(content.d, tags.d, content.n, L0, L1, &kd, &cands, &ncand);
    /* ---- D1 shift map: fit TWO candidate maps from the op walk — the hit-count fit and the
     * bits-based fit (C6). The LZ parse is map-independent (inj values never touch content bytes),
     * and emit_body derives mapped residuals from the plain-delta injections at the exact emit site.
     * Both maps then compete against the no-map body under the exact byte gate at the end
     * (ship-smallest => improve-or-tie per pair by construction). */
    uint32_t map_b[2][SMAP_CAP]; int32_t map_v[2][SMAP_CAP];
    int map_n[2] = {0, 0};   /* 0 = hit-count fit, 1 = bits fit */
    int use_map = -1;
    { size_t nfr = inj_field_count(inj, ops->n);
      if (nfr) {
          FieldKey *fk = (FieldKey *)xmalloc(nfr * sizeof(*fk));
          field_keys_from_inj(inj, ops->n, FWD, fk);
          uint32_t fb[SMAP_POOL_MAX]; int32_t fv[SMAP_POOL_MAX];
          int fn = smap_build_full(ops, fp_start, from_size, to_size, fk, nfr, fb, fv);
          map_n[0] = fit_shift_map_hit(fb, fv, fn, fk, nfr, map_b[0], map_v[0]);
          map_n[1] = fit_shift_map_bits(fb, fv, fn, fk, nfr, map_b[1], map_v[1]);
          free(fk);
      } }
    /* ---- D2 out-matches: per-content-position output-window limit (fixed at the consuming op:
     * FWD window [0, tp0); grow window [tp_end, to_size)) + candidates over the to image. */
    uint32_t *olim = (uint32_t *)xmalloc((content.n ? content.n : 1) * sizeof(uint32_t));
    uint32_t *olim2 = (uint32_t *)xmalloc((content.n ? content.n : 1) * sizeof(uint32_t));
    uint32_t *ocap = (uint32_t *)xmalloc((content.n ? content.n : 1) * sizeof(uint32_t));
    { size_t prev_end = 0;
      for (size_t step = 0; step < ops->n; step++) {
          we = &walk[opwalk_apply_index(ops->n, FWD, step)];
          int32_t tp0 = we->tp;
          uint32_t tpe = (uint32_t)(tp0 + we->o->diff_len + we->o->extra_len);
          uint32_t lim = FWD ? (uint32_t)tp0 : tpe;
          /* OLD window edge: FWD [tp_end, from_size) stays pristine through this op; grow
           * [0, min(tp0, from_size)) likewise (beyond from_size is erased/undefined flash). */
          uint32_t lim2 = FWD ? tpe
                              : ((uint32_t)tp0 < from_size ? (uint32_t)tp0 : from_size);
          for (size_t c = prev_end; c < ends[step]; c++) {
              olim[c] = lim; olim2[c] = lim2;
              ocap[c] = (uint32_t)(ends[step] - c);          /* OLD tokens end inside their op */
          }
          prev_end = ends[step];
      } }
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
        /* Keep the legacy mixed-mode trajectory as the incumbent (phases 0/1), then try the
         * corrected ring-only pricing state (phase 2) before re-opening out-candidates (phase 3).
         * Every candidate still has to beat the exact emitted body. */
        for (int phase = 0; phase < 4; phase++) {
            int price_out_en = (phase == 0 || phase == 1 || phase == 3);
            const uint8_t *noc = (phase == 1 || phase == 3) ? nocand : NULL;
            for (int pass = 0; pass < 16; pass++) {
                PriceTab pt;
                pt.oexp0 = FWD ? 0u : to_size; pt.fwd = FWD; pt.out_en = price_out_en;
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
        /* anneal rice parameters: fit_k_* minimizes raw codelen, but the shipped cost is the
         * ADAPTIVE ug_encode seed. Walk each parameter outward under the exact full-body gate. */
        for (int which = 0; which < 2; which++) {
            int *kp = which ? &ko : &kd;
            for (int dir = -1; dir <= 1; dir += 2) {
                for (int nk = *kp + dir; nk >= 0 && nk <= 15; nk += dir) {
                    size_t bb = emit_body_size(&meas, &seq, which ? kd : nk, which ? nk : ko,
                                               inj, NULL, NULL, 0);
                    if (bb < cur_bytes) { cur_bytes = bb; *kp = nk; }
                    else break;
                }
            }
        }
        /* map variants: same fixed parse, residual-space inj + shipped map. Each ships only if its
         * exact emitted body beats the current best (no-map, then whichever map already won). */
        for (int mi = 0; mi < 2; mi++) {
            int mn = map_n[mi];
            if (mn > 0) {
                const uint32_t *mb = map_b[mi];
                const int32_t *mv = map_v[mi];
                size_t mbytes = emit_body_size(&meas, &seq, kd, ko, inj, mb, mv, mn);
                if (mbytes < cur_bytes) { cur_bytes = mbytes; use_map = mi; }
            }
        }
    }
    free(cands); free(ncand);
    const uint32_t *sel_b = use_map >= 0 ? map_b[use_map] : NULL;
    const int32_t *sel_v = use_map >= 0 ? map_v[use_map] : NULL;
    int sel_n = use_map >= 0 ? map_n[use_map] : 0;
    Buf body = emit_body(&seq, kd, ko, ops, FWD, frm, from_size, pc, &content, &tags, ends,
                         inj, sel_b, sel_v, sel_n, overflow_out);
    injvec_array_free(inj, ops->n);
    free(olim); free(olim2); free(ocap); free(ocands); free(nocand);
    free(walk); free(ends); free(seq.v); buf_free(&content); buf_free(&tags);
    return body;
}
