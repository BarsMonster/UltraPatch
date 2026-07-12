/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 *
 * Pinned semantic stream for bsdiff's boundary-LCP suffix search and complete operations.
 * The real private module is included so no test-only production interface is required.
 */
#include "enc_internal.h"
#include "../src/enc_bsdiff.c"

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "encoder bsdiff probe failed at line %d: %s\n", __LINE__, #x); \
    return 1; \
} } while (0)

static uint64_t query_count, pair_count;
static FILE *records;

static int32_t *make_sa(const uint8_t *from, int32_t n) {
    int32_t *sa = (int32_t *)xmalloc(((size_t)n + 1u) * sizeof(*sa));
    sa[0] = n;
    if (n && divsufsort(from, &sa[1], n) != 0) die("divsufsort failed");
    return sa;
}

static int run_query(const char *name, const int32_t *sa,
                     const uint8_t *from, int32_t from_size,
                     const uint8_t *query, int32_t query_size,
                     int32_t begin, int32_t end,
                     int32_t expected_pos, int32_t expected_len) {
    int32_t new_pos = -1;
    int32_t new_len = suffix_search(sa, from, from_size, query, query_size,
                                    begin, end, &new_pos);
    if ((expected_pos != INT32_MIN && new_pos != expected_pos) ||
        (expected_len != INT32_MIN && new_len != expected_len)) {
        fprintf(stderr, "suffix-LCP unexpected result case=%s got=%d/%d expected=%d/%d\n",
                name, new_pos, new_len, expected_pos, expected_len);
        return 1;
    }
    fprintf(records, "query=%s\tfrom=%d\tq=%d\tbounds=%d,%d\tresult=%d,%d\n",
            name, from_size, query_size, begin, end, new_pos, new_len);
    query_count++;
    return 0;
}

static int find_sa_pos(const int32_t *sa, int32_t n, int32_t pos) {
    for (int32_t i = 0; i <= n; i++) if (sa[i] == pos) return i;
    return -1;
}

