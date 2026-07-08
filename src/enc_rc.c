/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 *
 * A1 host encoder module -- binary range encoder (REnc) + entropy models (bt/ug/idx/fl, raw bits, gamma).
 * Compiled as a normal internal encoder translation unit.
 */

#include "enc_internal.h"
/* ------------------------------------------------------------------------------------- */
/* Binary range encoder and models.                                                        */
/* ------------------------------------------------------------------------------------- */
/* byte trees use the shared packed A1BitTree + a1_bt_get/a1_bt_set/a1_bt_init from rc_models.h (decoder-identical) */

static void re_shift_low(REnc *r) {
    if (r->low < 0xff000000ull || r->low > 0xffffffffull) {
        uint8_t c = r->cache;
        for (;;) {
            buf_put(&r->out, (uint8_t)(c + (uint8_t)(r->low >> 32)));
            c = 0xff;
            r->csz--;
            if (r->csz == 0) break;
        }
        r->cache = (uint8_t)(r->low >> 24);
    }
    r->csz++;
    r->low = (r->low << 8) & 0xffffffffull;
}

void re_init(REnc *r) { memset(r, 0, sizeof(*r)); r->range = 0xffffffffu; r->csz = 1; }

/* RC_S_BIT_RATE (Golomb / order-2 flag / MTF rep+hit adaptation rate) and RC_REP0_INIT (rep0 prior)
 * are single-sourced in rc_models.h, shared verbatim with the decoder. */

void re_bit(REnc *r, uint16_t *prob, int bit, int rate) {
    uint32_t p = *prob, bound = RC_PROB_BOUND(r->range, p);
    if (bit == 0) r->range = bound;
    else { r->low += bound; r->range -= bound; }
    *prob = rc_adapt(p, bit, rate);   /* shared update rule (rc_models.h), mirror of decoder s_bit_r */
    while (r->range < RC_KTOP) { r->range <<= 8; re_shift_low(r); }
}

void re_raw(REnc *r, int bit) {
    uint32_t bound = r->range >> 1;
    if (bit == 0) r->range = bound;
    else { r->low += bound; r->range -= bound; }
    while (r->range < RC_KTOP) { r->range <<= 8; re_shift_low(r); }
}

Buf re_flush_opt(REnc *r) {
    int t = bitlen32(r->range) - 1;
    uint64_t mask = (1ull << t) - 1ull;
    if (r->low & mask) r->low = (r->low + (1ull << t)) & ~mask;
    size_t base = r->out.n;
    for (int i = 0; i < 5; i++) re_shift_low(r);
    while (r->out.n > base && r->out.d[r->out.n - 1] == 0) r->out.n--;
    Buf b = r->out;
    r->out = (Buf){0};
    return b;
}

void put_raw_bits(REnc *r, uint32_t v, int nb) {
    for (int sh = nb - 1; sh >= 0; sh--) re_raw(r, (int)((v >> sh) & 1u));
}

enum { BT_XFER_ENC, BT_XFER_PRICE, BT_XFER_PRICE_UPDATE };

static uint64_t bt_xfer(A1BitTree *t, REnc *r, uint8_t byte, int rate, int mode) {
    int m = 1;
    uint64_t cost = 0;
    for (int i = 7; i >= 0; i--) {
        int bit = (byte >> i) & 1;
        uint16_t p = a1_bt_get(t, m - 1);
        if (mode == BT_XFER_ENC) {
            re_bit(r, &p, bit, rate);
            a1_bt_set(t, m - 1, p);
        } else {
            cost += bit_price(p, bit);
            if (mode == BT_XFER_PRICE_UPDATE) a1_bt_set(t, m - 1, rc_adapt(p, bit, rate));
        }
        m = (m << 1) | bit;
    }
    return cost;
}

void bt_encode(A1BitTree *t, REnc *r, uint8_t byte, int rate) {
    (void)bt_xfer(t, r, byte, rate, BT_XFER_ENC);
}

void lit_tree_seed_e(const uint8_t *frm, size_t n, int parity, A1BitTree *t) {
    uint32_t hist[256], w[512];
    for (int i = 0; i < 256; i++) hist[i] = 1;
    for (size_t i = 0; i < n; i++) if ((int)(i & 1) == parity) hist[frm[i]]++;
    memset(t->p, 0, sizeof t->p);
    rc_lit_tree_from_hist(t, hist, w);   /* mirror of decoder lit_tree_from_hist */
}

