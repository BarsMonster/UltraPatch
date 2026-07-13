/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 *
 * Behavioral checks for encoder LZ kernels. The real private module is included so the parsers
 * and pricing primitives are exercised without exporting test-only APIs.
 */
#include "enc_internal.h"
#include "../src/enc_lz.c"

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "encoder LZ probe failed at line %d: %s\n", __LINE__, #x); \
    return 1; \
} } while (0)

static int check_parse(const char *name, size_t n, const uint8_t *content,
                       const uint8_t *tags, const CandArena *cands, const uint8_t *ncand,
                       const OCandArena *ocands, const uint8_t *nocand,
                       const PriceTab *pt) {
    TokenVec tv = lz_parse_priced(n, content, tags, cands, ncand, ocands, nocand, pt);
    size_t pos = 0;
    for (size_t i = 0; i < tv.n; i++) {
        const Token *t = &tv.v[i];
        if (t->start < 0 || (size_t)t->start != pos || t->len <= 0 ||
            (size_t)t->len > n - pos ||
            (t->type != 'S' && t->type != 'R' && t->type != 'O')) {
            fprintf(stderr, "invalid LZ token case=%s index=%zu\n", name, i);
            free(tv.v); return 1;
        }
        pos += (size_t)t->len;
    }
    if (pos != n) { fprintf(stderr, "incomplete LZ parse case=%s\n", name); free(tv.v); return 1; }
    free(tv.v);
    return 0;
}

static int check_case(const char *name, size_t n, const uint8_t *content, const uint8_t *tags,
                      const CandArena *cands, const uint8_t *ncand,
                      const uint8_t *nocand, const PriceTab *pt) {
    return check_parse(name, n, content, tags, cands, ncand, NULL, nocand, pt);
}

static uint32_t rnd_state = 0x64796164u;
static uint32_t rnd32(void) {
    rnd_state = rnd_state * 1664525u + 1013904223u;
    return rnd_state;
}

static uint16_t rnd_prob(void) { return (uint16_t)(1u + rnd32() % (RC_PBIT - 1u)); }

static void random_prices(PriceTab *pt) {
    memset(pt, 0, sizeof(*pt));
    for (int c = 0; c < UP_LIT0_CTX; c++)
        for (int b = 0; b < 256; b++) pt->lit0[c][b] = (uint16_t)(rnd32() % (PRICE_LIT_MAX + 1u));
    for (int b = 0; b < 256; b++) pt->lit1[b] = (uint16_t)(rnd32() % (PRICE_LIT_MAX + 1u));
    for (int i = 0; i <= UP_UG_CTX; i++) {
        pt->gs.u[i] = rnd_prob(); pt->gl.u[i] = rnd_prob(); pt->gd.u[i] = rnd_prob();
        for (int j = 0; j <= UP_UG_CTX; j++) pt->gd.m[i][j] = rnd_prob();
    }
    for (int i = 0; i < UP_UG_GAMMA_MANT; i++) {
        pt->gs.m[i] = rnd_prob(); pt->gl.m[i] = rnd_prob();
    }
    pt->gd.k = (uint8_t)(rnd32() % 12u);
    pt->fixed_dist_bits = (rnd32() & 1u) ? -1 : (int)(rnd32() % 17u);
    pt->bootstrap_simple = 1;
}

static int span_only_cover(const TokenVec *tv, size_t n) {
    size_t pos = 0;
    for (size_t i = 0; i < tv->n; i++) {
        const Token *t = &tv->v[i];
        if (t->type != 'S' || t->start < 0 || (size_t)t->start != pos ||
            t->len <= 0 || (size_t)t->len > n - pos) return 0;
        pos += (size_t)t->len;
    }
    return pos == n;
}

