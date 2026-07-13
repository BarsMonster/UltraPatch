/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 */

/* Divide-free range-coder model definitions (shared by decoder + host encoder).
 * Binary range coder (LZMA bound: bound=(range>>12)*prob; compare) — no runtime division
 * instructions or helpers, as required for Cortex-M0+/ARMv6-M. Header-only; both
 * src/patch_apply.h (device decoder) and the host encoder modules carry their own range-coder
 * bit I/O and share the model structs/constants below, so the wire stays bit-exact. */
#ifndef UP_RC_MODELS_H
#define UP_RC_MODELS_H
#include <stdint.h>
#include <string.h>
#include "patch_config.h"

/* GNU attributes are OPTIONAL codegen hints (host-text / ARM-size / stack shaping), never
 * correctness: the #else fallbacks keep this header standard C (C99 + C11 _Static_assert), so
 * non-GNU compilers build a wire-identical decoder. -DNO_GNU_EXTENSIONS forces the plain-C
 * fallbacks even when __GNUC__ is visible, for compilers that define it for compatibility
 * without full attribute support. */
#if !defined(NO_GNU_EXTENSIONS) && (defined(__GNUC__) || defined(__clang__))
#define RC_ALWAYS_INLINE static inline __attribute__((always_inline))
#define RC_NOINLINE __attribute__((noinline))
#define RC_NORETURN __attribute__((noreturn))
#define RC_WARN_UNUSED_RESULT __attribute__((warn_unused_result))
#define RC_ADD_OVERFLOW(a,b,out) __builtin_add_overflow((a),(b),(out))
#define RC_SUB_OVERFLOW(a,b,out) __builtin_sub_overflow((a),(b),(out))
#else
#define RC_ALWAYS_INLINE static inline
#define RC_NOINLINE
#define RC_NORETURN
#define RC_WARN_UNUSED_RESULT
static inline int rc_add_overflow_i32(int32_t a,int32_t b,int32_t*out){
    if((b>0 && a>INT32_MAX-b) || (b<0 && a<INT32_MIN-b)) return 1;
    *out=(int32_t)(a+b);
    return 0;
}
static inline int rc_sub_overflow_i32(int32_t a,int32_t b,int32_t*out){
    if((b<0 && a>INT32_MAX+b) || (b>0 && a<INT32_MIN+b)) return 1;
    *out=(int32_t)(a-b);
    return 0;
}
#define RC_ADD_OVERFLOW(a,b,out) rc_add_overflow_i32((a),(b),(out))
#define RC_SUB_OVERFLOW(a,b,out) rc_sub_overflow_i32((a),(b),(out))
#endif

/* Every model move below shifts bytes toward a higher address. Library mode deliberately uses
 * one memmove primitive for both overlapping MTF shifts and non-overlapping model copies, so a
 * decoder does not pull in both memcpy and memmove. The decoder normally lives inside a larger
 * program: if that program already links memmove, calling it here adds no library code. Otherwise,
 * define HAND_ROLLED_MEMMOVE to avoid pulling memmove in and use the private backward byte loop.
 * The volatile source access prevents an optimizing compiler from recognizing that loop and
 * reintroducing a libc call. */
#ifdef HAND_ROLLED_MEMMOVE
static RC_NOINLINE void rc_move_high(void*dst,const void*src,size_t n){
    uint8_t*d=(uint8_t*)dst;
    const volatile uint8_t*s=(const volatile uint8_t*)src;
    while(n){ n--; d[n]=s[n]; }
}
#define RC_MOVE_HIGH(dst,src,n) rc_move_high((dst),(src),(n))
#else
#define RC_MOVE_HIGH(dst,src,n) ((void)memmove((dst),(src),(n)))
#endif

