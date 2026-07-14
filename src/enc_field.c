/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 *
 * Host encoder module -- relocation indexes, shift-map candidates, and split-run planning.
 * Compiled as a normal internal encoder translation unit.
 */

#include "enc_internal.h"
void ldr_target_index_build(LdrTargetIndex *idx, const uint8_t *source, uint32_t source_size) {
    memset(idx, 0, sizeof(*idx));
    size_t nwords = ((size_t)source_size + 3u) >> 2;
    idx->back = (uint16_t *)xcalloc(nwords ? nwords : 1u, sizeof(*idx->back));
    for (uint32_t a = 0; a + 2u <= source_size; a += 2u) {
        uint16_t up = rc_u16le(source + a);
        if (!rc_thumb_ldr_lit(up)) continue;
        uint32_t t = rc_ldr_target(a, up & 0xffu);
        if (t > source_size || source_size - t < 4u) continue;
        uint16_t back = (uint16_t)(t - a);           /* exact domain: 2..1024 */
        uint16_t *slot = &idx->back[t >> 2];
        if (!*slot || back < *slot) *slot = back;    /* nearest instruction is sufficient */
    }
}

void ldr_target_index_free(LdrTargetIndex *idx) {
    free(idx->back);
    memset(idx, 0, sizeof(*idx));
}

/* ---- piecewise shift map (D1): lookups on both sides use the single rc_smap_at definition in
 * rc_models.h. ---- */

typedef struct { uint32_t b; int32_t v; uint32_t w, ord; } SegCand;
typedef struct { uint32_t w; size_t ord; } SegRank;
static int cmp_seg(const void *a, const void *b) {
    const SegCand *x = (const SegCand *)a, *y = (const SegCand *)b;
    if (x->b != y->b) return (x->b > y->b) - (x->b < y->b);
    return (x->ord > y->ord) - (x->ord < y->ord);
}
static int cmp_seg_rank(const void *a, const void *b) {
    const SegRank *x = (const SegRank *)a, *y = (const SegRank *)b;
    if (x->w != y->w) return (x->w > y->w) - (x->w < y->w);
    return (x->ord > y->ord) - (x->ord < y->ord);
}

static size_t smap_pretrim(SegCand *pool, size_t pn) {
    if (pn <= SMAP_POOL_MAX) return pn;
    SegRank *rank = (SegRank *)xmalloc(pn * sizeof(*rank));
    for (size_t i = 0; i < pn; i++) rank[i] = (SegRank){pool[i].w, i};
    qsort(rank, pn, sizeof(*rank), cmp_seg_rank);
    for (size_t i = 0; i < pn - SMAP_POOL_MAX; i++) pool[rank[i].ord].w = UINT32_MAX;
    free(rank);
    size_t out = 0;
    for (size_t i = 0; i < pn; i++) if (pool[i].w != UINT32_MAX) pool[out++] = pool[i];
    return out;
}

/* Build the FULL deduped candidate map from field injections (BL: k1,k2,need; EX: k2=word
 * value, need): bsdiff op-walk boundaries + span terminator + exact EX value runs, oversized pool
 * weight-trimmed to SMAP_POOL_MAX, sorted, adjacent-boundary-deduped keeping later EX entries. */
