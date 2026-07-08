/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 */

#ifndef A1_ENC_INTERNAL_H
#define A1_ENC_INTERNAL_H

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arm_cortex_m4.h"
#include "rc_models.h"

#if defined(__GNUC__) || defined(__clang__)
#define ENC_NORETURN __attribute__((noreturn))
#define ENC_ALWAYS_INLINE static inline __attribute__((always_inline))
#else
#define ENC_NORETURN
#define ENC_ALWAYS_INLINE static inline
#endif

extern const char *a1_selfcheck(const uint8_t *blob, size_t blob_n,
                                const uint8_t *from, size_t from_n,
                                const uint8_t *to, size_t to_n);
int divsufsort(const uint8_t *T, int32_t *suffix_array, int32_t n);

typedef struct {
    int fwd;
    int    deg_engaged;
    size_t deg_pres_needed;
    size_t deg_converted;
    size_t opc_splits;
} EncCtx;

static inline int a1_row_covered(const EncCtx *ctx, int64_t a, int64_t t) {
    if (ctx->fwd) return a / A1_OUTROW >= t / A1_OUTROW - (A1_ROW_DEPTH - 1);
    return a / A1_OUTROW <= t / A1_OUTROW + (A1_ROW_DEPTH - 1);
}

enum { STREAM_BL, STREAM_LDR, STREAM_N };

typedef struct { uint8_t *d; size_t n, cap; } Buf;
typedef struct { int32_t diff_len, adj; uint8_t *diff; uint8_t *extra; int32_t extra_len; } Op;
typedef struct { Op *v; size_t n, cap; } OpVec;
typedef struct { int32_t tp, fp; const Op *o; } OpWalkEnt;
typedef void (*OpWalkByteFn)(void *user, const OpWalkEnt *we, int32_t off, int is_diff, uint8_t byte);
typedef struct { int32_t from_offset, n; int32_t *values; } Block;
typedef struct { Block *v; size_t n, cap; } BlockVec;
typedef struct { uint32_t addr; int kind; int32_t delta; } FieldDelta;
typedef struct { FieldDelta *v; size_t n, cap; } FieldDeltaVec;
typedef struct { int32_t *v; size_t n, cap; } IVec;
typedef struct { int32_t off; uint8_t byte; } CorrEnt;
typedef struct { CorrEnt *v; size_t n, cap; } CorrVec;
typedef struct { IVec pres; CorrVec corr; } OpPC;
typedef struct { uint32_t data_off_begin, data_begin, data_end; } Ranges;
typedef struct {
    m4_stream_t from_st[M4_NSTREAMS];
    m4_stream_t to_st[M4_NSTREAMS];
} PairAnalysis;
typedef struct {
    int    deg_engaged;
    size_t deg_pres_needed, deg_converted, opc_splits;
} EncStats;
typedef struct { int variant, fuzz; } PlanCfg;
typedef struct { Buf body; int32_t fp_end, fp_start; EncStats st; } PlanResult;

typedef struct { uint64_t low; uint32_t range; uint8_t cache; uint32_t csz; Buf out; } REnc;
typedef struct { uint8_t code, k; uint16_t u[UG_CTX + 1], m[UG_CTX + 1][UG_CTX + 1]; } UGRiceE;
typedef struct { uint8_t code, k; uint16_t u[UG_CTX + 1], m[UG_GAMMA_MANT]; } UGGammaE;
typedef UGRiceE UGE;  /* compatibility name for model-contract probes: rice keeps the full table */
typedef struct { int32_t *dic; uint16_t cap, K; uint16_t rep[4], hit; uint8_t rh; } DRE;

typedef struct { int type; int32_t start, len, dist; } Token;
typedef struct { Token *v; size_t n, cap; } TokenVec;
typedef struct { int32_t dist, len; } Cand;
#ifndef LZ_CAND_MAX
#define LZ_CAND_MAX 128
#endif
#if LZ_CAND_MAX < 1
#error "LZ_CAND_MAX must be at least 1"
#endif
typedef struct { int32_t pos, len; } OCand;
#define OC_MAX 4
enum { PR_SCALE = 64 };
enum { PRICE_LIT_MAX = 255 * PR_SCALE };
_Static_assert(PRICE_LIT_MAX <= UINT16_MAX, "PriceTab literal prices must fit uint16_t");

enum { EV_NONE, EV_BL, EV_EX, EV_SBL };
typedef struct { int type; int32_t delta; } Event;
typedef struct {
    int fwd; int32_t dl, k;
    const uint8_t *frm; uint32_t from_size;
    const FieldDeltaVec *fd; const Op *o; int32_t fp0;
    int is_field; int32_t pos; Event ev;
} FieldWalk;
typedef struct { int kind; uint32_t k1, k2; int32_t need; } FieldKey;
enum { SMAP_MAX_LOSS = 2, SMAP_POOL_MAX = 160 };

