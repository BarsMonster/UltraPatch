/*
 * ultrapatch in-place decoder (C) — SHIPPING/PRODUCTION codec.
 * Divide-free binary range coder (LZMA bound: bound=(range>>12)*prob; compare — no division,
 * required for Cortex-M0+). Mirrors the golden model sim/ultrapatch/rc_codec.py byte-for-byte.
 * Wire spec: testbench/reference/RANGE_CODER.md; per-field models: FIELD_AUDIT.md.
 * (Supersedes the legacy prefix-code decoder c/ultrapatch.c / FORMAT.md, which is kept only
 * as the size-comparison baseline. −7.37% vs that format; 256/256 byte-exact.)
 *
 * Blob = CRC32(from)[4] + from_size[uLEB128] + body + CRC32(to)[4]. The body is ONE range
 * stream over [A] then [B]:
 * [A] = LZSS-C control: token_count, backref_dist Rice k (4b), then per token an order-2
 *       flag + (span_len + literals) or (backref_dist + backref_len). Literals are two
 *       from-image-seeded bit-trees by halfword parity (lit_t1 adapts at 1/16). 0 shipped tables.
 * [B] = per active stream (M0+ ships data,code,bl,ldr): capture blocks, dict + double-RLE
 *       deltas, exceptions. positions/dict_idx/run_len use adaptive UGolomb; the 8 small
 *       fields use RAM-less static Elias/Rice (raw bits) so peak model RAM fits the 32 KB M0+.
 * Reconstruct: to[i] = ctrl_diff[i] + fromzero[fp] + dfdiff[i]. Own m4 reconstructor (no
 * detools_m4_build). Models from the from-image / adaptive (nothing shipped). True
 * single-buffer in-place with a bounded RAM journal.
 *
 * Embedded target: NO 64-bit integers anywhere. All wire values, positions and sizes are
 * 32-bit. Varints/Elias/packsize readers cap their shift so a >32-bit field is rejected
 * (the golden encoder never emits one). Heap is used only for transient host-side buffers;
 * nothing dynamic is fixed-capacity (the value dictionary is sized to the stream).
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include "arm_cortex_m4.h"   /* detools_m4_disassemble / detools_m4_pack */
#include "rc_models.h"       /* range decoder + seeded models (RDec, UGolomb, Flag1, BitTree, ByteVarint) */

/* ---------- byte-buffer varints (control stream) ---------- */
/* unsigned LEB128 from byte buffer (E1: control varints are uLEB128); bounded, never OOB.
 * 32-bit only: sh>28 (a 6th continuation byte) means the value exceeds 32 bits -> error. */
static int32_t bb_uleb(const uint8_t *d, size_t *pos, size_t len, int *err){
    uint32_t v=0; int sh=0; uint8_t b;
    do{ if(*pos>=len||sh>28){ *err=1; return 0; } b=d[(*pos)++]; v|=(uint32_t)(b&0x7f)<<sh; sh+=7; }while(b&0x80);
    return (int32_t)v;
}
static int32_t bb_unzz(int32_t u){ return (u&1)? -((uint32_t)(u+1)>>1) : ((uint32_t)u>>1); }   /* zigzag decode (adj) */

/* ---------- MSB-first bit reader (bounded) ---------- */
typedef struct { const uint8_t *d; size_t bitpos; size_t nbits; int err; } BR;
static int br_bit(BR*r){ if(r->bitpos>=r->nbits){ r->err=1; return 0; } int b=(r->d[r->bitpos>>3]>>(7-(r->bitpos&7)))&1; r->bitpos++; return b; }
static uint32_t br_bits(BR*r,int k){ uint32_t v=0; while(k--) v=(v<<1)|br_bit(r); return v; }
/* Elias gamma. Cap at 31 leading zeros so the value stays within 32 bits. */
static uint32_t br_gamma(BR*r){ int n=0; while(br_bit(r)==0){ if(r->err||++n>31){ r->err=1; return 1; } } uint32_t v=1; while(n--) v=(v<<1)|br_bit(r); return v; }
static int32_t br_packsize(BR*r){ /* read a pack_size varint via 8-bit groups (matches Python bits(8)) */
    uint8_t b=br_bits(r,8); int sgn=b&0x40; int32_t v=b&0x3f; int off=6;
    while(b&0x80){ if(r->err||off>28){ r->err=1; return 0; } b=br_bits(r,8); v|=(int32_t)(b&0x7f)<<off; off+=7; }
    return sgn? -v : v;
}
/* Elias delta (mirrors BR.delta): n=gamma(); m=1; read n-1 bits. dz = delta of x+1, x>=0. */
static int32_t br_delta(BR*r){ uint32_t n=br_gamma(r); uint32_t m=1; for(uint32_t i=1;i<n && !r->err;i++) m=(m<<1)|br_bit(r); return (int32_t)m; }
static int32_t br_dz(BR*r){ return br_delta(r)-1; }
/* Rice decode (E2/E6): q ones, terminating 0, then k low bits. */
static uint32_t br_rice(BR*r,int k){ uint32_t q=0; while(br_bit(r)==1){ if(r->err||++q>1u<<20){ r->err=1; break; } } return (q<<k)|br_bits(r,k); }

/* ---------- deterministic Huffman (identical to Python huff_lengths) ---------- */
typedef struct { uint8_t len[256]; int count[33]; int firstidx[33]; uint16_t firstcode[33];
                 uint8_t sorted[256]; int maxlen; } Huff;
/* parity: <0 = all bytes; 0 = low byte (even from-addr); 1 = high byte (odd) */
static void huff_build(const uint8_t *from, size_t fsize, int parity, Huff*h){
    uint32_t freq[256]; for(int i=0;i<256;i++) freq[i]=1;          /* Laplace +1 */
    for(size_t i=0;i<fsize;i++) if(parity<0 || (int)(i&1)==parity) freq[from[i]]++;
    /* find-min tree, tie-break by node id; 256 leaves + up to 255 internals.
       Total weight <= fsize + 256, so 32-bit weights never overflow on this target. */
    uint32_t wt[512]; int parent[512];
    for(int i=0;i<512;i++){ parent[i]=-1; wt[i]= i<256? freq[i]:0; }
    int active[512]; int na=256; for(int i=0;i<256;i++) active[i]=i; int m=256;
    while(na>1){
        int ai=-1,bi=-1;            /* two smallest (weight,id) among active */
        for(int k=0;k<na;k++){ int nid=active[k];
            if(ai<0 || wt[nid]<wt[ai] || (wt[nid]==wt[ai]&&nid<ai)){ bi=ai; ai=nid; }
            else if(bi<0 || wt[nid]<wt[bi] || (wt[nid]==wt[bi]&&nid<bi)){ bi=nid; } }
        wt[m]=wt[ai]+wt[bi]; parent[ai]=m; parent[bi]=m;
        /* remove ai,bi from active, add m */
        int w=0; for(int k=0;k<na;k++){ if(active[k]!=ai&&active[k]!=bi) active[w++]=active[k]; }
        active[w++]=m; na=w; m++;
    }
    h->maxlen=0;
    for(int s=0;s<256;s++){ int d=0,p=s; while(parent[p]!=-1){ p=parent[p]; d++; } h->len[s]=d; if(d>h->maxlen)h->maxlen=d; }
    /* canonical: sort by (len, sym); build first_code/first_idx tables */
    for(int l=0;l<=32;l++) h->count[l]=0;
    for(int s=0;s<256;s++) h->count[h->len[s]]++;
    int idx=0; for(int l=1;l<=h->maxlen;l++){ h->firstidx[l]=idx; for(int s=0;s<256;s++) if(h->len[s]==l) h->sorted[idx++]=s; }
    uint32_t code=0; for(int l=1;l<=h->maxlen;l++){ h->firstcode[l]=code; code=(code+h->count[l])<<1; }
}
static int huff_decode(BR*r, Huff*h){
    uint32_t code=0;
    for(int l=1;l<=h->maxlen;l++){
        code=(code<<1)|br_bit(r);
        int off=(int)code - (int)h->firstcode[l];
        if(off>=0 && off<h->count[l]) return h->sorted[h->firstidx[l]+off];
    }
    return -1;
}

