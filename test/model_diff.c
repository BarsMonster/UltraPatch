/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 */

/* Model-level encoder/decoder differential test — ENCODER side + orchestration.
 *
 * See test/model_diff.h for the design. This TU #includes the whole host encoder
 * (src/patch_generate.c) WITHOUT RC_V3_ENC_MAIN, so patch_generate's main() is not compiled
 * and the wire is untouched (proved byte-neutral by the golden gate). That gives direct access
 * to every file-static encoder model (re_bit / bt_encode / ug_encode / idx_encode / fl_encode /
 * bv_encode / put_raw_bits / w_gz / emit_delta). Each test encodes a deterministic symbol stream
 * with the encoder model, hands the bytes to the matching decoder bridge (md_*, in model_diff_dec.c),
 * and asserts exact symbol recovery. On mismatch it prints model/seed/index/expected/got and exits 1.
 *
 * Determinism: a fixed-seed 64-bit LCG (no rand()); >= 4 seeds and both a short (10) and a long
 * (10000) length per model; values interleave small/large runs so the adaptive probabilities drift. */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "patch_generate.c"   /* no RC_V3_ENC_MAIN => no main(); all encoder models in scope */
#include "model_diff.h"

/* ------------------------------------------------------------------------------------------- */
/* deterministic LCG + failure reporting                                                         */
/* ------------------------------------------------------------------------------------------- */
static uint64_t s_lcg;
static void     lcg_seed(uint64_t sd) { s_lcg = sd * 2862933555777941757ull + 3037000493ull; }
static uint32_t lcg32(void) { s_lcg = s_lcg * 6364136223846793005ull + 1442695040888963407ull; return (uint32_t)(s_lcg >> 32); }

static int g_models, g_streams;

static void fail_val(const char *m, unsigned seed, int idx, long long exp, long long got) {
    fprintf(stderr, "MODEL DIFF MISMATCH: model=%s seed=%u idx=%d expected=%lld got=%lld\n",
            m, seed, idx, exp, got);
    exit(1);
}
static void fail_err(const char *m, unsigned seed) {
    fprintf(stderr, "MODEL DIFF DECODE ERROR (decoder g_rcerr set): model=%s seed=%u\n", m, seed);
    exit(1);
}
#define CMP8(m, seed, exp, got, ns)  do { for (int _i = 0; _i < (ns); _i++) if ((exp)[_i] != (got)[_i]) fail_val(m, seed, _i, (long long)(exp)[_i], (long long)(got)[_i]); } while (0)
#define CMP32(m, seed, exp, got, ns) do { for (int _i = 0; _i < (ns); _i++) if ((exp)[_i] != (got)[_i]) fail_val(m, seed, _i, (long long)(exp)[_i], (long long)(got)[_i]); } while (0)
#define CMPI(m, seed, exp, got, ns)  do { for (int _i = 0; _i < (ns); _i++) if ((exp)[_i] != (got)[_i]) fail_val(m, seed, _i, (long long)(exp)[_i], (long long)(got)[_i]); } while (0)

#define MAXNS  10000
#define NBUF   (MAXNS + 64)
static const unsigned SEEDS[] = { 0x1u, 0x9E3779B9u, 0x2545F491u, 0xDEADBEEFu, 0x0C0FFEE1u };
#define NSEED ((int)(sizeof SEEDS / sizeof SEEDS[0]))
static const int LENS[] = { 10, 10000 };
#define NLEN 2

static uint32_t V32[NBUF], G32[NBUF];
static uint8_t  V8[NBUF],  G8[NBUF];
static int32_t  VI[NBUF],  GI[NBUF];
static uint8_t  OPS[NBUF];
static int64_t  M64[512];