#define RC_KTOP (1u<<24)
#define RC_PROB_BITS 12u
#define RC_PBIT 4096u
#define RC_PHALF 2048u
#define RC_PROB_BOUND(range, prob) (((range) >> RC_PROB_BITS) * (prob))
#define RC_PACKED_POS_BITS 24u
#define RC_PACKED_POS_LIMIT (1u<<RC_PACKED_POS_BITS)
/* Inclusive maximum Rice quotient accepted by the decoder's adaptive-unary reader. Keep the
 * encoder on the same bound: a larger quotient would make an otherwise valid generated patch
 * fail as a malformed run-on at apply time. */
#define RC_RICE_UNARY_MAX (1u<<20)
static inline int rc_rice_feasible(uint32_t v,uint32_t k){
    return k<32u && (v>>k)<=RC_RICE_UNARY_MAX;
}

/* Domain-separate otherwise identical envelopes from incompatible wire revisions. Because the
 * source CRC is checked before apply, a revision mismatch rejects before any flash write. */
static inline uint32_t rc_wire_from_crc(uint32_t crc){
    return crc^(uint32_t)PATCH_WIRE_VERSION;
}

/* ---- 256-symbol byte via 8-level bit-tree; logical probs[1..255] are stored as 12-bit
 * range-coder probabilities (p[0..254]). Probabilities are always in 1..4095, so they pack into
 * 12 bits/node instead of a full uint16_t; this struct + accessors are the ONE byte-tree
 * implementation, used by both the device decoder and host encoder modules. The adaptation-shift rate is NOT
 * stored per-tree (saves SRAM): every call site passes its constant rate explicitly (RC_LIT0_RATE,
 * RC_LIT1_RATE, RC_DVAL_RATE — single-sourced below) — kept identical on both sides for wire-exactness. */
#define UP_BT_PROBS 255u
#define UP_BT_BYTES (((UP_BT_PROBS * 12u) + 7u) / 8u)
/* idx*12 has a byte shift of 0 or 4, so every 12-bit entry fits in two bytes. */
typedef struct { uint8_t p[UP_BT_BYTES]; } up_BitTree;
static inline uint16_t up_bt_get(const up_BitTree*t,int idx){
    uint32_t bit=(uint32_t)idx*12u, off=bit>>3, sh=bit&7u;
    uint32_t v=(uint32_t)t->p[off] | ((uint32_t)t->p[off+1u]<<8);
    return (uint16_t)((v>>sh)&0xfffu);
}
static inline void up_bt_set(up_BitTree*t,int idx,uint16_t prob){
    uint32_t bit=(uint32_t)idx*12u, off=bit>>3, sh=bit&7u;
    uint32_t v=(uint32_t)t->p[off] | ((uint32_t)t->p[off+1u]<<8);
    v=(v&~(0xfffu<<sh)) | (((uint32_t)prob&0xfffu)<<sh);
    t->p[off]=(uint8_t)v; t->p[off+1u]=(uint8_t)(v>>8);
}
static inline void up_bt_init(up_BitTree*t){ memset(t->p,0,sizeof t->p); for(int i=0;i<(int)UP_BT_PROBS;i++) up_bt_set(t,i,RC_PHALF); }

/* ---- seeded Golomb context clamp (Rice/Gamma length & dist models) ----
 * UP_UG_CTX = context clamp. Rice keeps a full (UP_UG_CTX+1)^2 mantissa table because any quotient
 * row can use k columns. Gamma only reaches triangular rows 1..UP_UG_CTX-1 plus the full clamped
 * row UP_UG_CTX; row 0 has no mantissa bits.
 * ENCODING-AFFECTING: decoder and encoder must use the same value. */
#define UP_UG_CTX 6
#define UP_UG_C(x) ((x)<UP_UG_CTX?(x):UP_UG_CTX)
#define UP_UG_GAMMA_MANT (((UP_UG_CTX) * ((UP_UG_CTX) - 1)) / 2 + ((UP_UG_CTX) + 1))
static inline int rc_ugg_mant_idx(int row,int pos){
    return row<UP_UG_CTX ? (row*(row-1))/2 + pos : ((UP_UG_CTX*(UP_UG_CTX-1))/2 + UP_UG_C(pos));
}

