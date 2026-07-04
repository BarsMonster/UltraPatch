/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 */

/* A1 divide-free range-coder model definitions (shared by decoder + host encoder).
 * Binary range coder (LZMA bound: bound=(range>>12)*prob; compare) — NO division anywhere,
 * required for Cortex-M0+/ARMv6-M (no hardware divide). Header-only; both
 * src/patch_apply.h (device decoder) and src/patch_generate.c
 * (host encoder) carry their own range-coder bit I/O and only share the model
 * struct/constants below, so the wire stays bit-exact between the two. */
#ifndef RC_MODELS_H
#define RC_MODELS_H
#include <stdint.h>
#include <string.h>

/* ---- target-family wire contract ----
 * The A1 wire is target-family-specific. CORTEX_M0 (Thumb-1/ARMv6-M, the implemented
 * family) must be defined for BOTH the encoder and the decoder TU — this header is
 * included by both, so a missing define fails BOTH builds and an encoder/decoder pair
 * can never silently disagree about the family. CORTEX_M4 is RESERVED for a future
 * Thumb-2 wire revision and MAY change the wire format (accepted by design). */
#if !defined(CORTEX_M0) && !defined(CORTEX_M4)
#error "define CORTEX_M0 for both the encoder and the decoder build (CORTEX_M4 is reserved)"
#endif
#ifdef CORTEX_M4
#error "CORTEX_M4 is reserved for a future wire revision; only CORTEX_M0 is implemented"
#endif

#define RC_KTOP (1u<<24)
#define RC_PBIT 4096u
#define RC_PHALF 2048u

/* ---- 256-symbol byte via 8-level bit-tree; logical probs[1..255] are stored as 12-bit
 * range-coder probabilities (p[0..254]). Probabilities are always in 1..4095, so they pack into
 * 12 bits/node instead of a full uint16_t; this struct + accessors are the ONE byte-tree
 * implementation, used by both patch_apply and patch_generate. The adaptation-shift rate is NOT
 * stored per-tree (saves SRAM): every call site passes its constant rate explicitly (RC_LIT0_RATE,
 * RC_LIT1_RATE, RC_DVAL_RATE — single-sourced below) — kept identical on both sides for wire-exactness. */
#define BT_PROBS 255u
#define BT_BYTES (((BT_PROBS * 12u) + 7u) / 8u)
typedef struct { uint8_t p[BT_BYTES + 1u]; } BitTree;  /* +1 lets accessors read/write 3 bytes */
static inline uint16_t bt_get(const BitTree*t,int idx){
    uint32_t bit=(uint32_t)idx*12u, off=bit>>3, sh=bit&7u;
    uint32_t v=(uint32_t)t->p[off] | ((uint32_t)t->p[off+1u]<<8) | ((uint32_t)t->p[off+2u]<<16);
    return (uint16_t)((v>>sh)&0xfffu);
}
static inline void bt_set(BitTree*t,int idx,uint16_t prob){
    uint32_t bit=(uint32_t)idx*12u, off=bit>>3, sh=bit&7u;
    uint32_t v=(uint32_t)t->p[off] | ((uint32_t)t->p[off+1u]<<8) | ((uint32_t)t->p[off+2u]<<16);
    v=(v&~(0xfffu<<sh)) | (((uint32_t)prob&0xfffu)<<sh);
    t->p[off]=(uint8_t)v; t->p[off+1u]=(uint8_t)(v>>8); t->p[off+2u]=(uint8_t)(v>>16);
}
static inline void bt_init(BitTree*t){ memset(t->p,0,sizeof t->p); for(int i=0;i<(int)BT_PROBS;i++) bt_set(t,i,RC_PHALF); }

/* ---- seeded Golomb context clamp (Rice/Gamma length & dist models) ----
 * UG_CTX = context clamp. A1 uses 6: the model array is sized at (UG_CTX+1)^2 u16.
 * ENCODING-AFFECTING: patch_apply and patch_generate must use the same value. */
#define UG_CTX 6
#define UG_C(x) ((x)<UG_CTX?(x):UG_CTX)

/* ---- piecewise shift map (BL/EX delta prediction) ----
 * Shipped per patch: ascending u32 boundaries + int32 byte-shift values; keys below the first
 * boundary (or an absent map) predict shift 0. ENCODING-AFFECTING: the decoder rejects
 * (REJ_RESOURCE) above SMAP_CAP, and the encoder never emits more entries than this. */
#define SMAP_CAP 48

/* ---- tag0 literal-tree context map: previous literal byte -> tree id. Re-derived 2026-07 by
 * agglomerative clustering of conditional literal histograms over surviving span literals
 * (corpus + one-face fixtures). ENCODING-AFFECTING: patch_apply and patch_generate share this
 * table (bit-exact wire). LIT0_CTX must equal 1 + max entry. ---- */