/* ---------- control-stream table scanner (mirrors codec.py CtrlScanner) ----------
 * Picks the literal Huffman table per control byte: table 1 only for an EXTRA
 * (new-code) byte at an odd to-address, else table 0. Run incrementally during decode. */
enum { PH_NOPS, PH_DIFFLEN, PH_NLITS, PH_GAP, PH_LITVAL, PH_EXTRALEN, PH_EXTRA, PH_ADJ, PH_DONE };
typedef struct { int ph,shift,first; int32_t acc,ops,lits,exleft,expos,exstart,tp,dl; } CScan;
static void cs_init(CScan*s){ memset(s,0,sizeof*s); s->ph=PH_NOPS; s->first=1; }
static int cs_tag(CScan*s){ return s->ph==PH_EXTRA ? (int)((s->exstart+s->expos)&1) : 0; }
static void cs_adv(CScan*s, uint8_t b){
    int ph=s->ph;
    if(ph==PH_NOPS||ph==PH_DIFFLEN||ph==PH_NLITS||ph==PH_GAP||ph==PH_EXTRALEN||ph==PH_ADJ){
        if(s->first){ s->acc=b&0x7f; s->shift=7; s->first=0; }   /* E1: uLEB128 (7-bit first byte) */
        else { s->acc |= (int32_t)(b&0x7f)<<s->shift; s->shift+=7; }
        if(b&0x80) return;
        int32_t v=s->acc; s->first=1;
        if(ph==PH_NOPS){ s->ops=v; s->ph=v==0?PH_DONE:PH_DIFFLEN; }
        else if(ph==PH_DIFFLEN){ s->dl=v; s->exstart=s->tp+v; s->ph=PH_NLITS; }
        else if(ph==PH_NLITS){ s->lits=v; s->ph=v>0?PH_GAP:PH_EXTRALEN; }
        else if(ph==PH_GAP){ s->ph=PH_LITVAL; }
        else if(ph==PH_EXTRALEN){ s->exleft=v; s->expos=0; s->tp=s->exstart+v; s->ph=v>0?PH_EXTRA:PH_ADJ; }
        else { s->ops--; s->ph=s->ops>0?PH_DIFFLEN:PH_DONE; }   /* PH_ADJ */
    } else if(ph==PH_LITVAL){ s->lits--; s->ph=s->lits>0?PH_GAP:PH_EXTRALEN; }
    else if(ph==PH_EXTRA){ s->expos++; s->exleft--; if(s->exleft==0) s->ph=PH_ADJ; }
}

/* ---------- decode_A: LZSS-C -> control bytes, SELF-TERMINATING (v2) ----------
 * No shipped clen/lenA: [A] ends after its nseq tokens. avail bounds the bit reader
 * (everything from [A] start to end of body). Returns clen via *clen_out and the
 * byte-aligned consumed length (= [B] offset) via *lenA_out. out is malloc'd to cap. */
static int decodeA(const uint8_t*A, size_t avail, Huff*h0, Huff*h1, uint8_t*out, size_t cap,
                   size_t*clen_out, size_t*lenA_out){
    BR r={A,0,avail*8,0};
    uint32_t rk=br_bits(&r,4);                 /* E2: per-patch Rice k for backref distance */
    uint32_t lk=br_bits(&r,4);                 /* E8: per-patch Rice k for backref length (len-3) */
    uint32_t nseq=br_gamma(&r)-1;
    size_t o=0; CScan sc; cs_init(&sc);
    for(uint32_t s=0;s<nseq && !r.err;s++){
        if(br_bit(&r)==0){ uint32_t ln=br_gamma(&r);
            while(ln-- && !r.err){ int sym=huff_decode(&r, cs_tag(&sc)?h1:h0);
                if(sym<0||o>=cap){ r.err=1; break; } out[o++]=(uint8_t)sym; cs_adv(&sc,(uint8_t)sym); } }
        else { uint32_t d=br_rice(&r,rk)+1; uint32_t ln=br_rice(&r,lk)+3;   /* E2 Rice dist, E8 Rice len */
            if(d>o){ r.err=1; break; } size_t st=o-d;
            for(uint32_t i=0;i<ln;i++){ if(o>=cap){ r.err=1; break; } uint8_t b=out[st+i]; out[o++]=b; cs_adv(&sc,b); } }
    }
    if(r.err) return 1;
    *clen_out=o; *lenA_out=(r.bitpos+7)/8;
    return 0;
}

/* ---------- CRC32 (zlib polynomial, matches Python zlib.crc32) ---------- */
static uint32_t crc32_buf(const uint8_t*d, size_t n){
    uint32_t c=0xffffffffu;
    for(size_t i=0;i<n;i++){ c^=d[i];
        for(int k=0;k<8;k++) c = (c>>1) ^ (0xedb88320u & (-(int32_t)(c&1))); }
    return c^0xffffffffu;
}

/* ---------- bounded RAM journal (configurable) for true single-buffer in-place ----------
 * One shared buffer holds `from` (zeroed-field = fromzero) and is overwritten by `to`.
 * Grow -> apply backward (write hi->lo); shrink -> forward. A source byte that the
 * write frontier overwrites before its last read is stashed here. Open-addressing
 * hash, backward-shift deletion; refuses to exceed the configured journal budget. */
/* Overlap buffer — a FIXED-SIZE STATIC array, not a dynamic structure.
 * In true single-buffer in-place, writing a new byte clobbers an old byte that a later copy
 * still needs as its source; only those few old bytes are held here until their last read.
 * That working set is tiny and bounded (256-pair corpus: peak 836 B, median 9 B, half need 0
 * — and a fixed 1 KiB buffer covers every pair with ZERO spill), so the buffer is sized once
 * at compile time. Long-range references do NOT live here: they read the old image directly
 * from flash (random-access, 0 RAM); a fixed *reference* window is infeasible (measured +478%
 * at 256 B). Only the fill level moves (like a stack); the allocation never changes.
 * Open addressing is just the index INTO this fixed array — no malloc, no resize. */
