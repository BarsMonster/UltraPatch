/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 *
 * Host encoder module -- shift-map fitting, body assembly, and range-coder emission:
 * op_emit_content, emit_delta, emit_geom, emit_body, encode_body.
 * Compiled as a normal internal encoder translation unit.
 */

#include "enc_internal.h"
/* One field injection (delta escaped into the LZ content stream): cc = absolute content byte cursor,
 * kind = EV_BL/EV_EX, k1 = from-image field address, need = plain (no-map) wire
 * residual, k2 = shift-map lookup key (BL branch target / EX word value) derived ONCE here from the
 * pristine from-image bytes. The mapped residual derives from need+k2 via smap_resid() without
 * re-walking those bytes. */
static void inj_push(FieldInjArena *v, uint32_t cc, int kind, uint32_t fpk,
                     int32_t delta, uint32_t k2) {
    v->v = (FieldInj *)vec_reserve(v->v, &v->cap, v->n + 1, sizeof(v->v[0]), 16);
    v->v[v->n++] = (FieldInj){cc, kind, fpk, delta, k2};
}

typedef struct {
    const uint8_t *payload;
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
    buf_put(lc->content, lc->payload[k]);
    buf_put(lc->tags, 0);
    lc->cprev = k;
}

/* Single per-op decoder-order walk: finalize payload bytes, collect fields, and emit this op's
 * content/tags (uLEB nlit + per-literal uLEB-gap+byte + extras). A field's temporary `cc` holds its
 * window position until the literal merge replaces it with the exact read-ahead content cursor.
 * Routes through the shared FieldWalk (fw_next), the one mirror of the decoder apply_op window
 * skeleton. FWD/reverse asymmetry is preserved exactly:
 * reverse writes the extras (byte-reversed) before the dl body and gaps step back from dl; FWD writes
 * extras after and gaps measure from 0. tp0 = this op's to-image start (extras tag parity).
 * cc is a READ-AHEAD cursor: like the decoder up_LitCur, the next literal's gap+byte is consumed before
 * the field window that follows it, so cc leads content->n by the pending literal's cost. */
static void op_emit_content(const Op *o, uint8_t *payload, int FWD,
                            const uint8_t *frm, uint32_t from_size,
                            const uint8_t *tob, uint32_t to_size,
                            int32_t fp0, int32_t tp0, const LdrTargetIndex *ldr,
                            Buf *content, Buf *tags, FieldInjArena *out) {
    size_t inj0 = out->n;
    FieldWalk w;
    fw_init(&w, FWD, frm, from_size, tob, to_size, ldr, payload,
            fp0, tp0, o->diff_len);
    while (fw_next(&w)) {
        uint32_t fpk = (uint32_t)(fp0 + w.pos);
        int keep = w.is_field && (w.ev.type == EV_BL || w.ev.type == EV_EX);
        if (keep) {
            uint8_t packed[4];
            if (w.ev.type == EV_BL) {
                uint16_t up = rc_u16le(frm + fpk), lo = rc_u16le(frm + fpk + 2);
                rc_bl_dereloc(up, lo, (uint32_t)w.ev.delta, packed);
            } else {
                rc_u32le_put(packed, rc_u32le(frm + fpk) - (uint32_t)w.ev.delta);
            }
            keep = !memcmp(packed, tob + tp0 + w.pos, sizeof(packed));
        }
        if (keep) {
            uint32_t k2;
            if (w.ev.type == EV_BL) {
                uint16_t up = rc_u16le(frm + fpk), lo = rc_u16le(frm + fpk + 2);
                k2 = rc_bl_target(fpk, up, lo);
            } else {
                k2 = rc_u32le(frm + fpk);
            }
            inj_push(out, (uint32_t)w.pos, w.ev.type, fpk, w.ev.delta, k2);
        } else if (w.is_field) {
            for (int b = 0; b < 4; b++)
                payload[w.pos + b] = (uint8_t)(tob[tp0 + w.pos + b] - frm[fpk + (uint32_t)b]);
        } else {
            int32_t fp = fp0 + w.pos;
            uint8_t src = 0;
            if (fp >= 0 && (uint32_t)fp < from_size) src = frm[fp];
            payload[w.pos] = (uint8_t)(tob[tp0 + w.pos] - src);
        }
    }
    if (o->extra_len)
        memcpy(payload + o->diff_len, tob + tp0 + o->diff_len, (size_t)o->extra_len);

    IVec lits = {0};
    for (int32_t k = 0; k < o->diff_len; k++) if (payload[k]) ivec_push(&lits, k);
    Buf tmp = {0};
    put_uleb(&tmp, (uint32_t)lits.n);
    buf_write(content, tmp.d, tmp.n);
    for (size_t i = 0; i < tmp.n; i++) buf_put(tags, 0);
    int32_t exstart = tp0 + o->diff_len;
    if (!FWD)   /* reverse: extras precede the dl body, byte-reversed in the content stream */
        for (int32_t e = o->extra_len - 1; e >= 0; e--) { buf_put(content, payload[o->diff_len + e]); buf_put(tags, (uint8_t)((exstart + e) & 1)); }
    EncLitCursor lc = { payload, &lits, &tmp, content, tags, (uint32_t)content->n,
        FWD, FWD ? 0 : o->diff_len, FWD ? 0 : o->diff_len, -1, 0 };
    enc_litcur_step(&lc);   /* prime: read-ahead the first literal (mirror litcur_init) */
    for (size_t i = inj0; i < out->n; i++) {
        int32_t p = (int32_t)out->v[i].cc;
        while (lc.nextpos >= 0 && (FWD ? lc.nextpos < p : lc.nextpos > p + 3)) {
            enc_litcur_emit(&lc, lc.nextpos);
            enc_litcur_step(&lc);
        }
        out->v[i].cc = lc.cc;
    }
    while (lc.nextpos >= 0) {
        enc_litcur_emit(&lc, lc.nextpos);
        enc_litcur_step(&lc);
    }
    if (FWD)
        for (int32_t e = 0; e < o->extra_len; e++) { buf_put(content, payload[o->diff_len + e]); buf_put(tags, (uint8_t)((exstart + e) & 1)); }
    free(lits.v); buf_free(&tmp);
}