/* ---- piecewise shift map (BL/EX delta prediction) ----
 * Shipped per patch: ascending u32 boundaries + int32 byte-shift values; keys below the first
 * boundary (or an absent map) predict shift 0. ENCODING-AFFECTING: the decoder rejects
 * (REJ_RESOURCE) above UP_SMAP_CAP, and the encoder never emits more entries than this. */
#define UP_SMAP_CAP 48

/* ---- tag0 literal-tree context map: previous literal byte -> tree id. Re-derived 2026-07 by a
 * coder-faithful greedy coordinate descent that replays the
 * ACTUAL shipped adaptive coder -- 5 even-parity from-image histogram-seeded BitTrees adapting at
 * RC_LIT0_RATE in wire order -- over the surviving tag0 span literals of the full home + foreign
 * corpora, accepting a row move only when it strictly cuts home bits and never raises foreign bits.
 * Supersedes the static agglomerative-clustering map (a measured dead end: a static-entropy objective
 * underperforms the real adaptive coder even home-only). ENCODING-AFFECTING: device decoder and
 * host encoder modules share this table (bit-exact wire). UP_LIT0_CTX must equal 1 + max entry. ---- */
#define UP_LIT0_CTX 5
static inline uint8_t rc_lit0_sel(uint8_t p){
    uint8_t v=(uint8_t)(
        "\001\000\000\000\000\000\000\035\252\252\252\252\125\125\125\025"
        "\367\177\125\125\121\167\114\160\377\377\077\377\377\377\377\077"
        "\377\065\177\177\252\252\252\252\000\000\232\146\000\004\021\025"
        "\102\200\000\050\125\205\125\005\000\000\000\000\002\000\000\000"
    )[p>>2];
    return p==247u ? 4u : (uint8_t)((v >> ((p&3u)*2u)) & 3u);
}

/* ---- order-2 token flag: 4 contexts (previous 2 flags) ---- */
typedef struct { uint16_t m[4]; int h; } up_Flag1;
static inline void up_fl_init(up_Flag1*f){ for(int i=0;i<4;i++) f->m[i]=RC_PHALF; f->h=0; }
/* flag-history context update, shared by decoder/encoder/DP pricing (wire-affecting mirror). */
static inline int rc_fl_hist(int h,int b){ return ((h<<1)|b)&3; }

/* =====================================================================================
 * Shared PURE wire semantics — hand-mirrored decoder<->encoder helpers single-sourced here so
 * the COMPILER (not a "must match" comment) enforces the mirror, exactly as bt_/fl_ above already
 * do. Most are used verbatim by BOTH src/patch_apply.h (device decoder) and the host encoder; the
 * decoder-direction mirrors kept here (rc_unzz32_value, rc_zz_abs, rc_uleb_overlong,
 * rc_outmatch_pos) are decoder-only — their encoder-side twins (rc_zz32, rc_uleb_len,
 * rc_natural_desc/rc_dir_is_natural, rc_outmatch_delta) live in src/enc_internal.h. All
 * are 64-bit-free and division-helper-free; the literal-seed ratio below uses fixed-width
 * shift/subtract long division. They preserve the exact dataflow so Cortex-M0+ codegen does not shift.
 * ===================================================================================== */

/* adaptive-bit probability update: nudge p toward 0 (bit=1) or RC_PBIT (bit=0) by 1/2^rate.
 * Shared by decoder s_bit_r and encoder re_bit. */
static inline uint16_t rc_adapt(uint32_t p, int bit, int rate){
    return (uint16_t)(bit ? p-(p>>rate) : p+((RC_PBIT-p)>>rate));
}

/* Histogram-seeded literal-tree node prob: round(RC_PBIT*num/den) half-up, then clamp to
 * 1..RC_PBIT-1 (an empty context degenerates to RC_PHALF). Long division emits one rounding bit
 * beyond the 12-bit fraction; num<=den and MAX_IMAGE<=2^26 keep every doubled remainder in u32.
 * The rounded result is 0..RC_PBIT, so subtracting its high bit performs the upper clamp.
 * Shared by decoder lit_tree_from_hist and encoder lit_tree_seed_e. */