#ifndef ULTRAPATCH_JOURNAL_BYTES
#define ULTRAPATCH_JOURNAL_BYTES 1024          /* default 1 KiB budget (corpus peak 836 B) */
#endif
#define JCAP 2048u                             /* fixed slots (pow2); load <= 1/2 at full budget */
#define JMASK (JCAP-1u)
#define JMAX_BYTES (JCAP/2)                     /* budget ceiling = half the fixed buffer */
static int Jlimit = ULTRAPATCH_JOURNAL_BYTES;  /* fill cap (entries); <= JMAX_BYTES */
/* Packed 4-byte slot: 0 = empty; else ((pos+1)<<8)|byte. Supports positions < 16 MiB-1, far
 * beyond any in-place target that fits a microcontroller (jr_put rejects anything larger). */
static uint32_t J[JCAP]; static int Jcount, Jpeak;   /* the fixed buffer: 2048*4 = 8 KiB */
static size_t jhash(uint32_t p){ return (size_t)(((uint64_t)p*2654435761u)>>11)&JMASK; }
static void jr_reset(void){ memset(J,0,sizeof J); Jcount=Jpeak=0; }
static int jr_put(uint32_t pos, uint8_t b){
    if(pos>=0xFFFFFFu) return -1;                      /* out of packed range (>= 16 MiB-1) */
    uint32_t key=(pos+1)<<8; size_t i=jhash(pos);
    while(J[i]){ if((J[i]&~0xFFu)==key){ J[i]=key|b; return 0; } i=(i+1)&JMASK; }
    if(Jcount>=Jlimit) return -1;                      /* would exceed the journal budget */
    J[i]=key|b; if(++Jcount>Jpeak) Jpeak=Jcount; return 0;
}
static int jr_get(uint32_t pos, uint8_t*out){
    uint32_t key=(pos+1)<<8; size_t i=jhash(pos);
    while(J[i]){ if((J[i]&~0xFFu)==key){ *out=(uint8_t)(J[i]&0xFFu); return 1; } i=(i+1)&JMASK; }
    return 0;
}
static void jr_del(uint32_t pos){
    uint32_t key=(pos+1)<<8; size_t i=jhash(pos);
    while(J[i] && (J[i]&~0xFFu)!=key) i=(i+1)&JMASK;
    if(!J[i]) return;
    J[i]=0; Jcount--;
    size_t j=(i+1)&JMASK;
    while(J[j]){
        size_t k=jhash((J[j]>>8)-1); int movable;
        if(i<=j) movable = !(i<k && k<=j); else movable = !(i<k || k<=j);
        if(movable){ J[i]=J[j]; J[j]=0; i=j; }
        j=(j+1)&JMASK;
    }
}

/* ---------- v2 relocation decode (own reconstructor, no detools_m4_build) ---------- */
static int32_t br_gz(BR*r){ return (int32_t)br_gamma(r)-1; }
static int32_t unzz(int32_t u){ return (u&1)? -((uint32_t)(u+1)>>1) : ((uint32_t)u>>1); }   /* signed block gap */

/* decode one double-RLE value stream into deltas[cnt] (zeros + dict values).
 * The dictionary is sized to the stream (dynamic) — no fixed cap. A corrupt K is
 * bounded by the bits actually left in the reader (each entry costs >=8 bits). */
static int dec_vstream(BR*r, int32_t cnt, int32_t*deltas){
    uint32_t K=br_gamma(r)-1;
    uint32_t maxK=(uint32_t)((r->nbits - r->bitpos)/8 + 1);   /* >=8 bits per dict entry */
    if(r->err||K>maxK) return -1;
    int32_t *dict=malloc((K?K:1)*sizeof(int32_t)); if(!dict) return -1;
    for(uint32_t i=0;i<K && !r->err;i++) dict[i]=br_packsize(r);
    int32_t N=(int32_t)br_gamma(r)-1; if(r->err||N<0||N>cnt){ free(dict); return -1; }
    int32_t *gaps=malloc((N?N:1)*sizeof(int32_t)); int32_t *nz=malloc((N?N:1)*sizeof(int32_t));
    if(!gaps||!nz){ free(dict); free(gaps); free(nz); return -1; }
    for(int32_t k=0;k<N && !r->err;k++){ gaps[k]=br_gz(r); if(gaps[k]<0){ r->err=1; break; } }
    int sel = (N>=16) ? (int)br_bits(r,4) : 15;   /* E6: gated dict-index selector (Rice k 0..9, 15=gz) */
    int32_t prod=0;
    while(prod<N && !r->err){
        uint32_t di = (sel==15) ? (br_gamma(r)-1) : br_rice(r,sel);  /* gz or Rice */
        uint32_t rl=br_gamma(r);
        if(di>=K){ r->err=1; break; } for(uint32_t t=0;t<rl && prod<N;t++) nz[prod++]=dict[di]; }
    int32_t p=0;
    for(int32_t k=0;k<N;k++){ for(int32_t z=0;z<gaps[k] && p<cnt;z++) deltas[p++]=0; if(p<cnt) deltas[p++]=nz[k]; }
    while(p<cnt) deltas[p++]=0;
    int rc = r->err?-1:0;
    free(dict); free(gaps); free(nz);
    return rc;
}

/* derive to-position of from-address faddr from the fp->tp copy segments (or -1) */
static int32_t topos(int32_t faddr, const int32_t*Ofp, const int32_t*Otp, const int32_t*Odl, int32_t nops){
    for(int32_t op=0;op<nops;op++)
        if(Ofp[op]<=faddr && faddr<Ofp[op]+Odl[op]) return Otp[op]+(faddr-Ofp[op]);
    return -1;
}

