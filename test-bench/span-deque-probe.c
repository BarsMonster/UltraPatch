/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 *
 * Exact old-scan oracle for the bootstrap LZ span deques. The real module is included so the
 * optimized parser and all pricing primitives are exercised without exporting test-only APIs.
 */
#include "enc_internal.h"
#include "../src/enc_lz.c"

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "span-deque oracle failed at line %d: %s\n", __LINE__, #x); \
    return 1; \
} } while (0)

static size_t *old_next_starts(size_t n, const uint8_t *ncand, const uint8_t *nocand) {
    size_t *next = (size_t *)xmalloc((n + 2u) * sizeof(*next));
    next[n] = n; next[n + 1u] = n;
    for (size_t j = n; j-- > 0;)
        next[j] = (ncand[j] || (nocand && nocand[j])) ? j : next[j + 1u];
    return next;
}

static TokenVec old_bootstrap_parse(size_t n, const uint8_t *content, const uint8_t *tags,
                                    const CandArena *cands, const uint8_t *ncand,
                                    const uint8_t *nocand, const PriceTab *pt,
                                    uint64_t *cost0) {
    const uint32_t maxrun = LZ_MAX_RUN, win = 1u << WINDOW_LOG;
    uint32_t slen[LZ_MAX_RUN + 2], mlen[LZ_MAX_MATCH + 2];
    uint32_t *dpr = (uint32_t *)xmalloc(((size_t)win + 1u) * sizeof(*dpr));
    uint64_t *span_lit = span_lit_prefix(n, content, tags, pt);
    for (size_t l = 1; l <= LZ_MAX_RUN + 1u; l++)
        slen[l] = ugg_price(&pt->gs, (uint32_t)l - 1u);
    for (size_t l = 1; l <= LZ_MAX_MATCH + 1u; l++)
        mlen[l] = ugg_price(&pt->gl, (uint32_t)l - 1u);
    for (uint32_t d = 1; d <= win; d++)
        dpr[d] = pt->fixed_dist_bits >= 0 ? (uint32_t)pt->fixed_dist_bits * PR_SCALE
                                          : ugr_price(&pt->gd, d - 1u);
    size_t *next = old_next_starts(n, ncand, nocand);
    const Cand *crow = (const Cand *)cands->d + cands->n / sizeof(Cand);
    uint64_t *cost = (uint64_t *)xmalloc((n + 1u) * sizeof(*cost));
    Token *nxt = (Token *)xcalloc(n + 1u, sizeof(*nxt));
    const uint64_t inf = UINT64_MAX / 4u;
    cost[n] = 0;
    for (size_t ri = n; ri-- > 0;) {
        crow -= ncand[ri];
        uint64_t best = inf; Token bt = {0};
        size_t lim = n < ri + maxrun ? n : ri + maxrun;
        size_t dense_end = ri + 8u < lim ? ri + 8u : lim;
        for (size_t j = ri + 1u; j <= dense_end; j++) {
            uint64_t c = PR_SCALE + (uint64_t)slen[j - ri] +
                         (span_lit[j] - span_lit[ri]) + cost[j];
            if (c < best) { best = c; bt = (Token){'S', (int32_t)ri, (int32_t)(j - ri), 0}; }
        }
        for (size_t j = next[dense_end + 1u]; j <= lim; j = next[j + 1u]) {
            uint64_t c = PR_SCALE + (uint64_t)slen[j - ri] +
                         (span_lit[j] - span_lit[ri]) + cost[j];
            if (c < best) { best = c; bt = (Token){'S', (int32_t)ri, (int32_t)(j - ri), 0}; }
            if (j >= n) break;
        }
        for (int ci = 0; ci < ncand[ri]; ci++) {
            int32_t bd = crow[ci].dist, bl = crow[ci].len;
            for (int32_t l = 3; l <= bl; l++) {
                uint64_t c = PR_SCALE + dpr[bd] + (uint64_t)mlen[l] +
                             cost[ri + (size_t)l];
                if (c < best) { best = c; bt = (Token){'R', (int32_t)ri, l, bd}; }
            }
        }
        cost[ri] = best; nxt[ri] = bt;
    }
    TokenVec out = {0};
    for (size_t i = 0; i < n;) { tok_push(&out, nxt[i]); i += (size_t)nxt[i].len; }
    *cost0 = cost[0];
    free(cost); free(nxt); free(next); free(dpr); free(span_lit);
    return out;
}

