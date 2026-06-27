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
 * + the 1024 B ldr-derive pristine ring; the image lives ONLY in flash (0 image bytes in RAM).
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

/* ===================================================================================== */
/* byte FIFO + ucontext coroutine (the push / blocking-fetch core, SPEC §3.1)             */
/* ===================================================================================== */
#ifndef FIFO_CAP
#define FIFO_CAP 1u                         /* decoder_push() supplies one byte per resume */
#endif
#if (FIFO_CAP == 0u) || ((FIFO_CAP & (FIFO_CAP - 1u)) != 0u)
#error "FIFO_CAP must be a nonzero power of two"
#endif
#define FIFO_MASK (FIFO_CAP-1u)
static uint8_t  g_fifo[FIFO_CAP];
static uint32_t g_fhead, g_ftail;          /* ring indices */
static uint8_t  g_eof;                     /* producer signalled end-of-stream */

#ifndef DEC_STACK_BYTES
#define DEC_STACK_BYTES 576                /* coroutine stack; measured high-water 504 B (data-independent
                                            * call depth) leaves a 72 B cushion for out-of-corpus call
                                            * paths. The deepest 8 B are a 0xC5 canary checked at decode
                                            * end -> stack overflow REJECTS, never silent-corrupts. */
#endif
static char     g_dec_stack[DEC_STACK_BYTES] __attribute__((aligned(16)));
static uint8_t  g_dec_done, g_dec_err;     /* set when the decode coroutine returns */

/* ===================================================================================== */
/* Coroutine: a context is just a saved stack pointer (+ callee-saved regs spilled onto    */
/* its own stack at the swap point) — a few words on a real Cortex-M0+ (PendSV-style swap, */
/* ~16 B static for the two SP slots). glibc ucontext_t is 968 B of HOST bloat that does    */
/* not exist on the device, so we use a MINIMAL fiber on x86-64 (the test arch) — the host  */
/* SRAM number then reflects the true device cost. The ARM device build also uses the fiber */
/* (no ucontext.h on bare metal). RC_V3_USE_UCONTEXT forces the glibc path if ever needed.  */
/* ===================================================================================== */
#if defined(__x86_64__) && !defined(RC_V3_USE_UCONTEXT)
typedef void* Fiber;                       /* saved stack pointer */
static Fiber g_main_sp, g_dec_sp;
__attribute__((noinline,naked)) static void pd_swap(Fiber*from __attribute__((unused)),
                                                    Fiber*to __attribute__((unused))){
    __asm__ volatile(
        "pushq %%rbp\n\t pushq %%rbx\n\t pushq %%r12\n\t pushq %%r13\n\t pushq %%r14\n\t pushq %%r15\n\t"
        "movq %%rsp, (%%rdi)\n\t"           /* *from = rsp */
        "movq (%%rsi), %%rsp\n\t"           /* rsp = *to */
        "popq %%r15\n\t popq %%r14\n\t popq %%r13\n\t popq %%r12\n\t popq %%rbx\n\t popq %%rbp\n\t"
        "ret\n\t" : : : "memory");
}
static void decode_body(void);
static void pd_trampoline(void){ decode_body(); for(;;); }  /* never returns (final swap inside) */
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
#elif !defined(RC_V3_ARM)
#include <ucontext.h>
static ucontext_t g_main_ctx, g_dec_ctx;
static void decode_body(void);
static void fiber_setup(void){
    getcontext(&g_dec_ctx);
    g_dec_ctx.uc_stack.ss_sp=g_dec_stack; g_dec_ctx.uc_stack.ss_size=sizeof g_dec_stack;
    g_dec_ctx.uc_link=&g_main_ctx; makecontext(&g_dec_ctx, decode_body, 0);
}
#define CO_SWAP_TO_MAIN()  swapcontext(&g_dec_ctx,&g_main_ctx)
#define CO_SWAP_TO_DEC()   swapcontext(&g_main_ctx,&g_dec_ctx)
#else
/* RC_V3_ARM device-static measurement build: the real device coroutine is a PendSV/SP swap;
 * a context is two saved SP words (~16 B static). We retain the FULL decoder graph + the
 * coroutine-stack reservation so arm-none-eabi-size reports the true device .bss+.data: the
 * trampoline references decode_body, fiber_setup references g_dec_stack. The asm swap is the
 * device's PendSV; here a minimal volatile stub keeps the symbols (size is graph-complete). */
typedef void* Fiber;
static Fiber g_main_sp, g_dec_sp;
static void decode_body(void);
static void pd_trampoline(void){ decode_body(); for(;;); }
static volatile void* g_arm_anchor;
static void fiber_setup(void){
    void**sp=(void**)(g_dec_stack+DEC_STACK_BYTES);   /* reserve & reference the coroutine stack */
    *--sp=(void*)pd_trampoline; g_dec_sp=(Fiber)sp; g_arm_anchor=(void*)&g_dec_stack[0];
}
#define CO_SWAP_TO_MAIN()  do{ g_main_sp=g_dec_sp; }while(0)
#define CO_SWAP_TO_DEC()   do{ g_dec_sp=g_main_sp; }while(0)
#endif

/* next_byte(): pull one byte; if the FIFO is empty, suspend back to the producer (block). */
static uint8_t next_byte(void){
    while (g_fhead == g_ftail){
        if (g_eof) return 0;               /* zero-fill past EOF (optimal-flush rule) */
        CO_SWAP_TO_MAIN();                 /* SUSPEND: yield to producer for more bytes */
    }
    uint8_t b = g_fifo[g_fhead & FIFO_MASK];
    g_fhead++;
    return b;
}

/* divide-free range decoder reading through the blocking FIFO (SPEC §4). */
typedef struct { uint32_t range, code; } SDec;
static SDec RC;
static void rc_init(void){
    RC.range = 0xFFFFFFFFu; RC.code = 0;
    (void)next_byte();                 /* skip the leading range-coder flush byte */
    for (int i=0;i<4;i++) RC.code = (RC.code<<8) | next_byte();
}
static int s_bit_r(uint16_t*prob,int rate){
    uint32_t p=*prob, bound=(RC.range>>12)*p; int b;
    if (RC.code<bound){ RC.range=bound; *prob=(uint16_t)(p+((RC_PBIT-p)>>rate)); b=0; }
    else { RC.code-=bound; RC.range-=bound; *prob=(uint16_t)(p-(p>>rate)); b=1; }
    while (RC.range<RC_KTOP){ RC.code=(RC.code<<8)|next_byte(); RC.range<<=8; }
    return b;
}
/* Default adaptive-bit rate for the Golomb (unary+mantissa), order-2 flag, and MTF
 * rep/hit streams. Tuned to 4 (1/16) — these low-cardinality, fast-drifting contexts
 * track better at 4 than the LZMA-classic 5; literal/dval bit-trees keep their own
 * per-tree rate via s_bit_r. Must match RC_S_BIT_RATE in rc_v3_enc.c (bit-exact wire). */