/* ------------------------------------------------------------------------------------------- */
/* value generators (interleave small/large runs so the models adapt)                            */
/* ------------------------------------------------------------------------------------------- */
static void fill_bits(uint8_t *v, int ns) {
    for (int i = 0; i < ns; i++) { int hi = (i / 24) & 1; uint32_t r = lcg32(); v[i] = (uint8_t)(hi ? (r % 4u != 0u) : (r % 4u == 0u)); }
}
static void fill_bytes(uint32_t *v, int ns) {
    for (int i = 0; i < ns; i++) { int hi = (i / 64) & 1; uint32_t r = lcg32(); v[i] = hi ? (r & 0xffu) : (r % 16u); }
}
static void fill_ugr(uint32_t *v, int ns, int k) {
    for (int i = 0; i < ns; i++) { int hi = (i / 64) & 1; uint32_t q = lcg32() % (hi ? 120u : 4u); uint32_t rem = k ? (lcg32() & ((1u << k) - 1u)) : 0u; v[i] = (q << k) | rem; }
}
static void fill_ugg(uint32_t *v, int ns) {
    for (int i = 0; i < ns; i++) { int b = (i / 64) % 3; uint32_t r = lcg32(); v[i] = b == 0 ? (r % 8u) : b == 1 ? (r % 4096u) : (r & 0x7ffffffeu); }
}
static void fill_idx(uint32_t *v, int ns) {
    for (int i = 0; i < ns; i++) { int hi = (i / 64) & 1; uint32_t r = lcg32(); v[i] = hi ? (r % 200u) : (r % 3u); }
}
static void fill_rawbits(uint32_t *v, int ns, int nb) {
    uint32_t mask = nb >= 32 ? 0xffffffffu : ((1u << nb) - 1u);
    for (int i = 0; i < ns; i++) v[i] = lcg32() & mask;
}
static void fill_rawgz(uint32_t *v, int ns) {
    for (int i = 0; i < ns; i++) { int b = (i / 64) % 3; uint32_t r = lcg32(); v[i] = b == 0 ? (r % 4u) : b == 1 ? (r % 65536u) : (r & 0x7ffffffeu); }
}
static void fill_bv(int32_t *v, int ns) {
    /* INT32_MIN excluded: the zigzag-32 escape domain is [-0x7fffffff, 0x7fffffff] */
    for (int i = 0; i < ns; i++) { int b = (i / 64) % 3; uint32_t r = lcg32(); if (r == 0x80000000u) r = 0; v[i] = b == 0 ? (int32_t)(r % 128u) - 64 : b == 1 ? ((int32_t)r >> 8) : (int32_t)r; }
}
static void fill_mixed(uint8_t *ops, uint32_t *v, int ns) {
    for (int i = 0; i < ns; i++) {
        uint8_t op = (uint8_t)(lcg32() % (uint32_t)MX_N);
        uint32_t r = lcg32();
        ops[i] = op;
        switch (op) {
            case MX_RAW: case MX_BIT: case MX_FLAG: v[i] = r & 1u;            break;
            case MX_BT:                             v[i] = r & 0xffu;         break;
            case MX_UGR:                            v[i] = r % 2048u;         break;   /* quotient v>>3 modest */
            case MX_UGG:                            v[i] = r & 0x7ffffffeu;   break;
            default:                                v[i] = 0;                 break;
        }
    }
}

/* ------------------------------------------------------------------------------------------- */
/* encoder-side stream helpers (each is a full re_init -> encode -> re_flush_selfterm session)    */
/* ------------------------------------------------------------------------------------------- */
/* Wire body = re_flush_selfterm() output with the always-zero LZMA leading cache byte dropped, exactly
 * as the real blob assembly does (patch_generate.c: "body.d[0] is always 0 here" -> buf_write(body+1)).
 * The decoder's rc_init reads its 4 code bytes directly, assuming that drop. Assert the invariant. */
