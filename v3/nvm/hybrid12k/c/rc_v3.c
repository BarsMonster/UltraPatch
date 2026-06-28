/*
 * ultrapatch v3-on-flash (A1) — streaming, in-place, real-NVM firmware decoder (C).
 * Production solution; full design and measured gates are in ../RESULT.md.
 *
 * The patch arrives BYTE-BY-BYTE over a slow link. A single divide-free binary range coder (LZMA
 * bound; no division — Cortex-M0+ has no HW divide) decodes ONE interleaved stream whose symbols
 * are emitted in decode-consumption order (no length prefixes, no byte-aligned sections).
 * next_byte() BLOCKS on an empty byte FIFO; the block is realised by a coroutine (a PendSV/SP swap
 * on the device; a minimal fiber on the x86 host test) — the decode state machine runs on its own
 * small stack and swaps back to the producer at the renorm point when the FIFO is empty. The
 * producer drives it with  decoder_push(byte) -> NEED_MORE / DONE / ERROR.
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
 * Embedded target: NO 64-bit integers in the hot path; all positions/sizes are 32-bit.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "rc_models.h"

/* ===================================================================================== */
/* flash model: the image lives in "flash", accessed ONLY through these. On the host test  */
/* harness flash is a file/buffer; on the device these are the real flash page primitives. */
/* The decoder keeps 0 image bytes in RAM.                                                 */
/* ===================================================================================== */
extern uint8_t flash_read(uint32_t addr);
extern void    flash_write(uint32_t addr, uint8_t val);
extern uint32_t g_image_span;      /* max(from_size,to_size) — bounds every flash access */

static uint32_t g_from_size, g_to_size, g_fp_end;
static int      g_FWD;

/* ===================================================================================== */
/* byte FIFO + ucontext coroutine (the push / blocking-fetch core)                        */
/* ===================================================================================== */
static uint8_t  g_fifo_byte, g_fifo_full;  /* decoder_push() supplies one byte per resume */
static uint8_t  g_eof;                     /* producer signalled end-of-stream */

#ifndef DEC_STACK_BYTES
#define DEC_STACK_BYTES 576                /* coroutine stack; measured high-water 456 B (data-independent
                                            * call depth) leaves a 120 B cushion for out-of-corpus call
                                            * paths. The deepest 8 B are a 0xC5 canary checked at decode
                                            * end -> stack overflow REJECTS, never silent-corrupts. */
#endif
static char     g_dec_stack[DEC_STACK_BYTES] __attribute__((aligned(16)));
enum { DEC_NEED_MORE=0, DEC_DONE=1, DEC_ERROR=2 };
static uint8_t  g_dec_status;              /* DEC_NEED_MORE while running, then DONE/ERROR */

/* ===================================================================================== */
/* Coroutine: a context is just a saved stack pointer (+ callee-saved regs spilled onto    */
/* its own stack at the swap point) — a few words on a real Cortex-M0+ (PendSV-style swap, */
/* ~16 B static for the two SP slots). The x86-64 host (the test arch) uses the same        */
/* MINIMAL SP-fiber so the host SRAM number reflects the true device cost; the ARM device   */
/* build also uses the fiber (no ucontext.h on bare metal).                                 */
/*                                                                                         */
/* The two arches share one interface (CO_SWAP_TO_MAIN / CO_SWAP_TO_DEC + fiber_setup): a   */
/* context is one saved stack pointer. Common to both is the Fiber type, the two SP slots,  */
/* decode_body, and the trampoline; they differ only in the swap mechanism (real naked asm  */
/* on x86 vs the device's PendSV, stubbed here for static sizing) and the initial frame      */
/* fiber_setup lays down.                                                                   */
/* ===================================================================================== */
static void decode_body(void);

/* --- shared SP-fiber core (x86 host + ARM device) --- */
typedef void* Fiber;                       /* a context is just a saved stack pointer */
static Fiber g_main_sp, g_dec_sp;
static void pd_trampoline(void){ decode_body(); for(;;); }  /* never returns (final swap inside) */

#ifdef RC_V3_ARM
/* RC_V3_ARM device-static measurement build: the real device coroutine is a PendSV/SP swap;
 * a context is two saved SP words (~16 B static). We retain the FULL decoder graph + the
 * coroutine-stack reservation so arm-none-eabi-size reports the true device .bss+.data: the
 * trampoline references decode_body, fiber_setup references g_dec_stack. The asm swap is the
 * device's PendSV; here a minimal volatile stub keeps the symbols (size is graph-complete). */
static volatile void* g_arm_anchor;
static void fiber_setup(void){
    void**sp=(void**)(g_dec_stack+DEC_STACK_BYTES);   /* reserve & reference the coroutine stack */
    *--sp=(void*)pd_trampoline; g_dec_sp=(Fiber)sp; g_arm_anchor=(void*)&g_dec_stack[0];
}
#define CO_SWAP_TO_MAIN()  do{ g_main_sp=g_dec_sp; }while(0)   /* device PendSV; stubbed for sizing */
#define CO_SWAP_TO_DEC()   do{ g_dec_sp=g_main_sp; }while(0)
#else /* x86-64 host: real SP swap via naked asm */
__attribute__((noinline,naked)) static void pd_swap(Fiber*from __attribute__((unused)),
                                                    Fiber*to __attribute__((unused))){
    __asm__ volatile(
        "pushq %%rbp\n\t pushq %%rbx\n\t pushq %%r12\n\t pushq %%r13\n\t pushq %%r14\n\t pushq %%r15\n\t"
        "movq %%rsp, (%%rdi)\n\t"           /* *from = rsp */
        "movq (%%rsi), %%rsp\n\t"           /* rsp = *to */
        "popq %%r15\n\t popq %%r14\n\t popq %%r13\n\t popq %%r12\n\t popq %%rbx\n\t popq %%rbp\n\t"
        "ret\n\t" : : : "memory");
}
static void fiber_setup(void){
    uintptr_t top=(uintptr_t)(g_dec_stack+DEC_STACK_BYTES);
    top &= ~(uintptr_t)15;                 /* 16-align */
    void**sp=(void**)top;
    *--sp=(void*)0;                        /* fake return addr slot (alignment) */
    *--sp=(void*)pd_trampoline;            /* ret target of pd_swap */
    *--sp=(void*)0;  /* r15 */ *--sp=(void*)0;  /* r14 */ *--sp=(void*)0;  /* r13 */
    *--sp=(void*)0;  /* r12 */ *--sp=(void*)0;  /* rbx */ *--sp=(void*)0;  /* rbp */
    g_dec_sp=(Fiber)sp;
}
#define CO_SWAP_TO_MAIN()  pd_swap(&g_dec_sp,&g_main_sp)
#define CO_SWAP_TO_DEC()   pd_swap(&g_main_sp,&g_dec_sp)
#endif

/* next_byte(): pull one byte; if the FIFO is empty, suspend back to the producer (block). */
static uint8_t next_byte(void){
    while (!g_fifo_full){
        if (g_eof) return 0;               /* zero-fill past EOF (optimal-flush rule) */
        CO_SWAP_TO_MAIN();                 /* SUSPEND: yield to producer for more bytes */
    }
    g_fifo_full=0;
    uint8_t b = g_fifo_byte;
    return b;
}

/* divide-free range decoder reading through the blocking FIFO. */
typedef struct { uint32_t range, code; } SDec;
static SDec RC;
static void rc_init(void){
    RC.range = 0xFFFFFFFFu; RC.code = 0;
    /* The encoder drops the always-zero LZMA leading cache byte from the wire, so the
     * 4 code bytes are read directly (no skip). */
    for (int i=0;i<4;i++) RC.code = (RC.code<<8) | next_byte();
}
/* range-coder DECODE core shared by both the adaptive bit and the raw (equiprobable) bit: split at
 * `bound`, pick the sub-interval the code lands in, return the bit, then renorm (refill from the FIFO
 * until range climbs back above KTOP). The readers differ ONLY in `bound` + probability adaptation. */
static int rc_decode(uint32_t bound){
    uint32_t code=RC.code, range=RC.range;
    int b;
    if (code<bound){ range=bound; b=0; } else { code-=bound; range-=bound; b=1; }
    while (range<RC_KTOP){ code=(code<<8)|next_byte(); range<<=8; }
    RC.code=code; RC.range=range;
    return b;
}
static int s_bit_r(uint16_t*prob,int rate){
    uint32_t p=*prob;
    int b=rc_decode((RC.range>>12)*p);
    *prob=(uint16_t)(b? p-(p>>rate) : p+((RC_PBIT-p)>>rate));
    return b;
}
/* Default adaptive-bit rate for the Golomb (unary+mantissa), order-2 flag, and MTF
 * rep/hit streams. Tuned to 4 (1/16) — these low-cardinality, fast-drifting contexts
 * track better at 4 than the LZMA-classic 5; literal/dval bit-trees keep their own
 * per-tree rate via s_bit_r. Must match RC_S_BIT_RATE in rc_v3_enc.c (bit-exact wire). */
#define RC_S_BIT_RATE 4
static int s_bit(uint16_t*prob){ return s_bit_r(prob,RC_S_BIT_RATE); }
static int s_raw(void){ return rc_decode(RC.range>>1); }
/* CRASH-HARDENING (fuzz gate): a corrupt/truncated stream yields zero-fill past EOF,
 * which can drive the unbounded unary loops below forever (hang) or shift a value by >=32 bits
 * (UB). Every unbounded loop is capped to the max a 32-bit value needs; on overflow set g_rcerr
 * and bail (the apply checks err -> clean reject, never crash / never silent-wrong). */
static uint8_t g_rcerr;
/* rob-1: distinguishable reject reason (read by the host main after a reject). 1 =
 * RESOURCE: a corpus-overfit cap was exceeded (journal full / per-op cap / dict cap) — this firmware
 * is bigger than the build was sized for, raise the cap (costs SRAM). 2 = CORRUPT: malformed/truncated
 * stream (bounds, underrun, range-coder overflow). 0 = none. Pure diagnostic; never affects decoding. */