#define RC_S_BIT_RATE 4
static int s_bit(uint16_t*prob){ return s_bit_r(prob,RC_S_BIT_RATE); }
static int s_raw(void){
    uint32_t bound=RC.range>>1; int b;
    if (RC.code<bound){ RC.range=bound; b=0; } else { RC.code-=bound; RC.range-=bound; b=1; }
    while (RC.range<RC_KTOP){ RC.code=(RC.code<<8)|next_byte(); RC.range<<=8; }
    return b;
}
/* CRASH-HARDENING (SPEC §7, fuzz gate): a corrupt/truncated stream yields zero-fill past EOF,
 * which can drive the unbounded unary loops below forever (hang) or shift a value by >=32 bits
 * (UB). Every unbounded loop is capped to the max a 32-bit value needs; on overflow set g_rcerr
 * and bail (the apply checks err -> clean reject, never crash / never silent-wrong). */
static int g_rcerr;
/* rob-1: distinguishable reject reason (read by the host main / rcv3_reject after a reject). 1 =
 * RESOURCE: a corpus-overfit cap was exceeded (journal full / per-op cap / dict cap) — this firmware
 * is bigger than the build was sized for, raise the cap (costs SRAM). 2 = CORRUPT: malformed/truncated
 * stream (bounds, underrun, range-coder overflow). 0 = none. Pure diagnostic; never affects decoding. */
enum { REJ_NONE=0, REJ_RESOURCE=1, REJ_CORRUPT=2 };
static uint8_t g_reject;
#define RC_UNARY_MAX 31           /* a uint32 value needs <=31 leading/unary bits */
static uint32_t s_raw_bits(int nb){ uint32_t v=0; for(int i=0;i<nb;i++) v=(v<<1)|s_raw(); return v; }
static uint32_t s_raw_gamma(void){
    int n=0; while(s_raw()==0){ if(++n>RC_UNARY_MAX){ g_rcerr=1; return 1; } }
    uint32_t v=1; for(int i=0;i<n;i++) v=(v<<1)|s_raw(); return v; }
