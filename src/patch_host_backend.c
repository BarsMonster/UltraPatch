/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 *
 * Shared host decoder backend for the CLI decode path and encoder selfcheck.
 * The device artifact remains src/patch_apply.h; this file exists only so the
 * host tool links one reference-decoder copy instead of one per harness.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nvm_emu.inc"

static long nvm_erases(void){ return sc_erases; }
static long nvm_programs(void){ return sc_programs; }
static uint32_t nvm_rows(void){ uint32_t n=0; for(uint32_t i=0;i<sc_nrows;i++) n+=(sc_erasecnt[i]>0); return n; }
static uint32_t nvm_rows_amplified(void){ uint32_t n=0; for(uint32_t i=0;i<sc_nrows;i++) n+=(sc_erasecnt[i]>1); return n; }
static uint32_t nvm_max_row_erases(void){ uint32_t m=0; for(uint32_t i=0;i<sc_nrows;i++) if(sc_erasecnt[i]>m) m=sc_erasecnt[i]; return m; }
static long nvm_frontier_inversions(void){ return sc_finv; }

#include "patch_apply.h"
#include "patch_apply_push_adapter.h"

typedef struct { const uint8_t *d; size_t n, i; } PullCtx;
static int pull_next(void *c, uint8_t *out){
    PullCtx *p=(PullCtx*)c;
    if(p->i>=p->n) return 0;
    *out=p->d[p->i++]; return 1;
}

typedef struct { const uint8_t *d; size_t n, i; PatchRing *r; long feeds; int eof_signaled; } FeedCtx;
static void feed_one(void *c){
    FeedCtx *f=(FeedCtx*)c;
    if(f->i < f->n){ if(patch_ring_push(f->r, f->d[f->i])){ f->i++; f->feeds++; } }
    else { f->eof_signaled=1; patch_ring_eof(f->r); }
}

const char *a1_selfcheck(const uint8_t *blob, size_t blob_n,
                         const uint8_t *from, size_t from_n,
                         const uint8_t *to, size_t to_n)
{
    uint32_t span = from_n > to_n ? (uint32_t)from_n : (uint32_t)to_n;
    uint32_t pad_seed = 0;
    for (int k = 0; k < 8; k++) pad_seed = pad_seed * 1664525u + 1013904223u + blob[k];
    if (nvm_init(from, (uint32_t)from_n, span, pad_seed)) return "selfcheck out of memory";

    PullCtx pc = { blob, blob_n, 0 };
    PatchApply pa;
    int rc = patch_apply_run(&pa, pull_next, &pc);
    if (rc != PATCH_APPLY_DONE)
        return patch_apply_reject(&pa) == REJ_RESOURCE ? "reference decoder rejected the patch (resource cap)"
                                                       : "reference decoder rejected the patch";
    if (pc.i != blob_n) return "decoder did not consume the whole blob";
    if (patch_apply_from_size(&pa) != (uint32_t)from_n || patch_apply_to_size(&pa) != (uint32_t)to_n) return "header size mismatch";
    if (to_n && memcmp(sc_flash, to, to_n) != 0) return "decoded image differs from target";
    for (uint32_t r = 0; r < sc_nrows; r++)
        if (sc_erasecnt[r] > 1) return "NVM row erased more than once (write amplification)";
    if (sc_finv) return "NVM erase frontier inversion";
    return NULL;
}

int decode_a1(const char *image_path, const char *patch_path, int byte_mode){
    FILE*bf=fopen(patch_path,"rb"); if(!bf){perror("patch");return 2;}
    fseek(bf,0,SEEK_END); long bsz=ftell(bf); fseek(bf,0,SEEK_SET);
    if(bsz<12){ fprintf(stderr,"blob too short\n"); fclose(bf); return 1; }
    uint8_t*blob=malloc((size_t)bsz);
    if(!blob){ fclose(bf); return 2; }
    if(fread(blob,1,(size_t)bsz,bf)!=(size_t)bsz){ fclose(bf); free(blob); return 2; } fclose(bf);

    FILE*mf=fopen(image_path,"r+b"); if(!mf){perror("image");free(blob);return 2;}
    fseek(mf,0,SEEK_END); long fsz=ftell(mf); fseek(mf,0,SEEK_SET);
    { uint32_t span = (uint32_t)fsz>A1_MAX_IMAGE ? (uint32_t)fsz : A1_MAX_IMAGE;
      uint8_t*tmp=(uint8_t*)malloc(fsz?(size_t)fsz:1);
      if(!tmp){ fclose(mf); free(blob); return 2; }
      if(fread(tmp,1,(size_t)fsz,mf)!=(size_t)fsz){ fclose(mf); free(tmp); free(blob); return 2; }
      nvm_init(tmp,(uint32_t)fsz,span,(uint32_t)fsz); free(tmp); }

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