static int same_tokens(const TokenVec *a, const TokenVec *b) {
    if (a->n != b->n) return 0;
    for (size_t i = 0; i < a->n; i++)
        if (a->v[i].type != b->v[i].type || a->v[i].start != b->v[i].start ||
            a->v[i].len != b->v[i].len || a->v[i].dist != b->v[i].dist) return 0;
    return 1;
}

static uint64_t token_cost(const TokenVec *tv, size_t n, const uint8_t *content,
                           const uint8_t *tags, const PriceTab *pt) {
    uint64_t *lit = span_lit_prefix(n, content, tags, pt);
    uint64_t cost = 0;
    for (size_t i = 0; i < tv->n; i++) {
        const Token *t = &tv->v[i];
        if (t->type == 'S')
            cost += PR_SCALE + ugg_price(&pt->gs, (uint32_t)t->len - 1u) +
                    lit[(size_t)t->start + (size_t)t->len] - lit[t->start];
        else
            cost += PR_SCALE +
                    (pt->fixed_dist_bits >= 0 ? (uint32_t)pt->fixed_dist_bits * PR_SCALE
                                              : ugr_price(&pt->gd, (uint32_t)t->dist - 1u)) +
                    ugg_price(&pt->gl, (uint32_t)t->len - 1u);
    }
    free(lit);
    return cost;
}

static int check_case(size_t n, const uint8_t *content, const uint8_t *tags,
                      const CandArena *cands, const uint8_t *ncand,
                      const uint8_t *nocand, const PriceTab *pt) {
    uint64_t old_cost;
    TokenVec old = old_bootstrap_parse(n, content, tags, cands, ncand, nocand, pt, &old_cost);
    TokenVec now = lz_parse_priced(n, content, tags, cands, ncand, NULL, nocand, pt);
    uint64_t now_cost = token_cost(&now, n, content, tags, pt);
    int ok = old_cost == now_cost && same_tokens(&old, &now);
    if (!ok)
        fprintf(stderr, "span-deque mismatch n=%zu old_cost=%llu new_cost=%llu old_n=%zu new_n=%zu\n",
                n, (unsigned long long)old_cost, (unsigned long long)now_cost, old.n, now.n);
    free(old.v); free(now.v);
    return ok ? 0 : 1;
}

static uint32_t rnd_state = 0x64796164u;
static uint32_t rnd32(void) {
    rnd_state = rnd_state * 1664525u + 1013904223u;
    return rnd_state;
}

static uint16_t rnd_prob(void) { return (uint16_t)(1u + rnd32() % (RC_PBIT - 1u)); }

static void random_prices(PriceTab *pt) {
    memset(pt, 0, sizeof(*pt));
    for (int c = 0; c < LIT0_CTX; c++)
        for (int b = 0; b < 256; b++) pt->lit0[c][b] = (uint16_t)(rnd32() % (PRICE_LIT_MAX + 1u));
    for (int b = 0; b < 256; b++) pt->lit1[b] = (uint16_t)(rnd32() % (PRICE_LIT_MAX + 1u));
    for (int i = 0; i <= UG_CTX; i++) {
        pt->gs.u[i] = rnd_prob(); pt->gl.u[i] = rnd_prob(); pt->gd.u[i] = rnd_prob();
        for (int j = 0; j <= UG_CTX; j++) pt->gd.m[i][j] = rnd_prob();
    }
    for (int i = 0; i < UG_GAMMA_MANT; i++) {
        pt->gs.m[i] = rnd_prob(); pt->gl.m[i] = rnd_prob();
    }
    pt->gd.k = (uint8_t)(rnd32() % 12u);
    pt->fixed_dist_bits = (rnd32() & 1u) ? -1 : (int)(rnd32() % 17u);
    pt->bootstrap_simple = 1;
}