/* MTF escape value: zigzag uLEB, each byte through the adaptive dval bit-tree (mirror s_bv). */
static uint64_t bv_xfer(up_BitTree *t, REnc *r, int32_t x) {
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

enum { DR_TR_REP, DR_TR_HIT, DR_TR_ESC };
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
        rc_mtf_insert_i32(D->dic, &D->s.K, D->cap, delta);
        tr.kind = DR_TR_ESC;
    }
    return tr;
}

static uint64_t delta_xfer(DRE *D, up_IdxUnary *gix, up_BitTree *dval,
                           REnc *r, int32_t delta) {
    DRTrans tr = dr_transition(D, delta);
    if (tr.kind == DR_TR_REP) {
        return ug_bit_xfer(r, &D->s.rep[tr.ri], 1, 1);
    }
    uint64_t c = ug_bit_xfer(r, &D->s.rep[tr.ri], 0, 1);
    if (tr.kind == DR_TR_HIT) {
        c += ug_bit_xfer(r, &D->s.hit, 1, 1);
        c += unary_xfer(r, gix->u, UP_IDX_CTX - 1, tr.idx, 1);
    } else {
        c += ug_bit_xfer(r, &D->s.hit, 0, 1);
        c += bv_xfer(dval, r, delta);
    }
    return c;
}

static void emit_delta(Models *M, REnc *r, int kind, int32_t delta) {
    DRE *D = kind == EV_BL ? &M->dr_bl : &M->dr_ex;
    up_IdxUnary *gix = kind == EV_BL ? &M->pre.dibl : &M->pre.diex;
    (void)delta_xfer(D, gix, &M->pre.dval, r, delta);
}

static void emit_geom(REnc *r, Models *M, const Op *o) {
    ugg_encode(&M->pre.gdl, r, (uint32_t)o->diff_len);
    ugg_encode(&M->pre.gel, r, (uint32_t)o->extra_len);
    ugg_encode(&M->pre.gadj, r, rc_zz32(o->adj));
}

/* residual = need - pred under a candidate shift map, wrapped mod 2^32 exactly like the decoder
 * recombines them. BL pred is (shift(k1=pc) - shift(k2=target)) / 2; EX pred is -shift(k2=word).
 * mn==0 degenerates to residual == need (no-map wire); safe with mb/mv NULL (rc_smap_at never
 * dereferences at n==0). Single source for all four shift-map prediction/residual sites. */