static inline uint16_t rc_lit_seed_prob(uint32_t num, uint32_t den){
    if(!den) return RC_PHALF;
    uint32_t pr=0;
    int bits=(int)RC_PROB_BITS+1;
    do {
        num<<=1; pr<<=1;
        if(num>=den){ num-=den; pr++; }
    } while(--bits);
    pr=(pr+1u)>>1;
    return (uint16_t)(pr ? pr-(pr>>RC_PROB_BITS) : 1u);
}

static inline void rc_u32le_put(uint8_t *p, uint32_t v){
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}

/* Reconstruct the signed value represented by a two's-complement wire bit pattern. Both casts and
 * the signed addition stay in range, including the 0x80000000 -> INT32_MIN boundary. */
static inline int32_t rc_i32_from_u32(uint32_t u){
    return u<=INT32_MAX ? (int32_t)u
                        : INT32_MIN+(int32_t)(u-((uint32_t)INT32_MAX+1u));
}
/* Zigzag decode mirror (encode-side rc_zz32 lives in src/enc_internal.h). Perform the inverse in
 * unsigned bit space, then reconstruct the resulting signed wire value without an out-of-range cast. */
static inline int32_t rc_unzz32_value(uint32_t u){
    return rc_i32_from_u32((u>>1) ^ (0u-(u&1u)));
}
static inline int rc_zz_abs(uint32_t base,uint32_t z,uint32_t max,uint32_t*out){
    if(z&1u){ uint32_t m=(z>>1)+1u; if(m>base) return 0; *out=base-m; }
    else    { uint32_t d=z>>1; if(d>max||base>max-d) return 0; *out=base+d; }
    return 1;
}

static inline int rc_uleb_overlong(int n,uint8_t last){ return n>1 && last==0u; }

static inline void rc_seed_cont_u(uint16_t*u,int maxidx,int depth){
    for(int i=0;i<depth && i<=maxidx;i++) u[i]=(uint16_t)(RC_PBIT/16);
}
RC_ALWAYS_INLINE void rc_init_probs(uint16_t*p,int n,uint16_t seed){
    for(int i=0;i<n;i++) p[i]=seed;
}
/* ---- UGolomb (Rice/Gamma) model structs, single-sourced so the COMPILER enforces the
 * decoder<->encoder mirror. Rice keeps a full (UP_UG_CTX+1)^2 mantissa table (any quotient row can use
 * k columns); Gamma keeps the compact triangular table (UP_UG_GAMMA_MANT). Both sides use these types
 * and the shared neutral-init helpers directly (no per-side wrappers). */
typedef struct { uint8_t k; uint16_t u[UP_UG_CTX+1]; uint16_t m[UP_UG_CTX+1][UP_UG_CTX+1]; } up_UGRice;
typedef struct { uint16_t u[UP_UG_CTX+1]; uint16_t m[UP_UG_GAMMA_MANT]; } up_UGGamma;
/* noinline: these neutral-init loops are the primitives folded into rc_init_prekd/rc_init_tok below.
 * Keeping them out-of-line preserves the single shared copy the host previously got via the ugg_init_e
 * wrapper (inlining a copy per model at every apply-init site bloats host .text); on the Cortex-M0+
 * -Os decoder they were already emitted once, so the attribute costs no ARM .text. */
static RC_NOINLINE void rc_ugr_init(up_UGRice*g,int k){
    g->k=(uint8_t)k;
    for(int i=0;i<=UP_UG_CTX;i++){ g->u[i]=RC_PHALF; for(int j=0;j<=UP_UG_CTX;j++) g->m[i][j]=RC_PHALF; }
}
static RC_NOINLINE void rc_ugg_init(up_UGGamma*g){
    for(int i=0;i<=UP_UG_CTX;i++) g->u[i]=RC_PHALF;
    for(int i=0;i<UP_UG_GAMMA_MANT;i++) g->m[i]=RC_PHALF;
}

