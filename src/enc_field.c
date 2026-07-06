/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 *
 * A1 host encoder module -- A1 field/delta model + apply planning: classify_field, merge_op_field_deltas, fit_shift_map, collect_fields, build_field_deltas, proxy pricing, split runs, preserve/corrections.
 * Compiled as a normal internal encoder translation unit.
 */

#include "enc_internal.h"
/* ------------------------------------------------------------------------------------- */
/* A1 field and apply planning.                                                            */
/* ------------------------------------------------------------------------------------- */
static uint16_t u16le_at(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t u32le_at(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void u32le_put(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* rc_bl_imm24 / rc_bl_pack / rc_bl_pattern (rc_models.h) single-source the decoder-mirrored BL
 * unpack/pack/predicate. unpack_bl_local sign-extends the raw 24-bit immediate (same idiom the
 * decoder uses); pack_bl_local writes the low 24 bits back through rc_bl_pack. */
static int32_t unpack_bl_local(uint16_t up, uint16_t lo) {
    uint32_t imm24 = rc_bl_imm24(up, lo);
    return (int32_t)(imm24 << 8) >> 8;
}

static void pack_bl_local(int32_t imm32, uint8_t out[4]) {
    rc_bl_pack((uint32_t)imm32 & 0x00ffffffu, out);
}

static int is_local_bl(const uint8_t *frm, uint32_t from_size, uint32_t fpk) {
    if (fpk & 1u) return 0;
    if (fpk > from_size || from_size - fpk < 4u) return 0;
    return rc_bl_pattern(u16le_at(frm + fpk), u16le_at(frm + fpk + 2));
}

static int field_addr(int32_t fp0, int32_t k, uint32_t from_size, uint32_t *out) {
    int64_t a = (int64_t)fp0 + k;
    if (a < 0 || a + 4 > (int64_t)from_size) return 0;
    *out = (uint32_t)a;
    return 1;
}

IVec op_ldr_set(const uint8_t *frm, int32_t fp0, int32_t dl, uint32_t from_size) {
    IVec s = {0};
    int32_t lo = fp0, hi = fp0 + dl;
    int32_t a = (lo & 1) ? lo + 1 : lo;
    while (a + 2 <= hi && a + 2 <= (int32_t)from_size) {
        uint16_t up = u16le_at(frm + a);
        if ((up & 0xf800u) == 0x4800u) {
            int32_t t = rc_ldr_target(a, (int32_t)(up & 0xffu));
            if (lo <= t && t + 4 <= hi && t + 4 <= (int32_t)from_size) ivec_push(&s, t);
        }
        a += 2;
    }
    if (s.n) {
        a1_sort(s.v, s.n, sizeof(s.v[0]), cmp_i32);
        size_t w = 0;
        for (size_t i = 0; i < s.n; i++) if (!w || s.v[w - 1] != s.v[i]) s.v[w++] = s.v[i];
        s.n = w;
    }
    return s;
}

static int ivec_has(const IVec *v, int32_t x) {
    size_t lo = 0, hi = v->n;
    while (lo < hi) {
        size_t m = (lo + hi) >> 1;
        if (v->v[m] < x) lo = m + 1;
        else hi = m;
    }
    return lo < v->n && v->v[lo] == x;
}

typedef struct {
    const EncCtx *ctx;
    const int32_t *readarr;
    uint32_t from_size, to_size;
    size_t budget, total;
    int32_t cutoff, wi;
    int found_cutoff;
} PreserveBudgetWalk;

static void preserve_budget_byte(void *user, const OpWalkEnt *we, int32_t off, int is_diff, uint8_t byte) {
    PreserveBudgetWalk *pw = (PreserveBudgetWalk *)user;
    int32_t tpw = we->tp + off;
    (void)is_diff;
    (void)byte;
    if (0 <= tpw && (uint32_t)tpw < pw->from_size) {
        int preserve = pw->ctx->fwd ? (pw->readarr[tpw] > tpw)
                                    : (pw->readarr[tpw] >= 0 && pw->readarr[tpw] < tpw);
        if (preserve) {
            if (!pw->found_cutoff && pw->total == pw->budget) {
                pw->cutoff = pw->ctx->fwd ? pw->wi : (int32_t)(pw->to_size - 1u - (uint32_t)pw->wi);
                pw->found_cutoff = 1;
            }
            pw->total++;
        }
    }
    pw->wi++;
}

static int32_t *preserve_readarr(const EncCtx *ctx, const OpVec *ops, uint32_t from_size) {
    int FWD = ctx->fwd;
    int32_t *readarr = (int32_t *)xmalloc((size_t)from_size * sizeof(int32_t));
    for (uint32_t i = 0; i < from_size; i++) readarr[i] = FWD ? -1 : INT_MAX;
    OpWalkEnt *m = opwalk_build(ops);
    for (size_t oi = 0; oi < ops->n; oi++) {
        const OpWalkEnt *we = &m[oi];
        for (int32_t k = 0; k < we->o->diff_len; k++) {
            int32_t a = we->fp + k;
            if (0 <= a && (uint32_t)a < from_size) {
                int32_t t = we->tp + k;
                /* a read behind the frontier that the row window covers reads OLD flash
                 * directly — it must not force a journal entry. */
                if ((FWD ? a < t : a > t) && a1_row_covered(ctx, a, t)) continue;
                if (FWD) { if (readarr[a] < t) readarr[a] = t; }
                else { if (readarr[a] > t) readarr[a] = t; }
            }
        }
    }
    free(m);
    return readarr;
}

static int preserve_needed_at(const EncCtx *ctx, const int32_t *readarr, uint32_t from_size, int32_t tpw) {
    if (!(0 <= tpw && (uint32_t)tpw < from_size)) return 0;
    return ctx->fwd ? (readarr[tpw] > tpw) : (readarr[tpw] >= 0 && readarr[tpw] < tpw);
}

typedef struct {
    const EncCtx *ctx;
    const int32_t *readarr;
    const uint8_t *frm, *true_to, *ohas, *oval;
    uint8_t *buf, *jhas, *jval;
    uint32_t from_size;
    OpPC *pc;
} PreserveCorrWalk;

static void preserve_corr_byte(void *user, const OpWalkEnt *we, int32_t off, int is_diff, uint8_t byte) {
    PreserveCorrWalk *pw = (PreserveCorrWalk *)user;
    int32_t tp = we->tp + off;
    int32_t fp = is_diff ? we->fp + off : -1;
    int preserve = preserve_needed_at(pw->ctx, pw->readarr, pw->from_size, tp);
    if (preserve) ivec_push(&pw->pc->pres, off);
    if (preserve && 0 <= tp && (uint32_t)tp < pw->from_size && !pw->jhas[tp]) {
        pw->jhas[tp] = 1;
        pw->jval[tp] = pw->buf[tp];
    }
    uint8_t produced;
    if (pw->ohas[tp]) produced = pw->oval[tp];
    else {
        uint8_t src = 0;
        if (is_diff && 0 <= fp && (uint32_t)fp < pw->from_size) {
            int behind = pw->ctx->fwd ? (fp < tp) : (fp > tp);
            src = pw->jhas[fp] ? pw->jval[fp]
                : ((behind && a1_row_covered(pw->ctx, fp, tp)) ? pw->frm[fp] : pw->buf[fp]);
        }
        produced = (uint8_t)(byte + src);
    }
    uint8_t want = pw->true_to[tp];
    uint8_t corr = (uint8_t)(want - produced);
    if (corr) corr_push(&pw->pc->corr, off, corr);
    pw->buf[tp] = want;
}

/* o==NULL forces the window pure (assume-pure classification: a local BL becomes EV_BL, never
 * EV_SBL) — coerce_reloc_literals uses that to detect fields before their literals are zeroed. */
static Event classify_field(const uint8_t *frm, uint32_t from_size, const FieldDeltaVec *fd,
                            const IVec *ldr, const Op *o, int32_t fp0, int32_t k) {
    uint32_t fpk;
    if (!field_addr(fp0, k, from_size, &fpk)) return (Event){ EV_NONE, 0 };
    int pure = !o || (o->diff[k] == 0 && o->diff[k + 1] == 0 && o->diff[k + 2] == 0 && o->diff[k + 3] == 0);
    if (is_local_bl(frm, from_size, fpk)) {
        if (!pure) return (Event){ EV_SBL, 0 };
        const FieldDelta *r = fd_find_kind(fd, fpk, STREAM_BL);
        return (Event){ EV_BL, r ? r->delta : 0 };
    }
    if (pure && ivec_has(ldr, (int32_t)fpk)) {
        const FieldDelta *r = fd_find_kind(fd, fpk, STREAM_LDR);
        return (Event){ EV_EX, r ? r->delta : 0 };
    }
    return (Event){ EV_NONE, 0 };
}

/* The single encoder mirror of the decoder's sa_apply_op 4-byte-window skeleton (patch_apply.h).
 * fw_next yields, in wire consume order, either a classified field window (is_field=1, pos=window
 * anchor, ev) or one copy position (is_field=0, pos). Direction-parametrized: FWD ascends from 0
 * with anchor==cursor; grow descends from dl-1 with anchor==cursor-3. Every encoder field walk
 * routes through this so no hand-copy can slip the order (a slip flips golden/selfcheck). */
void fw_init(FieldWalk *w, int fwd, const uint8_t *frm, uint32_t from_size,
                    const FieldDeltaVec *fd, const IVec *ldr, const Op *o, int32_t fp0, int32_t dl) {
    w->fwd = fwd; w->dl = dl; w->k = fwd ? 0 : dl - 1;
    w->frm = frm; w->from_size = from_size; w->fd = fd; w->ldr = ldr; w->o = o; w->fp0 = fp0;
}
int fw_next(FieldWalk *w) {
    if (w->fwd ? w->k >= w->dl : w->k < 0) return 0;
    int32_t a0 = w->fwd ? w->k : w->k - 3;
    if (a0 >= 0 && a0 + 4 <= w->dl) {
        Event ev = classify_field(w->frm, w->from_size, w->fd, w->ldr, w->o, w->fp0, a0);
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
        uint16_t up = u16le_at(real + a), lo = u16le_at(real + a + 2);
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
                                  uint32_t from_size, const uint8_t *tob, uint32_t to_size) {
    FieldDeltaVec add = {0};
    OpWalkEnt *walk = opwalk_build(ops);
    for (size_t oi = 0; oi < ops->n; oi++) {
        const OpWalkEnt *we = &walk[oi];
        IVec ldr = op_ldr_set(frm, we->fp, we->o->diff_len, from_size);
        for (int32_t k = 0; k + 4 <= we->o->diff_len; k += 2) {
            uint32_t fpk;
            if (!field_addr(we->fp, k, from_size, &fpk)) continue;
            int64_t tpk = (int64_t)we->tp + k;
            if (tpk < 0 || tpk + 4 > (int64_t)to_size) continue;
            if (is_local_bl(frm, from_size, fpk)) {
                uint16_t tu = u16le_at(tob + (size_t)tpk), tl = u16le_at(tob + (size_t)tpk + 2);
                if (rc_bl_pattern(tu, tl)) {
                    uint16_t fu = u16le_at(frm + fpk), fl2 = u16le_at(frm + fpk + 2);
                    fd_put(&add, fpk, STREAM_BL,
                           (int32_t)((uint32_t)unpack_bl_local(fu, fl2) - (uint32_t)unpack_bl_local(tu, tl)));
                }
            } else if (ivec_has(&ldr, (int32_t)fpk)) {
                fd_put(&add, fpk, STREAM_LDR,
                       (int32_t)(u32le_at(frm + fpk) - u32le_at(tob + (size_t)tpk)));
            }
        }
        free(ldr.v);
    }
    free(walk);
    fd_finalize(&add);
    /* rebuild fd: old entries not overridden + all op-derived entries */
    FieldDeltaVec out = {0};
    for (size_t i = 0; i < fd->n; i++)
        if (!fd_find_kind(&add, fd->v[i].addr, fd->v[i].kind)) fd_put(&out, fd->v[i].addr, fd->v[i].kind, fd->v[i].delta);
    for (size_t i = 0; i < add.n; i++) fd_put(&out, add.v[i].addr, add.v[i].kind, add.v[i].delta);
    fd_finalize(&out);
    free(add.v); free(fd->v);
    *fd = out;
}

/* ---- piecewise shift map (D1): lookups go through rc_smap_at (rc_models.h), the single-sourced
 * mirror of patch_apply smap_at. ---- */

/* residual = delta - pred, wrapped mod 2^32 exactly like the decoder recombines them.
 * BL pred is (shift(pc) - shift(target)) / 2 in imm24 halfword units (C truncation both sides);
 * EX pred is -shift(word value). mn==0 degenerates to residual == delta (no-map wire). */
int32_t field_residual(int kind, const uint8_t *frm, uint32_t fpk, int32_t delta,
                              const uint32_t *mb, const int32_t *mv, int mn) {
    int32_t pred;
    if (kind == EV_BL) {
        uint16_t up = u16le_at(frm + fpk), lo = u16le_at(frm + fpk + 2);
        uint32_t T = fpk + 4u + (uint32_t)(2 * unpack_bl_local(up, lo));
        /* mod-2^32 subtract then /2, bit-identical to the decoder's smap_at recombination
         * (patch_apply.h): a corrupt/degenerate map can make a shift INT32_MIN, so the signed
         * form would be UB — the unsigned wrap is both defined and the exact wire semantics. */
        pred = (int32_t)((uint32_t)rc_smap_at(mb, mv, mn, fpk) - (uint32_t)rc_smap_at(mb, mv, mn, T)) / 2;
    } else {
        pred = (int32_t)(0u - (uint32_t)rc_smap_at(mb, mv, mn, u32le_at(frm + fpk)));
    }
    return (int32_t)((uint32_t)delta - (uint32_t)pred);
}

/* collect every BL/EX field (kind, from-address, shipped delta) over the fixed op plan,
 * walking each op in APPLY direction (mirror op_emit_content / the decoder replay:
 * adjacent windows <4 B apart resolve to a different field set per direction). Entries
 * stay in ascending-fpk order so an unchanged field set fits an identical map. */
FieldRef *collect_fields(const EncCtx *ctx, const OpVec *ops, const OpWalkEnt *walk,
                         const IVec *ldrs, const uint8_t *frm, uint32_t from_size,
                         const FieldDeltaVec *fd, size_t *nout) {
    int FWD = ctx->fwd;
    FieldRef *out = NULL; size_t n = 0, cap = 0;
    for (size_t oi = 0; oi < ops->n; oi++) {
        const OpWalkEnt *we = &walk[oi];
        size_t n0 = n;
        FieldWalk w; fw_init(&w, FWD, frm, from_size, fd, &ldrs[oi], we->o, we->fp, we->o->diff_len);
        while (fw_next(&w)) {
            if (!w.is_field || (w.ev.type != EV_BL && w.ev.type != EV_EX)) continue;
            out = (FieldRef *)vec_reserve(out, &cap, n + 1, sizeof(*out), 256);
            out[n].kind = w.ev.type; out[n].fpk = (uint32_t)(we->fp + w.pos); out[n].delta = w.ev.delta; n++;
        }
        if (!FWD)                                        /* grow yields descending; restore ascending fpk */
            for (size_t a = n0, b = n; a + 1 < b; a++, b--) { FieldRef t = out[a]; out[a] = out[b - 1]; out[b - 1] = t; }
    }
    *nout = n;
    return out;
}

typedef struct { uint32_t b; int32_t v; } SegCand;
static int cmp_seg(const void *a, const void *b) {
    const SegCand *x = (const SegCand *)a, *y = (const SegCand *)b;
    return (x->b > y->b) - (x->b < y->b);
}

/* Build the per-field key table (BL: k1,k2,need; EX: k2=word value, need) and the FULL deduped
 * candidate map (bsdiff op-walk boundaries + span terminator + exact EX value runs, an oversized
 * pool weight-trimmed to SMAP_POOL_MAX, sorted, adjacent-boundary-deduped keeping the later
 * EX-derived value). Shared front half of both fits: the hit-count fit_shift_map (below) and the
 * bits-based fit_shift_map_bits (enc_emit.c). tb/tv are caller buffers of >= SMAP_POOL_MAX
 * entries, fk of >= nfr; returns the full-map entry count. */
int smap_build_full(const OpVec *ops, uint32_t from_size, uint32_t to_size,
                           const uint8_t *frm, const FieldRef *fr, size_t nfr,
                           uint32_t *tb, int32_t *tv, FieldKey *fk) {
    size_t nex = 0;
    for (size_t i = 0; i < nfr; i++) {
        fk[i].kind = fr[i].kind;
        fk[i].k1 = fr[i].fpk;
        fk[i].need = (int32_t)(uint32_t)fr[i].delta;
        if (fr[i].kind == EV_BL) {
            uint16_t up = u16le_at(frm + fr[i].fpk), lo = u16le_at(frm + fr[i].fpk + 2);
            fk[i].k2 = fr[i].fpk + 4u + (uint32_t)(2 * unpack_bl_local(up, lo));
        } else { fk[i].k2 = u32le_at(frm + fr[i].fpk); nex++; }
    }
    /* candidate union: op walk + terminator + exact EX value runs (weight = fields in run) */
    size_t pcap = ops->n + 2 + (nex ? nex : 1), pn = 0;
    SegCand *pool = (SegCand *)xmalloc(pcap * sizeof(*pool));
    uint32_t *pw = (uint32_t *)xmalloc(pcap * sizeof(*pw));
    { OpWalkEnt *walk = opwalk_build(ops);
      for (size_t i = 0; i < ops->n; i++) {
          if (walk[i].o->diff_len > 0 && walk[i].fp >= 0) {
              pool[pn].b = (uint32_t)walk[i].fp; pool[pn].v = walk[i].tp - walk[i].fp;
              pw[pn] = (uint32_t)walk[i].o->diff_len; pn++;
          }
      }
      free(walk);
      pool[pn].b = from_size > to_size ? from_size : to_size; pool[pn].v = 0;
      pw[pn] = 0x7fffffffu; pn++; }                      /* terminator: never pre-trimmed */
    if (nex) {
        SegCand *ex = (SegCand *)xmalloc(nex * sizeof(*ex));
        size_t en = 0;
        for (size_t i = 0; i < nfr; i++)
            if (fk[i].kind == EV_EX) { ex[en].b = fk[i].k2; ex[en].v = -fk[i].need; en++; }
        a1_sort(ex, en, sizeof(*ex), cmp_seg);
        for (size_t i = 0; i < en;) {
            size_t j = i;
            while (j < en && ex[j].v == ex[i].v) j++;
            pool[pn].b = ex[i].b; pool[pn].v = ex[i].v; pw[pn] = (uint32_t)(j - i); pn++;
            i = j;
        }
        free(ex);
    }
    /* pre-trim an oversized pool by weight so elimination stays fast */
    while (pn > SMAP_POOL_MAX) {
        size_t worst = 0;
        for (size_t i = 1; i < pn; i++) if (pw[i] < pw[worst]) worst = i;
        memmove(pool + worst, pool + worst + 1, (pn - worst - 1) * sizeof(*pool));
        memmove(pw + worst, pw + worst + 1, (pn - worst - 1) * sizeof(*pw));
        pn--;
    }
    /* sorted union map (dedupe same boundary: keep the later, i.e. EX value-derived, entry) */
    a1_sort(pool, pn, sizeof(*pool), cmp_seg);
    int mn = 0;
    for (size_t i = 0; i < pn; i++) {
        if (mn && tb[mn - 1] == pool[i].b) { tv[mn - 1] = pool[i].v; continue; }
        tb[mn] = pool[i].b; tv[mn] = pool[i].v; mn++;
    }
    free(pool); free(pw);
    return mn;
}

FieldDeltaVec build_field_deltas(const PairAnalysis *pa, const BlockVec blocks[STREAM_N]) {
    FieldDeltaVec out = {0};
    const int m4_stream_for_field[STREAM_N] = { M4_BL, M4_LDR };
    for (int s = 0; s < STREAM_N; s++) {
        const m4_stream_t *ms = &pa->from_st[m4_stream_for_field[s]];
        for (size_t bi = 0; bi < blocks[s].n; bi++) {
            const Block *b = &blocks[s].v[bi];
            for (int32_t k = 0; k < b->n; k++) {
                size_t idx = (size_t)b->from_offset + (size_t)k;
                if (idx < ms->n) fd_put(&out, ms->a[idx].addr, s, b->values[k]);
            }
        }
    }
    fd_finalize(&out);
    return out;
}

void coerce_reloc_literals(const EncCtx *ctx, OpVec *ops, const uint8_t *frm, uint32_t from_size,
                                  const FieldDeltaVec *fd) {
    int FWD = ctx->fwd;
    int32_t fp0 = 0;
    for (size_t oi = 0; oi < ops->n; oi++) {
        Op *o = &ops->v[oi];
        IVec ldr = op_ldr_set(frm, fp0, o->diff_len, from_size);
        /* o==NULL => assume-pure: a still-dirty field classifies as EV_BL/EV_EX so its
         * literals get zeroed here before they would suppress the field on the wire. */
        FieldWalk w; fw_init(&w, FWD, frm, from_size, fd, &ldr, NULL, fp0, o->diff_len);
        while (fw_next(&w)) {
            if (!w.is_field) continue;
            int32_t k = w.pos;
            uint32_t fpk = 0;
            (void)field_addr(fp0, k, from_size, &fpk);
            const FieldDelta *real = fd_find_kind(fd, fpk, w.ev.type == EV_BL ? STREAM_BL : STREAM_LDR);
            if (real && (o->diff[k] || o->diff[k+1] || o->diff[k+2] || o->diff[k+3])) memset(o->diff + k, 0, 4);
        }
        free(ldr.v);
        fp0 += o->diff_len + o->adj;
    }
}

Op op_copy(int32_t diff_len, const uint8_t *diff, int32_t extra_len, const uint8_t *extra, int32_t adj) {
    Op o;
    o.diff_len = diff_len;
    o.diff = (uint8_t *)xmalloc((size_t)diff_len);
    if (diff_len) memcpy(o.diff, diff, (size_t)diff_len);
    o.extra_len = extra_len;
    o.extra = (uint8_t *)xmalloc((size_t)extra_len);
    if (extra_len) memcpy(o.extra, extra, (size_t)extra_len);
    o.adj = adj;
    return o;
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



/* Dense diff runs can be represented either as literal diff bytes or as an
 * extra span plus an equal source skip. Choose the subset analytically per op:
 * dynamic programming minimizes the modeled cost of the resulting op sequence
 * using the same seeded literal bit-length proxy used by LZ planning, plus the
 * exact raw geometry code lengths. */
enum { SPLIT_GAIN_MARGIN_BITS = 8 };

void split_nonzero_diff_runs(const EncCtx *ctx, OpVec *ops, const Buf *from, const Buf *to) {
    uint8_t L0[256], L1[256];
    from_lit_proxy_bits(from->d, from->n, L0, L1);
    OpVec out = {0};
    int32_t tp = 0;
    for (size_t oi = 0; oi < ops->n; oi++) {
        Op *o = &ops->v[oi];
        Run *runs = NULL;
        size_t nr = 0, rcap = 0;
        for (int32_t scan = 0; scan < o->diff_len;) {
            while (scan < o->diff_len && o->diff[scan] == 0) scan++;
            if (scan >= o->diff_len) break;
            int32_t begin = scan++;
            while (scan < o->diff_len && o->diff[scan] != 0) scan++;
            runs = (Run *)vec_reserve(runs, &rcap, nr + 1, sizeof(runs[0]), 16);
            runs[nr++] = (Run){ begin, scan };
        }
        if (!nr) {
            opvec_push(&out, *o);
            tp += o->diff_len + o->extra_len;
            free(runs);
            continue;
        }
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
        uint64_t *W = (uint64_t *)xmalloc((nr + 1) * sizeof(uint64_t));
        uint64_t *GP = (uint64_t *)xmalloc(nr * sizeof(uint64_t));
        uint32_t *CNT = (uint32_t *)xmalloc((nr + 1) * sizeof(uint32_t));
        uint64_t *X = (uint64_t *)xmalloc(nr * sizeof(uint64_t));
        const uint64_t uleb1 = uleb_proxy_bits(1u, L0);
        W[0] = 0; CNT[0] = 0; GP[0] = 0;
        for (size_t j = 0; j < nr; j++) {
            uint64_t w = 0;
            for (int32_t k = runs[j].begin; k < runs[j].end; k++) w += L0[o->diff[k]];
            w += (uint64_t)(runs[j].end - runs[j].begin - 1) * uleb1;
            W[j + 1] = W[j] + w;
            CNT[j + 1] = CNT[j] + (uint32_t)(runs[j].end - runs[j].begin);
            if (j) GP[j] = GP[j - 1] + uleb_proxy_bits((uint32_t)(runs[j].begin - runs[j - 1].end + 1), L0);
            X[j] = extra_proxy_bits(to, tp + runs[j].begin,
                                    runs[j].end - runs[j].begin, L0, L1);
        }
        const uint64_t XF = extra_proxy_bits(to, tp + o->diff_len, o->extra_len, L0, L1);
        const int FWD = ctx->fwd;
#define DIFFBITS(P, E, ENDPOS, SEG) \
        (uleb_proxy_bits(CNT[E] - CNT[P], L0) + \
         ((E) > (P) ? (FWD ? uleb_proxy_bits((uint32_t)(runs[P].begin - (SEG)), L0) \
                           : uleb_proxy_bits((uint32_t)((ENDPOS) - runs[(E) - 1].end + 1), L0)) \
                      + (W[E] - W[P]) + (GP[(E) - 1] - GP[P]) : 0u))
        uint64_t *E = (uint64_t *)xmalloc((nr + 1) * sizeof(uint64_t));
        int32_t *ft = (int32_t *)xmalloc((nr + 1) * sizeof(int32_t));
        for (size_t p = nr + 1; p-- > 0;) {
            int32_t seg = p ? runs[p - 1].end : 0;
            /* terminal (no further split): the historical DP(nr, p) row */
            uint64_t v = dz_bits_u32((uint32_t)(o->diff_len - seg)) +
                         dz_bits_u32((uint32_t)o->extra_len) +
                         dz_bits_u32(rc_zz32(o->adj)) + 2 +
                         DIFFBITS(p, nr, o->diff_len, seg) + XF;
            int32_t take = -1;
            /* downward scan mirrors the historical backward recurrence: at each s the
             * running v IS DP(s+1, p), and the margin fires under the same condition,
             * so the lowest firing s equals the first TAKE the old walk would hit. */
            for (size_t s = nr; s-- > p;) {
                int32_t len = runs[s].end - runs[s].begin;
                uint64_t c = dz_bits_u32((uint32_t)(runs[s].begin - seg)) +
                             dz_bits_u32((uint32_t)len) +
                             dz_bits_u32(rc_zz32(len)) + 2 +
                             DIFFBITS(p, s, runs[s].begin, seg) + X[s] +
                             E[s + 1];
                if (c + SPLIT_GAIN_MARGIN_BITS < v) { v = c; take = (int32_t)s; }
            }
            E[p] = v; ft[p] = take;
        }
#undef DIFFBITS
        int32_t seg = 0;
        int split_any = 0;
        { size_t p = 0;
          while (ft[p] >= 0) {
              size_t s = (size_t)ft[p];
              int32_t len = runs[s].end - runs[s].begin;
              int32_t pre = runs[s].begin - seg;
              opvec_push(&out, op_copy(pre, o->diff + seg, len,
                                       to->d + (size_t)tp + (size_t)runs[s].begin, len));
              seg = runs[s].end;
              p = s + 1;
              split_any = 1;
          } }
        if (split_any) {
            int32_t tail = o->diff_len - seg;
            if (tail || o->extra_len || o->adj)
                opvec_push(&out, op_copy(tail, o->diff + seg, o->extra_len, o->extra, o->adj));
            free(o->diff);
            free(o->extra);
        } else {
            opvec_push(&out, *o);
        }
        free(W); free(GP); free(CNT); free(X); free(E); free(ft);
        free(runs);
        tp += o->diff_len + o->extra_len;
    }
    free(ops->v);
    *ops = out;
}

size_t preserve_budget_cutoff(const EncCtx *ctx, const OpVec *ops, uint32_t from_size,
                              uint32_t to_size, size_t budget, int32_t *cutoff) {
    int FWD = ctx->fwd;
    int32_t *readarr = preserve_readarr(ctx, ops, from_size);
    OpWalkEnt *m = opwalk_build(ops);
    PreserveBudgetWalk bw = { ctx, readarr, from_size, to_size, budget, 0, 0, 0, 0 };
    for (size_t step = 0; step < ops->n; step++) {
        const OpWalkEnt *we = &m[opwalk_apply_index(ops->n, FWD, step)];
        opwalk_each_byte(FWD, we, preserve_budget_byte, &bw);
    }
    free(m);
    free(readarr);
    *cutoff = bw.cutoff;
    return bw.total;
}

OpPC *preserve_corrections_pc(const EncCtx *ctx, const OpVec *ops, const uint8_t *frm, const uint8_t *true_to,
                              const FieldDeltaVec *fd, uint32_t from_size, uint32_t to_size) {
    int FWD = ctx->fwd;
    size_t span = from_size > to_size ? from_size : to_size;
    uint8_t *ohas = (uint8_t *)xcalloc(span ? span : 1, 1), *oval = (uint8_t *)xcalloc(span ? span : 1, 1);
    int32_t *readarr = preserve_readarr(ctx, ops, from_size);
    OpWalkEnt *m = opwalk_build(ops);
    for (size_t idx = 0; idx < ops->n; idx++) {
        const OpWalkEnt *we = &m[opwalk_apply_index(ops->n, FWD, idx)];
        const Op *o = we->o;
        IVec ldr = op_ldr_set(frm, we->fp, o->diff_len, from_size);
        FieldWalk w; fw_init(&w, FWD, frm, from_size, fd, &ldr, o, we->fp, o->diff_len);
        while (fw_next(&w)) {
            if (!w.is_field || (w.ev.type != EV_BL && w.ev.type != EV_EX)) continue;
            uint8_t packed[4];
            int32_t k = w.pos;
            uint32_t fpk = (uint32_t)(we->fp + k);
            if (w.ev.type == EV_BL) {
                uint16_t up = u16le_at(frm + fpk), lo = u16le_at(frm + fpk + 2);
                pack_bl_local(unpack_bl_local(up, lo) - w.ev.delta, packed);
            } else {
                uint32_t val = u32le_at(frm + fpk);
                u32le_put(packed, val - (uint32_t)w.ev.delta);
            }
            for (int b = 0; b < 4; b++) { ohas[we->tp + k + b] = 1; oval[we->tp + k + b] = packed[b]; }
        }
        free(ldr.v);
    }
    uint8_t *buf = (uint8_t *)xcalloc(span ? span : 1, 1);
    memcpy(buf, frm, from_size);
    uint8_t *jhas = (uint8_t *)xcalloc(from_size ? from_size : 1, 1), *jval = (uint8_t *)xcalloc(from_size ? from_size : 1, 1);
    OpPC *out = (OpPC *)xcalloc(ops->n ? ops->n : 1, sizeof(*out));
    PreserveCorrWalk cw = { ctx, readarr, frm, true_to, ohas, oval, buf, jhas, jval, from_size, NULL };
    for (size_t step = 0; step < ops->n; step++) {
        const OpWalkEnt *we = &m[opwalk_apply_index(ops->n, FWD, step)];
        OpPC *pc = &out[step];
        cw.pc = pc;
        opwalk_each_byte(FWD, we, preserve_corr_byte, &cw);
        if (!FWD) {
            if (pc->pres.n > 1) a1_sort(pc->pres.v, pc->pres.n, sizeof(pc->pres.v[0]), cmp_i32);
            if (pc->corr.n > 1) a1_sort(pc->corr.v, pc->corr.n, sizeof(pc->corr.v[0]), cmp_corr);
        }
    }
    free(buf); free(jhas); free(jval); free(ohas); free(oval); free(m); free(readarr);
    return out;
}
