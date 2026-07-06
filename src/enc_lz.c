/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 *
 * A1 host encoder module -- LZSS DP parse + entropy pricing (out_candidates, measure_prices, lz_parse_priced, lz_candidates_c).
 * Compiled as a normal internal encoder translation unit.
 */

#include "enc_internal.h"
/* ------------------------------------------------------------------------------------- */
/* LZSS planning and entropy models.                                                       */
/* ------------------------------------------------------------------------------------- */
static void tok_push(TokenVec *v, Token t) {
    v->v = (Token *)vec_reserve(v->v, &v->cap, v->n + 1, sizeof(v->v[0]), 1024);
    v->v[v->n++] = t;
}

uint64_t gammalen_u32(uint32_t x) { return (uint64_t)(2 * bitlen32(x) - 1); }

static int cand_value(Cand c) {
    int v = c.len * 16;
    for (int32_t d = c.dist; d > 1; d >>= 1) v--;
    return v;
}

static void cand_add(Cand cands[LZ_CAND_MAX], uint8_t *ncand, Cand c) {
    if (c.len < 3) return;
    for (uint8_t i = 0; i < *ncand; i++)
        if (cands[i].dist <= c.dist && cands[i].len >= c.len)
            return;
    uint8_t w = 0;
    for (uint8_t i = 0; i < *ncand; i++)
        if (!(c.dist <= cands[i].dist && c.len >= cands[i].len))
            cands[w++] = cands[i];
    *ncand = w;
    if (*ncand < LZ_CAND_MAX) {
        cands[(*ncand)++] = c;
        return;
    }
    int worst = 0, worstv = cand_value(cands[0]);
    for (int i = 1; i < LZ_CAND_MAX; i++) {
        int v = cand_value(cands[i]);
        if (v < worstv || (v == worstv && cands[i].dist > cands[worst].dist)) {
            worst = i;
            worstv = v;
        }
    }
    int cv = cand_value(c);
    if (cv > worstv || (cv == worstv && c.dist < cands[worst].dist)) cands[worst] = c;
}

/* ---- out-match candidates (D2): matches of the content stream against the ALREADY-PRODUCED
 * output image. olim[i] bounds the usable output window for a token starting at content position
 * i (conservative: fixed at the op that consumes position i, valid for the whole token):
 * FWD window is [0, olim[i]) — everything below the op's tp0 is written; grow window is
 * [olim[i], to_size) — everything above the op's output end is written. ---- */
static void oc_keep(OCand *row, uint8_t *nc, int32_t pos, int32_t len) {
    int w = -1;                                    /* keep the OC_MAX longest */
    for (int q = 0; q < *nc; q++) if (row[q].len < len) { w = q; break; }
    if (*nc < OC_MAX) { row[*nc].pos = pos; row[*nc].len = len; (*nc)++; }
    else if (w >= 0) { row[w].pos = pos; row[w].len = len; }
}

/* Direction-aware trigram hash chains over `src` (forward trigrams for FWD, reversed for grow —
 * grow content carries extras byte-REVERSED and the decoder replays in write direction). */
static void oc_index(const uint8_t *src, size_t src_n, int FWD, int32_t **head_out, int32_t **prev_out) {
    int32_t *head = (int32_t *)xmalloc((1u << 24) * sizeof(int32_t));
    for (size_t i = 0; i < (1u << 24); i++) head[i] = -1;
    int32_t *prev = (int32_t *)xmalloc((src_n ? src_n : 1) * sizeof(int32_t));
    if (FWD) {
        for (size_t i = 0; i + 3 < src_n; i++) {
            uint32_t key = (uint32_t)src[i] | ((uint32_t)src[i + 1] << 8) | ((uint32_t)src[i + 2] << 16);
            prev[i] = head[key]; head[key] = (int32_t)i;
        }
    } else {
        for (size_t i = 2; i < src_n; i++) {
            uint32_t key = (uint32_t)src[i] | ((uint32_t)src[i - 1] << 8) | ((uint32_t)src[i - 2] << 16);
            prev[i] = head[key]; head[key] = (int32_t)i;
        }
    }
    *head_out = head; *prev_out = prev;
}

/* Out-match candidates from BOTH decode-time flash states:
 *  NEW window (source `to`): FWD [0, olim[i]);      grow [olim[i], to_size)        — written output.
 *  OLD window (source `frm`): FWD [olim2[i], from_size); grow [0, olim2[i])        — pristine flash
 *    the frontier has not reached (out_read returns it verbatim). OLD regions DECAY as later ops
 *    write, so OLD candidates are clipped to finish inside the op that starts them (ocap[i]). */