static int boundary_cases(void) {
    static const uint16_t lengths[] = {
        1, 2, 3, 4, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65,
        127, 128, 129, 255, 256, 257, 511, 512, 513, 1023, 1024
    };
    uint8_t l0[256], l1[256]; memset(l0, 1, sizeof(l0)); memset(l1, 1, sizeof(l1));
    PriceTab pt; bootstrap_prices(&pt, l0, l1);
    /* Zero literals make equal-base and equal-price endpoint ties common within each bucket. */
    for (int c = 0; c < LIT0_CTX; c++) memset(pt.lit0[c], 0, sizeof(pt.lit0[c]));
    memset(pt.lit1, 0, sizeof(pt.lit1));
    for (size_t z = 0; z < sizeof(lengths) / sizeof(lengths[0]); z++) {
        size_t n = lengths[z];
        uint8_t *content = (uint8_t *)xcalloc(n ? n : 1u, 1);
        uint8_t *tags = (uint8_t *)xcalloc(n ? n : 1u, 1);
        uint8_t *ncand = (uint8_t *)xcalloc(n ? n : 1u, 1);
        uint8_t *nocand = (uint8_t *)xcalloc(n ? n : 1u, 1);
        for (size_t j = 0; j < n; j++) nocand[j] = 1; /* every long endpoint eligible */
        CandArena cands = { (uint8_t *)xmalloc(1), 0, 1 };
        int fail = check_case(n, content, tags, &cands, ncand, nocand, &pt);
        free(cands.d); free(content); free(tags); free(ncand); free(nocand);
        if (fail) return 1;
    }
    return 0;
}

static int randomized_cases(void) {
    enum { CASES = 384 };
    for (int c = 0; c < CASES; c++) {
        size_t n = 1u + rnd32() % 192u;
        uint8_t *content = (uint8_t *)xmalloc(n), *tags = (uint8_t *)xmalloc(n);
        uint8_t *ncand = (uint8_t *)xcalloc(n, 1), *nocand = (uint8_t *)xcalloc(n, 1);
        CandArena cands = { (uint8_t *)xmalloc(1), 0, 1 };
        for (size_t i = 0; i < n; i++) { content[i] = (uint8_t)rnd32(); tags[i] = (uint8_t)(rnd32() & 1u); }
        for (size_t i = 0; i < n; i++) {
            nocand[i] = (uint8_t)((rnd32() & 3u) == 0u);
            if (i == 0 || n - i < 3u) continue;
            uint8_t nr = (uint8_t)(rnd32() % 4u); ncand[i] = nr;
            for (uint8_t k = 0; k < nr; k++) {
                int32_t maxl = (int32_t)((n - i) < 40u ? n - i : 40u);
                Cand v = { 1 + (int32_t)(rnd32() % (i < (1u << WINDOW_LOG) ? i : (1u << WINDOW_LOG))),
                           3 + (int32_t)(rnd32() % (uint32_t)(maxl - 2)) };
                buf_write(&cands, &v, sizeof(v));
            }
        }
        PriceTab pt; random_prices(&pt);
        int fail = check_case(n, content, tags, &cands, ncand, nocand, &pt);
        free(content); free(tags); free(ncand); free(nocand); free(cands.d);
        if (fail) { fprintf(stderr, "span-deque randomized case=%d\n", c); return 1; }
    }
    return 0;
}

int main(void) {
    if (boundary_cases() || randomized_cases()) return 1;
    printf("span_deque_oracle=OK boundaries=27 equal_ties=1 randomized=384 costs=OK predecessors=OK tokens=OK\n");
    return 0;
}