/* build fromzero (captured fields zeroed) + dfdiff (relocated values at derived to_pos) */
static int decodeB_v2(const uint8_t*B, size_t lenB, m4_stream_t streams[M4_NSTREAMS],
                      const int32_t*Ofp, const int32_t*Otp, const int32_t*Odl, int32_t nops,
                      uint8_t*fz, size_t from_size, uint8_t*df, int32_t to_size){
    static const int PK[M4_NSTREAMS]={M4_PK_S32,M4_PK_S32,M4_PK_BW,M4_PK_BL,M4_PK_S32,M4_PK_S32};
    /* Production stream config: Thumb-1/M0+ ships data,code,bl,ldr (skip BW, LDRW). Must
       match codec_v2 DEFAULT_STREAMS / ORDER ordering. */
    static const int ACTIVE[]={M4_DATA,M4_CODE,M4_BL,M4_LDR};
    const int NACTIVE=(int)(sizeof(ACTIVE)/sizeof(ACTIVE[0]));
    BR r={B,0,lenB*8,0};
    for(int ai=0;ai<NACTIVE;ai++){
        int s=ACTIVE[ai];
        m4_stream_t*st=&streams[s];
        int32_t nblk=(int32_t)br_gamma(&r)-1; if(r.err||nblk<0||nblk>(int32_t)st->n+1) return -1;
        int32_t *bgap=malloc((nblk?nblk:1)*sizeof(int32_t)), *bn=malloc((nblk?nblk:1)*sizeof(int32_t)), cnt=0;
        if(!bgap||!bn){ free(bgap); free(bn); return -1; }
        for(int32_t b=0;b<nblk && !r.err;b++){ bgap[b]=unzz(br_gz(&r)); bn[b]=br_dz(&r)+1; cnt+=bn[b]; }  /* E5: n via Elias-delta */
        if(r.err||cnt<0||cnt>(int32_t)st->n+nblk){ free(bgap); free(bn); return -1; }
        int32_t *deltas=malloc((cnt?cnt:1)*sizeof(int32_t));
        if(!deltas||dec_vstream(&r,cnt,deltas)){ free(bgap); free(bn); free(deltas); return -1; }
        int32_t nexc=(int32_t)br_gamma(&r)-1; if(r.err||nexc<0){ free(bgap); free(bn); free(deltas); return -1; }
        int32_t *ecs=malloc((nexc?nexc:1)*sizeof(int32_t)), *etp=malloc((nexc?nexc:1)*sizeof(int32_t)), cs=0, pt=0;
        if(!ecs||!etp){ free(bgap); free(bn); free(deltas); free(ecs); free(etp); return -1; }
        /* E5: to_pos is zigzag-delta of the previous exception's to_pos */
        for(int32_t e=0;e<nexc && !r.err;e++){ cs+=br_gz(&r); ecs[e]=cs; pt+=unzz(br_dz(&r)); etp[e]=pt; }
        int32_t di=0, capseq=0, pos=0, ei=0; int bad=r.err;
        for(int32_t b=0;b<nblk && !bad;b++){
            pos+=bgap[b];
            for(int32_t k=0;k<bn[b];k++){
                if(pos+k<0||pos+k>=(int32_t)st->n){ bad=1; break; }
                uint32_t faddr=st->a[pos+k].addr; int32_t fval=st->a[pos+k].val; int32_t delta=deltas[di++];
                if((size_t)faddr+4<=from_size) memset(fz+faddr,0,4);
                int32_t tpos;
                if(ei<nexc && ecs[ei]==capseq){ tpos=etp[ei++]; }
                else tpos=topos((int32_t)faddr,Ofp,Otp,Odl,nops);
                capseq++;
                if(tpos>=0 && tpos+4<=to_size){ uint8_t enc[4];
                    detools_m4_pack(PK[s],(int32_t)((uint32_t)fval-(uint32_t)delta),enc); memcpy(df+tpos,enc,4); }
            }
            pos+=bn[b];
        }
        free(bgap); free(bn); free(deltas); free(ecs); free(etp);
        if(bad||r.err) return -1;
    }
    return 0;
}

/* ====================== RANGE-CODER decode (mirrors sim/ultrapatch/rc_codec.py) ====================== */
/* [A]: one range stream -> control bytes. token_count, shipped Rice k (4b x2), then per
 * token: order-1 flag + (span_len + literals) or (backref_dist + backref_len). Literal
 * table chosen by CScan parity, exactly as production. */
static int rc_decodeA(RDec*r, BitTree*M0, BitTree*M1, uint8_t*out, size_t cap, size_t*clen_out){
    UGolomb tc; ug_init(&tc,'r',11);                  /* token_count: baked Rice k=11 */
    uint32_t nseq=ug_decode(r,&tc);
    int kd=(int)rd_raw_bits(r,4);                      /* only backref_dist ships k now */
    UGolomb gd; ug_init(&gd,'r',kd); UGolomb gl; ug_init(&gl,'g',0); UGolomb gs; ug_init(&gs,'g',0);  /* backref_len -> gamma */
    Flag1 fl; fl_init(&fl); CScan sc; cs_init(&sc);
    size_t o=0;
    for(uint32_t s=0;s<nseq;s++){
        if(fl_decode(r,&fl)==0){
            uint32_t ln=ug_decode(r,&gs)+1;                 /* span_len stored len-1 */
            for(uint32_t i=0;i<ln;i++){ if(o>=cap) return 1;
                int t=cs_tag(&sc); int b=bt_decode(r,t?M1:M0); out[o++]=(uint8_t)b; cs_adv(&sc,(uint8_t)b); }
        } else {
            uint32_t d=ug_decode(r,&gd)+1, ln=ug_decode(r,&gl)+3;   /* dist-1, len-3 */
            if(d>o) return 1; size_t st=o-d;
            for(uint32_t i=0;i<ln;i++){ if(o>=cap) return 1; uint8_t b=out[st+i]; out[o++]=b; cs_adv(&sc,b); }
        }
    }
    *clen_out=o; return 0;
}

/* [B]: continues the same range stream. Per active stream: nblk, (block_gap,block_n)*,
 * dict (size, values, nz_count, positions, dict_idx/run_len runs), exceptions. Then the
 * SAME reconstruction as decodeB_v2 (fromzero zeroing + dfdiff via topos/pack). */
