/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 */

#ifndef ENC_INTERNAL_H
#define ENC_INTERNAL_H

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rc_models.h"

/* The encoder uses the canonical constants directly from patch_config.h;
 * PATCH_IMAGE_BASE and PATCH_IMAGE_CAPACITY are decoder-only. */

/* Encoder-only wire helpers. The decoder never reads a little-endian u16/u32, so these host-only
 * readers stay out of the shipped decoder headers (rc_u32le_put IS decoder-used and remains in
 * rc_models.h). */
static inline uint16_t rc_u16le(const uint8_t *p){ return (uint16_t)(p[0] | ((uint16_t)p[1]<<8)); }
static inline uint32_t rc_u32le(const uint8_t *p){
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}

/* Encode-side zigzag/uLEB/envelope-direction/out-match helpers. Each mirrors a decoder-direction
 * twin in rc_models.h (rc_unzz32_value, rc_zz_abs, rc_uleb_overlong, rc_outmatch_pos); no
 * decoder call site reads these, so they stay out of the shipped decoder headers. */
static inline uint32_t rc_zz32(int32_t v){
    uint32_t u=(uint32_t)v;
    return v<0 ? ((0u-u)<<1)-1u : u<<1;
}
static inline uint32_t rc_outmatch_delta(uint32_t pos, uint32_t expected){
    return rc_zz32(rc_i32_from_u32(pos - expected));
}
static inline int rc_uleb_len(uint32_t v){
    int n=1;
    while(v>>=7) n++;
    return n;
}
/* Envelope direction marker. The natural direction is descending iff the image grows;
 * the overlong uLEB marker flips that choice. */
static inline int rc_natural_desc(uint32_t from_size,uint32_t to_size){ return to_size>from_size; }
static inline int rc_dir_is_natural(uint32_t from_size,uint32_t to_size,int desc){
    return desc==rc_natural_desc(from_size,to_size);
}

/* RC_NORETURN comes from the shared portability shim in rc_models.h. */

extern const char *selfcheck(const uint8_t *blob, size_t blob_n,
                                const uint8_t *from, size_t from_n,
                                const uint8_t *to, size_t to_n);
int divsufsort(const uint8_t *T, int32_t *suffix_array, int32_t n);

typedef struct {
    int fwd;
    int deg_engaged;
} EncCtx;

static inline int row_covered(const EncCtx *ctx, int64_t a, int64_t t) {
    if (ctx->fwd) return a / OUTROW >= t / OUTROW - (OUTROW_DEPTH - 1);
    return a / OUTROW <= t / OUTROW + (OUTROW_DEPTH - 1);
}

typedef struct { uint8_t *d; size_t n, cap; } Buf;
/* Plan geometry only; content is derived from the immutable source and target images. */
typedef struct { int32_t diff_len, extra_len, adj; } Op;
typedef struct { Op *v; size_t n, cap; } OpVec;
typedef struct { int32_t tp, fp; const Op *o; } OpWalkEnt;
static inline uint8_t op_diff_byte(const uint8_t *frm, uint32_t from_size,
                                   const uint8_t *tob, int32_t fp, int32_t tp) {
    uint8_t src = fp >= 0 && (uint32_t)fp < from_size ? frm[fp] : 0;
    return (uint8_t)(tob[tp] - src);
}
/* One entry per aligned source word: distance to its nearest preceding LDR halfword (0 = none). */
typedef struct { uint16_t *back; } LdrTargetIndex;
typedef struct { int32_t *v; size_t n, cap; } IVec;
typedef struct {
    int deg_engaged;
} EncStats;
enum { PLAN_RAW_11, PLAN_RAW_6, PLAN_RAW_20, PLAN_RAW_N };
enum { PLAN_SPEC_N = 4 };
/* The ordered plan registry is the sole definition of sweep order. raw_key selects one of the
 * three prepared bsdiff results; merge_fields enables op-derived relocation deltas. */
typedef struct {
    uint8_t merge_fields, raw_key;
} PlanSpec;
extern const PlanSpec PLAN_SPECS[PLAN_SPEC_N];
/* Immutable source-derived literal models, built once per image pair and reused by every plan. */
typedef struct { up_BitTree lit0, lit1; } LitSeedTrees;
typedef struct {
    LitSeedTrees seeds;
    uint8_t L0[256], L1[256];
} SourceLitModels;
/* Pair-owned immutable planning inputs. Every plan clones its op state before mutation. */
typedef struct {
    OpVec raw[PLAN_RAW_N];
    LdrTargetIndex ldr;
    SourceLitModels lit;
} PlanPrep;
typedef struct { OpVec ops; int32_t fp_end, fp_start; EncStats st; } PlanGeometry;
typedef struct { Buf body; int32_t fp_end, fp_start; EncStats st; } PlanResult;

typedef struct {
    uint64_t low;
    uint32_t range;
    uint8_t cache;
    uint32_t csz;
    Buf out;
    int coding_overflow;
} REnc;
/* up_UGRice/up_UGGamma (shared wire model structs) are single-sourced in rc_models.h; the encoder uses
 * them directly (no runtime 'code' tag). DRE wraps the shared up_DRStream with the host-only MTF cache
 * pointer + cap; the shared fields (K/rep/hit/rh) live in .s so rc_dr_init can init both sides. */
