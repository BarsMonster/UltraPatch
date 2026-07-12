/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 *
 * Pinned semantic streams for encoder field kernels. The real private module is included so
 * no test-only production interface is required.
 */
#include "enc_internal.h"
#include "../src/enc_field.c"
#include "../src/enc_emit.c"

static FILE *ldr_records, *smap_records;
static uint64_t ldr_record_count, smap_record_count;

static int check_query(const LdrTargetIndex *idx, int32_t fp0, int32_t dl,
                       uint32_t fpk, int expected) {
    int got = ldr_target_index_query(idx, fp0, dl, fpk);
    fprintf(ldr_records, "query\tfp0=%d\tdl=%d\tfpk=%u\tresult=%d\n",
            fp0, dl, fpk, got);
    ldr_record_count++;
    if (expected >= 0 && got != expected) {
        fprintf(stderr,
                "ldr-index mismatch fp0=%d dl=%d fpk=%u result=%d expected=%d\n",
                fp0, dl, fpk, got, expected);
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

static int arithmetic_boundaries(void) {
    int fail = 0;
#define REQUIRE(x) do { if (!(x)) { \
    fprintf(stderr, "ldr arithmetic boundary failed: %s\n", #x); fail = 1; \
} } while (0)
    REQUIRE(rc_ldr_target(0u, 0u) == 4u);
    REQUIRE(rc_ldr_target(2u, 0u) == 4u);
    REQUIRE(rc_ldr_target(3u, 1u) == 8u);
    REQUIRE(rc_ldr_target(0x7ffffffeu, 255u) == 0x800003fcu);
    REQUIRE(rc_ldr_target(UINT32_MAX, 0u) == 0u);

    REQUIRE(rc_ldr_target_in_op(10, 22, 28u));             /* exact endpoint */
    REQUIRE(!rc_ldr_target_in_op(10, 21, 28u));            /* crosses endpoint */
    REQUIRE(!rc_ldr_target_in_op(10, -1, 12u));            /* negative length */
    REQUIRE(!rc_ldr_target_in_op(INT32_MAX, 1, 0u));       /* signed end overflow */
    REQUIRE(rc_ldr_target_in_op(INT32_MAX - 7, 7, 0x7ffffff8u));
    REQUIRE(!rc_ldr_target_in_op(0, INT32_MAX, UINT32_MAX - 3u));
    REQUIRE(rc_ldr_target_in_op(-7, 15, 4u));              /* negative origin */
    REQUIRE(!rc_ldr_target_in_op(-7, 10, 0u));             /* end before one word */
    REQUIRE(!rc_ldr_target_in_op(0, 8, 1u));               /* unaligned target */
#undef REQUIRE
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

static uint32_t smap_rnd(uint32_t *s) {
    *s = *s * UINT32_C(1664525) + UINT32_C(1013904223);
    return *s;
}

static int check_wire_feasibility(void) {
    static FieldInj fields[] = {
        { 0u, EV_BL, 4u, 7, 12u },
        { 0u, EV_EX, 0u, -9, 20u }
    };
    FieldInjArena inj = { fields, 2u, 2u };
    uint32_t first_max_b[] = { UINT32_MAX };
    uint32_t first_maxm1_b[] = { UINT32_MAX - 1u };
    uint32_t later_max_b[] = { 0u, UINT32_MAX };
    uint32_t equal_b[] = { 1u, 1u };
    uint32_t descending_b[] = { 2u, 1u };
    uint32_t pre_cap_b[UP_SMAP_CAP + 1];
    int32_t pre_cap_v[UP_SMAP_CAP + 1];
    int32_t zero_v[] = { 0 };
    int32_t later_v[] = { INT32_MAX, 1 };
    int32_t min_v[] = { INT32_MIN };
    uint64_t inf = UINT64_MAX / 2u;
    int32_t dic_bl[DR_KCAP_BL], dic_ex[DR_KCAP_EX];
    uint64_t no_map = px_map_total(NULL, NULL, 0, &inj, 1, dic_bl, dic_ex);
#define REQUIRE(x) do { if (!(x)) { \
    fprintf(stderr, "smap boundary failed: %s\n", #x); return 1; \
} } while (0)
    for (int i = 0; i <= UP_SMAP_CAP; i++) {
        pre_cap_b[i] = (uint32_t)i;
        pre_cap_v[i] = i;
    }
    REQUIRE(smap_wire_feasible(NULL, NULL, 0));
    REQUIRE(!smap_wire_feasible(NULL, NULL, -1));
    REQUIRE(smap_wire_feasible(first_maxm1_b, zero_v, 1));
    REQUIRE(!smap_wire_feasible(first_max_b, zero_v, 1));
    REQUIRE(smap_wire_feasible(later_max_b, later_v, 2));
    REQUIRE(!smap_wire_feasible(equal_b, later_v, 2));
    REQUIRE(!smap_wire_feasible(descending_b, later_v, 2));
    REQUIRE(!smap_wire_feasible(first_maxm1_b, min_v, 1));
    REQUIRE(smap_wire_feasible(pre_cap_b, pre_cap_v, UP_SMAP_CAP + 1));
    REQUIRE(no_map < inf);
    REQUIRE(px_map_total(first_maxm1_b, zero_v, 1, &inj, 1, dic_bl, dic_ex) < inf);
    REQUIRE(px_map_total(later_max_b, later_v, 2, &inj, 0, dic_bl, dic_ex) < inf);
    REQUIRE(px_map_total(pre_cap_b, pre_cap_v, UP_SMAP_CAP + 1,
                         &inj, 1, dic_bl, dic_ex) < inf);
    REQUIRE(px_map_total(first_max_b, zero_v, 1, &inj, 1, dic_bl, dic_ex) == inf);
    REQUIRE(px_map_total(first_maxm1_b, min_v, 1, &inj, 1, dic_bl, dic_ex) == inf);
    REQUIRE(!(px_map_total(first_max_b, zero_v, 1, &inj, 1, dic_bl, dic_ex) < no_map));
#undef REQUIRE
    return 0;
}

static int record_trim_case(unsigned c, const uint32_t *weights, size_t n) {
    SegCand *pool = (SegCand *)xmalloc((n ? n : 1u) * sizeof(*pool));
    for (size_t i = 0; i < n; i++)
        pool[i] = (SegCand){(uint32_t)i, (int32_t)i, weights[i]};
    size_t out = smap_pretrim(pool, n);
    fprintf(smap_records, "case=%u\tn=%zu\tweights=", c, n);
    for (size_t i = 0; i < n; i++) fprintf(smap_records, "%s%u", i ? "," : "", weights[i]);
    fputs("\tsurvivors=", smap_records);
    for (size_t i = 0; i < out; i++) fprintf(smap_records, "%s%u", i ? "," : "", pool[i].b);
    fputc('\n', smap_records);
    smap_record_count++;
    if (out != (n < SMAP_POOL_MAX ? n : SMAP_POOL_MAX)) {
        fprintf(stderr, "smap survivor count failed case=%u n=%zu out=%zu\n", c, n, out);
        free(pool); return 1;
    }
    free(pool);
    return 0;
}

static int smap_cases(void) {
    enum { MAXP = 512, STRESS_P = 8192 };
    uint32_t weights[STRESS_P], seed = UINT32_C(0xa341316c);
    if (check_wire_feasibility()) return 1;
    for (unsigned c = 0; c < 256; c++) {
        size_t n = (size_t)(smap_rnd(&seed) % MAXP);
        for (size_t i = 0; i < n; i++) {
            uint32_t r = smap_rnd(&seed);
            weights[i] = c % 4 == 0 ? 7u : c % 4 == 1 ? (uint32_t)(i % 3u)
                       : c % 4 == 2 ? r & 7u : r;
        }
        if (record_trim_case(c, weights, n)) return 1;
    }
    for (size_t i = 0; i < STRESS_P; i++) weights[i] = 1u;
    weights[STRESS_P - 1u] = INT32_MAX;
    if (record_trim_case(256, weights, STRESS_P)) return 1;
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s LDR_RECORDS SMAP_RECORDS\n", argv[0]);
        return 2;
    }
    ldr_records = fopen(argv[1], "wb");
    smap_records = fopen(argv[2], "wb");
    if (!ldr_records || !smap_records) die("open field record stream");
    int fail = arithmetic_boundaries() || boundary_cases() || randomized_cases() || smap_cases();
    if (fclose(ldr_records) || fclose(smap_records)) die("close field record stream");
    if (fail) return 1;
    printf("ldr_index_results=OK arithmetic=14 boundary=10 randomized=65536 records=%llu\n",
           (unsigned long long)ldr_record_count);
    printf("smap_trim_results=OK cases=256 tie_heavy=128 stress=8192 records=%llu "
           "wire_feasibility=OK\n", (unsigned long long)smap_record_count);
    return 0;
}
