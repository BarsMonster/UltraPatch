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

enum { STREAM_DATA, STREAM_CODE, STREAM_BL, STREAM_LDR, STREAM_N };

typedef struct { uint8_t *d; size_t n, cap; } Buf;
typedef struct { int32_t diff_len, adj; uint8_t *diff; uint8_t *extra; int32_t extra_len; } Op;
typedef struct { Op *v; size_t n, cap; } OpVec;
typedef struct { int32_t tp, fp; const Op *o; size_t orig; } OpWalkEnt;
typedef void (*OpWalkByteFn)(void *user, const OpWalkEnt *we, int32_t off, int is_diff, uint8_t byte);
typedef struct { int32_t from_offset, to_address, n; int64_t *values; } Block;
typedef struct { Block *v; size_t n, cap; } BlockVec;
typedef struct { uint32_t addr; int kind; int64_t delta; } FieldDelta;
typedef struct { FieldDelta *v; size_t n, cap; } FieldDeltaVec;
typedef struct { int32_t *v; size_t n, cap; } IVec;
typedef struct { int32_t off; uint8_t byte; } CorrEnt;
typedef struct { CorrEnt *v; size_t n, cap; } CorrVec;
typedef struct { IVec pres; CorrVec corr; } OpPC;
typedef struct { uint32_t data_off_begin, data_off_end, data_begin, data_end, code_begin, code_end; } Ranges;
typedef struct {
    int    deg_engaged;
    size_t deg_pres_needed, deg_converted, opc_splits;
} EncStats;
typedef struct { int variant, fuzz; } PlanCfg;

typedef struct { uint64_t low; uint32_t range; uint8_t cache; uint32_t csz; Buf out; } REnc;
typedef struct { uint8_t code, k; uint16_t u[UG_CTX + 1], m[UG_CTX + 1][UG_CTX + 1]; } UGE;
typedef struct { int64_t *dic; uint16_t cap, K; int64_t last; uint16_t rep[4], hit; uint8_t rh; } DRE;

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

enum { EV_NONE, EV_BL, EV_EX, EV_SBL };
typedef struct { int type; int64_t delta; } Event;
typedef struct {
    int fwd; int32_t dl, k;
    const uint8_t *frm; uint32_t from_size;
    const FieldDeltaVec *fd; const IVec *ldr; const Op *o; int32_t fp0;
    int is_field; int32_t pos; Event ev;
} FieldWalk;
typedef struct { int kind; uint32_t fpk; int64_t delta; } FieldRef;
typedef struct { int kind; uint32_t k1, k2; int32_t need; } FieldKey;
enum { SMAP_MAX_LOSS = 2, SMAP_POOL_MAX = 160 };

typedef struct {
    uint32_t fspan_c[4], fmatch_c[4];
    uint32_t rep0_yes, rep0_no;
    uint32_t outb_yes, outb_no;
    uint32_t opos_avg;
    uint32_t oexp0;
    int fwd;
    uint32_t lit0[LIT0_CTX][256];
    uint32_t lit1[256];
    UGE gs, gl, gd;
    UGE go, glo;
    int dk;
    int valid;
} PriceTab;

typedef struct {
    A1BitTree lit0[LIT0_CTX], lit1;
    A1Flag1 flag;
    A1BitTree dval;
    UGE gd, gl, gs, go, glo, pg, pgn, pg2, gdl, gel, gadj;
    uint16_t outb;
    A1IdxUnary dibl, diex;
    DRE dr_bl, dr_ex;
    int64_t dic_bl[DR_KCAP_BL], dic_ex[DR_KCAP_EX];
    uint16_t rep0[2];
    int rep0h;
    int32_t last_dist;
} Models;

void die(const char *msg) ENC_NORETURN;
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t s);
void *xrealloc(void *p, size_t n);
void *vec_reserve(void *p, size_t *cap, size_t need, size_t elem_size, size_t init_cap);
void a1_sort(void *base, size_t n, size_t esz, int (*cmp)(const void *, const void *));
void buf_put(Buf *b, uint8_t v);
void buf_write(Buf *b, const void *p, size_t n);
void buf_put_u32le(Buf *b, uint32_t v);
void buf_free(Buf *b);
void opvec_free_deep(OpVec *v);
void oppc_array_free(OpPC *pc, size_t n);
void blockvec_array_free(BlockVec blocks[STREAM_N]);
OpWalkEnt *opwalk_build(const OpVec *ops);
void opwalk_each_byte(int fwd, const OpWalkEnt *we, OpWalkByteFn fn, void *user);
static inline size_t opwalk_apply_index(size_t n, int fwd, size_t step) {
    return fwd ? step : n - 1u - step;
}
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
void blockvec_push(BlockVec *v, int32_t fo, int32_t ta, const int64_t *vals, int32_t n);
void fd_put(FieldDeltaVec *v, uint32_t addr, int kind, int64_t delta);
void fd_finalize(FieldDeltaVec *v);
const FieldDelta *fd_find_kind(const FieldDeltaVec *v, uint32_t addr, int kind);