static inline void rc_lit_tree_from_hist(up_BitTree*t,const uint32_t*hist,uint32_t*w){
    /* Leaves are already resident in hist[]. Keeping a second 256-word leaf copy in the
     * decoder's phase-overlaid seed workspace only inflated PatchApply: build the internal
     * nodes directly against hist and retain only those nodes in w[]. */
    for(int m=255;m>=1;m--){
        int leaf=2*m>=256;
        uint32_t l=leaf?hist[2*m-256]:w[2*m];
        uint32_t r=leaf?hist[2*m+1-256]:w[2*m+1];
        w[m]=l+r;
    }
    for(int m=1;m<256;m++){
        uint32_t l=2*m>=256?hist[2*m-256]:w[2*m];
        up_bt_set(t,m-1,rc_lit_seed_prob(l,w[m]));
    }
}

/* Thumb BL/BLX local-branch halfword pattern (F000 / D000), tested on pristine source bytes. */
static inline int rc_bl_pattern(uint16_t up, uint16_t lo){
    return ((up&0xf800u)==0xf000u) && ((lo&0xd000u)==0xd000u);
}
/* unpack the 24-bit BL immediate (halfword units), RAW (unsigned, not yet sign-extended). */
RC_ALWAYS_INLINE uint32_t rc_bl_imm24(uint16_t up, uint16_t lo){
    uint32_t s=(up>>10)&1u, i1=1u-(((lo>>13)&1u)^s), i2=1u-(((lo>>11)&1u)^s);
    return ((up&0x3ffu)<<11)|(lo&0x7ffu)|(s<<23)|(i1<<22)|(i2<<21);
}
/* pack a 24-bit BL immediate back into the F000/D000 halfword pair -> 4 little-endian bytes. */
static inline void rc_bl_pack(uint32_t imm24, uint8_t out[4]){
    uint32_t s=(imm24>>23)&1u;
    uint32_t j1=1u-(((imm24>>22)&1u)^s), j2=1u-(((imm24>>21)&1u)^s);
    uint16_t u=(uint16_t)(0xF000u|(s<<10)|((imm24>>11)&0x3ffu));
    uint16_t l=(uint16_t)(0xD000u|(j1<<13)|(j2<<11)|(imm24&0x7ffu));
    out[0]=(uint8_t)u; out[1]=(uint8_t)(u>>8); out[2]=(uint8_t)l; out[3]=(uint8_t)(l>>8);
}
static inline int32_t rc_bl_imm24s(uint16_t up, uint16_t lo){
    uint32_t imm=rc_bl_imm24(up,lo);
    if(imm&0x00800000u) imm|=0xff000000u;
    return rc_i32_from_u32(imm);
}
static inline uint32_t rc_bl_target(uint32_t pc, uint16_t up, uint16_t lo){
    return pc + 4u + (uint32_t)(2 * rc_bl_imm24s(up,lo));
}
static inline void rc_bl_dereloc(uint16_t up, uint16_t lo, uint32_t delta, uint8_t out[4]){
    rc_bl_pack((rc_bl_imm24(up,lo) - delta) & 0x00ffffffu, out);
}

/* Thumb LDR-literal target byte address from the instruction address (halfword-scanned, rounded
 * back to 4-alignment) + imm8 word field: (addr & ~3u) + 4u*imm8 + 4u. Address arithmetic is
 * explicitly unsigned: literal targets may cross INT32_MAX, while op-local cursors stay signed. */
static inline int rc_thumb_ldr_lit(uint16_t up){ return (up&0xf800u)==0x4800u; }
static inline uint32_t rc_ldr_target(uint32_t addr, uint32_t imm8){
    /* The word-domain form is identical modulo uint32_t and smaller on Thumb-1. */
    return ((addr>>2)+imm8+1u)<<2;
}
static inline int rc_ldr_target_in_op(int32_t fp0, int32_t dl, uint32_t fpk){
    int32_t end;
    if((fpk&3u) || dl<0 || fp0>INT32_MAX-dl) return 0;
    end=fp0+dl;
    return end>=4 && fpk<=(uint32_t)end-4u;
}
static inline int32_t rc_ldr_scan_first(int32_t fp0, uint32_t fpk){
    int32_t lo=(int32_t)fpk-1024;
    if(lo<fp0) lo=fp0;
    if(lo<0) lo=0;
    if(lo&1) lo++;
    return lo;
}