static Buf flush_wire(REnc *r) {
    Buf b = re_flush_selfterm(r);
    if (b.n == 0 || b.d[0] != 0) { fprintf(stderr, "model_diff: range-coder leading byte not 0 (n=%zu)\n", b.n); exit(1); }
    memmove(b.d, b.d + 1, b.n - 1);
    b.n--;
    return b;
}
static Buf enc_bits(int rate, const uint8_t *v, int ns) { REnc r; re_init(&r); uint16_t p = RC_PHALF; for (int i = 0; i < ns; i++) re_bit(&r, &p, v[i] & 1, rate); return flush_wire(&r); }
static Buf enc_bt(int rate, const uint32_t *v, int ns) { REnc r; re_init(&r); BitTree t; bt_init(&t); for (int i = 0; i < ns; i++) bt_encode(&t, &r, (uint8_t)v[i], rate); return flush_wire(&r); }
static Buf enc_bv(const int32_t *v, int ns) { REnc r; re_init(&r); BitTree t; bt_init(&t); for (int i = 0; i < ns; i++) bv_encode(&t, &r, (int64_t)v[i]); return flush_wire(&r); }
static Buf enc_ugr(int k, const uint32_t *v, int ns) { REnc r; re_init(&r); UGE g; ug_init_e(&g, 'r', k); for (int i = 0; i < ns; i++) ug_encode(&g, &r, v[i]); return flush_wire(&r); }
static Buf enc_ugg(int depth, const uint32_t *v, int ns) { REnc r; re_init(&r); UGE g; ug_init_e(&g, 'g', 0); if (depth > 0) ug_seed_cont_e(&g, depth); for (int i = 0; i < ns; i++) ug_encode(&g, &r, v[i]); return flush_wire(&r); }
static Buf enc_idx(const uint32_t *v, int ns) { REnc r; re_init(&r); IdxUnary g; idx_init(&g, RC_IDX_SEED); for (int i = 0; i < ns; i++) idx_encode(&g, &r, v[i]); return flush_wire(&r); }
static Buf enc_flag(const uint8_t *v, int ns) { REnc r; re_init(&r); Flag1 f; fl_init(&f); for (int i = 0; i < ns; i++) fl_encode(&f, &r, v[i] & 1); return flush_wire(&r); }
static Buf enc_rep0(const uint8_t *v, int ns) { REnc r; re_init(&r); uint16_t rep0[2]; rep0[0] = rep0[1] = RC_REP0_INIT; int h = 0; for (int i = 0; i < ns; i++) { re_bit(&r, &rep0[h], v[i] & 1, RC_S_BIT_RATE); h = v[i] & 1; } return flush_wire(&r); }
static Buf enc_rawbits(int nb, const uint32_t *v, int ns) { REnc r; re_init(&r); for (int i = 0; i < ns; i++) put_raw_bits(&r, v[i], nb); return flush_wire(&r); }
static Buf enc_rawgz(const uint32_t *v, int ns) { REnc r; re_init(&r); for (int i = 0; i < ns; i++) w_gz(&r, v[i]); return flush_wire(&r); }
static Buf enc_mixed(const uint8_t *ops, const uint32_t *v, int ns) {
    REnc r; re_init(&r);
    uint16_t p = RC_PHALF; BitTree bt; bt_init(&bt); UGE gr; ug_init_e(&gr, 'r', MX_UGR_K); UGE gg; ug_init_e(&gg, 'g', 0); Flag1 f; fl_init(&f);
    for (int i = 0; i < ns; i++) switch (ops[i]) {
        case MX_RAW:  re_raw(&r, (int)(v[i] & 1u));                      break;
        case MX_BIT:  re_bit(&r, &p, (int)(v[i] & 1u), MX_BIT_RATE);     break;
        case MX_BT:   bt_encode(&bt, &r, (uint8_t)v[i], MX_BT_RATE);     break;
        case MX_UGR:  ug_encode(&gr, &r, v[i]);                         break;
        case MX_UGG:  ug_encode(&gg, &r, v[i]);                         break;
        case MX_FLAG: fl_encode(&f, &r, (int)(v[i] & 1u));              break;
        default: break;
    }
    return flush_wire(&r);
}
/* MTF dict stream: encode m deltas, then probe one over-cap distinct value (must be refused
 * by construction — g_emit_overflow, never emitted). *of_during must be 0 (the m deltas fit),
 * *of_after must be 1 (the probe hit the cap). */