void out_candidates(const uint8_t *content, size_t n, const uint32_t *olim,
                           const uint32_t *olim2, const uint32_t *ocap, int FWD,
                           const uint8_t *to, size_t to_n, const uint8_t *frm, size_t from_n,
                           OCand (**oc_out)[OC_MAX], uint8_t **noc_out) {
    OCand (*oc)[OC_MAX] = (OCand (*)[OC_MAX])xcalloc(n ? n : 1, sizeof(OCand[OC_MAX]));
    uint8_t *noc = (uint8_t *)xcalloc(n ? n : 1, 1);
    if (to_n >= 4 && n >= 4) {
        int32_t *head, *prev, *fhead, *fprev;
        oc_index(to, to_n, FWD, &head, &prev);
        oc_index(frm, from_n, FWD, &fhead, &fprev);
        for (size_t i = 0; i + 4 <= n; i++) {
            uint32_t key = (uint32_t)content[i] | ((uint32_t)content[i + 1] << 8) | ((uint32_t)content[i + 2] << 16);
            int visits = 0;
            uint32_t lim = olim[i];
            for (int32_t pj = head[key]; pj >= 0 && visits < 1024; pj = prev[pj], visits++) {
                size_t maxl;
                if (FWD) {
                    if ((uint32_t)pj + 4 > lim) continue;      /* too high: keep walking down */
                    maxl = (size_t)(lim - (uint32_t)pj);
                } else {
                    if ((uint32_t)pj < lim + 3u) break;        /* run [pj-l+1, pj] must stay >= lim */
                    maxl = (size_t)((uint32_t)pj - lim + 1u);
                }
                if (maxl > n - i) maxl = n - i;
                size_t l = 0;
                if (FWD) while (l < maxl && to[(size_t)pj + l] == content[i + l]) l++;
                else     while (l < maxl && to[(size_t)pj - l] == content[i + l]) l++;
                if (l >= 4) oc_keep(oc[i], &noc[i], pj, (int32_t)l);
            }
            uint32_t lim2 = olim2[i], cap = ocap[i];
            if (cap < 4) continue;
            visits = 0;
            for (int32_t pj = fhead[key]; pj >= 0 && visits < 1024; pj = fprev[pj], visits++) {
                size_t maxl;
                if (FWD) {
                    if ((uint32_t)pj < lim2) break;            /* below the OLD window: chain descends */
                    maxl = from_n - (size_t)pj;
                } else {
                    if ((uint32_t)pj + 1 > lim2) continue;     /* run [pj-l+1, pj] must stay < lim2 */
                    maxl = (size_t)pj + 1u;
                }
                if (maxl > n - i) maxl = n - i;
                if (maxl > cap) maxl = cap;                    /* OLD tokens end inside their op */
                size_t l = 0;
                if (FWD) while (l < maxl && frm[(size_t)pj + l] == content[i + l]) l++;
                else     while (l < maxl && frm[(size_t)pj - l] == content[i + l]) l++;
                if (l >= 4) oc_keep(oc[i], &noc[i], pj, (int32_t)l);
            }
        }
        free(head); free(prev); free(fhead); free(fprev);
    }
    *oc_out = oc; *noc_out = noc;
}

/* ---- price feedback: fractional bit-prices measured from the real adaptive models ----
 * The DP proxy (1-bit flag + gamma/rice bit-length + seeded-A1BitTree litbits) systematically
 * mis-prices tokens because the wire uses *adaptive* range-coder models whose steady-state
 * probabilities diverge from the time-0 literal-tree seed and from a flat 1-bit flag. We run a
 * trial encode of the previous-pass token stream, read off the resulting model probabilities,
 * and turn them into static per-symbol prices (PR_SCALE-ths of a bit) for the next DP pass.
 * Encoder-only: the wire bytes are unchanged; only token *selection* improves. */
/* cost in 1/64-bit units of coding a single adaptive bit with P(0)=p/4096. */
/* log2(1+i/32)*2^16, i=0..32, for the fractional part of the fixed-point log2. */
static const uint16_t LOG2_FRAC[33] = {
    0,2909,5732,8473,11136,13727,16248,18704,21098,23433,25711,27936,30109,
    32234,34312,36346,38336,40286,42196,44068,45904,47705,49472,51207,52911,
    54584,56229,57845,59434,60997,62534,64047,65535
};
uint32_t bit_price(uint32_t p, int bit) {
    /* -log2(prob) * PR_SCALE, prob = (bit? 4096-p : p)/4096 = pr/4096. */
    uint32_t pr = bit ? (RC_PBIT - p) : p;
    if (pr < 1) pr = 1;
    int e = bitlen32(pr) - 1;                  /* floor(log2(pr)), 0..11 */
    uint32_t intbits = (uint32_t)(12 - e) * PR_SCALE;
    uint32_t mant = pr << (16 - e);            /* pr*2^(16-e) in [2^16, 2^17) */
    uint32_t frac16 = mant - (1u << 16);       /* mantissa fraction, [0, 2^16) */
    uint32_t idx = frac16 >> 11;               /* table bucket 0..31 */
    uint32_t sub = frac16 & 2047;
    uint32_t l2 = LOG2_FRAC[idx] + ((LOG2_FRAC[idx + 1] - LOG2_FRAC[idx]) * sub >> 11); /* log2(1+f)*2^16 */
    uint32_t fracbits = (l2 * PR_SCALE) >> 16;
    return intbits - fracbits;                 /* (12 - log2(pr)) * PR_SCALE */
}

/* LIT0_CTX / LIT0_SEL / LIT0_MAP come from rc_models.h (shared, bit-exact wire). */

/* price a ug value under a frozen model snapshot (probabilities held constant across the value) */
static uint32_t ug_price(const UGE *g, uint32_t v) {
    uint32_t cl, cost = 0;
    if (g->code == 'r') {
        cl = v >> g->k;
        for (uint32_t pos = 0; pos < cl; pos++) cost += bit_price(g->u[ug_c((int)pos)], 1);
        cost += bit_price(g->u[ug_c((int)cl)], 0);
        for (int pos = 0; pos < g->k; pos++)  /* rice: LSB-anchored ctx, mirror ug_encode */
            cost += bit_price(g->m[ug_c((int)cl)][ug_c(g->k - 1 - pos)], (int)((v >> (g->k - 1 - pos)) & 1u));
    } else {
        uint32_t mm = v + 1u;
        cl = (uint32_t)bitlen32(mm) - 1u;
        for (uint32_t pos = 0; pos < cl; pos++) cost += bit_price(g->u[ug_c((int)pos)], 1);
        cost += bit_price(g->u[ug_c((int)cl)], 0);
        for (uint32_t pos = 0; pos < cl; pos++)
            cost += bit_price(g->m[ug_c((int)cl)][ug_c((int)pos)], (int)((mm >> (cl - 1u - pos)) & 1u));
    }
    return cost;
}