static int adversarial_queries(void) {
    static const uint8_t dummy = 0;
    int32_t sa_empty[1] = {0};
    CHECK(!run_query("empty-empty", sa_empty, &dummy, 0, &dummy, 0,
                     0, 0, 0, 0));
    static const uint8_t q_nonempty[] = "query";
    CHECK(!run_query("empty-source", sa_empty, &dummy, 0,
                     q_nonempty, 5, 0, 0, 0, 0));

    static const uint8_t abc[] = "abc";
    int32_t *sa = make_sa(abc, 3);
    CHECK(!run_query("empty-query", sa, abc, 3, &dummy, 0,
                     0, 3, sa[1], 0));
    CHECK(!run_query("identical", sa, abc, 3, abc, 3,
                     0, 3, 0, 3));
    static const uint8_t abc_more[] = "abcX";
    CHECK(!run_query("suffix-prefix-exhaustion", sa, abc, 3, abc_more, 4,
                     0, 3, 0, 3));
    free(sa);

    sa = make_sa(abc_more, 4);
    CHECK(!run_query("query-prefix-exhaustion", sa, abc_more, 4, abc, 3,
                     0, 4, 0, 3));
    free(sa);

    static const uint8_t rep[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    sa = make_sa(rep, 32);
    CHECK(!run_query("repetitive-identical", sa, rep, 32, rep, 32,
                     0, 32, 31, 1));
    static const uint8_t rep_long[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaZ";
    CHECK(!run_query("repetitive-suffix-shorter", sa, rep, 32, rep_long, 33,
                     0, 32, 31, 1));
    free(sa);

    uint8_t long_from[515], long_query[258];
    memset(long_from, 'L', sizeof(long_from));
    memset(long_query, 'L', sizeof(long_query));
    long_from[257] = 'A'; long_from[514] = 'Z'; long_query[257] = 'M';
    sa = make_sa(long_from, (int32_t)sizeof(long_from));
    CHECK(!run_query("long-common-prefix", sa, long_from, (int32_t)sizeof(long_from),
                     long_query, (int32_t)sizeof(long_query),
                     0, (int32_t)sizeof(long_from), INT32_MIN, INT32_MIN));
    free(sa);

    /* Adjacent suffixes "axay" and "ay" both match one byte of "az". The legacy leaf uses
     * `x > y`, so equality must choose the END boundary (the "ay" suffix at position 2). */
    static const uint8_t tie_from[] = "axay", tie_query[] = "az";
    sa = make_sa(tie_from, 4);
    int32_t ib = find_sa_pos(sa, 4, 0), ie = find_sa_pos(sa, 4, 2);
    CHECK(ib >= 0 && ie == ib + 1);
    CHECK(matchlen(tie_from + sa[ib], 4 - sa[ib], tie_query, 2) == 1);
    CHECK(matchlen(tie_from + sa[ie], 4 - sa[ie], tie_query, 2) == 1);
    CHECK(!run_query("adjacent-leaf-equal-tie", sa, tie_from, 4, tie_query, 2,
                     ib, ie, 2, 1));
    free(sa);

    /* Both custom boundaries prove one equal byte. The middle suffix mismatches at exactly
     * that boundary, pinning the skip point rather than merely a long match after it. */
    static const uint8_t boundary_from[] = "aaZabZacZ", boundary_query[] = "ad";
    int32_t boundary_sa[3] = {0, 3, 6};
    CHECK(!run_query("mismatch-at-proven-boundary", boundary_sa,
                     boundary_from, 9, boundary_query, 2, 0, 2,
                     INT32_MIN, INT32_MIN));
    return 0;
}

static uint32_t rnd_state = 0x6c637031u;
static uint32_t rnd32(void) {
    rnd_state = rnd_state * 1664525u + 1013904223u;
    return rnd_state;
}

static int randomized_queries(void) {
    enum { SOURCES = 256, QUERIES = 32 };
    for (int c = 0; c < SOURCES; c++) {
        int32_t n = (int32_t)(rnd32() % 257u);
        uint8_t *from = (uint8_t *)xmalloc(n ? (size_t)n : 1u);
        for (int32_t i = 0; i < n; i++) {
            if ((c & 3) == 0) from[i] = (uint8_t)rnd32();
            else if ((c & 3) == 1) from[i] = 0x5au;
            else if ((c & 3) == 2) from[i] = (uint8_t)('a' + i % 5);
            else from[i] = (uint8_t)((i < n - 3) ? 0x33 : rnd32());
        }
        int32_t *sa = make_sa(from, n);
        for (int qn = 0; qn < QUERIES; qn++) {
            int32_t m = (int32_t)(rnd32() % 129u);
            uint8_t *query = (uint8_t *)xmalloc(m ? (size_t)m : 1u);
            for (int32_t i = 0; i < m; i++) query[i] = (uint8_t)rnd32();
            if (n && m && (qn & 3) != 0) {
                int32_t p = (int32_t)(rnd32() % (uint32_t)n);
                int32_t copy = n - p < m ? n - p : m;
                memcpy(query, from + p, (size_t)copy);
                if ((qn & 3) == 2 && copy) query[copy - 1] ^= 1u;
                if ((qn & 3) == 3 && copy < m)
                    memset(query + copy, (int)(rnd32() & 0xffu), (size_t)(m - copy));
            }
            int32_t begin = 0, end = n;
            if (qn & 1) {
                begin = (int32_t)(rnd32() % ((uint32_t)n + 1u));
                end = begin + (int32_t)(rnd32() % ((uint32_t)(n - begin) + 1u));
            }
            char name[40];
            (void)snprintf(name, sizeof(name), "random-%03d-%02d", c, qn);
            int fail = run_query(name, sa, from, n, query, m, begin, end,
                                 INT32_MIN, INT32_MIN);
            free(query);
            if (fail) { free(sa); free(from); return 1; }
        }
        free(sa); free(from);
    }
    return 0;
}

static int verify_ops(const OpVec *ops, const uint8_t *from, size_t from_n,
                      const uint8_t *to, size_t to_n) {
    int64_t fp = 0; size_t tp = 0;
    for (size_t oi = 0; oi < ops->n; oi++) {
        const Op *o = &ops->v[oi];
        if (o->diff_len < 0 || o->extra_len < 0 ||
            (size_t)o->diff_len > to_n - tp ||
            fp < 0 || (uint64_t)fp + (uint32_t)o->diff_len > from_n) return 1;
        for (int32_t k = 0; k < o->diff_len; k++)
            if ((uint8_t)(from[(size_t)fp + (size_t)k] + ops->payload[tp + (size_t)k]) !=
                to[tp + (size_t)k]) return 1;
        tp += (size_t)o->diff_len; fp += o->diff_len;
        if ((size_t)o->extra_len > to_n - tp) return 1;
        if (memcmp(ops->payload + tp, to + tp, (size_t)o->extra_len) != 0) return 1;
        tp += (size_t)o->extra_len; fp += o->adj;
    }
    return tp == to_n ? 0 : 1;
}

static int run_pair(const char *name, const uint8_t *from, size_t from_n,
                    const uint8_t *to, size_t to_n, int fuzz) {
    Buf f = { (uint8_t *)from, from_n, from_n };
    Buf t = { (uint8_t *)to, to_n, to_n };
    OpVec ops = bsdiff_ops(&f, &t, fuzz);
    if (verify_ops(&ops, from, from_n, to, to_n)) {
        fprintf(stderr, "suffix-LCP invalid bsdiff ops case=%s\n", name);
        opvec_free(&ops);
        return 1;
    }
    fprintf(records, "pair=%s\tfrom=%zu\tto=%zu\tfuzz=%d\tops=%zu", name, from_n, to_n, fuzz, ops.n);
    for (size_t i = 0; i < ops.n; i++)
        fprintf(records, "%s%d,%d,%d", i ? "|" : "\t", ops.v[i].diff_len,
                ops.v[i].extra_len, ops.v[i].adj);
    fputs("\tpayload=", records);
    for (size_t i = 0; i < to_n; i++) fprintf(records, "%02x", ops.payload[i]);
    fputc('\n', records);
    opvec_free(&ops); pair_count++;
    return 0;
}

static int bsdiff_cases(void) {
    static const uint8_t dummy = 0;
    static const uint8_t short_a[] = "abracadabra";
    static const uint8_t short_b[] = "abraXadabra!";
    CHECK(!run_pair("empty-empty", &dummy, 0, &dummy, 0, 11));
    CHECK(!run_pair("empty-target", short_a, 11, &dummy, 0, 11));
    CHECK(!run_pair("empty-source", &dummy, 0, short_a, 11, 11));
    CHECK(!run_pair("identical", short_a, 11, short_a, 11, 11));
    CHECK(!run_pair("short-edit", short_a, 11, short_b, 12, 6));

    uint8_t repetitive_a[768], repetitive_b[769];
    for (size_t i = 0; i < sizeof(repetitive_a); i++) repetitive_a[i] = (uint8_t)('a' + i % 3u);
    memcpy(repetitive_b, repetitive_a, sizeof(repetitive_a));
    repetitive_b[383] = 'Z'; repetitive_b[768] = '!';
    CHECK(!run_pair("repetitive-long-prefix", repetitive_a, sizeof(repetitive_a),
                    repetitive_b, sizeof(repetitive_b), 20));

    enum { RANDOM_PAIRS = 128 };
    for (int c = 0; c < RANDOM_PAIRS; c++) {
        size_t fn = rnd32() % 385u, tn = rnd32() % 385u;
        uint8_t *from = (uint8_t *)xmalloc(fn ? fn : 1u);
        uint8_t *to = (uint8_t *)xmalloc(tn ? tn : 1u);
        for (size_t i = 0; i < fn; i++)
            from[i] = (c & 1) ? (uint8_t)('A' + i % 7u) : (uint8_t)rnd32();
        for (size_t i = 0; i < tn; i++) {
            if (fn && (rnd32() & 3u)) {
                size_t p = (i + (size_t)(c % 17)) % fn;
                to[i] = from[p];
                if ((rnd32() & 31u) == 0u) to[i] ^= (uint8_t)(1u + rnd32() % 255u);
            } else to[i] = (uint8_t)rnd32();
        }
        char name[32]; (void)snprintf(name, sizeof(name), "random-%03d", c);
        int fail = run_pair(name, from, fn, to, tn, c % 3 == 0 ? 6 : (c % 3 == 1 ? 11 : 20));
        free(from); free(to);
        if (fail) return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s RECORDS\n", argv[0]); return 2; }
    records = fopen(argv[1], "wb"); if (!records) die("open bsdiff record stream");
    int fail = adversarial_queries() || randomized_queries() || bsdiff_cases();
    if (fclose(records)) die("close bsdiff record stream");
    if (fail) return 1;
    printf("suffix_lcp_results=OK queries=%llu randomized=8192 empty=OK repetitive=OK "
           "long_prefix=OK prefix_exhaustion=both adjacent=OK boundary_mismatch=OK "
           "leaf_ties=OK bsdiff_pairs=%llu ops=OK payloads=OK\n",
           (unsigned long long)query_count, (unsigned long long)pair_count);
    return 0;
}
