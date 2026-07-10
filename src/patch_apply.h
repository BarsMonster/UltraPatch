/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 */

#ifndef UP_PATCH_APPLY_H
#define UP_PATCH_APPLY_H
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
 * wire and the decoder zero-pads the range coder after the header's compressed byte count, so there
 * is no trailer or withhold ring. A single divide-free binary range coder (LZMA bound; no
 * division — Cortex-M0+ has no HW divide) decodes ONE interleaved stream whose symbols are
 * emitted in decode-consumption order (no length prefixes, no byte-aligned sections).
 *
 * Reconstruction is NO-BAKE (A1): NO source writes. The [A] copy reads RAW from[fp]; the new image
 * is corrected at the monotonic output frontier by the to-ordered additive corrections [C]
 * (out[tp] = db + raw_from[fp] + corr[tp]). Relocation fields are de-relocated on the fly: bl
 * positions are DERIVED by a local halfword pattern; ldr positions are DERIVED per op (a field is
 * ldr iff an ldr-literal instruction in the same op's copy range targets it) — so positions are
 * never shipped, only the per-field delta VALUES, pulled inline from the single stream (adaptive
 * MTF dict + order-1/zero-context repeat bit). A copy that reads a from-byte already overwritten by
 * the output frontier reads it from the never-evict journal, driven by the preserve events [P]. Output is
 * staged in a monotonic 256 B page write-back cache so each NVM page is erased+programmed exactly
 * once (no write amplification).
 *
 * RAM (hard gate <=12 KiB SRAM): entropy models + the never-evict journal + the LZSS history ring
 * + the packed FWD ldr-target metadata; the image lives ONLY in flash (0 image bytes in RAM).
 *
 * Embedded target: NO 64-bit integers ANYWHERE in the decoder — all positions/sizes are 32-bit and
 * every bounds/overflow guard is done in 32-bit (headroom tests plus __builtin_add/sub_overflow), so
 * the ARM object emits no libgcc 64-bit (or float) helpers, only one 32-bit divide at init.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "rc_models.h"

/* Inline/noinline/overflow attribute macros come from the shared portability shim in
 * rc_models.h (RC_ALWAYS_INLINE / RC_NOINLINE / RC_ADD_OVERFLOW / RC_SUB_OVERFLOW). */

/* ===================================================================================== */
/* flash model: the image lives in "flash", accessed ONLY through these. On the host test  */
/* harness flash is a buffer; on the device these are the real flash page primitives. Both */
/* receive absolute device addresses. One page write erases and programs the complete page. */
/* The decoder keeps 0 logical image bytes in RAM.                                          */
/* ===================================================================================== */
#ifndef PATCH_IMAGE_BASE
#error "define PATCH_IMAGE_BASE as the absolute device address of the patchable image"
#endif
extern uint8_t flash_read(uint32_t absolute_addr);
extern void    flash_write_page(uint32_t absolute_page_addr, const uint8_t page[OUTROW]);

static uint8_t up_image_flash_read(uint32_t image_offset){
    return flash_read((uint32_t)(PATCH_IMAGE_BASE + image_offset));
}

/* Decoder state is owned by the integrator and passed to patch_apply_run(). The header keeps no
 * file-scope storage: embedded users may place this object in .bss, noinit, a bootloader-owned
 * work area, or on a suitably sized stack. */
typedef struct { uint32_t range, code; } up_RangeDec;

/* STREAMED-DELTA per-stream state up_DRStream (bl, ex) is single-sourced in rc_models.h: a
 * MOVE-TO-FRONT dict of distinct delta values + an adaptive "repeat-last" bit + an adaptive
 * "dict-hit" bit. Each delta on the wire: rep-bit (==last?) | else hit-bit (in dict? -> MTF index
 * via the index UGolomb | else escape value as zigzag uLEB via M_dval, MTF-inserted at front). The
 * dict array is outside the struct so bl/ex get separate caps (distinct-value peaks: bl 180, ex 106). */

/* JSLOTS is configured above. Encoder and decoder builds MUST use the same macro name/value; the
 * encoder plans against that budget and DEGRADES over-budget plans host-side (over-budget reads
 * ship as extra bytes), so a valid blob never exceeds it; an over-cap stream still refuses cleanly
 * here (REJ_RESOURCE). */
#define UP_JREGION (JSLOTS*4u)                   /* journal byte region: JSLOTS uint32 slots (3072 B) */
/* LZSS window W (defined here so up_ApplyState can size the apply phase). Default value above
 * keeps the decoder within the 12 KiB SRAM cap; encoder and decoder builds MUST use the same
 * WINDOW_LOG value so distance coding uses the exact same window. */
/* WINDOW_LOG is configured above. */
#define UP_RING (1u<<WINDOW_LOG)
#define UP_MASK (UP_RING-1u)
/* up_ApplyState = the whole apply-phase working set: the LZSS content-history ring (2^W) + the count-bounded
 * per-op correction array + the streaming scalars/cursors. */
typedef struct {
    uint8_t ring[UP_RING]; uint32_t ototal;       /* content history (masked) + total produced */
    uint32_t tok_left, tok_src;                   /* token replay count + ring/output source */
    int32_t tp, fp;                               /* running accumulators (apply order) */
    uint32_t op_corr[OPC_CAP]; int32_t op_nc;     /* high 24 bits = offset, low 8 = correction */
    uint8_t tok_mode;                             /* 0=idle, 1=span, 2=backref, 3=out-match */
    uint8_t last_span;                            /* previous token was a span (flag implicit) */
} up_ApplyState;
#define UP_ARENA_BYTES (UP_JREGION + (uint32_t)sizeof(up_ApplyState))
/* One ARENA, two disjoint phase lifetimes overlaid as a union. */
typedef union {
    struct { uint32_t hist0[256], hist1[256], w[512]; } seed;
    struct { uint32_t jbuf[JSLOTS]; up_ApplyState sa; } apply;
} up_Arena;

typedef struct PatchApply {
    uint32_t g_image_span;
    uint32_t g_from_size, g_to_size, g_body_left;
    uint32_t g_want_to;
    int      g_FWD;

    int (*g_pull_fn)(void*, uint8_t*);
    void *g_pull_ctx;
    /* HOT-FLAG PLACEMENT INVARIANT: these decode error/EOF/reject latches are touched on nearly
     * every range-coder step, so they are packed into the low alignment hole right after the two
     * pointers (offsets ~32..35) instead of being sunk to the struct tail. Thumb-1 ldrb/strb take
     * only a 5-bit immediate offset, so a flag byte at a LOW struct offset stays reachable cheaply,
     * whereas a tail byte forces an extra address add. Keep any future hot flag byte HERE (low), not
     * appended at the end: a naive "all loose bytes at the tail" layout was measured at +72 B .text. */
    uint8_t g_pull_eof;
    uint8_t g_rcerr;
    uint8_t g_reject;   /* only REJ_RESOURCE cap sites set this during decode; REJ_CORRUPT is
                         * assigned once at the patch_apply_run boundary for every other failure */
    uint8_t g_flash_touched;

    up_RangeDec RC;
    /* g_jcount + g_smap_n pair fills RC's 4-byte tail up to the ARENA boundary (both uint16). */
    uint16_t g_jcount;
    uint16_t g_smap_n;

    up_Arena ARENA;

    uint32_t g_smap_b[UP_SMAP_CAP];
    int32_t  g_smap_v[UP_SMAP_CAP];

    /* g_orow_dirty (the 2 flag bytes) leads g_orow_buf so g_orow_base lands 4-aligned with no gap;
     * the old order left a 2-byte hole before the 4-aligned MDL_tok. */
    uint8_t  g_orow_dirty[OUTROW_DEPTH];
    uint8_t  g_orow_buf[OUTROW_DEPTH][OUTROW];
    uint32_t g_orow_base[OUTROW_DEPTH];

    up_TokModels   MDL_tok;    /* gd/go/gl/gs/glo/outb/flag/rep0 (rc_init_tok, rc_models.h) */
    uint32_t g_oexp;
    int g_rep0h;
    uint32_t g_lastdist;
    up_PreKdModels MDL_pre;    /* dval/dibl/diex/pg/pgn/pg2/gdl/gel/gadj (rc_init_prekd, rc_models.h) */
    up_BitTree M_lit0[UP_LIT0_CTX], M_lit1;
    int32_t DR_DIC_BL[DR_KCAP_BL], DR_DIC_EX[DR_KCAP_EX];
    up_DRStream DR_BL, DR_EX;

    uint32_t g_psrc_ldr[8];
    uint32_t g_psrc_even;     /* aligned word leads; the 3 loose flag bytes below pack into the
                               * struct's final tail (only 1 byte of end padding then remains). */
    uint8_t  g_psrc_lo;
    uint8_t  g_out_en;
    uint8_t  g_litprev;
} PatchApply;

_Static_assert(sizeof(up_Arena) == UP_ARENA_BYTES, "arena union size drifted from UP_ARENA_BYTES (.bss would change)");
_Static_assert(offsetof(up_Arena, apply.sa) == UP_JREGION && (offsetof(up_Arena, apply.sa) & 3u)==0u,
               "up_ApplyState must sit at UP_JREGION and be >=4-aligned (it holds uint32 fields)");
_Static_assert(MAX_IMAGE <= 0x7fffffffu, "MAX_IMAGE must stay < 2^31 (int32 tp/fp cursors, 32-bit overflow guards)");
_Static_assert(JSLOTS <= 65535u, "JSLOTS must fit the uint16 journal counters");
_Static_assert(DR_KCAP_BL <= 65535u && DR_KCAP_EX <= 65535u, "DR_KCAP_* must fit uint16 up_DRStream.K (else the REJ_RESOURCE refuse wraps)");

/* Patch geometry — decoder-OWNED. up_decode_body parses it from the blob envelope; the
 * integrator no longer supplies sizes/direction/span (footguns removed), it only provides
 * the two flash primitives above and pushes blob bytes. g_image_span = max(from,to) size
 * bounds every flash access. */
/* Feature 7B initial source seek: the source position where the ascending op walk BEGINS. Seeds
 * s->fp for FWD (was implicitly 0) and is the exact grow LANDING point (grow's final s->fp must
 * equal it). Carries a leading zero-output op's seek that was folded off the wire; almost always 0. */
/* MAX_IMAGE is configured above. */

/* ===================================================================================== */
/* byte source: the integrator supplies a (possibly blocking) callback and calls           */
/* patch_apply_run() — the whole decode runs on the caller's stack, plain calls only.      */
/* CRC32(to) rides in the header (g_want_to), so the range-coded body is the LAST thing on  */
/* the wire — NO trailer-withhold ring. The header carries the compressed body length;       */
/* up_next_byte() zero-fills after exactly that many body bytes, so a valid blob finishes       */
/* without asking the byte source for transport EOF. EOF from the callback is only the       */
/* truncation/abort path.                                                                    */
/* ===================================================================================== */
static int up_decode_body(PatchApply *pa);  /* 1 = body decoded; 0 = error (g_reject/g_rcerr hold why) */
/* The literal-seed histograms are real uint32_t arrays in the ARENA union's seed member, so the
 * reads/writes are ordinary typed lvalues — no may_alias / char-array reinterpretation needed. */
static void up_lit_tree_from_hist(up_BitTree*t,const uint32_t*hist,uint32_t*w);
enum { PATCH_APPLY_DONE=1, PATCH_APPLY_ERROR=2 };

/* one raw blob byte straight from the callback; EOF is latched so a spent stream stays spent
 * (later reads keep returning 0 without re-invoking the callback). */
static int up_pull_raw(PatchApply *pa, uint8_t*out){
    if(pa->g_pull_eof) return 0;
    if(!pa->g_pull_fn(pa->g_pull_ctx,out)){ pa->g_pull_eof=1; return 0; }
    return 1;
}
/* rob-1: distinguishable reject reason (read by the host main after a reject). 1 =
 * RESOURCE: a corpus-overfit cap was exceeded (journal full / per-op cap / dict cap) — this firmware
 * is bigger than the build was sized for, raise the cap (costs SRAM). 2 = CORRUPT: malformed/truncated
 * stream (bounds, underrun, range-coder overflow). 0 = none. Pure diagnostic; never affects decoding. */
enum { REJ_NONE=0, REJ_RESOURCE=1, REJ_CORRUPT=2 };

static uint8_t up_next_byte(PatchApply *pa){
    uint8_t b;
    if(pa->g_body_left==0) return 0;
    if(!up_pull_raw(pa,&b)){ pa->g_body_left=0; pa->g_rcerr=1; return 0; }
    pa->g_body_left--;
    return b;
}
static int up_env_byte(PatchApply *pa, uint8_t*out){ return up_pull_raw(pa,out); }             /* envelope: strict */

static int up_env_u32le(PatchApply *pa, uint32_t*out){
    uint32_t v=0; uint8_t b;
    for(int i=0;i<4;i++){ if(!up_env_byte(pa,&b)) return 0; v|=(uint32_t)b<<(8*i); }
    *out=v; return 1;
}
#define UP_ULEB32_OVERFLOW(sh,b) ((sh)==28 && (uint8_t)(b)>15u)
/* uLEB with a NON-CANONICAL-encoding flag: a canonical multi-byte uLEB never ends in a 0x00
 * byte (emission stops once the remaining value is zero; a lone 0x00 is the value 0), so one
 * redundant trailing continuation byte is a legal, value-neutral 1-bit side channel. The
 * envelope permits it ONLY on the size-delta field as the UNNATURAL-apply-direction marker. */
static int up_env_uleb(PatchApply *pa, uint32_t*out, uint8_t*ov){
    uint32_t v=0; int sh=0, n=0; uint8_t b;
    do{ if(!up_env_byte(pa,&b) || UP_ULEB32_OVERFLOW(sh,b)) return 0; v|=(uint32_t)(b&0x7fu)<<sh; sh+=7; n++; }while(b&0x80u);
    { uint8_t o=(uint8_t)rc_uleb_overlong(n,b); if(o && !ov) return 0; if(ov) *ov=o; }
    *out=v; return 1;
}
/* zigzag-uLEB absolute around `base` (to_size and fp_end are delta-coded vs from_size). */
static int up_env_zz_abs(PatchApply *pa, uint32_t base, uint32_t*out){
    uint32_t z; if(!up_env_uleb(pa,&z,0)) return 0;
    return rc_zz_abs(base,z,MAX_IMAGE,out);
}

/* divide-free range decoder reading through the (possibly blocking) byte source. */
static int up_rc_init(PatchApply *pa){
    pa->RC.range = 0xFFFFFFFFu; pa->RC.code = 0;
    /* The encoder drops the always-zero LZMA leading cache byte from the wire, so the
     * 4 code bytes are read directly (no skip). */
    for (int i=0;i<4;i++) pa->RC.code = (pa->RC.code<<8) | up_next_byte(pa);
    return !pa->g_rcerr;
}
/* range-coder DECODE core shared by both the adaptive bit and the raw (equiprobable) bit: split at
 * `bound`, pick the sub-interval the code lands in, return the bit, then renorm (refill from the FIFO
 * until range climbs back above KTOP). The readers differ ONLY in `bound` + probability adaptation. */
static int up_rc_decode(PatchApply *pa, uint32_t bound){
    uint32_t code=pa->RC.code, range=pa->RC.range;
    int b;
    if (code<bound){ range=bound; b=0; } else { code-=bound; range-=bound; b=1; }
    while (range<RC_KTOP){ code=(code<<8)|up_next_byte(pa); range<<=8; }
    pa->RC.code=code; pa->RC.range=range;
    return b;
}
static int up_s_bit_r(PatchApply *pa, uint16_t*prob,int rate){
    uint32_t p=*prob;
    int b=up_rc_decode(pa,RC_PROB_BOUND(pa->RC.range,p));
    *prob=rc_adapt(p,b,rate);
    return b;
}
/* RC_S_BIT_RATE: shared Golomb / order-2 flag / MTF rep+hit adaptation rate.
 * literal/dval bit-trees keep their own per-tree rate via up_s_bit_r. */
static int up_s_bit(PatchApply *pa, uint16_t*prob){ return up_s_bit_r(pa,prob,RC_S_BIT_RATE); }
static int up_s_raw(PatchApply *pa){ return up_rc_decode(pa,pa->RC.range>>1); }
/* CRASH-HARDENING (fuzz gate): a corrupt/truncated stream yields zero-fill past EOF,
 * which can drive the unbounded unary loops below forever (hang) or shift a value by >=32 bits
 * (UB). Every unbounded loop is capped to the max a 32-bit value needs; on overflow set g_rcerr
 * and bail. g_rcerr is the ONE decode error latch — RC-level and apply-level failures all set it
 * (clean reject, never crash / never silent-wrong); g_reject still carries the reason. */
/* terminal-state flag: set at the single flash_write_page site (up_orow_commit_slot's dirty branch);
 * on ERROR it tells the integrator whether the old image is still intact. */
#define UP_RC_UNARY_MAX 31           /* a uint32 value needs <=31 leading/unary bits */
static uint32_t up_s_raw_bits(PatchApply *pa, int nb){ uint32_t v=0; for(int i=0;i<nb;i++) v=(v<<1)|(uint32_t)up_s_raw(pa); return v; }
/* ---- bit-tree byte ---- */
static int up_s_bt(PatchApply *pa, up_BitTree*t,int rate){
    int m=1;
    for(int i=0;i<8;i++){
        uint16_t p=up_bt_get(t,m-1);
        int b=up_s_bit_r(pa,&p,rate);
        up_bt_set(t,m-1,p);
        m=(m<<1)|b;
    }
    return m-256;
}
/* ---- MTF escape value: zigzag uLEB, each byte through the adaptive M_dval bit-tree ---- */
static int32_t up_s_bv(PatchApply *pa, up_BitTree*t,int rate){
    uint32_t acc=0; int sh=0, b;
    do{
        b=up_s_bt(pa,t,rate);
        if(UP_ULEB32_OVERFLOW(sh,b)){ pa->g_rcerr=1; return 0; }
        acc|=(uint32_t)(b&0x7f)<<sh; sh+=7;
    }while(b&0x80);
    return rc_unzz32_value(acc);
}
/* ---- UGolomb (uses embedded UP_UG_CTX mirror) ----
 * Crash-hardening: cap the adaptive unary prefix. For GAMMA ('g') the prefix is the bit-length
 * of (value+1), so <=31 for any uint32 (UP_RC_UNARY_MAX). For RICE ('r') the prefix is the QUOTIENT
 * value>>k, which for a legit field (e.g. backref_dist <= window 2^WINDOW_LOG with no-split, WINDOW_LOG default
 * 10 (see patch_config.h) => up to ~32 at small k) can far exceed 31 — cap it much higher (1<<20) so valid streams decode
 * while a corrupt run-on is still bounded (the mantissa shift below caps the magnitude anyway).
 * The neutral rc_ugr_init/rc_ugg_init init helpers are single-sourced in rc_models.h (compact gamma
 * mantissa: rows 1..UP_UG_CTX-1 keep only reachable columns, the clamped row keeps all UP_UG_CTX+1). */
/* the unary-prefix "continue"-seed helper (per-op geometry: firmware delta op magnitudes are
 * essentially never tiny, a structural prior that makes the very first op as cheap as the warmed-up
 * state) is single-sourced as rc_ugg_seed_cont in rc_models.h, folded into rc_init_prekd/rc_init_tok. */
/* shared adaptive unary prefix for BOTH the Golomb (Rice/Gamma) prefix and the MTF dict-index code:
 * read 1-bits on the per-position priors u[min(pos,clampmax)] until a 0-bit, `cap`-bounded against a
 * corrupt run-on so a zero-fill stream can't spin forever / shift by >=32 (UP_RC_UNARY_MAX for gamma,
 * RC_RICE_UNARY_MAX for rice); on overflow it sets g_rcerr and returns 0 so the mantissa loop is a
 * no-op and the apply cleanly rejects. clampmax==UP_UG_CTX(=6) reproduces UP_UG_C over the 7-entry Golomb
 * u[]; clampmax==UP_IDX_CTX-1 (=4) reproduces the index code's min(pos,4) over the 5-entry u[]. */
static uint32_t up_s_unary(PatchApply *pa, uint16_t*u,uint32_t clampmax,uint32_t cap){
    uint32_t cl=0; while(up_s_bit(pa,&u[cl<clampmax?cl:clampmax])==1){ if(++cl>cap){ pa->g_rcerr=1; return 0; } }
    return cl;
}
/* shared Rice mantissa: read `cnt` adaptive bits MSB-first (significance-descending) from priors row[].
 * The ctx is anchored on significance from the LSB (UP_UG_C(sig)) so the LSB gets ctx 0 and the wide
 * MSBs share the clamp. Bit order on the wire stays MSB-first. */
static uint32_t up_s_ug_mant(PatchApply *pa, uint16_t*row,int cnt){
    uint32_t v=0; for(int sig=cnt-1;sig>=0;sig--) v|=(uint32_t)up_s_bit(pa,&row[UP_UG_C(sig)])<<sig;
    return v;
}
static uint32_t up_s_ug_mant_gamma(PatchApply *pa, up_UGGamma*g,int cnt){
    uint32_t v=0; int row=UP_UG_C(cnt);
    for(int sig=cnt-1;sig>=0;sig--) v|=(uint32_t)up_s_bit(pa,&g->m[rc_ugg_mant_idx(row,cnt-1-sig)])<<sig;
    return v;
}
static uint32_t up_s_ug_rice(PatchApply *pa, up_UGRice*g){
    uint32_t cl=up_s_unary(pa,g->u,UP_UG_CTX,RC_RICE_UNARY_MAX);
    if(pa->g_rcerr || (g->k && cl>(UINT32_MAX>>g->k))){ pa->g_rcerr=1; return 0; }
    return (cl<<g->k) | up_s_ug_mant(pa,g->m[UP_UG_C((int)cl)],g->k);
}
static uint32_t up_s_ug_gamma(PatchApply *pa, up_UGGamma*g){
    int cl=(int)up_s_unary(pa,g->u,UP_UG_CTX,UP_RC_UNARY_MAX);
    return ((1u<<cl) | up_s_ug_mant_gamma(pa,g,cl)) - 1u;
}
/* MTF dict-index model: a lean adaptive UNARY code. UP_IDX_CTX / up_IdxUnary / up_idx_init are single-sourced
 * in the encoder mirror. The encoded index value v (== dict pos j-1) is ~54% zero, ~22% one,
 * ~10% two, with a thin tail to ~140 worst case: emit v continue-bits then a stop-bit on the per-position
 * prior u[min(pos,UP_IDX_CTX-1)]. `cap` bounds the run on a corrupt stream (up_pull_delta validates j vs K). */
/* ---- order-2 flag ---- */
static int up_s_flag(PatchApply *pa, up_Flag1*f){ int b=up_s_bit(pa,&f->m[f->h]); f->h=rc_fl_hist(f->h,b); return b; }

/* ===================================================================================== */
/* CRC32 (zlib polynomial) over flash bytes. The 256-entry byte table is built at runtime */
/* in ARENA.seed.w: scratch before literal seeding and dead apply state after final flush. */
/* No table has static storage and the mandatory final CRC still reads physical flash.    */
/* ===================================================================================== */
static void up_crc32_table_init(uint32_t*t){
    for(uint32_t i=0;i<256u;i++){
        uint32_t c=i;
        for(int k=0;k<8;k++) c=(c>>1)^(0xedb88320u & (uint32_t)(-(int32_t)(c&1)));
        t[i]=c;
    }
}
static uint32_t RC_NOINLINE up_crc32_flash(PatchApply *pa, uint32_t n){
    uint32_t *t=pa->ARENA.seed.w, c=0xffffffffu;
    up_crc32_table_init(t);
    for(uint32_t i=0;i<n;i++) c=(c>>8)^t[(c^(uint32_t)up_image_flash_read(i))&0xffu];
    return c^0xffffffffu;
}
static uint32_t up_crc32_flash_hist(PatchApply *pa, uint32_t n, uint32_t*hist0, uint32_t*hist1){
    uint32_t *t=pa->ARENA.seed.w, c=0xffffffffu;
    up_crc32_table_init(t);
    for(uint32_t i=0;i<n;i++){
        uint8_t v=up_image_flash_read(i);
        if(i&1u) hist1[v]++; else hist0[v]++;
        c=(c>>8)^t[(c^(uint32_t)v)&0xffu];
    }
    return c^0xffffffffu;
}

/* ===================================================================================== */
/* never-evict journal — FLAT SORTED uint32 slots, each packed (pos<<8)|byte (the op_corr      */
/* packing). Preserve positions are globally monotonic: ascending for FWD; grow stores each    */
/* ascending wire block into reverse destination indices, making the array globally descending. */
/* Lookup is one direction-aware binary search on slot>>8. RC_PACKED_POS_BITS positions span   */
/* 16 MiB. Over-depth (would exceed JSLOTS) is REFUSED before writing the op. Lives in the      */
/* apply phase ONLY (overlaid in ARENA front).                                                  */
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
/* Direction-aware binary search on slot>>8 over [0, g_jcount). */
static int up_jr_find(PatchApply *pa, uint32_t pos){
    int lo=0, hi=(int)pa->g_jcount-1;
    while(lo<=hi){ int mid=(int)(((unsigned)lo+(unsigned)hi)>>1);
        uint32_t k=pa->ARENA.apply.jbuf[mid]>>8;
        if(k==pos) return mid;
        if(pa->g_FWD ? k<pos : k>pos) lo=mid+1; else hi=mid-1; }
    return -1;
}
static int up_jr_get(PatchApply *pa, uint32_t pos, uint8_t*out){
    int at=up_jr_find(pa,pos);
    if(at>=0){ *out=(uint8_t)pa->ARENA.apply.jbuf[at]; return 1; }
    return 0;
}

/* ===================================================================================== */
/* relocation unpack/pack (Thumb bl + s32) — de-relocation override only (no flash write).      */
/* The journal-aware pristine reads + the bl/ldr predicates live with the apply state below.    */
/* ===================================================================================== */
/* ---- piecewise shift map (D1): predicts BL/EX de-reloc deltas from addresses/values the
 * decoder already has in hand; only the RESIDUAL (delta - pred) rides the MTF dict streams.
 * rc_smap_at(x) = value of the last segment with boundary <= x, else 0 (also 0 when no map).
 * Values are BYTE shifts; BL predictions divide by 2 (imm24 is in halfwords) with C
 * truncation-toward-zero on both sides (bit-exact vs patch_generate). ---- */

/* ===================================================================================== */
/* HYBRID: monotonic output PAGE write-back cache (real flash = page erase/program).        */
/* Output writes are monotonic & contiguous (asc shrink / desc grow); SOURCE reads stay raw. */
/* The journal covers read-after-overwrite; reads of dirty buffered pages come from RAM, and */
/* not-yet-overwritten source bytes come straight from flash. Each resident output page is   */
/* committed with one full-page call. The page buffers use OUTROW * OUTROW_DEPTH bytes.       */
/* ===================================================================================== */
/* OUTROW is configured above; encoder and decoder builds MUST use the same value. */
/* Page-window depth — keep the last OUTROW_DEPTH pages uncommitted. Monotonic writes touch
 * pages in strictly monotonic order, so a DIRECT-MAPPED ring keyed by (page_number % D) is an
 * exact FIFO: the slot's previous occupant is always the page exactly D pages behind,
 * committed on eviction. The point: OLD flash content of uncommitted pages stays physically
 * readable through up_hy_src_peek's physical flash read, and the ENCODER's row_covered oracle
 * exploits exactly that window (journal-free old reads behind the frontier).
 * ENCODING-AFFECTING BUILD CONTRACT: the encoder's row_covered oracle and decoder both use
 * OUTROW x OUTROW_DEPTH. Their builds MUST use the same macro names/values so the uncommitted-
 * window assumption matches. A hardware page-size change requires matching encoder and decoder
 * builds. A deeper ring is a monotone-safe superset; a smaller window rejects via CRC32(to), never
 * silent-wrong. */
/* OUTROW_DEPTH is configured above; encoder and decoder builds MUST use the same value. */
#define UP_OROW_NONE UINT32_MAX
#define UP_OROW_SLOT(base) (uint32_t)(((base)/OUTROW) % OUTROW_DEPTH)
/* UP_OROW_SLOT / up_out_read / up_out_write use /OUTROW and %OUTROW_DEPTH; both must stay powers of two so
 * they lower to shift/mask — a non-pow2 -D retune would pull libgcc divide helpers into the
 * Cortex-M0+ build (the gate tracks divide policy) and break the encoder's pow2-page contract. */
_Static_assert(OUTROW>0u && (OUTROW & (OUTROW-1u))==0u, "OUTROW must be a power of two");
_Static_assert(OUTROW_DEPTH>0u && (OUTROW_DEPTH & (OUTROW_DEPTH-1u))==0u, "OUTROW_DEPTH must be a power of two");
_Static_assert(MAX_IMAGE>0u, "MAX_IMAGE must be nonzero");
_Static_assert((PATCH_IMAGE_BASE & (OUTROW-1u))==0u,
               "PATCH_IMAGE_BASE must be aligned to OUTROW");
_Static_assert(PATCH_IMAGE_BASE <= UINT32_MAX-
               (((MAX_IMAGE-1u) & ~(OUTROW-1u))+(OUTROW-1u)),
               "PATCH_IMAGE_BASE leaves insufficient uint32_t address space for MAX_IMAGE");
static void up_orow_commit_slot(PatchApply *pa, uint32_t s){
    if(pa->g_orow_base[s]!=UP_OROW_NONE && pa->g_orow_dirty[s]){
        uint32_t base=pa->g_orow_base[s];
        pa->g_flash_touched=1;
        flash_write_page((uint32_t)(PATCH_IMAGE_BASE+base),pa->g_orow_buf[s]);
    }
    pa->g_orow_dirty[s]=0; pa->g_orow_base[s]=UP_OROW_NONE;
}
/* final flush: commit remaining slots in WRITE order (frontier monotonicity holds on NVM). */
static void up_orow_commit_all(PatchApply *pa){
    for(uint32_t i=0;i<OUTROW_DEPTH;i++){
        uint32_t best=UP_OROW_NONE, bs=0;
        for(uint32_t s=0;s<OUTROW_DEPTH;s++){
            uint32_t b=pa->g_orow_base[s];
            if(b==UP_OROW_NONE) continue;
            if(best==UP_OROW_NONE || (pa->g_FWD ? b<best : b>best)){ best=b; bs=s; }
        }
        if(best==UP_OROW_NONE) break;
        up_orow_commit_slot(pa,bs);
    }
}
static void up_orow_reset(PatchApply *pa){ for(uint32_t s=0;s<OUTROW_DEPTH;s++){ pa->g_orow_base[s]=UP_OROW_NONE; pa->g_orow_dirty[s]=0; } }
/* read one byte of ALREADY-PRODUCED output at absolute position a: an uncommitted page from
 * its RAM slot, anything else from flash. Valid streams only reference written positions; a
 * corrupt position yields stale flash bytes, which the CRC32(to) gate rejects. */
static uint8_t up_out_read(PatchApply *pa, uint32_t a){
    if(a>=pa->g_image_span) return 0;
    { uint32_t base=(a/OUTROW)*OUTROW, s=UP_OROW_SLOT(base);
      if(pa->g_orow_base[s]==base) return pa->g_orow_buf[s][a-base]; }
    return up_image_flash_read(a);
}
/* OUTPUT write: buffer in the page's slot, committing the slot's previous occupant (the page
 * exactly OUTROW_DEPTH pages behind) on a page change. */
static void up_out_write(PatchApply *pa, uint32_t a, uint8_t v){
    if(a>=pa->g_image_span) return;
    uint32_t base=(a/OUTROW)*OUTROW, s=UP_OROW_SLOT(base);
    if(base!=pa->g_orow_base[s]){
        up_orow_commit_slot(pa,s);
        for(uint32_t x=0;x<OUTROW;x++)
            pa->g_orow_buf[s][x]=up_image_flash_read(base+x); /* preload the complete physical page */
        pa->g_orow_base[s]=base;   /* up_orow_commit_slot() above already cleared the dirty flag */
    }
    { uint32_t off=a-base;
      if(pa->g_orow_buf[s][off]!=v){ pa->g_orow_buf[s][off]=v; pa->g_orow_dirty[s]=1; } }
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

/* pull the next delta of a stream (bl/ex), inline:
 *   rep-bit: ==1 -> return last; else read hit-bit:
 *     hit==1 -> MTF index via the index UGolomb (gix); v=dic[j]; move dic[j] to front.
 *     hit==0 -> escape value (zigzag uLEB via M_dval); insert v at MTF front.
 *   then last=v. */
static int32_t up_pull_delta(PatchApply *pa, up_DRStream*d, up_IdxUnary*gix, int32_t*dic, uint32_t cap){
    { int ri=rc_dr_rep_ctx(d->rh,dic[0]);
      int rb=up_s_bit(pa,&d->rep[ri]); d->rh=(uint8_t)rb; if(rb==1) return dic[0]; }  /* repeat-last, order-1 + zero ctx */
    int32_t v;
    if(up_s_bit(pa,&d->hit)==1){
        uint32_t j=up_s_unary(pa,gix->u,UP_IDX_CTX-1,cap)+1u;    /* cmp-1: dict idx 0 unreachable -> encode j-1 */
        if(j>=(uint32_t)d->K){ pa->g_rcerr=1; return 0; }
        v=dic[j];
        rc_mtf_promote_i32(dic,j);
    } else {
        v=up_s_bv(pa,&pa->MDL_pre.dval, RC_DVAL_RATE);
        if((uint32_t)d->K>=cap){ pa->g_rcerr=1; pa->g_reject=REJ_RESOURCE; return 0; }   /* distinct-value cap -> reject */
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
/* The LZSS ring (size 2^WINDOW_LOG) is the content history; backref distances <= 2^WINDOW_LOG.             */
/* ===================================================================================== */
/* up_ApplyState type + UP_RING/UP_MASK + the arena asserts live up with the ARENA
 * union (near UP_JREGION), since the union embeds up_ApplyState directly in its apply phase. */
RC_ALWAYS_INLINE int up_op_next_offset(PatchApply *pa, uint32_t *off, uint32_t idx, uint32_t nwu){
    uint32_t gap=up_s_ug_gamma(pa,idx?&pa->MDL_pre.pg2:&pa->MDL_pre.pg);
    if((idx && gap==0u) || gap>UINT32_MAX-*off){ pa->g_rcerr=1; return 0; }
    *off+=gap;
    if(*off>=nwu || *off>=RC_PACKED_POS_LIMIT){ pa->g_rcerr=1; return 0; }
    return 1;
}
typedef struct { int32_t i, step; } up_CorrCur;
static void up_corr_cur_init(up_CorrCur*c, up_ApplyState*s, int fwd){
    c->step=fwd?1:-1; c->i=fwd?0:s->op_nc-1;
}
static int32_t up_corr_take(up_ApplyState*s, up_CorrCur*c, int32_t off){
    if(c->i>=0 && c->i<s->op_nc){
        uint32_t slot=s->op_corr[c->i];
        if((int32_t)(slot>>8)==off){ c->i+=c->step; return (int32_t)(slot&0xffu); }
    }
    return 0;
}
/* A1 ldr-derive: a 256-bit ring records future 4-aligned literal-pool targets of Thumb
 * LDR-literal halfwords. The target span is <=1024 B, so target>>2 modulo 256 is collision-free
 * while every consumed target stays in the current 1 KiB window. FWD records halfwords as their
 * copy bytes are read. Grow walks the window backward: an initial <=1 KiB fill followed by only
 * the newly admitted low halfwords at each descending query. */
#define UP_PSRC_TGT_MASK 255u
static uint8_t up_hy_src_peek(PatchApply *pa, int32_t fp); /* fwd decl: journal-aware pristine source read */
/* A1 ldr-derive (SAME-OP): is the 4-aligned from-addr fpk an ldr literal target of an instruction
 * IN THIS op [fp0,fp0+dl)? Scan even a in [max(fp0,fpk-1024), fpk-2]; an ldr literal
 * `(up&0xf800)==0x4800` targets t=(a&~3)+4*(up&0xff)+4; field iff some a targets fpk and
 * fpk+4<=fp0+dl. Reads PRISTINE source: FWD consumes packed metadata recorded at copy-read time;
 * grow incrementally fills the same ring from journal-aware source reads while descending. */
static inline int up_psrc_ldr_take(PatchApply *pa, uint32_t fpk){
    uint32_t i=(fpk>>2)&UP_PSRC_TGT_MASK;
    uint32_t bit=1u<<(i&31u);
    int hit=(pa->g_psrc_ldr[i>>5]&bit)!=0;
    pa->g_psrc_ldr[i>>5]&=~bit;
    return hit;
}
static inline void up_psrc_ldr_put(PatchApply *pa, uint32_t fpk){
    uint32_t i=(fpk>>2)&UP_PSRC_TGT_MASK;
    pa->g_psrc_ldr[i>>5]|=1u<<(i&31u);
}
/* Descending SAME-OP LDR query. g_psrc_even is the lowest even instruction address already
 * scanned in this op (UINT32_MAX before the first query). Moving fpk down can only admit lower
 * instruction halfwords; high halfwords that leave the 1 KiB window cannot target fpk or below.
 * Targets above the current query are deliberately not inserted: they will never be consumed by
 * the descending walk and would alias a real slot exactly 1024 B below. */
static inline int up_grow_ldr_take(PatchApply *pa, int32_t fp0, int32_t dl, uint32_t fpk){
    if(!rc_ldr_target_in_op(fp0,dl,fpk)) return 0;
    uint32_t lo=(uint32_t)rc_ldr_scan_first(fp0,fpk);
    uint32_t a=pa->g_psrc_even==UINT32_MAX ? fpk : pa->g_psrc_even;
    while(a>lo){
        a-=2u;
        uint16_t up=(uint16_t)(up_hy_src_peek(pa,(int32_t)a) |
                               ((uint16_t)up_hy_src_peek(pa,(int32_t)a+1)<<8));
        if(rc_thumb_ldr_lit(up)){
            uint32_t t=(uint32_t)rc_ldr_target((int32_t)a,(int32_t)(up&0xffu));
            /* fpk is already an in-source SAME-OP field start, and LDR targets are 4-aligned;
             * therefore t<=fpk also proves t+4 is in both the source and this op. */
            if(t<=fpk) up_psrc_ldr_put(pa,t);
        }
    }
    pa->g_psrc_even=a;
    return up_psrc_ldr_take(pa,fpk);
}
/* g_litprev = the actual previous CONTENT-stream byte (order-1 tag0 literal context): updated for
 * EVERY emitted content byte here -- span literal, ring backref, out-match copy -- so the literal at
 * the next span selects M_lit0[rc_lit0_sel(content[pos-1])] regardless of which token produced pos-1.
 * BL/EX field bytes are de-reloc'd outside the content stream and correctly do not update it. */
static void up_emit_ring(PatchApply *pa, up_ApplyState*s, uint8_t b){ s->ring[s->ototal & UP_MASK]=b; s->ototal++; pa->g_litprev=b; }
/* pull the next CONTENT byte from the cut LZSS token stream, decoding tokens lazily. */
static uint8_t up_next_content(PatchApply *pa, up_ApplyState*s, int tag){
    for(;;){
        if(pa->g_rcerr) goto fail;
        if(s->tok_left>0){
            uint8_t b;
            if(s->tok_mode==1) b=(uint8_t)up_s_bt(pa,tag?&pa->M_lit1:&pa->M_lit0[rc_lit0_sel(pa->g_litprev)], tag?RC_LIT1_RATE:RC_LIT0_RATE);
            else if(s->tok_mode==2){ b=s->ring[s->tok_src & UP_MASK]; s->tok_src++; }
            else { b=up_out_read(pa,s->tok_src); s->tok_src+=(uint32_t)(pa->g_FWD?1:-1); }
            if(pa->g_rcerr) goto fail;
            up_emit_ring(pa,s,b); s->tok_left--;
            if(s->tok_left==0) s->tok_mode=0;
            return b;
        }
        /* adjacent spans never ship (the encoder merges them), so after a span the
         * span/match flag is IMPLICITLY "match": skip the coded bit but keep the order-2
         * flag history tracking the true token kinds (mirror emit_body last_span). */
        int is_match;
        if(s->last_span){ pa->MDL_tok.flag.h=rc_fl_hist(pa->MDL_tok.flag.h,1); is_match=1; }
        else is_match=up_s_flag(pa,&pa->MDL_tok.flag);
        if(!is_match){
            uint32_t ln=up_s_ug_gamma(pa,&pa->MDL_tok.gs)+1u;
            s->tok_mode=1; s->tok_left=ln; s->last_span=1;
        } else {
            uint32_t d;
            int rb=up_s_bit(pa,&pa->MDL_tok.rep0[pa->g_rep0h]); pa->g_rep0h=rb;
            if(rb){ d=pa->g_lastdist; }                         /* rep0: reuse last distance */
            else if(pa->g_out_en && up_s_bit(pa,&pa->MDL_tok.outb)){         /* fresh + out-bit: long-range OUTPUT match */
                int32_t dpos=rc_unzz32_value(up_s_ug_rice(pa,&pa->MDL_tok.go)); /* position as zigzag delta vs expected */
                uint32_t p=rc_outmatch_pos(pa->g_oexp,dpos);
                uint32_t ln=up_s_ug_gamma(pa,&pa->MDL_tok.glo)+RC_OUTMATCH_MIN;
                /* replay walks the output in WRITE direction (ascending FWD, descending grow) so
                 * grow content (whose extras are byte-reversed) still matches produced output. */
                if(pa->g_rcerr || p>=pa->g_image_span || (pa->g_FWD ? ln>pa->g_image_span-p : ln>p+1u)) goto fail;
                pa->g_oexp=rc_outmatch_next_expect(pa->g_FWD,p,ln);  /* sequential runs keep deltas tiny */
                s->tok_mode=3; s->tok_src=p; s->tok_left=ln; s->last_span=0;
                continue;
            }
            else { d=up_s_ug_rice(pa,&pa->MDL_tok.gd)+1u; pa->g_lastdist=d; }
            uint32_t ln=up_s_ug_gamma(pa,&pa->MDL_tok.gl)+1u;
            if(d==0 || d>s->ototal || d-1u>=UP_RING) goto fail; /* reject before-start / ring-overrun */
            s->tok_mode=2; s->tok_src=s->ototal-d; s->tok_left=ln; s->last_span=0;
        }
    }
fail:
    pa->g_rcerr=1;
    return 0;
}
/* read a uLEB from the content stream (one byte pulled per iteration from the LZSS token replay). */
static uint32_t up_read_uleb(PatchApply *pa, up_ApplyState*s){
    uint32_t acc=0; int sh=0;
    for(;;){ if(pa->g_rcerr) return 0;
        uint8_t b=up_next_content(pa,s,0);
        if(pa->g_rcerr) return 0;
        if(UP_ULEB32_OVERFLOW(sh,b)){ pa->g_rcerr=1; return 0; }
        acc|=(uint32_t)(b&0x7f)<<sh; sh+=7;
        if(!(b&0x80)) return acc; }
}
static uint8_t up_hy_src_peek(PatchApply *pa, int32_t fp){
    if(fp>=0 && (uint32_t)fp<pa->g_from_size){ uint8_t jb; return up_jr_get(pa,(uint32_t)fp,&jb)?jb:up_image_flash_read((uint32_t)fp); }
    return 0;
}
static void up_hy_half_rec(PatchApply *pa, uint32_t a, uint8_t lo, uint8_t hi){
    uint16_t up=(uint16_t)(lo | ((uint16_t)hi<<8));
    if(rc_thumb_ldr_lit(up)) up_psrc_ldr_put(pa,(uint32_t)rc_ldr_target((int32_t)a,(int32_t)lo));
}
/* record one pristine source byte at fp into the packed FWD ldr metadata.
 * Out-of-range fp is a no-op (matches up_hy_src_peek's range gate). */
static void up_hy_src_rec(PatchApply *pa, int32_t fp, uint8_t v){
    if(fp>=0 && (uint32_t)fp<pa->g_from_size){
        uint32_t a=(uint32_t)fp;
        if(a&1u){
            if(pa->g_psrc_even==a-1u) up_hy_half_rec(pa,a-1u,pa->g_psrc_lo,v);
        } else {
            pa->g_psrc_lo=v;
            pa->g_psrc_even=a;
        }
    }
}
/* journal-aware pristine 4-byte source word (little-endian), peeked WITHOUT touching the psrc
 * ring. up_apply_op slides this word across ordinary copies so field detection and the following
 * copy consume the same pristine bytes. */
static uint32_t up_hy_word4_peek(PatchApply *pa, int32_t fp){
    return  (uint32_t)up_hy_src_peek(pa,fp)    | ((uint32_t)up_hy_src_peek(pa,fp+1)<<8)
          | ((uint32_t)up_hy_src_peek(pa,fp+2)<<16) | ((uint32_t)up_hy_src_peek(pa,fp+3)<<24);
}
/* FWD: record a known-in-range 4-byte field window [fpk,fpk+4) into the ring (ascending, LE). The caller's
 * entry guard pins fpk>=0 && fpk+4<=from_size, so every byte is in range and the per-byte up_hy_src_rec
 * gate would always pass — record straight into the 4 ring slots (the window cannot self-alias). */
static void up_hy_word4_rec(PatchApply *pa, uint32_t fpk, uint32_t w){
    if(!pa->g_FWD) return;
    up_hy_half_rec(pa,fpk,     (uint8_t)w,       (uint8_t)(w>>8));
    up_hy_half_rec(pa,fpk+2u,  (uint8_t)(w>>16), (uint8_t)(w>>24));
}
/* de-reloc field at op-local field-start ks. Returns: 0=no field; 1=bl/ex (packed4 filled,
 * a value consumed from the generator); 2=suppressed-bl (write the 4 bytes as NORMAL copies).
 *
 * The field window [fpk,fpk+4) is fully in-range (the entry guard pins fpk+4<=from_size). up_apply_op
 * supplies the 4 pristine field bytes from its peek-only rolling word; both the BL pattern test and
 * the BL/EX pack reuse w, and a real hit replays w directly into the ring (up_hy_word4_rec). Field kinds
 * are mutually exclusive and tried in encoder order: local-BL first (Thumb F000/D000 pattern,
 * 2-aligned), else a same-op LDR-literal target.
 * Both consume a stream delta ONLY on a real BL/EX hit (suppressed-BL and "no field" never touch the
 * stream, and record nothing into the ring). */
static int up_field_at(PatchApply *pa, int32_t fp0, int32_t ks, uint32_t w, uint8_t packed[4], int pure, int32_t dl){
    /* fpk = fp0 + ks, range-checked in pure 32-bit. ks >= 0 (op-local field offset) and image sizes are
     * < 2^31 (hard design invariant), so fpk < 0 iff fp0 < -ks (no overflow: -ks is a valid int32 for
     * ks >= 0), and once fpk >= 0 the unsigned sum fp0+ks cannot wrap (0 <= fp0+ks < 2^32). */
    if(fp0 < -ks) return 0;                                /* fpk would be negative */
    uint32_t fpk=(uint32_t)fp0+(uint32_t)ks;
    if(fpk>pa->g_from_size || pa->g_from_size-fpk<4u) return 0;    /* fpk+4 > from_size (no +4 overflow) */
    if(fpk&1u) return 0;                                  /* BL is 2-aligned; LDR targets are 4-aligned */
    uint16_t up=(uint16_t)w, lo=(uint16_t)(w>>16);
    int ldr_hit=0;
    if(!(fpk&3u)) ldr_hit=pa->g_FWD ? up_psrc_ldr_take(pa,fpk) : up_grow_ldr_take(pa,fp0,dl,fpk);
    /* local-BL: 2-aligned, low halfword F000-pattern, high halfword D000-pattern (pristine source) */
    if(rc_bl_pattern(up,lo)){
        if(fpk&2u) (void)up_psrc_ldr_take(pa,pa->g_FWD ? fpk+2u : fpk-2u);
        if(!pure) return 2;                                 /* suppressed-bl is implicit */
        int32_t res=up_pull_delta(pa,&pa->DR_BL, &pa->MDL_pre.dibl, pa->DR_DIC_BL, DR_KCAP_BL); /* residual from the single stream */
        if(pa->g_rcerr) return 0;
        /* byte shift -> imm24 halfword units. The subtract is done mod 2^32 (a corrupt map can
         * ship values near +-2^31; wrapped garbage is CRC-rejected, signed overflow would be UB);
         * valid map values are tiny, so the wrapped diff equals the true diff bit-exactly. */
        int32_t pred=rc_smap_pred_bl(pa->g_smap_b,pa->g_smap_v,(int)pa->g_smap_n,fpk,rc_bl_target(fpk,up,lo));
        rc_bl_dereloc(up, lo, (uint32_t)pred+(uint32_t)res, packed);
        up_hy_word4_rec(pa,fpk,w);                             /* record the 4 BL bytes into the ring */
        return 1;
    }
    /* A1: ex (ldr) DERIVED, gated by `pure` (no literal patch in the 4 bytes) — mirrors the
     * encoder's pure(k) + op_ldr_set; positions are derived, not shipped. */
    if(pure && ldr_hit){
        int32_t res=up_pull_delta(pa,&pa->DR_EX, &pa->MDL_pre.diex, pa->DR_DIC_EX, DR_KCAP_EX); /* residual from the single stream */
        if(pa->g_rcerr) return 0;
        int32_t pred=rc_smap_pred_ex(pa->g_smap_b,pa->g_smap_v,(int)pa->g_smap_n,w); /* pointer words move by the value's shift */
        up_hy_word4_rec(pa,fpk,w);                             /* record the 4 EX bytes into the ring */
        rc_u32le_put(packed, (w-((uint32_t)pred+(uint32_t)res))&0xffffffffu);
        return 1;
    }
    return 0;
}
/* Literal cursor over the op's content stream + the per-byte write helpers. The cursor advances in
 * wire order regardless of write direction; the copy/extra helpers are plain static functions so the
 * two call sites share one body instead of duplicating the write loops inside up_apply_op. */
/* Literal-patch cursor. The wire codes nl literal patches as a run of uLEB gaps + a literal byte
 * each, read in wire order. FWD next-positions accumulate forward from 0; grow next-positions step
 * back from dl (first = dl - gap, then -= gap). The first patch and every later patch share one
 * advance path; nextpos=-1 marks "no more". */
typedef struct { int32_t nextpos, litb, li, step, lim; } up_LitCur;
static void RC_NOINLINE up_litcur_step(PatchApply *pa, up_ApplyState*s, up_LitCur*lc, int32_t nl){
    if(lc->li<nl){
        uint32_t g=up_read_uleb(pa,s);
        /* corrupt-stream guard (also removes a signed-overflow UB): a valid gap always keeps
         * the cursor inside [0,lim] (lim==dl; the encoder emits only in-range positions), so
         * bound the gap and the stepped position in pure uint32 (no wrap can slip through:
         * g<=lim and base<=lim keep base+g < 2^32; a backward underflow lands >lim). */
        uint32_t np=(lc->step>0)? (uint32_t)lc->nextpos+g : (uint32_t)lc->nextpos-g;
        if(g>(uint32_t)lc->lim || np>(uint32_t)lc->lim){ pa->g_rcerr=1; lc->nextpos=-1; return; }
        lc->nextpos=(int32_t)np; lc->litb=up_next_content(pa,s,0);
        if(pa->g_rcerr){ lc->nextpos=-1; return; }
    }
    else lc->nextpos=-1;
}
RC_ALWAYS_INLINE void up_litcur_init(PatchApply *pa, up_ApplyState*s, up_LitCur*lc, int fwd, int32_t nl, int32_t dl){
    lc->step = fwd ? 1 : -1; lc->lim = dl;
    lc->nextpos = fwd ? 0 : dl; lc->litb=0; lc->li=0;
    up_litcur_step(pa, s, lc, nl);   /* read the first patch (or set nextpos=-1 when nl==0) */
}
/* el extra bytes, written in iteration direction (after dl for FWD, before dl for grow) */
static void up_wr_extras(PatchApply *pa, up_ApplyState*s, up_CorrCur*cc, int fwd, int32_t tp0, int32_t dl, int32_t el){
    for(int32_t i=0;i<el && !pa->g_rcerr;i++){
        int32_t e = fwd ? i : (el-1-i);
        uint8_t eb=up_next_content(pa,s,(int)((tp0+dl+e)&1));
        if(pa->g_rcerr) return;
        up_out_write(pa,(uint32_t)(tp0+dl+e), (uint8_t)(eb + up_corr_take(s,cc,dl+e)));
    }
}
/* copy-mode byte at K: take the literal patch if the cursor sits on K, else the caller's cached
 * pristine source byte. FWD records it only now, at the actual copy-read point. */
static void up_wr_copy(PatchApply *pa, up_ApplyState*s, up_LitCur*lc, up_CorrCur*cc, int32_t tp0, int32_t fp, int32_t nl, int32_t K, uint8_t src){
    uint8_t db=0;
    if(K==lc->nextpos){ db=(uint8_t)lc->litb; lc->li++; up_litcur_step(pa,s,lc,nl); }
    if(pa->g_rcerr) return;
    if(pa->g_FWD) up_hy_src_rec(pa,fp,src);
    up_out_write(pa,(uint32_t)(tp0+K), (uint8_t)(db + src + up_corr_take(s,cc,K)));
}
/* one streaming op: DIRECT geometry+P+C, journal P eagerly, then INLINE write-order field
 * detection + streaming write via up_out_write (asc FWD / desc grow). No override buffer. */
static void RC_NOINLINE up_apply_op(PatchApply *pa, up_ApplyState*s){
    uint32_t dl_u=up_s_ug_gamma(pa,&pa->MDL_pre.gdl), el_u=up_s_ug_gamma(pa,&pa->MDL_pre.gel), adj_u=up_s_ug_gamma(pa,&pa->MDL_pre.gadj);
    int32_t adj=rc_unzz32_value(adj_u);
    /* nw = dl+el (op output bytes). The image span is < 2^31 (g_image_span <= MAX_IMAGE via
     * up_decode_header envelope checks + _Static_assert), so this pure 32-bit headroom test (check dl,
     * then el against the rest of span) also subsumes the dl_u/el_u < 2^31 bound: any value >= 2^31
     * exceeds the span and is rejected here. Cast to int32 only after the guard passes. */
    if(pa->g_rcerr || dl_u>(uint32_t)pa->g_image_span || el_u>(uint32_t)pa->g_image_span-dl_u){ pa->g_rcerr=1; return; }
    int32_t dl=(int32_t)dl_u, el=(int32_t)el_u;
    int32_t nw=(int32_t)(dl_u+el_u);   /* <= g_image_span < 2^31 */
    /* Feature 7B termination safety: the encoder folds out every zero-OUTPUT op, so a valid op always
     * writes >=1 byte. REJECT nw==0 here (do NOT trust the encoder): it is what guarantees the
     * frontier-terminated op loop advances every iteration and cannot spin on a corrupt stream. */
    if(nw==0){ pa->g_rcerr=1; return; }
    int32_t tp0,fp0;
    if(pa->g_FWD){
        /* tp+nw must stay <= to_size (tp,nw >= 0: a 32-bit headroom test); fp+dl+adj must stay in int32,
         * checked with RC_ADD_OVERFLOW (no 64-bit math). (For a valid stream fp is a small
         * in-range position, so this never trips.) */
        int32_t nfp;
        if(nw>(int32_t)pa->g_to_size || s->tp>(int32_t)pa->g_to_size-nw ||
           RC_ADD_OVERFLOW(s->fp,dl,&nfp) || RC_ADD_OVERFLOW(nfp,adj,&nfp)){ pa->g_rcerr=1; return; }
        tp0=s->tp; fp0=s->fp; s->tp=s->tp+nw; s->fp=nfp;
    } else {
        int32_t nfp;
        if(s->tp<nw ||
           RC_SUB_OVERFLOW(s->fp,dl,&nfp) || RC_SUB_OVERFLOW(nfp,adj,&nfp)){ pa->g_rcerr=1; return; }
        s->tp=s->tp-nw; s->fp=nfp; tp0=s->tp; fp0=s->fp;
    }
    /* fp headroom: the copy loop and the ldr back-scan form fp0+K for K<=dl, so fp0+dl must be
     * representable in int32. The overflow builtins above only guard fp0+dl+adj / fp0 itself —
     * a corrupt adj walk can park fp0 near INT32_MAX and make the bare fp0+dl overflow (UB).
     * Valid streams keep fp inside the image plus small overshoot, so this never fires there. */
    if(fp0>(int32_t)0x7fffffff-dl){ pa->g_rcerr=1; return; }
    /* ---- [P]/[C] op-local offsets: journal preserves eagerly, then fill sorted corrections. ---- */
    uint32_t nwu=(uint32_t)nw;
    int corr=0;
    uint32_t n=up_s_ug_gamma(pa,&pa->MDL_pre.pgn);
    for(;;){
        uint32_t off=0;
        if(corr && n>(uint32_t)OPC_CAP){ pa->g_rcerr=1; pa->g_reject=REJ_RESOURCE; return; }
        if(!corr && n>(uint32_t)JSLOTS-pa->g_jcount){ pa->g_rcerr=1; pa->g_reject=REJ_RESOURCE; return; }
        if(n>nwu){ pa->g_rcerr=1; return; }
        if(corr) s->op_nc=(int32_t)n;
        for(uint32_t i=0;i<n && !pa->g_rcerr;i++){
            if(!up_op_next_offset(pa,&off,i,nwu)) return;
            if(corr){
                int cbyte=up_s_bt(pa,&pa->MDL_pre.dval, RC_DVAL_RATE);
                s->op_corr[i]=(off<<8)|(uint32_t)cbyte;
            } else {
                uint32_t pos=(uint32_t)(tp0+(int32_t)off);
                if(pos>=pa->g_from_size){ pa->g_rcerr=1; return; }
                if(pos>=RC_PACKED_POS_LIMIT){ pa->g_rcerr=1; pa->g_reject=REJ_RESOURCE; return; }
                uint32_t at=(uint32_t)pa->g_jcount+(pa->g_FWD?i:n-1u-i);
                pa->ARENA.apply.jbuf[at]=(pos<<8)|up_image_flash_read(pos);
            }
        }
        if(pa->g_rcerr) return;
        if(!corr) pa->g_jcount=(uint16_t)(pa->g_jcount+n);
        if(corr) break;
        corr=1; n=up_s_ug_gamma(pa,&pa->MDL_pre.pgn);
    }
    /* A1: no BL/LDR offsets on the wire. BL suppression is inferred from !pure, and ldr positions
     * are derived per op (ldr_targets). */
    /* ---- CONTENT decode + streaming write with inline field detection ----
     * Both directions process the same 4-byte ascending field windows over [0,dl); they differ only
     * in (a) iteration direction (step), (b) when the el extra bytes are consumed relative to the dl
     * body (FWD: after; grow: before), and (c) the literal-cursor gap encoding (FWD: absolute forward
     * next-positions; grow: gaps stepping back from dl). The litcur macros hide (c); the el macro is
     * invoked at the direction-correct point to keep the content-stream read order bit-exact. */
    uint32_t nl_u=up_read_uleb(pa,s);
    if(nl_u>(uint32_t)dl){ pa->g_rcerr=1; return; }
    int32_t nl=(int32_t)nl_u;
    uint8_t packed[4];
    int fwd=pa->g_FWD; int32_t step=fwd?1:-1;
    memset(pa->g_psrc_ldr,0,sizeof pa->g_psrc_ldr); pa->g_psrc_even=UINT32_MAX;
    up_LitCur lc;
    up_CorrCur cc; up_corr_cur_init(&cc,s,fwd);
    if(!fwd) up_wr_extras(pa,s, &cc, fwd, tp0, dl, el);
    up_litcur_init(pa,s, &lc, fwd, nl, dl);
    int32_t k=fwd?0:(dl-1);
    uint32_t srcw=0; int32_t srcw_at=-4;                 /* low op-local offset cached in srcw */
    while(k>=0 && k<dl && !pa->g_rcerr){
        int32_t a0=fwd?k:(k-3);                          /* low anchor of the 4-byte field window */
        if(a0>=0 && a0+4<=dl){
            if(a0==srcw_at+step){
                if(fwd) srcw=(srcw>>8)|((uint32_t)up_hy_src_peek(pa,fp0+a0+3)<<24);
                else srcw=(srcw<<8)|(uint32_t)up_hy_src_peek(pa,fp0+a0);
            } else srcw=up_hy_word4_peek(pa,fp0+a0);
            srcw_at=a0;
            int pure=(lc.nextpos<0 || lc.nextpos<a0 || lc.nextpos>=a0+4);   /* no literal patch in [a0,a0+3] */
            int fa=up_field_at(pa, fp0, a0, srcw, packed, pure, dl);
            if(fa){
                for(int b=fwd?0:3; b>=0 && b<4; b+=step){
                    if(fa==2) up_wr_copy(pa,s, &lc, &cc, tp0, fp0+a0+b, nl, a0+b, (uint8_t)(srcw>>(8*b)));
                    else up_out_write(pa,(uint32_t)(tp0+a0+b),(uint8_t)(packed[b]+up_corr_take(s,&cc,a0+b)));
                }
                k+=4*step; continue;
            }
            up_wr_copy(pa,s, &lc, &cc, tp0, fp0+k, nl, k, (uint8_t)(fwd?srcw:srcw>>24));
        } else {
            int32_t d=k-srcw_at;
            uint8_t src=(uint32_t)d<4u ? (uint8_t)(srcw>>(8*d)) : up_hy_src_peek(pa,fp0+k);
            up_wr_copy(pa,s, &lc, &cc, tp0, fp0+k, nl, k, src);
        }
        k+=step;
    }
    if(fwd) up_wr_extras(pa,s, &cc, fwd, tp0, dl, el);
}

/* ===================================================================================== */
/* the decode entry: runs the whole single-stream decode start-to-finish on the CALLER's    */
/* stack (plain calls, no coroutine/fiber). Shared state lives in the caller-owned PatchApply */
/* object that patch_apply_run primes before invoking up_decode_body.                            */
/* ===================================================================================== */
/* ---- envelope header (strict reads): CRC32(from)[4] | CRC32(to)[4] | from_size uLEB |
 * zz(to_size-from_size) | [zz(fp_end-from_size) iff descending] | [zz(fp_start) iff ascending] |
 * compressed_body_len. The decoder derives the apply direction and the image span itself
 * and verifies CRC32(from) over flash BEFORE the first flash write — a truncated header,
 * implausible sizes, or a wrong/dirty current image all reject cleanly with flash untouched.
 * CRC32(to) is stashed in g_want_to here and checked over the final image after apply
 * (patch_apply_run). Then the literal bit-trees are seeded from flash parity histograms
 * (hist0/hist1/w borrow 4 KiB of ARENA before the apply
 * overlays it). noinline: this runs ONCE at the top of up_decode_body, so keeping its locals in
 * their own frame (not merged into up_decode_body's) keeps them off the deep apply call chain
 * that runs afterward — it trims the decode's peak CALLER-stack usage. */
static int RC_NOINLINE up_decode_header(PatchApply *pa){
    uint32_t want_from, want_to, fs, ts, fpe, z, bl; uint8_t ov;
    int32_t fp_start=0;
    if(!up_env_u32le(pa,&want_from) || !up_env_u32le(pa,&want_to) || !up_env_uleb(pa,&fs,0) || fs>MAX_IMAGE || !up_env_uleb(pa,&z,&ov)) return 0;
    if(z&1u){ uint32_t m=(z>>1)+1u; if(m>fs) return 0; ts=fs-m; }
    else    { uint32_t d=z>>1; if(d>MAX_IMAGE||fs>MAX_IMAGE-d) return 0; ts=fs+d; }
    /* Apply direction is an ENCODER CHOICE. The NATURAL direction (descending iff the image
     * grows — the historical derived rule, so a canonically-coded envelope decodes exactly
     * as before) ships as a canonical size-delta uLEB; the UNNATURAL direction is signaled
     * by the overlong marker above and costs one byte (chosen only when it wins by more).
     * fp_end ships iff the apply is DESCENDING (it seeds the descending source walk; the
     * ascending walk derives fp from 0). */
    fpe=fs;
    { int desc=(ts>fs)!=(int)ov;
      if(desc && !up_env_zz_abs(pa,fs,&fpe)) return 0;
      pa->g_FWD=!desc; }
    /* Feature 7B initial source seek (fp_start): zigzag-uLEB, shipped ONLY when ASCENDING (descending
     * seeds fp from fp_end and CRC32(to) subsumes its landing — one seed field per direction). */
    if(pa->g_FWD){ uint32_t z2; if(!up_env_uleb(pa,&z2,0)) return 0;
      fp_start=rc_unzz32_value(z2); }
    if(!up_env_uleb(pa,&bl,0) || bl<4u) return 0;  /* dropped leading cache byte leaves at least 4 code bytes */
    pa->g_from_size=fs; pa->g_to_size=ts; pa->g_want_to=want_to;
    pa->g_body_left=bl;
    pa->g_image_span = fs>ts ? fs : ts;
    { uint32_t *hist0=pa->ARENA.seed.hist0, *hist1=pa->ARENA.seed.hist1, *w=pa->ARENA.seed.w;
      for(int i=0;i<256;i++){ hist0[i]=1; hist1[i]=1; }
      if(up_crc32_flash_hist(pa,fs,hist0,hist1)!=want_from) return 0;
      up_lit_tree_from_hist(&pa->M_lit0[0],hist0,w);
      for(int c=1;c<UP_LIT0_CTX;c++) pa->M_lit0[c]=pa->M_lit0[0];
      up_lit_tree_from_hist(&pa->M_lit1,hist1,w); }
    { up_ApplyState*s=&pa->ARENA.apply.sa; memset(s,0,sizeof*s); s->tp=pa->g_FWD?0:(int32_t)ts; s->fp=pa->g_FWD?fp_start:(int32_t)fpe; }
    return 1;
}
static int up_decode_body(PatchApply *pa){
    if(!up_decode_header(pa)) return 0;
    if(!up_rc_init(pa)) return 0;
    up_orow_reset(pa);
    /* ---- piecewise shift map: gamma count, then per entry a gamma boundary gap (first absolute,
     * later gaps-1; strictly ascending) and a zigzag-gamma byte-shift value. count 0 => no map
     * (all predictions 0 == the residual stream degenerates to the plain delta stream). ---- */
    /* BORROW two apply-phase gamma statics (not yet live: the map is read before apply-model setup,
     * and both are re-init'd fresh at rc_ugg_init(&M_gdl)/rc_ugg_init(&M_gadj) below before the token loop).
     * Coding the skewed map gap/value distributions through ADAPTIVE gamma beats raw equiprobable bits
     * at ZERO extra SRAM. M_gdl carries the count + boundary gaps; M_gadj carries the zz shift values. */
    rc_ugg_init(&pa->MDL_pre.gdl); rc_ugg_init(&pa->MDL_pre.gadj);
    { uint32_t mn=up_s_ug_gamma(pa,&pa->MDL_pre.gdl);
      if(mn>UP_SMAP_CAP){ pa->g_reject=REJ_RESOURCE; return 0; }
      uint32_t b=0;
      for(uint32_t i=0;i<mn && !pa->g_rcerr;i++){
          uint32_t gap=up_s_ug_gamma(pa,&pa->MDL_pre.gdl); if(i) gap+=1u;
          if(gap>UINT32_MAX-b){ pa->g_rcerr=1; break; }
          b+=gap;
          pa->g_smap_b[i]=b; pa->g_smap_v[i]=rc_unzz32_value(up_s_ug_gamma(pa,&pa->MDL_pre.gadj));
      }
      if(pa->g_rcerr) return 0;
      pa->g_smap_n=(uint16_t)mn; }
    /* out-match enable: 1 raw bit. 0 => no ko header field and no out-bit on any fresh match,
     * so patches that never out-match (e.g. the one-face update) pay exactly one bit. */
    pa->g_out_en=(uint8_t)up_s_raw(pa);
    /* ---- STREAMED DELTAS: NO up-front DEREL phase. The delta models are initialized fresh and used
     * INLINE during apply (up_pull_delta in up_field_at). M_dval (escape/correction bytes), the two MTF
     * dict streams, and the two index UGolombs all persist through apply. ---- */
    rc_dr_init(&pa->DR_BL, pa->DR_DIC_BL, UP_DR_HIT_INIT); rc_dr_init(&pa->DR_EX, pa->DR_DIC_EX, UP_DR_HIT_INIT);
    /* pre-kd apply-phase models: dval + dict-index seeds + preserve/corr/geometry gammas with their
     * structural seed_cont priors. This RE-INITs the gdl/gadj borrowed for the map header above; the
     * whole sequence is single-sourced as rc_init_prekd (rc_models.h, mirror of encoder emit_body). */
    rc_init_prekd(&pa->MDL_pre);
    /* ---- [A] streaming apply (no bake): per op read DIRECT geom+P+C, journal P eagerly,
     * then PULL the op's CONTENT from the cut whole-stream LZSS, detect de-reloc fields inline in
     * write order (pulling each delta from the single stream via up_pull_delta), write via up_out_write. ---- */
    { up_ApplyState*s=&pa->ARENA.apply.sa;
      int kd=(int)up_s_raw_bits(pa,RC_KFIELD_BITS);
      int ko=pa->g_out_en?(int)up_s_raw_bits(pa,RC_KFIELD_BITS):0;
      rc_init_tok(&pa->MDL_tok,kd,ko);   /* gd/go rice + gl(+seed)/gs/glo + outb + flag + rep0 prior */
      pa->g_oexp=pa->g_FWD?0u:pa->g_to_size;
      if(pa->g_rcerr) return 0;
      /* Feature 7B: NO shipped op count. Frontier-terminate — FWD until tp reaches to_size, grow
       * until tp reaches 0. This is SAFE because up_apply_op REJECTS any zero-output op (nw==0), so
       * every accepted op advances the frontier by >=1 and the overshoot/underrun guards cap it: the
       * loop runs at most to_size times, then rejects (a corrupt stream cannot spin). A truncated
       * stream decodes zero-fill garbage that trips a geometry/frontier guard or lands a wrong image
       * that CRC32(to) rejects. */
      while(!pa->g_rcerr && (pa->g_FWD ? s->tp!=(int32_t)pa->g_to_size : s->tp!=0)) up_apply_op(pa,s);
      /* tp must land exactly; the loop guarantees it on the clean path (it exits only on g_rcerr or
       * tp==target, and up_apply_op's frontier guards make overshoot impossible), so no post-loop tp
       * landing check is needed. fp is unchecked in both directions — CRC32(to) validates the image. */
      if(!pa->g_rcerr && s->tok_mode!=0) pa->g_rcerr=1;   /* a mid-token content underrun leaves tok_mode!=0 */
      if(pa->g_rcerr) return 0;
      up_orow_commit_all(pa);                 /* flush the remaining uncommitted pages */
    }
    /* body complete: the caller verifies CRC32(to) (g_want_to) over the final image before
     * declaring DONE. Bytes after the counted body belong to outer framing. */
    return 1;
}

/* ===================================================================================== */
/* public API: patch_apply_run(state, callback, ctx) -> DONE / ERROR                       */
/* ===================================================================================== */
static void RC_NOINLINE up_lit_tree_from_hist(up_BitTree*t,const uint32_t*hist,uint32_t*w){
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
    pa->g_pull_fn=next; pa->g_pull_ctx=ctx;
    if(!up_decode_body(pa) || up_crc32_flash(pa,pa->g_to_size)!=pa->g_want_to){
        if(!pa->g_reject) pa->g_reject=REJ_CORRUPT;
        return PATCH_APPLY_ERROR; }
    return PATCH_APPLY_DONE;
}

/* After PATCH_APPLY_ERROR, the reject reason: REJ_RESOURCE (a decoder cap was exceeded — this
 * firmware needs a larger build) vs REJ_CORRUPT (malformed/truncated/wrong-image). Prefer this
 * accessor over reading g_reject directly; it is unused-static-inline and compiles out when the
 * integrator does not call it (no ARM .text cost). */
static inline int patch_apply_reject(const PatchApply *pa){ return pa->g_reject; }

/* After PATCH_APPLY_ERROR, whether any flash_write_page happened this run: 0 = old image intact
 * (still bootable, safe to retry after fixing the cause), 1 = image partially overwritten
 * (bootloader recovery required). Same unused-static-inline compile-out as patch_apply_reject. */
static inline int patch_apply_flash_touched(const PatchApply *pa){ return pa->g_flash_touched; }

/* Parsed patch geometry/state after a completed run. These keep host harnesses and
 * integrations from depending on decoder global names while still compiling out when unused. */
static inline uint32_t patch_apply_from_size(const PatchApply *pa){ return pa->g_from_size; }
static inline uint32_t patch_apply_to_size(const PatchApply *pa){ return pa->g_to_size; }
static inline uint32_t patch_apply_image_span(const PatchApply *pa){ return pa->g_image_span; }
static inline int patch_apply_forward(const PatchApply *pa){ return pa->g_FWD; }
static inline uint32_t patch_apply_journal_used(const PatchApply *pa){ return pa->g_jcount; }

#undef RC_RICE_UNARY_MAX
#undef UP_JREGION
#undef UP_RING
#undef UP_MASK
#undef UP_ARENA_BYTES
#undef UP_RC_UNARY_MAX
#undef UP_ULEB32_OVERFLOW
#undef UP_OROW_NONE
#undef UP_OROW_SLOT
#undef UP_PSRC_TGT_MASK

/* Seal the non-knob model/wire macros that rc_models.h + patch_config.h define and that this
 * header pulls in transitively, so they do NOT leak into the integrator's TU after the include.
 * Only the documented integration/configuration macros (PATCH_IMAGE_BASE, JSLOTS, WINDOW_LOG,
 * OUTROW, OUTROW_DEPTH, OPC_CAP, DR_KCAP_BL, DR_KCAP_EX, and MAX_IMAGE) stay defined. The
 * encoder TUs include rc_models.h/patch_config.h DIRECTLY (not through this header), so these
 * #undefs never reach the encoder side of the mirror.
 *
 * The RC_ portability shim is sealed here too: rc_models.h keeps it defined for the encoder (which
 * never includes this header), but it is an implementation detail that must not leak to integrators. */
#undef RC_ALWAYS_INLINE
#undef RC_NOINLINE
#undef RC_NORETURN
#undef RC_ADD_OVERFLOW
#undef RC_SUB_OVERFLOW
#undef RC_KTOP
#undef RC_PROB_BITS
#undef RC_PBIT
#undef RC_PHALF
#undef RC_PROB_BOUND
#undef RC_PACKED_POS_BITS
#undef RC_PACKED_POS_LIMIT
#undef UP_BT_PROBS
#undef UP_BT_BYTES
#undef UP_UG_CTX
#undef UP_UG_C
#undef UP_UG_GAMMA_MANT
#undef UP_SMAP_CAP
#undef UP_LIT0_CTX
#undef UP_IDX_CTX
#undef RC_S_BIT_RATE
#undef RC_LIT0_RATE
#undef RC_LIT1_RATE
#undef RC_DVAL_RATE
#undef RC_REP0_INIT
#undef UP_DR_HIT_INIT
#undef RC_IDX_SEED
#undef RC_SEED_DEPTH_GDL
#undef RC_SEED_DEPTH_GADJ
#undef RC_SEED_DEPTH_PG2
#undef RC_SEED_DEPTH_GL
#undef RC_OUTMATCH_MIN
#undef RC_KFIELD_BITS

#endif /* UP_PATCH_APPLY_H */