static uint32_t s_raw_gz(void){ return s_raw_gamma()-1; }
/* ---- bit-tree byte ---- */
static int s_bt(BitTree*t){ int m=1; for(int i=0;i<8;i++) m=(m<<1)|s_bit_r(&t->p[m],t->rate); return m-256; }
/* ---- ByteVarint (pack_size) ---- */
static int32_t s_bv(BitTree*t){
    int b0=s_bt(t); int sgn=b0&0x40; uint32_t val=b0&0x3f; int off=6;
    while(b0&0x80){
        if(off>28){ g_rcerr=1; return 0; }
        b0=s_bt(t);
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
#if UG_CTX != 6
#error "packed gamma UGolomb layout assumes UG_CTX=6"
#endif
#define UG_GAMMA_M (1 + ((UG_CTX-1)*UG_CTX)/2 + (UG_CTX+1))
typedef struct { uint16_t u[UG_CTX+1]; uint16_t m[UG_GAMMA_M]; } UGGamma;
static void ugr_init(UGRice*g,int k){
    g->k=(uint8_t)k;
    for(int i=0;i<=UG_CTX;i++){ g->u[i]=RC_PHALF; for(int j=0;j<=UG_CTX;j++) g->m[i][j]=RC_PHALF; }
}
static void ugg_init(UGGamma*g){
    for(int i=0;i<=UG_CTX;i++) g->u[i]=RC_PHALF;
    for(int i=0;i<UG_GAMMA_M;i++) g->m[i]=RC_PHALF;
}
/* seed the unary-prefix priors of a gamma model toward "continue" for the first `depth` context
 * positions. Used by per-op geometry (diff_len/adj): firmware delta op magnitudes are essentially
 * never tiny, so cl>=depth almost always — this is a structural prior, NOT a corpus cap, and it
 * makes the very first op (e.g. a one-face update with few ops) as cheap as the warmed-up state. */
static void ugg_seed_cont(UGGamma*g,int depth){
    for(int i=0;i<depth && i<=UG_CTX;i++) g->u[i]=(uint16_t)(RC_PBIT/16);  /* low p == bit1(continue) cheap */
}
static uint32_t s_ug_rice(UGRice*g){
    uint32_t cl=0; while(s_bit(&g->u[UG_C((int)cl)])==1){ if(++cl>RC_RICE_UNARY_MAX){ g_rcerr=1; return 0; } }
    uint32_t v=cl<<g->k;
    for(int pos=0;pos<g->k;pos++) v|=(uint32_t)s_bit(&g->m[UG_C((int)cl)][UG_C(pos)])<<(g->k-1-pos);
    return v;
}
static uint32_t s_ug_gamma(UGGamma*g){
    int cl=0; while(s_bit(&g->u[UG_C(cl)])==1){ if(++cl>RC_UNARY_MAX){ g_rcerr=1; return 0; } }
    uint32_t mm=1u<<cl;
    static const uint8_t base[UG_CTX+1] = { 0, 1, 2, 4, 7, 11, 16 };
    int mb=base[UG_C(cl)];
    for(int pos=0;pos<cl;pos++) mm|=(uint32_t)s_bit(&g->m[mb+UG_C(pos)])<<(cl-1-pos);
    return mm-1;
}
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
/* never-evict journal (SPEC §6) — PACKED 3-BYTE PAGED scheme (Path E refinement of Path D). */
/* Each never-evict entry is (pos, byte). pos needs 18 bits (image span ~216 KB), byte 8 ->  */
/* 26 bits, so a fixed slot is 4 B (Path D's sorted (pos<<8)|byte) = 903*4 = 3612 B. We do    */
/* better: store only the LOW 16 bits of pos + the byte = exactly 3 B/slot, and amortise the  */
/* HIGH pos bits across a tiny PAGE TABLE (slots are kept sorted by full pos, so each 64 KB    */
/* page is one contiguous run). 903*3 = 2709 B + ~32 B page table vs 3612/4096 — saves ~1.39  */
/* KB at ZERO size cost (decoder-only). NEVER-EVICT: first write to a pos wins; over-depth     */
/* (would exceed JSLOTS) is REFUSED before any write (Path A's pre-write refusal, preserved).  */
/* Lives in the apply phase ONLY (overlaid in ARENA front).                                    */
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
    int32_t *dic; uint16_t cap, K;    /* MTF dict (index 0 = most-recently-used) + its capacity */
    int32_t  last;                    /* repeat-last fast path */
    uint16_t rep[4], hit; uint8_t rh; /* adaptive binary models (P(bit==0)); rep keyed by prev repeat + last==0 */
} DRStream;
#define DR_HIT_INIT 512u              /* zero-seeded MTF dict makes hit-bit==1 likely */

#ifndef JSLOTS
#define JSLOTS 904u                          /* packed journal capacity; 903 is the measured corpus
                                              * peak, and the 8-byte-aligned region gives slot 904
                                              * for free. Refuse above. */
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
/* SA apply state = ring(2^W) + fixed correction arrays. A1 derives ldr positions per op and infers
 * suppressed BL from `!pure`, so no ex/sbl offset buffers are resident. */
#define SA_ARENA ((1u<<SA_W) + 4u*OPC_CAP + 80u)  /* slack covers SA non-buffer fields + align */
#define ARENA_BYTES (JREGION + SA_ARENA)
static unsigned char ARENA[ARENA_BYTES] __attribute__((aligned(8)));
static uint8_t *const Jbuf = (uint8_t*)ARENA;        /* packed journal (apply phase): JSLOTS*3 bytes */
static uint16_t Jcount, Jpeak;
/* page[p] = index of the first slot whose (pos>>16) >= p, for p in [0..JPAGE_MAX].  Since slots
 * are sorted by full pos, page p occupies the contiguous run [page[p], page[p+1]). */
static uint16_t g_jpage[JPAGE_MAX+1];
static uint8_t g_npage;                         /* number of active pages = (span>>16)+1, capped */
static uint32_t jslot_lo(int i){ const uint8_t*s=Jbuf+(uint32_t)i*3u; return (uint32_t)s[1]|((uint32_t)s[2]<<8); }
static void jr_reset(void){ Jcount=Jpeak=0;
    uint32_t pg=(g_image_span>>16)+1u; if(pg>(uint32_t)JPAGE_MAX) pg=JPAGE_MAX; g_npage=(int)pg;
    for(int p=0;p<=JPAGE_MAX;p++) g_jpage[p]=0; }
/* binary search within page (pos>>16) by the 16-bit low key; on hit *ins=index & return 1,
 * else *ins=lower-bound insertion index (page-clamped) & return 0. */
static int jr_find(uint32_t pos, int*ins){
    uint32_t pg=pos>>16; if(pg>=(uint32_t)g_npage){ if(ins)*ins=Jcount; return 0; }
    uint32_t key=pos&0xFFFFu;
    int lo=g_jpage[pg], hi=g_jpage[pg+1]-1;
    while(lo<=hi){ int mid=(int)(((unsigned)lo+(unsigned)hi)>>1); uint32_t k=jslot_lo(mid);
        if(k==key){ if(ins)*ins=mid; return 1; } if(k<key) lo=mid+1; else hi=mid-1; }
    if(ins)*ins=lo;
    return 0;
}
static int jr_put(uint32_t pos, uint8_t b){
    if(pos>=0xFFFFFFu) return -1;
    uint32_t pg=pos>>16; if(pg>=(uint32_t)g_npage) return -1;     /* outside paged span */
    int ins;
    if(jr_find(pos,&ins)) return 0;                  /* never-evict: keep the first write */
    if((uint32_t)Jcount>=JSLOTS) return -1;          /* over-depth: refuse BEFORE any write */
    if(ins<Jcount) memmove(Jbuf+(uint32_t)(ins+1)*3u, Jbuf+(uint32_t)ins*3u, (size_t)(Jcount-ins)*3u);
    uint8_t*s=Jbuf+(uint32_t)ins*3u; s[0]=b; s[1]=(uint8_t)pos; s[2]=(uint8_t)(pos>>8);
    for(uint32_t p=pg+1u;p<=(uint32_t)g_npage;p++) g_jpage[p]++;   /* shift higher page boundaries */
    if(++Jcount>Jpeak) Jpeak=Jcount;
    return 0;
}
static int jr_get(uint32_t pos, uint8_t*out){
    int ins;
    if(jr_find(pos,&ins)){ *out=Jbuf[(uint32_t)ins*3u]; return 1; }
    return 0;
}

/* ===================================================================================== */
/* relocation unpack/pack (Thumb bl + s32) — de-relocation override only (no flash write).      */
/* The journal-aware pristine reads + the bl/ldr predicates live with the apply state below.    */
/* ===================================================================================== */
static int32_t unpack_bl(uint16_t up, uint16_t lo){
    int32_t s=(up>>10)&1, imm10=up&0x3ff, imm11=lo&0x7ff;
    int32_t j1=(lo>>13)&1, j2=(lo>>11)&1, i1=1-(j1^s), i2=1-(j2^s);
    int32_t v=(s<<23)|(i1<<22)|(i2<<21)|(imm10<<11)|imm11; if(s) v-=(1<<24); return v;
}
/* pack a 24-bit BL immediate into 4 bytes (de-relocation override; no flash write) */
static void pack_bl_buf(uint32_t imm24, uint8_t out[4]){
    imm24 &= 0x00ffffffu;
    uint32_t s=(imm24>>23)&1u,i1=(imm24>>22)&1u,i2=(imm24>>21)&1u;
    uint32_t j1=1u-(i1^s),j2=1u-(i2^s),imm10=(imm24>>11)&0x3ffu,imm11=imm24&0x7ffu;
    uint16_t up=(uint16_t)(0xF000u|(s<<10)|imm10), lo=(uint16_t)(0xD000u|(j1<<13)|(j2<<11)|imm11);
    out[0]=up&0xff; out[1]=(up>>8)&0xff; out[2]=lo&0xff; out[3]=(lo>>8)&0xff;
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
static uint32_t g_orow;        /* current resident output row index */
static uint8_t  g_orow_valid, g_orow_dirty;
static void orow_commit(void){
    if(g_orow_valid && g_orow_dirty){
        uint32_t base=g_orow*OUTROW, end=base+OUTROW; if(end>g_image_span) end=g_image_span;
        /* force the (at most one) row erase up front, then program from the RAM buffer so every
         * byte is a pure 1->0 program (no further erase). All bytes of the row were produced by the
         * monotonic output, so the buffer holds the exact final content. */
        for(uint32_t a=base;a<end;a++){ if(flash_read(a)!=0xFFu){ flash_write(a,0xFFu); break; } }
        for(uint32_t a=base;a<end;a++) flash_write(a, g_orow_buf[a-base]);
    }
    g_orow_dirty=0; g_orow_valid=0;
}
static void orow_reset(void){ g_orow_valid=0; g_orow_dirty=0; g_orow=0; }
/* OUTPUT write: buffer in the resident row, committing the previous row on a row change. */
static void out_write(uint32_t a, uint8_t v){
    if(a>=g_image_span) return;
    uint32_t row=a/OUTROW;
    if(!g_orow_valid || row!=g_orow){
        orow_commit();
        uint32_t base=row*OUTROW, end=base+OUTROW; if(end>g_image_span) end=g_image_span;
        for(uint32_t x=base;x<end;x++) g_orow_buf[x-base]=flash_read(x); /* preload (source not yet erased) */
        g_orow=row; g_orow_valid=1; g_orow_dirty=0;
    }
    uint32_t off=a-g_orow*OUTROW;
    if(g_orow_buf[off]!=v){ g_orow_buf[off]=v; g_orow_dirty=1; }
}
/* OUTPUT read (reads of already-produced output bytes, e.g. multi-write/correction overlay): serve
 * from the dirty buffer if resident, else physical flash. */
static uint8_t out_read(uint32_t a){
    if(g_orow_valid && a/OUTROW==g_orow && a<g_image_span) return g_orow_buf[a-g_orow*OUTROW];
    return flash_read(a);
}

/* ===================================================================================== */
/* entropy models (all live through the single streamed apply): tc/gd/gl/gs (content) +          */
/* pg/cg (preserve/correction) + dibl/diex (streamed-delta MTF dict-index Golombs). M_dval       */
/* (escape values) + DR_BL/DR_EX (MTF dicts) are separate statics. */
static UGRice  M_gd;             /* token_count first, then reinitialized as backref_dist */
static UGGamma M_gl, M_gs;       /* backref_len_v3 (len-1), span_len */
static UGGamma M_pg, M_cg;       /* preserve/correction gaps */
static UGGamma M_pgn, M_cgn;     /* preserve/correction COUNTS (np/nc) — separated from gaps so the
                                  * dominant count=0 case and the dominant gap=1 case stop fighting
                                  * over the shared adaptive unary/mantissa probabilities. */
static UGGamma M_pg2, M_cg2;     /* preserve/correction REST gaps (2nd..Nth). The first gap is an
                                  * op-relative offset (broad), but later gaps are ~98%/77% value=1
                                  * (consecutive run); a dedicated model converges hard on that. */
static UGGamma M_gdl, M_gel, M_gadj; /* per-op geometry: diff_len, extra_len, zz(adj). Were raw dz
                                  * (no model); their bit-length distributions are concentrated, so an
                                  * adaptive gamma UGolomb beats the fixed raw code. */
static UGGamma M_dibl, M_diex;   /* BL/EX MTF dict indices */
/* tag0 (diff-literal/even-extra) literal trees split by previous-literal top-2 bits (4 contexts);
 * tag1 (odd-parity extra) keeps a single tree. g_litprev = last literal byte emitted (any tag),
 * reset to 0 per blob; mirrors rc_v3_enc encode_body prevlit. */
#define LIT0_CTX 4
static BitTree M_lit0[LIT0_CTX], M_lit1;
static uint8_t g_litprev;
static BitTree M_dval;             /* shared byte tree for DEREL escape bytes and [C] correction bytes */
static Flag1   M_flag;
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

static void dr_init(DRStream*d, int32_t*dic, int cap){
    d->dic=dic; d->cap=cap; d->K=1; d->dic[0]=0;
    d->last=0;
    for(int i=0;i<4;i++) d->rep[i]=RC_PHALF;
    d->rh=0; d->hit=DR_HIT_INIT;
}
/* pull the next delta of a stream (bl/ex), inline:
 *   rep-bit: ==1 -> return last; else read hit-bit:
 *     hit==1 -> MTF index via the index UGolomb (gix); v=dic[j]; move dic[j] to front.
 *     hit==0 -> escape value via M_dval; insert v at MTF front.
 *   then last=v. */
static int32_t pull_delta(DRStream*d, UGGamma*gix){
    { int ri=d->rh | (d->last==0 ? 2 : 0);
      int rb=s_bit(&d->rep[ri]); d->rh=rb; if(rb==1) return d->last; }   /* repeat-last, order-1 + zero ctx */
    int32_t v;
    if(s_bit(&d->hit)==1){
        uint32_t j=s_ug_gamma(gix)+1u;                  /* cmp-1: dict idx 0 unreachable -> encode j-1 */
        if(j>=(uint32_t)d->K){ g_rcerr=1; return 0; }
        v=d->dic[j];
        if(j){ int32_t t=d->dic[j]; memmove(&d->dic[1], &d->dic[0], (size_t)j * sizeof(d->dic[0])); d->dic[0]=t; }
    } else {
        v=s_bv(&M_dval);
        if(d->K>=d->cap){ g_rcerr=1; g_reject=REJ_RESOURCE; return 0; }   /* distinct-value cap -> reject */
        memmove(&d->dic[1], &d->dic[0], (size_t)d->K * sizeof(d->dic[0]));
        d->dic[0]=v; d->K++;
    }
    d->last=v; return v;
}


/* ===================================================================================== */
/* Path F streaming [A] apply: NO split, NO per-op literal/extra buffer (SA_OPCAP gone).      */
/* Per op the decoder reads DIRECT geometry+P+C (outside LZSS), journals preserves EAGERLY,    */
/* then PULLS the op's CONTENT bytes from the cut whole-stream LZSS token stream and writes     */
/* each output byte immediately (ascending FWD / descending grow). Literal patches are consumed */
/* via an O(1) next-position cursor; corrections via an O(1) sorted-array cursor (count-bounded).*/
/* The LZSS ring (size 2^SA_W) is the content history; backref distances <= 2^SA_W.             */
/* ===================================================================================== */
#define SA_RING (1u<<SA_W)
#define SA_MASK (SA_RING-1u)
/* content-stream parity scanner (geometry-fed; mirrors rc_v3._ContentScanner). Tag is 0 except
 * for an EXTRA (new-code) byte, tagged by its true to-address parity (direction-independent).
 * Phase order: FWD = nl,[gap,litb]*,extra ; grow = nl,extra,[gap,litb]*. */
enum { CPH_NL, CPH_GAP, CPH_LITB, CPH_EXTRA, CPH_DONE };
typedef struct {
    int32_t acc,nl,li,el,ei,dl,tp0;
    uint8_t ph,first,shift,FWD;
} CScan;
static void cs_begin(CScan*s, int FWD, int32_t tp0, int32_t dl, int32_t el){
    s->FWD=FWD; s->tp0=tp0; s->dl=dl; s->el=el; s->ph=CPH_NL; s->first=1; s->acc=0; s->shift=0;
    s->nl=0; s->li=0; s->ei=0;
}
static int cs_tag(CScan*s){
    if(s->ph==CPH_EXTRA){ int32_t exstart=s->tp0+s->dl;
        int32_t off=s->FWD? s->ei : (s->el-1-s->ei); return (int)((exstart+off)&1); }
    return 0;
}
static void cs_after_nl(CScan*s){
    if(s->FWD){ s->ph = s->nl>0? CPH_GAP : (s->el>0? CPH_EXTRA : CPH_DONE); }
    else      { s->ph = s->el>0? CPH_EXTRA : (s->nl>0? CPH_GAP : CPH_DONE); }
    if(s->ph==CPH_EXTRA) s->ei=0;
}
static void cs_adv(CScan*s, uint8_t b){
    int ph=s->ph;
    if(ph==CPH_NL || ph==CPH_GAP){
        if(s->first){ s->acc=b&0x7f; s->shift=7; s->first=0; }
        else { if(s->shift>21){ g_rcerr=1; return; } s->acc|=(int32_t)(b&0x7f)<<s->shift; s->shift+=7; }
        if(b&0x80) return;
        s->first=1;
        if(ph==CPH_NL){ s->nl=s->acc; s->li=0; cs_after_nl(s); }
        else s->ph=CPH_LITB;
    } else if(ph==CPH_LITB){
        s->li++;
        if(s->li<s->nl){ s->ph=CPH_GAP; s->first=1; s->acc=0; }
        else if(s->FWD){ s->ph = s->el>0? CPH_EXTRA : CPH_DONE; if(s->ph==CPH_EXTRA) s->ei=0; }
        else s->ph=CPH_DONE;
    } else if(ph==CPH_EXTRA){
        s->ei++;
        if(s->ei>=s->el){ if(!s->FWD && s->nl>0) s->ph=CPH_GAP; else s->ph=CPH_DONE; }
    }
}

typedef struct {
    uint8_t ring[SA_RING]; uint32_t ototal;       /* content history (masked) + total produced */
    /* pauseable LZSS token replay state (pull-driven content producer) */
    uint32_t tok_left;                            /* tokens remaining (token_count) */
    uint32_t span_left, br_left; uint32_t br_src; /* br_src is an absolute ototal index */
    CScan sc;
    uint32_t from_size; int32_t to_size;
    int32_t tp, fp;                               /* running accumulators (apply order) */
    /* per-op corrections (sorted by offset; cursor by binary search). count-bounded, NOT op-size.
     * Packed as high 24 bits = op-local offset, low 8 bits = additive correction byte. */
    uint32_t op_corr[OPC_CAP]; int32_t op_nc;
    uint8_t tok_mode;                             /* 0=idle, 1=span, 2=backref */
    uint8_t FWD, err;
} SA;
/* SA apply state overlaid in ARENA right after the journal (both live only in the apply phase). */
#define SAst (*(SA*)(ARENA + JREGION))
_Static_assert(sizeof(SA) <= SA_ARENA, "SA apply state exceeds its ARENA reservation");
_Static_assert(4096u <= ARENA_BYTES, "seed scratch (hist0+hist1+w) exceeds ARENA");

static int32_t bb_unzz(uint32_t u){
    if(u==0xffffffffu){ g_rcerr=1; return 0; }
    return (u&1u)? -(int32_t)((u+1u)>>1) : (int32_t)(u>>1);
}
/* correction lookup by op-local write-offset (sorted small array; binary search) */
static int32_t corr_at(SA*s, int32_t off){
    int32_t lo=0,hi=s->op_nc-1;
    while(lo<=hi){ int32_t m=(int32_t)(((unsigned)lo+(unsigned)hi)>>1); int32_t o=(int32_t)(s->op_corr[m]>>8);
        if(o==off) return (int32_t)(s->op_corr[m]&0xffu);
        if(o<off) lo=m+1; else hi=m-1; }
    return 0;
}
/* A1 ldr-derive: a 1024-byte ring of recently-read PRISTINE source bytes (psrc[a&1023] = pristine
 * byte at addr a). FWD reads source ascending; field probes PEEK at the current 4-byte field before
 * the ldr back-scan, then record it only after the scan no longer needs the alias at fpk-1024. Grow
 * reads via the journal-aware source path because lower instruction-window bytes may be clobbered. */
#define PSRC_MASK 1023u
static uint8_t g_psrc[1024];
/* A1 ldr-derive (SAME-OP): is the 4-aligned from-addr fpk an ldr literal target of an instruction
 * IN THIS op [fp0,fp0+dl)? Scan even a in [max(fp0,fpk-1024),
 * fpk-2]; an ldr literal `(up&0xf800)==0x4800` targets t=(a&~3)+4*(up&0xff)+4; field iff some a
 * targets fpk and fpk+4<=fp0+dl. Reads PRISTINE source: FWD from the psrc ring, grow via hy_src().
 * No resident target store. */
static int is_ldr_fwd(int32_t fp0, int32_t dl, uint32_t fpk){
    int32_t hi=fp0+dl; if((int32_t)fpk+4>hi) return 0;
    int32_t lo=(int32_t)fpk-1024; if(lo<fp0) lo=fp0; if(lo&1) lo++;
    for(int32_t a=lo; a+2<=(int32_t)fpk; a+=2){
        uint16_t up=(uint16_t)(g_psrc[(uint32_t)a&PSRC_MASK] | (g_psrc[((uint32_t)a+1u)&PSRC_MASK]<<8));
        if((up&0xf800)==0x4800){ int32_t t=(a&~3)+4*(int32_t)(up&0xff)+4; if(t==(int32_t)fpk) return 1; }
    }
    return 0;
}
static uint8_t hy_src(SA*s, int32_t fp);   /* fwd decl: journal-aware pristine source read */
static int is_ldr_grow(SA*s, int32_t fp0, int32_t dl, uint32_t fpk){
    int32_t hi=fp0+dl; if((int32_t)fpk+4>hi) return 0;
    int32_t lo=(int32_t)fpk-1024; if(lo<fp0) lo=fp0; if(lo&1) lo++;
    for(int32_t a=lo; a+2<=(int32_t)fpk; a+=2){
        /* journal-aware: in grow, instr-window bytes the output frontier already clobbered are copy
         * source the codec preserved, so hy_src returns the pristine byte (raw flash would be wrong). */
        uint16_t up=(uint16_t)(hy_src(s,a) | (hy_src(s,a+1)<<8));
        if((up&0xf800)==0x4800){ int32_t t=(a&~3)+4*(int32_t)(up&0xff)+4; if(t==(int32_t)fpk) return 1; }
    }
    return 0;
}
static void sa_emit_ring(SA*s, uint8_t b){ s->ring[s->ototal & SA_MASK]=b; s->ototal++; }
/* pull the next CONTENT byte from the cut LZSS token stream, decoding tokens lazily. */
static uint8_t sa_next_content(SA*s){
    for(;;){
        if(g_rcerr) goto fail;
        if(s->tok_mode==1 && s->span_left>0){           /* span: decode one literal byte */
            int tag=cs_tag(&s->sc);
            int b=s_bt(tag?&M_lit1:&M_lit0[g_litprev>>6]);
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
            uint32_t d=s_ug_rice(&M_gd)+1u, ln=s_ug_gamma(&M_gl)+1u;
            if(d>s->ototal || d-1u>=SA_RING) goto fail;   /* reject before-start / ring-overrun */
            s->tok_mode=2; s->br_src=s->ototal-d; s->br_left=ln;
        }
        s->tok_left--;
    }
fail:
    s->err=1;
    return 0;
}
/* read a uLEB from the content stream (advancing the content-scanner per byte). */
static uint32_t sa_read_uleb(SA*s){
    uint32_t acc=0; int sh=0;
    for(;;){ if(s->err||g_rcerr) return 0;
        uint8_t b=sa_next_content(s); cs_adv(&s->sc,b);
        if(sh>28){ s->err=1; return 0; }            /* cap shift (uLEB <=32-bit) */
        acc|=(uint32_t)(b&0x7f)<<sh; sh+=7;
        if(!(b&0x80)) return acc; }
}
/* journal one preserve site (old flash byte captured BEFORE any write of this op). */
static void sa_journal(SA*s, int32_t tp){
    if(tp>=0 && tp<(int32_t)s->from_size){ if(jr_put((uint32_t)tp, out_read((uint32_t)tp))){ s->err=1; g_reject=REJ_RESOURCE; }
    }
}
static uint8_t hy_src_peek(SA*s, int32_t fp){
    if(fp>=0 && (uint32_t)fp<s->from_size){ uint8_t jb; return jr_get((uint32_t)fp,&jb)?jb:out_read((uint32_t)fp); }
    return 0;
}
/* journal-aware RAW source byte at fp (no bake). Returns the PRISTINE from-byte: the journal
 * preserves the original byte where the output frontier later overwrote source flash. Records every
 * pristine read into the psrc ring (used by the FWD ldr back-scan). */
static uint8_t hy_src(SA*s, int32_t fp){
    uint8_t v=hy_src_peek(s,fp);
    if(fp>=0 && (uint32_t)fp<s->from_size) g_psrc[(uint32_t)fp & PSRC_MASK]=v;
    return v;
}
static void hy_note_src4(SA*s, uint32_t a){
    for(uint32_t i=0;i<4;i++) g_psrc[(a+i)&PSRC_MASK]=hy_src_peek(s,(int32_t)(a+i));
}
/* journal-aware pristine source halfword/word: the
 * from-image flash may already be overwritten by the monotonic output frontier, so reads MUST be
 * journal-aware (preserves hold the original bytes) — a raw flash read would return clobbered output. */
static uint32_t hy_src32(SA*s, uint32_t a){ return (uint32_t)hy_src(s,(int32_t)a)|((uint32_t)hy_src(s,(int32_t)a+1)<<8)|((uint32_t)hy_src(s,(int32_t)a+2)<<16)|((uint32_t)hy_src(s,(int32_t)a+3)<<24); }
static uint16_t hy_src16_peek(SA*s, uint32_t a){ return (uint16_t)(hy_src_peek(s,(int32_t)a)|(hy_src_peek(s,(int32_t)a+1)<<8)); }
/* journal-aware local-bl predicate (pristine source). */
static int hy_is_local_bl(SA*s, uint32_t fpk){
    if(fpk&1u) return 0;
    if(fpk>s->from_size || s->from_size-fpk<4u) return 0;
    uint16_t up=hy_src16_peek(s,fpk), lo=hy_src16_peek(s,fpk+2);
    return ((up&0xf800)==0xf000) && ((lo&0xd000)==0xd000);
}
/* de-reloc field at op-local field-start ks. Returns: 0=no field; 1=bl/ex (packed4 filled,
 * a value consumed from the generator); 2=suppressed-bl (write the 4 bytes as NORMAL copies). */
static int field_at(SA*s, int32_t fp0, int32_t ks, uint8_t packed[4], int pure, int32_t dl){
    int64_t fpk64=(int64_t)fp0+ks;
    if(fpk64<0 || fpk64+4>(int64_t)s->from_size) return 0;
    uint32_t fpk=(uint32_t)fpk64;
    if(hy_is_local_bl(s, fpk)){
        if(!pure) return 2;                                 /* suppressed-bl is implicit */
        int32_t delta=pull_delta(&DR_BL, &M_dibl);         /* INLINE pull from the single stream */
        uint16_t up=hy_src16_peek(s,fpk), lo=hy_src16_peek(s,fpk+2);
        pack_bl_buf(((uint32_t)unpack_bl(up,lo)-(uint32_t)delta)&0x00ffffffu, packed);
        hy_note_src4(s,fpk);
        return 1;
    }
    /* A1: ex (ldr) DERIVED (same-op back-scan), gated by `pure` (no literal patch in the 4 bytes) —
     * mirrors the encoder's pure(k) + _op_ldr_set; positions are no longer shipped. */
    if(pure && (fpk&3u)==0 && fp0+ks+4<=(int32_t)s->from_size &&
       (s->FWD ? is_ldr_fwd(fp0,dl,fpk) : is_ldr_grow(s,fp0,dl,fpk))){
        int32_t delta=pull_delta(&DR_EX, &M_diex);         /* INLINE pull from the single stream */
        uint32_t val=hy_src32(s,fpk);
        pack_s32_buf((val-(uint32_t)delta)&0xffffffffu, packed);
        return 1;
    }
    return 0;
}
/* one Path F op: DIRECT geometry+P+C, journal P eagerly, then INLINE write-order field
 * detection + streaming write via out_write (asc FWD / desc grow). No override buffer. */
static void sa_apply_op(SA*s){
    uint32_t dl_u=s_ug_gamma(&M_gdl), el_u=s_ug_gamma(&M_gel), adj_u=s_ug_gamma(&M_gadj);
    int32_t adj=bb_unzz(adj_u);
    if(g_rcerr || dl_u>0x7fffffffu || el_u>0x7fffffffu){ s->err=1; return; }
    int32_t dl=(int32_t)dl_u, el=(int32_t)el_u;
    int64_t nw=(int64_t)dl+el;
    if(nw<0 || nw>(int64_t)g_image_span){ s->err=1; return; }
    int32_t tp0,fp0;
    if(s->FWD){
        int64_t ntp=(int64_t)s->tp+nw, nfp=(int64_t)s->fp+dl+adj;
        if(ntp>(int64_t)s->to_size || nfp<INT32_MIN || nfp>INT32_MAX){ s->err=1; return; }
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
    uint32_t nc=s_ug_gamma(&M_cgn);
    if(nc>(uint32_t)OPC_CAP){ s->err=1; g_reject=REJ_RESOURCE; return; }
    if(nc>(uint32_t)nw){ s->err=1; return; }
    s->op_nc=(int32_t)nc; { uint32_t coff=0;
        for(uint32_t i=0;i<nc && !g_rcerr;i++){
            uint32_t gap=s_ug_gamma(i?&M_cg2:&M_cg);
            if(gap>UINT32_MAX-coff){ s->err=1; return; }
            coff+=gap;
            if(coff>=nwu){ s->err=1; return; }
            int cbyte=s_bt(&M_dval);
            s->op_corr[i]=(coff<<8)|(uint32_t)cbyte; } }
    if(s->err||g_rcerr) return;
    /* A1: no BL/LDR offsets on the wire. BL suppression is inferred from !pure, and ldr positions
     * are derived per op (is_ldr_*). */
    if(s->err||g_rcerr) return;
#ifdef HY_DBG
    fprintf(stderr,"OP tp0=%d fp0=%d dl=%d el=%d adj=%d blK=%d exK=%d\n",
        tp0,fp0,dl,el,adj,DR_BL.K,DR_EX.K);
#endif
    /* ---- CONTENT decode + streaming write with inline field detection ---- */
    cs_begin(&s->sc, s->FWD, tp0, dl, el);
    int32_t nl=(int32_t)sa_read_uleb(s);
    if(nl<0||nl>dl){ s->err=1; return; }
    uint8_t packed[4];
    if(s->FWD){
        int32_t nextpos=-1, litb=0, li=0;
        if(nl>0){ nextpos=(int32_t)sa_read_uleb(s); litb=sa_next_content(s); cs_adv(&s->sc,(uint8_t)litb); }
        #define TAKE_DB_F(K) ({ uint8_t _take_db=0; if((K)==nextpos){ _take_db=(uint8_t)litb; li++; \
            if(li<nl){ nextpos+=(int32_t)sa_read_uleb(s); litb=sa_next_content(s); cs_adv(&s->sc,(uint8_t)litb);} else nextpos=-1; } _take_db; })
        #define WR_COPY_F(K) do{ uint8_t _copy_db=TAKE_DB_F(K); \
            out_write((uint32_t)(tp0+(K)), (uint8_t)(_copy_db + hy_src(s,fp0+(K)) + corr_at(s,(K)))); }while(0)
        int32_t k=0;
        while(k<dl && !s->err && !g_rcerr){
            if(k+4<=dl){
                int pure=(nextpos<0 || nextpos>=k+4);   /* no literal patch in [k,k+3] */
                int fa=field_at(s, fp0, k, packed, pure, dl);
                if(fa==2){ for(int b=0;b<4;b++) WR_COPY_F(k+b); k+=4; continue; }
                if(fa==1){ for(int b=0;b<4;b++) out_write((uint32_t)(tp0+k+b),(uint8_t)(packed[b]+corr_at(s,k+b))); k+=4; continue; }
            }
            WR_COPY_F(k); k++;
        }
        for(int32_t e=0;e<el && !s->err && !g_rcerr;e++){
            uint8_t eb=sa_next_content(s); cs_adv(&s->sc,eb);
            out_write((uint32_t)(tp0+dl+e), (uint8_t)(eb + corr_at(s,dl+e)));
        }
        #undef TAKE_DB_F
        #undef WR_COPY_F
    } else {
        for(int32_t e=el-1;e>=0 && !s->err && !g_rcerr;e--){
            uint8_t eb=sa_next_content(s); cs_adv(&s->sc,eb);
            out_write((uint32_t)(tp0+dl+e), (uint8_t)(eb + corr_at(s,dl+e)));
        }
        int32_t nextpos=-1, litb=0, li=0;
        if(nl>0){ int32_t gap=(int32_t)sa_read_uleb(s); nextpos=dl-gap; litb=sa_next_content(s); cs_adv(&s->sc,(uint8_t)litb); }
        #define TAKE_DB_D(K) ({ uint8_t _take_db=0; if((K)==nextpos){ _take_db=(uint8_t)litb; li++; \
            if(li<nl){ int32_t _g=(int32_t)sa_read_uleb(s); nextpos-=_g; litb=sa_next_content(s); cs_adv(&s->sc,(uint8_t)litb);} else nextpos=-1; } _take_db; })
        #define WR_COPY_D(K) do{ uint8_t _copy_db=TAKE_DB_D(K); \
            out_write((uint32_t)(tp0+(K)), (uint8_t)(_copy_db + hy_src(s,fp0+(K)) + corr_at(s,(K)))); }while(0)
        int32_t k=dl-1;
        while(k>=0 && !s->err && !g_rcerr){
            int32_t ks=k-3;
            if(ks>=0){
                int pure=(nextpos<ks);                  /* no literal patch in [ks,ks+3] (nextpos<0 -> pure) */
                int fa=field_at(s, fp0, ks, packed, pure, dl);
                if(fa==2){ for(int b=3;b>=0;b--) WR_COPY_D(ks+b); k-=4; continue; }
                if(fa==1){ for(int b=3;b>=0;b--) out_write((uint32_t)(tp0+ks+b),(uint8_t)(packed[b]+corr_at(s,ks+b))); k-=4; continue; }
            }
            WR_COPY_D(k); k--;
        }
        #undef TAKE_DB_D
        #undef WR_COPY_D
    }
}

