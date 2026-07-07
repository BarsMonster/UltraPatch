/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 */

#ifndef PATCH_APPLY_H
#define PATCH_APPLY_H
/*
 * ultrapatch v3-on-flash (A1) — streaming, in-place, real-NVM firmware decoder (C).
 * Production solution; integration and release gates are documented under docs/.
 *
 * The patch arrives BYTE-BY-BYTE over a slow link. The integrator supplies a byte-source
 * callback (which may block internally — e.g. poll a UART ring) and passes the blob —
 * envelope (both CRCs and the compressed body length in the header) followed by the range-coded body — through one
 * patch_apply_run(state, callback, ctx) call whose return is the DONE/ERROR verdict; the whole decode
 * runs synchronously on the CALLER's stack (no coroutine, no fiber, no private decode stack).
 * The decoder itself parses the envelope (sizes, direction, span) and gates on CRC32(from)
 * BEFORE the first flash write and CRC32(to) after the apply. The body is the last thing on the
 * wire and the decoder zero-fills internally after the header's compressed byte count, so there
 * is no trailer or withhold ring. A single divide-free binary range coder (LZMA bound; no
 * division — Cortex-M0+ has no HW divide) decodes ONE interleaved stream whose symbols are
 * emitted in decode-consumption order (no length prefixes, no byte-aligned sections).
 * Event-driven producers (ISR push) integrate via the optional SPSC ring adapter in
 * patch_apply_push_adapter.h, which stays OUTSIDE this device artifact.
 *
 * Reconstruction is NO-BAKE (A1): NO source writes. The [A] copy reads RAW from[fp]; the new image
 * is corrected at the monotonic output frontier by the to-ordered additive corrections [C]
 * (out[tp] = db + raw_from[fp] + corr[tp]). Relocation fields are de-relocated on the fly: bl
 * positions are DERIVED by a local halfword pattern; ldr positions are DERIVED per op (a field is
 * ldr iff an ldr-literal instruction in the same op's copy range targets it) — so positions are
 * never shipped, only the per-field delta VALUES, pulled inline from the single stream (adaptive
 * MTF dict + order-1/zero-context repeat bit). A copy that reads a from-byte already overwritten by
 * the output frontier reads it from the never-evict journal, driven by the preserve events [P]. Output is
 * staged in a monotonic 256 B row write-back cache so each NVM row is erased+programmed exactly
 * once (no write amplification).
 *
 * RAM (hard gate <=12 KiB SRAM): entropy models + the never-evict journal + the LZSS history ring
 * + the packed 576 B ldr-derive pristine metadata; the image lives ONLY in flash (0 image bytes in RAM).
 *
 * Embedded target: NO 64-bit integers ANYWHERE in the decoder — all positions/sizes are 32-bit and
 * every bounds/overflow guard is done in 32-bit (headroom tests plus __builtin_add/sub_overflow), so
 * the ARM object emits no libgcc 64-bit (or float) helpers, only one 32-bit divide at init.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "rc_models.h"

#if defined(__GNUC__) || defined(__clang__)
#define A1_NOINLINE __attribute__((noinline))
#define A1_ALWAYS_INLINE static inline __attribute__((always_inline))
#define A1_ADD_OVERFLOW(a,b,out) __builtin_add_overflow((a),(b),(out))
#define A1_SUB_OVERFLOW(a,b,out) __builtin_sub_overflow((a),(b),(out))
#else
#define A1_NOINLINE
#define A1_ALWAYS_INLINE static inline
static inline int a1_add_overflow_i32(int32_t a,int32_t b,int32_t*out){
    if((b>0 && a>INT32_MAX-b) || (b<0 && a<INT32_MIN-b)) return 1;
    *out=(int32_t)(a+b);
    return 0;
}
static inline int a1_sub_overflow_i32(int32_t a,int32_t b,int32_t*out){
    if((b<0 && a>INT32_MAX+b) || (b>0 && a<INT32_MIN+b)) return 1;
    *out=(int32_t)(a-b);
    return 0;
}
#define A1_ADD_OVERFLOW(a,b,out) a1_add_overflow_i32((a),(b),(out))
#define A1_SUB_OVERFLOW(a,b,out) a1_sub_overflow_i32((a),(b),(out))
#endif

/* ===================================================================================== */
/* flash model: the image lives in "flash", accessed ONLY through these. On the host test  */
/* harness flash is a file/buffer; on the device these are the real flash page primitives. */
/* The decoder keeps 0 image bytes in RAM.                                                 */
/* ===================================================================================== */
extern uint8_t flash_read(uint32_t addr);
extern void    flash_write(uint32_t addr, uint8_t val);

/* Decoder state is owned by the integrator and passed to patch_apply_run(). The header keeps no
 * file-scope storage: embedded users may place this object in .bss, noinit, a bootloader-owned
 * work area, or on a suitably sized stack. */
typedef struct { uint32_t range, code; } A1RangeDec;

/* ---- UGolomb (uses embedded UG_CTX mirror) ----
 * Crash-hardening: cap the adaptive unary prefix. For GAMMA ('g') the prefix is the bit-length
 * of (value+1), so <=31 for any uint32 (RC_UNARY_MAX). For RICE ('r') the prefix is the QUOTIENT
 * value>>k, which for a legit field (e.g. backref_dist <= window 2^SA_W with no-split, W=11 =>
 * up to ~64 at small k) can far exceed 31 — cap it much higher (1<<20) so valid streams decode
 * while a corrupt run-on is still bounded (the mantissa shift below caps the magnitude anyway). */
#define RC_RICE_UNARY_MAX (1u<<20)
typedef struct { uint8_t k; uint16_t u[UG_CTX+1]; uint16_t m[UG_CTX+1][UG_CTX+1]; } A1UGRice;
typedef struct { uint16_t u[UG_CTX+1]; uint16_t m[UG_CTX+1][UG_CTX+1]; } A1UGGamma;

/* STREAMED-DELTA per-stream state (bl, ex): a MOVE-TO-FRONT dict of distinct delta values + an
 * adaptive "repeat-last" bit + an adaptive "dict-hit" bit. Each delta on the wire: rep-bit (==last?)
 * | else hit-bit (in dict? -> MTF index via the index UGolomb | else escape value as zigzag uLEB
 * via M_dval, MTF-inserted at front). The dict array is outside the struct so bl/ex get separate caps
 * (distinct-value peaks: bl 180, ex 106). */
typedef struct {
    uint16_t K;                       /* MTF dict entries in use (index 0 = most-recently-used) */
    uint16_t rep[4], hit; uint8_t rh; /* adaptive binary models (P(bit==0)); rep keyed by prev repeat + last==0 */
} A1DRStream;

/* JSLOTS is configured above. The ENCODER mirrors this budget
 * (A1_JSLOTS in patch_generate) and DEGRADES over-budget plans host-side
 * (over-budget reads ship as extra bytes), so a valid blob never exceeds it;
 * an over-cap stream still refuses cleanly here (REJ_RESOURCE). */
#define JREGION (JSLOTS*4u)                   /* journal byte region: JSLOTS uint32 slots (3072 B) */
/* LZSS window W (defined here so A1ApplyState can size the apply phase). Default value above
 * keeps the decoder within the 12 KiB SRAM cap; the encoder PATHE_W MUST match this SA_W. */
/* SA_W is configured above. */
#define SA_RING (1u<<SA_W)
#define SA_MASK (SA_RING-1u)
/* A1ApplyState = the whole apply-phase working set: the LZSS content-history ring (2^W) + the count-bounded
 * per-op correction array + the streaming scalars/cursors. */
typedef struct {
    uint8_t ring[SA_RING]; uint32_t ototal;       /* content history (masked) + total produced */
    uint32_t tok_left, tok_src;                   /* token replay count + ring/output source */
    int32_t tp, fp;                               /* running accumulators (apply order) */
    uint32_t op_corr[OPC_CAP]; int32_t op_nc;     /* high 24 bits = offset, low 8 = correction */
    uint8_t tok_mode;                             /* 0=idle, 1=span, 2=backref, 3=out-match */
    uint8_t last_span;                            /* previous token was a span (flag implicit) */
} A1ApplyState;
/* SA_ARENA: apply-phase reservation follows the actual state object. */
#define SA_ARENA ((uint32_t)sizeof(A1ApplyState))
#define ARENA_BYTES (JREGION + SA_ARENA)
/* One ARENA, two disjoint phase lifetimes overlaid as a union. */
typedef union {
    struct { uint32_t hist0[256], hist1[256], w[512]; } seed;
    struct { uint32_t jbuf[JSLOTS]; union { A1ApplyState sa; uint8_t rsv[SA_ARENA]; } sab; } apply;
} A1Arena;

typedef struct PatchApply {
    uint32_t g_image_span;
    uint32_t g_from_size, g_to_size, g_body_left;
    int32_t  g_fp_start;
    uint32_t g_want_to;
    int      g_FWD;

    int (*g_pull_fn)(void*, uint8_t*);
    void *g_pull_ctx;
    uint8_t g_pull_eof;

    A1RangeDec RC;
    uint8_t g_rcerr;
    uint8_t g_reject;
    uint8_t g_flash_touched;

    A1Arena ARENA;
    uint16_t g_jcount;

    uint32_t g_smap_b[SMAP_CAP];
    int32_t  g_smap_v[SMAP_CAP];
    uint16_t g_smap_n;

    uint8_t  g_orow_buf[OUTROW_DEPTH][OUTROW];
    uint32_t g_orow_base[OUTROW_DEPTH];
    uint8_t  g_orow_dirty[OUTROW_DEPTH];

    A1UGRice  M_gd;
    A1UGGamma M_gl, M_gs;
    A1UGRice  M_go;
    A1UGGamma M_glo;
    uint16_t M_outb;
    uint8_t  g_out_en;
    uint32_t g_oexp;
    A1UGGamma M_pg;
    A1UGGamma M_pgn;
    A1UGGamma M_pg2;
    A1UGGamma M_gdl, M_gel, M_gadj;
    A1IdxUnary M_dibl, M_diex;
    A1BitTree M_lit0[LIT0_CTX], M_lit1;
    uint8_t g_litprev;
    A1BitTree M_dval;
    A1Flag1   M_flag;
    uint16_t M_rep0[2];
    int g_rep0h;
    uint32_t g_lastdist;
    int32_t DR_DIC_BL[DR_KCAP_BL], DR_DIC_EX[DR_KCAP_EX];
    A1DRStream DR_BL, DR_EX;

    uint8_t g_psrc_imm[512], g_psrc_ldr[64];
} PatchApply;

