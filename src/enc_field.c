/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 *
 * A1 host encoder module -- A1 field/delta model + apply planning: classify_field, merge_op_field_deltas, fit_shift_map, proxy pricing, split runs, preserve/corrections.
 * Compiled as a normal internal encoder translation unit.
 */

#include "enc_internal.h"
/* ------------------------------------------------------------------------------------- */
/* A1 field and apply planning.                                                            */
/* ------------------------------------------------------------------------------------- */
static int is_local_bl(const uint8_t *frm, uint32_t from_size, uint32_t fpk) {
    if (fpk & 1u) return 0;
    if (fpk > from_size || from_size - fpk < 4u) return 0;
    return rc_bl_pattern(rc_u16le(frm + fpk), rc_u16le(frm + fpk + 2));
}

static int field_addr(int32_t fp0, int32_t k, uint32_t from_size, uint32_t *out) {
    int64_t a = (int64_t)fp0 + k;
    if (a < 0 || a + 4 > (int64_t)from_size) return 0;
    *out = (uint32_t)a;
    return 1;
}

void ldr_target_index_build(LdrTargetIndex *idx, const uint8_t *source, uint32_t source_size) {
    memset(idx, 0, sizeof(*idx));
    idx->nwords = ((size_t)source_size + 3u) >> 2;
    idx->back = (uint16_t *)xcalloc(idx->nwords ? idx->nwords : 1u, sizeof(*idx->back));
    idx->source = source;
    idx->source_size = source_size;
    for (uint32_t a = 0; a + 2u <= source_size; a += 2u) {
        uint16_t up = rc_u16le(source + a);
        if (!rc_thumb_ldr_lit(up)) continue;
        uint32_t t = (uint32_t)rc_ldr_target((int32_t)a, (int32_t)(up & 0xffu));
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

int ldr_target_index_query(const LdrTargetIndex *idx, int32_t fp0, int32_t dl, uint32_t fpk) {
    int hit = 0;
    if (fpk <= idx->source_size && idx->source_size - fpk >= 4u &&
        rc_ldr_target_in_op(fp0, dl, fpk)) {
        uint16_t back = idx->back[fpk >> 2];
        hit = back && (int32_t)(fpk - back) >= fp0;
    }
    return hit;
}

static int preserve_needed_at(const EncCtx *ctx, const int32_t *readarr, uint32_t from_size, int32_t tpw) {
    if (!(0 <= tpw && (uint32_t)tpw < from_size)) return 0;
    return ctx->fwd ? (readarr[tpw] > tpw) : (readarr[tpw] >= 0 && readarr[tpw] < tpw);
}

static int32_t *preserve_readarr(const EncCtx *ctx, const OpWalkEnt *walk,
                                 size_t nwalk, uint32_t from_size) {
    int FWD = ctx->fwd;
    int32_t *readarr = (int32_t *)xmalloc((size_t)from_size * sizeof(int32_t));
    for (uint32_t i = 0; i < from_size; i++) readarr[i] = FWD ? -1 : INT_MAX;
    for (size_t oi = 0; oi < nwalk; oi++) {
        const OpWalkEnt *we = &walk[oi];
        for (int32_t k = 0; k < we->o->diff_len; k++) {
            int32_t a = we->fp + k;
            if (0 <= a && (uint32_t)a < from_size) {
                int32_t t = we->tp + k;
                /* a read behind the frontier that the page window covers reads OLD flash
                 * directly — it must not force a journal entry. */
                if ((FWD ? a < t : a > t) && row_covered(ctx, a, t)) continue;
                if (FWD) { if (readarr[a] < t) readarr[a] = t; }
                else { if (readarr[a] > t) readarr[a] = t; }
            }
        }
    }
    return readarr;
}

typedef struct { FieldWalk w; int32_t pos; uint8_t packed[4]; int have; } PreserveFieldCursor;

typedef struct {
    const EncCtx *ctx;
    const int32_t *readarr;
    const uint8_t *payload, *frm, *true_to;
    uint8_t *buf, *jhas;
    uint32_t from_size;
    PlanCaps *caps;
} PreserveCorrWalk;

static void preserve_field_cursor_next(PreserveFieldCursor *fc) {
    while (fw_next(&fc->w)) {
        if (!fc->w.is_field || (fc->w.ev.type != EV_BL && fc->w.ev.type != EV_EX)) continue;
        fc->pos = fc->w.pos;
        uint32_t fpk = (uint32_t)(fc->w.fp0 + fc->w.pos);
        if (fc->w.ev.type == EV_BL) {
            uint16_t up = rc_u16le(fc->w.frm + fpk), lo = rc_u16le(fc->w.frm + fpk + 2);
            rc_bl_dereloc(up, lo, (uint32_t)fc->w.ev.delta, fc->packed);
        } else {
            uint32_t val = rc_u32le(fc->w.frm + fpk);
            rc_u32le_put(fc->packed, val - (uint32_t)fc->w.ev.delta);
        }
        fc->have = 1;
        return;
    }
    fc->have = 0;
}

static const PreserveFieldCursor *preserve_corr_event(const EncCtx *ctx,
                                                      PreserveFieldCursor *fc, int32_t off) {
    if (!fc->have) preserve_field_cursor_next(fc);
    while (fc->have) {
        int32_t d = off - fc->pos;
        if ((uint32_t)d < 4u) return fc;
        if (ctx->fwd ? d > 3 : d < 0) { preserve_field_cursor_next(fc); continue; }
        return NULL;
    }
    return NULL;
}

static void preserve_corr_byte(PreserveCorrWalk *pw, PreserveFieldCursor *fc, OpPC *pc,
                               const OpWalkEnt *we, int32_t off, int is_diff, uint8_t byte) {
    int32_t tp = we->tp + off;
    int32_t fp = is_diff ? we->fp + off : -1;
    int preserve = preserve_needed_at(pw->ctx, pw->readarr, pw->from_size, tp);
    if (preserve) {
        ivec_push(&pc->pres, off);
        pw->caps->pres_total++;
        if ((uint32_t)tp < RC_PACKED_POS_LIMIT && pw->caps->pres_kept < JSLOTS) {
            pw->caps->pres_kept++;
        } else {
            pw->caps->ok = 0;
            /* Preserves arrive in actual apply order. FWD's first unkept position starts
             * its high suffix; grow skips the unrepresentable >=16 MiB prefix, then the
             * first over-budget representable position starts its low suffix. */
            if ((uint32_t)tp < RC_PACKED_POS_LIMIT &&
                (pw->ctx->fwd ? pw->caps->pres_cutoff == (int32_t)RC_PACKED_POS_LIMIT
                              : pw->caps->pres_cutoff < 0))
                pw->caps->pres_cutoff = tp;
        }
        pw->jhas[tp] = 1;
    }
    uint8_t produced;
    const PreserveFieldCursor *ev = is_diff ? preserve_corr_event(pw->ctx, fc, off) : NULL;
    if (ev) {
        produced = ev->packed[off - ev->pos];
    } else {
        uint8_t src = 0;
        if (is_diff && 0 <= fp && (uint32_t)fp < pw->from_size) {
            int behind = pw->ctx->fwd ? (fp < tp) : (fp > tp);
            src = (pw->jhas[fp] || (behind && row_covered(pw->ctx, fp, tp))) ? pw->frm[fp] : pw->buf[fp];
        }
        produced = (uint8_t)(byte + src);
    }
    uint8_t want = pw->true_to[tp];
    uint8_t corr = (uint8_t)(want - produced);
    if (corr) {
        corr_push(&pc->corr, off, corr);
        if ((uint32_t)off >= RC_PACKED_POS_LIMIT || pc->corr.n > OPC_CAP)
            pw->caps->ok = 0;
    }
    pw->buf[tp] = want;
}

static void preserve_corr_op(PreserveCorrWalk *cw, PreserveFieldCursor *fc, OpPC *pc,
                             const OpWalkEnt *we) {
    const Op *o = we->o;
    int32_t n = o->diff_len + o->extra_len;
    int FWD = cw->ctx->fwd;
    for (int32_t i = 0; i < n; i++) {
        int32_t off = FWD ? i : n - 1 - i;
        int is_diff = off < o->diff_len;
        uint8_t byte = cw->payload[we->tp + off];
        preserve_corr_byte(cw, fc, pc, we, off, is_diff, byte);
    }
}

/* diff==NULL forces the window pure (assume-pure classification: a local BL becomes EV_BL, never
 * EV_SBL) — coerce_reloc_literals uses that to detect fields before their literals are zeroed. */
static Event classify_field(const uint8_t *frm, uint32_t from_size, const FieldDeltaVec *fd,
                            const LdrTargetIndex *ldr, const uint8_t *diff,
                            int32_t fp0, int32_t dl, int32_t k) {
    uint32_t fpk;
    if (!field_addr(fp0, k, from_size, &fpk)) return (Event){ EV_NONE, 0 };
    int pure = !diff || (diff[k] == 0 && diff[k + 1] == 0 && diff[k + 2] == 0 && diff[k + 3] == 0);
    if (is_local_bl(frm, from_size, fpk)) {
        if (!pure) return (Event){ EV_SBL, 0 };
        const FieldDelta *r = fd_find_kind(fd, fpk, STREAM_BL);
        return (Event){ EV_BL, r ? r->delta : 0 };
    }
    if (pure && ldr_target_index_query(ldr, fp0, dl, fpk)) {
        const FieldDelta *r = fd_find_kind(fd, fpk, STREAM_LDR);
        return (Event){ EV_EX, r ? r->delta : 0 };
    }
    return (Event){ EV_NONE, 0 };
}

/* The single encoder mirror of the decoder's apply_op 4-byte-window skeleton (patch_apply.h).
 * fw_next yields, in wire consume order, either a classified field window (is_field=1, pos=window
 * anchor, ev) or one copy position (is_field=0, pos). Direction-parametrized: FWD ascends from 0
 * with anchor==cursor; grow descends from dl-1 with anchor==cursor-3. Every encoder field walk
 * routes through this so no hand-copy can slip the order (a slip flips golden/selfcheck). */
void fw_init(FieldWalk *w, int fwd, const uint8_t *frm, uint32_t from_size,
             const FieldDeltaVec *fd, const LdrTargetIndex *ldr,
             const uint8_t *diff, int32_t fp0, int32_t dl) {
    w->fwd = fwd; w->dl = dl; w->k = fwd ? 0 : dl - 1;
    w->frm = frm; w->from_size = from_size; w->fd = fd; w->ldr = ldr;
    w->diff = diff; w->fp0 = fp0;
}
int fw_next(FieldWalk *w) {
    if (w->fwd ? w->k >= w->dl : w->k < 0) return 0;
    int32_t a0 = w->fwd ? w->k : w->k - 3;
    if (a0 >= 0 && a0 + 4 <= w->dl) {
        Event ev = classify_field(w->frm, w->from_size, w->fd, w->ldr,
                                  w->diff, w->fp0, w->dl, a0);
        if (ev.type != EV_NONE) {
            w->is_field = 1; w->pos = a0; w->ev = ev; w->k += w->fwd ? 4 : -4;
            return 1;
        }
    }
    w->is_field = 0; w->pos = w->k; w->ev = (Event){ EV_NONE, 0 }; w->k += w->fwd ? 1 : -1;
    return 1;
}

/* Mask every local-BL immediate in `mut` (positions detected on the REAL image), keeping the
 * F000/D000 anchors, so bsdiff sees identical bytes for any two BLs and copies extend through
 * recompiled code. Encoder-only: the decoder classifies fields on the pristine from image, and
 * preserve_corrections_pc absorbs any mask-induced diff mismatch by construction. */
void mask_bl_imms(const uint8_t *real, uint8_t *mut, size_t n) {
    for (size_t a = 0; a + 4 <= n;) {
        uint16_t up = rc_u16le(real + a), lo = rc_u16le(real + a + 2);
        if (rc_bl_pattern(up, lo)) {
            mut[a] = 0x00; mut[a + 1] = 0xf0; mut[a + 2] = 0x00; mut[a + 3] = 0xd0;
            a += 4;
        } else a += 2;
    }
}

/* Op-derived field deltas: for every BL/LDR field candidate inside a copy, the exact delta under
 * the bsdiff alignment (from value at fpk minus to value at tp0+k). These override block-matched
 * entries (which can be misaligned vs the op plan and then cost 4 correction bytes per field). */
void merge_op_field_deltas(FieldDeltaVec *fd, const OpVec *ops, const uint8_t *frm,
                           uint32_t from_size, const uint8_t *tob, uint32_t to_size,
                           const LdrTargetIndex *ldr) {
    /* Append op-derived entries after the already-finalized block-matched entries, then a single
     * fd_finalize: its stable sort keeps the appended op-derived entry after the same-key block
     * entry, and its last-wins dedup makes op-derived override block-matched (and, among op-derived
     * duplicates, the last fd_put win) -- bit-identical to the former add/out rebuild. */
    OpWalkEnt *walk = opwalk_build(ops, 0);
    for (size_t oi = 0; oi < ops->n; oi++) {
        const OpWalkEnt *we = &walk[oi];
        for (int32_t k = 0; k + 4 <= we->o->diff_len; k += 2) {
            uint32_t fpk;
            if (!field_addr(we->fp, k, from_size, &fpk)) continue;
            int64_t tpk = (int64_t)we->tp + k;
            if (tpk < 0 || tpk + 4 > (int64_t)to_size) continue;
            if (is_local_bl(frm, from_size, fpk)) {
                uint16_t tu = rc_u16le(tob + (size_t)tpk), tl = rc_u16le(tob + (size_t)tpk + 2);
                if (rc_bl_pattern(tu, tl)) {
                    uint16_t fu = rc_u16le(frm + fpk), fl2 = rc_u16le(frm + fpk + 2);
                    fd_put(fd, fpk, STREAM_BL,
                           rc_i32_from_u32((uint32_t)rc_bl_imm24s(fu, fl2) -
                                           (uint32_t)rc_bl_imm24s(tu, tl)));
                }
            } else if (ldr_target_index_query(ldr, we->fp, we->o->diff_len, fpk)) {
                fd_put(fd, fpk, STREAM_LDR,
                       rc_i32_from_u32(rc_u32le(frm + fpk) - rc_u32le(tob + (size_t)tpk)));
            }
        }
    }
    free(walk);
    fd_finalize(fd);
}

/* ---- piecewise shift map (D1): lookups go through rc_smap_at (rc_models.h), the single-sourced
 * mirror of patch_apply smap_at. ---- */

typedef struct { uint32_t b; int32_t v; uint32_t w; } SegCand;
typedef struct { uint32_t w; size_t ord; } SegRank;
static int cmp_seg(const void *a, const void *b) {
    const SegCand *x = (const SegCand *)a, *y = (const SegCand *)b;
    return (x->b > y->b) - (x->b < y->b);
}
static int cmp_seg_rank(const void *a, const void *b) {
    const SegRank *x = (const SegRank *)a, *y = (const SegRank *)b;
#ifdef SMAP_PRETRIM_ORACLE
    extern uint64_t smap_pretrim_comparisons;
    smap_pretrim_comparisons++;
#endif
    if (x->w != y->w) return (x->w > y->w) - (x->w < y->w);
    return (x->ord > y->ord) - (x->ord < y->ord);
}

#ifdef SMAP_PRETRIM_ORACLE
uint64_t smap_pretrim_comparisons;
#endif

static size_t smap_pretrim(SegCand *pool, size_t pn) {
    if (pn <= SMAP_POOL_MAX) return pn;
    SegRank *rank = (SegRank *)xmalloc(pn * sizeof(*rank));
    for (size_t i = 0; i < pn; i++) rank[i] = (SegRank){pool[i].w, i};
    sort(rank, pn, sizeof(*rank), cmp_seg_rank);
    for (size_t i = 0; i < pn - SMAP_POOL_MAX; i++) pool[rank[i].ord].w = UINT32_MAX;
    free(rank);
    size_t out = 0;
    for (size_t i = 0; i < pn; i++) if (pool[i].w != UINT32_MAX) pool[out++] = pool[i];
    return out;
}

#ifdef SMAP_PRETRIM_ORACLE
size_t smap_pretrim_probe(const uint32_t *weights, size_t n, size_t *survivors) {
    SegCand *pool = (SegCand *)xmalloc((n ? n : 1u) * sizeof(*pool));
    for (size_t i = 0; i < n; i++) pool[i] = (SegCand){(uint32_t)i, (int32_t)i, weights[i]};
    size_t out = smap_pretrim(pool, n);
    for (size_t i = 0; i < out; i++) survivors[i] = pool[i].b;
    free(pool);
    return out;
}
#endif

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
              pool[pn].w = (uint32_t)walk[i].o->diff_len; pn++;
          }
      }
      free(walk);
      pool[pn].b = from_size > to_size ? from_size : to_size; pool[pn].v = 0;
      pool[pn].w = 0x7fffffffu; pn++; }                  /* terminator: never pre-trimmed */
    if (nex) {
        SegCand *ex = (SegCand *)xmalloc(nex * sizeof(*ex));
        size_t en = 0;
        for (size_t i = 0; i < inj->n; i++) {
            const FieldInj *fk = field_inj_key(inj, fwd, i);
            if (fk->kind == EV_EX && fk->need != INT32_MIN) {
                ex[en].b = fk->k2;
                ex[en].v = rc_i32_from_u32(0u - (uint32_t)fk->need);
                en++;
            }
        }
        sort(ex, en, sizeof(*ex), cmp_seg);
        for (size_t i = 0; i < en;) {
            size_t j = i;
            while (j < en && ex[j].v == ex[i].v) j++;
            pool[pn].b = ex[i].b; pool[pn].v = ex[i].v; pool[pn].w = (uint32_t)(j - i); pn++;
            i = j;
        }
        free(ex);
    }
    /* Drop the lightest original entries (earliest first on a tie), then compact in
     * original order. UINT32_MAX is outside every real weight (all are <= INT32_MAX). */
    pn = smap_pretrim(pool, pn);
    /* sorted union map (dedupe same boundary: keep the later, i.e. EX value-derived, entry) */
    sort(pool, pn, sizeof(*pool), cmp_seg);
    int mn = 0;
    for (size_t i = 0; i < pn; i++) {
        if (mn && tb[mn - 1] == pool[i].b) { tv[mn - 1] = pool[i].v; continue; }
        tb[mn] = pool[i].b; tv[mn] = pool[i].v; mn++;
    }
    free(pool);
    return mn;
}

