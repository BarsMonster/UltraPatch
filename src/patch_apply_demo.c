/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 */

/* Host demo/gate wrapper for the header-only patch_apply decoder.
 * The reusable device artifact is patch_apply.h; this file owns only file I/O,
 * plaintext patch header/trailer parsing, and the host NVM safety checks. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* SAML22-shaped NVM emulator for the host demo/gate. The reusable decoder only
 * needs flash_read() and flash_write(); all emulation and safety counters live
 * here, outside the production header artifact. */
#ifndef NVM_PAGE
#define NVM_PAGE 64u
#endif
#ifndef NVM_ROW
#define NVM_ROW 256u
#endif
static uint8_t *g_flash, *g_erasecnt;
static uint32_t g_flash_n, g_nrows;
static long g_erases, g_programs, g_finv;
static uint32_t g_last_erow;
static int g_edir, g_ecount;

static void nvm_init(const uint8_t *from, uint32_t from_size, uint32_t span){
    g_flash_n = span;
    free(g_flash); g_flash = (uint8_t*)malloc(span?span:1);
    memcpy(g_flash, from, from_size);
    if(span>from_size) memset(g_flash+from_size, 0xFF, span-from_size);
    g_erases = g_programs = 0; g_nrows = (span+NVM_ROW-1)/NVM_ROW;
    g_finv = 0; g_last_erow = 0; g_edir = 0; g_ecount = 0;
    free(g_erasecnt); g_erasecnt = (uint8_t*)calloc(g_nrows?g_nrows:1, 1);
}
uint8_t flash_read(uint32_t a){ return a<g_flash_n ? g_flash[a] : 0xFF; }
void flash_write(uint32_t a, uint8_t v){
    if(a>=g_flash_n) return;
    uint8_t cur = g_flash[a];
    if(cur == v) return;
    if((uint8_t)(cur & v) != v){
        uint32_t row = (a/NVM_ROW)*NVM_ROW, end = row+NVM_ROW; if(end>g_flash_n) end=g_flash_n;
        memset(g_flash+row, 0xFF, end-row);
        g_erases++; if(g_erasecnt[a/NVM_ROW]<255) g_erasecnt[a/NVM_ROW]++;
        uint32_t er = a/NVM_ROW;
        if(g_ecount){ int d = (er>g_last_erow) ? 1 : (er<g_last_erow ? -1 : 0);
            if(d){ if(g_edir==0) g_edir=d; else if(d!=g_edir) g_finv++; } }
        g_last_erow = er; g_ecount++;
    }
    g_flash[a] = v; g_programs++;
}
static long nvm_erases(void){ return g_erases; }
static long nvm_programs(void){ return g_programs; }
static uint32_t nvm_rows(void){ uint32_t n=0; for(uint32_t i=0;i<g_nrows;i++) n+=(g_erasecnt[i]>0); return n; }
static uint32_t nvm_rows_amplified(void){ uint32_t n=0; for(uint32_t i=0;i<g_nrows;i++) n+=(g_erasecnt[i]>1); return n; }
static uint32_t nvm_max_row_erases(void){ uint32_t m=0; for(uint32_t i=0;i<g_nrows;i++) if(g_erasecnt[i]>m) m=g_erasecnt[i]; return m; }
static long nvm_frontier_inversions(void){ return g_finv; }

#include "patch_apply.h"
#include "patch_apply_push_adapter.h"

/* pull-mode byte source: serve the blob buffer to patch_apply_run() */
typedef struct { const uint8_t *d; size_t n, i; } PullCtx;
static int pull_next(void *c, uint8_t *out){
    PullCtx *p=(PullCtx*)c;
    if(p->i>=p->n) return 0;
    *out=p->d[p->i++]; return 1;
}

/* byte_mode: exercise the optional push adapter with a tiny ring. The wait hook plays the
 * event-driven producer, feeding ONE blob byte per invocation (then EOF) — proving the
 * decoder streams byte-by-byte through the adapter with an 8-byte buffer. */
typedef struct { const uint8_t *d; size_t n, i; PatchRing *r; long feeds; } FeedCtx;
static void feed_one(void *c){
    FeedCtx *f=(FeedCtx*)c;
    if(f->i < f->n){ if(patch_ring_push(f->r, f->d[f->i])){ f->i++; f->feeds++; } }
    else patch_ring_eof(f->r);
}