#define g_image_span (pa->g_image_span)
#define g_from_size (pa->g_from_size)
#define g_to_size (pa->g_to_size)
#define g_fp_start (pa->g_fp_start)
#define g_want_to (pa->g_want_to)
#define g_FWD (pa->g_FWD)
#define g_pull_fn (pa->g_pull_fn)
#define g_pull_ctx (pa->g_pull_ctx)
#define g_body_left (pa->g_body_left)
#define g_pull_eof (pa->g_pull_eof)
#define RC (pa->RC)
#define g_rcerr (pa->g_rcerr)
#define g_reject (pa->g_reject)
#define g_flash_touched (pa->g_flash_touched)
#define ARENA (pa->ARENA)
#define Jbuf (ARENA.apply.jbuf)
#define SAst (ARENA.apply.sab.sa)
#define g_jcount (pa->g_jcount)
#define g_smap_b (pa->g_smap_b)
#define g_smap_v (pa->g_smap_v)
#define g_smap_n (pa->g_smap_n)
#define g_orow_buf (pa->g_orow_buf)
#define g_orow_base (pa->g_orow_base)
#define g_orow_dirty (pa->g_orow_dirty)
#define M_gd (pa->M_gd)
#define M_gl (pa->M_gl)
#define M_gs (pa->M_gs)
#define M_go (pa->M_go)
#define M_glo (pa->M_glo)
#define M_outb (pa->M_outb)
#define g_out_en (pa->g_out_en)
#define g_oexp (pa->g_oexp)
#define M_pg (pa->M_pg)
#define M_pgn (pa->M_pgn)
#define M_pg2 (pa->M_pg2)
#define M_gdl (pa->M_gdl)
#define M_gel (pa->M_gel)
#define M_gadj (pa->M_gadj)
#define M_dibl (pa->M_dibl)
#define M_diex (pa->M_diex)
#define M_lit0 (pa->M_lit0)
#define M_lit1 (pa->M_lit1)
#define g_litprev (pa->g_litprev)
#define M_dval (pa->M_dval)
#define M_flag (pa->M_flag)
#define M_rep0 (pa->M_rep0)
#define g_rep0h (pa->g_rep0h)
#define g_lastdist (pa->g_lastdist)
#define DR_DIC_BL (pa->DR_DIC_BL)
#define DR_DIC_EX (pa->DR_DIC_EX)
#define DR_BL (pa->DR_BL)
#define DR_EX (pa->DR_EX)
#define g_psrc_imm (pa->g_psrc_imm)
#define g_psrc_ldr (pa->g_psrc_ldr)

_Static_assert(sizeof(A1ApplyState) <= SA_ARENA, "A1ApplyState apply state exceeds its ARENA reservation");
_Static_assert(sizeof(A1Arena) == ARENA_BYTES, "arena union size drifted from ARENA_BYTES (.bss would change)");
_Static_assert(offsetof(A1Arena, apply.sab.sa) == JREGION && (offsetof(A1Arena, apply.sab.sa) & 3u)==0u,
               "A1ApplyState must sit at JREGION and be >=4-aligned (it holds uint32 fields)");
_Static_assert(A1_MAX_IMAGE <= 0x7fffffffu, "A1_MAX_IMAGE must stay < 2^31 (int32 tp/fp cursors, 32-bit overflow guards)");
_Static_assert(JSLOTS <= 65535u, "JSLOTS must fit the uint16 journal counters");
_Static_assert(DR_KCAP_BL <= 65535u && DR_KCAP_EX <= 65535u, "DR_KCAP_* must fit uint16 A1DRStream.K (else the REJ_RESOURCE refuse wraps)");

/* Patch geometry — decoder-OWNED. decode_body parses it from the blob envelope; the
 * integrator no longer supplies sizes/direction/span (footguns removed), it only provides
 * the two flash primitives above and pushes blob bytes. g_image_span = max(from,to) size
 * bounds every flash access. */
/* Feature 7B initial source seek: the source position where the ascending op walk BEGINS. Seeds
 * s->fp for FWD (was implicitly 0) and is the exact grow LANDING point (grow's final s->fp must
 * equal it). Carries a leading zero-output op's seek that was folded off the wire; almost always 0. */
/* A1_MAX_IMAGE is configured above. */

/* ===================================================================================== */
/* byte source: the integrator supplies a (possibly blocking) callback and calls           */
/* patch_apply_run() — the whole decode runs on the caller's stack, plain calls only.      */
/* CRC32(to) rides in the header (g_want_to), so the range-coded body is the LAST thing on  */
/* the wire — NO trailer-withhold ring. The header carries the compressed body length;       */
/* next_byte() zero-fills after exactly that many body bytes, so a valid blob finishes       */
/* without asking the byte source for transport EOF. EOF from the callback is only the       */
/* truncation/abort path.                                                                    */
/* ===================================================================================== */
static int decode_body(PatchApply *pa);  /* 1 = body decoded; 0 = error (g_reject/g_rcerr hold why) */
/* The literal-seed histograms are real uint32_t arrays in the ARENA union's seed member, so the
 * reads/writes are ordinary typed lvalues — no may_alias / char-array reinterpretation needed. */
static void lit_tree_from_hist(A1BitTree*t,const uint32_t*hist,uint32_t*w);
enum { PATCH_APPLY_DONE=1, PATCH_APPLY_ERROR=2 };

/* one raw blob byte straight from the callback; EOF is latched so a spent stream stays spent
 * (later reads keep returning 0 without re-invoking the callback). */
static int pull_raw(PatchApply *pa, uint8_t*out){
    if(g_pull_eof) return 0;
    if(!g_pull_fn(g_pull_ctx,out)){ g_pull_eof=1; return 0; }
    return 1;
}
static uint8_t next_byte(PatchApply *pa){
    uint8_t b;
    if(g_body_left==0) return 0;
    if(!pull_raw(pa,&b)){ g_body_left=0; return 0; }
    g_body_left--;
    return b;
}
static int env_byte(PatchApply *pa, uint8_t*out){ return pull_raw(pa,out); }             /* envelope: strict */

static int env_u32le(PatchApply *pa, uint32_t*out){
    uint32_t v=0; uint8_t b;
    for(int i=0;i<4;i++){ if(!env_byte(pa,&b)) return 0; v|=(uint32_t)b<<(8*i); }
    *out=v; return 1;
}
/* uLEB with a NON-CANONICAL-encoding flag: a canonical multi-byte uLEB never ends in a 0x00
 * byte (emission stops once the remaining value is zero; a lone 0x00 is the value 0), so one
 * redundant trailing continuation byte is a legal, value-neutral 1-bit side channel. The
 * envelope reads it on the size-delta field as the UNNATURAL-apply-direction marker. This is
 * the ONE uLEB reader; env_uleb below is the plain wrapper (discards the flag into a dummy). */
static int env_uleb_ov(PatchApply *pa, uint32_t*out, uint8_t*ov){
    uint32_t v=0; int sh=0, n=0; uint8_t b;
    do{ if(sh>28||!env_byte(pa,&b)) return 0; v|=(uint32_t)(b&0x7fu)<<sh; sh+=7; n++; }while(b&0x80u);
    *ov=(uint8_t)rc_uleb_overlong(n,b);
    *out=v; return 1;
}
static int env_uleb(PatchApply *pa, uint32_t*out){ uint8_t ov; return env_uleb_ov(pa,out, &ov); }
/* zigzag-uLEB absolute around `base` (to_size and fp_end are delta-coded vs from_size). */
static int env_zz_abs(PatchApply *pa, uint32_t base, uint32_t*out){
    uint32_t z; if(!env_uleb(pa,&z)) return 0;
    return rc_zz_abs(base,z,A1_MAX_IMAGE,out);
}

/* divide-free range decoder reading through the (possibly blocking) byte source. */
static void rc_init(PatchApply *pa){
    RC.range = 0xFFFFFFFFu; RC.code = 0;
    /* The encoder drops the always-zero LZMA leading cache byte from the wire, so the
     * 4 code bytes are read directly (no skip). */
    for (int i=0;i<4;i++) RC.code = (RC.code<<8) | next_byte(pa);
}
/* range-coder DECODE core shared by both the adaptive bit and the raw (equiprobable) bit: split at
 * `bound`, pick the sub-interval the code lands in, return the bit, then renorm (refill from the FIFO
 * until range climbs back above KTOP). The readers differ ONLY in `bound` + probability adaptation. */
static int rc_decode(PatchApply *pa, uint32_t bound){
    uint32_t code=RC.code, range=RC.range;
    int b;
    if (code<bound){ range=bound; b=0; } else { code-=bound; range-=bound; b=1; }
    while (range<RC_KTOP){ code=(code<<8)|next_byte(pa); range<<=8; }
    RC.code=code; RC.range=range;
    return b;
}
static int s_bit_r(PatchApply *pa, uint16_t*prob,int rate){
    uint32_t p=*prob;
    int b=rc_decode(pa,(RC.range>>12)*p);
    *prob=rc_adapt(p,b,rate);
    return b;
}
/* RC_S_BIT_RATE: shared Golomb / order-2 flag / MTF rep+hit adaptation rate.
 * literal/dval bit-trees keep their own per-tree rate via s_bit_r. */
static int s_bit(PatchApply *pa, uint16_t*prob){ return s_bit_r(pa,prob,RC_S_BIT_RATE); }
static int s_raw(PatchApply *pa){ return rc_decode(pa,RC.range>>1); }
/* CRASH-HARDENING (fuzz gate): a corrupt/truncated stream yields zero-fill past EOF,
 * which can drive the unbounded unary loops below forever (hang) or shift a value by >=32 bits
 * (UB). Every unbounded loop is capped to the max a 32-bit value needs; on overflow set g_rcerr
 * and bail. g_rcerr is the ONE decode error latch — RC-level and apply-level failures all set it
 * (clean reject, never crash / never silent-wrong); g_reject still carries the reason. */
/* rob-1: distinguishable reject reason (read by the host main after a reject). 1 =
 * RESOURCE: a corpus-overfit cap was exceeded (journal full / per-op cap / dict cap) — this firmware
 * is bigger than the build was sized for, raise the cap (costs SRAM). 2 = CORRUPT: malformed/truncated
 * stream (bounds, underrun, range-coder overflow). 0 = none. Pure diagnostic; never affects decoding. */
