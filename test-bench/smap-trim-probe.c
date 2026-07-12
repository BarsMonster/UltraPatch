/* SPDX-License-Identifier: MIT */
#include "enc_internal.h"
#include <time.h>

extern uint64_t smap_pretrim_comparisons;
size_t smap_pretrim_probe(const uint32_t *weights, size_t n, size_t *survivors);
int smap_wire_feasible_probe(const uint32_t *mb, const int32_t *mv, int mn);
uint64_t smap_wire_price_probe(const uint32_t *mb, const int32_t *mv, int mn,
                               const FieldInjArena *inj, int fwd);

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "smap-trim check failed at line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static uint32_t rnd(uint32_t *s) {
    *s = *s * UINT32_C(1664525) + UINT32_C(1013904223);
    return *s;
}

static size_t old_trim(const uint32_t *weights, size_t n, size_t *out,
                       uint64_t *comparisons, uint64_t *moves) {
    uint32_t *w = (uint32_t *)xmalloc((n ? n : 1u) * sizeof(*w));
    size_t *id = (size_t *)xmalloc((n ? n : 1u) * sizeof(*id));
    for (size_t i = 0; i < n; i++) { w[i] = weights[i]; id[i] = i; }
    while (n > SMAP_POOL_MAX) {
        size_t worst = 0;
        for (size_t i = 1; i < n; i++) { (*comparisons)++; if (w[i] < w[worst]) worst = i; }
        *moves += n - worst - 1u;
        memmove(w + worst, w + worst + 1u, (n - worst - 1u) * sizeof(*w));
        memmove(id + worst, id + worst + 1u, (n - worst - 1u) * sizeof(*id));
        n--;
    }
    memcpy(out, id, n * sizeof(*out)); free(w); free(id);
    return n;
}

static uint64_t now_ns(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) die("clock_gettime failed");
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
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
    uint64_t no_map = smap_wire_price_probe(NULL, NULL, 0, &inj, 1);

    for (int i = 0; i <= UP_SMAP_CAP; i++) {
        pre_cap_b[i] = (uint32_t)i;
        pre_cap_v[i] = i;
    }

    CHECK(smap_wire_feasible_probe(NULL, NULL, 0));
    CHECK(!smap_wire_feasible_probe(NULL, NULL, -1));
    CHECK(smap_wire_feasible_probe(first_maxm1_b, zero_v, 1));
    CHECK(!smap_wire_feasible_probe(first_max_b, zero_v, 1));
    CHECK(smap_wire_feasible_probe(later_max_b, later_v, 2));
    CHECK(!smap_wire_feasible_probe(equal_b, later_v, 2));
    CHECK(!smap_wire_feasible_probe(descending_b, later_v, 2));
    CHECK(!smap_wire_feasible_probe(first_maxm1_b, min_v, 1));
    CHECK(smap_wire_feasible_probe(pre_cap_b, pre_cap_v, UP_SMAP_CAP + 1));
    CHECK(no_map < inf);
    CHECK(smap_wire_price_probe(first_maxm1_b, zero_v, 1, &inj, 1) < inf);
    CHECK(smap_wire_price_probe(later_max_b, later_v, 2, &inj, 0) < inf);
    CHECK(smap_wire_price_probe(pre_cap_b, pre_cap_v, UP_SMAP_CAP + 1, &inj, 1) < inf);
    CHECK(smap_wire_price_probe(first_max_b, zero_v, 1, &inj, 1) == inf);
    CHECK(smap_wire_price_probe(first_maxm1_b, min_v, 1, &inj, 1) == inf);
    CHECK(!(smap_wire_price_probe(first_max_b, zero_v, 1, &inj, 1) < no_map));
    puts("smap_wire_feasibility=OK empty/pre-cap/max-1/later-max/zigzag/mixed-BL-EX");
    return 0;
}

int main(void) {
    enum { MAXP = 512, STRESS_P = 8192, REPS = 3 };
    uint32_t weights[STRESS_P], seed = UINT32_C(0xa341316c);
    size_t oldv[STRESS_P], newv[STRESS_P];
    if (check_wire_feasibility()) return 1;
    for (int c = 0; c < 256; c++) {
        size_t n = (size_t)(rnd(&seed) % MAXP);
        for (size_t i = 0; i < n; i++) {
            uint32_t r = rnd(&seed);
            weights[i] = c % 4 == 0 ? 7u : c % 4 == 1 ? (uint32_t)(i % 3u)
                       : c % 4 == 2 ? r & 7u : r;
        }
        uint64_t comparisons = 0, moves = 0;
        size_t on = old_trim(weights, n, oldv, &comparisons, &moves);
        size_t nn = smap_pretrim_probe(weights, n, newv);
        CHECK(on == nn && !memcmp(oldv, newv, on * sizeof(*oldv)));
    }
    printf("smap_trim_property=OK cases=256 tie_heavy=128\n");

    for (size_t i = 0; i < STRESS_P; i++) weights[i] = 1u;
    weights[STRESS_P - 1u] = INT32_MAX; /* terminator-shaped maximal final entry */
    uint64_t old_cmp = 0, old_moves = 0, t0 = now_ns();
    size_t on = 0, nn = 0;
    for (int r = 0; r < REPS; r++) on = old_trim(weights, STRESS_P, oldv, &old_cmp, &old_moves);
    uint64_t old_ns = now_ns() - t0;
    smap_pretrim_comparisons = 0; t0 = now_ns();
    for (int r = 0; r < REPS; r++) nn = smap_pretrim_probe(weights, STRESS_P, newv);
    uint64_t new_ns = now_ns() - t0;
    CHECK(on == nn && !memcmp(oldv, newv, on * sizeof(*oldv)));
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < nn; i++) { hash ^= newv[i]; hash *= UINT64_C(1099511628211); }
    printf("smap_trim_stress=P:%u reps:%u old_cmp:%llu old_moves:%llu new_cmp:%llu old_ns:%llu new_ns:%llu survivors:%zu first:%zu last:%zu hash:%016llx\n",
           STRESS_P, REPS, (unsigned long long)old_cmp, (unsigned long long)old_moves,
           (unsigned long long)smap_pretrim_comparisons, (unsigned long long)old_ns,
           (unsigned long long)new_ns, nn, newv[0], newv[nn - 1u], (unsigned long long)hash);
    return 0;
}
