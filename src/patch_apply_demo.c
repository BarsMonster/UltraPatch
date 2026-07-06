/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 */

/* Host decode mode for the ultrapatch CLI.
 * The reusable device artifact is patch_apply.h; this file owns only file I/O
 * (max-span flash sizing) and the NVM safety checks -- the DECODER performs the
 * authoritative envelope parse, size caps and both CRC gates itself. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* SAML22-shaped NVM emulator, shared with patch_selfcheck.c (the ship/no-ship oracle)
 * through nvm_emu.inc; it must be #included BEFORE patch_apply.h. On top of the shared
 * state this file keeps the extra reporting accessors the gate output line needs. */
#include "nvm_emu.inc"
static long nvm_erases(void){ return sc_erases; }
static long nvm_programs(void){ return sc_programs; }
static uint32_t nvm_rows(void){ uint32_t n=0; for(uint32_t i=0;i<sc_nrows;i++) n+=(sc_erasecnt[i]>0); return n; }
static uint32_t nvm_rows_amplified(void){ uint32_t n=0; for(uint32_t i=0;i<sc_nrows;i++) n+=(sc_erasecnt[i]>1); return n; }
static uint32_t nvm_max_row_erases(void){ uint32_t m=0; for(uint32_t i=0;i<sc_nrows;i++) if(sc_erasecnt[i]>m) m=sc_erasecnt[i]; return m; }
static long nvm_frontier_inversions(void){ return sc_finv; }

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
 * event-driven producer, feeding ONE blob byte per invocation — proving the decoder streams
 * byte-by-byte through the adapter with an 8-byte buffer and completes without transport EOF. */
typedef struct { const uint8_t *d; size_t n, i; PatchRing *r; long feeds; int eof_signaled; } FeedCtx;
static void feed_one(void *c){
    FeedCtx *f=(FeedCtx*)c;
    if(f->i < f->n){ if(patch_ring_push(f->r, f->d[f->i])){ f->i++; f->feeds++; } }
    else { f->eof_signaled=1; patch_ring_eof(f->r); }
}