enum { REJ_NONE=0, REJ_RESOURCE=1, REJ_CORRUPT=2 };
/* terminal-state flag: set at the single flash_write site (orow_commit_slot's dirty branch);
 * on ERROR it tells the integrator whether the old image is still intact. */
#define RC_UNARY_MAX 31           /* a uint32 value needs <=31 leading/unary bits */
static uint32_t s_raw_bits(PatchApply *pa, int nb){ uint32_t v=0; for(int i=0;i<nb;i++) v=(v<<1)|(uint32_t)s_raw(pa); return v; }
/* ---- bit-tree byte ---- */
static int s_bt(PatchApply *pa, A1BitTree*t,int rate){
    int m=1;
    for(int i=0;i<8;i++){
        uint16_t p=a1_bt_get(t,m-1);
        int b=s_bit_r(pa,&p,rate);
        a1_bt_set(t,m-1,p);
        m=(m<<1)|b;
    }
    return m-256;
}
static int32_t bb_unzz(PatchApply *pa, uint32_t u){
    if(!rc_unzz32_valid(u)){ g_rcerr=1; return 0; }
    return rc_unzz32_value(u);
}
/* ---- MTF escape value: zigzag uLEB, each byte through the adaptive M_dval bit-tree ---- */
static int32_t s_bv(PatchApply *pa, A1BitTree*t,int rate){
    uint32_t acc=0; int sh=0, b;
    do{
        if(sh>28){ g_rcerr=1; return 0; }            /* cap shift (uLEB <=32-bit) */
        b=s_bt(pa,t,rate);
        acc|=(uint32_t)(b&0x7f)<<sh; sh+=7;
    }while(b&0x80);
    return bb_unzz(pa,acc);
}
/* ---- UGolomb (uses embedded UG_CTX mirror) ----
 * Crash-hardening: cap the adaptive unary prefix. For GAMMA ('g') the prefix is the bit-length
 * of (value+1), so <=31 for any uint32 (RC_UNARY_MAX). For RICE ('r') the prefix is the QUOTIENT
 * value>>k, which for a legit field (e.g. backref_dist <= window 2^SA_W with no-split, W=11 =>
 * up to ~64 at small k) can far exceed 31 — cap it much higher (1<<20) so valid streams decode
 * while a corrupt run-on is still bounded (the mantissa shift below caps the magnitude anyway). */
static void ugr_init(A1UGRice*g,int k){
    g->k=(uint8_t)k;
    for(int i=0;i<=UG_CTX;i++){ g->u[i]=RC_PHALF; for(int j=0;j<=UG_CTX;j++) g->m[i][j]=RC_PHALF; }
}
/* init a gamma model, seeding EVERY unary-prefix prior to `useed` (the mantissa priors are always
 * RC_PHALF). useed==RC_PHALF is the neutral init; useed==RC_PBIT-RC_PBIT/4 biases toward STOP (bit 0)
 * so the smallest gamma values are cheap from the first symbol — used by the MTF dict-index gammas
 * (dibl/diex), where the just-promoted index 1 (encoded value 0) dominates (RC_PBIT-RC_PBIT/4 is the
 * corpus optimum). Folding the u[] seed into the init avoids a second pass over u[]. */
static void ugg_init_u(A1UGGamma*g,uint16_t useed){
    for(int i=0;i<=UG_CTX;i++){ g->u[i]=useed; for(int j=0;j<=UG_CTX;j++) g->m[i][j]=RC_PHALF; }
}
static void ugg_init(A1UGGamma*g){ ugg_init_u(g,RC_PHALF); }
/* seed the unary-prefix priors of a gamma model toward "continue" for the first `depth` context
 * positions. Used by per-op geometry (diff_len/adj): firmware delta op magnitudes are essentially
 * never tiny, so cl>=depth almost always — this is a structural prior, NOT a corpus cap, and it
 * makes the very first op (e.g. a one-face update with few ops) as cheap as the warmed-up state. */
static void ugg_seed_cont(A1UGGamma*g,int depth){
    rc_seed_cont_u(g->u,UG_CTX,depth);  /* low p == bit1(continue) cheap */
}
/* shared adaptive unary prefix for BOTH the Golomb (Rice/Gamma) prefix and the MTF dict-index code:
 * read 1-bits on the per-position priors u[min(pos,clampmax)] until a 0-bit, `cap`-bounded against a
 * corrupt run-on so a zero-fill stream can't spin forever / shift by >=32 (RC_UNARY_MAX for gamma,
 * RC_RICE_UNARY_MAX for rice); on overflow it sets g_rcerr and returns 0 so the mantissa loop is a
 * no-op and the apply cleanly rejects. clampmax==UG_CTX(=6) reproduces UG_C over the 7-entry Golomb
 * u[]; clampmax==IDX_CTX-1 (=4) reproduces the index code's min(pos,4) over the 5-entry u[]. */
static uint32_t s_unary(PatchApply *pa, uint16_t*u,uint32_t clampmax,uint32_t cap){
    uint32_t cl=0; while(s_bit(pa,&u[cl<clampmax?cl:clampmax])==1){ if(++cl>cap){ g_rcerr=1; return 0; } }
    return cl;
}
/* shared mantissa: read `cnt` adaptive bits MSB-first (significance-descending) from priors row[],
 * the m[UG_C(cl)] row already selected by caller. lsb!=0 (rice) anchors the ctx on significance from
 * the LSB (UG_C(sig)) so the LSB gets ctx 0 and the wide MSBs share the clamp; lsb==0 (gamma) keeps
 * MSB-anchored UG_C(cnt-1-sig)==UG_C(pos). Bit order on the wire stays MSB-first either way. */
static uint32_t s_ug_mant(PatchApply *pa, uint16_t*row,int cnt,int lsb){
    uint32_t v=0; for(int sig=cnt-1;sig>=0;sig--) v|=(uint32_t)s_bit(pa,&row[UG_C(lsb?sig:cnt-1-sig)])<<sig;
    return v;
}
static uint32_t s_ug_rice(PatchApply *pa, A1UGRice*g){
    uint32_t cl=s_unary(pa,g->u,UG_CTX,RC_RICE_UNARY_MAX);
    return (cl<<g->k) | s_ug_mant(pa,g->m[UG_C((int)cl)],g->k,1);
}
static uint32_t s_ug_gamma(PatchApply *pa, A1UGGamma*g){
    int cl=(int)s_unary(pa,g->u,UG_CTX,RC_UNARY_MAX);
    return ((1u<<cl) | s_ug_mant(pa,g->m[UG_C(cl)],cl,0)) - 1u;
}
/* MTF dict-index model: a lean adaptive UNARY code. IDX_CTX / A1IdxUnary / a1_idx_init are single-sourced
 * in the encoder mirror. The encoded index value v (== dict pos j-1) is ~54% zero, ~22% one,
 * ~10% two, with a thin tail to ~140 worst case: emit v continue-bits then a stop-bit on the per-position
 * prior u[min(pos,IDX_CTX-1)]. `cap` bounds the run on a corrupt stream (pull_delta validates j vs K). */
/* ---- order-2 flag ---- */
static int s_flag(PatchApply *pa, A1Flag1*f){ int b=s_bit(pa,&f->m[f->h]); f->h=((f->h<<1)|b)&3; return b; }

/* ===================================================================================== */
/* CRC32 (zlib polynomial) over flash bytes                                               */
/* ===================================================================================== */
static uint32_t crc32_flash(uint32_t n){
    uint32_t c=0xffffffffu;
    for(uint32_t i=0;i<n;i++){ c^=flash_read(i);
        for(int k=0;k<8;k++) c=(c>>1)^(0xedb88320u & (uint32_t)(-(int32_t)(c&1))); }
    return c^0xffffffffu;
}

/* ===================================================================================== */
/* never-evict journal — FLAT SORTED uint32 slots, each packed (pos<<8)|byte (the op_corr      */
/* packing). Sorted by pos, so lookup is one binary search on slot>>8. 24-bit pos spans 16 MiB */
/* — any realistic image. NEVER-EVICT: the first write to a pos wins; over-depth (would exceed */
/* JSLOTS) is REFUSED before any write. Lives in the apply phase ONLY (overlaid in ARENA       */
/* front).                                                                                     */
/* ===================================================================================== */
/* DEREL dict cap (corpus distinct-value peak ~179 per substream + margin). The STREAMED-DELTA wire
 * (12 KiB build) holds NO resident per-detection store — only a small MOVE-TO-FRONT dict of the
 * distinct delta values (the frequently-repeated relocation offsets keep tiny MTF indices). */
/* Caps are corpus-peak + margin; over-cap input is REJECTED (CRC-gated, never silent-wrong), not
 * applied. All -D overridable (#ifndef) so a deployment with a known firmware family can re-tune
 * them; raising a cap costs SRAM and must be followed by the release gate. */
/* DR_KCAP_* and OPC_CAP are configured above. */
/* Suppressed BL positions are implicit: a BL-looking field with any literal patch byte (`!pure`) is
 * a normal 4-byte copy. No sbl offset stream or resident gap buffer is needed. */
static void jr_reset(PatchApply *pa){ g_jcount=0; }
/* Binary search on slot>>8 over [0, g_jcount). On a hit *at is the matching slot and the return
 * is 1; on a miss *at is the sorted insertion index and the return is 0. */
static int jr_find(PatchApply *pa, uint32_t pos, int*at){
    int lo=0, hi=(int)g_jcount-1;
    while(lo<=hi){ int mid=(int)(((unsigned)lo+(unsigned)hi)>>1);
        uint32_t k=Jbuf[mid]>>8;
        if(k==pos){ *at=mid; return 1; } if(k<pos) lo=mid+1; else hi=mid-1; }
    *at=lo;
    return 0;
}
static int jr_put(PatchApply *pa, uint32_t pos, uint8_t b){
    if(pos>=(1u<<24)) return -1;                     /* slot packs pos in 24 bits (16 MiB span) */
    int at;
    if(jr_find(pa,pos,&at)) return 0;                /* never-evict: keep the first write */
    if(g_jcount>=JSLOTS) return -1;                  /* over-depth: refuse BEFORE any write */
    if(at<(int)g_jcount) memmove(Jbuf+at+1, Jbuf+at, (size_t)((int)g_jcount-at)*4u);
    Jbuf[at]=(pos<<8)|b;
    g_jcount++;
    return 0;
}
static int jr_get(PatchApply *pa, uint32_t pos, uint8_t*out){
    int at;
    if(jr_find(pa,pos,&at)){ *out=(uint8_t)Jbuf[at]; return 1; }
    return 0;
}