/* piecewise shift-map lookup: value of the last boundary <= x, else 0 (also 0 for an empty map). */
static inline int32_t rc_smap_at(const uint32_t*b, const int32_t*v, int n, uint32_t x){
    int lo=0, hi=n-1, r=-1;
    while(lo<=hi){ int mid=(lo+hi)>>1; if(b[mid]<=x){ r=mid; lo=mid+1; } else hi=mid-1; }
    return r<0 ? 0 : v[r];
}
static inline int32_t rc_smap_pred_bl(const uint32_t*b, const int32_t*v, int n, uint32_t pc, uint32_t target){
    return rc_i32_from_u32((uint32_t)rc_smap_at(b,v,n,pc) -
                           (uint32_t)rc_smap_at(b,v,n,target)) / 2;
}
static inline int32_t rc_smap_pred_ex(const uint32_t*b, const int32_t*v, int n, uint32_t word){
    return rc_i32_from_u32(0u - (uint32_t)rc_smap_at(b,v,n,word));
}

/* MTF cache-index UNARY model: UP_IDX_CTX per-position priors (min(pos,UP_IDX_CTX-1) clamp). The encoded
 * index is ~54% zero, so unary fits the concentration; UP_IDX_CTX=5 is the corpus optimum (4/6/8/16 all
 * code worse). Shared struct + seed; the encode/decode loops live in each side. */
#define UP_IDX_CTX 5
typedef struct { uint16_t u[UP_IDX_CTX]; } up_IdxUnary;
static inline void up_idx_init(up_IdxUnary*g,uint16_t seed){ for(int i=0;i<UP_IDX_CTX;i++) g->u[i]=seed; }

static inline void rc_mtf_promote_i32(int32_t*dic,uint32_t j){
    if(j){ int32_t t=dic[j]; RC_MOVE_HIGH(&dic[1],&dic[0],(size_t)j*sizeof(dic[0])); dic[0]=t; }
}
static inline void rc_mtf_insert_i32(int32_t*dic,uint16_t*K,uint16_t cap,int32_t v){
    uint16_t n=*K;
    if(n<cap) (*K)++;
    else n=(uint16_t)(cap-1u);
    RC_MOVE_HIGH(&dic[1],&dic[0],(size_t)n*sizeof(dic[0]));
    dic[0]=v;
}
RC_ALWAYS_INLINE uint8_t rc_dr_rep_ctx(uint8_t rh,int32_t last){
    return (uint8_t)(rh | (last==0 ? 2 : 0));
}
/* STREAMED-DELTA per-stream state (bl, ex): a bounded MOVE-TO-FRONT cache of delta values + an
 * adaptive "repeat-last" bit + an adaptive "cache-hit" bit. The cache array lives OUTSIDE the struct
 * so bl/ex get separate caps; single-sourced here so decoder + encoder share the neutral init. */
typedef struct {
    uint16_t K;                       /* MTF cache entries in use (index 0 = most-recently-used) */
    uint16_t rep[4], hit; uint8_t rh; /* adaptive binary models (P(bit==0)); rep keyed by prev repeat + last==0 */
} up_DRStream;
RC_ALWAYS_INLINE void rc_dr_init(up_DRStream*d,int32_t*dic,uint16_t hitseed){
    d->K=1; dic[0]=0;
    rc_init_probs(d->rep,4,RC_PHALF);
    d->rh=0; d->hit=hitseed;
}

/* =====================================================================================
 * Shared wire constants — single-sourced so the COMPILER (not a "must match" comment)
 * enforces the encoder/decoder mirror. Each macro below is used verbatim by the device decoder
 * and host encoder modules; changing a value here moves both sides at once, keeping the
 * range-coded wire bit-exact.
 * ===================================================================================== */