/* per-byte literal price under a frozen bit-tree snapshot */
static uint32_t bt_price(const A1BitTree *t, uint8_t byte) {
    int m = 1; uint32_t cost = 0;
    for (int i = 7; i >= 0; i--) {
        int bit = (byte >> i) & 1;
        cost += bit_price(a1_bt_get(t, m - 1), bit);
        m = (m << 1) | bit;
    }
    return cost;
}

/* Per-byte integer bit-length literal proxy from the wire's time-0 seeded literal BitTrees -- the
 * exact model the real emit uses at seed (lit_tree_seed_e), replacing the from-image static-Huffman
 * code that used to feed the pre-parse and the diff-run splitter. Context policy: tag0 bytes price
 * under the parity-0 seed tree, tag1 under parity-1; at seed every LIT0_SEL context shares one seed,
 * so no previous-byte context need be chosen here -- adaptation (and the real pricing refinement)
 * happen later in the price-feedback loop, which this proxy only bootstraps. Units: bt_price is
 * 1/64-bit fixed point (PR_SCALE); round to nearest integer bit (floor 1, like a Huffman length) so
 * the proxy mixes cleanly with the integer gamma/flag/distance bit-lengths in lz_parse_once and the
 * split_nonzero_diff_runs DP. Max seeded byte price is ~96 bits, well within uint8. */
void from_lit_proxy_bits(const uint8_t *frm, size_t n, uint8_t L0[256], uint8_t L1[256]) {
    A1BitTree t0, t1;
    lit_tree_seed_e(frm, n, 0, &t0);
    lit_tree_seed_e(frm, n, 1, &t1);
    for (int b = 0; b < 256; b++) {
        uint32_t p0 = (bt_price(&t0, (uint8_t)b) + PR_SCALE / 2) / PR_SCALE;
        uint32_t p1 = (bt_price(&t1, (uint8_t)b) + PR_SCALE / 2) / PR_SCALE;
        L0[b] = (uint8_t)(p0 < 1 ? 1 : (p0 > 255 ? 255 : p0));
        L1[b] = (uint8_t)(p1 < 1 ? 1 : (p1 > 255 ? 255 : p1));
    }
}

/* Simulate the real adaptive content models over a token sequence to obtain steady-state
 * probabilities and average flag prices; fill a PriceTab for the next DP pass. */
