/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 * Deterministic behavioral cases for encoder-lz-probe.c's out-envelope logic.
 */

static uint32_t out_rnd_state = 0x6f757465u;
static uint32_t out_rnd32(void) {
    out_rnd_state = out_rnd_state * 1664525u + 1013904223u;
    return out_rnd_state;
}

static uint16_t out_rnd_prob(void) {
    return (uint16_t)(1u + out_rnd32() % (RC_PBIT - 1u));
}

static void out_random_prices(PriceTab *pt, int out_en) {
    memset(pt, 0, sizeof(*pt));
    for (int h = 0; h < 4; h++) {
        pt->fspan_c[h] = out_rnd32() % (24u * PR_SCALE + 1u);
        pt->fmatch_c[h] = out_rnd32() % (24u * PR_SCALE + 1u);
    }
    pt->rep0_yes = out_rnd32() % (16u * PR_SCALE + 1u);
    pt->rep0_no = out_rnd32() % (16u * PR_SCALE + 1u);
    pt->outb_yes = out_rnd32() % (16u * PR_SCALE + 1u);
    pt->outb_no = out_rnd32() % (16u * PR_SCALE + 1u);
    pt->opos_avg = out_rnd32() % (32u * PR_SCALE + 1u);
    for (int c = 0; c < UP_LIT0_CTX; c++)
        for (int b = 0; b < 256; b++)
            pt->lit0[c][b] = (uint16_t)(out_rnd32() % (PRICE_LIT_MAX + 1u));
    for (int b = 0; b < 256; b++)
        pt->lit1[b] = (uint16_t)(out_rnd32() % (PRICE_LIT_MAX + 1u));
    for (int i = 0; i <= UP_UG_CTX; i++) {
        pt->gs.u[i] = out_rnd_prob();
        pt->gl.u[i] = out_rnd_prob();
        pt->glo.u[i] = out_rnd_prob();
        pt->gd.u[i] = out_rnd_prob();
        for (int j = 0; j <= UP_UG_CTX; j++) pt->gd.m[i][j] = out_rnd_prob();
    }
    for (int i = 0; i < UP_UG_GAMMA_MANT; i++) {
        pt->gs.m[i] = out_rnd_prob();
        pt->gl.m[i] = out_rnd_prob();
        pt->glo.m[i] = out_rnd_prob();
    }
    pt->gd.k = (uint8_t)(out_rnd32() % 12u);
    pt->fixed_dist_bits = (out_rnd32() & 1u) ? -1 : (int)(out_rnd32() % 17u);
    pt->bootstrap_simple = 0;
    pt->out_en = out_en;
}

static void out_cheap_prices(PriceTab *pt, int out_en) {
    memset(pt, 0, sizeof(*pt));
    for (int c = 0; c < UP_LIT0_CTX; c++)
        for (int b = 0; b < 256; b++) pt->lit0[c][b] = PRICE_LIT_MAX;
    for (int b = 0; b < 256; b++) pt->lit1[b] = PRICE_LIT_MAX;
    for (int i = 0; i <= UP_UG_CTX; i++) {
        pt->gs.u[i] = RC_PHALF;
        pt->gl.u[i] = RC_PHALF;
        pt->glo.u[i] = RC_PHALF;
        pt->gd.u[i] = RC_PHALF;
        for (int j = 0; j <= UP_UG_CTX; j++) pt->gd.m[i][j] = RC_PHALF;
    }
    for (int i = 0; i < UP_UG_GAMMA_MANT; i++) {
        pt->gs.m[i] = RC_PHALF;
        pt->gl.m[i] = RC_PHALF;
        pt->glo.m[i] = RC_PHALF;
    }
    pt->fixed_dist_bits = -1;
    pt->bootstrap_simple = 0;
    pt->out_en = out_en;
}

static void out_put_orow(OCandArena *arena, uint8_t *nocand, size_t i,
                     const OCand row[OC_MAX], uint8_t nr) {
    nocand[i] = nr;
    if (nr) buf_write(arena, row, (size_t)nr * sizeof(*row));
}

static int out_emit_case(const char *name, size_t n, const uint8_t *content, const uint8_t *tags,
                     const CandArena *cands, const uint8_t *ncand,
                     const OCandArena *ocands, const uint8_t *nocand, const PriceTab *pt,
                     int32_t expect_opos, int32_t expect_olen) {
    TokenVec tv = lz_parse_priced(n, content, tags, cands, ncand, ocands, nocand, pt);
    if (expect_opos >= 0 &&
        (tv.n != 1u || tv.v[0].type != 'O' || tv.v[0].dist != expect_opos ||
         tv.v[0].len != expect_olen)) {
        fprintf(stderr, "out-envelope expected one O token case=%s opos=%d len=%d\n",
                name, expect_opos, expect_olen);
        free(tv.v);
        return 1;
    }
    free(tv.v);
    return check_parse(name, n, content, tags, cands, ncand, ocands, nocand, pt);
}

