/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 *
 * Exact oracle for the encoder-only source LDR-target index. The reference is the former
 * per-query backward scan; randomized geometry and explicit boundary cases keep its op-local
 * eligibility semantics pinned independently of the indexed implementation.
 */
#include "enc_internal.h"

static uint64_t queries, scan_halfwords;

static int old_query(const uint8_t *src, uint32_t source_size,
                     int32_t fp0, int32_t dl, uint32_t fpk) {
    if (fpk + 4u > source_size || !rc_ldr_target_in_op(fp0, dl, fpk)) return 0;
    for (int32_t a = rc_ldr_scan_first(fp0, fpk); a + 2 <= (int32_t)fpk; a += 2) {
        scan_halfwords++;
        uint16_t up = rc_u16le(src + a);
        if (rc_thumb_ldr_lit(up) &&
            rc_ldr_target(a, (int32_t)(up & 0xffu)) == (int32_t)fpk)
            return 1;
    }
    return 0;
}

static int check_query(const LdrTargetIndex *idx, int32_t fp0, int32_t dl,
                       uint32_t fpk, int expected) {
    int old = old_query(idx->source, idx->source_size, fp0, dl, fpk);
    int got = ldr_target_index_query(idx, fp0, dl, fpk);
    queries++;
    if (old != got || (expected >= 0 && got != expected)) {
        fprintf(stderr,
                "ldr-index mismatch fp0=%d dl=%d fpk=%u old=%d index=%d expected=%d\n",
                fp0, dl, fpk, old, got, expected);
        return 1;
    }
    return 0;
}

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

static int boundary_cases(void) {
    enum { N = 1032 };
    uint8_t src[N]; memset(src, 0, sizeof(src));
    /* Adjacent halfwords both target word 4: the nearest one must decide odd op starts. */
    put16(src + 0, 0x4800u); put16(src + 2, 0x4800u);
    /* imm8=255 is the maximum 1024-byte backward reach: address 4 -> target 1028. */
    put16(src + 4, 0x48ffu);
    LdrTargetIndex idx; ldr_target_index_build(&idx, src, N);
    int fail = 0;
    fail |= check_query(&idx, 0, 8, 4, 1);       /* target word ends exactly at op end */
    fail |= check_query(&idx, 1, 7, 4, 1);       /* odd start includes instruction at 2 */
    fail |= check_query(&idx, 3, 5, 4, 0);       /* odd start excludes both instructions */
    fail |= check_query(&idx, -7, 15, 4, 1);     /* negative/odd source origin clamps to zero */
    fail |= check_query(&idx, 4, 4, 4, 0);       /* target at op start; producer is outside */
    fail |= check_query(&idx, 0, 7, 4, 0);       /* target word crosses op end */
    fail |= check_query(&idx, 4, 1028, 1028, 1); /* maximum imm8 reach, exact op end */
    fail |= check_query(&idx, 5, 1027, 1028, 0); /* odd boundary excludes producer at 4 */
    fail |= check_query(&idx, 0, N, 5, 0);       /* unaligned target */
    fail |= check_query(&idx, 0, N + 4, N, 0);   /* target outside source */
    ldr_target_index_free(&idx);
    return fail;
}

static uint32_t rnd_state = 0x6c647269u;
static uint32_t rnd32(void) {
    rnd_state = rnd_state * 1664525u + 1013904223u;
    return rnd_state;
}

static int randomized_cases(void) {
    enum { MAX_N = 4096, CASES = 256, QUERIES = 256 };
    uint8_t src[MAX_N];
    for (int c = 0; c < CASES; c++) {
        uint32_t n = rnd32() % (MAX_N + 1u);
        for (uint32_t i = 0; i < n; i++) src[i] = (uint8_t)(rnd32() >> 24);
        /* Force overlapping/raw halfword candidates in addition to incidental random LDRs. */
        for (uint32_t j = 0; j < 24u && n >= 2u; j++) {
            uint32_t a = (rnd32() % (n / 2u)) * 2u;
            if (a + 2u <= n) put16(src + a, (uint16_t)(0x4800u | (rnd32() & 0xffu)));
        }
        LdrTargetIndex idx; ldr_target_index_build(&idx, src, n);
        for (int q = 0; q < QUERIES; q++) {
            int32_t fp0 = (int32_t)(rnd32() % (n + 129u)) - 64;
            int32_t dl = (int32_t)(rnd32() % (n + 129u));
            uint32_t fpk = rnd32() % (n + 9u);
            if ((q & 3) == 0) fpk &= ~3u;
            if (check_query(&idx, fp0, dl, fpk, -1)) {
                ldr_target_index_free(&idx);
                return 1;
            }
        }
        ldr_target_index_free(&idx);
    }
    return 0;
}

int main(void) {
    if (boundary_cases() || randomized_cases()) return 1;
    printf("ldr_index_oracle=OK boundary=10 randomized=65536 queries=%llu scan_halfwords=%llu\n",
           (unsigned long long)queries, (unsigned long long)scan_halfwords);
    return 0;
}
