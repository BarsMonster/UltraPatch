/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 */

/* Decoder side of the model-level differential test. Includes the REAL device decoder
 * (src/patch_apply.h) UNCHANGED and exposes per-model whole-stream decode bridges that the
 * encoder-driving TU (model_diff.c) calls. The range coder + entropy models read ONLY the
 * byte source (never flash), so the two flash primitives patch_apply.h requires are trivial
 * stubs here. */
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/* patch_apply.h declares these extern; the model layer never dereferences flash. */
uint8_t flash_read(uint32_t a) { (void)a; return 0xFFu; }
void    flash_write(uint32_t a, uint8_t v) { (void)a; (void)v; }

#include "patch_apply.h"
#include "model_diff.h"

/* ---- byte-source priming ------------------------------------------------------------------
 * rc_init() and next_byte() pull through patch_apply.h's 4-byte withhold ring (g_tail): the ring
 * holds the 4 most-recent pulled bytes and serves bytes with a 4-byte delay, so the last 4 bytes
 * of the pull source are NEVER handed to the range coder (on the real wire those 4 are the
 * CRC32(to) trailer). To feed the range coder the WHOLE encoder body we therefore pull
 * body ++ 4 dummy bytes: the body is served in full and the 4 dummies stay withheld. Reads past
 * the body zero-fill (raw_next latches EOF) — exactly matching the encoder's re_flush_opt, which
 * strips trailing zero bytes. This mirrors how patch_selfcheck.c / patch_apply_run prime the ring. */
static uint8_t *md_src;
static size_t   md_cap;
typedef struct { const uint8_t *d; size_t n, i; } MdPull;
static MdPull md_ctx;
static int md_pull(void *c, uint8_t *out) {
    MdPull *p = (MdPull *)c;
    if (p->i >= p->n) return 0;
    *out = p->d[p->i++];
    return 1;
}
static void md_begin(const uint8_t *body, size_t n) {
    if (n + 4u > md_cap) { md_cap = n + 4u; md_src = (uint8_t *)realloc(md_src, md_cap); }
    if (n) memcpy(md_src, body, n);
    md_src[n] = md_src[n + 1] = md_src[n + 2] = md_src[n + 3] = 0u;  /* 4 dummy trailer bytes */
    md_ctx.d = md_src; md_ctx.n = n + 4u; md_ctx.i = 0;
    g_pull_fn = md_pull; g_pull_ctx = &md_ctx; g_pull_eof = 0;
    g_tailn = 0; g_tailw = 0;
    g_rcerr = 0; g_reject = REJ_NONE;
    rc_init();
}