void measure_prices(const TokenVec *seq, const uint8_t *content, const uint8_t *tags,
                           const uint8_t *frm, size_t from_size, int dk, int ko, PriceTab *pt) {
    A1BitTree lit0[LIT0_CTX], lit1;        /* mirror the wire's order-1 tag0 trees + single tag1 tree */
    A1Flag1 flag;
    UGE gs, gl, gd;
    for (int c = 0; c < LIT0_CTX; c++) lit_tree_seed_e(frm, from_size, 0, &lit0[c]);
    lit_tree_seed_e(frm, from_size, 1, &lit1);
    a1_fl_init(&flag);
    ug_init_e(&gd, 'r', dk);
    ug_init_e(&gl, 'g', 0);
    ug_seed_cont_e(&gl, RC_SEED_DEPTH_GL);   /* mirror the wire: matches are len>=3, so M_gl's first unary bit is always continue */
    ug_init_e(&gs, 'g', 0);
    UGE go, glo;
    ug_init_e(&go, 'r', ko);
    ug_init_e(&glo, 'g', 0);
    uint16_t outb = RC_PHALF;
    uint64_t oy_cost = 0, on_cost = 0; uint32_t oy_n = 0, on_n = 0;
    uint32_t oexp = pt->oexp0;                    /* caller pre-sets oexp0 (FWD ? 0 : to_size) */
    uint64_t op_cost = 0; uint32_t op_n = 0;
    REnc r; re_init(&r);                 /* drives adaptation; emitted bytes discarded */
    /* token count (seq->n) is no longer on the wire (Feature 7A); the old raw count bits touched
     * no adaptive model, so dropping them leaves every price unchanged. */
    /* Mirror the rep0 last-distance flag so its price reflects real adaptation. */
    uint16_t rep0[2] = { RC_REP0_INIT, RC_REP0_INIT }; int rep0h = 0; int32_t last_dist = 0;
    uint64_t r0y_cost = 0, r0n_cost = 0; uint32_t r0y_n = 0, r0n_n = 0;
    uint8_t prevlit = 0;                 /* true previous content-stream byte (order-1 tag0 context) */
    int last_span = 0;                   /* mirror the wire: match flag implicit after a span */
    for (size_t i = 0; i < seq->n; i++) {
        Token t = seq->v[i];
        if (t.type == 'S') {
            fl_encode(&flag, &r, 0);
            last_span = 1;
            ug_encode(&gs, &r, (uint32_t)t.len - 1u);
            for (int32_t j = 0; j < t.len; j++) {
                size_t cp = (size_t)t.start + (size_t)j;
                uint8_t byte = content[cp];
                bt_encode(tags[cp] ? &lit1 : &lit0[LIT0_SEL(prevlit)], &r, byte, tags[cp] ? RC_LIT1_RATE : RC_LIT0_RATE);
                prevlit = byte;
            }
        } else if (t.type == 'O') {
            if (last_span) { flag.h = ((flag.h << 1) | 1) & 3; last_span = 0; }
            else fl_encode(&flag, &r, 1);
            r0n_cost += bit_price(rep0[rep0h], 0); r0n_n++;
            re_bit(&r, &rep0[rep0h], 0, RC_S_BIT_RATE); rep0h = 0;
            oy_cost += bit_price(outb, 1); oy_n++;
            re_bit(&r, &outb, 1, RC_S_BIT_RATE);
            { uint32_t zv = rc_outmatch_delta((uint32_t)t.dist, oexp);
              op_cost += ug_price(&go, zv); op_n++;
              ug_encode(&go, &r, zv);
              oexp = rc_outmatch_next_expect(pt->fwd, (uint32_t)t.dist, (uint32_t)t.len); }
            ug_encode(&glo, &r, (uint32_t)t.len - RC_OUTMATCH_MIN);
        } else {
            if (last_span) { flag.h = ((flag.h << 1) | 1) & 3; last_span = 0; }
            else fl_encode(&flag, &r, 1);
            if (t.dist == last_dist) {
                r0y_cost += bit_price(rep0[rep0h], 1); r0y_n++;
                re_bit(&r, &rep0[rep0h], 1, RC_S_BIT_RATE); rep0h = 1;
            } else {
                r0n_cost += bit_price(rep0[rep0h], 0); r0n_n++;
                re_bit(&r, &rep0[rep0h], 0, RC_S_BIT_RATE); rep0h = 0;
                on_cost += bit_price(outb, 0); on_n++;
                re_bit(&r, &outb, 0, RC_S_BIT_RATE);
                ug_encode(&gd, &r, (uint32_t)t.dist - 1u);
                last_dist = t.dist;
            }
            ug_encode(&gl, &r, (uint32_t)t.len - 1u);
        }
        /* order-1 context: prevlit = the last content byte this token produced (t.start is the
         * content position of every token kind; spans set it per-byte above to the same value). */
        prevlit = content[(size_t)t.start + (size_t)t.len - 1u];
    }
    buf_free(&r.out);
    /* Per-context flag price from the steady-state probabilities. The wire's token flag is an
     * order-2 model on the previous two token kinds (A1Flag1, 4 contexts); a scalar span/match
     * average would wash that out. Pricing each flag under its real context lets the rep0-aware
     * DP (which tracks a forward flag history) value match/span transitions accurately. */
    for (int h = 0; h < 4; h++) {
        pt->fspan_c[h]  = bit_price(flag.m[h], 0);
        pt->fmatch_c[h] = bit_price(flag.m[h], 1);
    }
    /* Fall back to the prior-implied price when a flavor was never used in this parse. */
    pt->rep0_yes = r0y_n ? (uint32_t)(r0y_cost / r0y_n) : bit_price(RC_REP0_INIT, 1);
    pt->rep0_no  = r0n_n ? (uint32_t)(r0n_cost / r0n_n) : bit_price(RC_REP0_INIT, 0);
    pt->outb_yes = oy_n ? (uint32_t)(oy_cost / oy_n) : bit_price(RC_PHALF, 1);
    pt->outb_no  = on_n ? (uint32_t)(on_cost / on_n) : bit_price(RC_PHALF, 0);
    /* DP position price: the measured average (the DP cannot track the expected-position state);
     * with no out tokens yet, an optimistic small-delta estimate lets phase 2 explore. */
    pt->opos_avg = op_n ? (uint32_t)(op_cost / op_n) : ug_price(&go, rc_zz32(256));
    pt->go = go; pt->glo = glo;
    for (int c = 0; c < LIT0_CTX; c++)
        for (int b = 0; b < 256; b++) pt->lit0[c][b] = bt_price(&lit0[c], (uint8_t)b);
    for (int b = 0; b < 256; b++) pt->lit1[b] = bt_price(&lit1, (uint8_t)b);
    pt->gs = gs; pt->gl = gl; pt->gd = gd; pt->dk = dk;
    pt->valid = 1;
}