static int32_t smap_resid(const uint32_t *mb, const int32_t *mv, int mn,
                          int kind, uint32_t k1, uint32_t k2, int32_t need) {
    int32_t pred = kind == EV_BL ? rc_smap_pred_bl(mb, mv, mn, k1, k2)
                                 : rc_smap_pred_ex(mb, mv, mn, k2);
    return rc_i32_from_u32((uint32_t)need - (uint32_t)pred);
}

/* The map header uses Gamma(value), whose stored positive integer is value+1. Validate its field
 * representation and ordering before either proxy pricing or emission so an internal candidate
 * cannot wrap a first boundary/value to zero or reconstruct a nonascending map. A later
 * UINT32_MAX boundary is valid: its wire value is the positive gap minus one. */
static int smap_wire_feasible(const uint32_t *mb, const int32_t *mv, int mn) {
    /* The bits fitter intentionally prices its pre-cap working set, so enforce the decoder's
     * UP_SMAP_CAP only at exact emission after fitting has reduced the candidate. */
    if (mn < 0) return 0;
    uint32_t prev = 0;
    for (int i = 0; i < mn; i++) {
        uint32_t wire_gap;
        if (i == 0) {
            wire_gap = mb[i];
        } else {
            if (mb[i] <= prev) return 0;
            wire_gap = mb[i] - prev - 1u;
        }
        if (wire_gap == UINT32_MAX || rc_zz32(mv[i]) == UINT32_MAX) return 0;
        prev = mb[i];
    }
    return 1;
}

/* Emit the full body (token geometry/delta streams interleaved with the LZ content tokens)
 * for a given parse `seq` and rice parameter `kd`, finalize with the optimal flush, and return
 * the flushed Buf. The geometry/delta streams are parse-independent, so this same routine
 * both measures a candidate parse's true shipped size and produces the final output. */
static Buf emit_body(const TokenVec *seq, int kd, int ko, const OpVec *ops, int FWD,
                     const LitSeedTrees *seeds, const Buf *content, const Buf *tags,
                     const OpEmitRow *rows, const FieldInjArena *inj,
                     const uint32_t *mb, const int32_t *mv, int mn,
                     int *overflow) {
    Models M;
    memset(&M, 0, sizeof(M));
    models_init_content(&M, seeds, kd, ko);   /* fresh literal trees + token-loop models */
    dr_init_e(&M.dr_bl, M.dic_bl, DR_KCAP_BL, UP_DR_HIT_INIT);
    dr_init_e(&M.dr_ex, M.dic_ex, DR_KCAP_EX, UP_DR_HIT_INIT);
    rc_init_prekd(&M.pre);   /* map header and operation geometry share gdl/gadj adaptation */
    *overflow = 0;
    int out_en = 0;
    for (size_t i = 0; i < seq->n; i++) if (seq->v[i].type == 'O') { out_en = 1; break; }
    uint32_t oexp = 0;
    if (!FWD) { for (size_t i = 0; i < ops->n; i++) oexp += (uint32_t)(ops->v[i].diff_len + ops->v[i].extra_len); }
    REnc rc;
    re_init(&rc);
    /* piecewise shift map: ADAPTIVE-gamma count + per-entry gap (first absolute, later -1) + zz value,
     * coded through M.pre.gdl (count+gaps) / M.pre.gadj (zz values) — mirror of the patch_apply
     * decode_body map reader. Their adapted state continues into operation geometry. */
    if (mn > UP_SMAP_CAP || !smap_wire_feasible(mb, mv, mn)) {
        rc.coding_overflow = 1;
    } else {
        ugg_encode(&M.pre.gdl, &rc, (uint32_t)mn);
        { uint32_t prev = 0;
          for (int i = 0; i < mn; i++) {
              uint32_t gap = mb[i] - prev;
              ugg_encode(&M.pre.gdl, &rc, i ? gap - 1u : gap);
              prev = mb[i];
              ugg_encode(&M.pre.gadj, &rc, rc_zz32(mv[i]));
          } }
    }
    re_raw(&rc, out_en);   /* out-match enable bit (mirror patch_apply) */
    /* token count (seq->n) is NO LONGER shipped: the decoder pulls content demand-driven and the
     * op loop bounds it. */
    put_raw_bits(&rc, (uint32_t)kd, RC_KFIELD_BITS);
    if (out_en) put_raw_bits(&rc, (uint32_t)ko, RC_KFIELD_BITS);
    /* true previous content-stream byte (order-1 tag0 context): updated on EVERY content byte --
     * span literal, backref, out-match copy. */
    ContentCursor ec;
    content_cursor_init(&ec, seq, content->d, tags->d, content->n, &M, &rc, FWD, out_en, oexp);
    for (size_t step = 0; step < ops->n; step++) {
        size_t oi = FWD ? step : ops->n - 1 - step;
        emit_geom(&rc, &M, &ops->v[oi]);
        size_t ib = step ? rows[step - 1u].inj_end : 0u;
        for (size_t ii = ib; ii < rows[step].inj_end; ii++) {
            const FieldInj *ij = &inj->v[ii];
            int32_t delta = mn ? smap_resid(mb, mv, mn, ij->kind, ij->k1, ij->k2, ij->need) : ij->need;
            content_cursor_to(&ec, ij->cc, NULL);
            emit_delta(&M, &rc, ij->kind, delta);
        }
        content_cursor_to(&ec, rows[step].content_end, NULL);
    }
    if (ec.pos != content->n || ec.tok_i != seq->n || ec.tok_mode) die("content token cursor out of sync");
    if (rc.coding_overflow) *overflow = 1;
    return re_flush_opt(&rc);
}

