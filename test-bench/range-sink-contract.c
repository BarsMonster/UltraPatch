/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 *
 * Exact range-encoder sink oracle: the count-only sink must advance the same carry/cache core and
 * report the same optimized flush length as the material sink for crafted carry chains and
 * deterministic randomized bit streams.
 */
#include "enc_internal.h"

#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)

static int same_core(const REnc *material, const REnc *counted) {
    CHECK(material->low == counted->low);
    CHECK(material->range == counted->range);
    CHECK(material->cache == counted->cache);
    CHECK(material->csz == counted->csz);
    CHECK(material->coding_overflow == counted->coding_overflow);
    CHECK(material->out.n == counted->out.n);
    CHECK(counted->count_only && counted->out.d == NULL && counted->out.cap == 0);
    return 0;
}

static int finish_pair(REnc *material, REnc *counted) {
    size_t base = material->out.n;
    Buf bytes = re_flush_opt(material);
    Buf count = re_flush_opt(counted);
    CHECK(count.d == NULL && count.cap == 0);
    CHECK(count.n == bytes.n);
    CHECK(bytes.n >= base); /* optimized flush must never trim a preexisting body byte */
    buf_free(&bytes);
    return 0;
}

static int check_carry_chain(void) {
    REnc material, counted;
    re_init(&material);
    re_init_count(&counted);
    material.low = counted.low = (uint64_t)UINT32_MAX + 1u;
    material.range = counted.range = RC_KTOP;
    material.cache = counted.cache = 0xfeu;
    material.csz = counted.csz = 8u;
    re_raw(&material, 0);
    re_raw(&counted, 0);
    CHECK(same_core(&material, &counted) == 0);
    CHECK(material.out.n == 8u && material.out.d[0] == 0xffu);
    for (size_t i = 1; i < material.out.n; i++) CHECK(material.out.d[i] == 0u);
    /* The seven body zeros above precede the flush base and therefore must survive even when the
     * flush itself ends in zeros. This directly exercises the base-bounded trim rule. */
    return finish_pair(&material, &counted);
}

static uint32_t lcg(uint32_t *state) {
    *state = *state * 1664525u + 1013904223u;
    return *state;
}

static int check_random_streams(void) {
    uint32_t seed = 0x91e10da5u;
    for (int run = 0; run < 256; run++) {
        REnc material, counted;
        uint16_t mp = (uint16_t)(1u + lcg(&seed) % (RC_PBIT - 1u));
        uint16_t cp = mp;
        size_t n = lcg(&seed) % 20001u;
        re_init(&material);
        re_init_count(&counted);
        for (size_t i = 0; i < n; i++) {
            uint32_t r = lcg(&seed);
            int bit = (int)((r >> 31) & 1u);
            if (r & 3u) {
                re_raw(&material, bit);
                re_raw(&counted, bit);
            } else {
                int rate = 1 + (int)((r >> 8) % 7u);
                re_bit(&material, &mp, bit, rate);
                re_bit(&counted, &cp, bit, rate);
                CHECK(mp == cp);
            }
            CHECK(same_core(&material, &counted) == 0);
        }
        CHECK(finish_pair(&material, &counted) == 0);
    }
    return 0;
}

int main(void) {
    int r = check_carry_chain();
    if (r) return r;
    return check_random_streams();
}