int decode_a1(const char *image_path, const char *patch_path, int byte_mode){
    FILE*bf=fopen(patch_path,"rb"); if(!bf){perror("patch");return 2;}
    fseek(bf,0,SEEK_END); long bsz=ftell(bf); fseek(bf,0,SEEK_SET);
    if(bsz<12){ fprintf(stderr,"blob too short\n"); fclose(bf); return 1; }
    uint8_t*blob=malloc((size_t)bsz);
    if(!blob){ fclose(bf); return 2; }
    if(fread(blob,1,(size_t)bsz,bf)!=(size_t)bsz){ fclose(bf); free(blob); return 2; } fclose(bf);
    /* Size the emulated flash to the max image span and load the CURRENT image; the DECODER
     * owns the envelope parse, size caps and CRC32(from) gate, so the host does NO header
     * pre-parse (a device integration needs none of this — its flash is fixed hardware). One
     * 64 MiB memset+pad-fill per process (~15 ms), negligible even x256 in the corpus leg. */
    FILE*mf=fopen(image_path,"r+b"); if(!mf){perror("image");free(blob);return 2;}
    fseek(mf,0,SEEK_END); long fsz=ftell(mf); fseek(mf,0,SEEK_SET);
    { uint32_t span = (uint32_t)fsz>A1_MAX_IMAGE ? (uint32_t)fsz : A1_MAX_IMAGE;
      uint8_t*tmp=(uint8_t*)malloc(fsz?(size_t)fsz:1);
      if(!tmp){ fclose(mf); free(blob); return 2; }
      if(fread(tmp,1,(size_t)fsz,mf)!=(size_t)fsz){ fclose(mf); free(tmp); free(blob); return 2; }
      nvm_init(tmp,(uint32_t)fsz,span,(uint32_t)fsz); free(tmp); }

    /* the decoder drives; the callback serves blob bytes. byte_mode routes through the
     * push adapter (tiny ring, one byte fed per wait) as the streaming proof. */
    PatchApply pa;
    int rc;
    if(byte_mode){
        uint8_t rbuf[8]; PatchRing ring; FeedCtx fc={blob,(size_t)bsz,0,&ring,0,0};
        if(!patch_ring_init(&ring, rbuf, sizeof rbuf, feed_one, &fc)){
            fprintf(stderr,"patch ring init failed\n");
            fclose(mf); free(sc_flash); free(blob); return 2;
        }
        rc=patch_apply_run(&pa, patch_ring_next,&ring);
        if(rc==PATCH_APPLY_DONE){
            if(fc.feeds!=(long)bsz || fc.eof_signaled){
                fprintf(stderr,"adapter streaming check FAILED: fed %ld of %ld bytes, eof=%d\n",fc.feeds,bsz,fc.eof_signaled);
                fclose(mf); free(sc_flash); free(blob); return 1;
            }
            fprintf(stderr,"adapter streaming OK: %ld single-byte feeds over %ld blob bytes\n",fc.feeds,bsz);
        }
    } else {
        PullCtx pc={blob,(size_t)bsz,0}; rc=patch_apply_run(&pa, pull_next,&pc);
    }
    if(rc==PATCH_APPLY_ERROR){ int rj=patch_apply_reject(&pa); if(!rj) rj=REJ_CORRUPT;
        fprintf(stderr,"decode error - rejected (reason=%d: %s)\n", rj,
                rj==REJ_RESOURCE?"resource cap exceeded - firmware larger than build sizing":"corrupt/truncated patch");
        fclose(mf); free(sc_flash); free(blob); return 1; }
    /* decode succeeded: the decoder owns the true sizes (envelope-parsed + CRC-gated). */
    uint32_t to_size=patch_apply_to_size(&pa), span=patch_apply_image_span(&pa);
    if(nvm_rows_amplified()!=0 || nvm_max_row_erases()>1 || nvm_frontier_inversions()!=0){
        fprintf(stderr,"NVM safety gate FAILED: amplified=%u maxrowerase=%u inversions=%ld\n",
                nvm_rows_amplified(),nvm_max_row_erases(),nvm_frontier_inversions());
        fclose(mf); free(sc_flash); free(blob); return 1;
    }
    fseek(mf,0,SEEK_SET);
    if(fwrite(sc_flash,1,to_size,mf)!=to_size){ fclose(mf); free(sc_flash); free(blob); return 1; }
    fflush(mf);
    if((long)to_size<fsz){ if(ftruncate(fileno(mf),to_size)){} }
    fprintf(stderr,"ok to_size=%u dir=%s journal_used=%u slots (cap=%u)\n",to_size,patch_apply_forward(&pa)?"fwd":"bwd",(unsigned)patch_apply_journal_used(&pa),(unsigned)JSLOTS);
    fprintf(stderr,"NVM: erases=%ld rows=%u programs=%ld amplified=%u maxrowerase=%u inversions=%ld (span=%u rows_total=%u, ideal=span/256)\n",
            nvm_erases(),nvm_rows(),nvm_programs(),nvm_rows_amplified(),nvm_max_row_erases(),nvm_frontier_inversions(),span,(span+255)/256);
    fclose(mf); free(blob);
    return 0;
}

#ifdef PATCH_APPLY_DEMO_MAIN
static void decode_usage(const char *prog){
    fprintf(stderr, "usage: %s --decode [--byte-mode] <image> <patch>\n", prog);
}

int main(int argc,char**argv){
    int byte_mode=0;
    const char *pos[2]={0};
    int npos=0;
    for(int i=1;i<argc;i++){
        if(strcmp(argv[i],"--decode")==0){
            continue;
        } else if(strcmp(argv[i],"--byte-mode")==0){
            byte_mode=1;
        } else if(strcmp(argv[i],"-h")==0 || strcmp(argv[i],"--help")==0){
            decode_usage(argv[0]); return 0;
        } else if(argv[i][0]=='-' && argv[i][1]){
            decode_usage(argv[0]); return 2;
        } else {
            if(npos==2){ decode_usage(argv[0]); return 2; }
            pos[npos++]=argv[i];
        }
    }
    if(npos!=2){ decode_usage(argv[0]); return 2; }
    return decode_a1(pos[0],pos[1],byte_mode);
}
#endif
