/* ultrapatch DIVIDE-FREE range-coder decoder + models (C port of sim/ultrapatch/rc_codec.py).
 * Binary range coder (LZMA bound: bound=(range>>12)*prob; compare) — NO division anywhere,
 * required for Cortex-M0+/ARMv6-M (no hardware divide). Header-only; shared by the unit
 * test and the full decoder. */
#ifndef RC_MODELS_H
#define RC_MODELS_H
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#define RC_KTOP (1u<<24)
#define RC_PBIT 4096u
#define RC_PHALF 2048u

/* ---- divide-free binary range decoder (reads past EOF as 0) ---- */
typedef struct { const uint8_t*d; size_t p,n; uint32_t range,code; } RDec;
static inline uint8_t rd_byte(RDec*r){ uint8_t v = r->p<r->n ? r->d[r->p] : 0; r->p++; return v; }
static inline void rd_init(RDec*r,const uint8_t*d,size_t n){
    r->d=d; r->n=n; r->p=1; r->range=0xFFFFFFFFu; r->code=0;
    for(int i=0;i<4;i++) r->code=(r->code<<8)|rd_byte(r);
}
/* adaptive bit: prob in [1,4095]=P(bit==0). bound = (range>>12)*prob; shift+mul+compare.
 * rate = LZMA adaptation shift (5 = 1/32 default; 4 = 1/16 for the lit_t1 opcode stream). */
static inline int rd_bit_r(RDec*r,uint16_t*prob,int rate){
    uint32_t p=*prob, bound=(r->range>>12)*p; int b;
    if(r->code<bound){ r->range=bound; *prob=(uint16_t)(p+((RC_PBIT-p)>>rate)); b=0; }
    else { r->code-=bound; r->range-=bound; *prob=(uint16_t)(p-(p>>rate)); b=1; }
    while(r->range<RC_KTOP){ r->code=(r->code<<8)|rd_byte(r); r->range<<=8; }
    return b;
}
static inline int rd_bit(RDec*r,uint16_t*prob){ return rd_bit_r(r,prob,5); }
/* uniform p=0.5 bit (shipped raw bits) */
static inline int rd_raw(RDec*r){
    uint32_t bound=r->range>>1; int b;
    if(r->code<bound){ r->range=bound; b=0; } else { r->code-=bound; r->range-=bound; b=1; }
    while(r->range<RC_KTOP){ r->code=(r->code<<8)|rd_byte(r); r->range<<=8; }
    return b;
}
static inline uint32_t rd_raw_bits(RDec*r,int nb){ uint32_t v=0; for(int i=0;i<nb;i++) v=(v<<1)|rd_raw(r); return v; }

/* ---- RAM-less static prefix codes via raw 0.5 bits (no model state) ----
 * For the small [B] fields that don't benefit from adaptation (FIELD_AUDIT.md hybrid).
 * Bit order matches sim codec.BW/BR and the rc_codec w_/r_ writers (canonical Elias/Rice). */
static inline uint32_t rd_raw_gamma(RDec*r){ int n=0; while(rd_raw(r)==0) n++;
    uint32_t v=1; for(int i=0;i<n;i++) v=(v<<1)|rd_raw(r); return v; }
static inline uint32_t rd_raw_gz(RDec*r){ return rd_raw_gamma(r)-1; }              /* x>=0 */
static inline uint32_t rd_raw_dz(RDec*r){ uint32_t n=rd_raw_gamma(r), m=1;         /* Elias-delta of x+1 */
    for(uint32_t i=0;i+1<n;i++) m=(m<<1)|rd_raw(r);
    return m-1; }
static inline uint32_t rd_raw_rice(RDec*r,int k){ uint32_t q=0; while(rd_raw(r)==1) q++;
    uint32_t m=0; for(int i=0;i<k;i++) m=(m<<1)|rd_raw(r); return (q<<k)|m; }