static int empty_no_match_cases(void) {
    static const size_t lengths[] = { 0, 1, 2, LZ_MAX_RUN + 17u };
    uint8_t l0[256], l1[256]; memset(l0, 1, sizeof(l0)); memset(l1, 1, sizeof(l1));
    for (size_t z = 0; z < sizeof(lengths) / sizeof(lengths[0]); z++) {
        size_t n = lengths[z];
        uint8_t *content = (uint8_t *)xcalloc(n ? n : 1u, 1);
        uint8_t *tags = (uint8_t *)xcalloc(n ? n : 1u, 1);
        uint8_t *ncand = (uint8_t *)xcalloc(n ? n : 1u, 1);
        CandArena cands = {0};
        PriceTab pt; bootstrap_prices(&pt, l0, l1);
        char name[40]; (void)snprintf(name, sizeof(name), "empty-%zu-bootstrap", n);
        CHECK(!check_case(name, n, content, tags, &cands, ncand, NULL, &pt));
        pt.bootstrap_simple = 0;
        (void)snprintf(name, sizeof(name), "empty-%zu-adaptive", n);
        TokenVec adaptive = lz_parse_priced(n, content, tags, &cands, ncand, NULL, NULL, &pt);
        CHECK(span_only_cover(&adaptive, n)); free(adaptive.v);
        CHECK(!check_case(name, n, content, tags, &cands, ncand, NULL, &pt));
        free(content); free(tags); free(ncand);
    }
    return 0;
}

static int boundary_cases(void) {
    static const uint16_t lengths[] = {
        1, 2, 3, 4, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65,
        127, 128, 129, 255, 256, 257, 511, 512, 513, 1023, 1024
    };
    uint8_t l0[256], l1[256]; memset(l0, 1, sizeof(l0)); memset(l1, 1, sizeof(l1));
    PriceTab pt; bootstrap_prices(&pt, l0, l1);
    /* Zero literals make equal-base and equal-price endpoint ties common within each bucket. */
    for (int c = 0; c < UP_LIT0_CTX; c++) memset(pt.lit0[c], 0, sizeof(pt.lit0[c]));
    memset(pt.lit1, 0, sizeof(pt.lit1));
    for (size_t z = 0; z < sizeof(lengths) / sizeof(lengths[0]); z++) {
        size_t n = lengths[z];
        uint8_t *content = (uint8_t *)xcalloc(n ? n : 1u, 1);
        uint8_t *tags = (uint8_t *)xcalloc(n ? n : 1u, 1);
        uint8_t *ncand = (uint8_t *)xcalloc(n ? n : 1u, 1);
        uint8_t *nocand = (uint8_t *)xcalloc(n ? n : 1u, 1);
        for (size_t j = 0; j < n; j++) nocand[j] = 1; /* every long endpoint eligible */
        CandArena cands = {0};
        char name[40]; (void)snprintf(name, sizeof(name), "boundary-%zu", n);
        int fail = check_case(name, n, content, tags, &cands, ncand, nocand, &pt);
        free(content); free(tags); free(ncand); free(nocand);
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
        CandArena cands = {0};
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
        char name[40]; (void)snprintf(name, sizeof(name), "random-%03d", c);
        int fail = check_case(name, n, content, tags, &cands, ncand, nocand, &pt);
        free(content); free(tags); free(ncand); free(nocand); free(cands.d);
        if (fail) { fprintf(stderr, "span-deque randomized case=%d\n", c); return 1; }
    }
    return 0;
}

#include "encoder-lz-out-cases.inc"

int main(void) {
    int fail = empty_no_match_cases() || boundary_cases() || randomized_cases() ||
               out_envelope_cases();
    if (fail) return 1;
    printf("span_deque_results=OK empty_no_match=4 bootstrap=OK adaptive=OK boundaries=27 "
           "randomized=384 tokens=OK\n");
    printf("out_envelope_results=OK rows=4098 empty=OK nonmonotone=OK dominated=OK "
           "strict_ties=OK disabled=OK beyond_lz_max=OK randomized=320 tokens=OK\n");
    return 0;
}