enum { REJ_NONE=0, REJ_RESOURCE=1, REJ_CORRUPT=2 };
static uint8_t g_reject;
#define RC_UNARY_MAX 31           /* a uint32 value needs <=31 leading/unary bits */
static uint32_t s_raw_bits(int nb){ uint32_t v=0; for(int i=0;i<nb;i++) v=(v<<1)|s_raw(); return v; }
/* raw (no-model) Elias-gamma minus 1: reads gamma value v>=1, returns v-1. The only raw-gamma site
 * is the header op count, so the gamma reader and its -1 wrapper are merged into one function. */
static uint32_t s_raw_gz(void){
    int n=0; while(s_raw()==0){ if(++n>RC_UNARY_MAX){ g_rcerr=1; return 0; } }
    return ((1u<<n) | s_raw_bits(n)) - 1u; }   /* mantissa via s_raw_bits (was a duplicate bit loop) */
/* ---- bit-tree byte ---- */
static int s_bt(BitTree*t,int rate){
    int m=1;
    for(int i=0;i<8;i++){
        uint16_t p=bt_get(t,m-1);
        int b=s_bit_r(&p,rate);
        bt_set(t,m-1,p);
        m=(m<<1)|b;
    }
    return m-256;
}
/* ---- ByteVarint (pack_size) ---- */
static int32_t s_bv(BitTree*t,int rate){
    int b0=s_bt(t,rate); int sgn=b0&0x40; uint32_t val=b0&0x3f; int off=6;
    while(b0&0x80){
        if(off>28){ g_rcerr=1; return 0; }
        b0=s_bt(t,rate);
        uint32_t chunk=(uint32_t)(b0&0x7f);
        if(off>=32 || chunk>=(1u<<(32-off))){ g_rcerr=1; return 0; }
        val|=chunk<<off; off+=7;
    }
    return sgn? (int32_t)(0u-val) : (int32_t)val;
}
/* ---- UGolomb (uses UG_CTX from rc_models.h) ----
 * Crash-hardening: cap the adaptive unary prefix. For GAMMA ('g') the prefix is the bit-length
 * of (value+1), so <=31 for any uint32 (RC_UNARY_MAX). For RICE ('r') the prefix is the QUOTIENT
 * value>>k, which for a legit field (e.g. backref_dist <= window 2^SA_W with no-split, W=11 =>
 * up to ~64 at small k) can far exceed 31 — cap it much higher (1<<20) so valid streams decode
 * while a corrupt run-on is still bounded (the mantissa shift below caps the magnitude anyway). */
#define RC_RICE_UNARY_MAX (1u<<20)
typedef struct { uint8_t k; uint16_t u[UG_CTX+1]; uint16_t m[UG_CTX+1][UG_CTX+1]; } UGRice;
/* Packed gamma mantissa layout (a RAM win vs the encoder's square (UG_CTX+1)^2 array): the only
 * (cl_ctx, pos_ctx) pairs that ever occur are UG_C(pos) for pos in 0..cl-1 — a TRIANGLE, not a
 * square — so we pack one row per clamped cl level into a flat m[], using UG_GAMMA_BASE(c) as the
 * row offset. Row widths: c==0 reserves 1 slot (no mantissa bits, but keep row offsets distinct);
 * 1<=c<UG_CTX uses c slots (pos contexts 0..c-1, all distinct); c==UG_CTX uses UG_CTX+1 slots (the
 * catch-all top level, where pos clamps into 0..UG_CTX). The row start has the closed form
 *   UG_GAMMA_BASE(c) = (c==0) ? 0 : 1 + c*(c-1)/2
 * valid for ANY UG_CTX, and UG_GAMMA_M = top-row start + its width. These derive the layout from
 * UG_CTX (no hand-typed offsets in the size/total), and the _Static_assert below pins the runtime
 * base[] table to the same formula — so changing UG_CTX is caught at COMPILE time (a clear assert)
 * instead of being silently miscomputed. This is bit-exact identical to the historical layout. */
#define UG_GAMMA_BASE(c) ((c)==0 ? 0 : 1 + ((c)*((c)-1))/2)
#define UG_GAMMA_M (UG_GAMMA_BASE(UG_CTX) + (UG_CTX+1))
typedef struct { uint16_t u[UG_CTX+1]; uint16_t m[UG_GAMMA_M]; } UGGamma;
/* Compile-time guard for the default packed gamma layout. */
_Static_assert(UG_GAMMA_BASE(0)==0 && UG_GAMMA_BASE(1)==1 && UG_GAMMA_BASE(2)==2 &&
               UG_GAMMA_BASE(3)==4 && UG_GAMMA_BASE(4)==7 && UG_GAMMA_BASE(5)==11 &&
               UG_GAMMA_BASE(6)==16, "UG_GAMMA_BASE formula drifted from the packed gamma layout");
static void ugr_init(UGRice*g,int k){
    g->k=(uint8_t)k;
    for(int i=0;i<=UG_CTX;i++){ g->u[i]=RC_PHALF; for(int j=0;j<=UG_CTX;j++) g->m[i][j]=RC_PHALF; }
}
/* init a gamma model, seeding EVERY unary-prefix prior to `useed` (the mantissa priors are always
 * RC_PHALF). useed==RC_PHALF is the neutral init; useed==RC_PBIT-RC_PBIT/4 biases toward STOP (bit 0)
 * so the smallest gamma values are cheap from the first symbol — used by the MTF dict-index gammas
 * (dibl/diex), where the just-promoted index 1 (encoded value 0) dominates (RC_PBIT-RC_PBIT/4 was the
 * corpus optimum). Folding the u[] seed into the init avoids a second pass over u[]. */
static void ugg_init_u(UGGamma*g,uint16_t useed){
    for(int i=0;i<=UG_CTX;i++) g->u[i]=useed;
    for(int i=0;i<UG_GAMMA_M;i++) g->m[i]=RC_PHALF;
}
static void ugg_init(UGGamma*g){ ugg_init_u(g,RC_PHALF); }
/* seed the unary-prefix priors of a gamma model toward "continue" for the first `depth` context
 * positions. Used by per-op geometry (diff_len/adj): firmware delta op magnitudes are essentially
 * never tiny, so cl>=depth almost always — this is a structural prior, NOT a corpus cap, and it
 * makes the very first op (e.g. a one-face update with few ops) as cheap as the warmed-up state. */
static void ugg_seed_cont(UGGamma*g,int depth){
    for(int i=0;i<depth && i<=UG_CTX;i++) g->u[i]=(uint16_t)(RC_PBIT/16);  /* low p == bit1(continue) cheap */
}
/* shared adaptive unary prefix: read 1-bits on the per-clamped-level priors u[] until a 0 bit,
 * returning the run length cl (== the value's bit-length class). `cap` bounds the run so a corrupt
 * zero-fill stream can't spin forever / shift by >=32 (RC_UNARY_MAX for gamma, RC_RICE_UNARY_MAX for
 * rice); on overflow it sets g_rcerr and returns 0 so the mantissa loop is a no-op and the apply
 * cleanly rejects (same effect as the old per-function early return). Bit-exact for valid streams. */
/* shared adaptive unary prefix for BOTH the Golomb (Rice/Gamma) prefix and the MTF dict-index code:
 * read 1-bits on the per-position priors u[min(pos,clampmax)] until a 0-bit, `cap`-bounded against a
 * corrupt run-on. clampmax==UG_CTX(=6) reproduces UG_C over the 7-entry Golomb u[]; clampmax==IDX_CTX-1
 * (=4) reproduces the index code's min(pos,4) over the 5-entry u[]. Bit-exact for both call families. */
static uint32_t s_unary(uint16_t*u,uint32_t clampmax,uint32_t cap){
    uint32_t cl=0; while(s_bit(&u[cl<clampmax?cl:clampmax])==1){ if(++cl>cap){ g_rcerr=1; return 0; } }
    return cl;
}
/* shared mantissa: read `cnt` adaptive bits MSB-first from the per-clamped-position priors row[], the
 * row already selected by caller (rice: m[UG_C(cl)] full square row; gamma: m[UG_GAMMA_BASE(UG_C(cl))]
 * packed-triangle row). Bit-exact identical to the inlined per-function loops. */
static uint32_t s_ug_mant(uint16_t*row,int cnt){
    uint32_t v=0; for(int pos=0;pos<cnt;pos++) v|=(uint32_t)s_bit(&row[UG_C(pos)])<<(cnt-1-pos);
    return v;
}
static uint32_t s_ug_rice(UGRice*g){
    uint32_t cl=s_unary(g->u,UG_CTX,RC_RICE_UNARY_MAX);
    return (cl<<g->k) | s_ug_mant(g->m[UG_C((int)cl)],g->k);
}
static uint32_t s_ug_gamma(UGGamma*g){
    int cl=(int)s_unary(g->u,UG_CTX,RC_UNARY_MAX);
    return ((1u<<cl) | s_ug_mant(&g->m[UG_GAMMA_BASE(UG_C(cl))],cl)) - 1u;
}
/* MTF dict-index model: a lean adaptive UNARY code (replaces the per-stream UGGamma, ~60B each).
 * The encoded index value v (== dict pos j-1) is ~54% zero, ~22% one, ~10% two, with a thin tail
 * to ~140 worst case. Unary fits this concentration: emit v continue-bits then a stop-bit, each on
 * the per-position prior idx[min(pos,IDX_CTX-1)]; tail positions share the last prior so the model
 * stays tiny. IDX_CTX=5 is the corpus optimum (4/6/8/16 all code worse: too few distinct head priors
 * or too-sparse tail priors). `cap` bounds the run on a corrupt stream (pull_delta validates j vs K). */