/* Adaptive-bit rate (probability-update shift) for the Golomb (unary+mantissa), order-2 flag,
 * and MTF rep/hit streams. Tuned to 4 (1/16): these low-cardinality, fast-drifting contexts
 * track better at 4 than the LZMA-classic 5. The literal/dval bit-trees keep their OWN per-tree
 * rate, passed per call (see the up_BitTree note above), so only this shared default lives here. */
#define RC_S_BIT_RATE 4

/* Per-tree adaptation-shift rates for the literal + dval byte-trees. These bit-trees carry their
 * OWN rate (not stored per-tree — see the up_BitTree note above — but passed at every s_bt/bt_encode
 * call site), single-sourced here so encoder and decoder move together. RC_LIT0_RATE (tag0 span
 * literals, order-1 context) adapts slower (1/32); RC_LIT1_RATE (tag1 literals) + RC_DVAL_RATE
 * (MTF escape + [C] correction bytes) track at 1/16. */
#define RC_LIT0_RATE 5
#define RC_LIT1_RATE 4
#define RC_DVAL_RATE 4

/* rep0 prior: adaptive flag before a match distance. =1 reuses the immediately-previous match
 * distance (value omitted); =0 codes a fresh distance. Seeded toward 0 (P(reuse)~1/4) because
 * exact distance reuse is the minority case: a low prior keeps the dominant =0 flag near-free on
 * small patches while corpus-scale streams adapt up. 3072 = RC_PBIT - RC_PBIT/4; corpus tuning
 * (paired min-over-N sweep) puts the optimum that does NOT regress the one-face product patch at
 * ~1/4 (3/8 helps corpus more but regresses the real one-face update by +1/+1). Both sides store
 * the prior in up_TokModels.rep0 and initialize it through rc_init_tok. */
#define RC_REP0_INIT (RC_PBIT - (RC_PBIT>>2))

/* MTF cache-hit bit seed (DR_BL/DR_EX): a zero-seeded MTF cache makes the hit-bit==1 likely, so seed
 * the adaptive hit model high. 576 = tuned corpus optimum. Shared rc_dr_init seeds both sides. */
#define UP_DR_HIT_INIT 576u

/* MTF cache-index unary seed (dibl/diex): seed every unary-prefix prior toward STOP (idx 0) so the
 * just-promoted index 1 (encoded value 0), which dominates, is cheap from the first symbol. 2816 =
 * corpus optimum. (Shared up_idx_init seeds both sides.) */
#define RC_IDX_SEED 2816u

/* seed_cont depths: bias the first N unary-prefix positions of a gamma model toward "continue"
 * (bit 1). Structural priors (format invariants), NOT corpus caps — they make the very first op as
 * cheap as the warmed-up state. Shared rc_ugg_seed_cont and rc_init_* seed both sides.
 *   GDL  = per-op diff_len gamma; op magnitudes are essentially never tiny.
 *   GADJ = per-op adj gamma.
 *   PG2  = rest preserve/corr gaps are strictly-increasing distinct offsets => gap>=1.
 *   GL   = match length gamma; matches are always len>=3 (value>=2 => cl>=1). */
#define RC_SEED_DEPTH_GDL  6
#define RC_SEED_DEPTH_GADJ 3
#define RC_SEED_DEPTH_PG2  1
#define RC_SEED_DEPTH_GL   1

/* Out-match minimum length: out-match lengths ship as (len - RC_OUTMATCH_MIN) via
 * up_TokModels.glo, so the smallest representable out-match is RC_OUTMATCH_MIN bytes. */
#define RC_OUTMATCH_MIN 4u

static inline uint32_t rc_outmatch_pos(uint32_t expected, int32_t delta){
    return expected + (uint32_t)delta;
}
static inline uint32_t rc_outmatch_next_expect(int fwd, uint32_t pos, uint32_t len){
    return fwd ? pos + len : pos - len;
}