typedef struct { int32_t *dic; uint16_t cap; up_DRStream s; } DRE;

typedef struct { int type; int32_t start, len, dist; } Token;
typedef struct { Token *v; size_t n, cap; } TokenVec;
typedef struct { int32_t dist, len; } Cand;
typedef Buf CandArena;
#ifndef LZ_CAND_MAX
#define LZ_CAND_MAX 128
#endif
#if LZ_CAND_MAX < 1
#error "LZ_CAND_MAX must be at least 1"
#endif
enum { LZ_MAX_RUN = 1024, LZ_MAX_MATCH = 2048 };
typedef struct { int32_t pos, len; } OCand;
typedef struct {
    int32_t *to_head, *to_prev, *from_head, *from_prev;
    int fwd, built;
} OutIndex;
enum { PR_SCALE = 64 };
enum { PRICE_LIT_MAX = 255 * PR_SCALE };
_Static_assert(PRICE_LIT_MAX <= UINT16_MAX, "PriceTab literal prices must fit uint16_t");

enum { EV_NONE, EV_BL, EV_EX, EV_SBL };
typedef struct { int type; int32_t delta; uint32_t k2; } Event;
/* Deltas injected between content bytes live in one decoder-apply-order arena. Shift-map fitting
 * sees the historical source order by globally reversing this arena for a reverse apply. */
typedef struct { uint32_t cc; int kind; uint32_t k1; int32_t need; uint32_t k2; } FieldInj;
typedef struct { FieldInj *v; size_t n, cap; } FieldInjArena;
typedef struct { size_t content_end, inj_end; } OpEmitRow;
static inline const FieldInj *field_inj_key(const FieldInjArena *a, int fwd, size_t i) {
    return &a->v[fwd ? i : a->n - 1u - i];
}
enum { SMAP_MAX_LOSS = 2, SMAP_POOL_MAX = 160 };

typedef struct {
    uint32_t fspan_c[4], fmatch_c[4];
    uint32_t rep0_yes, rep0_no;
    uint32_t outb_yes, outb_no;
    uint32_t opos_avg;
    uint32_t oexp0;
    int fwd;
    uint16_t lit0[UP_LIT0_CTX][256];
    uint16_t lit1[256];
    up_UGGamma gs, gl;
    up_UGRice gd;
    up_UGGamma glo;
    int fixed_dist_bits;
} PriceTab;

typedef struct {
    up_BitTree lit0[UP_LIT0_CTX], lit1;
    up_PreKdModels pre;   /* dval/dibl/diex/gdl/gel/gadj (rc_init_prekd, rc_models.h) */
    up_TokModels tok;     /* gd/go/gl/gs/glo/outb/flag/rep0 (rc_init_tok, rc_models.h) */
    DRE dr_bl, dr_ex;
    int32_t dic_bl[DR_KCAP_BL], dic_ex[DR_KCAP_EX];
    int rep0h;
    int32_t last_dist;
} Models;

typedef struct {
    uint64_t oy_cost, on_cost, op_cost, r0y_cost, r0n_cost;
    uint32_t oy_n, on_n, op_n, r0y_n, r0n_n;
} ContentStats;

typedef struct {
    const TokenVec *seq;
    const uint8_t *content;
    const uint8_t *tags;
    size_t content_n;
    Models *M;
    REnc *rc;
    int fwd, out_en;
    uint32_t oexp;
    size_t tok_i, pos, span_pos;
    int tok_mode;
    int32_t tok_left;
    int last_span;
    uint8_t prevlit;
    Token cur;
} ContentCursor;

void die(const char *msg) RC_NORETURN;
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t s);
void *vec_reserve(void *p, size_t *cap, size_t need, size_t elem_size, size_t init_cap);
void buf_put(Buf *b, uint8_t v);
void buf_write(Buf *b, const void *p, size_t n);
void buf_put_u32le(Buf *b, uint32_t v);
void buf_free(Buf *b);
void opvec_free(OpVec *v);
OpWalkEnt *opwalk_build(const OpVec *ops, int32_t fp_start);
/* Decoder-order index for step `step` of an n-op apply walk (forward: 0..n-1; reverse: n-1..0).
 * Callers loop `for (step=0; step<n; step++) we = &walk[opwalk_apply_index(n, fwd, step)];`. */
static inline size_t opwalk_apply_index(size_t n, int fwd, size_t step) {
    return fwd ? step : n - 1u - step;
}
void read_file_buf(const char *path, Buf *out, uint64_t max_size);
int file_alias(const char *a, const char *b);
void reject_if_alias(const char *a, const char *b, const char *what);
void replace_file(const char *path, const void *p, size_t n);
uint32_t crc32_buf(const uint8_t *p, size_t n);
int bitlen32(uint32_t v);
void put_uleb(Buf *b, uint32_t v);
void put_uleb_overlong(Buf *b, uint32_t v);
void ivec_push(IVec *v, int32_t x);
void opvec_push(OpVec *v, Op o);