#define IDX_CTX 5
typedef struct { uint16_t u[IDX_CTX]; } IdxUnary;
static void idx_init(IdxUnary*g,uint16_t seed){ for(int i=0;i<IDX_CTX;i++) g->u[i]=seed; }
/* ---- order-2 flag ---- */
static int s_flag(Flag1*f){ int b=s_bit(&f->m[f->h]); f->h=((f->h<<1)|b)&3; return b; }

/* ===================================================================================== */
/* CRC32 (zlib polynomial) over flash bytes                                               */
/* ===================================================================================== */
static uint32_t crc32_flash(uint32_t n){
    uint32_t c=0xffffffffu;
    for(uint32_t i=0;i<n;i++){ c^=flash_read(i);
        for(int k=0;k<8;k++) c=(c>>1)^(0xedb88320u & (-(int32_t)(c&1))); }
    return c^0xffffffffu;
}

/* ===================================================================================== */
/* never-evict journal — PACKED 3-BYTE PAGED scheme. Each entry is (pos, byte). Storing the    */
/* full pos (needs 18 bits for an image span up to ~256 KB) + the byte would be a 4 B slot. We  */
/* store only the LOW 16 bits of pos + the byte = exactly 3 B/slot, and amortise the HIGH pos   */
/* bits across a tiny PAGE TABLE: slots are kept sorted by full pos, so each 64 KB page is one  */
/* contiguous run and its slots share the same high bits. NEVER-EVICT: the first write to a pos */
/* wins; over-depth (would exceed JSLOTS) is REFUSED before any write. Lives in the apply phase */
/* ONLY (overlaid in ARENA front).                                                             */
/* ===================================================================================== */
/* DEREL dict cap (corpus distinct-value peak ~179 per substream + margin). The STREAMED-DELTA wire
 * (12 KiB build) holds NO resident per-detection store — only a small MOVE-TO-FRONT dict of the
 * distinct delta values (the frequently-repeated relocation offsets keep tiny MTF indices). */
/* Caps are corpus-peak + margin; over-cap input is REJECTED (CRC-gated, never silent-wrong), not
 * applied. All -D overridable (#ifndef) so a deployment with a known firmware family can re-tune
 * them; raising a cap costs SRAM (see ../RESULT.md "Known limitations" for per-cap thresholds). */
#ifndef DR_KCAP_BL
#define DR_KCAP_BL  208        /* max distinct bl delta values (corpus peak 180; +28 margin) */
#endif
#ifndef DR_KCAP_EX
#define DR_KCAP_EX  128        /* max distinct ex delta values (corpus peak 106; +22 margin) */
#endif
#ifndef OPC_CAP
#define OPC_CAP 80            /* per-op correction cap (corpus peak 68; +12 margin) */
#endif
/* Suppressed BL positions are implicit: a BL-looking field with any literal patch byte (`!pure`) is
 * a normal 4-byte copy. No sbl offset stream or resident gap buffer is needed. */
/* STREAMED-DELTA per-stream state (bl, ex): a MOVE-TO-FRONT dict of distinct delta values + an
 * adaptive "repeat-last" bit + an adaptive "dict-hit" bit. Each delta on the wire: rep-bit (==last?)
 * | else hit-bit (in dict? -> MTF index via the index UGolomb | else escape value via M_dval, MTF-
 * inserted at front). The dict array is outside the struct so bl/ex get separate caps
 * (distinct-value peaks: bl 180, ex 106). */
typedef struct {
    uint16_t K;                       /* MTF dict entries in use (index 0 = most-recently-used) */
    uint16_t rep[4], hit; uint8_t rh; /* adaptive binary models (P(bit==0)); rep keyed by prev repeat + last==0 */
} DRStream;
#define DR_HIT_INIT 576u              /* zero-seeded MTF dict makes hit-bit==1 likely; 576 is the
                                       * corpus optimum that still holds the one-face revert at 582
                                       * (higher seeds nick it to 583). Must match rc_v3_enc.c. */

#ifndef JSLOTS
#define JSLOTS 904u                          /* packed journal capacity; corpus peak is 625, and the cap
                                              * is kept well above it (the 8-byte-aligned journal region
                                              * holds 904 slots) for out-of-corpus headroom. Refuse above. */
#endif
#define JREGION (((JSLOTS*3u)+7u)&~7u)        /* journal byte region (2712, 8-aligned) */
#define JPAGE_MAX 6                           /* page-table size (covers span up to 6*64 KB = 384 KB; corpus 216 KB = 4 pages) */
/* LZSS window W (defined here so SA_ARENA can size the apply phase). W=10 (ring 1024) is the
 * default that keeps the decoder within the 12 KiB SRAM cap; MUST match the encoder W. */
#ifndef SA_W
#define SA_W 10
#endif
/* HYBRID ARENA layout (apply phase): [journal JREGION][SA apply SA_ARENA]. The STREAMED-DELTA wire
 * holds NO resident per-detection store; the small MTF-dict streams (DR_BL/DR_EX), the M_dval
 * escape-value bit-tree, and the two index UGolombs are SEPARATE statics (~2.4 KB) — far smaller than
 * the old 8,080 B generator stores. Seed phase (hist0/hist1/w = 4096 B) overlays ARENA front. */
/* SA apply state = ring(2^W) + fixed correction arrays + the SA scalar/control fields. A1 derives
 * ldr positions per op and infers suppressed BL from `!pure`, so no ex/sbl offset buffers are
 * resident. The reservation is the two buffer caps (ring 2^W, op_corr 4*OPC_CAP) plus the non-buffer
 * fields: 8 u32 (ototal/tok_left/span_left/br_left/br_src/tp/fp/op_nc = 32 B) +
 * 2 u8 (tok_mode/err) struct-padded and rounded to 40 B to keep ARENA_BYTES 8-aligned. The
 * _Static_assert(sizeof(SA) <= SA_ARENA) below is the hard guard if a field is ever added. */
#define SA_ARENA ((1u<<SA_W) + 4u*OPC_CAP + 40u)
#define ARENA_BYTES (JREGION + SA_ARENA)
static unsigned char ARENA[ARENA_BYTES] __attribute__((aligned(8)));
static uint8_t *const Jbuf = (uint8_t*)ARENA;        /* packed journal (apply phase): JSLOTS*3 bytes */
/* page[p] = index of the first slot whose (pos>>16) >= p, for p in [0..JPAGE_MAX].  Since slots
 * are sorted by full pos, page p occupies the contiguous run [page[p], page[p+1]). Every journalled
 * pos is < g_image_span, so a pos with pos>>16 >= JPAGE_MAX exists only for out-of-corpus firmware
 * whose span overflows the page table — that page is unrepresentable and refused. No separate active-
 * page count is kept: pages above the live span never receive an insert, so their boundaries trail
 * the final sentinel and search empty. */
static uint16_t g_jpage[JPAGE_MAX+1];
static void jr_reset(void){
    for(int p=0;p<=JPAGE_MAX;p++) g_jpage[p]=0; }
/* Binary search for `pos` within its page (pos>>16), comparing the 16-bit low key. Returns the
 * slot index in *at: on a hit *at is the matching slot and the return is 1; on a miss *at is the
 * sorted insertion index (page-clamped) and the return is 0. `pos` is always inside the paged span
 * (callers gate on pg<JPAGE_MAX), so no out-of-range branch is needed here. */
static int jr_find(uint32_t pos, int*at){
    uint32_t key=pos&0xFFFFu;
    int lo=g_jpage[pos>>16], hi=g_jpage[(pos>>16)+1]-1;
    while(lo<=hi){ int mid=(int)(((unsigned)lo+(unsigned)hi)>>1);
        const uint8_t*s=Jbuf+(uint32_t)mid*3u; uint32_t k=(uint32_t)s[1]|((uint32_t)s[2]<<8);
        if(k==key){ *at=mid; return 1; } if(k<key) lo=mid+1; else hi=mid-1; }
    *at=lo;
    return 0;
}
static int jr_put(uint32_t pos, uint8_t b){
    uint32_t pg=pos>>16; if(pg>=(uint32_t)JPAGE_MAX) return -1;   /* outside paged span (also bounds pos) */
    int at;
    if(jr_find(pos,&at)) return 0;                   /* never-evict: keep the first write */
    uint16_t jcount=g_jpage[JPAGE_MAX];
    if((uint32_t)jcount>=JSLOTS) return -1;          /* over-depth: refuse BEFORE any write */
    if(at<jcount) memmove(Jbuf+(uint32_t)(at+1)*3u, Jbuf+(uint32_t)at*3u, (size_t)(jcount-at)*3u);
    uint8_t*s=Jbuf+(uint32_t)at*3u; s[0]=b; s[1]=(uint8_t)pos; s[2]=(uint8_t)(pos>>8);
    for(uint32_t p=pg+1u;p<=(uint32_t)JPAGE_MAX;p++) g_jpage[p]++;  /* shift higher page boundaries */
    return 0;
}
static int jr_get(uint32_t pos, uint8_t*out){
    if(pos>>16>=(uint32_t)JPAGE_MAX) return 0;       /* outside paged span */
    int at;
    if(jr_find(pos,&at)){ *out=Jbuf[(uint32_t)at*3u]; return 1; }
    return 0;
}

/* ===================================================================================== */
/* relocation unpack/pack (Thumb bl + s32) — de-relocation override only (no flash write).      */
/* The journal-aware pristine reads + the bl/ldr predicates live with the apply state below.    */
/* ===================================================================================== */
/* de-relocate a Thumb BL halfword pair (imm24 = imm - delta), repacked into 4 bytes (no flash write).
 * j1/j2 are the s-conditioned complements of the imm's i1/i2 bits; the subtract is mod 2^24 (masked
 * & repacked immediately) so no sign-extension is needed. */