int smap_build_full(const OpVec *ops, int32_t fp_start, uint32_t from_size, uint32_t to_size,
                    const FieldInjArena *inj, int fwd, uint32_t *tb, int32_t *tv) {
    /* Op-derived shifts are bounded by MAX_IMAGE. EX needs may span all int32_t values, but the
     * modular inverse of INT32_MIN is still INT32_MIN, whose zigzag UINT32_MAX cannot be gamma-coded
     * (gamma stores value+1). Leave that one residual on the full-domain dval path instead; this also
     * keeps INT32_MIN available as fit_shift_map_scored's private removal sentinel. */
    size_t nex = 0;
    for (size_t i = 0; i < inj->n; i++) {
        const FieldInj *fk = field_inj_key(inj, fwd, i);
        nex += fk->kind == EV_EX && fk->need != INT32_MIN;
    }
    /* candidate union: op walk + terminator + exact EX value runs (weight = fields in run) */
    size_t pcap = ops->n + 2 + (nex ? nex : 1), pn = 0;
    SegCand *pool = (SegCand *)xmalloc(pcap * sizeof(*pool));
    { OpWalkEnt *walk = opwalk_build(ops, fp_start);
      for (size_t i = 0; i < ops->n; i++) {
          if (walk[i].o->diff_len > 0 && walk[i].fp >= 0) {
              pool[pn].b = (uint32_t)walk[i].fp; pool[pn].v = walk[i].tp - walk[i].fp;
              pool[pn].w = (uint32_t)walk[i].o->diff_len; pool[pn].ord = (uint32_t)pn; pn++;
          }
      }
      free(walk);
      pool[pn].b = from_size > to_size ? from_size : to_size; pool[pn].v = 0;
      pool[pn].w = 0x7fffffffu; pool[pn].ord = (uint32_t)pn; pn++; } /* terminator: never pre-trimmed */
    if (nex) {
        SegCand *ex = (SegCand *)xmalloc(nex * sizeof(*ex));
        size_t en = 0;
        for (size_t i = 0; i < inj->n; i++) {
            const FieldInj *fk = field_inj_key(inj, fwd, i);
            if (fk->kind == EV_EX && fk->need != INT32_MIN) {
                ex[en].b = fk->k2;
                ex[en].v = rc_i32_from_u32(0u - (uint32_t)fk->need);
                ex[en].ord = (uint32_t)en;
                en++;
            }
        }
        qsort(ex, en, sizeof(*ex), cmp_seg);
        for (size_t i = 0; i < en;) {
            size_t j = i;
            while (j < en && ex[j].v == ex[i].v) j++;
            pool[pn].b = ex[i].b; pool[pn].v = ex[i].v; pool[pn].w = (uint32_t)(j - i);
            pool[pn].ord = (uint32_t)pn; pn++;
            i = j;
        }
        free(ex);
    }
    /* Drop the lightest original entries (earliest first on a tie), then compact in
     * original order. UINT32_MAX is outside every real weight (all are <= INT32_MAX). */
    pn = smap_pretrim(pool, pn);
    /* sorted union map (dedupe same boundary: keep the later, i.e. EX value-derived, entry) */
    qsort(pool, pn, sizeof(*pool), cmp_seg);
    int mn = 0;
    for (size_t i = 0; i < pn; i++) {
        if (mn && tb[mn - 1] == pool[i].b) { tv[mn - 1] = pool[i].v; continue; }
        tb[mn] = pool[i].b; tv[mn] = pool[i].v; mn++;
    }
    free(pool);
    return mn;
}

static uint64_t dz_bits_u32(uint32_t x) {
    uint32_t m = x + 1u;
    int n = bitlen32(m);
    return (uint64_t)(2 * bitlen32((uint32_t)n) - 1 + n - 1);
}

static uint64_t uleb_proxy_bits(uint32_t v, const uint8_t L0[256]) {
    uint64_t bits = 0;
    for (;;) {
        uint8_t x = (uint8_t)(v & 0x7fu);
        v >>= 7;
        bits += L0[v ? (uint8_t)(x | 0x80u) : x];
        if (!v) return bits;
    }
}

static uint64_t extra_proxy_bits(const Buf *to, int32_t abs_begin, int32_t len,
                                 const uint8_t L0[256], const uint8_t L1[256]) {
    uint64_t bits = 0;
    for (int32_t i = 0; i < len; i++) {
        const uint8_t *L = ((abs_begin + i) & 1) ? L1 : L0;
        bits += L[to->d[(size_t)abs_begin + (size_t)i]];
    }
    return bits;
}

typedef struct { int32_t begin, end; } Run;
typedef struct {
    uint64_t w, gp, x, e;
    uint32_t cnt;
    int32_t ft;
} SplitScratch;

static uint64_t split_diff_bits(const SplitScratch *sc, const Run *runs,
                                size_t p, size_t e, int32_t endpos, int32_t seg,
                                int fwd, const uint8_t L0[256]) {
    uint64_t bits = uleb_proxy_bits(sc[e].cnt - sc[p].cnt, L0);
    if (e > p) {
        bits += fwd ? uleb_proxy_bits((uint32_t)(runs[p].begin - seg), L0)
                    : uleb_proxy_bits((uint32_t)(endpos - runs[e - 1].end + 1), L0);
        bits += (sc[e].w - sc[p].w) + (sc[e - 1].gp - sc[p].gp);
    }
    return bits;
}