/* ===================================================================================== */
/* relocation unpack/pack (Thumb bl + s32) — de-relocation override only (no flash write).      */
/* The journal-aware pristine reads + the bl/ldr predicates live with the apply state below.    */
/* ===================================================================================== */
/* de-relocate a Thumb BL halfword pair (imm24 = imm - delta), repacked into 4 bytes (no flash write).
 * The subtract is mod 2^24 (masked & repacked immediately) so no sign-extension is needed.
 * rc_bl_imm24 / rc_bl_pack are the encoder-mirrored unpack/pack primitives. */
static void bl_dereloc(uint16_t up, uint16_t lo, uint32_t delta, uint8_t out[4]){
    rc_bl_pack((rc_bl_imm24(up,lo) - delta) & 0x00ffffffu, out);
}

/* ---- piecewise shift map (D1): predicts BL/EX de-reloc deltas from addresses/values the
 * decoder already has in hand; only the RESIDUAL (delta - pred) rides the MTF dict streams.
 * smap_at(x) = value of the last segment with boundary <= x, else 0 (also 0 when no map).
 * Values are BYTE shifts; BL predictions divide by 2 (imm24 is in halfwords) with C
 * truncation-toward-zero on both sides (bit-exact vs patch_generate). ---- */
static int32_t smap_at(PatchApply *pa, uint32_t x){ return rc_smap_at(g_smap_b, g_smap_v, (int)g_smap_n, x); }

/* ===================================================================================== */
/* HYBRID: monotonic output ROW write-back cache (real flash = row-erase 256 B).            */
/* Output writes are monotonic & contiguous (asc shrink / desc grow); SOURCE reads stay raw  */
/* (the never-evict journal covers read-after-overwrite). One resident output row buffer: a   */
/* write to a NEW row first commits the previous row (one erase+program == 1 write/row, so    */
/* nvm_rows_amplified=0); reads of the dirty buffered row are served from RAM. Source reads of */
/* not-yet-overwritten flash go straight to physical flash (free). 256 B SRAM.                */
/* ===================================================================================== */
/* OUTROW is configured above (encoder mirror: A1_OUTROW). */
/* Row-window depth — keep the last OUTROW_DEPTH rows uncommitted. Monotonic writes touch
 * rows in strictly monotonic order, so a DIRECT-MAPPED ring keyed by (row_number % D) is an
 * exact FIFO: the slot's previous occupant is always the row exactly D rows behind,
 * committed on eviction. The point: OLD flash content of uncommitted rows stays physically
 * readable through hy_src_peek's plain flash_read, and the ENCODER's a1_row_covered oracle
 * exploits exactly that window (journal-free old reads behind the frontier).
 * ENCODING-AFFECTING BUILD CONTRACT: OUTROW x OUTROW_DEPTH must match the encoder's
 * A1_OUTROW x A1_ROW_DEPTH assumption. Compatibility is monotone: a decoder whose
 * uncommitted window is a superset (larger aligned rows and/or a deeper ring) still decodes
 * correctly — old bytes survive strictly longer than the encoder assumed; a smaller window
 * rejects via CRC32(to), never silent-wrong. */
/* OUTROW_DEPTH is configured above (encoder mirror: A1_ROW_DEPTH). */
#define OROW_NONE UINT32_MAX
#define OROW_SLOT(base) (uint32_t)(((base)/OUTROW) % OUTROW_DEPTH)
/* OROW_SLOT / out_read / out_write use /OUTROW and %OUTROW_DEPTH; both must stay powers of two so
 * they lower to shift/mask — a non-pow2 -D retune would pull libgcc divide helpers into the
 * Cortex-M0+ build (the gate tracks divide policy) and break the encoder's pow2-aligned-row contract. */
_Static_assert(OUTROW>0u && (OUTROW & (OUTROW-1u))==0u, "OUTROW must be a power of two");
_Static_assert(OUTROW_DEPTH>0u && (OUTROW_DEPTH & (OUTROW_DEPTH-1u))==0u, "OUTROW_DEPTH must be a power of two");
static void orow_commit_slot(PatchApply *pa, uint32_t s){
    if(g_orow_base[s]!=OROW_NONE && g_orow_dirty[s]){
        uint32_t base=g_orow_base[s], end=base+OUTROW; if(end>g_image_span) end=g_image_span;
        g_flash_touched=1;
        /* force the (at most one) row erase up front, then program from the RAM buffer so every
         * byte is a pure 1->0 program (no further erase). All bytes of the row were produced by the
         * monotonic output, so the buffer holds the exact final content. */
        for(uint32_t a=base;a<end;a++){ if(flash_read(a)!=0xFFu){ flash_write(a,0xFFu); break; } }
        for(uint32_t a=base;a<end;a++) flash_write(a, g_orow_buf[s][a-base]);
    }
    g_orow_dirty[s]=0; g_orow_base[s]=OROW_NONE;
}
/* final flush: commit remaining slots in WRITE order (frontier monotonicity holds on NVM). */
static void orow_commit_all(PatchApply *pa){
    for(uint32_t i=0;i<OUTROW_DEPTH;i++){
        uint32_t best=OROW_NONE, bs=0;
        for(uint32_t s=0;s<OUTROW_DEPTH;s++){
            uint32_t b=g_orow_base[s];
            if(b==OROW_NONE) continue;
            if(best==OROW_NONE || (g_FWD ? b<best : b>best)){ best=b; bs=s; }
        }
        if(best==OROW_NONE) break;
        orow_commit_slot(pa,bs);
    }
}
static void orow_reset(PatchApply *pa){ for(uint32_t s=0;s<OUTROW_DEPTH;s++){ g_orow_base[s]=OROW_NONE; g_orow_dirty[s]=0; } }
/* read one byte of ALREADY-PRODUCED output at absolute position a: an uncommitted row from
 * its RAM slot, anything else from flash. Valid streams only reference written positions; a
 * corrupt position yields stale flash bytes, which the CRC32(to) gate rejects. */
static uint8_t out_read(PatchApply *pa, uint32_t a){
    if(a>=g_image_span) return 0;
    { uint32_t base=(a/OUTROW)*OUTROW, s=OROW_SLOT(base);
      if(g_orow_base[s]==base) return g_orow_buf[s][a-base]; }
    return flash_read(a);
}
/* OUTPUT write: buffer in the row's slot, committing the slot's previous occupant (the row
 * exactly OUTROW_DEPTH rows behind) on a row change. */
static void out_write(PatchApply *pa, uint32_t a, uint8_t v){
    if(a>=g_image_span) return;
    uint32_t base=(a/OUTROW)*OUTROW, s=OROW_SLOT(base);
    if(base!=g_orow_base[s]){
        orow_commit_slot(pa,s);
        { uint32_t end=base+OUTROW;
          if(end>g_image_span) end=g_image_span;
          for(uint32_t x=base;x<end;x++) g_orow_buf[s][x-base]=flash_read(x); } /* preload (source not yet erased) */
        g_orow_base[s]=base;   /* orow_commit_slot() above already cleared the dirty flag */
    }
    { uint32_t off=a-base;
      if(g_orow_buf[s][off]!=v){ g_orow_buf[s][off]=v; g_orow_dirty[s]=1; } }
}
/* ===================================================================================== */
/* HYBRID STREAMED DELTAS (12 KiB build). Each field's                                              */
/* delta VALUE is pulled INLINE from the single range stream the instant the apply detects the      */
/* field (the field TYPE is known from detection, so no untyped up-front decode is needed). Per      */
/* stream we keep only a small MOVE-TO-FRONT dict of the distinct delta values (the frequently-      */
/* repeated relocation offsets keep tiny MTF indices) + a repeat-last bit keyed by previous repeat   */
/* and last-is-zero + a dict-hit bit.                                                                */
/* ===================================================================================== */
/* A1 derives bl/ldr positions on-device, so there is no on-device disassembler
 * and no ranges side table. */
/* pack s32 (ldr/data/code de-reloc) into 4 little-endian bytes (no flash write) */
static void pack_s32_buf(uint32_t v, uint8_t out[4]){ out[0]=(uint8_t)v; out[1]=(uint8_t)(v>>8); out[2]=(uint8_t)(v>>16); out[3]=(uint8_t)(v>>24); }

static void dr_init(A1DRStream*d, int32_t*dic, uint16_t hitseed){
    d->K=1; dic[0]=0;
    for(int i=0;i<4;i++) d->rep[i]=RC_PHALF;
    d->rh=0; d->hit=hitseed;
}
/* pull the next delta of a stream (bl/ex), inline:
 *   rep-bit: ==1 -> return last; else read hit-bit:
 *     hit==1 -> MTF index via the index UGolomb (gix); v=dic[j]; move dic[j] to front.
 *     hit==0 -> escape value (zigzag uLEB via M_dval); insert v at MTF front.
 *   then last=v. */
static int32_t pull_delta(PatchApply *pa, A1DRStream*d, A1IdxUnary*gix, int32_t*dic, uint32_t cap){
    { int ri=d->rh | (dic[0]==0 ? 2 : 0);
      int rb=s_bit(pa,&d->rep[ri]); d->rh=(uint8_t)rb; if(rb==1) return dic[0]; }  /* repeat-last, order-1 + zero ctx */
    int32_t v;
    if(s_bit(pa,&d->hit)==1){
        uint32_t j=s_unary(pa,gix->u,IDX_CTX-1,cap)+1u;    /* cmp-1: dict idx 0 unreachable -> encode j-1 */
        if(j>=(uint32_t)d->K){ g_rcerr=1; return 0; }
        v=dic[j];
        rc_mtf_promote_i32(dic,j);
    } else {
        v=s_bv(pa,&M_dval, RC_DVAL_RATE);
        if((uint32_t)d->K>=cap){ g_rcerr=1; g_reject=REJ_RESOURCE; return 0; }   /* distinct-value cap -> reject */
        rc_mtf_insert_i32(dic,&d->K,v);
    }
    return v;
}