static TokenVec lz_parse_once(size_t n, const uint16_t *litbits,
                              Cand (*cands)[LZ_CAND_MAX], uint8_t *ncand, int (*dist_bits)(int32_t, int),
                              int dk) {
    uint32_t maxrun = 1024;
    uint64_t *cost = (uint64_t *)xmalloc((n + 1) * sizeof(uint64_t));
    Token *nxt = (Token *)xcalloc(n + 1, sizeof(Token));
    /* litbits prefix sums + span ends restricted to candidate positions (same argument as
     * lz_parse_priced: spans ending in a literal desert fold into a longer span). */
    uint64_t *lsum = (uint64_t *)xmalloc((n + 1) * sizeof(uint64_t));
    lsum[0] = 0;
    for (size_t i = 0; i < n; i++) lsum[i + 1] = lsum[i] + litbits[i];
    size_t *ntok = (size_t *)xmalloc((n + 2) * sizeof(size_t));
    ntok[n] = n; ntok[n + 1] = n;
    for (size_t j = n; j-- > 0;) ntok[j] = ncand[j] ? j : ntok[j + 1];
    const uint64_t INF = UINT64_MAX / 4;
    cost[n] = 0;
    for (size_t ri = n; ri-- > 0;) {
        uint64_t best = INF;
        Token bt = {0};
        size_t lim = n < ri + maxrun ? n : ri + maxrun;
        size_t dense_end = ri + 8 < lim ? ri + 8 : lim;
        for (size_t j = ri + 1; j <= dense_end; j++) {
            uint64_t c = 1 + gammalen_u32((uint32_t)(j - ri)) + (lsum[j] - lsum[ri]) + cost[j];
            if (c < best) { best = c; bt = (Token){ 'S', (int32_t)ri, (int32_t)(j - ri), 0 }; }
        }
        for (size_t j = ntok[dense_end + 1]; j <= lim; j = ntok[j + 1]) {
            uint64_t c = 1 + gammalen_u32((uint32_t)(j - ri)) + (lsum[j] - lsum[ri]) + cost[j];
            if (c < best) { best = c; bt = (Token){ 'S', (int32_t)ri, (int32_t)(j - ri), 0 }; }
            if (j >= n) break;
        }
        for (int ci = 0; ci < ncand[ri]; ci++) {
            int32_t bd = cands[ri][ci].dist, bl = cands[ri][ci].len;
            uint64_t db = (uint64_t)dist_bits(bd, dk);
            for (int32_t l = 3; l <= bl; l++) {
                uint64_t c = 1 + db + gammalen_u32((uint32_t)l) + cost[ri + (size_t)l];
                if (c < best) { best = c; bt = (Token){ 'R', (int32_t)ri, l, bd }; }
            }
        }
        cost[ri] = best;
        nxt[ri] = bt;
    }
    TokenVec tv = {0};
    for (size_t i = 0; i < n;) {
        Token t = nxt[i];
        tok_push(&tv, t);
        i += (size_t)t.len;
    }
    free(cost); free(nxt); free(lsum); free(ntok);
    return tv;
}

/* DP parse using measured fractional prices (PR_SCALE-ths of a bit), made rep0-aware:
 * the wire lets a match REUSE the immediately-previous match distance for one cheap flag
 * bit (rep0) instead of re-coding the whole gd distance value. That is a forward dependency
 * (the price of a match depends on the distance chosen earlier), so this is a FORWARD DP
 * carrying, per reachable position, the cheapest arrival cost plus the rep distance in effect
 * there. The wire's token flag is also an order-2 model (A1Flag1, 4 contexts on the previous two
 * token kinds), so the flag history h=(prev2<<1|prev1) is part of the forward state too: we keep
 * one DP state per (position, h) and price each flag under its real context fspan_c[h]/fmatch_c[h]
 * instead of a washed-out scalar average. Cheapest-arrival-per-state keeps it O(n*4); the chosen
 * parse is only ever ACCEPTED by the exact full-body byte gate in encode_body, so any approximation
 * here can never corrupt the wire -- it only changes which legal parse is tried.
 * Length/dist value prices are precomputed once per pass (frozen model snapshot). */
/* Relax a candidate arrival (cost c, rep rr, token v, predecessor h) into the state
 * at index jb = j*4 + h'. Cheapest arrival only: a second distinct-rep arrival variant was
 * measured worth 0.08% corpus for ~25% of encode time and removed. (No prevlit is carried: the
 * tag0 literal context is now the deterministic content[p-1], fully baked into span_lit.) */
__attribute__((always_inline))
static inline void relax2(uint64_t *cost, int32_t *rep, Token *via, uint8_t *vh,
                          size_t jb, uint64_t c, int32_t rr, Token v, uint8_t hr) {
    if (c < cost[jb]) {
        cost[jb] = c; rep[jb] = rr; via[jb] = v; vh[jb] = hr;
    }
}