typedef struct {
    const OpVec *ops;
    const LitSeedTrees *seeds;
    const Buf *content;
    const Buf *tags;
    const OpEmitRow *rows;
    int FWD;
} EmitBodyMeasure;

static size_t emit_body_size(const EmitBodyMeasure *m, const TokenVec *seq, int kd, int ko,
                             const FieldInjArena *inj, const uint32_t *mb, const int32_t *mv, int mn) {
    int overflow = 0;
    Buf body = emit_body(seq, kd, ko, m->ops, m->FWD, m->seeds, m->content,
                         m->tags, m->rows, inj, mb, mv, mn, &overflow);
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
 * field-reference order rather than full emission order, a second-order effect); the final map
 * choice is always settled by encode_body's exact full-body byte
 * gate, which competes this map against the hit-count map and the no-map body. ---- */

/* Price one residual through the bounded MTF/rep/hit/escape machine (mirror emit_delta). */
static uint64_t px_delta(DRE *D, up_IdxUnary *gix, up_BitTree *dval, int32_t delta) {
    return delta_xfer(D, gix, dval, NULL, delta);
}

/* map header bits (raw gamma, 1 bit each): count + per entry (gap gamma + zz value gamma), scaled
 * to PR_SCALE units so it adds to the residual price. Mirrors the emit_body map-header writer. */
static uint64_t px_hdr_bits(const uint32_t *mb, const int32_t *mv, int mn) {
    if (!smap_wire_feasible(mb, mv, mn)) return UINT64_MAX / 2u;
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
                             const FieldInjArena *inj, int fwd,
                             int32_t *dic_bl, int32_t *dic_ex) {
    uint64_t c = px_hdr_bits(mb, mv, mn);
    if (c == UINT64_MAX / 2u) return c;
    DRE bl, ex; dr_init_e(&bl, dic_bl, DR_KCAP_BL, UP_DR_HIT_INIT); dr_init_e(&ex, dic_ex, DR_KCAP_EX, UP_DR_HIT_INIT);
    up_IdxUnary di_bl, di_ex; up_idx_init(&di_bl, RC_IDX_SEED); up_idx_init(&di_ex, RC_IDX_SEED);
    up_BitTree dval; up_bt_init(&dval);
    for (size_t i = 0; i < inj->n; i++) {
        const FieldInj *fk = field_inj_key(inj, fwd, i);
        int32_t resid = smap_resid(mb, mv, mn, fk->kind, fk->k1, fk->k2, fk->need);
        c += px_delta(fk->kind == EV_BL ? &bl : &ex, fk->kind == EV_BL ? &di_bl : &di_ex,
                      &dval, resid);
    }
    return c;
}

typedef uint64_t (*SMapScoreFn)(const uint32_t *mb, const int32_t *mv, int mn, void *ctx);