/* Neutral-init entry points: thin out-of-line wrappers over the shared rc_ugr_init/rc_ugg_init
 * (rc_models.h) so the host keeps ONE copy of each init loop instead of inlining it at every model. */
void ugr_init_e(A1UGRice *g, int k) { rc_ugr_init(g, k); }
void ugg_init_e(A1UGGamma *g) { rc_ugg_init(g); }

static uint32_t unary_xfer(REnc *r, uint16_t *u, uint32_t clampmax, uint32_t v) {
    uint32_t cost = 0;
    for (uint32_t pos = 0; pos < v; pos++) {
        uint16_t *p = &u[pos < clampmax ? pos : clampmax];
        if (r) re_bit(r, p, 1, RC_S_BIT_RATE);
        else cost += bit_price(*p, 1);
    }
    uint16_t *p = &u[v < clampmax ? v : clampmax];
    if (r) re_bit(r, p, 0, RC_S_BIT_RATE);
    else cost += bit_price(*p, 0);
    return cost;
}

static uint32_t ug_bit_xfer(REnc *r, uint16_t *prob, int bit) {
    if (r) {
        re_bit(r, prob, bit, RC_S_BIT_RATE);
        return 0;
    }
    return bit_price(*prob, bit);
}

/* Rice/Gamma transfer cores: statically typed, sharing the unary-prefix + bit transfer helpers. */
static uint32_t ugr_xfer(A1UGRice *g, REnc *r, uint32_t v) {
    uint32_t cl = v >> g->k;
    uint32_t cost = unary_xfer(r, g->u, UG_CTX, cl);
    for (int pos = 0; pos < g->k; pos++)
        cost += ug_bit_xfer(r, &g->m[UG_C((int)cl)][UG_C(g->k - 1 - pos)],
                            (int)((v >> (g->k - 1 - pos)) & 1u));  /* rice: LSB-anchored ctx */
    return cost;
}
static uint32_t ugg_xfer(A1UGGamma *g, REnc *r, uint32_t v) {
    uint32_t mm = v + 1u;
    uint32_t cl = (uint32_t)bitlen32(mm) - 1u;
    int row = UG_C((int)cl);
    uint32_t cost = unary_xfer(r, g->u, UG_CTX, cl);
    for (uint32_t pos = 0; pos < cl; pos++)
        cost += ug_bit_xfer(r, &g->m[rc_ugg_mant_idx(row, (int)pos)],
                            (int)((mm >> (cl - 1u - pos)) & 1u));
    return cost;
}

void ugr_encode(A1UGRice *g, REnc *r, uint32_t v) { (void)ugr_xfer(g, r, v); }
void ugg_encode(A1UGGamma *g, REnc *r, uint32_t v) { (void)ugg_xfer(g, r, v); }

/* order-2 token flag: the A1Flag1 struct + a1_fl_init are single-sourced in rc_models.h (decoder mirror). */
void fl_encode(A1Flag1 *f, REnc *r, int b) { re_bit(r, &f->m[f->h], b, RC_S_BIT_RATE); f->h = ((f->h << 1) | b) & 3; }

void models_init_content(Models *m, const uint8_t *frm, uint32_t from_size, int kd, int ko) {
    for (int c = 0; c < LIT0_CTX; c++) lit_tree_seed_e(frm, from_size, 0, &m->lit0[c]);
    lit_tree_seed_e(frm, from_size, 1, &m->lit1);
    rc_init_tok(&m->tok, kd, ko);   /* gd/go rice + gl(+seed)/gs/glo + outb + flag + rep0 (rc_models.h) */
    m->rep0h = 0;
    m->last_dist = 0;
}

uint32_t ugr_price(const A1UGRice *g, uint32_t v) { return ugr_xfer((A1UGRice *)g, NULL, v); }
uint32_t ugg_price(const A1UGGamma *g, uint32_t v) { return ugg_xfer((A1UGGamma *)g, NULL, v); }

uint32_t bt_price_static(const A1BitTree *t, uint8_t byte) {
    return (uint32_t)bt_xfer((A1BitTree *)t, NULL, byte, 0, BT_XFER_PRICE);
}

uint32_t bit_price_update(uint16_t *prob, int bit, int rate) {
    uint32_t cost = bit_price(*prob, bit);
    *prob = rc_adapt(*prob, bit, rate);
    return cost;
}

uint64_t bt_price_update(A1BitTree *t, uint8_t byte, int rate) {
    return bt_xfer(t, NULL, byte, rate, BT_XFER_PRICE_UPDATE);
}
