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

enum { HASH3_SIZE = 1u << 24 };
_Static_assert((int32_t)~(uint32_t)0 == -1, "hash sentinel uses all-bits-one int32_t");

static int32_t *hash3_heads_new(void) {
    int32_t *head = (int32_t *)xmalloc(HASH3_SIZE * sizeof(*head));
    memset(head, 0xff, HASH3_SIZE * sizeof(*head));
    return head;
}

static int32_t *hash3_prev_new(size_t n) {
    int32_t *prev = (int32_t *)xmalloc((n ? n : 1) * sizeof(*prev));
    memset(prev, 0xff, n * sizeof(*prev));
    return prev;
}

static uint32_t hash3_key_fwd(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

static uint32_t hash3_key_rev(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[-1] << 8) | ((uint32_t)p[-2] << 16);
}

/* ---- out-match candidates (D2): matches of the content stream against the ALREADY-PRODUCED
 * output image. Each token inherits a conservative window from the op that consumes its first
 * content byte: FWD [0, tp0), grow [tp_end, to_size). ---- */
static void oc_keep(OCand *row, uint8_t *nc, int32_t pos, int32_t len) {
    int w = -1;                                   /* legacy first-shorter replacement, not strict top-K */
    for (int q = 0; q < *nc; q++) if (row[q].len < len) { w = q; break; }
    if (*nc < OC_MAX) { row[*nc].pos = pos; row[*nc].len = len; (*nc)++; }
    else if (w >= 0) { row[w].pos = pos; row[w].len = len; }
}

static void oc_match(OCand *row, uint8_t *nc, const uint8_t *src, int FWD,
                     int32_t pj, const uint8_t *content, size_t i, size_t maxl) {
    size_t l = 0, p = (size_t)pj;
    if (FWD) while (l < maxl && src[p + l] == content[i + l]) l++;
    else     while (l < maxl && src[p - l] == content[i + l]) l++;
    if (l >= RC_OUTMATCH_MIN) oc_keep(row, nc, pj, (int32_t)l);
}

/* Direction-aware trigram hash chains over `src` (forward trigrams for FWD, reversed for grow —
 * grow content carries extras byte-REVERSED and the decoder replays in write direction). */
static void oc_index(const uint8_t *src, size_t src_n, int FWD, int32_t **head_out, int32_t **prev_out) {
    int32_t *head = hash3_heads_new();
    int32_t *prev = hash3_prev_new(src_n);
    if (FWD) {
        for (size_t i = 0; i + 3 < src_n; i++) {
            uint32_t key = hash3_key_fwd(src + i);
            prev[i] = head[key]; head[key] = (int32_t)i;
        }
    } else {
        for (size_t i = 2; i < src_n; i++) {
            uint32_t key = hash3_key_rev(src + i);
            prev[i] = head[key]; head[key] = (int32_t)i;
        }
    }
    *head_out = head; *prev_out = prev;
}

/* Out-match candidates from BOTH decode-time flash states:
 *  NEW window (source `to`): FWD [0, tp0);          grow [tp_end, to_size)         — written output.
 *  OLD window (source `frm`): FWD [tp_end, from_size); grow [0, tp0)               — pristine flash
 *    the frontier has not reached (out_read returns it verbatim). OLD regions DECAY as later ops
 *    write, so OLD candidates are clipped to finish inside the op that starts them. */
void out_candidates(const uint8_t *content, size_t n, const OpVec *ops,
                           const OpWalkEnt *walk, const OpEmitRow *rows, int FWD,
                           const uint8_t *to, size_t to_n, const uint8_t *frm, size_t from_n,
                           OCandArena *oc_out, uint8_t **noc_out) {
    OCandArena oc = {0};
    uint8_t *noc = (uint8_t *)xcalloc(n ? n : 1, 1);
    if (to_n >= 4 && n >= 4) {
        int32_t *head, *prev, *fhead, *fprev;
        oc_index(to, to_n, FWD, &head, &prev);
        oc_index(frm, from_n, FWD, &fhead, &fprev);
        size_t step = 0;
        for (size_t i = 0; i + 4 <= n; i++) {
            while (i >= rows[step].content_end) step++;
            const OpWalkEnt *we = &walk[opwalk_apply_index(ops->n, FWD, step)];
            uint32_t tp0 = (uint32_t)we->tp;
            uint32_t tpe = tp0 + (uint32_t)we->o->diff_len + (uint32_t)we->o->extra_len;
            uint32_t lim = FWD ? tp0 : tpe;
            uint32_t lim2 = FWD ? tpe : (tp0 < from_n ? tp0 : (uint32_t)from_n);
            uint32_t cap = (uint32_t)(rows[step].content_end - i);
            OCand row[OC_MAX];
            uint8_t nr = 0;
            uint32_t key = hash3_key_fwd(content + i);
            int visits = 0;
            for (int32_t pj = head[key]; pj >= 0 && visits < 1024; pj = prev[pj], visits++) {
                size_t maxl;
                if (FWD) {
                    if ((uint32_t)pj + RC_OUTMATCH_MIN > lim) continue; /* too high: keep walking down */
                    maxl = (size_t)(lim - (uint32_t)pj);
                } else {
                    if ((uint32_t)pj < lim + RC_OUTMATCH_MIN - 1u) break; /* run [pj-l+1, pj] must stay >= lim */
                    maxl = (size_t)((uint32_t)pj - lim + 1u);
                }
                if (maxl > n - i) maxl = n - i;
                oc_match(row, &nr, to, FWD, pj, content, i, maxl);
            }
            if (cap >= RC_OUTMATCH_MIN) {
                visits = 0;
                for (int32_t pj = fhead[key]; pj >= 0 && visits < 1024; pj = fprev[pj], visits++) {
                    size_t maxl;
                    if (FWD) {
                        if ((uint32_t)pj < lim2) break;        /* below the OLD window: chain descends */
                        maxl = from_n - (size_t)pj;
                    } else {
                        if ((uint32_t)pj + 1u > lim2) continue; /* run [pj-l+1, pj] must stay < lim2 */
                        maxl = (size_t)pj + 1u;
                    }
                    if (maxl > n - i) maxl = n - i;
                    if (maxl > cap) maxl = cap;                /* OLD tokens end inside their op */
                    oc_match(row, &nr, frm, FWD, pj, content, i, maxl);
                }
            }
            noc[i] = nr;
            if (nr) buf_write(&oc, row, (size_t)nr * sizeof(*row));
        }
        free(head); free(prev); free(fhead); free(fprev);
    }
    *oc_out = oc; *noc_out = noc;
}