static void bl_dereloc(uint16_t up, uint16_t lo, uint32_t delta, uint8_t out[4]){
    uint32_t s=(up>>10)&1u, i1=1u-(((lo>>13)&1u)^s), i2=1u-(((lo>>11)&1u)^s);
    uint32_t imm24=(((up&0x3ffu)<<11)|(lo&0x7ffu)|(s<<23)|(i1<<22)|(i2<<21)) - delta;
    imm24&=0x00ffffffu; s=(imm24>>23)&1u;
    uint32_t j1=1u-(((imm24>>22)&1u)^s), j2=1u-(((imm24>>21)&1u)^s);
    uint16_t u=(uint16_t)(0xF000u|(s<<10)|((imm24>>11)&0x3ffu));
    uint16_t l=(uint16_t)(0xD000u|(j1<<13)|(j2<<11)|(imm24&0x7ffu));
    out[0]=u&0xff; out[1]=(u>>8)&0xff; out[2]=l&0xff; out[3]=(l>>8)&0xff;
}

/* ===================================================================================== */
/* HYBRID: monotonic output ROW write-back cache (real flash = row-erase 256 B).            */
/* Output writes are monotonic & contiguous (asc shrink / desc grow); SOURCE reads stay raw  */
/* (the never-evict journal covers read-after-overwrite). One resident output row buffer: a   */
/* write to a NEW row first commits the previous row (one erase+program == 1 write/row, so    */
/* nvm_rows_amplified=0); reads of the dirty buffered row are served from RAM. Source reads of */
/* not-yet-overwritten flash go straight to physical flash (free). 256 B SRAM.                */
/* ===================================================================================== */
#ifndef OUTROW
#ifdef NVM_ROW
#define OUTROW NVM_ROW
#else
#define OUTROW 256u
#endif
#endif
static uint8_t  g_orow_buf[OUTROW];
#define OROW_NONE UINT32_MAX
static uint32_t g_orow_base;   /* current resident output row base, or OROW_NONE */
static uint8_t  g_orow_dirty;
static void orow_commit(void){
    if(g_orow_base!=OROW_NONE && g_orow_dirty){
        uint32_t base=g_orow_base, end=base+OUTROW; if(end>g_image_span) end=g_image_span;
        /* force the (at most one) row erase up front, then program from the RAM buffer so every
         * byte is a pure 1->0 program (no further erase). All bytes of the row were produced by the
         * monotonic output, so the buffer holds the exact final content. */
        for(uint32_t a=base;a<end;a++){ if(flash_read(a)!=0xFFu){ flash_write(a,0xFFu); break; } }
        for(uint32_t a=base;a<end;a++) flash_write(a, g_orow_buf[a-base]);
    }
    g_orow_dirty=0; g_orow_base=OROW_NONE;
}
static void orow_reset(void){ g_orow_base=OROW_NONE; g_orow_dirty=0; }
/* OUTPUT write: buffer in the resident row, committing the previous row on a row change. */
static void out_write(uint32_t a, uint8_t v){
    if(a>=g_image_span) return;
    uint32_t base=(a/OUTROW)*OUTROW;
    if(base!=g_orow_base){
        orow_commit();
        uint32_t end=base+OUTROW; if(end>g_image_span) end=g_image_span;
        for(uint32_t x=base;x<end;x++) g_orow_buf[x-base]=flash_read(x); /* preload (source not yet erased) */
        g_orow_base=base;   /* orow_commit() above already cleared g_orow_dirty */
    }
    uint32_t off=a-g_orow_base;
    if(g_orow_buf[off]!=v){ g_orow_buf[off]=v; g_orow_dirty=1; }
}
/* ===================================================================================== */
/* entropy models (all live through the single streamed apply): tc/gd/gl/gs (content) +          */
/* pg/pgn/pg2 (preserve+correction, SHARED) + dibl/diex (streamed-delta MTF dict-index Golombs). M_dval */
/* (escape values) + DR_BL/DR_EX (MTF dicts) are separate statics. */
static UGRice  M_gd;             /* token_count first, then reinitialized as backref_dist */
static UGGamma M_gl, M_gs;       /* backref_len_v3 (len-1), span_len */
static UGGamma M_pg;             /* preserve/correction FIRST gaps, SHARED (corr gaps are the same
                                  * op-relative offset distribution; one model adapts on both). */
static UGGamma M_pgn;            /* preserve/correction COUNTS (np/nc), SHARED — separated from gaps so
                                  * the dominant count=0 case and the dominant gap=1 case stop fighting
                                  * over the shared adaptive unary/mantissa probabilities. */
static UGGamma M_pg2;            /* preserve/correction REST gaps (2nd..Nth), SHARED. The first gap is
                                  * an op-relative offset (broad), but later gaps are ~98%/77% value=1
                                  * (consecutive run); a single shared model converges hard on that. */
static UGGamma M_gdl, M_gel, M_gadj; /* per-op geometry: diff_len, extra_len, zz(adj). Were raw dz
                                  * (no model); their bit-length distributions are concentrated, so an
                                  * adaptive gamma UGolomb beats the fixed raw code. */
static IdxUnary M_dibl, M_diex;  /* BL/EX MTF dict indices (lean unary code) */
/* tag0 (diff-literal/even-extra) literal trees split by previous-literal range (LIT0_SEL);
 * tag1 (odd-parity extra) keeps a single tree. g_litprev = last literal byte emitted (any tag),
 * reset to 0 per blob; mirrors rc_v3_enc encode_body prevlit. */
#define LIT0_CTX 5
/* tag0 context from the previous literal: 7 contiguous prevlit regions folded onto
 * 5 trees (a non-monotone region->tree map; one tree is shared by two disjoint ranges).
 * The region cuts and the fold were derived by minimising the conditional entropy of the
 * tag0-literal distribution over the firmware corpus (DP over cuts + optimal tree merge).
 * prevlit==0x00 (zero-runs, ~8% of literals) and ==0xf7 (high-byte/0xff region) are their
 * own contexts; the rest fold so the high range reuses tree 1. Affects compression ratio
 * only, never correctness; all 5 trees seed from the same parity-0 histogram. */
#define LIT0_SEL(p) ( (p)==0 ? 0 : (p)<0x20 ? 1 : (p)<0x3d ? 0 : (p)<0x90 ? 2 : (p)<0xf7 ? 4 : (p)==0xf7 ? 3 : 1 )
static BitTree M_lit0[LIT0_CTX], M_lit1;
static uint8_t g_litprev;
static BitTree M_dval;             /* shared byte tree for DEREL escape bytes and [C] correction bytes */
static Flag1   M_flag;
/* rep0: adaptive flag before a match distance. =1 reuses the immediately-previous match distance
 * (distance value omitted); =0 codes a fresh distance. Seeded toward 0 (RC_REP0_INIT, P(reuse)~1/4)
 * because exact distance reuse is the minority case: a low prior keeps the dominant =0 flag near-free
 * on small patches while corpus-scale streams adapt up. g_lastdist holds the last match distance.
 * Mirrors rc_v3_enc Models.rep0/last_dist (bit-exact wire). */
#define RC_REP0_INIT (RC_PBIT - (RC_PBIT>>2))   /* 3072: P(rep0=1) prior ~ 1/4. rep0=1 reuses the
                                                   * previous match distance; corpus tuning (paired
                                                   * min-over-N sweep) puts the optimum that does NOT
                                                   * regress the one-face product patch at ~1/4 (1/8
                                                   * was the old value; 3/8 helps corpus more but
                                                   * regresses the real one-face update by +1/+1). */
static uint16_t M_rep0[2];   /* order-1 on previous rep0 outcome: rep0 runs cluster */
static int g_rep0h;          /* last rep0 bit (context) */
static uint32_t g_lastdist;
static int32_t DR_DIC_BL[DR_KCAP_BL], DR_DIC_EX[DR_KCAP_EX];   /* MTF dict arrays (separate caps) */
static DRStream DR_BL, DR_EX;      /* the two streamed-delta MTF states (resident) */

/* ===================================================================================== */
/* HYBRID STREAMED DELTAS (replaces the two resident generator stores; 12 KiB build). Each field's */
/* delta VALUE is pulled INLINE from the single range stream the instant the apply detects the      */
/* field (the field TYPE is known from detection, so no untyped up-front decode is needed). Per      */
/* stream we keep only a small MOVE-TO-FRONT dict of the distinct delta values (the frequently-      */
/* repeated relocation offsets keep tiny MTF indices) + a repeat-last bit keyed by previous repeat   */
/* and last-is-zero + a dict-hit bit. This fixed stream state replaces the old resident stores.      */
/* ===================================================================================== */
/* A1 derives bl/ldr positions on-device, so there is no on-device disassembler
 * and no ranges side table. */
/* pack s32 (ldr/data/code de-reloc) into 4 little-endian bytes (no flash write) */
static void pack_s32_buf(uint32_t v, uint8_t out[4]){ out[0]=v&0xff; out[1]=(v>>8)&0xff; out[2]=(v>>16)&0xff; out[3]=(v>>24)&0xff; }

static void dr_init(DRStream*d, int32_t*dic){
    d->K=1; dic[0]=0;
    for(int i=0;i<4;i++) d->rep[i]=RC_PHALF;
    d->rh=0; d->hit=DR_HIT_INIT;
}
/* pull the next delta of a stream (bl/ex), inline:
 *   rep-bit: ==1 -> return last; else read hit-bit:
 *     hit==1 -> MTF index via the index UGolomb (gix); v=dic[j]; move dic[j] to front.
 *     hit==0 -> escape value via M_dval; insert v at MTF front.
 *   then last=v. */