static int rc_decodeB(RDec*r, m4_stream_t streams[M4_NSTREAMS],
                      const int32_t*Ofp,const int32_t*Otp,const int32_t*Odl,int32_t nops,
                      uint8_t*fz,size_t from_size,uint8_t*df,int32_t to_size){
    static const int PK[M4_NSTREAMS]={M4_PK_S32,M4_PK_S32,M4_PK_BW,M4_PK_BL,M4_PK_S32,M4_PK_S32};
    static const int ACTIVE[]={M4_DATA,M4_CODE,M4_BL,M4_LDR}; const int NACTIVE=4;
    /* [B] hybrid (FIELD_AUDIT.md): only positions/dict_idx/run_len stay adaptive UGolomb
     * (3 x 2,252 B); the other 8 fields are RAM-less static codes -> [B] model RAM ~7 KB. */
    UGolomb m_pos,m_didx,m_rl;
    ug_init(&m_pos,'g',0); ug_init(&m_didx,'g',0); ug_init(&m_rl,'g',0);
    ByteVarint bv; bv_init(&bv);
    int ret=-1;
    for(int ai=0;ai<NACTIVE;ai++){
        int s=ACTIVE[ai]; m4_stream_t*st=&streams[s];
        int32_t nblk=(int32_t)rd_raw_gz(r); if(nblk<0||nblk>(int32_t)st->n+1) goto done;   /* nblk: static gamma */
        int32_t *bgap=malloc((nblk?nblk:1)*sizeof(int32_t)), *bn=malloc((nblk?nblk:1)*sizeof(int32_t)), cnt=0;
        if(!bgap||!bn){ free(bgap); free(bn); goto done; }
        for(int32_t b=0;b<nblk;b++){ bgap[b]=unzz((int32_t)rd_raw_dz(r)); bn[b]=(int32_t)rd_raw_rice(r,7)+1; cnt+=bn[b]; }  /* block_gap: delta; block_n: Rice k7 */
        if(cnt<0||cnt>(int32_t)st->n+nblk){ free(bgap); free(bn); goto done; }
        /* value stream: dict + positions + double-RLE */
        uint32_t K=rd_raw_dz(r);                                                            /* dict_size: static delta */
        int32_t *dict=malloc((K?K:1)*sizeof(int32_t)); if(!dict){ free(bgap); free(bn); goto done; }
        for(uint32_t i=0;i<K;i++) dict[i]=bv_decode(r,&bv);
        int32_t N=(int32_t)rd_raw_dz(r); if(N<0||N>cnt){ free(bgap); free(bn); free(dict); goto done; }  /* nz_count: static delta */
        int32_t *gaps=malloc((N?N:1)*sizeof(int32_t)), *nz=malloc((N?N:1)*sizeof(int32_t)), *deltas=malloc((cnt?cnt:1)*sizeof(int32_t));
        if(!gaps||!nz||!deltas){ free(bgap); free(bn); free(dict); free(gaps); free(nz); free(deltas); goto done; }
        for(int32_t k=0;k<N;k++) gaps[k]=(int32_t)ug_decode(r,&m_pos);
        int32_t prod=0;
        while(prod<N){ uint32_t di=ug_decode(r,&m_didx); uint32_t rl=ug_decode(r,&m_rl)+1;
            if(di>=K){ free(bgap); free(bn); free(dict); free(gaps); free(nz); free(deltas); goto done; }
            for(uint32_t t=0;t<rl && prod<N;t++) nz[prod++]=dict[di]; }
        int32_t pp=0;
        for(int32_t k=0;k<N;k++){ for(int32_t z=0;z<gaps[k] && pp<cnt;z++) deltas[pp++]=0; if(pp<cnt) deltas[pp++]=nz[k]; }
        while(pp<cnt) deltas[pp++]=0;
        /* exceptions */
        int32_t nexc=(int32_t)rd_raw_gz(r); if(nexc<0){ free(bgap); free(bn); free(dict); free(gaps); free(nz); free(deltas); goto done; }  /* exc_count: static gamma */
        int32_t *ecs=malloc((nexc?nexc:1)*sizeof(int32_t)), *etp=malloc((nexc?nexc:1)*sizeof(int32_t)), cs=0, pt=0;
        if(!ecs||!etp){ free(bgap); free(bn); free(dict); free(gaps); free(nz); free(deltas); free(ecs); free(etp); goto done; }
        for(int32_t e=0;e<nexc;e++){ cs+=(int32_t)rd_raw_rice(r,7); ecs[e]=cs; pt+=unzz((int32_t)rd_raw_rice(r,14)); etp[e]=pt; }  /* exc_gap k7; exc_tpos k14 */
        /* apply (identical to decodeB_v2) */
        int32_t di=0, capseq=0, pos=0, ei=0, bad=0;
        for(int32_t b=0;b<nblk && !bad;b++){ pos+=bgap[b];
            for(int32_t k=0;k<bn[b];k++){
                if(pos+k<0||pos+k>=(int32_t)st->n){ bad=1; break; }
                uint32_t faddr=st->a[pos+k].addr; int32_t fval=st->a[pos+k].val; int32_t delta=deltas[di++];
                if((size_t)faddr+4<=from_size) memset(fz+faddr,0,4);
                int32_t tpos;
                if(ei<nexc && ecs[ei]==capseq) tpos=etp[ei++];
                else tpos=topos((int32_t)faddr,Ofp,Otp,Odl,nops);
                capseq++;
                if(tpos>=0 && tpos+4<=to_size){ uint8_t enc[4];
                    detools_m4_pack(PK[s],(int32_t)((uint32_t)fval-(uint32_t)delta),enc); memcpy(df+tpos,enc,4); }
            }
            pos+=bn[b];
        }
        free(bgap); free(bn); free(dict); free(gaps); free(nz); free(deltas); free(ecs); free(etp);
        if(bad) goto done;
    }
    ret=0;
done:
    return ret;
}

/* [B] BAKE: same parse as rc_decodeB, but write pack(fval-delta) INTO buf at each captured
 * faddr (from-order) instead of building a dfdiff array. The ordinary [A] copy then carries
 * each relocated value to its to-position; rare multi-read/exception cases are fixed by the
 * [C] additive corrections. Exceptions in the [B] stream are consumed (for alignment) but
 * ignored here. No topos, no dfdiff, no fromzero -> O(window). */