/* Independent row-level oracle: the old nested loop chooses the first row entry whose top
 * reaches a fixed length. Compare that owner with the compact extension ranges used by the
 * production parser, including empty, dominated, nonmonotone, and equal-top rows. */
static int out_row_envelope_cases(void) {
    OCand explicit_row[OC_MAX] = { {101, 12}, {202, 5}, {303, 20}, {404, 9} };
    for (int pass = -2; pass < 4096; pass++) {
        OCand row[OC_MAX]; int nr;
        if (pass == -2) {
            nr = 0;
        } else if (pass == -1) {
            memcpy(row, explicit_row, sizeof(row)); nr = OC_MAX;
        } else {
            nr = (int)(out_rnd32() % (OC_MAX + 1u));
            for (int k = 0; k < nr; k++) {
                row[k].pos = 1000 + pass * OC_MAX + k;
                row[k].len = (int32_t)(out_rnd32() % 97u);
            }
        }
        int from[OC_MAX], to[OC_MAX], owner[OC_MAX], nenv = 0;
        int covered = (int)RC_OUTMATCH_MIN - 1;
        for (int k = 0; k < nr; k++) {
            int top = row[k].len;
            if (top <= covered) continue;
            int begin = covered + 1;
            if (begin < (int)RC_OUTMATCH_MIN) begin = (int)RC_OUTMATCH_MIN;
            if (begin <= top) {
                from[nenv] = begin; to[nenv] = top; owner[nenv] = k; nenv++;
                covered = top;
            }
        }
        for (int l = (int)RC_OUTMATCH_MIN; l <= 96; l++) {
            int old_owner = -1, env_owner = -1;
            for (int k = 0; k < nr; k++)
                if (row[k].len >= l) { old_owner = k; break; }
            for (int e = 0; e < nenv; e++)
                if (from[e] <= l && l <= to[e]) { env_owner = owner[e]; break; }
            if (old_owner != env_owner) {
                fprintf(stderr, "out-envelope row mismatch pass=%d len=%d old=%d env=%d\n",
                        pass, l, old_owner, env_owner);
                return 1;
            }
        }
        if (pass == -1 &&
            (nenv != 2 || owner[0] != 0 || from[0] != 4 || to[0] != 12 ||
             owner[1] != 2 || from[1] != 13 || to[1] != 20)) return 1;
    }
    return 0;
}

static int out_adversarial_cases(void) {
    enum { SMALL_CAP = 32 };
    uint8_t content[SMALL_CAP] = {0}, tags[SMALL_CAP] = {0};
    uint8_t ncand[SMALL_CAP] = {0}, nocand[SMALL_CAP] = {0};
    CandArena cands = {0};
    OCandArena ocands = {0};
    PriceTab pt;

    out_random_prices(&pt, 1);
    CHECK(!out_emit_case("zero-row", 32, content, tags, &cands, ncand,
                     &ocands, nocand, &pt, -1, -1));

    memset(nocand, 0, sizeof(nocand));
    OCand nonmono[OC_MAX] = { {101, 12}, {202, 5}, {303, 20}, {404, 9} };
    out_put_orow(&ocands, nocand, 0, nonmono, OC_MAX);
    out_cheap_prices(&pt, 1);
    CHECK(!out_emit_case("nonmonotone-four", 20, content, tags, &cands, ncand,
                     &ocands, nocand, &pt, 303, 20));
    buf_free(&ocands);

    memset(nocand, 0, sizeof(nocand));
    OCand ties[OC_MAX] = { {111, 16}, {222, 16}, {333, 9}, {444, 16} };
    out_put_orow(&ocands, nocand, 0, ties, OC_MAX);
    CHECK(!out_emit_case("strict-first-tie", 16, content, tags, &cands, ncand,
                     &ocands, nocand, &pt, 111, 16));
    buf_free(&ocands);

    memset(nocand, 0, sizeof(nocand));
    OCand minimum[OC_MAX] = { {515, (int32_t)RC_OUTMATCH_MIN} };
    out_put_orow(&ocands, nocand, 0, minimum, 1);
    CHECK(!out_emit_case("minimum-four", RC_OUTMATCH_MIN, content, tags, &cands, ncand,
                     &ocands, nocand, &pt, 515, RC_OUTMATCH_MIN));
    buf_free(&ocands);

    memset(nocand, 0, sizeof(nocand));
    OCand disabled[OC_MAX] = { {616, 24}, {717, 12} };
    out_put_orow(&ocands, nocand, 0, disabled, 2);
    out_cheap_prices(&pt, 0);
    CHECK(!out_emit_case("disabled", 24, content, tags, &cands, ncand,
                     &ocands, nocand, &pt, -1, -1));
    buf_free(&ocands);

    size_t n = (size_t)LZ_MAX_MATCH + 17u;
    uint8_t *big_content = (uint8_t *)xcalloc(n, 1);
    uint8_t *big_tags = (uint8_t *)xcalloc(n, 1);
    uint8_t *big_ncand = (uint8_t *)xcalloc(n, 1);
    uint8_t *big_nocand = (uint8_t *)xcalloc(n, 1);
    OCand beyond[OC_MAX] = { {818, (int32_t)n} };
    out_put_orow(&ocands, big_nocand, 0, beyond, 1);
    out_cheap_prices(&pt, 1);
    int fail = out_emit_case("beyond-lz-max", n, big_content, big_tags, &cands, big_ncand,
                         &ocands, big_nocand, &pt, 818, (int32_t)n);
    free(big_content); free(big_tags); free(big_ncand); free(big_nocand);
    buf_free(&ocands);
    return fail;
}