typedef struct { const FieldInjArena *inj; int fwd; } SMapHitScore;
typedef struct { const FieldInjArena *inj; int fwd; int32_t *dic_bl, *dic_ex; } SMapBitScore;

static uint64_t smap_hit_cost(const uint32_t *mb, const int32_t *mv, int mn, void *ctx) {
    SMapHitScore *s = (SMapHitScore *)ctx;
    size_t hits = 0;
    for (size_t i = 0; i < s->inj->n; i++) {
        const FieldInj *fk = field_inj_key(s->inj, s->fwd, i);
        hits += smap_resid(mb, mv, mn, fk->kind, fk->k1, fk->k2, fk->need) == 0;
    }
    return (uint64_t)(s->inj->n - hits);
}

static uint64_t smap_bit_cost(const uint32_t *mb, const int32_t *mv, int mn, void *ctx) {
    SMapBitScore *s = (SMapBitScore *)ctx;
    return px_map_total(mb, mv, mn, s->inj, s->fwd, s->dic_bl, s->dic_ex);
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
    while (mn > UP_SMAP_CAP) {
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
                             const FieldInjArena *inj, int fwd,
                             uint32_t *mb, int32_t *mv) {
    SMapHitScore sc = { inj, fwd };
    return fit_shift_map_scored(fb, fv, fn, smap_hit_cost, &sc, SMAP_MAX_LOSS, 0, 1, mb, mv);
}

/* Bits-based shift-map fit: same prune/cap/finish engine as the hit-count fit, but the score is
 * estimated shipped bits for the map header plus BL/EX residual streams. A segment is batch-removed
 * only when its removal strictly reduces the estimate. The final exact byte gate still chooses among
 * no map, hit-count map, and this map, so proxy over-removal cannot regress a pair. */
static int fit_shift_map_bits(const uint32_t *fb, const int32_t *fv, int fn,
                              const FieldInjArena *inj, int fwd,
                              uint32_t *mb, int32_t *mv) {
    int32_t *dic_bl = (int32_t *)xmalloc(DR_KCAP_BL * sizeof(int32_t));
    int32_t *dic_ex = (int32_t *)xmalloc(DR_KCAP_EX * sizeof(int32_t));
    SMapBitScore sc = { inj, fwd, dic_bl, dic_ex };
    int mn = fit_shift_map_scored(fb, fv, fn, smap_bit_cost, &sc, 0, 1, 0, mb, mv);
    free(dic_bl); free(dic_ex);
    return mn;
}

Buf encode_body(const EncCtx *ctx, const OpVec *ops, const uint8_t *frm, uint32_t from_size,
                const uint8_t *tob, uint32_t to_size,
                const LdrTargetIndex *ldr, int32_t fp_start, int *overflow_out) {
    int FWD = ctx->fwd;
    Buf content = {0}, tags = {0};
    OpEmitRow *rows = (OpEmitRow *)xmalloc((ops->n ? ops->n : 1) * sizeof(*rows));
    FieldInjArena inj = {0};
    OpWalkEnt *walk = opwalk_build(ops, fp_start);
    /* Fused build: one decoder-order walk per op emits content/tags AND records the field-injection
     * cursors in the same pass -- byte layout and cursors can never disagree by construction. */
    const OpWalkEnt *we;
    for (size_t step = 0; step < ops->n; step++) {
        we = &walk[opwalk_apply_index(ops->n, FWD, step)];
        op_emit_content(we->o, ops->payload + we->tp, FWD, frm, from_size,
                        tob, to_size, we->fp, we->tp, ldr, &content, &tags, &inj);
        rows[step] = (OpEmitRow){content.n, inj.n};
    }
    uint8_t L0[256], L1[256];
    LitSeedTrees seeds;
    lit_seed_trees_init(&seeds, frm, from_size);
    from_lit_proxy_bits(&seeds, L0, L1);
    int kd = 0;
    int ko = bitlen32(to_size ? to_size : 1); ko = ko > 2 ? ko - 2 : 0; if (ko > 15) ko = 15;
    CandArena cands = {0}; uint8_t *ncand = NULL;
    TokenVec seq = lz_candidates_c(content.d, tags.d, content.n, L0, L1, &kd, &cands, &ncand);
    /* ---- D1 shift map: fit TWO candidate maps from the op walk — the hit-count fit and the
     * bits-based fit (C6). The LZ parse is map-independent (inj values never touch content bytes),
     * and emit_body derives mapped residuals from the plain-delta injections at the exact emit site.
     * Both maps then compete against the no-map body under the exact byte gate at the end
     * (ship-smallest => improve-or-tie per pair by construction). */
    uint32_t map_b[2][UP_SMAP_CAP]; int32_t map_v[2][UP_SMAP_CAP];
    int map_n[2] = {0, 0};   /* 0 = hit-count fit, 1 = bits fit */
    int use_map = -1;
    if (inj.n) {
        uint32_t fb[SMAP_POOL_MAX]; int32_t fv[SMAP_POOL_MAX];
        int fn = smap_build_full(ops, fp_start, from_size, to_size, &inj, FWD, fb, fv);
        map_n[0] = fit_shift_map_hit(fb, fv, fn, &inj, FWD, map_b[0], map_v[0]);
        map_n[1] = fit_shift_map_bits(fb, fv, fn, &inj, FWD, map_b[1], map_v[1]);
    }
    /* ---- D2 out-matches: each content position inherits the decode-time NEW/OLD flash
     * windows and OLD-token cap from the op that consumes it. */
    OCand *ocands = out_candidates(content.d, content.n, ops, walk, rows, FWD,
                                   tob, to_size, frm, from_size);
    int32_t max_out_len = 0;
    for (size_t i = 0; i < content.n; i++)
        if (ocands[i].len > max_out_len) max_out_len = ocands[i].len;
    EmitBodyMeasure meas = { ops, &seeds, &content, &tags, rows, FWD };
    /* Price-feedback: re-parse using bit-prices measured from the real adaptive models, and keep
     * the result only if the FULL body (geometry/delta interleaved with the LZ tokens, after
     * the optimal range-coder flush) is strictly fewer bytes -- i.e. the exact shipped size. This
     * gates token selection on the quantity we actually pay, so an order-1-cheaper parse that the
     * range-coder interleave would round up by a byte is correctly rejected. Iterate to a fixpoint. */
    merge_adjacent_spans(&seq);   /* ship-shape: adjacent spans coded as one */
    if (content.n) {
        size_t cur_bytes = emit_body_size(&meas, &seq, kd, ko, &inj, NULL, NULL, 0);
        /* Keep the legacy mixed-mode trajectory: establish prices without out-candidates, then
         * open them for two convergence passes. Every candidate still has to beat the exact body. */
        for (int phase = 0; phase < 3; phase++) {
            const OCand *phase_ocands = phase ? ocands : NULL;
            for (int pass = 0; pass < 16; pass++) {
                PriceTab pt;
                pt.oexp0 = FWD ? 0u : to_size; pt.fwd = FWD; pt.out_en = 1;
                measure_prices(&seq, content.d, tags.d, &seeds, kd, ko, &pt);
                TokenVec cand_seq = lz_parse_priced(content.n, content.d, tags.d,
                                                    &cands, ncand, phase_ocands,
                                                    phase ? max_out_len : 0, &pt);
                int nk = fit_k_tokens(&cand_seq);
                int nko = fit_k_out(&cand_seq, ko, FWD ? 0u : to_size, FWD);
                merge_adjacent_spans(&cand_seq);
                size_t cand_bytes = emit_body_size(&meas, &cand_seq, nk, nko, &inj, NULL, NULL, 0);
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
                                               &inj, NULL, NULL, 0);
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
                size_t mbytes = emit_body_size(&meas, &seq, kd, ko, &inj, mb, mv, mn);
                if (mbytes < cur_bytes) { cur_bytes = mbytes; use_map = mi; }
            }
        }
    }
    buf_free(&cands); free(ncand);
    const uint32_t *sel_b = use_map >= 0 ? map_b[use_map] : NULL;
    const int32_t *sel_v = use_map >= 0 ? map_v[use_map] : NULL;
    int sel_n = use_map >= 0 ? map_n[use_map] : 0;
    Buf body = emit_body(&seq, kd, ko, ops, FWD, &seeds, &content, &tags, rows,
                         &inj, sel_b, sel_v, sel_n, overflow_out);
    free(inj.v);
    free(ocands);
    free(walk); free(rows); free(seq.v); buf_free(&content); buf_free(&tags);
    return body;
}