static int rc_bake(RDec*r, m4_stream_t streams[M4_NSTREAMS], uint8_t*buf, size_t from_size){
    static const int PK[M4_NSTREAMS]={M4_PK_S32,M4_PK_S32,M4_PK_BW,M4_PK_BL,M4_PK_S32,M4_PK_S32};
    static const int ACTIVE[]={M4_DATA,M4_CODE,M4_BL,M4_LDR}; const int NACTIVE=4;
    UGolomb m_pos,m_didx,m_rl; ug_init(&m_pos,'g',0); ug_init(&m_didx,'g',0); ug_init(&m_rl,'g',0);
    ByteVarint bv; bv_init(&bv); int ret=-1;
    for(int ai=0;ai<NACTIVE;ai++){
        int s=ACTIVE[ai]; m4_stream_t*st=&streams[s];
        int32_t nblk=(int32_t)rd_raw_gz(r); if(nblk<0||nblk>(int32_t)st->n+1) goto done;
        int32_t *bgap=malloc((nblk?nblk:1)*sizeof(int32_t)), *bn=malloc((nblk?nblk:1)*sizeof(int32_t)), cnt=0;
        if(!bgap||!bn){ free(bgap); free(bn); goto done; }
        for(int32_t b=0;b<nblk;b++){ bgap[b]=unzz((int32_t)rd_raw_dz(r)); bn[b]=(int32_t)rd_raw_rice(r,7)+1; cnt+=bn[b]; }
        if(cnt<0||cnt>(int32_t)st->n+nblk){ free(bgap); free(bn); goto done; }
        uint32_t K=rd_raw_dz(r); int32_t *dict=malloc((K?K:1)*sizeof(int32_t));
        if(!dict){ free(bgap); free(bn); goto done; }
        for(uint32_t i=0;i<K;i++) dict[i]=bv_decode(r,&bv);
        int32_t N=(int32_t)rd_raw_dz(r); if(N<0||N>cnt){ free(bgap); free(bn); free(dict); goto done; }
        int32_t *gaps=malloc((N?N:1)*sizeof(int32_t)), *nz=malloc((N?N:1)*sizeof(int32_t)), *deltas=malloc((cnt?cnt:1)*sizeof(int32_t));
        if(!gaps||!nz||!deltas){ free(bgap); free(bn); free(dict); free(gaps); free(nz); free(deltas); goto done; }
        for(int32_t k=0;k<N;k++) gaps[k]=(int32_t)ug_decode(r,&m_pos);
        int32_t prod=0;
        while(prod<N){ uint32_t di=ug_decode(r,&m_didx); uint32_t rl=ug_decode(r,&m_rl)+1;
            if(di>=K){ free(bgap); free(bn); free(dict); free(gaps); free(nz); free(deltas); goto done; }
            for(uint32_t t=0;t<rl && prod<N;t++) nz[prod++]=dict[di]; }
        int32_t pp=0;
        for(int32_t k=0;k<N;k++){ for(int32_t z=0;z<gaps[k] && pp<cnt;z++) deltas[pp++]=0; if(pp<cnt) deltas[pp++]=nz[k]; }
        while(pp<cnt) deltas[pp++]=0;
        int32_t nexc=(int32_t)rd_raw_gz(r);                  /* consume exceptions (handled by [C]) */
        if(nexc<0){ free(bgap); free(bn); free(dict); free(gaps); free(nz); free(deltas); goto done; }
        for(int32_t e=0;e<nexc;e++){ (void)rd_raw_rice(r,7); (void)rd_raw_rice(r,14); }
        int32_t di=0, pos=0, bad=0;
        for(int32_t b=0;b<nblk && !bad;b++){ pos+=bgap[b];
            for(int32_t k=0;k<bn[b];k++){
                if(pos+k<0||pos+k>=(int32_t)st->n){ bad=1; break; }
                uint32_t faddr=st->a[pos+k].addr; int32_t fval=st->a[pos+k].val; int32_t delta=deltas[di++];
                if((size_t)faddr+4<=from_size){ uint8_t enc[4];
                    detools_m4_pack(PK[s],(int32_t)((uint32_t)fval-(uint32_t)delta),enc); memcpy(buf+faddr,enc,4); }
            }
            pos+=bn[b];
        }
        free(bgap); free(bn); free(dict); free(gaps); free(nz); free(deltas);
        if(bad) goto done;
    }
    ret=0;
done:
    return ret;
}

/* additive correction lookup ([C], sorted ascending, tiny) */
static int corr_get(const int32_t*ct,const uint8_t*cb,int32_t nc,int32_t tp){
    int32_t lo=0,hi=nc-1;
    while(lo<=hi){ int32_t m=(lo+hi)/2; if(ct[m]==tp) return cb[m]; if(ct[m]<tp) lo=m+1; else hi=m-1; }
    return 0;
}

/* ===== STREAMING [A]: decode op-by-op via a small LZSS ring, apply each op immediately =====
 * No `ctrl` (4xIMG) and no per-op arrays. Control bytes go into a ring (LZSS history, dist<=2048)
 * and feed an incremental op parser; on each complete op we apply it to `buf` with the
 * never-evict journal + [C] corrections. Ops are emitted in apply order (reverse for grow), so
 * tp0/fp0 are reverse-accumulated from to_size/from_size for grow. Per-op buffer <= OPCAP. */
#define SA_RING 4096
#define SA_MASK (SA_RING-1)
#define SA_OPCAP 2048
typedef struct {
    uint8_t ring[SA_RING]; size_t ototal;
    CScan sc;                                   /* literal-table parity (unchanged) */
    int ph; uint32_t acc; int shift, first;     /* uLEB parser */
    int32_t nops, dl, nl, el, li, ei, prevpos, adj;
    uint16_t lit_pos[SA_OPCAP]; uint8_t lit_byte[SA_OPCAP], opextra[SA_OPCAP];
    int32_t tp, fp, wi, pp; int FWD, err;
    uint8_t *buf; size_t from_size; int32_t to_size;
    const int32_t *presarr; int32_t npre;
    const int32_t *corr_tp; const uint8_t *corr_b; int32_t nc;
} SA;
enum { SP_NOPS, SP_DL, SP_NL, SP_GAP, SP_LITB, SP_EL, SP_EXTRA, SP_ADJ };

