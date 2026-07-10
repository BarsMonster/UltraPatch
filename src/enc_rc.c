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
/* byte trees use the shared packed up_BitTree + up_bt_get/up_bt_set/up_bt_init from rc_models.h (decoder-identical) */

static void re_put(REnc *r, uint8_t b) {
    if (!r->count_only) {
        buf_put(&r->out, b);
    } else {
        r->out.n++;
        r->count_zero_run = b ? 0 : r->count_zero_run + 1u;
    }
}

static void re_shift_low(REnc *r) {
    if (r->low < 0xff000000ull || r->low > 0xffffffffull) {
        uint8_t c = r->cache;
        for (;;) {
            re_put(r, (uint8_t)(c + (uint8_t)(r->low >> 32)));
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
void re_init_count(REnc *r) { re_init(r); r->count_only = 1; }

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
    if (r->count_only) {
        /* The material sink trims only zero bytes emitted by the five flush shifts. A zero run may
         * begin in the pre-flush body, so cap its count at the bytes appended after `base`. */
        size_t flush_n = r->out.n - base;
        size_t trim = r->count_zero_run < flush_n ? r->count_zero_run : flush_n;
        r->out.n -= trim;
    } else {
        while (r->out.n > base && r->out.d[r->out.n - 1] == 0) r->out.n--;
    }
    Buf b = r->out;
    r->out = (Buf){0};
    return b;
}

void put_raw_bits(REnc *r, uint32_t v, int nb) {
    for (int sh = nb - 1; sh >= 0; sh--) re_raw(r, (int)((v >> sh) & 1u));
}

enum { BT_XFER_ENC, BT_XFER_PRICE, BT_XFER_PRICE_UPDATE };

static uint64_t bt_xfer(up_BitTree *t, REnc *r, uint8_t byte, int rate, int mode) {
    int m = 1;
    uint64_t cost = 0;
    for (int i = 7; i >= 0; i--) {
        int bit = (byte >> i) & 1;
        uint16_t p = up_bt_get(t, m - 1);
        if (mode == BT_XFER_ENC) {
            re_bit(r, &p, bit, rate);
            up_bt_set(t, m - 1, p);
        } else {
            cost += bit_price(p, bit);
            if (mode == BT_XFER_PRICE_UPDATE) up_bt_set(t, m - 1, rc_adapt(p, bit, rate));
        }
        m = (m << 1) | bit;
    }
    return cost;
}

void bt_encode(up_BitTree *t, REnc *r, uint8_t byte, int rate) {
    (void)bt_xfer(t, r, byte, rate, BT_XFER_ENC);
}

void lit_tree_seed_e(const uint8_t *frm, size_t n, int parity, up_BitTree *t) {
    uint32_t hist[256], w[512];
    for (int i = 0; i < 256; i++) hist[i] = 1;
    for (size_t i = 0; i < n; i++) if ((int)(i & 1) == parity) hist[frm[i]]++;
    memset(t->p, 0, sizeof t->p);
    rc_lit_tree_from_hist(t, hist, w);   /* mirror of decoder lit_tree_from_hist */
}

void lit_seed_trees_init(LitSeedTrees *s, const uint8_t *frm, size_t n) {
    lit_tree_seed_e(frm, n, 0, &s->lit0);
    lit_tree_seed_e(frm, n, 1, &s->lit1);
}

/* Neutral-init entry points: thin out-of-line wrappers over the shared rc_ugr_init/rc_ugg_init
 * (rc_models.h) so the host keeps ONE copy of each init loop instead of inlining it at every model. */
void ugr_init_e(up_UGRice *g, int k) { rc_ugr_init(g, k); }
void ugg_init_e(up_UGGamma *g) { rc_ugg_init(g); }

/* Shared single-bit transfer: encode (r!=NULL) or price. adapt=0 leaves the prob untouched (the frozen
 * ugr_price/ugg_price snapshots); adapt=1 advances it via bit_price_update (the adaptive delta streams). */
uint32_t ug_bit_xfer(REnc *r, uint16_t *prob, int bit, int adapt) {
    if (r) {
        re_bit(r, prob, bit, RC_S_BIT_RATE);
        return 0;
    }
    return adapt ? bit_price_update(prob, bit, RC_S_BIT_RATE) : bit_price(*prob, bit);
}

/* Adaptive unary prefix: min(pos,clampmax)-clamped u[] priors (mirror of the decoder s_unary). */
uint32_t unary_xfer(REnc *r, uint16_t *u, uint32_t clampmax, uint32_t v, int adapt) {
    uint32_t cost = 0;
    for (uint32_t pos = 0; pos < v; pos++)
        cost += ug_bit_xfer(r, &u[pos < clampmax ? pos : clampmax], 1, adapt);
    return cost + ug_bit_xfer(r, &u[v < clampmax ? v : clampmax], 0, adapt);
}

/* Rice/Gamma transfer cores: statically typed, sharing the unary-prefix + bit transfer helpers.
 * These snapshots are frozen (adapt=0): the prob state never advances during pricing. Keep Rice
 * out of line so the feasibility arm does not clone the whole unary loop for price and emit. */
static uint32_t RC_NOINLINE ugr_xfer(up_UGRice *g, REnc *r, uint32_t v) {
    if (!rc_rice_feasible(v, g->k)) { if (r) r->rice_overflow = 1; return UINT32_MAX; }
    uint32_t cl = v >> g->k;
    uint32_t cost = unary_xfer(r, g->u, UP_UG_CTX, cl, 0);
    for (int pos = 0; pos < g->k; pos++)
        cost += ug_bit_xfer(r, &g->m[UP_UG_C((int)cl)][UP_UG_C(g->k - 1 - pos)],
                            (int)((v >> (g->k - 1 - pos)) & 1u), 0);  /* rice: LSB-anchored ctx */
    return cost;
}
static uint32_t ugg_xfer(up_UGGamma *g, REnc *r, uint32_t v) {
    uint32_t mm = v + 1u;
    uint32_t cl = (uint32_t)bitlen32(mm) - 1u;
    int row = UP_UG_C((int)cl);
    uint32_t cost = unary_xfer(r, g->u, UP_UG_CTX, cl, 0);
    for (uint32_t pos = 0; pos < cl; pos++)
        cost += ug_bit_xfer(r, &g->m[rc_ugg_mant_idx(row, (int)pos)],
                            (int)((mm >> (cl - 1u - pos)) & 1u), 0);
    return cost;
}

void ugr_encode(up_UGRice *g, REnc *r, uint32_t v) { (void)ugr_xfer(g, r, v); }
void ugg_encode(up_UGGamma *g, REnc *r, uint32_t v) { (void)ugg_xfer(g, r, v); }

/* order-2 token flag: the up_Flag1 struct + up_fl_init are single-sourced in rc_models.h (decoder mirror). */
void fl_encode(up_Flag1 *f, REnc *r, int b) { re_bit(r, &f->m[f->h], b, RC_S_BIT_RATE); f->h = rc_fl_hist(f->h, b); }

void models_init_content(Models *m, const LitSeedTrees *seeds, int kd, int ko) {
    for (int c = 0; c < UP_LIT0_CTX; c++) m->lit0[c] = seeds->lit0;
    m->lit1 = seeds->lit1;
    rc_init_tok(&m->tok, kd, ko);   /* gd/go rice + gl(+seed)/gs/glo + outb + flag + rep0 (rc_models.h) */
    m->rep0h = 0;
    m->last_dist = 0;
}

uint32_t ugr_price(const up_UGRice *g, uint32_t v) { return ugr_xfer((up_UGRice *)g, NULL, v); }
uint32_t ugg_price(const up_UGGamma *g, uint32_t v) { return ugg_xfer((up_UGGamma *)g, NULL, v); }

uint32_t bt_price_static(const up_BitTree *t, uint8_t byte) {
    return (uint32_t)bt_xfer((up_BitTree *)t, NULL, byte, 0, BT_XFER_PRICE);
}

uint32_t bit_price_update(uint16_t *prob, int bit, int rate) {
    uint32_t cost = bit_price(*prob, bit);
    *prob = rc_adapt(*prob, bit, rate);
    return cost;
}

uint64_t bt_price_update(up_BitTree *t, uint8_t byte, int rate) {
    return bt_xfer(t, NULL, byte, rate, BT_XFER_PRICE_UPDATE);
}