/* ---- price feedback: fractional bit-prices measured from the real adaptive models ----
 * The DP proxy (1-bit flag + gamma/rice bit-length + seeded-up_BitTree litbits) systematically
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

/* UP_LIT0_CTX / rc_lit0_sel / LIT0_MAP come from rc_models.h (shared, bit-exact wire). */

void content_cursor_init(ContentCursor *cc, const TokenVec *seq,
                         const uint8_t *content, const uint8_t *tags, size_t content_n,
                         Models *m, REnc *rc, int fwd, int out_en, uint32_t oexp) {
    *cc = (ContentCursor){
        .seq = seq, .content = content, .tags = tags, .content_n = content_n,
        .M = m, .rc = rc, .fwd = fwd, .out_en = out_en, .oexp = oexp,
    };
}

static void content_cursor_start_token(ContentCursor *cc, ContentStats *stats) {
    Models *M = cc->M;
    REnc *rc = cc->rc;
    if (cc->tok_i >= cc->seq->n) die("content token underrun");
    cc->cur = cc->seq->v[cc->tok_i++];
    if (cc->cur.type == 'S') {
        if (cc->last_span) die("adjacent span tokens on wire");
        fl_encode(&M->tok.flag, rc, 0);
        ugg_encode(&M->tok.gs, rc, (uint32_t)cc->cur.len - 1u);
        cc->tok_mode = 'S';
        cc->tok_left = cc->cur.len;
        cc->span_pos = 0;
        cc->last_span = 1;
    } else if (cc->cur.type == 'O') {
        if (cc->last_span) { M->tok.flag.h = rc_fl_hist(M->tok.flag.h, 1); cc->last_span = 0; }
        else fl_encode(&M->tok.flag, rc, 1);
        if (stats) { stats->r0n_cost += bit_price(M->tok.rep0[M->rep0h], 0); stats->r0n_n++; }
        re_bit(rc, &M->tok.rep0[M->rep0h], 0, RC_S_BIT_RATE);
        M->rep0h = 0;
        if (stats) { stats->oy_cost += bit_price(M->tok.outb, 1); stats->oy_n++; }
        re_bit(rc, &M->tok.outb, 1, RC_S_BIT_RATE);
        { uint32_t zv = rc_outmatch_delta((uint32_t)cc->cur.dist, cc->oexp);
          if (stats) { stats->op_cost += ugr_price(&M->tok.go, zv); stats->op_n++; }
          ugr_encode(&M->tok.go, rc, zv);
          cc->oexp = rc_outmatch_next_expect(cc->fwd, (uint32_t)cc->cur.dist, (uint32_t)cc->cur.len); }
        ugg_encode(&M->tok.glo, rc, (uint32_t)cc->cur.len - RC_OUTMATCH_MIN);
        cc->tok_mode = 'R';
        cc->tok_left = cc->cur.len;
    } else {
        if (cc->last_span) { M->tok.flag.h = rc_fl_hist(M->tok.flag.h, 1); cc->last_span = 0; }
        else fl_encode(&M->tok.flag, rc, 1);
        if (cc->cur.dist == M->last_dist) {
            if (stats) { stats->r0y_cost += bit_price(M->tok.rep0[M->rep0h], 1); stats->r0y_n++; }
            re_bit(rc, &M->tok.rep0[M->rep0h], 1, RC_S_BIT_RATE);
            M->rep0h = 1;
        } else {
            if (stats) { stats->r0n_cost += bit_price(M->tok.rep0[M->rep0h], 0); stats->r0n_n++; }
            re_bit(rc, &M->tok.rep0[M->rep0h], 0, RC_S_BIT_RATE);
            M->rep0h = 0;
            if (cc->out_en) {
                if (stats) { stats->on_cost += bit_price(M->tok.outb, 0); stats->on_n++; }
                re_bit(rc, &M->tok.outb, 0, RC_S_BIT_RATE);
            }
            ugr_encode(&M->tok.gd, rc, (uint32_t)cc->cur.dist - 1u);
            M->last_dist = cc->cur.dist;
        }
        ugg_encode(&M->tok.gl, rc, (uint32_t)cc->cur.len - 1u);
        cc->tok_mode = 'R';
        cc->tok_left = cc->cur.len;
    }
}