/* ---- 256-symbol byte via 8-level bit-tree; probs[1..255]; rate = adaptation shift ---- */
typedef struct { uint16_t p[256]; uint8_t rate; } BitTree;
static inline void bt_init(BitTree*t){ for(int i=0;i<256;i++) t->p[i]=RC_PHALF; t->rate=5; }
static inline int bt_decode(RDec*r,BitTree*t){
    int m=1; for(int i=0;i<8;i++) m=(m<<1)|rd_bit_r(r,&t->p[m],t->rate); return m-256;
}
/* literal bit-tree seeded from from-image parity histogram (node = P(left subtree)) */
static inline void lit_tree_seed(const uint8_t*frm,size_t n,int parity,BitTree*t){
    uint32_t hist[256]; for(int i=0;i<256;i++) hist[i]=1;
    for(size_t i=0;i<n;i++) if((int)(i&1)==parity) hist[frm[i]]++;
    uint32_t w[512]; for(int s=0;s<256;s++) w[256+s]=hist[s];
    for(int m=255;m>=1;m--) w[m]=w[2*m]+w[2*m+1];
    t->p[0]=RC_PHALF; t->rate=5;
    for(int m=1;m<256;m++){ uint32_t num=w[2*m],den=w[m];
        uint32_t pr=den?(2u*RC_PBIT*num+den)/(2u*den):RC_PHALF;     /* round(4096*num/den) */
        t->p[m]=(uint16_t)(pr<1?1:(pr>RC_PBIT-1?RC_PBIT-1:pr)); }
}

/* ---- seeded Golomb: adaptive unary prefix + adaptive mantissa (static==gamma/Rice) ----
 * UG_CTX = context clamp (SPEC §6 RAM lever). v3 uses 8 (down from v2's 32): m[] is the
 * dominant model array at (UG_CTX+1)^2 u16 each, so this roughly quarters the matrix vs 16.
 * ENCODING-AFFECTING: the golden encoder (rc_codec.UG_CTX) MUST use the identical value. */
#define UG_CTX 8
#define UG_C(x) ((x)<UG_CTX?(x):UG_CTX)
typedef struct { char code; int k; uint16_t u[UG_CTX+1]; uint16_t m[UG_CTX+1][UG_CTX+1]; } UGolomb;
static inline void ug_init(UGolomb*g,char code,int k){
    g->code=code; g->k=k;
    for(int i=0;i<=UG_CTX;i++){ g->u[i]=RC_PHALF; for(int j=0;j<=UG_CTX;j++) g->m[i][j]=RC_PHALF; }
}
static inline uint32_t ug_decode(RDec*r,UGolomb*g){
    int cl=0; while(rd_bit(r,&g->u[UG_C(cl)])==1) cl++;       /* unbounded value, capped context */
    if(g->code=='r'){
        uint32_t v=(uint32_t)cl<<g->k;
        for(int pos=0;pos<g->k;pos++) v|=(uint32_t)rd_bit(r,&g->m[UG_C(cl)][UG_C(pos)])<<(g->k-1-pos);
        return v;
    }
    uint32_t mm=1u<<cl;
    for(int pos=0;pos<cl;pos++) mm|=(uint32_t)rd_bit(r,&g->m[UG_C(cl)][UG_C(pos)])<<(cl-1-pos);
    return mm-1;
}

/* ---- order-2 token flag: 4 contexts (previous 2 flags) ---- */
typedef struct { uint16_t m[4]; int h; } Flag1;
static inline void fl_init(Flag1*f){ for(int i=0;i<4;i++) f->m[i]=RC_PHALF; f->h=0; }
static inline int fl_decode(RDec*r,Flag1*f){ int b=rd_bit(r,&f->m[f->h]); f->h=((f->h<<1)|b)&3; return b; }

/* ---- dict_values: pack_size bytes via adaptive byte bit-tree (static == pack_size) ---- */
typedef struct { BitTree t; } ByteVarint;
static inline void bv_init(ByteVarint*v){ bt_init(&v->t); }
static inline int32_t bv_decode(RDec*r,ByteVarint*v){
    int b0=bt_decode(r,&v->t); int sgn=b0&0x40; uint32_t val=b0&0x3f; int off=6;
    while(b0&0x80){ b0=bt_decode(r,&v->t); val|=(uint32_t)(b0&0x7f)<<off; off+=7; }
    return sgn? -(int32_t)val : (int32_t)val;
}

#endif