/* ---- per-model whole-stream decoders (mirror decode_body's init of each model exactly) ---- */
int md_bits(const uint8_t *b, size_t n, int rate, int ns, uint8_t *got) {
    md_begin(b, n);
    uint16_t p = RC_PHALF;
    for (int i = 0; i < ns; i++) got[i] = (uint8_t)s_bit_r(&p, rate);
    return g_rcerr;
}
int md_bt(const uint8_t *b, size_t n, int rate, int ns, uint32_t *got) {
    md_begin(b, n);
    BitTree t; bt_init(&t);
    for (int i = 0; i < ns; i++) got[i] = (uint32_t)(uint8_t)s_bt(&t, rate);
    return g_rcerr;
}
int md_bv(const uint8_t *b, size_t n, int ns, int32_t *got) {
    md_begin(b, n);
    BitTree t; bt_init(&t);
    for (int i = 0; i < ns; i++) got[i] = s_bv(&t, 4);
    return g_rcerr;
}
int md_ugr(const uint8_t *b, size_t n, int k, int ns, uint32_t *got) {
    md_begin(b, n);
    UGRice g; ugr_init(&g, k);
    for (int i = 0; i < ns && !g_rcerr; i++) got[i] = s_ug_rice(&g);
    return g_rcerr;
}
int md_ugg(const uint8_t *b, size_t n, int depth, int ns, uint32_t *got) {
    md_begin(b, n);
    UGGamma g; ugg_init(&g); if (depth > 0) ugg_seed_cont(&g, depth);
    for (int i = 0; i < ns && !g_rcerr; i++) got[i] = s_ug_gamma(&g);
    return g_rcerr;
}
int md_idx(const uint8_t *b, size_t n, int ns, uint32_t *got) {
    md_begin(b, n);
    IdxUnary g; idx_init(&g, RC_IDX_SEED);
    /* mirror pull_delta's index read: clamp min(pos,IDX_CTX-1), generous run cap */
    for (int i = 0; i < ns && !g_rcerr; i++) got[i] = s_unary(g.u, IDX_CTX - 1u, 1u << 20);
    return g_rcerr;
}
int md_flag(const uint8_t *b, size_t n, int ns, uint8_t *got) {
    md_begin(b, n);
    Flag1 f; fl_init(&f);
    for (int i = 0; i < ns; i++) got[i] = (uint8_t)s_flag(&f);
    return g_rcerr;
}
int md_rep0(const uint8_t *b, size_t n, int ns, uint8_t *got) {
    md_begin(b, n);
    uint16_t rep0[2]; rep0[0] = rep0[1] = RC_REP0_INIT; int h = 0;   /* order-1 on previous rep0 outcome */
    for (int i = 0; i < ns; i++) { int bit = s_bit(&rep0[h]); h = bit; got[i] = (uint8_t)bit; }
    return g_rcerr;
}
int md_rawbits(const uint8_t *b, size_t n, int nb, int ns, uint32_t *got) {
    md_begin(b, n);
    for (int i = 0; i < ns; i++) got[i] = s_raw_bits(nb);
    return g_rcerr;
}
int md_rawgz(const uint8_t *b, size_t n, int ns, uint32_t *got) {
    md_begin(b, n);
    for (int i = 0; i < ns && !g_rcerr; i++) got[i] = s_raw_gz();
    return g_rcerr;
}
/* MTF dict stream: sized to the larger cap; pull_delta escapes ride the shared global M_dval. */
static int32_t md_dic[DR_KCAP_BL > DR_KCAP_EX ? DR_KCAP_BL : DR_KCAP_EX];
int md_mtf(const uint8_t *b, size_t n, int cap, int ns, int32_t *got) {
    md_begin(b, n);
    bt_init(&M_dval);
    DRStream d; dr_init(&d, md_dic, DR_HIT_INIT);
    IdxUnary gix; idx_init(&gix, RC_IDX_SEED);
    for (int i = 0; i < ns && !g_rcerr; i++) got[i] = pull_delta(&d, &gix, md_dic, (uint32_t)cap);
    return g_rcerr;
}
int md_mixed(const uint8_t *b, size_t n, const uint8_t *ops, int ns, uint32_t *got) {
    md_begin(b, n);
    uint16_t p = RC_PHALF;
    BitTree bt; bt_init(&bt);
    UGRice gr; ugr_init(&gr, MX_UGR_K);
    UGGamma gg; ugg_init(&gg);
    Flag1 f; fl_init(&f);
    for (int i = 0; i < ns && !g_rcerr; i++) {
        switch (ops[i]) {
            case MX_RAW:  got[i] = (uint32_t)s_raw();                   break;
            case MX_BIT:  got[i] = (uint32_t)s_bit_r(&p, MX_BIT_RATE);  break;
            case MX_BT:   got[i] = (uint32_t)(uint8_t)s_bt(&bt, MX_BT_RATE); break;
            case MX_UGR:  got[i] = s_ug_rice(&gr);                      break;
            case MX_UGG:  got[i] = s_ug_gamma(&gg);                     break;
            case MX_FLAG: got[i] = (uint32_t)s_flag(&f);                break;
            default:      got[i] = 0;                                   break;
        }
    }
    return g_rcerr;
}