/* ===================================================================================== */
/* streaming [A] apply: NO split, NO per-op literal/extra buffer.                             */
/* Per op the decoder reads DIRECT geometry+P+C (outside LZSS), journals preserves EAGERLY,    */
/* then PULLS the op's CONTENT bytes from the cut whole-stream LZSS token stream and writes     */
/* each output byte immediately (ascending FWD / descending grow). Literal patches are consumed */
/* via an O(1) next-position cursor; corrections via an O(1) sorted-array cursor (count-bounded).*/
/* The LZSS ring (size 2^SA_W) is the content history; backref distances <= 2^SA_W.             */
/* ===================================================================================== */
/* A1ApplyState type + SA_RING/SA_MASK + the SAst accessor and the arena asserts live up with the ARENA
 * union (near JREGION), since the union embeds A1ApplyState and reserves SA_ARENA bytes for it. */
static int op_next_offset(PatchApply *pa, uint32_t *off, uint32_t idx, uint32_t nwu){
    uint32_t gap=s_ug_gamma(pa,idx?&M_pg2:&M_pg);
    if((idx && gap==0u) || gap>UINT32_MAX-*off){ g_rcerr=1; return 0; }
    *off+=gap;
    if(*off>=nwu){ g_rcerr=1; return 0; }
    return 1;
}

typedef struct { int32_t i, step; } A1CorrCur;
static void corr_cur_init(A1CorrCur*c, A1ApplyState*s, int fwd){
    c->step=fwd?1:-1; c->i=fwd?0:s->op_nc-1;
}
static int32_t corr_take(A1ApplyState*s, A1CorrCur*c, int32_t off){
    if(c->i>=0 && c->i<s->op_nc){
        uint32_t slot=s->op_corr[c->i];
        if((int32_t)(slot>>8)==off){ c->i+=c->step; return (int32_t)(slot&0xffu); }
    }
    return 0;
}
/* A1 ldr-derive: for FWD, a 1024-byte pristine window is represented as 512 even-halfword slots:
 * imm8 byte + one "is Thumb LDR literal" bit. The ldr back-scan never needs the full instruction
 * halfword after classification. Field probes PEEK at the current 4-byte field before recording it.
 * Grow reads via the journal-aware source path because lower instruction-window bytes may be
 * clobbered. */
#define PSRC_HW_MASK 511u
static uint8_t hy_src(PatchApply *pa, int32_t fp);   /* fwd decl: journal-aware pristine source read */
/* A1 ldr-derive (SAME-OP): is the 4-aligned from-addr fpk an ldr literal target of an instruction
 * IN THIS op [fp0,fp0+dl)? Scan even a in [max(fp0,fpk-1024), fpk-2]; an ldr literal
 * `(up&0xf800)==0x4800` targets t=(a&~3)+4*(up&0xff)+4; field iff some a targets fpk and
 * fpk+4<=fp0+dl. Reads PRISTINE source: the FWD and grow apply directions share ONE back-scan and
 * differ ONLY in the halfword source — FWD reads the packed LDR metadata recorded at read-time, grow
 * reads via hy_src() (journal-aware: instr-window bytes the output frontier already clobbered are
 * preserved copy source, so raw flash would be wrong). s==NULL selects the FWD metadata path. Kept
 * static-inline so the per-direction branch folds away at each call site (no indirect call / no text
 * bloat on Cortex-M0+). No resident target store. */
static inline int ldr_targets(PatchApply *pa, A1ApplyState*s, int32_t fp0, int32_t dl, uint32_t fpk){
    /* the scan only ever yields 4-aligned targets (t=(a&~3)+4n+4), so a non-aligned fpk can never
     * match -> reject it up front (also keeps the caller's EX gate a single ldr_targets() test, and
     * skips any pristine read/record for an impossible target). */
    if(fpk&3u) return 0;
    int32_t hi=fp0+dl; if((int32_t)fpk+4>hi) return 0;
    int32_t lo=(int32_t)fpk-1024; if(lo<fp0) lo=fp0; if(lo&1) lo++;
    for(int32_t a=lo; a+2<=(int32_t)fpk; a+=2){
        int32_t imm;
        if(s){
            uint16_t up=(uint16_t)(hy_src(pa,a) | (hy_src(pa,a+1)<<8));
            if((up&0xf800)!=0x4800) continue;
            imm=(int32_t)(up&0xff);
        } else {
            uint32_t i=((uint32_t)a>>1)&PSRC_HW_MASK;
            if(!(g_psrc_ldr[i>>3]&(uint8_t)(1u<<(i&7u)))) continue;
            imm=(int32_t)g_psrc_imm[i];
        }
        if(rc_ldr_target(a,imm)==(int32_t)fpk) return 1;
    }
    return 0;
}
/* g_litprev = the actual previous CONTENT-stream byte (order-1 tag0 literal context): updated for
 * EVERY emitted content byte here -- span literal, ring backref, out-match copy -- so the literal at
 * the next span selects M_lit0[LIT0_SEL(content[pos-1])] regardless of which token produced pos-1.
 * BL/EX field bytes are de-reloc'd outside the content stream and correctly do not update it. */
static void sa_emit_ring(PatchApply *pa, A1ApplyState*s, uint8_t b){ s->ring[s->ototal & SA_MASK]=b; s->ototal++; g_litprev=b; }
/* pull the next CONTENT byte from the cut LZSS token stream, decoding tokens lazily. */
static uint8_t sa_next_content(PatchApply *pa, A1ApplyState*s, int tag){
    for(;;){
        if(g_rcerr) goto fail;
        if(s->tok_left>0){
            uint8_t b;
            if(s->tok_mode==1) b=(uint8_t)s_bt(pa,tag?&M_lit1:&M_lit0[LIT0_SEL(g_litprev)], tag?RC_LIT1_RATE:RC_LIT0_RATE);
            else if(s->tok_mode==2){ b=s->ring[s->tok_src & SA_MASK]; s->tok_src++; }
            else { b=out_read(pa,s->tok_src); s->tok_src+=(uint32_t)(g_FWD?1:-1); }
            sa_emit_ring(pa,s,b); s->tok_left--;
            if(s->tok_left==0) s->tok_mode=0;
            return b;
        }
        /* adjacent spans never ship (the encoder merges them), so after a span the
         * span/match flag is IMPLICITLY "match": skip the coded bit but keep the order-2
         * flag history tracking the true token kinds (mirror emit_body last_span). */
        int is_match;
        if(s->last_span){ M_flag.h=((M_flag.h<<1)|1)&3; is_match=1; }
        else is_match=s_flag(pa,&M_flag);
        if(!is_match){
            uint32_t ln=s_ug_gamma(pa,&M_gs)+1u;
            s->tok_mode=1; s->tok_left=ln; s->last_span=1;
        } else {
            uint32_t d;
            int rb=s_bit(pa,&M_rep0[g_rep0h]); g_rep0h=rb;
            if(rb){ d=g_lastdist; }                         /* rep0: reuse last distance */
            else if(g_out_en && s_bit(pa,&M_outb)){         /* fresh + out-bit: long-range OUTPUT match */
                int32_t dpos=bb_unzz(pa,s_ug_rice(pa,&M_go)); /* position as zigzag delta vs expected */
                uint32_t p=rc_outmatch_pos(g_oexp,dpos);
                uint32_t ln=s_ug_gamma(pa,&M_glo)+RC_OUTMATCH_MIN;
                /* replay walks the output in WRITE direction (ascending FWD, descending grow) so
                 * grow content (whose extras are byte-reversed) still matches produced output. */
                if(g_rcerr || p>=g_image_span || (g_FWD ? ln>g_image_span-p : ln>p+1u)) goto fail;
                g_oexp=rc_outmatch_next_expect(g_FWD,p,ln);  /* sequential runs keep deltas tiny */
                s->tok_mode=3; s->tok_src=p; s->tok_left=ln; s->last_span=0;
                continue;
            }
            else { d=s_ug_rice(pa,&M_gd)+1u; g_lastdist=d; }
            uint32_t ln=s_ug_gamma(pa,&M_gl)+1u;
            if(d==0 || d>s->ototal || d-1u>=SA_RING) goto fail; /* reject before-start / ring-overrun */
            s->tok_mode=2; s->tok_src=s->ototal-d; s->tok_left=ln; s->last_span=0;
        }
    }
fail:
    g_rcerr=1;
    return 0;
}
/* read a uLEB from the content stream (one byte pulled per iteration from the LZSS token replay). */
static uint32_t sa_read_uleb(PatchApply *pa, A1ApplyState*s){
    uint32_t acc=0; int sh=0;
    for(;;){ if(g_rcerr) return 0;
        uint8_t b=sa_next_content(pa,s,0);
        if(sh>28){ g_rcerr=1; return 0; }            /* cap shift (uLEB <=32-bit) */
        acc|=(uint32_t)(b&0x7f)<<sh; sh+=7;
        if(!(b&0x80)) return acc; }
}
/* journal one preserve site (old flash byte captured BEFORE any write of this op). */
static void sa_journal(PatchApply *pa, int32_t tp){
    if(tp>=0 && tp<(int32_t)g_from_size){ if(jr_put(pa,(uint32_t)tp, flash_read((uint32_t)tp))){ g_rcerr=1; g_reject=REJ_RESOURCE; }
    }
}
static uint8_t hy_src_peek(PatchApply *pa, int32_t fp){
    if(fp>=0 && (uint32_t)fp<g_from_size){ uint8_t jb; return jr_get(pa,(uint32_t)fp,&jb)?jb:flash_read((uint32_t)fp); }
    return 0;
}
static void hy_half_rec(PatchApply *pa, uint32_t a, uint8_t lo, uint8_t hi){
    uint32_t i=(a>>1)&PSRC_HW_MASK;
    uint16_t up=(uint16_t)(lo | ((uint16_t)hi<<8));
    g_psrc_imm[i]=lo;
    if((up&0xf800)==0x4800) g_psrc_ldr[i>>3]|=(uint8_t)(1u<<(i&7u));
    else g_psrc_ldr[i>>3]&=(uint8_t)~(1u<<(i&7u));
}
/* record one pristine source byte at fp into the packed FWD ldr metadata.
 * Out-of-range fp is a no-op (matches hy_src_peek's range gate). */
static void hy_src_rec(PatchApply *pa, int32_t fp, uint8_t v){
    if(fp>=0 && (uint32_t)fp<g_from_size){
        uint32_t a=(uint32_t)fp, i=(a>>1)&PSRC_HW_MASK;
        if(a&1u) hy_half_rec(pa,a-1u, g_psrc_imm[i], v);
        /* even fp: the low byte of the halfword. hy_half_rec(a,v,0) stores imm=v and, since the high
         * byte 0 makes up=v<256 never match the 0xf800==0x4800 ldr pattern, clears the ldr bit —
         * identical to the inlined store+clear, and lets the two record paths share one body. */
        else hy_half_rec(pa,a, v, 0);
    }
}
/* journal-aware RAW source byte at fp (no bake). Returns the PRISTINE from-byte: the journal
 * preserves the original byte where the output frontier later overwrote source flash. Records every
 * pristine read into the packed FWD ldr metadata. */