static Buf enc_mtf(int kind, const int64_t *v, int m, int *of_during, int *of_after, int64_t extra) {
    Models *M = (Models *)calloc(1, sizeof *M);
    REnc r; re_init(&r);
    bt_init(&M->dval);
    dr_init_e(&M->dr_bl, M->dic_bl, DR_KCAP_BL, DR_HIT_INIT);
    dr_init_e(&M->dr_ex, M->dic_ex, DR_KCAP_EX, DR_HIT_INIT);
    idx_init(&M->dibl, RC_IDX_SEED);
    idx_init(&M->diex, RC_IDX_SEED);
    g_emit_overflow = 0;
    for (int i = 0; i < m; i++) emit_delta(M, &r, kind, v[i]);
    *of_during = g_emit_overflow;
    emit_delta(M, &r, kind, extra);   /* K == cap now => must overflow, emit nothing */
    *of_after = g_emit_overflow;
    free(M);
    return flush_wire(&r);
}

/* ------------------------------------------------------------------------------------------- */
/* per-model tests                                                                               */
/* ------------------------------------------------------------------------------------------- */
static void test_bits(void) {
    g_models++;
    int rates[2] = { 4, 5 };
    for (int ri = 0; ri < 2; ri++) for (int si = 0; si < NSEED; si++) for (int li = 0; li < NLEN; li++) {
        unsigned seed = SEEDS[si]; int ns = LENS[li];
        lcg_seed(seed ^ (0x101u * (unsigned)rates[ri]) ^ ((unsigned)li << 20));
        fill_bits(V8, ns);
        Buf b = enc_bits(rates[ri], V8, ns);
        if (md_bits(b.d, b.n, rates[ri], ns, G8)) fail_err("bit", seed);
        CMP8("bit", seed, V8, G8, ns);
        buf_free(&b); g_streams++;
    }
}
static void test_bt(void) {
    g_models++;
    int rates[2] = { 4, 5 };
    static const uint32_t edge[] = { 0,255,1,254,128,127,0xAA,0x55,0,0,0,255,255,255,7,240,42,42 };
    int en = (int)(sizeof edge / sizeof edge[0]);
    for (int ri = 0; ri < 2; ri++) {
        for (int si = 0; si < NSEED; si++) for (int li = 0; li < NLEN; li++) {
            unsigned seed = SEEDS[si]; int ns = LENS[li];
            lcg_seed(seed ^ (0x202u * (unsigned)rates[ri]) ^ ((unsigned)li << 20));
            fill_bytes(V32, ns);
            Buf b = enc_bt(rates[ri], V32, ns);
            if (md_bt(b.d, b.n, rates[ri], ns, G32)) fail_err("bt", seed);
            CMP32("bt", seed, V32, G32, ns);
            buf_free(&b); g_streams++;
        }
        Buf b = enc_bt(rates[ri], edge, en);
        if (md_bt(b.d, b.n, rates[ri], en, G32)) fail_err("bt/edge", 0);
        CMP32("bt/edge", (unsigned)rates[ri], edge, G32, en);
        buf_free(&b); g_streams++;
    }
}
static void test_bv(void) {
    g_models++;
    static const int32_t edge[] = { 0,1,-1,63,64,-63,-64,65,-65,8191,8192,-8192,
        0x7fffffff, -0x7fffffff, 0x3fffffff, (int32_t)0xc0000000, 100, -100 };
    int en = (int)(sizeof edge / sizeof edge[0]);
    for (int si = 0; si < NSEED; si++) for (int li = 0; li < NLEN; li++) {
        unsigned seed = SEEDS[si]; int ns = LENS[li];
        lcg_seed(seed ^ 0x0BADu ^ ((unsigned)li << 20));
        fill_bv(VI, ns);
        Buf b = enc_bv(VI, ns);
        if (md_bv(b.d, b.n, ns, GI)) fail_err("bv", seed);
        CMPI("bv", seed, VI, GI, ns);
        buf_free(&b); g_streams++;
    }
    Buf b = enc_bv(edge, en);
    if (md_bv(b.d, b.n, en, GI)) fail_err("bv/edge", 0);
    CMPI("bv/edge", 0, edge, GI, en);
    buf_free(&b); g_streams++;
}
static void test_ugr(void) {
    g_models++;
    int ks[3] = { 0, 11, 15 };
    for (int ki = 0; ki < 3; ki++) {
        int k = ks[ki];
        for (int si = 0; si < NSEED; si++) for (int li = 0; li < NLEN; li++) {
            unsigned seed = SEEDS[si]; int ns = LENS[li];
            lcg_seed(seed ^ (0x303u * (unsigned)(k + 1)) ^ ((unsigned)li << 20));
            fill_ugr(V32, ns, k);
            Buf b = enc_ugr(k, V32, ns);
            if (md_ugr(b.d, b.n, k, ns, G32)) fail_err("ug_rice", seed);
            CMP32("ug_rice", seed, V32, G32, ns);
            buf_free(&b); g_streams++;
        }
        /* edge incl. 0, 1, the k-boundary, and a huge quotient just below RC_RICE_UNARY_MAX */
        uint32_t hq = 0xffffffffu >> k; if (hq > 900000u) hq = 900000u;
        uint32_t e[6]; e[0] = 0; e[1] = 1; e[2] = 2;
        e[3] = k ? (1u << k) - 1u : 0u; e[4] = k ? (1u << k) : 1u; e[5] = hq << k;
        Buf b = enc_ugr(k, e, 6);
        if (md_ugr(b.d, b.n, k, 6, G32)) fail_err("ug_rice/edge", (unsigned)k);
        CMP32("ug_rice/edge", (unsigned)k, e, G32, 6);
        buf_free(&b); g_streams++;
    }
}
static void test_ugg(void) {
    g_models++;
    int depths[4] = { 0, RC_SEED_DEPTH_GDL, RC_SEED_DEPTH_GADJ, RC_SEED_DEPTH_PG2 };  /* 0, 6, 3, 1 */
    static const uint32_t edge[] = { 0,1,2,3,7,8,4095,4096,0x7ffffffe,0x40000000,0x3fffffff,0 };
    int en = (int)(sizeof edge / sizeof edge[0]);
    for (int di = 0; di < 4; di++) {
        int depth = depths[di];
        for (int si = 0; si < NSEED; si++) for (int li = 0; li < NLEN; li++) {
            unsigned seed = SEEDS[si]; int ns = LENS[li];
            lcg_seed(seed ^ (0x404u * (unsigned)(depth + 1)) ^ ((unsigned)li << 20));
            fill_ugg(V32, ns);
            Buf b = enc_ugg(depth, V32, ns);
            if (md_ugg(b.d, b.n, depth, ns, G32)) fail_err("ug_gamma", seed);
            CMP32("ug_gamma", seed, V32, G32, ns);
            buf_free(&b); g_streams++;
        }
        Buf b = enc_ugg(depth, edge, en);
        if (md_ugg(b.d, b.n, depth, en, G32)) fail_err("ug_gamma/edge", (unsigned)depth);
        CMP32("ug_gamma/edge", (unsigned)depth, edge, G32, en);
        buf_free(&b); g_streams++;
    }
}
static void test_idx(void) {
    g_models++;
    static const uint32_t edge[] = { 0,1,2,3,4,5,6,10,50,140,199,0 };
    int en = (int)(sizeof edge / sizeof edge[0]);
    for (int si = 0; si < NSEED; si++) for (int li = 0; li < NLEN; li++) {
        unsigned seed = SEEDS[si]; int ns = LENS[li];
        lcg_seed(seed ^ 0x515u ^ ((unsigned)li << 20));
        fill_idx(V32, ns);
        Buf b = enc_idx(V32, ns);
        if (md_idx(b.d, b.n, ns, G32)) fail_err("idx_unary", seed);
        CMP32("idx_unary", seed, V32, G32, ns);
        buf_free(&b); g_streams++;
    }
    Buf b = enc_idx(edge, en);
    if (md_idx(b.d, b.n, en, G32)) fail_err("idx_unary/edge", 0);
    CMP32("idx_unary/edge", 0, edge, G32, en);
    buf_free(&b); g_streams++;
}
static void test_flag(void) {
    g_models++;
    for (int si = 0; si < NSEED; si++) for (int li = 0; li < NLEN; li++) {
        unsigned seed = SEEDS[si]; int ns = LENS[li];
        lcg_seed(seed ^ 0x616u ^ ((unsigned)li << 20));
        fill_bits(V8, ns);
        Buf b = enc_flag(V8, ns);
        if (md_flag(b.d, b.n, ns, G8)) fail_err("flag", seed);
        CMP8("flag", seed, V8, G8, ns);
        buf_free(&b); g_streams++;
    }
}
static void test_rep0(void) {
    g_models++;
    for (int si = 0; si < NSEED; si++) for (int li = 0; li < NLEN; li++) {
        unsigned seed = SEEDS[si]; int ns = LENS[li];
        lcg_seed(seed ^ 0x717u ^ ((unsigned)li << 20));
        fill_bits(V8, ns);
        Buf b = enc_rep0(V8, ns);
        if (md_rep0(b.d, b.n, ns, G8)) fail_err("rep0", seed);
        CMP8("rep0", seed, V8, G8, ns);
        buf_free(&b); g_streams++;
    }
}
static void test_rawbits(void) {
    g_models++;
    int nbs[5] = { 1, RC_KFIELD_BITS, 8, 16, 24 };   /* incl. the 4-bit header k-field width */
    for (int ni = 0; ni < 5; ni++) {
        int nb = nbs[ni];
        for (int si = 0; si < NSEED; si++) for (int li = 0; li < NLEN; li++) {
            unsigned seed = SEEDS[si]; int ns = LENS[li];
            lcg_seed(seed ^ (0x818u * (unsigned)nb) ^ ((unsigned)li << 20));
            fill_rawbits(V32, ns, nb);
            Buf b = enc_rawbits(nb, V32, ns);
            if (md_rawbits(b.d, b.n, nb, ns, G32)) fail_err("raw_bits", seed);
            CMP32("raw_bits", seed, V32, G32, ns);
            buf_free(&b); g_streams++;
        }
        uint32_t mask = nb >= 32 ? 0xffffffffu : ((1u << nb) - 1u);
        uint32_t e[4]; e[0] = 0; e[1] = mask; e[2] = mask >> 1; e[3] = mask & 0xA5A5A5A5u;
        Buf b = enc_rawbits(nb, e, 4);
        if (md_rawbits(b.d, b.n, nb, 4, G32)) fail_err("raw_bits/edge", (unsigned)nb);
        CMP32("raw_bits/edge", (unsigned)nb, e, G32, 4);
        buf_free(&b); g_streams++;
    }
}
static void test_rawgz(void) {
    g_models++;
    static const uint32_t edge[] = { 0,1,2,3,65535,65536,0x7ffffffe,0 };
    int en = (int)(sizeof edge / sizeof edge[0]);
    for (int si = 0; si < NSEED; si++) for (int li = 0; li < NLEN; li++) {
        unsigned seed = SEEDS[si]; int ns = LENS[li];
        lcg_seed(seed ^ 0x919u ^ ((unsigned)li << 20));
        fill_rawgz(V32, ns);
        Buf b = enc_rawgz(V32, ns);
        if (md_rawgz(b.d, b.n, ns, G32)) fail_err("raw_gz", seed);
        CMP32("raw_gz", seed, V32, G32, ns);
        buf_free(&b); g_streams++;
    }
    Buf b = enc_rawgz(edge, en);
    if (md_rawgz(b.d, b.n, en, G32)) fail_err("raw_gz/edge", 0);
    CMP32("raw_gz/edge", 0, edge, G32, en);
    buf_free(&b); g_streams++;
}
/* Build an MTF delta sequence that grows the dict to exactly `cap` distinct entries (cap-1
 * escapes), interleaving repeat-last and older-value hits, then two zero-value ops to exercise
 * the last==0 repeat context. Returns the op count. */
