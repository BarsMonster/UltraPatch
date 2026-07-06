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
    uint32_t p = *prob, bound = (r->range >> 12) * p;
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

/* Raw (no-model) Elias-gamma writers. After Features 4+7 the main encoder no longer ships any
 * raw-gamma header field (the shift map went adaptive-gamma; the token/op counts were dropped), so
 * these are unused in the normal encoder build — but the model_diff test bench still drives them for its
 * raw_gz mirror self-test (check-models), so keep them and silence the per-binary unused warning. */
static ENC_UNUSED void w_gamma(REnc *r, uint32_t m) {
    int n = bitlen32(m) - 1;
    for (int i = 0; i < n; i++) re_raw(r, 0);
    for (int i = n; i >= 0; i--) re_raw(r, (int)((m >> i) & 1u));
}

void w_gz(REnc *r, uint32_t x) { w_gamma(r, x + 1u); }

void bt_encode(A1BitTree *t, REnc *r, uint8_t byte, int rate) {
    int m = 1;
    for (int i = 7; i >= 0; i--) {
        int bit = (byte >> i) & 1;
        uint16_t p = a1_bt_get(t, m - 1);
        re_bit(r, &p, bit, rate);
        a1_bt_set(t, m - 1, p);
        m = (m << 1) | bit;
    }
}

void lit_tree_seed_e(const uint8_t *frm, size_t n, int parity, A1BitTree *t) {
    uint32_t hist[256], w[512];
    for (int i = 0; i < 256; i++) hist[i] = 1;
    for (size_t i = 0; i < n; i++) if ((int)(i & 1) == parity) hist[frm[i]]++;
    memset(t->p, 0, sizeof t->p);
    rc_lit_tree_from_hist(t, hist, w);   /* mirror of decoder lit_tree_from_hist */
}

void ug_init_e(UGE *g, char code, int k) {
    g->code = (uint8_t)code; g->k = (uint8_t)k;
    for (int i = 0; i <= UG_CTX; i++) { g->u[i] = RC_PHALF; for (int j = 0; j <= UG_CTX; j++) g->m[i][j] = RC_PHALF; }
}

int ug_c(int x) { return x < UG_CTX ? x : UG_CTX; }

/* mirror of decoder ugg_seed_cont: bias the first `depth` unary positions toward continue (bit 1). */
void ug_seed_cont_e(UGE *g, int depth) {
    rc_seed_cont_u(g->u, UG_CTX, depth);
}

void ug_encode(UGE *g, REnc *r, uint32_t v) {
    uint32_t cl;
    if (g->code == 'r') {
        cl = v >> g->k;
        for (uint32_t pos = 0; pos < cl; pos++) re_bit(r, &g->u[ug_c((int)pos)], 1, RC_S_BIT_RATE);
        re_bit(r, &g->u[ug_c((int)cl)], 0, RC_S_BIT_RATE);
        for (int pos = 0; pos < g->k; pos++) re_bit(r, &g->m[ug_c((int)cl)][ug_c(g->k - 1 - pos)], (int)((v >> (g->k - 1 - pos)) & 1u), RC_S_BIT_RATE);  /* rice: LSB-anchored ctx */
    } else {
        uint32_t mm = v + 1u;
        cl = (uint32_t)bitlen32(mm) - 1u;
        for (uint32_t pos = 0; pos < cl; pos++) re_bit(r, &g->u[ug_c((int)pos)], 1, RC_S_BIT_RATE);
        re_bit(r, &g->u[ug_c((int)cl)], 0, RC_S_BIT_RATE);
        for (uint32_t pos = 0; pos < cl; pos++) re_bit(r, &g->m[ug_c((int)cl)][ug_c((int)pos)], (int)((mm >> (cl - 1u - pos)) & 1u), RC_S_BIT_RATE);
    }
}

/* MTF dict-index model: IDX_CTX / A1IdxUnary / a1_idx_init single-sourced in rc_models.h (decoder mirror).
 * The encoded index value v is ~54% zero; unary fits that concentration and drops the per-stream A1UGGamma. */
void idx_encode(A1IdxUnary *g, REnc *r, uint32_t v) {
    for (uint32_t pos = 0; pos < v; pos++) re_bit(r, &g->u[pos < IDX_CTX ? pos : IDX_CTX - 1], 1, RC_S_BIT_RATE);
    re_bit(r, &g->u[v < IDX_CTX ? v : IDX_CTX - 1], 0, RC_S_BIT_RATE);
}

/* order-2 token flag: the A1Flag1 struct + a1_fl_init are single-sourced in rc_models.h (decoder mirror). */
void fl_encode(A1Flag1 *f, REnc *r, int b) { re_bit(r, &f->m[f->h], b, RC_S_BIT_RATE); f->h = ((f->h << 1) | b) & 3; }