TokenVec lz_parse_priced(size_t n, const uint8_t *content, const uint8_t *tags,
                                Cand (*cands)[LZ_CAND_MAX], uint8_t *ncand,
                                OCand (*ocands)[OC_MAX], const uint8_t *nocand,
                                const PriceTab *pt) {
    uint32_t maxrun = 1024, win = 1u << PATHE_W;
    /* slen[L]=span len price (flag added per-context below); mlen[L]=match len price;
     * dpr[D]=fresh-distance value price. */
    size_t maxlen = n + 1;
    uint32_t *slen = (uint32_t *)xmalloc((maxlen + 1) * sizeof(uint32_t));
    uint32_t *mlen = (uint32_t *)xmalloc((maxlen + 1) * sizeof(uint32_t));
    uint32_t *dpr  = (uint32_t *)xmalloc(((size_t)win + 1) * sizeof(uint32_t));
    uint64_t *span_lit = (uint64_t *)xmalloc((n + 1) * sizeof(uint64_t));
    uint32_t *olen = (uint32_t *)xmalloc((maxlen + 1) * sizeof(uint32_t));
    for (size_t L = 1; L <= maxlen; L++) {
        slen[L] = ug_price(&pt->gs, (uint32_t)L - 1u);
        mlen[L] = ug_price(&pt->gl, (uint32_t)L - 1u);
        olen[L] = L >= RC_OUTMATCH_MIN ? ug_price(&pt->glo, (uint32_t)L - RC_OUTMATCH_MIN) : 0;
    }
    for (uint32_t D = 1; D <= win; D++) dpr[D] = ug_price(&pt->gd, D - 1u);
    /* Every literal's tag0 context is now the deterministic previous content byte content[p-1]
     * (order-1 wire: matches/backrefs/out-matches update prevlit too), so this prefix table gives
     * the EXACT literal cost of any span [i,j) as span_lit[j] - span_lit[i], first literal included
     * -- no per-state carried prevlit, no first-literal special case. */
    span_lit[0] = 0;
    for (size_t p = 0; p < n; p++) {
        uint8_t byte = content[p];
        uint32_t c = tags[p] ? pt->lit1[byte] : pt->lit0[LIT0_SEL(p ? content[p - 1] : 0)][byte];
        span_lit[p + 1] = span_lit[p] + c;
    }
    /* per-context match-flag extras: match flag + fresh-distance flag + value, vs reuse flag (rep0).
     * After a span (h&1 == 0: previous token kind bit is 0) the match flag is implicit on the
     * wire (adjacent spans never ship), so its price is zero in those contexts. */
    uint64_t fresh_extra[4], reuse_extra[4], out_extra[4];
    for (int h = 0; h < 4; h++) {
        uint64_t mflag = (h & 1) ? (uint64_t)pt->fmatch_c[h] : 0;
        fresh_extra[h] = mflag + pt->rep0_no + pt->outb_no;   /* fresh ring dist also pays outb=0 */
        reuse_extra[h] = mflag + pt->rep0_yes;
        out_extra[h]   = mflag + pt->rep0_no + pt->outb_yes;
    }
    const uint64_t INF = UINT64_MAX / 4;
    /* One state per (position, flag-history h): h = (prev2<<1)|prev1, span=0/match=1, keeping only
     * the cheapest arrival. State index s = i*4 + h; ns = (n+1)*4. Encoder-only; the chosen parse
     * is re-checked by the exact byte gate. */
    /* span ends only matter where a token can START (or at the content end): spans ending in a
     * literal desert fold into a longer span (merge_adjacent_spans keeps the wire identical, the
     * DP merely prices long spans as several). next_tok[j] = first j' >= j with candidates. */
    size_t *next_tok = (size_t *)xmalloc((n + 2) * sizeof(size_t));
    next_tok[n] = n; next_tok[n + 1] = n;
    for (size_t j = n; j-- > 0;) next_tok[j] = (ncand[j] || nocand[j]) ? j : next_tok[j + 1];
    size_t ns = (n + 1) * 4;   /* one state per (position, flag-history h): cheapest arrival */
    uint64_t *cost = (uint64_t *)xmalloc(ns * sizeof(uint64_t));
    int32_t  *rep  = (int32_t *)xmalloc(ns * sizeof(int32_t)); /* rep distance arriving at state */
    Token *via = (Token *)xcalloc(ns, sizeof(Token));         /* token that arrives at state */
    uint8_t *vh = (uint8_t *)xcalloc(ns, sizeof(uint8_t));    /* predecessor flag-history h for backtrack */
    for (size_t s = 0; s < ns; s++) { cost[s] = INF; rep[s] = 0; }
    cost[0] = 0; rep[0] = 0; /* pos 0, h=0: wire seeds last_dist=0, flag.h=0 */
    int ord_len[LZ_CAND_MAX]; int32_t ord_dist[LZ_CAND_MAX]; uint32_t ord_dpr[LZ_CAND_MAX];
    for (size_t i = 0; i < n; i++) {
        /* len-ascending candidate ranges with suffix-min distance price: for lengths in
         * (len[k-1], len[k]] the cheapest eligible candidate is the suffix argmin, so each
         * length is relaxed ONCE per state instead of once per candidate. Distance-reuse
         * (rep0) pricing is fully covered by the explicit rep probe below, so the candidate
         * loop prices fresh only. State-independent; computed once per position. */
        int nc = ncand[i];
        for (int k = 0; k < nc; k++) {
            int32_t lk = cands[i][k].len, dk2 = cands[i][k].dist;
            int at = k;
            while (at > 0 && ord_len[at - 1] > lk) { ord_len[at] = ord_len[at - 1]; ord_dist[at] = ord_dist[at - 1]; at--; }
            ord_len[at] = lk; ord_dist[at] = dk2;
        }
        for (int k = nc; k-- > 0;) {
            uint32_t dp2 = dpr[ord_dist[k]];
            if (k + 1 < nc && ord_dpr[k + 1] < dp2) { ord_dpr[k] = ord_dpr[k + 1]; ord_dist[k] = ord_dist[k + 1]; }
            else ord_dpr[k] = dp2;
        }
        int32_t probe_ri = -1; size_t probe_rl = 0;   /* rep-probe scan memo: states share ri */
        for (int hr = 0; hr < 4; hr++) {
            int h = hr;
            size_t si = i * 4 + (size_t)h;
            if (cost[si] >= INF) continue;       /* unreachable along any cheap path */
            uint64_t ci = cost[si]; int32_t ri = rep[si];
            /* spans: emit one span flag (kind 0) under context h, then the gamma length and the
             * tag0/tag1 literals. New flag history after a span: h' = (h<<1|0)&3. tag0 literals are
             * priced under the wire's order-1 prev-byte context (LIT0_SEL of content[p-1]); that
             * context is deterministic per position now, so the whole span cost is the exact prefix
             * difference span_lit[j] - span_lit[i] (first literal included). No carried prevlit. */
            int hs = (h << 1) & 3;               /* history after a span flag (bit 0) */
            uint64_t span_base = ci + pt->fspan_c[h];
            size_t lim = n < i + maxrun ? n : i + maxrun;
            size_t dense_end = i + 8 < lim ? i + 8 : lim;
            for (size_t j = i + 1; j <= dense_end; j++) {
                uint64_t c = span_base + (uint64_t)slen[j - i] + (span_lit[j] - span_lit[i]);
                size_t jb = j * 4 + (size_t)hs;
                relax2(cost, rep, via, vh, jb, c, ri,
                       (Token){ 'S', (int32_t)i, (int32_t)(j - i), 0 }, (uint8_t)hr);
            }
            for (size_t j = next_tok[dense_end + 1]; j <= lim; j = next_tok[j + 1]) {
                uint64_t c = span_base + (uint64_t)slen[j - i] + (span_lit[j] - span_lit[i]);
                size_t jb = j * 4 + (size_t)hs;
                relax2(cost, rep, via, vh, jb, c, ri,
                       (Token){ 'S', (int32_t)i, (int32_t)(j - i), 0 }, (uint8_t)hr);
                if (j >= n) break;
            }
            /* matches: emit one match flag (kind 1) under context h; rep0 reuse when the candidate
             * distance equals the incoming rep distance. New flag history after a match: h' = (h<<1|1)&3. */
            int hm = ((h << 1) | 1) & 3;
            { int32_t prevl = 2;
              uint64_t fbase = ci + fresh_extra[h];
              for (int k = 0; k < nc; k++) {
                int32_t lk = ord_len[k];
                if (lk <= prevl) continue;
                int32_t bd = ord_dist[k];
                uint64_t mbase = fbase + ord_dpr[k];
                for (int32_t l = prevl + 1; l <= lk; l++) {
                    size_t j = i + (size_t)l;
                    uint64_t c = mbase + mlen[l];
                    size_t jb = j * 4 + (size_t)hm;
                    relax2(cost, rep, via, vh, jb, c, bd,
                           (Token){ 'R', (int32_t)i, l, bd }, (uint8_t)hr);
                }
                prevl = lk;
              } }
            /* out-matches: fresh rep0 + out-bit + absolute output position + own length gamma.
             * The rep distance is carried through unchanged (out-matches do not set last_dist). */
            for (int cix = 0; cix < nocand[i]; cix++) {
                int32_t opos = ocands[i][cix].pos, olm = ocands[i][cix].len;
                uint64_t obase = ci + out_extra[h] + pt->opos_avg;
                (void)opos;
                for (int32_t l = 4; l <= olm; l++) {
                    size_t j = i + (size_t)l;
                    uint64_t c = obase + olen[l];
                    size_t jb = j * 4 + (size_t)hm;
                    relax2(cost, rep, via, vh, jb, c, ri,
                           (Token){ 'O', (int32_t)i, l, opos }, (uint8_t)hr);
                }
            }
            /* explicit rep0 (reuse-distance) probe. The Pareto candidate set can drop a match at
             * distance == ri (the incoming rep distance) because it is not on the (dist,len) frontier,
             * yet there it costs only the reuse flag (no fresh-distance value). Recover it directly from
             * content so the DP can extend the previous distance for one cheap flag bit (len>=3 like the
             * wire; the chosen parse is re-checked by encode_body's exact full-body byte gate). */
            if (ri > 0 && (size_t)ri <= i) {
                size_t rl;
                if (ri == probe_ri) rl = probe_rl;
                else {
                    size_t src = i - (size_t)ri;
                    size_t rlim = lim - i;         /* same maxrun cap as spans (lim from the span block) */
                    rl = 0;
                    while (rl < rlim && content[src + rl] == content[i + rl]) rl++;
                    probe_ri = ri; probe_rl = rl;
                }
                for (size_t l = 3; l <= rl; l++) {
                    size_t j = i + l;
                    uint64_t c = ci + reuse_extra[h] + mlen[l];
                    size_t jb = j * 4 + (size_t)hm;
                    relax2(cost, rep, via, vh, jb, c, ri,
                           (Token){ 'R', (int32_t)i, (int32_t)l, ri }, (uint8_t)hr);
                }
            }
        }
    }
    /* reconstruct backward from the cheapest terminal (h,r) state at position n. */
    int hrbest = 0; uint64_t cbest = INF;
    for (int hr = 0; hr < 4; hr++) {
        size_t s = n * 4 + (size_t)hr;
        if (cost[s] < cbest) { cbest = cost[s]; hrbest = hr; }
    }
    TokenVec tv = {0};
    size_t pos = n; int hr = hrbest;
    while (pos > 0) {
        size_t s = pos * 4 + (size_t)hr;
        tok_push(&tv, via[s]);
        pos = (size_t)via[s].start;
        hr = vh[s];
    }
    for (size_t a = 0, b = tv.n; a + 1 < b; a++, b--) { Token t = tv.v[a]; tv.v[a] = tv.v[b - 1]; tv.v[b - 1] = t; }
    free(cost); free(rep); free(via); free(vh); free(slen); free(mlen); free(olen); free(dpr); free(span_lit); free(next_tok);
    return tv;
}