/* ===================================================================================== */
/* the decode coroutine entry: runs the whole single-stream decode start-to-finish.        */
/* Shared state passed via globals (set by decoder_run before swapcontext).                 */
/* ===================================================================================== */
static uint32_t g_from_size, g_to_size, g_fp_end;
static int      g_FWD;

static void decode_body(void){
    g_rcerr=0; g_reject=REJ_NONE;
    rc_init();
    orow_reset();
    /* ---- STREAMED DELTAS: NO up-front DEREL phase. The delta models are initialized fresh and used
     * INLINE during apply (pull_delta in field_at). M_dval (escape/correction bytes), the two MTF
     * dict streams, and the two index UGolombs all persist through apply. ---- */
    bt_init_rate(&M_dval,4); dr_init(&DR_BL, DR_DIC_BL, DR_KCAP_BL); dr_init(&DR_EX, DR_DIC_EX, DR_KCAP_EX);
    ugg_init(&M_dibl); ugg_init(&M_diex);
    /* ---- [A] streaming apply (no bake): per op read DIRECT geom+P+C, journal P eagerly,
     * then PULL the op's CONTENT from the cut whole-stream LZSS, detect de-reloc fields inline in
     * write order (pulling each delta from the single stream via pull_delta), write via out_write. ---- */
    jr_reset();
    ugg_init(&M_pg); ugg_init(&M_cg); ugg_init(&M_pgn); ugg_init(&M_cgn);
    ugg_init(&M_pg2); ugg_init(&M_cg2);
    ugg_init(&M_gdl); ugg_init(&M_gel); ugg_init(&M_gadj);
    ugg_seed_cont(&M_gdl,3); ugg_seed_cont(&M_gadj,2);
    { SA*s=&SAst; memset(s,0,sizeof*s);
      s->from_size=g_from_size; s->to_size=(int32_t)g_to_size; s->FWD=g_FWD;
      s->tp=g_FWD?0:(int32_t)g_to_size; s->fp=g_FWD?0:(int32_t)g_fp_end;
      ugr_init(&M_gd,11); uint32_t nseq=s_ug_rice(&M_gd);
      int kd=(int)s_raw_bits(4);
      ugr_init(&M_gd,kd); ugg_init(&M_gl); ugg_init(&M_gs);
      fl_init(&M_flag);
      uint32_t nops=s_raw_gz();
      if(g_rcerr || nseq > g_to_size + 1u || nops > g_to_size + 1u){ g_dec_err=1; goto done; }
      s->tok_left=nseq; s->tok_mode=0;
      for(uint32_t oi=0; oi<nops && !s->err && !g_rcerr; oi++) sa_apply_op(s);
      if(!s->err && (s->tp!=(s->FWD?(int32_t)g_to_size:0) || s->fp!=(s->FWD?(int32_t)g_fp_end:0))) s->err=1;
      if(!s->err && (s->tok_mode!=0 || s->tok_left!=0)) s->err=1;
      if(s->err||g_rcerr){ g_dec_err=1; goto done; }
      orow_commit();                       /* flush the last resident output row */
    }
done:
#ifndef RC_V3_STACKMEAS
    /* stack-overflow canary: the deepest 8 B were painted 0xC5; if the coroutine overran them the
     * decode is untrustworthy -> reject (CRC would catch corruption too, but this is direct). */
    for(int i=0;i<8;i++) if((unsigned char)g_dec_stack[i]!=0xC5u){ g_dec_err=1; break; }
#endif
    g_dec_done=1;
    CO_SWAP_TO_MAIN();                      /* final yield: never returns here */
}