void coerce_reloc_literals(const EncCtx *ctx, OpVec *ops, const uint8_t *frm,
                           uint32_t from_size, const FieldDeltaVec *fd,
                           const LdrTargetIndex *ldr) {
    int FWD = ctx->fwd;
    uint8_t *payload = ops->payload;
    int32_t fp0 = 0, tp0 = 0;
    for (size_t oi = 0; oi < ops->n; oi++) {
        Op *o = &ops->v[oi];
        /* diff==NULL => assume-pure: a still-dirty field classifies as EV_BL/EV_EX so its
         * literals get zeroed here before they would suppress the field on the wire. */
        FieldWalk w; fw_init(&w, FWD, frm, from_size, fd, ldr, NULL, fp0, o->diff_len);
        while (fw_next(&w)) {
            if (!w.is_field) continue;
            int32_t k = w.pos;
            uint32_t fpk = (uint32_t)(fp0 + k);
            const FieldDelta *real = fd_find_kind(fd, fpk, w.ev.type == EV_BL ? STREAM_BL : STREAM_LDR);
            uint8_t *d = payload + tp0 + k;
            if (real && (d[0] || d[1] || d[2] || d[3])) memset(d, 0, 4);
        }
        tp0 += o->diff_len + o->extra_len;
        fp0 += o->diff_len + o->adj;
    }
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

/* Bound the quadratic inner recurrence across one candidate plan. Instrumenting the ordinary
 * release corpus plus the preexisting one-face, edge, degradation, and golden workloads found a
 * high-water mark of 7,489,853 candidate transitions in one plan call. (The adversarial timing
 * fixtures added with this bound are intentionally outside that acceptance workload.) Four times
 * the high-water is 29,959,412; rounding up gives this 2^25 budget (above the 2^20 minimum).
 * T(8191)=33,550,336 fits while T(8192)=33,558,528 does not, pinning the boundary arithmetic. */
#define SPLIT_TRANSITION_BUDGET UINT64_C(33554432)
_Static_assert(UINT64_C(8191) * 8192u / 2u <= SPLIT_TRANSITION_BUDGET,
               "split transition budget lower boundary");
_Static_assert(UINT64_C(8192) * 8193u / 2u > SPLIT_TRANSITION_BUDGET,
               "split transition budget upper boundary");

static void split_nonzero_diff_runs_budget(const EncCtx *ctx, OpVec *ops,
                                           const Buf *from, const Buf *to,
                                           uint64_t transitions_left) {
    uint8_t L0[256], L1[256];
    LitSeedTrees seeds;
    uint8_t *payload = ops->payload;
    lit_seed_trees_init(&seeds, from->d, from->n);
    from_lit_proxy_bits(&seeds, L0, L1);
    OpVec out = { .payload = payload };
    int32_t tp = 0;
    for (size_t oi = 0; oi < ops->n; oi++) {
        Op *o = &ops->v[oi];
        Run *runs = NULL;
        size_t nr = 0, rcap = 0;
        for (int32_t scan = 0; scan < o->diff_len;) {
            while (scan < o->diff_len && payload[tp + scan] == 0) scan++;
            if (scan >= o->diff_len) break;
            int32_t begin = scan++;
            while (scan < o->diff_len && payload[tp + scan] != 0) scan++;
            runs = (Run *)vec_reserve(runs, &rcap, nr + 1, sizeof(runs[0]), 16);
            runs[nr++] = (Run){ begin, scan };
        }
        if (!nr) {
            opvec_push(&out, *o);
            tp += o->diff_len + o->extra_len;
            continue;
        }
        /* nr cannot exceed the ceiling of half the int32 diff length on alternating input, so this
         * uint64_t product is bounded. An over-budget op remains byte-for-byte in the plan;
         * later small ops may still spend the unchanged remainder. */
        uint64_t transitions = (uint64_t)nr * (nr + 1u) / 2u;
        if (transitions > transitions_left) {
            opvec_push(&out, *o);
            free(runs);
            tp += o->diff_len + o->extra_len;
            continue;
        }
        transitions_left -= transitions;
        /* The historical DP re-priced every (segment start, split run) pair with a fresh
         * op_proxy_bits walk over the diff bytes -- O(nr^2 * diff_len) time and an O(nr^2)
         * state table, quadratic-cubic on scattered-diff inputs (the stress_journal_scatter
         * / stress_corr_storm hangs). Every query boundary is a RUN boundary, so
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
         * from seg_p to the first run, grow prices the gap from `end` back to the last. */
        SplitScratch *sc = (SplitScratch *)xmalloc((nr + 1) * sizeof(*sc));
        const uint64_t uleb1 = uleb_proxy_bits(1u, L0);
        sc[0].w = 0; sc[0].cnt = 0; sc[0].gp = 0;
        for (size_t j = 0; j < nr; j++) {
            uint64_t w = 0;
            for (int32_t k = runs[j].begin; k < runs[j].end; k++) w += L0[payload[tp + k]];
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
              /* Split-generated extras stay in the normalized data-format domain. */
              memcpy(payload + tp + runs[s].begin,
                     to->d + (size_t)tp + (size_t)runs[s].begin, (size_t)len);
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
        tp += o->diff_len + o->extra_len;
    }
    free(ops->v);
    *ops = out;
}

void split_nonzero_diff_runs(const EncCtx *ctx, OpVec *ops,
                             const Buf *from, const Buf *to) {
    split_nonzero_diff_runs_budget(ctx, ops, from, to, SPLIT_TRANSITION_BUDGET);
}

#ifdef SPLIT_WORK_PROBE
void split_nonzero_diff_runs_probe(const EncCtx *ctx, OpVec *ops,
                                   const Buf *from, const Buf *to,
                                   uint64_t transitions) {
    split_nonzero_diff_runs_budget(ctx, ops, from, to, transitions);
}
#endif

OpPC *preserve_corrections_pc(const EncCtx *ctx, const OpVec *ops, int32_t fp_start,
                              const uint8_t *frm, const uint8_t *true_to,
                              const FieldDeltaVec *fd, uint32_t from_size, uint32_t to_size,
                              const LdrTargetIndex *ldr, PlanCaps *caps) {
    int FWD = ctx->fwd;
    size_t span = from_size > to_size ? from_size : to_size;
    OpWalkEnt *m = opwalk_build(ops, fp_start);
    *caps = (PlanCaps){ 1, fp_start,
        FWD ? (int32_t)RC_PACKED_POS_LIMIT : -1, 0, 0 };
    if (ops->n) {
        const OpWalkEnt *last = &m[ops->n - 1u];
        caps->fp_end = last->fp + last->o->diff_len + last->o->adj;
    }
    int32_t *readarr = preserve_readarr(ctx, m, ops->n, from_size);
    uint8_t *buf = (uint8_t *)xcalloc(span ? span : 1, 1);
    memcpy(buf, frm, from_size);
    uint8_t *jhas = (uint8_t *)xcalloc(from_size ? from_size : 1, 1);
    OpPC *out = (OpPC *)xcalloc(ops->n ? ops->n : 1, sizeof(*out));
    PreserveCorrWalk cw = { ctx, readarr, ops->payload, frm, true_to, buf, jhas, from_size, caps };
    const OpWalkEnt *we;
    for (size_t step = 0; step < ops->n; step++) {
        we = &m[opwalk_apply_index(ops->n, FWD, step)];
        PreserveFieldCursor fc;
        fw_init(&fc.w, FWD, frm, from_size, fd, ldr, ops->payload + we->tp,
                we->fp, we->o->diff_len);
        fc.have = 0;
        OpPC *pc = &out[step];
        preserve_corr_op(&cw, &fc, pc, we);
        if (!FWD) {
            if (pc->pres.n > 1) sort(pc->pres.v, pc->pres.n, sizeof(pc->pres.v[0]), cmp_i32);
            if (pc->corr.n > 1) sort(pc->corr.v, pc->corr.n, sizeof(pc->corr.v[0]), cmp_corr);
        }
    }
    free(buf); free(jhas); free(m); free(readarr);
    return out;
}
