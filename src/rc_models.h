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
 * range-coder probabilities (p[0..254]). Probabilities are always in 1..4095, so the decoder
 * packs them instead of spending a full uint16_t per node. The adaptation-shift rate is NOT stored
 * per-tree (saves SRAM): every call site passes its constant rate explicitly (lit0=5, lit1=4,
 * dval=4) — kept identical in patch_apply and patch_generate for wire-exactness. */
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
 * corpus optimum. (Decoder idx_init / encoder idx_init_e.) */
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

/* Initial rice k for the distance model M_gd, used ONLY to code the leading token count (nseq)
 * before k is reset from the header kd field. (Decoder ugr_init(&M_gd,.) / encoder ug_init_e(&gd,'r',.).) */
#define RC_GD_INIT_K 11

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
#define RC_JSLOTS_DEFAULT      1024u  /* packed journal capacity (home-corpus peak 478; out-of-corpus headroom) */
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