typedef struct {
    uint32_t fspan_c[4], fmatch_c[4];
    uint32_t rep0_yes, rep0_no;
    uint32_t outb_yes, outb_no;
    uint32_t opos_avg;
    uint32_t oexp0;
    int fwd;
    uint16_t lit0[LIT0_CTX][256];
    uint16_t lit1[256];
    UGGammaE gs, gl;
    UGRiceE gd;
    UGGammaE glo;
    int fixed_dist_bits;
    int bootstrap_simple;
    int out_en;
} PriceTab;

typedef struct {
    A1BitTree lit0[LIT0_CTX], lit1;
    A1Flag1 flag;
    A1BitTree dval;
    UGRiceE gd, go;
    UGGammaE gl, gs, glo, pg, pgn, pg2, gdl, gel, gadj;
    uint16_t outb;
    A1IdxUnary dibl, diex;
    DRE dr_bl, dr_ex;
    int32_t dic_bl[DR_KCAP_BL], dic_ex[DR_KCAP_EX];
    uint16_t rep0[2];
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

void die(const char *msg) ENC_NORETURN;
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t s);
void *vec_reserve(void *p, size_t *cap, size_t need, size_t elem_size, size_t init_cap);
void a1_sort(void *base, size_t n, size_t esz, int (*cmp)(const void *, const void *));
void buf_put(Buf *b, uint8_t v);
void buf_write(Buf *b, const void *p, size_t n);
void buf_put_u32le(Buf *b, uint32_t v);
void buf_free(Buf *b);
void opvec_free_deep(OpVec *v);
void oppc_array_free(OpPC *pc, size_t n);
void blockvec_array_free(BlockVec blocks[STREAM_N]);
OpWalkEnt *opwalk_build(const OpVec *ops, int32_t fp_start);
void opwalk_each_byte(int fwd, const OpWalkEnt *we, OpWalkByteFn fn, void *user);
static inline size_t opwalk_apply_index(size_t n, int fwd, size_t step) {
    return fwd ? step : n - 1u - step;
}
#define OP_EVENT_FOR(we_, walk_, n_, fwd_, step_) \
    for (size_t step_ = 0; step_ < (n_) && (((we_) = &(walk_)[opwalk_apply_index((n_), (fwd_), step_)]), 1); step_++)
int read_file_buf(const char *path, Buf *out, uint64_t max_size);
Buf slurp(const char *path);
void write_file(const char *path, const void *p, size_t n);
uint32_t crc32_buf(const uint8_t *p, size_t n);
int bitlen32(uint32_t v);
void put_uleb(Buf *b, uint32_t v);
void put_uleb_overlong(Buf *b, uint32_t v);
void ivec_push(IVec *v, int32_t x);
void corr_push(CorrVec *v, int32_t off, uint8_t byte);
int cmp_i32(const void *a, const void *b);
int cmp_corr(const void *a, const void *b);
void opvec_push(OpVec *v, Op o);
void blockvec_push(BlockVec *v, int32_t fo, int32_t *vals, int32_t n);
void fd_put(FieldDeltaVec *v, uint32_t addr, int kind, int32_t delta);
void fd_finalize(FieldDeltaVec *v);
const FieldDelta *fd_find_kind(const FieldDeltaVec *v, uint32_t addr, int kind);

Ranges elf_ranges(const char *elf_path, const Buf *bin, const char *which);
void pair_analysis_init(PairAnalysis *pa, const Buf *from, const Buf *to,
                        const Ranges *fr, const Ranges *tr);
void pair_analysis_free(PairAnalysis *pa);
void data_format_encode(const Buf *from, const Buf *to, const PairAnalysis *pa,
                        Buf *from_df, Buf *to_df, BlockVec blocks[STREAM_N], int mask_bl);
OpVec bsdiff_ops(const Buf *from, const Buf *to, int fuzz);

void mask_bl_imms(const uint8_t *real, uint8_t *mut, size_t n);
void fw_init(FieldWalk *w, int fwd, const uint8_t *frm, uint32_t from_size,
             const FieldDeltaVec *fd, const Op *o, int32_t fp0, int32_t dl);
int fw_next(FieldWalk *w);
void merge_op_field_deltas(FieldDeltaVec *fd, const OpVec *ops, const uint8_t *frm,
                           uint32_t from_size, const uint8_t *tob, uint32_t to_size);
int32_t field_residual(int kind, const uint8_t *frm, uint32_t fpk, int32_t delta,
                       const uint32_t *mb, const int32_t *mv, int mn);
int smap_build_full(const OpVec *ops, int32_t fp_start, uint32_t from_size, uint32_t to_size,
                    const FieldKey *fk, size_t nfr, uint32_t *tb, int32_t *tv);