void bsdiff_ops_all(const Buf *from, const Buf *to, OpVec out[PLAN_RAW_N]);

void ldr_target_index_build(LdrTargetIndex *idx, const uint8_t *source, uint32_t source_size);
void ldr_target_index_free(LdrTargetIndex *idx);
int smap_build_full(const OpVec *ops, int32_t fp_start, uint32_t from_size, uint32_t to_size,
                    const FieldInjArena *inj, int fwd, uint32_t *tb, int32_t *tv);
void split_nonzero_diff_runs(const EncCtx *ctx, OpVec *ops,
                             const Buf *from, const Buf *to,
                             const SourceLitModels *lit);

void re_init(REnc *r);
void re_bit(REnc *r, uint16_t *prob, int bit, int rate);
void re_raw(REnc *r, int bit);
Buf re_flush_opt(REnc *r);
void put_raw_bits(REnc *r, uint32_t v, int nb);
void bt_encode(up_BitTree *t, REnc *r, uint8_t byte, int rate);
void lit_seed_trees_init(LitSeedTrees *s, const uint8_t *frm, size_t n);
void ugr_init_e(up_UGRice *g, int k);
void ugg_init_e(up_UGGamma *g);
void ugr_encode(up_UGRice *g, REnc *r, uint32_t v);
void ugg_encode(up_UGGamma *g, REnc *r, uint32_t v);
uint32_t ug_bit_xfer(REnc *r, uint16_t *prob, int bit, int adapt);
uint32_t unary_xfer(REnc *r, uint16_t *u, uint32_t clampmax, uint32_t v, int adapt);
void fl_encode(up_Flag1 *f, REnc *r, int b);
void models_init_content(Models *m, const LitSeedTrees *seeds, int kd, int ko);
uint32_t ugr_price(const up_UGRice *g, uint32_t v);
uint32_t ugg_price(const up_UGGamma *g, uint32_t v);
uint32_t bt_price_static(const up_BitTree *t, uint8_t byte);
uint32_t bit_price_update(uint16_t *prob, int bit, int rate);
uint64_t bt_price_update(up_BitTree *t, uint8_t byte, int rate);

void from_lit_proxy_bits(const LitSeedTrees *seeds, uint8_t L0[256], uint8_t L1[256]);
void content_cursor_init(ContentCursor *cc, const TokenVec *seq,
                         const uint8_t *content, const uint8_t *tags, size_t content_n,
                         Models *m, REnc *rc, int fwd, int out_en, uint32_t oexp);
void content_cursor_to(ContentCursor *cc, size_t end, ContentStats *stats);
OCand *out_candidates(const uint8_t *content, size_t n, const OpVec *ops,
                      const OpWalkEnt *walk, const OpEmitRow *rows, int FWD,
                      OutIndex *index,
                      const uint8_t *to, size_t to_n, const uint8_t *frm, size_t from_n);
void out_index_free(OutIndex *index);
void measure_prices(const TokenVec *seq, const uint8_t *content, const uint8_t *tags,
                    const LitSeedTrees *seeds, int dk, int ko, PriceTab *pt);
TokenVec lz_parse_priced(size_t n, const uint8_t *content, const uint8_t *tags,
                         const CandArena *cands, const uint8_t *ncand,
                         const OCand *ocands, int32_t max_out_len, const PriceTab *pt);
void merge_adjacent_spans(TokenVec *tv);
int fit_k_tokens(const TokenVec *tv);
int fit_k_out(const TokenVec *tv, int cur, uint32_t oexp0, int fwd);
TokenVec lz_candidates_c(const uint8_t *data, const uint8_t *tags, size_t n,
                         const uint8_t L0[256], const uint8_t L1[256],
                         int *k_out, CandArena *cands_out, uint8_t **ncand_out);
uint64_t gammalen_u32(uint32_t x);
uint32_t bit_price(uint32_t p, int bit);

Buf encode_body(const EncCtx *ctx, const OpVec *ops, const uint8_t *frm, uint32_t from_size,
                const uint8_t *tob, uint32_t to_size,
                const LdrTargetIndex *ldr, const SourceLitModels *lit, int merge_fields,
                int32_t fp_start, OutIndex *out_index, int *overflow_out);

void plan_prepare(PlanPrep *prep, const Buf *from, const Buf *to);
void plan_prepare_free(PlanPrep *prep);
void plan_geometry_prepare(PlanGeometry *geom, EncCtx *ctx,
                           const Buf *from, const Buf *to,
                           const PlanPrep *prep, int raw_key);
void plan_geometry_free(PlanGeometry *geom);
PlanResult plan_encode(EncCtx *ctx, const Buf *from, const Buf *to,
                       const PlanPrep *prep, const PlanGeometry *geom,
                       int merge_fields, OutIndex *out_index);

void encode_patch(const char *from_image, const char *to_image, const char *patch_out);
int decode_patch(const char *image_path, const char *patch_path);

#endif /* ENC_INTERNAL_H */
