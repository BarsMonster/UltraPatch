/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 *
 * A1 host encoder module -- binary range encoder (REnc) + entropy models (bt/ug/idx/fl, raw bits, gamma).
 * Part of the single translation unit rooted at src/patch_generate.c, which
 * #includes the enc_*.inc modules in dependency order. NOT a standalone TU:
 * it relies on the shared prologue (typedefs, file-scope state) and on symbols
 * defined by earlier modules. Umbrella split preserves the single-TU byte-exact
 * wire and model_diff.c's #include "patch_generate.c".
 */
/* ------------------------------------------------------------------------------------- */
/* Binary range encoder and models.                                                        */
/* ------------------------------------------------------------------------------------- */
typedef struct { uint64_t low; uint32_t range; uint8_t cache; uint32_t csz; Buf out; } REnc;
typedef struct { uint8_t code, k; uint16_t u[UG_CTX + 1], m[UG_CTX + 1][UG_CTX + 1]; } UGE;
/* byte trees use the shared packed BitTree + bt_get/bt_set/bt_init from rc_models.h (decoder-identical) */
typedef struct { int64_t *dic; uint16_t cap, K; int64_t last; uint16_t rep[4], hit; uint8_t rh; } DRE;

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

static void re_init(REnc *r) { memset(r, 0, sizeof(*r)); r->range = 0xffffffffu; r->csz = 1; }

/* RC_S_BIT_RATE (Golomb / order-2 flag / MTF rep+hit adaptation rate) and RC_REP0_INIT (rep0 prior)
 * are single-sourced in rc_models.h, shared verbatim with the decoder. */

static void re_bit(REnc *r, uint16_t *prob, int bit, int rate) {
    uint32_t p = *prob, bound = (r->range >> 12) * p;
    if (bit == 0) r->range = bound;
    else { r->low += bound; r->range -= bound; }
    *prob = rc_adapt(p, bit, rate);   /* shared update rule (rc_models.h), mirror of decoder s_bit_r */
    while (r->range < RC_KTOP) { r->range <<= 8; re_shift_low(r); }
}

static void re_raw(REnc *r, int bit) {
    uint32_t bound = r->range >> 1;
    if (bit == 0) r->range = bound;
    else { r->low += bound; r->range -= bound; }
    while (r->range < RC_KTOP) { r->range <<= 8; re_shift_low(r); }
}

static Buf re_flush_opt(REnc *r) {
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

static void put_raw_bits(REnc *r, uint32_t v, int nb) {
    for (int sh = nb - 1; sh >= 0; sh--) re_raw(r, (int)((v >> sh) & 1u));
}

/* Raw (no-model) Elias-gamma writers. After Features 4+7 the main encoder no longer ships any
 * raw-gamma header field (the shift map went adaptive-gamma; the token/op counts were dropped), so
 * these are unused in the hy_enc build — but the model_diff test bench still drives them for its
 * raw_gz mirror self-test (check-models), so keep them and silence the per-binary unused warning. */
static __attribute__((unused)) void w_gamma(REnc *r, uint32_t m) {
    int n = bitlen32(m) - 1;
    for (int i = 0; i < n; i++) re_raw(r, 0);
    for (int i = n; i >= 0; i--) re_raw(r, (int)((m >> i) & 1u));
}

static __attribute__((unused)) void w_gz(REnc *r, uint32_t x) { w_gamma(r, x + 1u); }

static void bt_encode(BitTree *t, REnc *r, uint8_t byte, int rate) {
    int m = 1;
    for (int i = 7; i >= 0; i--) {
        int bit = (byte >> i) & 1;
        uint16_t p = bt_get(t, m - 1);
        re_bit(r, &p, bit, rate);
        bt_set(t, m - 1, p);
        m = (m << 1) | bit;
    }
}

static void lit_tree_seed_e(const uint8_t *frm, size_t n, int parity, BitTree *t) {
    uint32_t hist[256], w[512];
    for (int i = 0; i < 256; i++) hist[i] = 1;
    for (size_t i = 0; i < n; i++) if ((int)(i & 1) == parity) hist[frm[i]]++;
    memset(t->p, 0, sizeof t->p);
    rc_lit_tree_from_hist(t, hist, w);   /* mirror of decoder lit_tree_from_hist */
}

static void ug_init_e(UGE *g, char code, int k) {
    g->code = (uint8_t)code; g->k = (uint8_t)k;
    for (int i = 0; i <= UG_CTX; i++) { g->u[i] = RC_PHALF; for (int j = 0; j <= UG_CTX; j++) g->m[i][j] = RC_PHALF; }
}

static int ug_c(int x) { return x < UG_CTX ? x : UG_CTX; }

/* mirror of decoder ugg_seed_cont: bias the first `depth` unary positions toward continue (bit 1). */
static void ug_seed_cont_e(UGE *g, int depth) {
    rc_seed_cont_u(g->u, UG_CTX, depth);
}

static void ug_encode(UGE *g, REnc *r, uint32_t v) {
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

/* MTF dict-index model: IDX_CTX / IdxUnary / idx_init single-sourced in rc_models.h (decoder mirror).
 * The encoded index value v is ~54% zero; unary fits that concentration and drops the per-stream UGGamma. */
static void idx_encode(IdxUnary *g, REnc *r, uint32_t v) {
    for (uint32_t pos = 0; pos < v; pos++) re_bit(r, &g->u[pos < IDX_CTX ? pos : IDX_CTX - 1], 1, RC_S_BIT_RATE);
    re_bit(r, &g->u[v < IDX_CTX ? v : IDX_CTX - 1], 0, RC_S_BIT_RATE);
}

/* order-2 token flag: the Flag1 struct + fl_init are single-sourced in rc_models.h (decoder mirror). */
static void fl_encode(Flag1 *f, REnc *r, int b) { re_bit(r, &f->m[f->h], b, RC_S_BIT_RATE); f->h = ((f->h << 1) | b) & 3; }