static uint8_t hy_src(PatchApply *pa, int32_t fp){
    uint8_t v=hy_src_peek(pa,fp);
    hy_src_rec(pa,fp,v);
    return v;
}
/* journal-aware pristine 4-byte field word (little-endian) at fpk, peeked WITHOUT touching the psrc
 * ring (suppressed-bl / no-field must record nothing). On a real BL/EX hit the caller replays this
 * word into the ring via hy_word4_rec (ascending byte order). */
static uint32_t hy_word4_peek(PatchApply *pa, uint32_t fpk){
    return  (uint32_t)hy_src_peek(pa,(int32_t)fpk)    | ((uint32_t)hy_src_peek(pa,(int32_t)fpk+1)<<8)
          | ((uint32_t)hy_src_peek(pa,(int32_t)fpk+2)<<16) | ((uint32_t)hy_src_peek(pa,(int32_t)fpk+3)<<24);
}
/* record a known-in-range 4-byte field window [fpk,fpk+4) into the ring (ascending, LE). The caller's
 * entry guard pins fpk>=0 && fpk+4<=from_size, so every byte is in range and the per-byte hy_src_rec
 * gate would always pass — record straight into the 4 ring slots (the window cannot self-alias). */
static void hy_word4_rec(PatchApply *pa, uint32_t fpk, uint32_t w){
    hy_half_rec(pa,fpk,     (uint8_t)w,       (uint8_t)(w>>8));
    hy_half_rec(pa,fpk+2u,  (uint8_t)(w>>16), (uint8_t)(w>>24));
}
/* de-reloc field at op-local field-start ks. Returns: 0=no field; 1=bl/ex (packed4 filled,
 * a value consumed from the generator); 2=suppressed-bl (write the 4 bytes as NORMAL copies).
 *
 * The field window [fpk,fpk+4) is fully in-range (the entry guard pins fpk+4<=from_size). The 4
 * pristine field bytes are PEEKED ONCE into w up front; both the BL pattern test and the BL/EX pack
 * reuse w (no re-read), and the ring record on a real BL/EX hit replays w directly (hy_word4_rec)
 * rather than reading the source again. Field kinds are mutually exclusive and tried in encoder
 * order: local-BL first (Thumb F000/D000 pattern, 2-aligned), else a same-op LDR-literal target.
 * Both consume a stream delta ONLY on a real BL/EX hit (suppressed-BL and "no field" never touch the
 * stream, and record nothing into the ring). */
static int field_at(PatchApply *pa, A1ApplyState*s, int32_t fp0, int32_t ks, uint8_t packed[4], int pure, int32_t dl){
    /* fpk = fp0 + ks, range-checked in pure 32-bit. ks >= 0 (op-local field offset) and image sizes are
     * < 2^31 (hard design invariant), so fpk < 0 iff fp0 < -ks (no overflow: -ks is a valid int32 for
     * ks >= 0), and once fpk >= 0 the unsigned sum fp0+ks cannot wrap (0 <= fp0+ks < 2^32). */
    if(fp0 < -ks) return 0;                                /* fpk would be negative */
    uint32_t fpk=(uint32_t)fp0+(uint32_t)ks;
    if(fpk>g_from_size || g_from_size-fpk<4u) return 0;    /* fpk+4 > from_size (no +4 overflow) */
    if(fpk&1u) return 0;                                  /* BL is 2-aligned; LDR targets are 4-aligned */
    /* ONE pristine read of the 4 field bytes (no ring record yet — suppressed-bl must record nothing) */
    uint32_t w=hy_word4_peek(pa,fpk);
    uint16_t up=(uint16_t)w, lo=(uint16_t)(w>>16);
    /* local-BL: 2-aligned, low halfword F000-pattern, high halfword D000-pattern (pristine source) */
    if(rc_bl_pattern(up,lo)){
        if(!pure) return 2;                                 /* suppressed-bl is implicit */
        int32_t res=pull_delta(pa,&DR_BL, &M_dibl, DR_DIC_BL, DR_KCAP_BL); /* residual from the single stream */
        uint32_t imm24=rc_bl_imm24(up,lo);
        int32_t off=(int32_t)(imm24<<8)>>8;                 /* sign-extend the 24-bit imm */
        uint32_t T=fpk+4u+(uint32_t)(2*off);                /* BL target byte address (mod 2^32) */
        /* byte shift -> imm24 halfword units. The subtract is done mod 2^32 (a corrupt map can
         * ship values near +-2^31; wrapped garbage is CRC-rejected, signed overflow would be UB);
         * valid map values are tiny, so the wrapped diff equals the true diff bit-exactly. */
        int32_t pred=(int32_t)((uint32_t)smap_at(pa,fpk)-(uint32_t)smap_at(pa,T))/2;
        bl_dereloc(up, lo, (uint32_t)pred+(uint32_t)res, packed);
        hy_word4_rec(pa,fpk,w);                             /* record the 4 BL bytes into the ring */
        return 1;
    }
    /* A1: ex (ldr) DERIVED (same-op back-scan), gated by `pure` (no literal patch in the 4 bytes) —
     * mirrors the encoder's pure(k) + op_ldr_set; positions are derived, not shipped. ldr_targets
     * rejects any non-4-aligned fpk itself, so no alignment pre-test is needed here. */
    if(pure && ldr_targets(pa,g_FWD?NULL:s, fp0, dl, fpk)){
        int32_t res=pull_delta(pa,&DR_EX, &M_diex, DR_DIC_EX, DR_KCAP_EX); /* residual from the single stream */
        int32_t pred=-smap_at(pa,w);                        /* pointer words move by the value's shift */
        hy_word4_rec(pa,fpk,w);                             /* record the 4 EX bytes into the ring */
        pack_s32_buf((w-((uint32_t)pred+(uint32_t)res))&0xffffffffu, packed);
        return 1;
    }
    return 0;
}
/* Literal cursor over the op's content stream + the per-byte write helpers. The cursor advances in
 * wire order regardless of write direction; the copy/extra helpers are plain static functions so the
 * two call sites share one body instead of duplicating the write loops inside sa_apply_op. */
/* Literal-patch cursor. The wire codes nl literal patches as a run of uLEB gaps + a literal byte
 * each, read in wire order. FWD next-positions accumulate forward from 0; grow next-positions step
 * back from dl (first = dl - gap, then -= gap). The first patch and every later patch share one
 * advance path; nextpos=-1 marks "no more". */
typedef struct { int32_t nextpos, litb, li, step, lim; } A1LitCur;
A1_ALWAYS_INLINE void litcur_step(PatchApply *pa, A1ApplyState*s, A1LitCur*lc, int32_t nl){
    if(lc->li<nl){
        uint32_t g=sa_read_uleb(pa,s);
        /* corrupt-stream guard (also removes a signed-overflow UB): a valid gap always keeps
         * the cursor inside [0,lim] (lim==dl; the encoder emits only in-range positions), so
         * bound the gap and the stepped position in pure uint32 (no wrap can slip through:
         * g<=lim and base<=lim keep base+g < 2^32; a backward underflow lands >lim). */
        uint32_t np=(lc->step>0)? (uint32_t)lc->nextpos+g : (uint32_t)lc->nextpos-g;
        if(g>(uint32_t)lc->lim || np>(uint32_t)lc->lim){ g_rcerr=1; lc->nextpos=-1; return; }
        lc->nextpos=(int32_t)np; lc->litb=sa_next_content(pa,s,0);
    }
    else lc->nextpos=-1;
}
A1_ALWAYS_INLINE void litcur_init(PatchApply *pa, A1ApplyState*s, A1LitCur*lc, int fwd, int32_t nl, int32_t dl){
    lc->step = fwd ? 1 : -1; lc->lim = dl;
    lc->nextpos = fwd ? 0 : dl; lc->litb=0; lc->li=0;
    litcur_step(pa, s, lc, nl);   /* read the first patch (or set nextpos=-1 when nl==0) */
}
A1_ALWAYS_INLINE void litcur_next(PatchApply *pa, A1ApplyState*s, A1LitCur*lc, int32_t nl){
    litcur_step(pa, s, lc, nl);
}
/* el extra bytes, written in iteration direction (after dl for FWD, before dl for grow) */
static void wr_extras(PatchApply *pa, A1ApplyState*s, A1CorrCur*cc, int fwd, int32_t tp0, int32_t dl, int32_t el){
    for(int32_t i=0;i<el && !g_rcerr;i++){
        int32_t e = fwd ? i : (el-1-i);
        uint8_t eb=sa_next_content(pa,s,(int)((tp0+dl+e)&1));
        out_write(pa,(uint32_t)(tp0+dl+e), (uint8_t)(eb + corr_take(s,cc,dl+e)));
    }
}
/* copy-mode byte at K: take the literal patch if the cursor sits on K, else pristine source */
static void wr_copy(PatchApply *pa, A1ApplyState*s, A1LitCur*lc, A1CorrCur*cc, int32_t tp0, int32_t fp0, int32_t nl, int32_t K){
    uint8_t db=0;
    if(K==lc->nextpos){ db=(uint8_t)lc->litb; lc->li++; litcur_next(pa,s,lc,nl); }
    out_write(pa,(uint32_t)(tp0+K), (uint8_t)(db + hy_src(pa,fp0+K) + corr_take(s,cc,K)));
}
/* one streaming op: DIRECT geometry+P+C, journal P eagerly, then INLINE write-order field
 * detection + streaming write via out_write (asc FWD / desc grow). No override buffer. */