/* ===================================================================================== */
/* public push API (SPEC §3.1): decoder_push(byte) -> NEED_MORE / DONE / ERROR             */
/* ===================================================================================== */
enum { DEC_NEED_MORE=0, DEC_DONE=1, DEC_ERROR=2 };

static void __attribute__((noinline)) lit_tree_from_hist(BitTree*t,const uint32_t*hist,uint32_t*w,uint8_t rate){
    for(int s=0;s<256;s++) w[256+s]=hist[s];
    for(int m=255;m>=1;m--) w[m]=w[2*m]+w[2*m+1];
    t->rate=rate; t->p[0]=RC_PHALF;
    for(int m=1;m<256;m++){ uint32_t num=w[2*m],den=w[m]; uint32_t pr=den?(2u*RC_PBIT*num+den)/(2u*den):RC_PHALF;
        t->p[m]=(uint16_t)(pr<1?1:(pr>RC_PBIT-1?RC_PBIT-1:pr)); }
}

/* initialize the decoder coroutine; lit_tree seeds from flash. */
static int decoder_init(void){
    g_fhead=g_ftail=0; g_eof=0; g_dec_done=0; g_dec_err=0;
    /* literal bit-trees seeded from flash parity histograms (no transient image buffer).
     * hist0/hist1/w (4 KiB) borrow ARENA at init — this runs BEFORE the streaming apply reuses
     * ARENA, keeping them off the main thread's peak stack (SPEC §6 / RAM budget). */
    {   uint32_t *hist0=(uint32_t*)ARENA;            /* [0..256)   */
        uint32_t *hist1=hist0+256;                   /* [256..512) */
        uint32_t *w=hist1+256;                       /* [512..1024) -> 4 KiB total, fits ARENA */
        for(int i=0;i<256;i++){ hist0[i]=1; hist1[i]=1; }
        for(uint32_t i=0;i<g_from_size;i++){ uint8_t v=flash_read(i); if(i&1) hist1[v]++; else hist0[v]++; }
        for(int c=0;c<LIT0_CTX;c++) lit_tree_from_hist(&M_lit0[c],hist0,w,5);
        lit_tree_from_hist(&M_lit1,hist1,w,4);
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
    return g_dec_err?DEC_ERROR:(g_dec_done?DEC_DONE:DEC_NEED_MORE);
}

static int decoder_push(uint8_t byte){
    if(g_dec_done) return g_dec_err?DEC_ERROR:DEC_DONE;
    /* enqueue; FIFO should never overflow because the decoder drains as fast as we push */
    if((g_ftail - g_fhead) >= FIFO_CAP){ return DEC_ERROR; }
    g_fifo[g_ftail & FIFO_MASK]=byte; g_ftail++;
    /* resume decoder; it will consume from the FIFO and suspend again when empty */
    CO_SWAP_TO_DEC();
    return g_dec_err?DEC_ERROR:(g_dec_done?DEC_DONE:DEC_NEED_MORE);
}
static int decoder_finish(void){
    /* signal EOF so next_byte() zero-fills; drain until the coroutine completes */
    g_eof=1;
    while(!g_dec_done){ CO_SWAP_TO_DEC(); }
    return g_dec_err?DEC_ERROR:DEC_DONE;
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
uint32_t rcv3_jpeak(void){ return (uint32_t)Jpeak; }
/* rob-1: after a DEC_ERROR, the reject reason — REJ_RESOURCE(1)=cap too small for this firmware
 * (raise a cap, costs SRAM) vs REJ_CORRUPT(2)=malformed stream. See the REJ_* enum. */
uint8_t rcv3_reject(void){ return g_reject?g_reject:REJ_CORRUPT; }
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
    /* parse plaintext header: CRC32(from)[4] | from_size | to_size | fp_end */
    uint32_t want_from_crc = blob[0]|(blob[1]<<8)|(blob[2]<<16)|((uint32_t)blob[3]<<24);
    size_t p=4; int err=0;
    uint32_t from_size=0,to_size=0,fp_end=0; { uint32_t v=0; int sh=0; uint8_t b;
        do{ if(p>=(size_t)bsz||sh>28){err=1;break;} b=blob[p++]; v|=(uint32_t)(b&0x7f)<<sh; sh+=7; }while(b&0x80); from_size=v; }
    { uint32_t v=0; int sh=0; uint8_t b; do{ if(p>=(size_t)bsz||sh>28){err=1;break;} b=blob[p++]; v|=(uint32_t)(b&0x7f)<<sh; sh+=7; }while(b&0x80); to_size=v; }
    { uint32_t v=0; int sh=0; uint8_t b; do{ if(p>=(size_t)bsz||sh>28){err=1;break;} b=blob[p++]; v|=(uint32_t)(b&0x7f)<<sh; sh+=7; }while(b&0x80); fp_end=v; }
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
    fprintf(stderr,"ok to_size=%u dir=%s journal_used=%d slots (cap=%u)\n",to_size,g_FWD?"fwd":"bwd",Jpeak,(unsigned)JSLOTS);
#ifdef RC_V3_NVM
    fprintf(stderr,"NVM: erases=%ld rows=%u programs=%ld amplified=%u maxrowerase=%u inversions=%ld (span=%u rows_total=%u, ideal=span/256)\n",
            nvm_erases(),nvm_rows(),nvm_programs(),nvm_rows_amplified(),nvm_max_row_erases(),nvm_frontier_inversions(),g_image_span,(g_image_span+255)/256);
#endif
    fclose(mf); free(blob);
    return 0;
}
#endif