FieldDeltaVec build_field_deltas(const PairAnalysis *pa, const BlockVec blocks[STREAM_N]);
void coerce_reloc_literals(const EncCtx *ctx, OpVec *ops, const uint8_t *frm, uint32_t from_size,
                           const FieldDeltaVec *fd);
Op op_copy(int32_t diff_len, const uint8_t *diff, int32_t extra_len, const uint8_t *extra, int32_t adj);
#define op_free_payload(o_) do { free((o_)->diff); free((o_)->extra); } while (0)
void split_nonzero_diff_runs(const EncCtx *ctx, OpVec *ops, const Buf *from, const Buf *to);
size_t preserve_budget_cutoff(const EncCtx *ctx, const OpVec *ops, uint32_t from_size,
                              uint32_t to_size, size_t budget, int32_t *cutoff);
OpPC *preserve_corrections_pc(const EncCtx *ctx, const OpVec *ops, int32_t fp_start,
                              const uint8_t *frm, const uint8_t *true_to,
                              const FieldDeltaVec *fd, uint32_t from_size, uint32_t to_size);

void re_init(REnc *r);
void re_bit(REnc *r, uint16_t *prob, int bit, int rate);
void re_raw(REnc *r, int bit);
Buf re_flush_opt(REnc *r);
void put_raw_bits(REnc *r, uint32_t v, int nb);
void bt_encode(A1BitTree *t, REnc *r, uint8_t byte, int rate);
void lit_tree_seed_e(const uint8_t *frm, size_t n, int parity, A1BitTree *t);
void ug_init_e_impl(void *g, char code, int k, size_t sz);
#define ug_init_e(g, code, k) ug_init_e_impl((g), (code), (k), sizeof(*(g)))
void ug_seed_cont_e(void *g, int depth);
void ug_encode(void *g, REnc *r, uint32_t v);
void idx_encode(A1IdxUnary *g, REnc *r, uint32_t v);
void fl_encode(A1Flag1 *f, REnc *r, int b);
void models_init_content(Models *m, const uint8_t *frm, uint32_t from_size, int kd, int ko);
uint32_t ug_price(const void *g, uint32_t v);
uint32_t bt_price_static(const A1BitTree *t, uint8_t byte);
uint32_t bit_price_update(uint16_t *prob, int bit, int rate);
uint64_t bt_price_update(A1BitTree *t, uint8_t byte, int rate);

void from_lit_proxy_bits(const uint8_t *frm, size_t n, uint8_t L0[256], uint8_t L1[256]);
void content_cursor_init(ContentCursor *cc, const TokenVec *seq,
                         const uint8_t *content, const uint8_t *tags, size_t content_n,
                         Models *m, REnc *rc, int fwd, int out_en, uint32_t oexp);
void content_cursor_to(ContentCursor *cc, size_t end, ContentStats *stats);
void out_candidates(const uint8_t *content, size_t n, const uint32_t *olim,
                    const uint32_t *olim2, const uint32_t *ocap, int FWD,
                    const uint8_t *to, size_t to_n, const uint8_t *frm, size_t from_n,
                    OCand (**oc_out)[OC_MAX], uint8_t **noc_out);
void measure_prices(const TokenVec *seq, const uint8_t *content, const uint8_t *tags,
                    const uint8_t *frm, size_t from_size, int dk, int ko, PriceTab *pt);
TokenVec lz_parse_priced(size_t n, const uint8_t *content, const uint8_t *tags,
                         Cand (*cands)[LZ_CAND_MAX], uint8_t *ncand,
                         OCand (*ocands)[OC_MAX], const uint8_t *nocand,
                         const PriceTab *pt);
void merge_adjacent_spans(TokenVec *tv);
int fit_k_tokens(const TokenVec *tv);
int fit_k_out(const TokenVec *tv, int cur, uint32_t oexp0, int fwd);
TokenVec lz_candidates_c(const uint8_t *data, const uint8_t *tags, size_t n,
                         const uint8_t L0[256], const uint8_t L1[256],
                         int *k_out, Cand (**cands_out)[LZ_CAND_MAX], uint8_t **ncand_out);
uint64_t gammalen_u32(uint32_t x);
uint32_t bit_price(uint32_t p, int bit);

Buf encode_body(const EncCtx *ctx, const OpVec *ops, const uint8_t *frm, uint32_t from_size,
                const uint8_t *tob, uint32_t to_size,
                const FieldDeltaVec *fd, const OpPC *pc, int32_t fp_start,
                int *overflow_out);

PlanResult plan_encode(EncCtx *ctx, const Buf *from, const Buf *to, const PairAnalysis *pa, PlanCfg cfg);

void encode_a1(const char *from_image, const char *to_image, const char *patch_out);
int decode_a1(const char *image_path, const char *patch_path);

#endif /* A1_ENC_INTERNAL_H */