#define LIT0_CTX 5
static const uint8_t LIT0_MAP[256] = {
    1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,1,0,1,0,
    2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,
    3,1,3,3,3,3,3,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,3,1,3,1,3,1,3,0,3,1,
    3,3,3,3,3,3,3,0,3,3,3,0,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,0,3,3,3,0,
    3,3,3,3,0,1,1,1,3,3,3,1,1,0,0,1,2,2,2,2,1,1,2,1,2,2,2,2,1,1,2,1,
    0,0,0,0,0,0,0,0,2,1,0,1,0,0,1,1,0,0,0,0,0,1,0,0,0,0,1,0,0,1,1,0,
    0,1,0,0,0,0,0,1,0,0,0,0,0,1,1,0,1,1,1,1,1,1,0,0,1,1,1,3,1,1,0,0,
    0,0,0,0,0,0,0,0,0,1,1,0,1,0,0,0,2,1,0,1,0,1,0,4,0,0,0,0,0,0,0,0,
};
#define LIT0_SEL(p) (LIT0_MAP[(uint8_t)(p)])

/* ---- order-2 token flag: 4 contexts (previous 2 flags) ---- */
typedef struct { uint16_t m[4]; int h; } Flag1;
static inline void fl_init(Flag1*f){ for(int i=0;i<4;i++) f->m[i]=RC_PHALF; f->h=0; }

/* =====================================================================================
 * Shared PURE wire semantics — hand-mirrored decoder<->encoder helpers single-sourced here so
 * the COMPILER (not a "must match" comment) enforces the mirror, exactly as bt_/fl_ above already
 * do. Each is used verbatim by BOTH src/patch_apply.h (device decoder) and the host encoder; all
 * are 64-bit-free and (the one-time literal-seed rounding divide aside) divide-free, and preserve
 * the exact dataflow so the Cortex-M0+ decoder codegen does not shift.
 * ===================================================================================== */

/* adaptive-bit probability update: nudge p toward 0 (bit=1) or RC_PBIT (bit=0) by 1/2^rate.
 * Shared by decoder s_bit_r and encoder re_bit. */
static inline uint16_t rc_adapt(uint32_t p, int bit, int rate){
    return (uint16_t)(bit ? p-(p>>rate) : p+((RC_PBIT-p)>>rate));
}

/* histogram-seeded literal-tree node prob: rounded (2*RC_PBIT*num)/(2*den) clamped to 1..RC_PBIT-1
 * (an empty context degenerates to RC_PHALF). The rounding divide is one-time header init (software-
 * divided on M0+; the ARM gate pins exactly one soft-divide call). Shared by decoder
 * lit_tree_from_hist and encoder lit_tree_seed_e. */
static inline uint16_t rc_lit_seed_prob(uint32_t num, uint32_t den){
    uint32_t pr = den ? (2u*RC_PBIT*num+den)/(2u*den) : RC_PHALF;
    return (uint16_t)(pr<1 ? 1 : (pr>RC_PBIT-1 ? RC_PBIT-1 : pr));
}