/* Header raw k-field width: the distance rice parameter kd and (when out-matches are enabled) the
 * out-position rice parameter ko each ship as a fixed RC_KFIELD_BITS-bit raw field. */
#define RC_KFIELD_BITS 4

/* ---- Apply-phase model groups. The two blocks of field-for-field-identical models embedded in BOTH
 * the decoder PatchApply (patch_apply.h) and the host encoder Models (enc_internal.h): the COMPILER,
 * not a "must match" comment, now enforces the layout mirror, and the two rc_init_* helpers below are
 * the SINGLE source of the ~20-statement apply-phase init call sequence that was hand-mirrored across
 * decode_body, models_init_content and emit_body. Excluded by design: DR_BL/DR_EX (the host DRE wraps
 * up_DRStream with its own dict ptr+cap), the literal trees (different seed sources), and rep0h/last_dist
 * (zeroed by each side's memset). Init is idempotent per model, so either side may run these in whatever
 * order suits its surrounding code (e.g. the encoder borrows gdl/gadj for the map header, then re-inits
 * via rc_init_prekd) without moving the wire. */
typedef struct {
    up_BitTree  dval;                         /* MTF escape + [C] correction bytes */
    up_IdxUnary dibl, diex;                   /* MTF cache-index unary priors (bl/ex) */
    up_UGGamma  pg, pgn, pg2, gdl, gel, gadj; /* preserve/corr gaps + counts + per-op geometry */
} up_PreKdModels;
typedef struct {
    up_UGRice  gd, go;                         /* backref-distance + out-position rice */
    up_UGGamma gl, gs, glo;                    /* match / span / out-match lengths */
    uint16_t  outb;                           /* out-match enable bit */
    up_Flag1   flag;                           /* order-2 token flag */
    uint16_t  rep0[2];                        /* rep0 (reuse-last-distance) prior */
} up_TokModels;

/* bias the first `depth` unary-prefix positions of a gamma model toward "continue" (bit 1). */
static inline void rc_ugg_seed_cont(up_UGGamma*g,int depth){ rc_seed_cont_u(g->u,UP_UG_CTX,depth); }

/* pre-kd apply-phase init: neutral models + the structural seed_cont priors (PG2/GDL/GADJ). */
static inline void rc_init_prekd(up_PreKdModels*m){
    up_bt_init(&m->dval);
    up_idx_init(&m->dibl,RC_IDX_SEED); up_idx_init(&m->diex,RC_IDX_SEED);
    rc_ugg_init(&m->pg); rc_ugg_init(&m->pgn);
    rc_ugg_init(&m->pg2); rc_ugg_seed_cont(&m->pg2,RC_SEED_DEPTH_PG2);
    rc_ugg_init(&m->gdl); rc_ugg_init(&m->gel); rc_ugg_init(&m->gadj);
    rc_ugg_seed_cont(&m->gdl,RC_SEED_DEPTH_GDL); rc_ugg_seed_cont(&m->gadj,RC_SEED_DEPTH_GADJ);
}
/* token-loop init: distance/out rice (kd/ko), length gammas, out-enable bit, flag, rep0 prior. */
static inline void rc_init_tok(up_TokModels*m,int kd,int ko){
    rc_ugr_init(&m->gd,kd); rc_ugr_init(&m->go,ko);
    rc_ugg_init(&m->gl); rc_ugg_seed_cont(&m->gl,RC_SEED_DEPTH_GL);
    rc_ugg_init(&m->gs); rc_ugg_init(&m->glo);
    m->outb=RC_PHALF;
    up_fl_init(&m->flag);
    m->rep0[0]=m->rep0[1]=RC_REP0_INIT;
}

/* RC_ALWAYS_INLINE / RC_NOINLINE / RC_NORETURN / RC_ADD_OVERFLOW / RC_SUB_OVERFLOW are the shared
 * portability shim. They persist out of this header for the encoder TUs (which include rc_models.h
 * directly); the decoder public header patch_apply.h seals them at its end so they never leak to
 * integrators. */

#endif