int main(int argc,char**argv){
    if(argc<3||argc>4){ fprintf(stderr,"usage: %s <memfile> <blob> [byte_mode]\n",argv[0]); return 2; }
    int byte_mode = (argc==4);   /* arg3 present -> stream via the push adapter, 1 byte per feed */
    FILE*bf=fopen(argv[2],"rb"); if(!bf){perror("blob");return 2;}
    fseek(bf,0,SEEK_END); long bsz=ftell(bf); fseek(bf,0,SEEK_SET);
    if(bsz<12){ fprintf(stderr,"blob too short\n"); fclose(bf); return 1; }
    uint8_t*blob=malloc((size_t)bsz); if(fread(blob,1,(size_t)bsz,bf)!=(size_t)bsz){ fclose(bf); return 2; } fclose(bf);
    /* MINIMAL envelope pre-parse, for the HOST EMULATOR ONLY (flash sizing + memfile sanity).
     * The decoder performs the authoritative envelope parse and both CRC gates itself; a device
     * integration needs none of this — its flash is fixed hardware.
     * Envelope: CRC32(from)[4] | from_size | zz(to_size-from_size)
     * | [zz(fp_end-from_size) iff to_size>from_size] | range body | CRC32(to)[4]. */
    size_t p=4; int err=(size_t)bsz<12;
    uint32_t from_size=0,to_size=0; { uint32_t v=0; int sh=0; uint8_t b;
        do{ if(p>=(size_t)bsz||sh>28){err=1;break;} b=blob[p++]; v|=(uint32_t)(b&0x7f)<<sh; sh+=7; }while(b&0x80); from_size=v; }
    { uint32_t z=0; int sh=0; uint8_t b; do{ if(err||p>=(size_t)bsz||sh>28){err=1;break;} b=blob[p++]; z|=(uint32_t)(b&0x7f)<<sh; sh+=7; }while(b&0x80);
      if(err){} else if(z&1u){ uint32_t m=(z>>1)+1u; if(m>from_size){err=1;} else to_size=from_size-m; }
      else    { uint32_t d=z>>1;     if(d>(64u<<20) || from_size>(64u<<20)-d){err=1;} else to_size=from_size+d; } }
    if(err){ fprintf(stderr,"bad header\n"); free(blob); return 1; }
    if(from_size>(64u<<20)){ fprintf(stderr,"implausible size - rejected\n"); free(blob); return 1; }

    FILE*mf=fopen(argv[1],"r+b"); if(!mf){perror("mem");free(blob);return 2;}
    fseek(mf,0,SEEK_END); long fsz=ftell(mf); fseek(mf,0,SEEK_SET);
    if((uint32_t)fsz!=from_size){ fprintf(stderr,"from_size mismatch (%ld vs %u)\n",fsz,from_size); fclose(mf); free(blob); return 1; }
    { uint32_t span = from_size>to_size? from_size:to_size;
      uint8_t*tmp=(uint8_t*)malloc(from_size?from_size:1);
      if(fread(tmp,1,from_size,mf)!=from_size){ fclose(mf); free(tmp); free(blob); return 2; }
      nvm_init(tmp,from_size,span); free(tmp); }

    /* the decoder drives; the callback serves blob bytes. byte_mode routes through the
     * push adapter (tiny ring, one byte fed per wait) as the streaming proof. */
    int rc;
    if(byte_mode){
        uint8_t rbuf[8]; PatchRing ring; FeedCtx fc={blob,(size_t)bsz,0,&ring,0};
        patch_ring_init(&ring, rbuf, sizeof rbuf, feed_one, &fc);
        rc=patch_apply_run(patch_ring_next,&ring);
        if(rc==PATCH_APPLY_DONE){
            if(fc.feeds!=(long)bsz){
                fprintf(stderr,"adapter streaming check FAILED: fed %ld of %ld bytes\n",fc.feeds,bsz);
                fclose(mf); free(g_flash); free(blob); return 1;
            }
            fprintf(stderr,"adapter streaming OK: %ld single-byte feeds over %ld blob bytes\n",fc.feeds,bsz);
        }
    } else {
        PullCtx pc={blob,(size_t)bsz,0}; rc=patch_apply_run(pull_next,&pc);
    }
    if(rc==PATCH_APPLY_ERROR){ uint8_t rj=g_reject?g_reject:REJ_CORRUPT;
        fprintf(stderr,"decode error - rejected (reason=%u: %s)\n", rj,
                rj==REJ_RESOURCE?"resource cap exceeded - firmware larger than build sizing":"corrupt/truncated patch");
        fclose(mf); free(g_flash); free(blob); return 1; }
#ifdef RC_V3_BAKEDUMP
    { const char*dp=getenv("AGENT07_OUTDUMP"); if(dp){ FILE*f=fopen(dp,"wb"); fwrite(g_flash,1,to_size,f); fclose(f); } }
#endif
    if(nvm_rows_amplified()!=0 || nvm_max_row_erases()>1 || nvm_frontier_inversions()!=0){
        fprintf(stderr,"NVM safety gate FAILED: amplified=%u maxrowerase=%u inversions=%ld\n",
                nvm_rows_amplified(),nvm_max_row_erases(),nvm_frontier_inversions());
        fclose(mf); free(g_flash); free(blob); return 1;
    }
    fseek(mf,0,SEEK_SET);
    if(fwrite(g_flash,1,to_size,mf)!=to_size){ fclose(mf); free(g_flash); free(blob); return 1; }
    fflush(mf);
    if((long)to_size<fsz){ if(ftruncate(fileno(mf),to_size)){} }
    fprintf(stderr,"ok to_size=%u dir=%s journal_used=%u slots (cap=%u)\n",to_size,g_FWD?"fwd":"bwd",(unsigned)g_jpage[JPAGE_MAX],(unsigned)JSLOTS);
    fprintf(stderr,"NVM: erases=%ld rows=%u programs=%ld amplified=%u maxrowerase=%u inversions=%ld (span=%u rows_total=%u, ideal=span/256)\n",
            nvm_erases(),nvm_rows(),nvm_programs(),nvm_rows_amplified(),nvm_max_row_erases(),nvm_frontier_inversions(),g_flash_n,(g_flash_n+255)/256);
    fclose(mf); free(blob);
    return 0;
}