/* Thumb BL/BLX local-branch halfword pattern (F000 / D000), tested on pristine source bytes. */
static inline int rc_bl_pattern(uint16_t up, uint16_t lo){
    return ((up&0xf800u)==0xf000u) && ((lo&0xd000u)==0xd000u);
}
/* unpack the 24-bit BL immediate (halfword units), RAW (unsigned, not yet sign-extended). */
static inline uint32_t rc_bl_imm24(uint16_t up, uint16_t lo){
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

/* Thumb LDR-literal target byte address from the instruction address (halfword-scanned, rounded
 * back to 4-alignment) + imm8 word field: (addr & ~3) + 4*imm8 + 4. Signed to match the op-local
 * field cursors. */
static inline int32_t rc_ldr_target(int32_t addr, int32_t imm8){
    return (addr & ~3) + 4*imm8 + 4;
}

/* piecewise shift-map lookup: value of the last boundary <= x, else 0 (also 0 for an empty map). */
static inline int32_t rc_smap_at(const uint32_t*b, const int32_t*v, int n, uint32_t x){
    int lo=0, hi=n-1, r=-1;
    while(lo<=hi){ int mid=(lo+hi)>>1; if(b[mid]<=x){ r=mid; lo=mid+1; } else hi=mid-1; }
    return r<0 ? 0 : v[r];
}

/* MTF dict-index UNARY model: IDX_CTX per-position priors (min(pos,IDX_CTX-1) clamp). The encoded
 * index is ~54% zero, so unary fits the concentration; IDX_CTX=5 is the corpus optimum (4/6/8/16 all
 * code worse). Shared struct + seed; the encode/decode loops live in each side. */
#define IDX_CTX 5
typedef struct { uint16_t u[IDX_CTX]; } IdxUnary;
static inline void idx_init(IdxUnary*g,uint16_t seed){ for(int i=0;i<IDX_CTX;i++) g->u[i]=seed; }

/* =====================================================================================
 * Shared wire constants — single-sourced so the COMPILER (not a "must match" comment)
 * enforces the encoder/decoder mirror. Each macro below is used verbatim by BOTH
 * src/patch_apply.h (device decoder) and src/patch_generate.c (host encoder); changing a
 * value here moves both sides at once, keeping the range-coded wire bit-exact.
 * ===================================================================================== */

/* Adaptive-bit rate (probability-update shift) for the Golomb (unary+mantissa), order-2 flag,
 * and MTF rep/hit streams. Tuned to 4 (1/16): these low-cardinality, fast-drifting contexts
 * track better at 4 than the LZMA-classic 5. The literal/dval bit-trees keep their OWN per-tree
 * rate, passed per call (see the BitTree note above), so only this shared default lives here. */
#define RC_S_BIT_RATE 4

/* Per-tree adaptation-shift rates for the literal + dval byte-trees. These bit-trees carry their
 * OWN rate (not stored per-tree — see the BitTree note above — but passed at every s_bt/bt_encode
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
 * ~1/4 (3/8 helps corpus more but regresses the real one-face update by +1/+1). Mirrors the
 * decoder M_rep0 and the encoder Models.rep0/last_dist. */
#define RC_REP0_INIT (RC_PBIT - (RC_PBIT>>2))

/* MTF dict-hit bit seed (DR_BL/DR_EX): a zero-seeded MTF dict makes the hit-bit==1 likely, so seed
 * the adaptive hit model high. 576 = tuned corpus optimum. (Decoder dr_init / encoder dr_init_e.) */
#define DR_HIT_INIT 576u

/* MTF dict-index UGolomb seed (dibl/diex): seed every unary-prefix prior toward STOP (idx 0) so the
 * just-promoted index 1 (encoded value 0), which dominates, is cheap from the first symbol. 2816 =
 * corpus optimum. (Shared idx_init seeds both sides.) */
#define RC_IDX_SEED 2816u

/* seed_cont depths: bias the first N unary-prefix positions of a gamma model toward "continue"
 * (bit 1). Structural priors (format invariants), NOT corpus caps — they make the very first op as
 * cheap as the warmed-up state. Mirrored by decoder ugg_seed_cont / encoder ug_seed_cont_e.
 *   GDL  = per-op diff_len gamma; op magnitudes are essentially never tiny.
 *   GADJ = per-op adj gamma.
 *   PG2  = rest preserve/corr gaps are strictly-increasing distinct offsets => gap>=1.
 *   GL   = match length gamma; matches are always len>=3 (value>=2 => cl>=1). */
#define RC_SEED_DEPTH_GDL  6
#define RC_SEED_DEPTH_GADJ 3
#define RC_SEED_DEPTH_PG2  1
#define RC_SEED_DEPTH_GL   1

/* Out-match minimum length: out-match lengths ship as (len - RC_OUTMATCH_MIN) via the M_glo gamma,
 * so the smallest representable out-match is RC_OUTMATCH_MIN bytes. */
#define RC_OUTMATCH_MIN 4u

/* Header raw k-field width: the distance rice parameter kd and (when out-matches are enabled) the
 * out-position rice parameter ko each ship as a fixed RC_KFIELD_BITS-bit raw field. */
#define RC_KFIELD_BITS 4

/* ---- decoder resource-cap / window DEFAULTS. ONE default site; each TU keeps its own
 * -D-overridable knob (decoder JSLOTS, OPC_CAP, DR_KCAP_BL/EX, SA_W; encoder A1_JSLOTS, A1_OPC_CAP, PATHE_W)
 * that falls back to the value here. BUILD CONTRACT: a deployment that -D-retunes the decoder caps
 * must retune the encoder mirrors identically, or the round-trip breaks. Caps are corpus-peak +
 * margin; over-cap input is REJECTED on-device (CRC-gated, never silent-wrong), not applied. ---- */
#define RC_JSLOTS_DEFAULT      768u   /* journal slot capacity (home-corpus peak 478; out-of-corpus headroom) */
#define RC_OPC_CAP_DEFAULT     80     /* per-op correction cap (corpus peak 68; +12 margin) */
#define RC_DR_KCAP_BL_DEFAULT  208    /* max distinct bl delta values (corpus peak 180; +28 margin) */
#define RC_DR_KCAP_EX_DEFAULT  128    /* max distinct ex delta values (corpus peak 106; +22 margin) */
#define RC_WINDOW_LOG_DEFAULT  10     /* LZSS window log2: ring 2^W. W=10 (ring 1024) keeps the decoder within 12 KiB SRAM */

/* NVM row-window DEFAULTS (row size x depth). The decoder window may legitimately be a SUPERSET of
 * the encoder's assumption (monotone compatibility — see docs/device-integration.md "NVM row
 * window"), so decoder OUTROW/OUTROW_DEPTH and encoder A1_OUTROW/A1_ROW_DEPTH stay SEPARATELY
 * overridable; only their shared DEFAULT lives here. */
#define RC_OUTROW_DEFAULT      256u
#define RC_ROW_DEPTH_DEFAULT   2u

#endif