static int out_randomized_cases(void) {
    enum { CASES = 320 };
    for (int c = 0; c < CASES; c++) {
        size_t n = 1u + out_rnd32() % 192u;
        uint8_t *content = (uint8_t *)xmalloc(n);
        uint8_t *tags = (uint8_t *)xmalloc(n);
        uint8_t *ncand = (uint8_t *)xcalloc(n, 1);
        uint8_t *nocand = (uint8_t *)xcalloc(n, 1);
        CandArena cands = {0}; OCandArena ocands = {0};
        for (size_t i = 0; i < n; i++) {
            if (i && (out_rnd32() & 3u)) {
                size_t back = 1u + out_rnd32() % (i < 24u ? i : 24u);
                content[i] = content[i - back];
            } else {
                content[i] = (uint8_t)(out_rnd32() & 15u);
            }
            tags[i] = (uint8_t)(out_rnd32() & 1u);
        }
        for (size_t i = 0; i < n; i++) {
            Cand row[LZ_CAND_MAX]; uint8_t nr = 0;
            if (i && n - i >= 3u) {
                int tries = (int)(out_rnd32() % 7u);
                for (int t = 0; t < tries && nr < LZ_CAND_MAX; t++) {
                    size_t dcap = i < (1u << WINDOW_LOG) ? i : (1u << WINDOW_LOG);
                    int32_t dist = 1 + (int32_t)(out_rnd32() % dcap);
                    size_t cap = n - i;
                    if (cap > LZ_MAX_MATCH) cap = LZ_MAX_MATCH;
                    size_t ml = 0, src = i - (size_t)dist;
                    while (ml < cap && content[src + ml] == content[i + ml]) ml++;
                    if (ml >= 3u) {
                        int32_t len = 3 + (int32_t)(out_rnd32() % (ml - 2u));
                        row[nr++] = (Cand){ dist, len };
                    }
                }
            }
            ncand[i] = nr;
            if (nr) buf_write(&cands, row, (size_t)nr * sizeof(*row));

            OCand orow[OC_MAX]; uint8_t no = 0;
            size_t remain = n - i;
            if (remain >= RC_OUTMATCH_MIN) {
                no = (uint8_t)(out_rnd32() % (OC_MAX + 1u));
                for (uint8_t k = 0; k < no; k++) {
                    orow[k].pos = (int32_t)(1u + out_rnd32() % 0x3fffffffu);
                    orow[k].len = (int32_t)RC_OUTMATCH_MIN +
                                  (int32_t)(out_rnd32() % (remain - RC_OUTMATCH_MIN + 1u));
                    if (k && (out_rnd32() & 3u) == 0u) orow[k].len = orow[k - 1u].len;
                }
            }
            out_put_orow(&ocands, nocand, i, orow, no);
        }
        PriceTab pt; out_random_prices(&pt, (c % 17) != 0);
        char name[32]; (void)snprintf(name, sizeof(name), "random-%03d", c);
        int fail = out_emit_case(name, n, content, tags, &cands, ncand,
                             &ocands, nocand, &pt, -1, -1);
        free(content); free(tags); free(ncand); free(nocand);
        buf_free(&cands); buf_free(&ocands);
        if (fail) return 1;
    }
    return 0;
}

static int out_envelope_cases(void) {
    if (out_row_envelope_cases() || out_adversarial_cases() || out_randomized_cases()) return 1;
    return 0;
}