static int32_t pull_delta(DRStream*d, IdxUnary*gix, int32_t*dic, uint32_t cap){
    { int ri=d->rh | (dic[0]==0 ? 2 : 0);
      int rb=s_bit(&d->rep[ri]); d->rh=rb; if(rb==1) return dic[0]; }  /* repeat-last, order-1 + zero ctx */
    int32_t v;
    if(s_bit(&d->hit)==1){
        uint32_t j=s_unary(gix->u,IDX_CTX-1,cap)+1u;    /* cmp-1: dict idx 0 unreachable -> encode j-1 */
        if(j>=(uint32_t)d->K){ g_rcerr=1; return 0; }
        v=dic[j];
        if(j){ int32_t t=dic[j]; memmove(&dic[1], &dic[0], (size_t)j * sizeof(dic[0])); dic[0]=t; }
    } else {
        v=s_bv(&M_dval, 4);
        if((uint32_t)d->K>=cap){ g_rcerr=1; g_reject=REJ_RESOURCE; return 0; }   /* distinct-value cap -> reject */
        memmove(&dic[1], &dic[0], (size_t)d->K * sizeof(dic[0]));
        dic[0]=v; d->K++;
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
#define SA_RING (1u<<SA_W)
#define SA_MASK (SA_RING-1u)
typedef struct {
    uint8_t ring[SA_RING]; uint32_t ototal;       /* content history (masked) + total produced */
    /* pauseable LZSS token replay state (pull-driven content producer) */
    uint32_t tok_left;                            /* tokens remaining (token_count) */
    uint32_t span_left, br_left; uint32_t br_src; /* br_src is an absolute ototal index */
    int32_t tp, fp;                               /* running accumulators (apply order) */
    /* per-op corrections (sorted by offset; cursor by binary search). count-bounded, NOT op-size.
     * Packed as high 24 bits = op-local offset, low 8 bits = additive correction byte. */
    uint32_t op_corr[OPC_CAP]; int32_t op_nc;
    uint8_t tok_mode;                             /* 0=idle, 1=span, 2=backref */
    uint8_t err;
} SA;
/* SA apply state overlaid in ARENA right after the journal (both live only in the apply phase). */
#define SAst (*(SA*)(ARENA + JREGION))
_Static_assert(sizeof(SA) <= SA_ARENA, "SA apply state exceeds its ARENA reservation");
_Static_assert(4096u <= ARENA_BYTES, "seed scratch (hist0+hist1+w) exceeds ARENA");

static int32_t bb_unzz(uint32_t u){
    if(u==0xffffffffu){ g_rcerr=1; return 0; }
    return (u&1u)? -(int32_t)((u+1u)>>1) : (int32_t)(u>>1);
}
/* correction lookup by op-local write-offset (small array; unique offsets => linear scan) */
static int32_t corr_at(SA*s, int32_t off){
    for(int32_t i=0;i<s->op_nc;i++){
        if((int32_t)(s->op_corr[i]>>8)==off) return (int32_t)(s->op_corr[i]&0xffu);
    }
    return 0;
}
/* A1 ldr-derive: for FWD, a 1024-byte pristine window is represented as 512 even-halfword slots:
 * imm8 byte + one "is Thumb LDR literal" bit. The ldr back-scan never needs the full instruction
 * halfword after classification. Field probes PEEK at the current 4-byte field before recording it,
 * preserving the old fpk/fpk-1024 ring alias timing. Grow reads via the journal-aware source path
 * because lower instruction-window bytes may be clobbered. */
#define PSRC_HW_MASK 511u
static uint8_t g_psrc_imm[512], g_psrc_ldr[64];
static uint8_t hy_src(int32_t fp);   /* fwd decl: journal-aware pristine source read */
/* A1 ldr-derive (SAME-OP): is the 4-aligned from-addr fpk an ldr literal target of an instruction
 * IN THIS op [fp0,fp0+dl)? Scan even a in [max(fp0,fpk-1024), fpk-2]; an ldr literal
 * `(up&0xf800)==0x4800` targets t=(a&~3)+4*(up&0xff)+4; field iff some a targets fpk and
 * fpk+4<=fp0+dl. Reads PRISTINE source: the FWD and grow apply directions share ONE back-scan and
 * differ ONLY in the halfword source — FWD reads the packed LDR metadata recorded at read-time, grow
 * reads via hy_src() (journal-aware: instr-window bytes the output frontier already clobbered are
 * preserved copy source, so raw flash would be wrong). s==NULL selects the FWD metadata path. Kept
 * static-inline so the per-direction branch folds away at each call site (no indirect call / no text
 * bloat on Cortex-M0+). No resident target store. */
static inline int ldr_targets(SA*s, int32_t fp0, int32_t dl, uint32_t fpk){
    /* the scan only ever yields 4-aligned targets (t=(a&~3)+4n+4), so a non-aligned fpk can never
     * match -> reject it up front (also keeps the caller's EX gate a single ldr_targets() test, and
     * skips any pristine read/record for an impossible target). */
    if(fpk&3u) return 0;
    int32_t hi=fp0+dl; if((int32_t)fpk+4>hi) return 0;
    int32_t lo=(int32_t)fpk-1024; if(lo<fp0) lo=fp0; if(lo&1) lo++;
    for(int32_t a=lo; a+2<=(int32_t)fpk; a+=2){
        int32_t imm;
        if(s){
            uint16_t up=(uint16_t)(hy_src(a) | (hy_src(a+1)<<8));
            if((up&0xf800)!=0x4800) continue;
            imm=(int32_t)(up&0xff);
        } else {
            uint32_t i=((uint32_t)a>>1)&PSRC_HW_MASK;
            if(!(g_psrc_ldr[i>>3]&(uint8_t)(1u<<(i&7u)))) continue;
            imm=(int32_t)g_psrc_imm[i];
        }
        if((a&~3)+4*imm+4==(int32_t)fpk) return 1;
    }
    return 0;
}
static void sa_emit_ring(SA*s, uint8_t b){ s->ring[s->ototal & SA_MASK]=b; s->ototal++; }
/* pull the next CONTENT byte from the cut LZSS token stream, decoding tokens lazily. */
static uint8_t sa_next_content(SA*s, int tag){
    for(;;){
        if(g_rcerr) goto fail;
        if(s->tok_mode==1 && s->span_left>0){           /* span: decode one literal byte */
            int b=s_bt(tag?&M_lit1:&M_lit0[LIT0_SEL(g_litprev)], tag?4:5);
            g_litprev=(uint8_t)b;
            sa_emit_ring(s,(uint8_t)b); s->span_left--;
            if(s->span_left==0) s->tok_mode=0;
            return (uint8_t)b;
        }
        if(s->tok_mode==2 && s->br_left>0){              /* backref: copy from ring */
            uint8_t b=s->ring[s->br_src & SA_MASK]; sa_emit_ring(s,b); s->br_src++; s->br_left--;
            if(s->br_left==0) s->tok_mode=0;
            return b;
        }
        if(s->tok_left==0) goto fail;   /* content underrun -> reject */
        if(s_flag(&M_flag)==0){
            uint32_t ln=s_ug_gamma(&M_gs)+1u;
            s->tok_mode=1; s->span_left=ln;
        } else {
            uint32_t d;
            int rb=s_bit(&M_rep0[g_rep0h]); g_rep0h=rb;
            if(rb){ d=g_lastdist; }                         /* rep0: reuse last distance */
            else { d=s_ug_rice(&M_gd)+1u; g_lastdist=d; }
            uint32_t ln=s_ug_gamma(&M_gl)+1u;
            if(d==0 || d>s->ototal || d-1u>=SA_RING) goto fail; /* reject before-start / ring-overrun */
            s->tok_mode=2; s->br_src=s->ototal-d; s->br_left=ln;
        }
        s->tok_left--;
    }
fail:
    s->err=1;
    return 0;
}
/* read a uLEB from the content stream (one byte pulled per iteration from the LZSS token replay). */
static uint32_t sa_read_uleb(SA*s){
    uint32_t acc=0; int sh=0;
    for(;;){ if(s->err||g_rcerr) return 0;
        uint8_t b=sa_next_content(s,0);
        if(sh>28){ s->err=1; return 0; }            /* cap shift (uLEB <=32-bit) */
        acc|=(uint32_t)(b&0x7f)<<sh; sh+=7;
        if(!(b&0x80)) return acc; }
}
/* journal one preserve site (old flash byte captured BEFORE any write of this op). */
static void sa_journal(SA*s, int32_t tp){
    if(tp>=0 && tp<(int32_t)g_from_size){ if(jr_put((uint32_t)tp, flash_read((uint32_t)tp))){ s->err=1; g_reject=REJ_RESOURCE; }
    }
}
static uint8_t hy_src_peek(int32_t fp){
    if(fp>=0 && (uint32_t)fp<g_from_size){ uint8_t jb; return jr_get((uint32_t)fp,&jb)?jb:flash_read((uint32_t)fp); }
    return 0;
}
static void hy_half_rec(uint32_t a, uint8_t lo, uint8_t hi){
    uint32_t i=(a>>1)&PSRC_HW_MASK;
    uint16_t up=(uint16_t)(lo | ((uint16_t)hi<<8));
    g_psrc_imm[i]=lo;
    if((up&0xf800)==0x4800) g_psrc_ldr[i>>3]|=(uint8_t)(1u<<(i&7u));
    else g_psrc_ldr[i>>3]&=(uint8_t)~(1u<<(i&7u));
}
/* record one pristine source byte at fp into the packed FWD ldr metadata.
 * Out-of-range fp is a no-op (matches hy_src_peek's range gate). */
static void hy_src_rec(int32_t fp, uint8_t v){
    if(fp>=0 && (uint32_t)fp<g_from_size){
        uint32_t a=(uint32_t)fp, i=(a>>1)&PSRC_HW_MASK;
        if(a&1u) hy_half_rec(a-1u, g_psrc_imm[i], v);
        else { g_psrc_imm[i]=v; g_psrc_ldr[i>>3]&=(uint8_t)~(1u<<(i&7u)); }
    }
}
/* journal-aware RAW source byte at fp (no bake). Returns the PRISTINE from-byte: the journal
 * preserves the original byte where the output frontier later overwrote source flash. Records every
 * pristine read into the packed FWD ldr metadata. */
static uint8_t hy_src(int32_t fp){
    uint8_t v=hy_src_peek(fp);
    hy_src_rec(fp,v);
    return v;
}
/* journal-aware pristine 4-byte field word (little-endian) at fpk, peeked WITHOUT touching the psrc
 * ring (suppressed-bl / no-field must record nothing). On a real BL/EX hit the caller replays this
 * word into the ring via hy_word4_rec — the ascending byte/order match the old read+record path. */
static uint32_t hy_word4_peek(uint32_t fpk){
    return  (uint32_t)hy_src_peek((int32_t)fpk)    | ((uint32_t)hy_src_peek((int32_t)fpk+1)<<8)
          | ((uint32_t)hy_src_peek((int32_t)fpk+2)<<16) | ((uint32_t)hy_src_peek((int32_t)fpk+3)<<24);
}
/* record a known-in-range 4-byte field window [fpk,fpk+4) into the ring (ascending, LE). The caller's
 * entry guard pins fpk>=0 && fpk+4<=from_size, so every byte is in range and the per-byte hy_src_rec
 * gate would always pass — record straight into the 4 ring slots (the window cannot self-alias). */
static void hy_word4_rec(uint32_t fpk, uint32_t w){
    hy_half_rec(fpk,     (uint8_t)w,       (uint8_t)(w>>8));
    hy_half_rec(fpk+2u,  (uint8_t)(w>>16), (uint8_t)(w>>24));
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
 * stream, and — matching the original readers — record nothing into the ring). */
static int field_at(SA*s, int32_t fp0, int32_t ks, uint8_t packed[4], int pure, int32_t dl){
    int64_t fpk64=(int64_t)fp0+ks;
    if(fpk64<0 || fpk64+4>(int64_t)g_from_size) return 0;
    uint32_t fpk=(uint32_t)fpk64;
    if(fpk&1u) return 0;                                  /* BL is 2-aligned; LDR targets are 4-aligned */
    /* ONE pristine read of the 4 field bytes (no ring record yet — suppressed-bl must record nothing) */
    uint32_t w=hy_word4_peek(fpk);
    uint16_t up=(uint16_t)w, lo=(uint16_t)(w>>16);
    /* local-BL: 2-aligned, low halfword F000-pattern, high halfword D000-pattern (pristine source) */
    if((up&0xf800)==0xf000 && (lo&0xd000)==0xd000){
        if(!pure) return 2;                                 /* suppressed-bl is implicit */
        int32_t delta=pull_delta(&DR_BL, &M_dibl, DR_DIC_BL, DR_KCAP_BL); /* INLINE pull from the single stream */
        bl_dereloc(up, lo, (uint32_t)delta, packed);
        hy_word4_rec(fpk,w);                                /* record the 4 BL bytes into the ring */
        return 1;
    }
    /* A1: ex (ldr) DERIVED (same-op back-scan), gated by `pure` (no literal patch in the 4 bytes) —
     * mirrors the encoder's pure(k) + op_ldr_set; positions are no longer shipped. ldr_targets
     * rejects any non-4-aligned fpk itself, so no alignment pre-test is needed here. */
    if(pure && ldr_targets(g_FWD?NULL:s, fp0, dl, fpk)){
        int32_t delta=pull_delta(&DR_EX, &M_diex, DR_DIC_EX, DR_KCAP_EX); /* INLINE pull from the single stream */
        hy_word4_rec(fpk,w);                                /* record the 4 EX bytes into the ring */
        pack_s32_buf((w-(uint32_t)delta)&0xffffffffu, packed);
        return 1;
    }
    return 0;
}
/* Literal cursor over the op's content stream + the per-byte write helpers. `fwd` is loop-invariant,
 * so these inline to the same straight-line code the former LITCUR_/WR_ macros produced (the per-
 * direction branch folds away) — but with named typed parameters and no hidden mutation of enclosing
 * locals. The cursor advances in wire order regardless of write direction. */
/* Literal-patch cursor. The wire codes nl literal patches as a run of uLEB gaps + a literal byte
 * each, read in wire order. FWD next-positions accumulate forward from 0; grow next-positions step
 * back from dl (first = dl - gap, then -= gap). The first patch and every later patch share one
 * advance path; nextpos=-1 marks "no more". */
typedef struct { int32_t nextpos, litb, li, step; } LitCur;
__attribute__((always_inline)) static inline void litcur_step(SA*s, LitCur*lc, int32_t nl){
    if(lc->li<nl){ int32_t g=(int32_t)sa_read_uleb(s);
        lc->nextpos += lc->step*g; lc->litb=sa_next_content(s,0); }
    else lc->nextpos=-1;
}
__attribute__((always_inline)) static inline void litcur_init(SA*s, LitCur*lc, int fwd, int32_t nl, int32_t dl){
    lc->step = fwd ? 1 : -1;
    lc->nextpos = fwd ? 0 : dl; lc->litb=0; lc->li=0;
    litcur_step(s, lc, nl);   /* read the first patch (or set nextpos=-1 when nl==0) */
}
__attribute__((always_inline)) static inline void litcur_next(SA*s, LitCur*lc, int32_t nl){
    litcur_step(s, lc, nl);
}
/* el extra bytes, written in iteration direction (after dl for FWD, before dl for grow) */
__attribute__((always_inline)) static inline void wr_extras(SA*s, int fwd, int32_t tp0, int32_t dl, int32_t el){
    for(int32_t i=0;i<el && !s->err && !g_rcerr;i++){
        int32_t e = fwd ? i : (el-1-i);
        uint8_t eb=sa_next_content(s,(int)((tp0+dl+e)&1));
        out_write((uint32_t)(tp0+dl+e), (uint8_t)(eb + corr_at(s,dl+e)));
    }
}
/* copy-mode byte at K: take the literal patch if the cursor sits on K, else pristine source */
__attribute__((always_inline)) static inline void wr_copy(SA*s, LitCur*lc, int32_t tp0, int32_t fp0, int32_t nl, int32_t K){
    uint8_t db=0;
    if(K==lc->nextpos){ db=(uint8_t)lc->litb; lc->li++; litcur_next(s,lc,nl); }
    out_write((uint32_t)(tp0+K), (uint8_t)(db + hy_src(fp0+K) + corr_at(s,K)));
}
/* one streaming op: DIRECT geometry+P+C, journal P eagerly, then INLINE write-order field
 * detection + streaming write via out_write (asc FWD / desc grow). No override buffer. */
static void sa_apply_op(SA*s){
    uint32_t dl_u=s_ug_gamma(&M_gdl), el_u=s_ug_gamma(&M_gel), adj_u=s_ug_gamma(&M_gadj);
    int32_t adj=bb_unzz(adj_u);
    if(g_rcerr || dl_u>0x7fffffffu || el_u>0x7fffffffu){ s->err=1; return; }
    int32_t dl=(int32_t)dl_u, el=(int32_t)el_u;
    int64_t nw=(int64_t)dl+el;
    if(nw<0 || nw>(int64_t)g_image_span){ s->err=1; return; }
    int32_t tp0,fp0;
    if(g_FWD){
        int64_t ntp=(int64_t)s->tp+nw, nfp=(int64_t)s->fp+dl+adj;
        if(ntp>(int64_t)g_to_size || nfp<INT32_MIN || nfp>INT32_MAX){ s->err=1; return; }
        tp0=s->tp; fp0=s->fp; s->tp=(int32_t)ntp; s->fp=(int32_t)nfp;
    } else {
        int64_t ntp=(int64_t)s->tp-nw, nfp=(int64_t)s->fp-dl-adj;
        if(ntp<0 || nfp<INT32_MIN || nfp>INT32_MAX){ s->err=1; return; }
        s->tp=(int32_t)ntp; s->fp=(int32_t)nfp; tp0=s->tp; fp0=s->fp;
    }
    /* ---- [P] preserves: journal eagerly (offset deltas) ---- */
    uint32_t np=s_ug_gamma(&M_pgn);
    if(np>(uint32_t)nw){ s->err=1; return; }
    uint32_t poff=0, nwu=(uint32_t)nw;
    for(uint32_t i=0;i<np && !s->err && !g_rcerr;i++){
        uint32_t gap=s_ug_gamma(i?&M_pg2:&M_pg);
        if(gap>UINT32_MAX-poff){ s->err=1; return; }
        poff+=gap;
        if(poff>=nwu){ s->err=1; return; }
        sa_journal(s, tp0+(int32_t)poff);
    }
    if(s->err||g_rcerr) return;
    /* ---- [C] corrections (sorted cursor array, count-bounded). Correction bytes share M_dval's
     * adaptive byte tree with DEREL escape bytes; this costs no extra resident model state. ---- */
    uint32_t nc=s_ug_gamma(&M_pgn);
    if(nc>(uint32_t)OPC_CAP){ s->err=1; g_reject=REJ_RESOURCE; return; }
    if(nc>(uint32_t)nw){ s->err=1; return; }
    s->op_nc=(int32_t)nc; { uint32_t coff=0;
        for(uint32_t i=0;i<nc && !g_rcerr;i++){
            uint32_t gap=s_ug_gamma(i?&M_pg2:&M_pg);
            if(gap>UINT32_MAX-coff){ s->err=1; return; }
            coff+=gap;
            if(coff>=nwu){ s->err=1; return; }
            int cbyte=s_bt(&M_dval, 4);
            s->op_corr[i]=(coff<<8)|(uint32_t)cbyte; } }
    if(s->err||g_rcerr) return;
    /* A1: no BL/LDR offsets on the wire. BL suppression is inferred from !pure, and ldr positions
     * are derived per op (ldr_targets). */
#ifdef HY_DBG
    fprintf(stderr,"OP tp0=%d fp0=%d dl=%d el=%d adj=%d blK=%d exK=%d\n",
        tp0,fp0,dl,el,adj,DR_BL.K,DR_EX.K);
#endif
    /* ---- CONTENT decode + streaming write with inline field detection ----
     * Both directions process the same 4-byte ascending field windows over [0,dl); they differ only
     * in (a) iteration direction (step), (b) when the el extra bytes are consumed relative to the dl
     * body (FWD: after; grow: before), and (c) the literal-cursor gap encoding (FWD: absolute forward
     * next-positions; grow: gaps stepping back from dl). The litcur macros hide (c); the el macro is
     * invoked at the direction-correct point to keep the content-stream read order bit-exact. */
    int32_t nl=(int32_t)sa_read_uleb(s);
    if(nl<0||nl>dl){ s->err=1; return; }
    uint8_t packed[4];
    int fwd=g_FWD; int32_t step=fwd?1:-1;
    LitCur lc;
    if(!fwd) wr_extras(s, fwd, tp0, dl, el);
    litcur_init(s, &lc, fwd, nl, dl);
    int32_t k=fwd?0:(dl-1);
    while(k>=0 && k<dl && !s->err && !g_rcerr){
        int32_t a0=fwd?k:(k-3);                          /* low anchor of the 4-byte field window */
        if(a0>=0 && a0+4<=dl){
            int pure=(lc.nextpos<0 || lc.nextpos<a0 || lc.nextpos>=a0+4);   /* no literal patch in [a0,a0+3] */
            int fa=field_at(s, fp0, a0, packed, pure, dl);
            if(fa){
                for(int b=fwd?0:3; b>=0 && b<4; b+=step){
                    if(fa==2) wr_copy(s, &lc, tp0, fp0, nl, a0+b);
                    else out_write((uint32_t)(tp0+a0+b),(uint8_t)(packed[b]+corr_at(s,a0+b)));
                }
                k+=4*step; continue;
            }
        }
        wr_copy(s, &lc, tp0, fp0, nl, k); k+=step;
    }
    if(fwd) wr_extras(s, fwd, tp0, dl, el);
}

/* ===================================================================================== */
/* the decode coroutine entry: runs the whole single-stream decode start-to-finish.        */
/* Shared state passed via globals (set by decoder_run before swapcontext).                 */
/* ===================================================================================== */
static void decode_body(void){
    g_rcerr=0; g_reject=REJ_NONE;
    rc_init();
    orow_reset();
    /* ---- STREAMED DELTAS: NO up-front DEREL phase. The delta models are initialized fresh and used
     * INLINE during apply (pull_delta in field_at). M_dval (escape/correction bytes), the two MTF
     * dict streams, and the two index UGolombs all persist through apply. ---- */
    bt_init(&M_dval); dr_init(&DR_BL, DR_DIC_BL); dr_init(&DR_EX, DR_DIC_EX);
    idx_init(&M_dibl, 2816u); idx_init(&M_diex, 2816u);  /* seed toward STOP (idx 0); 2816 is the
                                                          * corpus optimum holding one-face 582 (was
                                                          * RC_PBIT-RC_PBIT/4=3072). Mirror in rc_v3_enc.c. */
    /* ---- [A] streaming apply (no bake): per op read DIRECT geom+P+C, journal P eagerly,
     * then PULL the op's CONTENT from the cut whole-stream LZSS, detect de-reloc fields inline in
     * write order (pulling each delta from the single stream via pull_delta), write via out_write. ---- */
    jr_reset();
    ugg_init(&M_pg); ugg_init(&M_pgn);
    ugg_init(&M_pg2);
    ugg_init(&M_gdl); ugg_init(&M_gel); ugg_init(&M_gadj);
    ugg_seed_cont(&M_gdl,6); ugg_seed_cont(&M_gadj,3);
    { SA*s=&SAst; memset(s,0,sizeof*s);
      s->tp=g_FWD?0:(int32_t)g_to_size; s->fp=g_FWD?0:(int32_t)g_fp_end;
      ugr_init(&M_gd,11); uint32_t nseq=s_ug_rice(&M_gd);
      int kd=(int)s_raw_bits(4);
      ugr_init(&M_gd,kd); ugg_init(&M_gl); ugg_init(&M_gs);
      ugg_seed_cont(&M_gl,1);   /* matches are always len>=3 (value>=2 => cl>=1): M_gl's first unary
                                 * prefix bit is ALWAYS continue, so seed it cheap from symbol 1. */
      fl_init(&M_flag);
      M_rep0[0]=M_rep0[1]=RC_REP0_INIT; g_rep0h=0; g_lastdist=0;
      uint32_t nops=s_raw_gz();
      if(g_rcerr || nseq > g_to_size + 1u || nops > g_to_size + 1u){ g_dec_status=DEC_ERROR; goto done; }
      s->tok_left=nseq; s->tok_mode=0;
      for(uint32_t oi=0; oi<nops && !s->err && !g_rcerr; oi++) sa_apply_op(s);
      /* tp must land exactly; fp must return to 0 for grow (started at the seed g_fp_end). For FWD
       * we did not receive fp_end (it is redundant — CRC32(to) validates), so fp is unchecked there. */
      if(!s->err && (s->tp!=(g_FWD?(int32_t)g_to_size:0) || (!g_FWD && s->fp!=0))) s->err=1;
      if(!s->err && (s->tok_mode!=0 || s->tok_left!=0)) s->err=1;
      if(s->err||g_rcerr){ g_dec_status=DEC_ERROR; goto done; }
      orow_commit();                       /* flush the last resident output row */
    }
done:
#ifndef RC_V3_STACKMEAS
    /* stack-overflow canary: the deepest 8 B were painted 0xC5; if the coroutine overran them the
     * decode is untrustworthy -> reject (CRC would catch corruption too, but this is direct). */
    for(int i=0;i<8;i++) if((unsigned char)g_dec_stack[i]!=0xC5u){ g_dec_status=DEC_ERROR; break; }
#endif
    if(g_dec_status!=DEC_ERROR) g_dec_status=DEC_DONE;
    CO_SWAP_TO_MAIN();                      /* final yield: never returns here */
}

/* ===================================================================================== */
/* public push API: decoder_push(byte) -> NEED_MORE / DONE / ERROR                         */
/* ===================================================================================== */
static void __attribute__((noinline)) lit_tree_from_hist(BitTree*t,const uint32_t*hist,uint32_t*w){
    for(int s=0;s<256;s++) w[256+s]=hist[s];
    for(int m=255;m>=1;m--) w[m]=w[2*m]+w[2*m+1];
    for(int m=1;m<256;m++){ uint32_t num=w[2*m],den=w[m]; uint32_t pr=den?(2u*RC_PBIT*num+den)/(2u*den):RC_PHALF;
        bt_set(t,m-1,(uint16_t)(pr<1?1:(pr>RC_PBIT-1?RC_PBIT-1:pr))); }
}

/* initialize the decoder coroutine; lit_tree seeds from flash. */
static int decoder_init(void){
    g_fifo_full=0; g_eof=0; g_dec_status=DEC_NEED_MORE;
    /* literal bit-trees seeded from flash parity histograms (no transient image buffer).
     * hist0/hist1/w (4 KiB) borrow ARENA at init — this runs BEFORE the streaming apply reuses
     * ARENA, keeping them off the main thread's peak stack (RAM budget). */
    {   uint32_t *hist0=(uint32_t*)ARENA;            /* [0..256)   */
        uint32_t *hist1=hist0+256;                   /* [256..512) */
        uint32_t *w=hist1+256;                       /* [512..1024) -> 4 KiB total, fits ARENA */
        for(int i=0;i<256;i++){ hist0[i]=1; hist1[i]=1; }
        for(uint32_t i=0;i<g_from_size;i++){ uint8_t v=flash_read(i); if(i&1) hist1[v]++; else hist0[v]++; }
        for(int c=0;c<LIT0_CTX;c++) lit_tree_from_hist(&M_lit0[c],hist0,w);
        lit_tree_from_hist(&M_lit1,hist1,w);
    }
    g_litprev=0;
#ifdef RC_V3_STACKMEAS
    memset(g_dec_stack,0xAA,sizeof g_dec_stack);   /* paint to measure coroutine high-water */
#else
    for(int i=0;i<8;i++) g_dec_stack[i]=(char)0xC5u;  /* deepest-8-B stack-overflow canary */
#endif
    fiber_setup();
    /* first resume: runs until the decoder first blocks for a byte (FIFO empty) */
    CO_SWAP_TO_DEC();
    return g_dec_status;
}

static int decoder_push(uint8_t byte){
    if(g_dec_status!=DEC_NEED_MORE) return g_dec_status;
    /* enqueue; the decoder should drain the single byte before returning */
    if(g_fifo_full){ return DEC_ERROR; }
    g_fifo_byte=byte; g_fifo_full=1;
    /* resume decoder; it will consume from the FIFO and suspend again when empty */
    CO_SWAP_TO_DEC();
    return g_dec_status;
}
static int decoder_finish(void){
    /* signal EOF so next_byte() zero-fills; drain until the coroutine completes */
    g_eof=1;
    while(g_dec_status==DEC_NEED_MORE){ CO_SWAP_TO_DEC(); }
    return g_dec_status;
}

/* ===================================================================================== */
/* DEVICE static measurement: exported entry points so arm-none-eabi-size sees the real     */
/* .bss+.data (all decoder static + stack reservation). The push API is the device's actual  */
/* interface; exporting it forces the linker to retain the whole decoder graph + its statics. */
/* On the device flash_read/flash_write/g_image_span are the real flash primitives (extern).  */
/* ===================================================================================== */
#ifdef RC_V3_ARM
int  rcv3_init(void){ return decoder_init(); }
int  rcv3_push(uint8_t b){ return decoder_push(b); }
int  rcv3_finish(void){ return decoder_finish(); }
void rcv3_set(uint32_t fs,uint32_t ts,uint32_t fpe,int fwd){ g_from_size=fs; g_to_size=ts; g_fp_end=fpe; g_FWD=fwd; }
#endif

/* ===================================================================================== */
/* host test driver: flash modelled as a file mapped to a RAM buffer OUTSIDE the budget.   */
/* The decoder only touches it through flash_read/flash_write (0 image bytes in decoder).  */
/* ===================================================================================== */
#ifdef RC_V3_MAIN
#include <unistd.h>
#ifdef RC_V3_NVM
#include "flash_nvm.h"   /* shared NVM emulator owns g_flash/flash_read/flash_write + wear counters */
#else
static uint8_t *g_flash; static uint32_t g_flash_n;
uint32_t g_image_span;
uint8_t flash_read(uint32_t a){ return a<g_flash_n? g_flash[a]:0; }
void flash_write(uint32_t a, uint8_t v){ if(a<g_flash_n) g_flash[a]=v; }
#endif

int main(int argc,char**argv){
    if(argc<3||argc>4){ fprintf(stderr,"usage: %s <memfile> <blob> [byte_mode]\n",argv[0]); return 2; }
    int byte_mode = (argc==4);   /* arg3 present -> push 1 byte at a time (suspend/resume proof) */
    /* load blob fully (this is the patch bytes; we push them through the FIFO) */
    FILE*bf=fopen(argv[2],"rb"); if(!bf){perror("blob");return 2;}
    fseek(bf,0,SEEK_END); long bsz=ftell(bf); fseek(bf,0,SEEK_SET);
    if(bsz<12){ fprintf(stderr,"blob too short\n"); fclose(bf); return 1; }
    uint8_t*blob=malloc(bsz); if(fread(blob,1,bsz,bf)!=(size_t)bsz){ fclose(bf); return 2; } fclose(bf);
    /* parse plaintext header: CRC32(from)[4] | from_size | zz(to_size-from_size)
     *                          | [zz(fp_end-from_size) iff to_size>from_size]. to_size and fp_end are
     * zigzag-delta-coded against from_size (mirror of rc_v3_enc.c encode_a1). */
    uint32_t want_from_crc = blob[0]|(blob[1]<<8)|(blob[2]<<16)|((uint32_t)blob[3]<<24);
    size_t p=4; int err=0;
    uint32_t from_size=0,to_size=0,fp_end=0; { uint32_t v=0; int sh=0; uint8_t b;
        do{ if(p>=(size_t)bsz||sh>28){err=1;break;} b=blob[p++]; v|=(uint32_t)(b&0x7f)<<sh; sh+=7; }while(b&0x80); from_size=v; }
    { uint32_t z=0; int sh=0; uint8_t b; do{ if(p>=(size_t)bsz||sh>28){err=1;break;} b=blob[p++]; z|=(uint32_t)(b&0x7f)<<sh; sh+=7; }while(b&0x80);
      int64_t ts=(int64_t)from_size + ((z&1u)? -(int64_t)((z>>1)+1u) : (int64_t)(z>>1));
      if(ts<0 || ts>(int64_t)(64u<<20)){ err=1; ts=0; } to_size=(uint32_t)ts; }
    /* fp_end is the grow (!FWD) start seed; FWD/shrink/equal patches omit it (computed inline). */
    if(to_size>from_size){ uint32_t z=0; int sh=0; uint8_t b; do{ if(p>=(size_t)bsz||sh>28){err=1;break;} b=blob[p++]; z|=(uint32_t)(b&0x7f)<<sh; sh+=7; }while(b&0x80);
      int64_t fe=(int64_t)from_size + ((z&1u)? -(int64_t)((z>>1)+1u) : (int64_t)(z>>1));
      if(fe<0 || fe>(int64_t)(64u<<20)){ err=1; fe=0; } fp_end=(uint32_t)fe; }
    if(err){ fprintf(stderr,"bad header\n"); free(blob); return 1; }
    /* host-harness sanity (fuzz-hardening): an implausibly large size field would make nvm_init malloc
     * a huge span. Real images are <1 MiB; reject >64 MiB up front. (Device has no malloc; host-only.) */
    if(from_size>(64u<<20) || to_size>(64u<<20)){ fprintf(stderr,"implausible size — rejected\n"); free(blob); return 1; }
    size_t body_start=p; size_t body_end=bsz-4;   /* trailer CRC32(to) is the last 4 bytes */
    uint32_t want_to_crc = blob[bsz-4]|(blob[bsz-3]<<8)|(blob[bsz-2]<<16)|((uint32_t)blob[bsz-1]<<24);

    /* open the flash file (the image; sized to span = max(from,to)) */
    FILE*mf=fopen(argv[1],"r+b"); if(!mf){perror("mem");free(blob);return 2;}
    fseek(mf,0,SEEK_END); long fsz=ftell(mf); fseek(mf,0,SEEK_SET);
    if((uint32_t)fsz!=from_size){ fprintf(stderr,"from_size mismatch (%ld vs %u)\n",fsz,from_size); fclose(mf); free(blob); return 1; }
    g_image_span = from_size>to_size? from_size:to_size;
#ifdef RC_V3_NVM
    { uint8_t*tmp=(uint8_t*)malloc(from_size?from_size:1);
      if(fread(tmp,1,from_size,mf)!=from_size){ fclose(mf); free(tmp); free(blob); return 2; }
      nvm_init(tmp,from_size,g_image_span); free(tmp); }   /* erased tail = 0xFF (real flash) */
#else
    g_flash_n=g_image_span; g_flash=malloc(g_image_span?g_image_span:1);
    if(fread(g_flash,1,from_size,mf)!=from_size){ fclose(mf); free(g_flash); free(blob); return 2; }
    if(g_image_span>from_size) memset(g_flash+from_size,0,g_image_span-from_size);
#endif

    g_from_size=from_size; g_to_size=to_size; g_fp_end=fp_end; g_FWD=(to_size<=from_size);

    /* pre-write gate: CRC32(from) over flash */
    if(crc32_flash(from_size)!=want_from_crc){ fprintf(stderr,"crc(from) mismatch — refusing\n"); fclose(mf); free(g_flash); free(blob); return 1; }

    /* run the streaming decode by pushing patch body bytes through the FIFO. In both modes we
     * push 1 byte per decoder_push() — the difference is byte_mode AUDITS the suspend/resume:
     * it counts how often decoder_push returned NEED_MORE (i.e. the coroutine suspended waiting
     * for more bytes) and asserts the decoder genuinely streamed (suspended many times), proving
     * it never buffered the whole patch. */
    int rc=decoder_init();
    size_t bi=body_start; long suspends=0;
    while(bi<body_end && rc==DEC_NEED_MORE){ rc=decoder_push(blob[bi++]); if(rc==DEC_NEED_MORE) suspends++; }
    if(rc==DEC_NEED_MORE) rc=decoder_finish();
    else decoder_finish();   /* still free resources */
    if(byte_mode){
        /* a genuinely streaming decoder suspends roughly once per consumed byte */
        if(suspends < (long)((body_end-body_start)/2)){
            fprintf(stderr,"streaming check FAILED: only %ld suspends for %zu bytes\n",suspends,body_end-body_start);
            fclose(mf); free(g_flash); free(blob); return 1;
        }
        fprintf(stderr,"streaming OK: %ld suspends over %zu body bytes\n",suspends,body_end-body_start);
    }
    if(rc==DEC_ERROR){ uint8_t rj=g_reject?g_reject:REJ_CORRUPT;
        fprintf(stderr,"decode error — rejected (reason=%u: %s)\n", rj,
                rj==REJ_RESOURCE?"resource cap exceeded — firmware larger than build sizing":"corrupt/truncated patch");
        fclose(mf); free(g_flash); free(blob); return 1; }
#ifdef RC_V3_STACKMEAS
    { unsigned hw=0; for(unsigned k=0;k<sizeof g_dec_stack;k++) if((unsigned char)g_dec_stack[k]!=0xAA){ hw=(unsigned)sizeof g_dec_stack-k; break; }
      fprintf(stderr,"STACKMEAS coroutine high-water = %u B (reserved %u)\n",hw,(unsigned)sizeof g_dec_stack); }
#endif

#ifdef RC_V3_BAKEDUMP
    { const char*dp=getenv("AGENT07_OUTDUMP"); if(dp){ FILE*f=fopen(dp,"wb"); fwrite(g_flash,1,to_size,f); fclose(f); } }
#endif
    /* post-write gate: CRC32(to) over the reconstructed flash, BEFORE committing */
    if(crc32_flash(to_size)!=want_to_crc){ fprintf(stderr,"crc(to) mismatch — corrupt patch rejected\n"); fclose(mf); free(g_flash); free(blob); return 1; }
#ifdef RC_V3_NVM
    if(nvm_rows_amplified()!=0 || nvm_max_row_erases()>1 || nvm_frontier_inversions()!=0){
        fprintf(stderr,"NVM safety gate FAILED: amplified=%u maxrowerase=%u inversions=%ld\n",
                nvm_rows_amplified(),nvm_max_row_erases(),nvm_frontier_inversions());
        fclose(mf); free(g_flash); free(blob); return 1;
    }
#endif
    /* commit to the memfile */
    fseek(mf,0,SEEK_SET);
    if(fwrite(g_flash,1,to_size,mf)!=to_size){ fclose(mf); free(g_flash); free(blob); return 1; }
    fflush(mf);
    if((long)to_size<fsz){ if(ftruncate(fileno(mf),to_size)){} }
    fprintf(stderr,"ok to_size=%u dir=%s journal_used=%u slots (cap=%u)\n",to_size,g_FWD?"fwd":"bwd",(unsigned)g_jpage[JPAGE_MAX],(unsigned)JSLOTS);
#ifdef RC_V3_NVM
    fprintf(stderr,"NVM: erases=%ld rows=%u programs=%ld amplified=%u maxrowerase=%u inversions=%ld (span=%u rows_total=%u, ideal=span/256)\n",
            nvm_erases(),nvm_rows(),nvm_programs(),nvm_rows_amplified(),nvm_max_row_erases(),nvm_frontier_inversions(),g_image_span,(g_image_span+255)/256);
#endif
    fclose(mf); free(blob);
    return 0;
}
#endif