/* Dense diff runs can be represented either as literal diff bytes or as an
 * extra span plus an equal source skip. Choose the subset analytically per op:
 * dynamic programming minimizes the modeled cost of the resulting op sequence
 * using the same seeded literal bit-length proxy used by LZ planning, plus the
 * exact raw geometry code lengths. */
enum { SPLIT_GAIN_MARGIN_BITS = 8 };

/* Bound the quadratic inner recurrence across one candidate plan. Development measurements over
 * the release corpus and synthetic adversaries found a high-water mark of 7,489,853 candidate
 * transitions in one plan call. Four times the high-water is 29,959,412; rounding up gives this
 * 2^25 budget (above the 2^20 minimum).
 * T(8191)=33,550,336 fits while T(8192)=33,558,528 does not, pinning the boundary arithmetic. */
#define SPLIT_TRANSITION_BUDGET UINT64_C(33554432)
_Static_assert(UINT64_C(8191) * 8192u / 2u <= SPLIT_TRANSITION_BUDGET,
               "split transition budget lower boundary");
_Static_assert(UINT64_C(8192) * 8193u / 2u > SPLIT_TRANSITION_BUDGET,
               "split transition budget upper boundary");

static void split_nonzero_diff_runs_budget(const EncCtx *ctx, OpVec *ops,
                                           const Buf *from, const Buf *to,
                                           const SourceLitModels *lit,
                                           uint64_t transitions_left) {
    const uint8_t *L0 = lit->L0, *L1 = lit->L1;
    OpVec out = {0};
    int32_t tp = 0, fp = 0;
    for (size_t oi = 0; oi < ops->n; oi++) {
        Op *o = &ops->v[oi];
        Run *runs = NULL;
        size_t nr = 0, rcap = 0;
        for (int32_t scan = 0; scan < o->diff_len;) {
            while (scan < o->diff_len && op_diff_byte(from->d, (uint32_t)from->n, to->d,
                                                       fp + scan, tp + scan) == 0) scan++;
            if (scan >= o->diff_len) break;
            int32_t begin = scan++;
            while (scan < o->diff_len && op_diff_byte(from->d, (uint32_t)from->n, to->d,
                                                       fp + scan, tp + scan) != 0) scan++;
            runs = (Run *)vec_reserve(runs, &rcap, nr + 1, sizeof(runs[0]), 16);
            runs[nr++] = (Run){ begin, scan };
        }
        if (!nr) {
            opvec_push(&out, *o);
            tp += o->diff_len + o->extra_len; fp += o->diff_len + o->adj;
            continue;
        }
        /* nr cannot exceed the ceiling of half the int32 diff length on alternating input, so this
         * uint64_t product is bounded. An over-budget op remains byte-for-byte in the plan;
         * later small ops may still spend the unchanged remainder. */
        uint64_t transitions = (uint64_t)nr * (nr + 1u) / 2u;
        if (transitions > transitions_left) {
            opvec_push(&out, *o);
            free(runs);
            tp += o->diff_len + o->extra_len; fp += o->diff_len + o->adj;
            continue;
        }
        transitions_left -= transitions;
        /* The historical DP re-priced every (segment start, split run) pair with a fresh
         * op_proxy_bits walk over the diff bytes -- O(nr^2 * diff_len) time and an O(nr^2)
         * state table, quadratic-cubic on scattered-diff inputs (the scattered-diff /
         * scattered-diff stress cases). Every query boundary is a RUN boundary, so
         * diff_proxy_bits decomposes exactly into run-indexed prefix sums (uint64 addition
         * is associative; the terms are the same uleb_proxy_bits/L0 values in a different
         * association order), and the full (ri, p) table collapses to its diagonal
         * E(p) = DP(p, p) plus a first-take pointer per p. Same decisions, same margins,
         * bit-identical output; O(nr^2) time with O(1) transitions and O(nr) memory.
         *
         * Per run j: W[j+1]-W[j] = literal bits of run j (its L0 bytes + (len-1) gap-1
         * ulebs); GP[j] accumulates the between-run gap uleb (run j-1 -> run j); CNT is the
         * nonzero-count prefix. diff_proxy_bits(seg_p, end) over runs p..e-1 is then
         *   uleb(CNT[e]-CNT[p]) [+ boundary-gap uleb + (W[e]-W[p]) + (GP[e-1]-GP[p]) if e>p]
         * where the boundary gap is the only direction-dependent term: FWD prices the gap
         * from seg_p to the first run, reverse prices the gap from `end` back to the last. */
        SplitScratch *sc = (SplitScratch *)xmalloc((nr + 1) * sizeof(*sc));
        const uint64_t uleb1 = uleb_proxy_bits(1u, L0);
        sc[0].w = 0; sc[0].cnt = 0; sc[0].gp = 0;
        for (size_t j = 0; j < nr; j++) {
            uint64_t w = 0;
            for (int32_t k = runs[j].begin; k < runs[j].end; k++)
                w += L0[op_diff_byte(from->d, (uint32_t)from->n, to->d, fp + k, tp + k)];
            w += (uint64_t)(runs[j].end - runs[j].begin - 1) * uleb1;
            sc[j + 1].w = sc[j].w + w;
            sc[j + 1].cnt = sc[j].cnt + (uint32_t)(runs[j].end - runs[j].begin);
            if (j) sc[j].gp = sc[j - 1].gp + uleb_proxy_bits((uint32_t)(runs[j].begin - runs[j - 1].end + 1), L0);
            sc[j].x = extra_proxy_bits(to, tp + runs[j].begin,
                                       runs[j].end - runs[j].begin, L0, L1);
        }
        const uint64_t XF = extra_proxy_bits(to, tp + o->diff_len, o->extra_len, L0, L1);
        const int FWD = ctx->fwd;
        for (size_t p = nr + 1; p-- > 0;) {
            int32_t seg = p ? runs[p - 1].end : 0;
            /* terminal (no further split): the historical DP(nr, p) row */
            uint64_t v = dz_bits_u32((uint32_t)(o->diff_len - seg)) +
                         dz_bits_u32((uint32_t)o->extra_len) +
                         dz_bits_u32(rc_zz32(o->adj)) + 2 +
                         split_diff_bits(sc, runs, p, nr, o->diff_len, seg, FWD, L0) + XF;
            int32_t take = -1;
            /* downward scan mirrors the historical backward recurrence: at each s the
             * running v IS DP(s+1, p), and the margin fires under the same condition,
             * so the lowest firing s equals the first TAKE the old walk would hit. */
            for (size_t s = nr; s-- > p;) {
                int32_t len = runs[s].end - runs[s].begin;
                uint64_t c = dz_bits_u32((uint32_t)(runs[s].begin - seg)) +
                             dz_bits_u32((uint32_t)len) +
                             dz_bits_u32(rc_zz32(len)) + 2 +
                             split_diff_bits(sc, runs, p, s, runs[s].begin, seg, FWD, L0) + sc[s].x +
                             sc[s + 1].e;
                if (c + SPLIT_GAIN_MARGIN_BITS < v) { v = c; take = (int32_t)s; }
            }
            sc[p].e = v; sc[p].ft = take;
        }
        int32_t seg = 0;
        { size_t p = 0;
          while (sc[p].ft >= 0) {
              size_t s = (size_t)sc[p].ft;
              int32_t len = runs[s].end - runs[s].begin;
              int32_t pre = runs[s].begin - seg;
              opvec_push(&out, (Op){ pre, len, len });
              seg = runs[s].end;
              p = s + 1;
          } }
        if (seg) {
            int32_t tail = o->diff_len - seg;
            if (tail || o->extra_len || o->adj)
                opvec_push(&out, (Op){ tail, o->extra_len, o->adj });
        } else {
            opvec_push(&out, *o);
        }
        free(sc);
        free(runs);
        tp += o->diff_len + o->extra_len; fp += o->diff_len + o->adj;
    }
    free(ops->v);
    *ops = out;
}

void split_nonzero_diff_runs(const EncCtx *ctx, OpVec *ops,
                             const Buf *from, const Buf *to,
                             const SourceLitModels *lit) {
    split_nonzero_diff_runs_budget(ctx, ops, from, to, lit, SPLIT_TRANSITION_BUDGET);
}