void merge_adjacent_spans(TokenVec *tv) {
    size_t w = 0;
    for (size_t i = 0; i < tv->n; i++) {
        Token t = tv->v[i];
        if (w > 0 && t.type == 'S') {
            Token *p = &tv->v[w - 1];
            if (p->type == 'S' && p->start <= INT32_MAX - p->len &&
                p->start + p->len == t.start &&
                p->len <= INT32_MAX - t.len) {
                p->len += t.len;
                continue;
            }
        }
        tv->v[w++] = t;
    }
    tv->n = w;
}

static int fixed_dist_bits(int32_t d, int k) { (void)d; return k; }
static int rice_dist_bits(int32_t d, int k) { uint32_t v = (uint32_t)d - 1u; return (int)((v >> k) + 1u + (uint32_t)k); }

int fit_k_tokens(const TokenVec *tv) {
    int best = 0;
    uint64_t bestc = UINT64_MAX;
    for (int k = 0; k < 16; k++) {
        uint64_t c = 0;
        for (size_t i = 0; i < tv->n; i++) if (tv->v[i].type == 'R') {
            uint32_t v = (uint32_t)tv->v[i].dist - 1u;
            c += (v >> k) + 1u + (uint32_t)k;
        }
        if (c < bestc) { bestc = c; best = k; }
    }
    return best;
}