static void sa_write(SA*s, int32_t tp, int32_t fp, int isdiff, uint8_t db){
    if(s->pp < s->npre && s->wi == s->presarr[s->pp]){       /* preserve old byte before overwrite */
        if(tp < (int32_t)s->from_size && jr_put((uint32_t)tp, s->buf[tp])) s->err=1;
        s->pp++;
    }
    s->wi++;
    uint8_t src=0;
    if(isdiff && fp>=0 && (size_t)fp<s->from_size){ uint8_t jb; src=jr_get((uint32_t)fp,&jb)?jb:s->buf[fp]; }
    s->buf[tp]=(uint8_t)(db + src + corr_get(s->corr_tp,s->corr_b,s->nc,tp));
}
static void sa_apply_op(SA*s){
    int32_t dl=s->dl, el=s->el, adj=s->adj, tp0, fp0;
    if(s->FWD){ tp0=s->tp; fp0=s->fp; s->tp+=dl+el; s->fp+=dl+adj; }
    else { s->tp-=dl+el; s->fp-=dl+adj; tp0=s->tp; fp0=s->fp; }   /* grow: reverse-accumulate */
    if(s->FWD){
        int32_t li=0;
        for(int32_t k=0;k<dl && !s->err;k++){ uint8_t db=(li<s->nl && s->lit_pos[li]==k)?s->lit_byte[li++]:0;
            sa_write(s,tp0+k,fp0+k,1,db); }
        for(int32_t e=0;e<el && !s->err;e++) sa_write(s,tp0+dl+e,0,0,s->opextra[e]);
    } else {                                                     /* within-op backward (matches _walk BWD) */
        for(int32_t e=el-1;e>=0 && !s->err;e--) sa_write(s,tp0+dl+e,0,0,s->opextra[e]);
        int32_t li=s->nl-1;
        for(int32_t k=dl-1;k>=0 && !s->err;k--){ uint8_t db=(li>=0 && s->lit_pos[li]==k)?s->lit_byte[li--]:0;
            sa_write(s,tp0+k,fp0+k,1,db); }
    }
}
/* feed one finished uLEB value `v` while in a uLEB phase */
static void sa_value(SA*s, uint32_t v){
    switch(s->ph){
    case SP_NOPS: s->nops=(int32_t)v; s->ph = v? SP_DL : SP_NOPS; break;
    case SP_DL:   s->dl=(int32_t)v; if(s->dl>SA_OPCAP){s->err=1;break;} s->ph=SP_NL; break;
    case SP_NL:   s->nl=(int32_t)v; s->li=0; s->prevpos=0; s->ph = s->nl? SP_GAP : SP_EL; break;
    case SP_GAP:  s->prevpos += (int32_t)v; if(s->li){} s->lit_pos[s->li]=(uint16_t)(s->prevpos);
                  if(s->prevpos>=s->dl){s->err=1;break;} s->ph=SP_LITB; break;
    case SP_EL:   s->el=(int32_t)v; if(s->el>SA_OPCAP){s->err=1;break;} s->ei=0; s->ph = s->el? SP_EXTRA : SP_ADJ; break;
    case SP_ADJ:  s->adj=bb_unzz((int32_t)v); sa_apply_op(s);     /* op complete */
                  s->nops--; s->ph = s->nops>0? SP_DL : SP_NOPS; break;
    default: break;
    }
}
static void sa_feed(SA*s, uint8_t b){
    /* raw-byte phases: literal byte after a GAP, and extra bytes */
    if(s->ph==SP_LITB){ s->lit_byte[s->li]=b; s->li++; s->ph = (s->li<s->nl)? SP_GAP : SP_EL; return; }
    if(s->ph==SP_EXTRA){ s->opextra[s->ei]=b; s->ei++; if(s->ei>=s->el) s->ph=SP_ADJ; return; }
    /* uLEB phases */
    if(s->first){ s->acc=b&0x7f; s->shift=7; s->first=0; } else { s->acc|=(uint32_t)(b&0x7f)<<s->shift; s->shift+=7; }
    if(b&0x80) return;
    uint32_t v=s->acc; s->first=1; sa_value(s,v);
}
static void sa_emit(SA*s, uint8_t b){
    s->ring[s->ototal & SA_MASK]=b; s->ototal++;
    cs_adv(&s->sc,b); sa_feed(s,b);
}
static SA SAst;   /* the streaming state (large arrays; one decoder instance) */
static int rc_stream_apply(RDec*r, BitTree*M0, BitTree*M1, uint8_t*buf,
                           size_t from_size, int32_t to_size, int32_t fp_end, int FWD,
                           const int32_t*presarr, int32_t npre,
                           const int32_t*corr_tp, const uint8_t*corr_b, int32_t nc){
    SA*s=&SAst; memset(s,0,sizeof*s);
    s->buf=buf; s->from_size=from_size; s->to_size=to_size; s->FWD=FWD;
    s->presarr=presarr; s->npre=npre; s->corr_tp=corr_tp; s->corr_b=corr_b; s->nc=nc;
    s->tp = FWD?0:to_size; s->fp = FWD?0:fp_end; s->first=1; s->ph=SP_NOPS; cs_init(&s->sc);  /* fp_end != from_size */
    UGolomb tc; ug_init(&tc,'r',11); uint32_t nseq=ug_decode(r,&tc);
    int kd=(int)rd_raw_bits(r,4);
    UGolomb gd; ug_init(&gd,'r',kd); UGolomb gl; ug_init(&gl,'g',0); UGolomb gs; ug_init(&gs,'g',0);
    Flag1 fl; fl_init(&fl);
    for(uint32_t t=0;t<nseq && !s->err;t++){
        if(fl_decode(r,&fl)==0){
            uint32_t ln=ug_decode(r,&gs)+1;
            for(uint32_t i=0;i<ln;i++){ int tag=cs_tag(&s->sc); int b=bt_decode(r, tag?M1:M0); sa_emit(s,(uint8_t)b); }
        } else {
            uint32_t d=ug_decode(r,&gd)+1, ln=ug_decode(r,&gl)+3;
            if(d>s->ototal){ s->err=1; break; }
            for(uint32_t i=0;i<ln;i++){ uint8_t b=s->ring[(s->ototal-d) & SA_MASK]; sa_emit(s,b); }
        }
    }
    return s->err;
}

