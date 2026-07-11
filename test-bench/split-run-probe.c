/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 *
 * Direct contract probe for split_nonzero_diff_runs' per-plan transition budget.
 */
#include "enc_internal.h"

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "split-run check failed at line %d: %s\n", __LINE__, #x); \
    return 1; \
} } while (0)

enum { HALF = 64, TOTAL = 2 * HALF };

static OpVec fixture(void) {
    OpVec v = {0};
    v.payload = (uint8_t *)xmalloc(TOTAL);
    memset(v.payload, 0x44, TOTAL);
    opvec_push(&v, (Op){ HALF, 0, 0 });
    opvec_push(&v, (Op){ HALF, 0, 0 });
    return v;
}

static int same(const OpVec *a, const OpVec *b) {
    return a->n == b->n &&
           (!a->n || memcmp(a->v, b->v, a->n * sizeof(*a->v)) == 0) &&
           memcmp(a->payload, b->payload, TOTAL) == 0;
}

int main(void) {
    Buf from = { (uint8_t *)xmalloc(TOTAL), TOTAL, TOTAL };
    Buf to = { (uint8_t *)xmalloc(TOTAL), TOTAL, TOTAL };
    memset(from.d, 0x11, TOTAL);
    memset(to.d, 0x55, TOTAL);
    EncCtx ctx = { .fwd = 1 };

    OpVec original = fixture(), exact = fixture(), unlimited = fixture();
    split_nonzero_diff_runs_probe(&ctx, &exact, &from, &to, 2);
    split_nonzero_diff_runs_probe(&ctx, &unlimited, &from, &to, UINT64_MAX);
    CHECK(same(&exact, &unlimited)); /* under-budget decisions match an uncapped plan */

    OpVec none = fixture();
    split_nonzero_diff_runs_probe(&ctx, &none, &from, &to, 0);
    CHECK(same(&none, &original)); /* an over-budget first op retains geometry and payload */

    OpVec first = fixture(), again = fixture();
    split_nonzero_diff_runs_probe(&ctx, &first, &from, &to, 1);
    split_nonzero_diff_runs_probe(&ctx, &again, &from, &to, 1);
    CHECK(same(&first, &again)); /* each plan call receives a fresh budget */
    CHECK(first.n == 2u);
    CHECK(first.v[0].diff_len == 0 && first.v[0].extra_len == HALF &&
          first.v[0].adj == HALF);
    CHECK(first.v[1].diff_len == HALF && first.v[1].extra_len == 0 &&
          first.v[1].adj == 0); /* the first op consumed the plan's sole transition */
    for (size_t i = 0; i < HALF; i++) CHECK(first.payload[i] == 0x55u);
    for (size_t i = HALF; i < TOTAL; i++) CHECK(first.payload[i] == 0x44u);

    opvec_free(&original); opvec_free(&exact); opvec_free(&unlimited);
    opvec_free(&none); opvec_free(&first); opvec_free(&again);
    buf_free(&from); buf_free(&to);
    puts("split_run_budget=OK");
    return 0;
}