void content_cursor_to(ContentCursor *cc, size_t end, ContentStats *stats) {
    if (end < cc->pos || end > cc->content_n) die("invalid content cursor");
    while (cc->pos < end) {
        if (!cc->tok_mode) content_cursor_start_token(cc, stats);
        size_t nn = (size_t)cc->tok_left < (end - cc->pos) ? (size_t)cc->tok_left : (end - cc->pos);
        if (cc->tok_mode == 'S') {
            for (size_t i = 0; i < nn; i++) {
                uint8_t byte = cc->content[(size_t)cc->cur.start + cc->span_pos + i];
                int tag = cc->tags[cc->pos];
                bt_encode(tag ? &cc->M->lit1 : &cc->M->lit0[rc_lit0_sel(cc->prevlit)],
                          cc->rc, byte, tag ? RC_LIT1_RATE : RC_LIT0_RATE);
                cc->prevlit = byte;
                cc->pos++;
            }
            cc->span_pos += nn;
        } else {
            cc->pos += nn;
            if (nn) cc->prevlit = cc->content[cc->pos - 1u];
        }
        cc->tok_left -= (int32_t)nn;
        if (cc->tok_left == 0) cc->tok_mode = 0;
    }
}

/* Per-byte integer bit-length literal proxy from the wire's time-0 seeded literal BitTrees -- the
 * exact model the real emit uses at seed (lit_tree_seed_e), replacing the from-image static-Huffman
 * code that used to feed the pre-parse and the diff-run splitter. Context policy: tag0 bytes price
 * under the parity-0 seed tree, tag1 under parity-1; at seed every rc_lit0_sel context shares one seed,
 * so no previous-byte context need be chosen here -- adaptation (and the real pricing refinement)
 * happen later in the price-feedback loop, which this proxy only bootstraps. Units: bt_price is
 * 1/64-bit fixed point (PR_SCALE); round to nearest integer bit (floor 1, like a Huffman length) so
 * the proxy mixes cleanly with the integer gamma/flag/distance bit-lengths in the bootstrap parse
 * and the split_nonzero_diff_runs DP. Max seeded byte price is ~96 bits, well within uint8. */
void from_lit_proxy_bits(const LitSeedTrees *seeds, uint8_t L0[256], uint8_t L1[256]) {
    for (int b = 0; b < 256; b++) {
        uint32_t p0 = (bt_price_static(&seeds->lit0, (uint8_t)b) + PR_SCALE / 2) / PR_SCALE;
        uint32_t p1 = (bt_price_static(&seeds->lit1, (uint8_t)b) + PR_SCALE / 2) / PR_SCALE;
        L0[b] = (uint8_t)(p0 < 1 ? 1 : (p0 > 255 ? 255 : p0));
        L1[b] = (uint8_t)(p1 < 1 ? 1 : (p1 > 255 ? 255 : p1));
    }
}

/* Simulate the real adaptive content models over a token sequence to obtain steady-state
 * probabilities and average flag prices; fill a PriceTab for the next DP pass. */