/* ---------- apply: reconstruct to via own disassembler + control ops (v2) ---------- */
int main(int argc,char**argv){
    /* All heap/handles tracked here and released once at `cleanup` (every exit path
       routes through it), so the decoder is leak-free on success AND on every error.
       This matters for the embedded port, where main() becomes a callable function. */
    int rc=0, err=0, streams_live=0;
    FILE *mf=NULL, *bf=NULL;
    uint8_t *from=NULL, *blob=NULL, *buf=NULL, *corr_b=NULL;
    int32_t *presarr=NULL, *corr_tp=NULL;
    m4_stream_t streams[M4_NSTREAMS];
    RDec rA, rC, rP, rB; BitTree M0, M1;        /* baked: [B] bake | [C] corr | [P] preserve | [A] */

    if(argc<4||argc>5){ fprintf(stderr,"usage: %s <memfile> <blob> <ranges.cfg> [journal_bytes]\n",argv[0]); return 2; }
    if(argc==5){ int v=atoi(argv[4]); if(v<0||v>JMAX_BYTES){ fprintf(stderr,"journal_bytes must be 0..%d\n",JMAX_BYTES); return 2; } Jlimit=v; }
    /* ranges = device-side config (hardcoded on a real device): 7 ints
       data_present data_offset data_begin data_end code_present code_begin code_end */
    uint32_t rdo=0,rdb=0,rde=0,rcb=0,rce=0; int dpres=0,cpres=0;
    { FILE*rf=fopen(argv[3],"r"); if(!rf){perror("ranges");return 2;}
      if(fscanf(rf,"%d %u %u %u %d %u %u",&dpres,&rdo,&rdb,&rde,&cpres,&rcb,&rce)!=7){ fprintf(stderr,"bad ranges.cfg\n"); fclose(rf); return 2; }
      fclose(rf); if(!dpres){ rdo=rdb=rde=0; } if(!cpres){ rcb=rce=0; } }
    mf=fopen(argv[1],"r+b"); if(!mf){perror("mem");return 2;}
    fseek(mf,0,SEEK_END); long fsz=ftell(mf); fseek(mf,0,SEEK_SET);
    if(fsz<0){ fprintf(stderr,"bad memfile\n"); rc=2; goto cleanup; }
    from=malloc(fsz?fsz:1); if(!from){ fprintf(stderr,"oom\n"); rc=1; goto cleanup; }
    if(fread(from,1,fsz,mf)!=(size_t)fsz){ fprintf(stderr,"read fail\n"); rc=2; goto cleanup; }
    size_t from_size=fsz;
    bf=fopen(argv[2],"rb"); if(!bf){perror("blob"); rc=2; goto cleanup;}
    fseek(bf,0,SEEK_END); long bsz=ftell(bf); fseek(bf,0,SEEK_SET);
    if(bsz<0){ fprintf(stderr,"bad blob\n"); rc=2; goto cleanup; }
    blob=malloc(bsz?bsz:1); if(!blob){ fprintf(stderr,"oom\n"); rc=1; goto cleanup; }
    if(fread(blob,1,bsz,bf)!=(size_t)bsz){ fprintf(stderr,"read fail\n"); rc=2; goto cleanup; }
    fclose(bf); bf=NULL;
    if(bsz<12){ fprintf(stderr,"blob too short\n"); rc=1; goto cleanup; }
    /* baked header: CRC32(from)[4] | from_size | to_size | lenB | lenC | lenP (all uLEB) ;
       then [B] | [C] corrections | [P] preserve | [A] control ; trailing 4B = CRC32(to). */
    uint32_t want_crc = (uint32_t)blob[bsz-4]|((uint32_t)blob[bsz-3]<<8)|((uint32_t)blob[bsz-2]<<16)|((uint32_t)blob[bsz-1]<<24);
    size_t body=bsz-4;
    uint32_t crc_from = (uint32_t)blob[0]|((uint32_t)blob[1]<<8)|((uint32_t)blob[2]<<16)|((uint32_t)blob[3]<<24);
    size_t pos=4;
    uint32_t from_size_hdr=(uint32_t)bb_uleb(blob,&pos,body,&err);
    uint32_t to_size_hdr =(uint32_t)bb_uleb(blob,&pos,body,&err);
    uint32_t fp_end_hdr  =(uint32_t)bb_uleb(blob,&pos,body,&err);  /* final from-pointer (reverse-accum) */
    uint32_t lenB=(uint32_t)bb_uleb(blob,&pos,body,&err);
    uint32_t lenC=(uint32_t)bb_uleb(blob,&pos,body,&err);
    uint32_t lenP=(uint32_t)bb_uleb(blob,&pos,body,&err);
    if(err||from_size_hdr!=(uint32_t)from_size||pos+(size_t)lenB+lenC+lenP>body){ fprintf(stderr,"bad header\n"); rc=1; goto cleanup; }
    if(crc32_buf(from,from_size)!=crc_from){ fprintf(stderr,"crc(from) mismatch — wrong base image, refusing\n"); rc=1; goto cleanup; }
    const uint8_t*Bb=blob+pos; const uint8_t*Cb=Bb+lenB; const uint8_t*Pb=Cb+lenC; const uint8_t*Ab=Pb+lenP;
    size_t lenA=body-(pos+lenB+lenC+lenP);
    rd_init(&rA, Ab, lenA);                      /* [A] control range stream (decoded streaming) */
    lit_tree_seed(from,from_size,0,&M0); lit_tree_seed(from,from_size,1,&M1);  /* dual literal bit-trees */
    M1.rate=4;                                  /* lit_t1 (opcode byte) adapts at 1/16 (FIELD_AUDIT.md) */
    int32_t to_size=(int32_t)to_size_hdr;        /* to_size from header; no pass1, no ctrl buffer */
    if(to_size<0){ fprintf(stderr,"bad header (to_size)\n"); rc=1; goto cleanup; }
    int FWD = ((size_t)to_size <= from_size);          /* shrink/self -> forward; grow -> backward */
    size_t span = from_size > (size_t)to_size ? from_size : (size_t)to_size;

    /* preserve events ([P]): write-indices whose old byte the never-evict journal keeps.
       Replaces the former readarr (4x image). Emitted sorted ascending as gaps (UGolomb 'g'). */
    rd_init(&rP, Pb, lenP);
    UGolomb pg; ug_init(&pg,'g',0);
    int32_t npre=(int32_t)ug_decode(&rP,&pg);
    if(npre<0||(size_t)npre>(size_t)to_size+1){ fprintf(stderr,"bad preserve count\n"); rc=1; goto cleanup; }
    presarr=malloc(sizeof(int32_t)*(npre?npre:1));
    if(!presarr){ fprintf(stderr,"oom\n"); rc=1; goto cleanup; }
    { int32_t pv=0; for(int32_t i=0;i<npre;i++){ pv+=(int32_t)ug_decode(&rP,&pg); presarr[i]=pv; } }

    /* corrections ([C]): rare to-ordered additive fixes for baking errors (multi-read /
       exceptions). Sorted ascending; nc is tiny so a binary search per write is cheap. */
    rd_init(&rC, Cb, lenC);
    UGolomb cg; ug_init(&cg,'g',0);
    int32_t nc=(int32_t)ug_decode(&rC,&cg);
    if(nc<0||(size_t)nc>(size_t)to_size+1){ fprintf(stderr,"bad corr count\n"); rc=1; goto cleanup; }
    corr_tp=malloc(sizeof(int32_t)*(nc?nc:1)); corr_b=malloc(nc?nc:1);
    if(!corr_tp||!corr_b){ fprintf(stderr,"oom\n"); rc=1; goto cleanup; }
    { int32_t cv=0; for(int32_t i=0;i<nc;i++){ cv+=(int32_t)ug_decode(&rC,&cg); corr_tp[i]=cv; corr_b[i]=(uint8_t)rd_raw_bits(&rC,8); } }

    /* ---- BAKE: disassemble from, write pack(fval-delta) into buf at each captured field ---- */
    buf = malloc(span ? span : 1);
    if(!buf){ fprintf(stderr,"oom\n"); rc=1; goto cleanup; }
    memcpy(buf, from, from_size);
    if(span>from_size) memset(buf+from_size, 0, span-from_size);
    if(detools_m4_disassemble(from,from_size,rdo,rdb,rde,rcb,rce,streams)){ fprintf(stderr,"disasm oom\n"); rc=1; goto cleanup; }
    streams_live=1;
    rd_init(&rB, Bb, lenB);
    if(rc_bake(&rB,streams,buf,from_size)){ fprintf(stderr,"bad [B]\n"); rc=1; goto cleanup; }
    detools_m4_free_streams(streams); streams_live=0;

    /* STREAMING [A] apply: decode op-by-op (2 KB ring), apply each immediately. No ctrl, no
       per-op arrays. ops emitted in apply order (reverse for grow); tp0/fp0 reverse-accumulated. */
    jr_reset();
    if(rc_stream_apply(&rA,&M0,&M1,buf,from_size,to_size,(int32_t)fp_end_hdr,FWD,presarr,npre,corr_tp,corr_b,nc)){
        fprintf(stderr,"journal overflow (>%d B) or bad [A]/stream\n",Jlimit); rc=1; goto cleanup; }

    /* integrity: reject before touching flash if reconstruction != intended to-image */
    if(crc32_buf(buf,to_size)!=want_crc){ fprintf(stderr,"crc mismatch — corrupt patch rejected\n"); rc=1; goto cleanup; }
    fseek(mf,0,SEEK_SET); if(fwrite(buf,1,to_size,mf)!=(size_t)to_size){ fprintf(stderr,"write fail\n"); rc=1; goto cleanup; }
    fflush(mf);
    if((long)to_size<fsz){ if(ftruncate(fileno(mf),to_size)){} }
    fprintf(stderr,"ok to_size=%ld dir=%s journal_used=%d B (budget=%d)\n",
            (long)to_size, FWD?"fwd":"bwd", Jpeak, Jlimit);
    rc=0;

cleanup:
    if(streams_live) detools_m4_free_streams(streams);
    if(mf) fclose(mf);
    if(bf) fclose(bf);
    free(from); free(blob);
    free(presarr); free(corr_tp); free(corr_b); free(buf);
    return rc;
}