int fit_k_out(const TokenVec *tv, int cur, uint32_t oexp0, int fwd) {
    int best = cur, any = 0;
    uint64_t bestc = UINT64_MAX;
    for (int k = 0; k < 16; k++) {
        uint64_t c = 0;
        uint32_t exp = oexp0;
        for (size_t i = 0; i < tv->n; i++) if (tv->v[i].type == 'O') {
            any = 1;
            uint32_t v = rc_outmatch_delta((uint32_t)tv->v[i].dist, exp);
            exp = rc_outmatch_next_expect(fwd, (uint32_t)tv->v[i].dist, (uint32_t)tv->v[i].len);
            c += (v >> k) + 1u + (uint32_t)k;
        }
        if (c < bestc) { bestc = c; best = k; }
    }
    return any ? best : cur;
}

/* Build the LZ match-candidate set (hash-chain over 3-byte keys, full chain within the window)
 * and an initial rice-DP parse. The caller owns *cands_out / *ncand_out and frees them after the
 * price-feedback loop (which it runs with its own, full-body acceptance gate). */
TokenVec lz_candidates_c(const uint8_t *data, size_t n, const uint16_t *litbits,
                                int *k_out,
                                Cand (**cands_out)[LZ_CAND_MAX], uint8_t **ncand_out) {
    int32_t win = 1 << PATHE_W, maxm = 2048;
    Cand (*cands)[LZ_CAND_MAX] = (Cand (*)[LZ_CAND_MAX])xcalloc(n ? n : 1, sizeof(Cand[LZ_CAND_MAX]));
    uint8_t *ncand = (uint8_t *)xcalloc(n ? n : 1, 1);
    int32_t *head = (int32_t *)xmalloc((1u << 24) * sizeof(int32_t));
    for (size_t i = 0; i < (1u << 24); i++) head[i] = -1;
    int32_t *prev = (int32_t *)xmalloc((n ? n : 1) * sizeof(int32_t));
    for (size_t i = 0; i < n; i++) prev[i] = -1;
    for (size_t i = 0; i < n; i++) {
        if (i + 3 <= n) {
            uint32_t key = (uint32_t)data[i] | ((uint32_t)data[i+1] << 8) | ((uint32_t)data[i+2] << 16);
            int32_t ml = (int32_t)((n - i) < (size_t)maxm ? (n - i) : (size_t)maxm);
            int32_t lbest = 0;   /* longest len RETAINED at i (only lens >= 3 reach cands[i]) */
            for (int32_t pj = head[key]; pj >= 0; pj = prev[pj]) {
                int32_t dist = (int32_t)i - pj;
                if (dist > win) break;
                /* EXACT chain pruning — the candidate set is provably unchanged, because the
                 * chain is walked in ascending dist order and cand_add drops any candidate
                 * dominated by a retained one (dist <=, len >=):
                 *   (a) probe data[.+lbest] first: a mismatch there bounds this entry at
                 *       l <= lbest, dominated by the retained lbest candidate — skip the
                 *       O(ml) extension. Sound while no capacity eviction has occurred
                 *       (ncand < LZ_CAND_MAX: eviction could remove the dominator).
                 *   (b) an entry that reaches the position cap ml dominates every later
                 *       entry — stop the walk. This kills the constant-data blowup
                 *       (dist=1 hits ml immediately: walk length 1, was ~window x ml). */
                if (lbest > 0 && ncand[i] < LZ_CAND_MAX &&
                    data[(size_t)pj + (size_t)lbest] != data[i + (size_t)lbest])
                    continue;
                int32_t l = 0;
                while (l < ml && data[(size_t)pj + (size_t)l] == data[i + (size_t)l]) l++;
                cand_add(cands[i], &ncand[i], (Cand){ dist, l });
                if (l >= 3 && l > lbest) lbest = l;
                if (l == ml) break;
            }
            prev[i] = head[key];
            head[key] = (int32_t)i;
        }
    }
    free(head); free(prev);
    TokenVec seq = lz_parse_once(n, litbits, cands, ncand, fixed_dist_bits, PATHE_W);
    int k = fit_k_tokens(&seq);
    int parsed_k = -1;
    for (int pass = 0; pass < 8; pass++) {
        free(seq.v);
        seq = lz_parse_once(n, litbits, cands, ncand, rice_dist_bits, k);
        parsed_k = k;
        int nk = fit_k_tokens(&seq);
        if (nk == k) break;
        k = nk;
    }
    if (parsed_k != k) {
        free(seq.v);
        seq = lz_parse_once(n, litbits, cands, ncand, rice_dist_bits, k);
    }
    *k_out = k;
    *cands_out = cands;
    *ncand_out = ncand;
    return seq;
}