static void A1_NOINLINE sa_apply_op(PatchApply *pa, A1ApplyState*s){
    uint32_t dl_u=s_ug_gamma(pa,&M_gdl), el_u=s_ug_gamma(pa,&M_gel), adj_u=s_ug_gamma(pa,&M_gadj);
    int32_t adj=bb_unzz(pa,adj_u);
    if(g_rcerr || dl_u>0x7fffffffu || el_u>0x7fffffffu){ g_rcerr=1; return; }
    int32_t dl=(int32_t)dl_u, el=(int32_t)el_u;
    /* nw = dl+el (op output bytes). dl,el in [0,2^31) and the image span is < 2^31 (design invariant),
     * so the overflow guard is a pure 32-bit headroom test: check dl, then el against the rest of span. */
    if(dl_u>(uint32_t)g_image_span || el_u>(uint32_t)g_image_span-dl_u){ g_rcerr=1; return; }
    int32_t nw=(int32_t)(dl_u+el_u);   /* <= g_image_span < 2^31 */
    /* Feature 7B termination safety: the encoder folds out every zero-OUTPUT op, so a valid op always
     * writes >=1 byte. REJECT nw==0 here (do NOT trust the encoder): it is what guarantees the
     * frontier-terminated op loop advances every iteration and cannot spin on a corrupt stream. */
    if(nw==0){ g_rcerr=1; g_reject=REJ_CORRUPT; return; }
    int32_t tp0,fp0;
    if(g_FWD){
        /* tp+nw must stay <= to_size (tp,nw >= 0: a 32-bit headroom test); fp+dl+adj must stay in int32,
         * checked with A1_ADD_OVERFLOW (no 64-bit math). (For a valid stream fp is a small
         * in-range position, so this never trips.) */
        int32_t nfp;
        if(nw>(int32_t)g_to_size || s->tp>(int32_t)g_to_size-nw ||
           A1_ADD_OVERFLOW(s->fp,dl,&nfp) || A1_ADD_OVERFLOW(nfp,adj,&nfp)){ g_rcerr=1; return; }
        tp0=s->tp; fp0=s->fp; s->tp=s->tp+nw; s->fp=nfp;
    } else {
        int32_t nfp;
        if(s->tp<nw ||
           A1_SUB_OVERFLOW(s->fp,dl,&nfp) || A1_SUB_OVERFLOW(nfp,adj,&nfp)){ g_rcerr=1; return; }
        s->tp=s->tp-nw; s->fp=nfp; tp0=s->tp; fp0=s->fp;
    }
    /* fp headroom: the copy loop and the ldr back-scan form fp0+K for K<=dl, so fp0+dl must be
     * representable in int32. The overflow builtins above only guard fp0+dl+adj / fp0 itself —
     * a corrupt adj walk can park fp0 near INT32_MAX and make the bare fp0+dl overflow (UB).
     * Valid streams keep fp inside the image plus small overshoot, so this never fires there. */
    if(fp0>(int32_t)0x7fffffff-dl){ g_rcerr=1; return; }
    /* ---- [P] preserves: journal eagerly (offset deltas) ---- */
    uint32_t np=s_ug_gamma(pa,&M_pgn);
    if(np>(uint32_t)nw){ g_rcerr=1; return; }
    uint32_t poff=0, nwu=(uint32_t)nw;
    for(uint32_t i=0;i<np && !g_rcerr;i++){
        if(!op_next_offset(pa,&poff,i,nwu)) return;
        sa_journal(pa,tp0+(int32_t)poff);
    }
    if(g_rcerr) return;
    /* ---- [C] corrections (sorted cursor array, count-bounded). Correction bytes share M_dval's
     * adaptive byte tree with DEREL escape bytes; this costs no extra resident model state. ---- */
    uint32_t nc=s_ug_gamma(pa,&M_pgn);
    if(nc>(uint32_t)OPC_CAP){ g_rcerr=1; g_reject=REJ_RESOURCE; return; }
    if(nc>(uint32_t)nw){ g_rcerr=1; return; }
    s->op_nc=(int32_t)nc; { uint32_t coff=0;
        for(uint32_t i=0;i<nc && !g_rcerr;i++){
            if(!op_next_offset(pa,&coff,i,nwu)) return;
            int cbyte=s_bt(pa,&M_dval, RC_DVAL_RATE);
            s->op_corr[i]=(coff<<8)|(uint32_t)cbyte; } }
    if(g_rcerr) return;
    /* A1: no BL/LDR offsets on the wire. BL suppression is inferred from !pure, and ldr positions
     * are derived per op (ldr_targets). */
    /* ---- CONTENT decode + streaming write with inline field detection ----
     * Both directions process the same 4-byte ascending field windows over [0,dl); they differ only
     * in (a) iteration direction (step), (b) when the el extra bytes are consumed relative to the dl
     * body (FWD: after; grow: before), and (c) the literal-cursor gap encoding (FWD: absolute forward
     * next-positions; grow: gaps stepping back from dl). The litcur macros hide (c); the el macro is
     * invoked at the direction-correct point to keep the content-stream read order bit-exact. */
    int32_t nl=(int32_t)sa_read_uleb(pa,s);
    if(nl<0||nl>dl){ g_rcerr=1; return; }
    uint8_t packed[4];
    int fwd=g_FWD; int32_t step=fwd?1:-1;
    A1LitCur lc;
    A1CorrCur cc; corr_cur_init(&cc,s,fwd);
    if(!fwd) wr_extras(pa,s, &cc, fwd, tp0, dl, el);
    litcur_init(pa,s, &lc, fwd, nl, dl);
    int32_t k=fwd?0:(dl-1);
    while(k>=0 && k<dl && !g_rcerr){
        int32_t a0=fwd?k:(k-3);                          /* low anchor of the 4-byte field window */
        if(a0>=0 && a0+4<=dl){
            int pure=(lc.nextpos<0 || lc.nextpos<a0 || lc.nextpos>=a0+4);   /* no literal patch in [a0,a0+3] */
            int fa=field_at(pa,s, fp0, a0, packed, pure, dl);
            if(fa){
                for(int b=fwd?0:3; b>=0 && b<4; b+=step){
                    if(fa==2) wr_copy(pa,s, &lc, &cc, tp0, fp0, nl, a0+b);
                    else out_write(pa,(uint32_t)(tp0+a0+b),(uint8_t)(packed[b]+corr_take(s,&cc,a0+b)));
                }
                k+=4*step; continue;
            }
        }
        wr_copy(pa,s, &lc, &cc, tp0, fp0, nl, k); k+=step;
    }
    if(fwd) wr_extras(pa,s, &cc, fwd, tp0, dl, el);
}

/* ===================================================================================== */
/* the decode entry: runs the whole single-stream decode start-to-finish on the CALLER's    */
/* stack (plain calls, no coroutine/fiber). Shared state is passed via the file-scope globals*/
/* that patch_apply_run primes before invoking decode_body.                                  */
/* ===================================================================================== */
/* ---- envelope header (strict reads): CRC32(from)[4] | CRC32(to)[4] | from_size uLEB |
 * zz(to_size-from_size) | [zz(fp_end-from_size) iff descending] | zz(fp_start) |
 * compressed_body_len. The decoder derives the apply direction and the image span itself
 * and verifies CRC32(from) over flash BEFORE the first flash write — a truncated header,
 * implausible sizes, or a wrong/dirty current image all reject cleanly with flash untouched.
 * CRC32(to) is stashed in g_want_to here and checked over the final image after apply
 * (patch_apply_run). Then the literal bit-trees are seeded from flash parity histograms
 * (hist0/hist1/w borrow 4 KiB of ARENA before the apply
 * overlays it). noinline: this runs ONCE at the top of decode_body, so keeping its locals in
 * their own frame (not merged into decode_body's) keeps them off the deep apply call chain
 * that runs afterward — it trims the decode's peak CALLER-stack usage. */