Ranges elf_ranges(const char *elf_path, const Buf *bin, const char *which);
void data_format_encode(const Buf *from, const Buf *to, const Ranges *fr, const Ranges *tr,
                        Buf *from_df, Buf *to_df, BlockVec blocks[STREAM_N], int mask_bl);
OpVec bsdiff_ops(const Buf *from, const Buf *to, int fuzz);

void mask_bl_imms(const uint8_t *real, uint8_t *mut, size_t n);
IVec op_ldr_set(const uint8_t *frm, int32_t fp0, int32_t dl, uint32_t from_size);
void fw_init(FieldWalk *w, int fwd, const uint8_t *frm, uint32_t from_size,
             const FieldDeltaVec *fd, const IVec *ldr, const Op *o, int32_t fp0, int32_t dl);
int fw_next(FieldWalk *w);
void merge_op_field_deltas(FieldDeltaVec *fd, const OpVec *ops, const uint8_t *frm,
                           uint32_t from_size, const uint8_t *tob, uint32_t to_size);
int64_t field_residual(int kind, const uint8_t *frm, uint32_t fpk, int64_t delta,
                       const uint32_t *mb, const int32_t *mv, int mn);
FieldRef *collect_fields(const EncCtx *ctx, const OpVec *ops, const uint8_t *frm, uint32_t from_size,
                         const FieldDeltaVec *fd, size_t *nout);
int smap_build_full(const OpVec *ops, uint32_t from_size, uint32_t to_size,
                    const uint8_t *frm, const FieldRef *fr, size_t nfr,
                    uint32_t *tb, int32_t *tv, FieldKey *fk);
FieldDeltaVec build_field_deltas(const Buf *from, const Ranges *fr, const BlockVec blocks[STREAM_N]);
void coerce_reloc_literals(const EncCtx *ctx, OpVec *ops, const uint8_t *frm, uint32_t from_size,
                           uint32_t to_size, const FieldDeltaVec *fd);
Op op_copy(int32_t diff_len, const uint8_t *diff, int32_t extra_len, const uint8_t *extra, int32_t adj);
void split_nonzero_diff_runs(const EncCtx *ctx, OpVec *ops, const Buf *from, const Buf *to);
uint8_t *preserve_indices(const EncCtx *ctx, const OpVec *ops, uint32_t from_size, uint32_t to_size);
OpPC *preserve_corrections_pc(const EncCtx *ctx, const OpVec *ops, const uint8_t *frm, const uint8_t *true_to,
                              const FieldDeltaVec *fd, uint32_t from_size, uint32_t to_size,
                              const uint8_t *presset);

void re_init(REnc *r);
void re_bit(REnc *r, uint16_t *prob, int bit, int rate);
void re_raw(REnc *r, int bit);
Buf re_flush_opt(REnc *r);
void put_raw_bits(REnc *r, uint32_t v, int nb);
void bt_encode(A1BitTree *t, REnc *r, uint8_t byte, int rate);
void lit_tree_seed_e(const uint8_t *frm, size_t n, int parity, A1BitTree *t);
void ug_init_e(UGE *g, char code, int k);
int ug_c(int x);
void ug_seed_cont_e(UGE *g, int depth);
void ug_encode(UGE *g, REnc *r, uint32_t v);
void idx_encode(A1IdxUnary *g, REnc *r, uint32_t v);
void fl_encode(A1Flag1 *f, REnc *r, int b);
void models_init_content(Models *m, const uint8_t *frm, uint32_t from_size, int kd, int ko);

void from_lit_proxy_bits(const uint8_t *frm, size_t n, uint8_t L0[256], uint8_t L1[256]);
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
TokenVec lz_candidates_c(const uint8_t *data, size_t n, const uint16_t *litbits,
                         int *k_out, Cand (**cands_out)[LZ_CAND_MAX], uint8_t **ncand_out);
uint64_t gammalen_u32(uint32_t x);
uint32_t bit_price(uint32_t p, int bit);

extern int g_emit_overflow;
void bv_encode(A1BitTree *t, REnc *r, int64_t x);
void dr_init_e(DRE *d, int64_t *dic, int cap, uint16_t hitseed);
void emit_delta(Models *M, REnc *r, int kind, int64_t delta);
int32_t fold_zero_ops(const OpVec *ops, int32_t *eff_adj, uint8_t *skip);
Buf encode_body(const EncCtx *ctx, const OpVec *ops, const uint8_t *frm, uint32_t from_size,
                const uint8_t *tob, uint32_t to_size,
                const FieldDeltaVec *fd, const OpPC *pc);

Buf plan_encode(EncCtx *ctx, const Buf *from, const Buf *to, const Ranges *fr, const Ranges *tr,
                PlanCfg cfg, int32_t *fp_end_out, int32_t *fp_start_out, EncStats *st_out);

void encode_a1(const char *from_image, const char *to_image, const char *patch_out);
int decode_a1(const char *image_path, const char *patch_path);

#endif /* A1_ENC_INTERNAL_H */