static int build_mtf(int64_t *v, int cap) {
    int m = 0;
    for (int k = 1; k <= cap - 1; k++) {
        int64_t val = (int64_t)k * 7;
        v[m++] = val;                                       /* distinct escape -> K grows */
        if ((k & 3) == 0) v[m++] = val;                     /* repeat-last (rep bit) */
        if (k >= 10 && (k % 5) == 0) v[m++] = (int64_t)(k - 5) * 7;  /* hit an older dict value */
    }
    v[m++] = 0;   /* hit the never-evicted initial 0 (index >= 1) -> last becomes 0 */
    v[m++] = 0;   /* repeat-last with last==0 -> rep context bit for dic[0]==0 */
    return m;
}
static void test_mtf(void) {
    g_models++;
    struct { int kind, cap; const char *nm; } cases[2] = {
        { EV_BL, DR_KCAP_BL, "mtf/bl" }, { EV_EX, DR_KCAP_EX, "mtf/ex" }
    };
    for (int c = 0; c < 2; c++) {
        int cap = cases[c].cap;
        int m = build_mtf(M64, cap);
        int of_during = -1, of_after = -1;
        Buf b = enc_mtf(cases[c].kind, M64, m, &of_during, &of_after, (int64_t)(cap + 5) * 7);
        if (of_during != 0) { fprintf(stderr, "MTF encoder overflowed while filling to cap-1 (%s cap=%d)\n", cases[c].nm, cap); exit(1); }
        if (of_after != 1)  { fprintf(stderr, "MTF encoder FAILED to refuse the over-cap value (%s cap=%d) -- would emit a decoder-reject stream\n", cases[c].nm, cap); exit(1); }
        if (md_mtf(b.d, b.n, cap, m, GI)) fail_err(cases[c].nm, (unsigned)cap);
        for (int i = 0; i < m; i++) if ((int32_t)M64[i] != GI[i]) fail_val(cases[c].nm, (unsigned)cap, i, (long long)M64[i], (long long)GI[i]);
        buf_free(&b); g_streams++;
    }
}
static void test_mixed(void) {
    g_models++;
    for (int si = 0; si < NSEED; si++) for (int li = 0; li < NLEN; li++) {
        unsigned seed = SEEDS[si]; int ns = LENS[li];
        lcg_seed(seed ^ 0xA1A1u ^ ((unsigned)li << 20));
        fill_mixed(OPS, V32, ns);
        Buf b = enc_mixed(OPS, V32, ns);
        if (md_mixed(b.d, b.n, OPS, ns, G32)) fail_err("mixed", seed);
        CMP32("mixed", seed, V32, G32, ns);
        buf_free(&b); g_streams++;
    }
}

int main(void) {
    test_bits(); test_bt(); test_bv(); test_ugr(); test_ugg(); test_idx();
    test_flag(); test_rep0(); test_rawbits(); test_rawgz(); test_mtf(); test_mixed();
    printf("model_diff=%d models x %d streams OK\n", g_models, g_streams);
    return 0;
}