static int A1_NOINLINE decode_header(PatchApply *pa){
    uint32_t want_from, want_to, fs, ts, fpe, z, bl; uint8_t ov;
    if(!env_u32le(pa,&want_from) || !env_u32le(pa,&want_to) || !env_uleb(pa,&fs) || fs>A1_MAX_IMAGE || !env_uleb_ov(pa,&z,&ov)) return 0;
    if(z&1u){ uint32_t m=(z>>1)+1u; if(m>fs) return 0; ts=fs-m; }
    else    { uint32_t d=z>>1; if(d>A1_MAX_IMAGE||fs>A1_MAX_IMAGE-d) return 0; ts=fs+d; }
    /* Apply direction is an ENCODER CHOICE. The NATURAL direction (descending iff the image
     * grows — the historical derived rule, so a canonically-coded envelope decodes exactly
     * as before) ships as a canonical size-delta uLEB; the UNNATURAL direction is signaled
     * by the overlong marker above and costs one byte (chosen only when it wins by more).
     * fp_end ships iff the apply is DESCENDING (it seeds the descending source walk; the
     * ascending walk derives fp from 0). */
    fpe=fs;
    { int desc=(ts>fs)!=(int)ov;
      if(desc && !env_zz_abs(pa,fs,&fpe)) return 0;
      g_FWD=!desc; }
    /* Feature 7B initial source seek (fp_start): zigzag-uLEB, shipped for both directions right after
     * the (grow-only) fp_end field. Plain signed zigzag (NOT delta-vs-a-base) — usually 0. */
    { uint32_t z2; if(!env_uleb(pa,&z2) || z2==0xffffffffu) return 0;
      g_fp_start=(z2&1u)? -(int32_t)((z2>>1)+1u) : (int32_t)(z2>>1); }
    if(!env_uleb(pa,&bl) || bl<4u) return 0;  /* dropped leading cache byte leaves at least 4 code bytes */
    g_from_size=fs; g_to_size=ts; g_want_to=want_to;
    g_body_left=bl;
    g_image_span = fs>ts ? fs : ts;
    if(crc32_flash(fs)!=want_from) return 0;
    { uint32_t *hist0=ARENA.seed.hist0, *hist1=ARENA.seed.hist1, *w=ARENA.seed.w;
      for(int i=0;i<256;i++){ hist0[i]=1; hist1[i]=1; }
      for(uint32_t i=0;i<fs;i++){ uint8_t v=flash_read(i); if(i&1) hist1[v]++; else hist0[v]++; }
      for(int c=0;c<LIT0_CTX;c++) lit_tree_from_hist(&M_lit0[c],hist0,w);
      lit_tree_from_hist(&M_lit1,hist1,w);
      g_litprev=0; }
    { A1ApplyState*s=&SAst; memset(s,0,sizeof*s); s->tp=g_FWD?0:(int32_t)ts; s->fp=g_FWD?g_fp_start:(int32_t)fpe; }
    return 1;
}
static int decode_body(PatchApply *pa){
    g_rcerr=0; g_reject=REJ_NONE;
    if(!decode_header(pa)){ g_reject=REJ_CORRUPT; return 0; }
    rc_init(pa);
    orow_reset(pa);
    /* ---- piecewise shift map: gamma count, then per entry a gamma boundary gap (first absolute,
     * later gaps-1; strictly ascending) and a zigzag-gamma byte-shift value. count 0 => no map
     * (all predictions 0 == the residual stream degenerates to the plain delta stream). ---- */
    /* BORROW two apply-phase gamma statics (not yet live: the map is read before apply-model setup,
     * and both are re-init'd fresh at ugg_init(&M_gdl)/ugg_init(&M_gadj) below before the token loop).
     * Coding the skewed map gap/value distributions through ADAPTIVE gamma beats raw equiprobable bits
     * at ZERO extra SRAM. M_gdl carries the count + boundary gaps; M_gadj carries the zz shift values. */
    ugg_init(&M_gdl); ugg_init(&M_gadj);
    g_smap_n=0;
    { uint32_t mn=s_ug_gamma(pa,&M_gdl);
      if(mn>SMAP_CAP){ g_reject=REJ_RESOURCE; return 0; }
      uint32_t b=0;
      for(uint32_t i=0;i<mn && !g_rcerr;i++){
          uint32_t gap=s_ug_gamma(pa,&M_gdl); if(i) gap+=1u;
          if(gap>UINT32_MAX-b){ g_rcerr=1; break; }
          b+=gap;
          g_smap_b[i]=b; g_smap_v[i]=bb_unzz(pa,s_ug_gamma(pa,&M_gadj));
      }
      if(g_rcerr) return 0;
      g_smap_n=(uint16_t)mn; }
    /* out-match enable: 1 raw bit. 0 => no ko header field and no out-bit on any fresh match,
     * so patches that never out-match (e.g. the one-face update) pay exactly one bit. */
    g_out_en=(uint8_t)s_raw(pa);
    /* ---- STREAMED DELTAS: NO up-front DEREL phase. The delta models are initialized fresh and used
     * INLINE during apply (pull_delta in field_at). M_dval (escape/correction bytes), the two MTF
     * dict streams, and the two index UGolombs all persist through apply. ---- */
    a1_bt_init(&M_dval); dr_init(&DR_BL, DR_DIC_BL, DR_HIT_INIT); dr_init(&DR_EX, DR_DIC_EX, DR_HIT_INIT);
    a1_idx_init(&M_dibl, RC_IDX_SEED); a1_idx_init(&M_diex, RC_IDX_SEED);   /* dict-hit/index seeds mirrored with encoder */
    /* ---- [A] streaming apply (no bake): per op read DIRECT geom+P+C, journal P eagerly,
     * then PULL the op's CONTENT from the cut whole-stream LZSS, detect de-reloc fields inline in
     * write order (pulling each delta from the single stream via pull_delta), write via out_write. ---- */
    jr_reset(pa);
    ugg_init(&M_pg); ugg_init(&M_pgn);
    ugg_init(&M_pg2);
    ugg_seed_cont(&M_pg2,RC_SEED_DEPTH_PG2);  /* rest preserve/corr gaps are strictly-increasing distinct offsets
                               * => gap>=1 => M_pg2's first unary-prefix bit is ALWAYS continue (a format
                               * invariant, like M_gl): seed it cheap from symbol 1. */
    ugg_init(&M_gdl); ugg_init(&M_gel); ugg_init(&M_gadj);
    ugg_seed_cont(&M_gdl,RC_SEED_DEPTH_GDL); ugg_seed_cont(&M_gadj,RC_SEED_DEPTH_GADJ);
    { A1ApplyState*s=&SAst;
      int kd=(int)s_raw_bits(pa,RC_KFIELD_BITS);
      int ko=g_out_en?(int)s_raw_bits(pa,RC_KFIELD_BITS):0;
      ugr_init(&M_gd,kd); ugg_init(&M_gl); ugg_init(&M_gs);
      ugr_init(&M_go,ko); ugg_init(&M_glo); M_outb=RC_PHALF;
      g_oexp=g_FWD?0u:g_to_size;
      ugg_seed_cont(&M_gl,RC_SEED_DEPTH_GL);   /* matches are always len>=3 (value>=2 => cl>=1): M_gl's first
                                 * unary prefix bit is ALWAYS continue, so seed it cheap. */
      a1_fl_init(&M_flag);
      M_rep0[0]=M_rep0[1]=RC_REP0_INIT; g_rep0h=0; g_lastdist=0;
      if(g_rcerr) return 0;
      s->tok_mode=0;
      /* Feature 7B: NO shipped op count. Frontier-terminate — FWD until tp reaches to_size, grow
       * until tp reaches 0. This is SAFE because sa_apply_op REJECTS any zero-output op (nw==0), so
       * every accepted op advances the frontier by >=1 and the overshoot/underrun guards cap it: the
       * loop runs at most to_size times, then rejects (a corrupt stream cannot spin). A truncated
       * stream decodes zero-fill garbage that trips a geometry/frontier guard or lands a wrong image
       * that CRC32(to) rejects. */
      while(!g_rcerr && (g_FWD ? s->tp!=(int32_t)g_to_size : s->tp!=0)) sa_apply_op(pa,s);
      /* tp must land exactly (the loop guarantees it on the clean path); grow additionally must land
       * fp exactly on the initial source seek g_fp_start (was 0 pre-7B). FWD fp is unchecked (fp_end
       * is not shipped for FWD — CRC32(to) validates). */
      if(!g_rcerr && (s->tp!=(g_FWD?(int32_t)g_to_size:0) || (!g_FWD && s->fp!=g_fp_start))) g_rcerr=1;
      if(!g_rcerr && s->tok_mode!=0) g_rcerr=1;   /* a mid-token content underrun leaves tok_mode!=0 */
      if(g_rcerr) return 0;
      orow_commit_all(pa);                 /* flush the remaining uncommitted rows */
    }
    /* body complete: the caller verifies CRC32(to) (g_want_to) over the final image before
     * declaring DONE. Bytes after the counted body belong to outer framing. */
    return 1;
}

/* ===================================================================================== */
/* public API: patch_apply_run(state, callback, ctx) -> DONE / ERROR                       */
/* ===================================================================================== */
static void A1_NOINLINE lit_tree_from_hist(A1BitTree*t,const uint32_t*hist,uint32_t*w){
    rc_lit_tree_from_hist(t,hist,w);
}

/* Run the whole decode synchronously on the CALLER's stack. `next` returns 1 and one blob
 * byte, or 0 when the source aborts/ends early (it may block internally — e.g. poll a UART —
 * before returning; the decoder consumes bytes strictly in order). Valid patches terminate at
 * the header's compressed body length and do not require callback EOF. Returns
 * PATCH_APPLY_DONE (image written, both CRCs verified) or PATCH_APPLY_ERROR (g_reject holds
 * the reason). */
static int patch_apply_run(PatchApply *pa, int (*next)(void*, uint8_t*), void *ctx){
    memset(pa,0,sizeof *pa);
    g_pull_fn=next; g_pull_ctx=ctx; g_body_left=0; g_pull_eof=0; g_flash_touched=0;
    if(!decode_body(pa)) return PATCH_APPLY_ERROR;
    if(crc32_flash(g_to_size)!=g_want_to){
        if(!g_reject) g_reject=REJ_CORRUPT;
        return PATCH_APPLY_ERROR; }
    return PATCH_APPLY_DONE;
}

/* After PATCH_APPLY_ERROR, the reject reason: REJ_RESOURCE (a decoder cap was exceeded — this
 * firmware needs a larger build) vs REJ_CORRUPT (malformed/truncated/wrong-image). Prefer this
 * accessor over reading g_reject directly; it is unused-static-inline and compiles out when the
 * integrator does not call it (no ARM .text cost). */
static inline int patch_apply_reject(const PatchApply *pa){ return g_reject; }

/* After PATCH_APPLY_ERROR, whether any flash_write happened this run: 0 = old image intact
 * (still bootable, safe to retry after fixing the cause), 1 = image partially overwritten
 * (bootloader recovery required). Same unused-static-inline compile-out as patch_apply_reject. */
static inline int patch_apply_flash_touched(const PatchApply *pa){ return g_flash_touched; }

/* Parsed patch geometry/state after a completed run. These keep host harnesses and
 * integrations from depending on decoder global names while still compiling out when unused. */
static inline uint32_t patch_apply_from_size(const PatchApply *pa){ return g_from_size; }
static inline uint32_t patch_apply_to_size(const PatchApply *pa){ return g_to_size; }
static inline uint32_t patch_apply_image_span(const PatchApply *pa){ return g_image_span; }
static inline int patch_apply_forward(const PatchApply *pa){ return g_FWD; }
static inline uint32_t patch_apply_journal_used(const PatchApply *pa){ return g_jcount; }

#undef g_image_span
#undef g_from_size
#undef g_to_size
#undef g_fp_start
#undef g_want_to
#undef g_FWD
#undef g_pull_fn
#undef g_pull_ctx
#undef g_body_left
#undef g_pull_eof
#undef RC
#undef g_rcerr
#undef g_reject
#undef g_flash_touched
#undef ARENA
#undef Jbuf
#undef SAst
#undef g_jcount
#undef g_smap_b
#undef g_smap_v
#undef g_smap_n
#undef g_orow_buf
#undef g_orow_base
#undef g_orow_dirty
#undef M_gd
#undef M_gl
#undef M_gs
#undef M_go
#undef M_glo
#undef M_outb
#undef g_out_en
#undef g_oexp
#undef M_pg
#undef M_pgn
#undef M_pg2
#undef M_gdl
#undef M_gel
#undef M_gadj
#undef M_dibl
#undef M_diex
#undef M_lit0
#undef M_lit1
#undef g_litprev
#undef M_dval
#undef M_flag
#undef M_rep0
#undef g_rep0h
#undef g_lastdist
#undef DR_DIC_BL
#undef DR_DIC_EX
#undef DR_BL
#undef DR_EX
#undef g_psrc_imm
#undef g_psrc_ldr
#undef A1_NOINLINE
#undef A1_ALWAYS_INLINE
#undef A1_ADD_OVERFLOW
#undef A1_SUB_OVERFLOW
#undef RC_RICE_UNARY_MAX
#undef JREGION
#undef SA_RING
#undef SA_MASK
#undef SA_ARENA
#undef ARENA_BYTES
#undef RC_UNARY_MAX
#undef OROW_NONE
#undef OROW_SLOT
#undef PSRC_HW_MASK

#endif /* PATCH_APPLY_H */