void measure_prices(const TokenVec *seq, const uint8_t *content, const uint8_t *tags,
                    const LitSeedTrees *seeds, int dk, int ko, PriceTab *pt) {
    Models M;
    memset(&M, 0, sizeof(M));
    models_init_content(&M, seeds, dk, ko);
    REnc r; re_init_count(&r);           /* drives adaptation; emitted bytes counted, not stored */
    /* token count (seq->n) is no longer on the wire (Feature 7A); the old raw count bits touched
     * no adaptive model, so dropping them leaves every price unchanged. */
    ContentStats st = {0};
    ContentCursor cc;
    content_cursor_init(&cc, seq, content, tags, seq->n ? (size_t)seq->v[seq->n - 1u].start + (size_t)seq->v[seq->n - 1u].len : 0,
                        &M, &r, pt->fwd, pt->out_en, pt->oexp0);
    content_cursor_to(&cc, cc.content_n, &st);
    /* Per-context flag price from the steady-state probabilities. The wire's token flag is an
     * order-2 model on the previous two token kinds (up_Flag1, 4 contexts); a scalar span/match
     * average would wash that out. Pricing each flag under its real context lets the rep0-aware
     * DP (which tracks a forward flag history) value match/span transitions accurately. */
    for (int h = 0; h < 4; h++) {
        pt->fspan_c[h]  = bit_price(M.tok.flag.m[h], 0);
        pt->fmatch_c[h] = bit_price(M.tok.flag.m[h], 1);
    }
    /* Fall back to the prior-implied price when a flavor was never used in this parse. */
    pt->rep0_yes = st.r0y_n ? (uint32_t)(st.r0y_cost / st.r0y_n) : bit_price(RC_REP0_INIT, 1);
    pt->rep0_no  = st.r0n_n ? (uint32_t)(st.r0n_cost / st.r0n_n) : bit_price(RC_REP0_INIT, 0);
    pt->outb_yes = st.oy_n ? (uint32_t)(st.oy_cost / st.oy_n) : bit_price(RC_PHALF, 1);
    pt->outb_no  = st.on_n ? (uint32_t)(st.on_cost / st.on_n) : bit_price(RC_PHALF, 0);
    /* DP position price: the measured average (the DP cannot track the expected-position state);
     * with no out tokens yet, an optimistic small-delta estimate lets phase 2 explore. */
    pt->opos_avg = st.op_n ? (uint32_t)(st.op_cost / st.op_n) : ugr_price(&M.tok.go, rc_zz32(256));
    pt->glo = M.tok.glo;
    for (int c = 0; c < UP_LIT0_CTX; c++)
        for (int b = 0; b < 256; b++) pt->lit0[c][b] = (uint16_t)bt_price_static(&M.lit0[c], (uint8_t)b);
    for (int b = 0; b < 256; b++) pt->lit1[b] = (uint16_t)bt_price_static(&M.lit1, (uint8_t)b);
    pt->gs = M.tok.gs; pt->gl = M.tok.gl; pt->gd = M.tok.gd;
    pt->fixed_dist_bits = -1;
    pt->bootstrap_simple = 0;
}

