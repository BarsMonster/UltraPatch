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

typedef struct { const uint8_t *d; size_t n, i; } PullCtx;
static int pull_next(void *c, uint8_t *out){
    PullCtx *p=(PullCtx*)c;
    if(p->i>=p->n) return 0;
    *out=p->d[p->i++]; return 1;
}

typedef struct {
    PatchApply pa;
    size_t consumed;
    int rc;
} HostApply;

static int host_apply_blob(const uint8_t *blob, size_t blob_n,
                           const uint8_t *from, uint32_t from_n,
                           uint32_t span, uint32_t pad_seed,
                           HostApply *out)
{
    if (nvm_init(from, from_n, span, pad_seed)) return -1;
    PullCtx pc = { blob, blob_n, 0 };
    out->rc = patch_apply_run(&out->pa, pull_next, &pc);
    out->consumed = pc.i;
    return 0;
}

static int nvm_safety_ok(void){
    return nvm_rows_amplified()==0 && nvm_max_row_erases()<=1 && nvm_frontier_inversions()==0;
}

const char *a1_selfcheck(const uint8_t *blob, size_t blob_n,
                         const uint8_t *from, size_t from_n,
                         const uint8_t *to, size_t to_n)
{
    uint32_t span = from_n > to_n ? (uint32_t)from_n : (uint32_t)to_n;
    uint32_t pad_seed = 0;
    for (int k = 0; k < 8; k++) pad_seed = pad_seed * 1664525u + 1013904223u + blob[k];
    HostApply ha;
    if (host_apply_blob(blob, blob_n, from, (uint32_t)from_n, span, pad_seed, &ha)) return "selfcheck out of memory";
    if (ha.rc != PATCH_APPLY_DONE)
        return patch_apply_reject(&ha.pa) == REJ_RESOURCE ? "reference decoder rejected the patch (resource cap)"
                                                          : "reference decoder rejected the patch";
    if (ha.consumed != blob_n) return "decoder did not consume the whole blob";
    if (patch_apply_from_size(&ha.pa) != (uint32_t)from_n || patch_apply_to_size(&ha.pa) != (uint32_t)to_n) return "header size mismatch";
    if (to_n && memcmp(sc_flash, to, to_n) != 0) return "decoded image differs from target";
    if (!nvm_safety_ok()) return nvm_rows_amplified() ? "NVM row erased more than once (write amplification)"
                                                       : "NVM erase frontier inversion";
    return NULL;
}

int decode_a1(const char *image_path, const char *patch_path){
    FILE*bf=fopen(patch_path,"rb"); if(!bf){perror("patch");return 2;}
    fseek(bf,0,SEEK_END); long bsz=ftell(bf); fseek(bf,0,SEEK_SET);
    if(bsz<12){ fprintf(stderr,"blob too short\n"); fclose(bf); return 1; }
    uint8_t*blob=malloc((size_t)bsz);
    if(!blob){ fclose(bf); return 2; }
    if(fread(blob,1,(size_t)bsz,bf)!=(size_t)bsz){ fclose(bf); free(blob); return 2; } fclose(bf);

    FILE*mf=fopen(image_path,"r+b"); if(!mf){perror("image");free(blob);return 2;}
    fseek(mf,0,SEEK_END); long fsz=ftell(mf); fseek(mf,0,SEEK_SET);
    HostApply ha;
    { uint32_t span = (uint32_t)fsz>A1_MAX_IMAGE ? (uint32_t)fsz : A1_MAX_IMAGE;
      uint8_t*tmp=(uint8_t*)malloc(fsz?(size_t)fsz:1);
      if(!tmp){ fclose(mf); free(blob); return 2; }
      if(fread(tmp,1,(size_t)fsz,mf)!=(size_t)fsz){ fclose(mf); free(tmp); free(blob); return 2; }
      if(host_apply_blob(blob,(size_t)bsz,tmp,(uint32_t)fsz,span,(uint32_t)fsz,&ha)){
          fclose(mf); free(tmp); free(blob); return 2; }
      free(tmp); }

    if(ha.rc==PATCH_APPLY_ERROR){ int rj=patch_apply_reject(&ha.pa); if(!rj) rj=REJ_CORRUPT;
        fprintf(stderr,"decode error - rejected (reason=%d: %s)\n", rj,
                rj==REJ_RESOURCE?"resource cap exceeded - firmware larger than build sizing":"corrupt/truncated patch");
        fclose(mf); free(sc_flash); free(blob); return 1; }
    uint32_t to_size=patch_apply_to_size(&ha.pa), span=patch_apply_image_span(&ha.pa);
    if(!nvm_safety_ok()){
        fprintf(stderr,"NVM safety gate FAILED: amplified=%u maxrowerase=%u inversions=%ld\n",
                nvm_rows_amplified(),nvm_max_row_erases(),nvm_frontier_inversions());
        fclose(mf); free(sc_flash); free(blob); return 1;
    }
    fseek(mf,0,SEEK_SET);
    if(fwrite(sc_flash,1,to_size,mf)!=to_size){ fclose(mf); free(sc_flash); free(blob); return 1; }
    fflush(mf);
    if((long)to_size<fsz){ if(ftruncate(fileno(mf),to_size)){} }
    fprintf(stderr,"ok to_size=%u dir=%s journal_used=%u slots (cap=%u)\n",to_size,patch_apply_forward(&ha.pa)?"fwd":"bwd",(unsigned)patch_apply_journal_used(&ha.pa),(unsigned)JSLOTS);
    fprintf(stderr,"NVM: erases=%ld rows=%u programs=%ld amplified=%u maxrowerase=%u inversions=%ld (span=%u rows_total=%u, ideal=span/256)\n",
            nvm_erases(),nvm_rows(),nvm_programs(),nvm_rows_amplified(),nvm_max_row_erases(),nvm_frontier_inversions(),span,(span+255)/256);
    fclose(mf); free(blob);
    return 0;
}

#ifdef PATCH_APPLY_DEMO_MAIN
static void decode_usage(const char *prog){
    fprintf(stderr, "usage: %s --decode <image> <patch>\n", prog);
}

int main(int argc,char**argv){
    if(argc==2 && (strcmp(argv[1],"-h")==0 || strcmp(argv[1],"--help")==0)){ decode_usage(argv[0]); return 0; }
    if(argc==4 && strcmp(argv[1],"--decode")==0) return decode_a1(argv[2],argv[3]);
    decode_usage(argv[0]);
    return 2;
}
#endif