/* DP parse using measured fractional prices (PR_SCALE-ths of a bit), made rep0-aware:
 * the wire lets a match REUSE the immediately-previous match distance for one cheap flag
 * bit (rep0) instead of re-coding the whole gd distance value. That is a forward dependency
 * (the price of a match depends on the distance chosen earlier), so this is a FORWARD DP
 * carrying, per reachable position, the cheapest arrival cost plus the rep distance in effect
 * there. The wire's token flag is also an order-2 model (up_Flag1, 4 contexts on the previous two
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

static uint64_t *span_lit_prefix(size_t n, const uint8_t *content, const uint8_t *tags,
                                 const PriceTab *pt) {
    uint64_t *sum = (uint64_t *)xmalloc((n + 1) * sizeof(*sum));
    sum[0] = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t byte = content[i];
        uint32_t c = tags[i] ? pt->lit1[byte] : pt->lit0[rc_lit0_sel(i ? content[i - 1] : 0u)][byte];
        sum[i + 1] = sum[i] + c;
    }
    return sum;
}

typedef struct {
    size_t lo, hi, off, cap, head, count;
    uint32_t price;
} SpanMinQ;

#ifdef OUT_ENVELOPE_PROBE
uint64_t out_envelope_probe_last_cost;
#endif

TokenVec lz_parse_priced(size_t n, const uint8_t *content, const uint8_t *tags,
                                const CandArena *cands, const uint8_t *ncand,
                                const OCandArena *ocands, const uint8_t *nocand,
                                const PriceTab *pt) {
    uint32_t maxrun = LZ_MAX_RUN, win = 1u << WINDOW_LOG;
    /* slen[L]=span len price (flag added per-context below); mlen[L]=match len price;
     * dpr[D]=fresh-distance value price. slen is only ever indexed by a span/rep run length
     * (<= maxrun == LZ_MAX_RUN) and mlen by a match length (candidate lens clamp to LZ_MAX_MATCH
     * in lz_candidates_c; rep-probe runs are <= maxrun), so both tables are fixed-size at the real
     * token caps rather than n+1. Pricing is pure with the NULL encoder, so filling entries beyond
     * n is harmless. static (single-threaded host, non-recursive, fully rewritten before any read
     * each call) keeps the ~12 KB off the caller frame. */
    static uint32_t slen[LZ_MAX_RUN + 2], mlen[LZ_MAX_MATCH + 2];
    uint32_t *dpr  = (uint32_t *)xmalloc(((size_t)win + 1) * sizeof(uint32_t));
    uint64_t *span_lit = span_lit_prefix(n, content, tags, pt);
    for (size_t L = 1; L <= LZ_MAX_RUN + 1; L++) slen[L] = ugg_price(&pt->gs, (uint32_t)L - 1u);
    for (size_t L = 1; L <= LZ_MAX_MATCH + 1; L++) mlen[L] = ugg_price(&pt->gl, (uint32_t)L - 1u);
    for (uint32_t D = 1; D <= win; D++)
        dpr[D] = pt->fixed_dist_bits >= 0 ? (uint32_t)pt->fixed_dist_bits * PR_SCALE
                                          : ugr_price(&pt->gd, D - 1u);
    const uint64_t INF = UINT64_MAX / 4;
    if (pt->bootstrap_simple) {
        const Cand *crow = (const Cand *)cands->d + cands->n / sizeof(Cand);
        uint64_t *cost = (uint64_t *)xmalloc((n + 1) * sizeof(uint64_t));
        Token *nxt = (Token *)xcalloc(n + 1, sizeof(Token));
        size_t longmax = n < maxrun ? n : maxrun;
        size_t longn = longmax > 8u ? longmax - 8u : 0;
        SpanMinQ *sq = (SpanMinQ *)xmalloc((longn ? longn : 1u) * sizeof(*sq));
        size_t *sq_pos = (size_t *)xmalloc((longn ? longn : 1u) * sizeof(*sq_pos));
        /* With the neutral bootstrap gamma model, slen is constant on each dyadic length
         * bucket. Build from the actual frozen prices anyway: consecutive equal-price runs
         * preserve exactness if bootstrap seeding changes, degenerating safely to singletons. */
        size_t nsq = 0, pool_n = 0;
        for (size_t lo = 9; lo <= longmax;) {
            size_t hi = lo;
            while (hi < longmax && slen[hi + 1u] == slen[lo]) hi++;
            size_t cap = hi - lo + 1u;
            sq[nsq++] = (SpanMinQ){ lo, hi, pool_n, cap, 0, 0, slen[lo] };
            pool_n += cap;
            lo = hi + 1u;
        }
        cost[n] = 0;
        for (size_t ri = n; ri-- > 0;) {
            crow -= ncand[ri];
            uint64_t best = INF;
            Token bt = {0};
            size_t lim = n < ri + maxrun ? n : ri + maxrun;
            size_t dense_end = ri + 8 < lim ? ri + 8 : lim;
            for (size_t j = ri + 1; j <= dense_end; j++) {
                uint64_t c = PR_SCALE + (uint64_t)slen[j - ri] + (span_lit[j] - span_lit[ri]) + cost[j];
                if (c < best) { best = c; bt = (Token){ 'S', (int32_t)ri, (int32_t)(j - ri), 0 }; }
            }
            for (size_t qi = 0; qi < nsq; qi++) {
                SpanMinQ *q = &sq[qi];
                size_t upper = ri + q->hi < n ? ri + q->hi : n;
                while (q->count && sq_pos[q->off + q->head] > upper) {
                    q->head = (q->head + 1u) % q->cap; q->count--;
                }
                size_t j = ri + q->lo;
                if (j <= n && (j == n || ncand[j] || (nocand && nocand[j])) && cost[j] < INF) {
                    uint64_t base = cost[j] + span_lit[j];
                    /* ri descends, so endpoint indexes enter in descending order. Replacing an
                     * equal-base older endpoint keeps the smaller j, matching the old ascending-j
                     * traversal with its strict `c < best` tie. */
                    while (q->count) {
                        size_t bi = (q->head + q->count - 1u) % q->cap;
                        size_t bj = sq_pos[q->off + bi];
                        if (cost[bj] + span_lit[bj] < base) break;
                        q->count--;
                    }
                    size_t at = (q->head + q->count) % q->cap;
                    sq_pos[q->off + at] = j; q->count++;
                }
                if (!q->count) continue;
                j = sq_pos[q->off + q->head];
                uint64_t c = PR_SCALE + (uint64_t)q->price +
                             (span_lit[j] - span_lit[ri]) + cost[j];
                if (c < best) { best = c; bt = (Token){ 'S', (int32_t)ri, (int32_t)(j - ri), 0 }; }
            }
            for (int ci = 0; ci < ncand[ri]; ci++) {
                int32_t bd = crow[ci].dist, bl = crow[ci].len;
                for (int32_t l = 3; l <= bl; l++) {
                    uint64_t c = PR_SCALE + dpr[bd] + (uint64_t)mlen[l] + cost[ri + (size_t)l];
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
        free(cost); free(nxt); free(sq); free(sq_pos); free(dpr); free(span_lit);
        return tv;
    }
    /* Measured/adaptive gs mantissa prices generally vary inside a dyadic bucket, so the
     * history/rep DP below keeps its exact predecessor scan; the bootstrap deque separation
     * must not be generalized here by treating gamma length as a bit-length-only price. */
    uint32_t *glo_price = NULL;
#ifndef OUT_ENVELOPE_REFERENCE
    int32_t max_out_len = 0;
    if (pt->out_en && ocands && nocand && ocands->n) {
        const OCand *all = (const OCand *)ocands->d;
        size_t all_n = ocands->n / sizeof(*all);
        for (size_t oi = 0; oi < all_n; oi++)
            if (all[oi].len > max_out_len) max_out_len = all[oi].len;
        if (max_out_len >= (int32_t)RC_OUTMATCH_MIN) {
            glo_price = (uint32_t *)xmalloc(((size_t)max_out_len + 1u) * sizeof(*glo_price));
            for (int32_t l = (int32_t)RC_OUTMATCH_MIN; l <= max_out_len; l++) {
                glo_price[l] = ugg_price(&pt->glo, (uint32_t)l - RC_OUTMATCH_MIN);
            }
        }
    }
#endif
    /* Every literal's tag0 context is now the deterministic previous content byte content[p-1]
     * (order-1 wire: matches/backrefs/out-matches update prevlit too), so this prefix table gives
     * the EXACT literal cost of any span [i,j) as span_lit[j] - span_lit[i], first literal included
     * -- no per-state carried prevlit, no first-literal special case. */
    /* per-context match-flag extras: match flag + fresh-distance flag + value, vs reuse flag (rep0).
     * After a span (h&1 == 0: previous token kind bit is 0) the match flag is implicit on the
     * wire (adjacent spans never ship), so its price is zero in those contexts. */
    uint64_t fresh_extra[4], reuse_extra[4], out_extra[4];
    for (int h = 0; h < 4; h++) {
        uint64_t mflag = (h & 1) ? (uint64_t)pt->fmatch_c[h] : 0;
        fresh_extra[h] = mflag + pt->rep0_no + (pt->out_en ? pt->outb_no : 0);
        reuse_extra[h] = mflag + pt->rep0_yes;
        out_extra[h]   = mflag + pt->rep0_no + pt->outb_yes;
    }
    /* One state per (position, flag-history h): h = (prev2<<1)|prev1, span=0/match=1, keeping only
     * the cheapest arrival. State index s = i*4 + h; ns = (n+1)*4. Encoder-only; the chosen parse
     * is re-checked by the exact byte gate. */
    /* Span ends only matter where a token can START (or at the content end): spans ending in a
     * literal desert fold into a longer span (merge_adjacent_spans keeps the wire identical, the
     * DP merely prices long spans as several). */
    size_t ns = (n + 1) * 4;   /* one state per (position, flag-history h): cheapest arrival */
    uint64_t *cost = (uint64_t *)xmalloc(ns * sizeof(uint64_t));
    int32_t  *rep  = (int32_t *)xmalloc(ns * sizeof(int32_t)); /* rep distance arriving at state */
    Token *via = (Token *)xcalloc(ns, sizeof(Token));         /* token that arrives at state */
    uint8_t *vh = (uint8_t *)xcalloc(ns, sizeof(uint8_t));    /* predecessor flag-history h for backtrack */
    /* A span always arrives in an even flag-history state. Collapse the two predecessor
     * histories that map to each even state once per position, then scan those two bases at
     * each destination. This preserves the old strict-< order: positions are considered in
     * ascending order, and the lower history wins an equal-cost pair. */
    uint64_t *span_pair = (uint64_t *)xmalloc((n ? n : 1) * 2u * sizeof(*span_pair));
    for (size_t s = 0; s < ns; s++) { cost[s] = INF; rep[s] = 0; }
    cost[0] = 0; rep[0] = 0; /* pos 0, h=0: wire seeds last_dist=0, flag.h=0 */
    int ord_len[LZ_CAND_MAX]; int32_t ord_dist[LZ_CAND_MAX]; uint32_t ord_dpr[LZ_CAND_MAX];
    const Cand *crow = (const Cand *)cands->d;
    const OCand *ocrow = ocands ? (const OCand *)ocands->d : NULL;
    for (size_t i = 0; i <= n; i++) {
        /* Reverse the old predecessor->endpoint span walk. Short spans may end anywhere;
         * longer spans end only where another token can start, or at the content end. Only
         * spans reach histories 0/2, so resolving them here cannot reorder a match arrival. */
        if (i) {
            size_t span_cap = (i == n || ncand[i] || (nocand && nocand[i])) ? maxrun : 8u;
            size_t first = i > span_cap ? i - span_cap : 0;
            uint64_t best[2] = { INF, INF };
            size_t best_pos[2] = { 0, 0 };
            for (size_t p = first; p < i; p++) {
                uint64_t tail = (uint64_t)slen[i - p] + (span_lit[i] - span_lit[p]);
                for (int q = 0; q < 2; q++) {
                    uint64_t b = span_pair[p * 2u + (size_t)q];
                    if (b < INF && b + tail < best[q]) {
                        best[q] = b + tail;
                        best_pos[q] = p;
                    }
                }
            }
            for (int q = 0; q < 2; q++) if (best[q] < INF) {
                size_t p = best_pos[q], ps = p * 4u;
                int h0 = q, h1 = q + 2;
                uint64_t b0 = cost[ps + (size_t)h0] < INF
                            ? cost[ps + (size_t)h0] + pt->fspan_c[h0] : INF;
                uint64_t b1 = cost[ps + (size_t)h1] < INF
                            ? cost[ps + (size_t)h1] + pt->fspan_c[h1] : INF;
                int h = b1 < b0 ? h1 : h0;
                size_t jb = i * 4u + (size_t)(q * 2);
                relax2(cost, rep, via, vh, jb, best[q], rep[ps + (size_t)h],
                       (Token){ 'S', (int32_t)p, (int32_t)(i - p), 0 }, (uint8_t)h);
            }
        }
        if (i == n) break;

        /* Pair histories {0,2}->{0} and {1,3}->{2}; strict comparison retains the
         * lower predecessor history on ties, matching the old hr=0..3 traversal. */
        for (int q = 0; q < 2; q++) {
            int h0 = q, h1 = q + 2;
            size_t si = i * 4u;
            uint64_t b0 = cost[si + (size_t)h0] < INF
                        ? cost[si + (size_t)h0] + pt->fspan_c[h0] : INF;
            uint64_t b1 = cost[si + (size_t)h1] < INF
                        ? cost[si + (size_t)h1] + pt->fspan_c[h1] : INF;
            span_pair[i * 2u + (size_t)q] = b1 < b0 ? b1 : b0;
        }
        /* len-ascending candidate ranges with suffix-min distance price: for lengths in
         * (len[k-1], len[k]] the cheapest eligible candidate is the suffix argmin, so each
         * length is relaxed ONCE per state instead of once per candidate. Distance-reuse
         * (rep0) pricing is fully covered by the explicit rep probe below, so the candidate
         * loop prices fresh only. State-independent; computed once per position. */
        int nc = ncand[i];
        const Cand *row = crow;
        crow += nc;
        int no = nocand ? nocand[i] : 0;
        const OCand *orow = ocrow;
        if (ocrow) ocrow += no;
        for (int k = 0; k < nc; k++) {
            int32_t lk = row[k].len, dk2 = row[k].dist;
            int at = k;
            while (at > 0 && ord_len[at - 1] > lk) { ord_len[at] = ord_len[at - 1]; ord_dist[at] = ord_dist[at - 1]; at--; }
            ord_len[at] = lk; ord_dist[at] = dk2;
        }
        for (int k = nc; k-- > 0;) {
            uint32_t dp2 = dpr[ord_dist[k]];
            if (k + 1 < nc && ord_dpr[k + 1] < dp2) { ord_dpr[k] = ord_dpr[k + 1]; ord_dist[k] = ord_dist[k + 1]; }
            else ord_dpr[k] = dp2;
        }
        /* At a fixed length every out candidate has identical price, next history, and rep.
         * Preserve legacy row order by assigning each length only to the first candidate that
         * reaches it; later candidates own only their extension beyond the earlier maximum. */
#ifndef OUT_ENVELOPE_REFERENCE
        int out_nenv = 0, out_covered = (int)RC_OUTMATCH_MIN - 1;
        int32_t out_from[OC_MAX], out_to[OC_MAX], out_pos[OC_MAX];
        for (int cix = 0, nout = pt->out_en && orow ? no : 0; cix < nout; cix++) {
            int32_t top = orow[cix].len;
            if (top <= out_covered) continue;
            int32_t begin = out_covered + 1;
            if (begin < (int32_t)RC_OUTMATCH_MIN) begin = (int32_t)RC_OUTMATCH_MIN;
            if (begin <= top) {
                out_from[out_nenv] = begin; out_to[out_nenv] = top;
                out_pos[out_nenv] = orow[cix].pos; out_nenv++;
                out_covered = top;
            }
        }
#endif
        int32_t probe_ri = -1; size_t probe_rl = 0;   /* rep-probe scan memo: states share ri */
        size_t match_lim = n < i + maxrun ? n : i + maxrun;
        for (int hr = 0; hr < 4; hr++) {
            int h = hr;
            size_t si = i * 4 + (size_t)h;
            if (cost[si] >= INF) continue;       /* unreachable along any cheap path */
            uint64_t ci = cost[si]; int32_t ri = rep[si];
            /* matches: emit one match flag (kind 1) under context h; rep0 reuse when the candidate
             * distance equals the incoming rep distance. New flag history after a match: h' = (h<<1|1)&3. */
            int hm = rc_fl_hist(h, 1);
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
#ifdef OUT_ENVELOPE_REFERENCE
            for (int cix = 0, nout = pt->out_en && orow ? no : 0; cix < nout; cix++) {
                int32_t opos = orow[cix].pos, olm = orow[cix].len;
                uint64_t obase = ci + out_extra[h] + pt->opos_avg;
                for (int32_t l = (int32_t)RC_OUTMATCH_MIN; l <= olm; l++) {
                    size_t j = i + (size_t)l;
                    uint64_t c = obase + ugg_price(&pt->glo, (uint32_t)l - RC_OUTMATCH_MIN);
                    size_t jb = j * 4 + (size_t)hm;
                    relax2(cost, rep, via, vh, jb, c, ri,
                           (Token){ 'O', (int32_t)i, l, opos }, (uint8_t)hr);
                }
            }
#else
            uint64_t obase = ci + out_extra[h] + pt->opos_avg;
            for (int oe = 0; oe < out_nenv; oe++) {
                int32_t opos = out_pos[oe];
                for (int32_t l = out_from[oe]; l <= out_to[oe]; l++) {
                    size_t j = i + (size_t)l;
                    uint64_t c = obase + glo_price[l];
                    size_t jb = j * 4 + (size_t)hm;
                    relax2(cost, rep, via, vh, jb, c, ri,
                           (Token){ 'O', (int32_t)i, l, opos }, (uint8_t)hr);
                }
            }
#endif
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
                    size_t rlim = match_lim - i;   /* same maxrun cap as spans */
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
#ifdef OUT_ENVELOPE_PROBE
    out_envelope_probe_last_cost = cbest;
#endif
    TokenVec tv = {0};
    size_t pos = n; int hr = hrbest;
    while (pos > 0) {
        size_t s = pos * 4 + (size_t)hr;
        tok_push(&tv, via[s]);
        pos = (size_t)via[s].start;
        hr = vh[s];
    }
    for (size_t a = 0, b = tv.n; a + 1 < b; a++, b--) { Token t = tv.v[a]; tv.v[a] = tv.v[b - 1]; tv.v[b - 1] = t; }
    free(cost); free(rep); free(via); free(vh); free(span_pair); free(glo_price); free(dpr); free(span_lit);
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

int fit_k_tokens(const TokenVec *tv) {
    int best = 0;
    uint64_t bestc = UINT64_MAX;
    for (int k = 0; k < 16; k++) {
        uint64_t c = 0;
        for (size_t i = 0; i < tv->n; i++) if (tv->v[i].type == 'R') {
            uint32_t v = (uint32_t)tv->v[i].dist - 1u;
            if (!rc_rice_feasible(v, (uint32_t)k)) { c = UINT64_MAX; break; }
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
            if (!rc_rice_feasible(v, (uint32_t)k)) { c = UINT64_MAX; break; }
            c += (v >> k) + 1u + (uint32_t)k;
        }
        if (c < bestc) { bestc = c; best = k; }
    }
    return any ? best : cur;
}

static void bootstrap_prices(PriceTab *pt, const uint8_t L0[256], const uint8_t L1[256]) {
    memset(pt, 0, sizeof(*pt));
    for (int c = 0; c < UP_LIT0_CTX; c++)
        for (int b = 0; b < 256; b++) pt->lit0[c][b] = (uint16_t)((uint32_t)L0[b] * PR_SCALE);
    for (int b = 0; b < 256; b++) pt->lit1[b] = (uint16_t)((uint32_t)L1[b] * PR_SCALE);
    ugg_init_e(&pt->gs);
    ugg_init_e(&pt->gl);
    ugr_init_e(&pt->gd, WINDOW_LOG);
    pt->fixed_dist_bits = -1;
    pt->bootstrap_simple = 1;
}

/* Build the LZ match-candidate set (hash-chain over 3-byte keys, full chain within the window)
 * and an initial rice-DP parse. The caller owns *cands_out / *ncand_out and frees them after the
 * price-feedback loop (which it runs with its own, full-body acceptance gate). */
TokenVec lz_candidates_c(const uint8_t *data, const uint8_t *tags, size_t n,
                                const uint8_t L0[256], const uint8_t L1[256],
                                int *k_out,
                                CandArena *cands_out, uint8_t **ncand_out) {
    int32_t win = 1 << WINDOW_LOG, maxm = LZ_MAX_MATCH;
    CandArena cands = {0};
    uint8_t *ncand = (uint8_t *)xcalloc(n ? n : 1, 1);
    int32_t *head = hash3_heads_new();
    int32_t *prev = hash3_prev_new(n);
    for (size_t i = 0; i < n; i++) {
        Cand row[LZ_CAND_MAX];
        uint8_t nr = 0;
        if (i + 3 <= n) {
            uint32_t key = hash3_key_fwd(data + i);
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
                if (lbest > 0 && nr < LZ_CAND_MAX &&
                    data[(size_t)pj + (size_t)lbest] != data[i + (size_t)lbest])
                    continue;
                int32_t l = 0;
                while (l < ml && data[(size_t)pj + (size_t)l] == data[i + (size_t)l]) l++;
                cand_add(row, &nr, (Cand){ dist, l });
                if (l >= 3 && l > lbest) lbest = l;
                if (l == ml) break;
            }
            prev[i] = head[key];
            head[key] = (int32_t)i;
        }
        ncand[i] = nr;
        if (nr) buf_write(&cands, row, (size_t)nr * sizeof(*row));
    }
    free(head); free(prev);
    PriceTab pt;
    bootstrap_prices(&pt, L0, L1);
    pt.gd.k = WINDOW_LOG;
    pt.fixed_dist_bits = WINDOW_LOG;
    TokenVec seq = lz_parse_priced(n, data, tags, &cands, ncand, NULL, NULL, &pt);
    int k = fit_k_tokens(&seq);
    int parsed_k = -1;
    for (int pass = 0; pass < 8; pass++) {
        free(seq.v);
        pt.gd.k = (uint8_t)k;
        pt.fixed_dist_bits = -1;
        seq = lz_parse_priced(n, data, tags, &cands, ncand, NULL, NULL, &pt);
        parsed_k = k;
        int nk = fit_k_tokens(&seq);
        if (nk == k) break;
        k = nk;
    }
    if (parsed_k != k) {
        free(seq.v);
        pt.gd.k = (uint8_t)k;
        pt.fixed_dist_bits = -1;
        seq = lz_parse_priced(n, data, tags, &cands, ncand, NULL, NULL, &pt);
    }
    *k_out = k;
    *cands_out = cands;
    *ncand_out = ncand;
    return seq;
}
