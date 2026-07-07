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
#include <sys/types.h>
#include <unistd.h>

#include "nvm_emu.inc"

typedef struct { uint8_t *d; size_t n; } HostFile;

static void host_file_free(HostFile *b){
    free(b->d); b->d = NULL; b->n = 0;
}

static int host_read_file(const char *path, HostFile *out){
    FILE *f = fopen(path, "rb");
    if(!f){ perror(path); return 2; }
    if(fseek(f,0,SEEK_END)){ perror(path); if(fclose(f)) perror(path); return 2; }
    long sz = ftell(f);
    if(sz < 0){ perror(path); if(fclose(f)) perror(path); return 2; }
    if(fseek(f,0,SEEK_SET)){ perror(path); if(fclose(f)) perror(path); return 2; }
    out->d = (uint8_t*)malloc(sz ? (size_t)sz : 1u);
    if(!out->d){ if(fclose(f)) perror(path); return 2; }
    out->n = (size_t)sz;
    if(out->n && fread(out->d,1,out->n,f)!=out->n){ perror(path); if(fclose(f)) perror(path); host_file_free(out); return 2; }
    if(fclose(f)){ perror(path); host_file_free(out); return 2; }
    return 0;
}

static int host_close_file(FILE **fp, const char *path){
    if(!*fp) return 0;
    FILE *f = *fp;
    *fp = NULL;
    if(fclose(f)){ perror(path); return 2; }
    return 0;
}

static int host_commit_image(FILE *mf, const char *image_path, const uint8_t *data,
                             uint32_t to_size, size_t old_size){
    if(fseek(mf,0,SEEK_SET)){ perror(image_path); return 2; }
    if(to_size && fwrite(data,1,to_size,mf)!=(size_t)to_size){ perror(image_path); return 2; }
    if(fflush(mf)){ perror(image_path); return 2; }
    if((size_t)to_size<old_size){
        int fd = fileno(mf);
        if(fd < 0){ perror(image_path); return 2; }
        if(ftruncate(fd,(off_t)to_size)){ perror(image_path); return 2; }
    }
    return 0;
}

typedef struct {
    long erases, programs, inversions;
    uint32_t rows, amplified, max_row_erases;
} NvmStats;

static NvmStats nvm_stats(void){
    NvmStats st = { sc_erases, sc_programs, sc_finv, 0, 0, 0 };
    for(uint32_t i=0;i<sc_nrows;i++){
        uint8_t e = sc_erasecnt[i];
        st.rows += e>0;
        st.amplified += e>1;
        if(e>st.max_row_erases) st.max_row_erases=e;
    }
    return st;
}

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

static int nvm_safety_ok(const NvmStats *st){
    return st->amplified==0 && st->max_row_erases<=1 && st->inversions==0;
}

enum {
    HOST_VERIFY_OK = 0,
    HOST_VERIFY_REJECTED,
    HOST_VERIFY_TRAILING,
    HOST_VERIFY_FROM_SIZE,
    HOST_VERIFY_TO_SIZE,
    HOST_VERIFY_NVM
};

static int host_verify_apply(const HostApply *ha, size_t blob_n,
                             uint32_t from_n, uint32_t to_n, int check_to,
                             int *reject_out)
{
    if (ha->rc != PATCH_APPLY_DONE) {
        int rj = patch_apply_reject(&ha->pa);
        if (!rj) rj = REJ_CORRUPT;
        if (reject_out) *reject_out = rj;
        return HOST_VERIFY_REJECTED;
    }
    if (ha->consumed != blob_n) return HOST_VERIFY_TRAILING;
    if (patch_apply_from_size(&ha->pa) != from_n) return HOST_VERIFY_FROM_SIZE;
    if (check_to && patch_apply_to_size(&ha->pa) != to_n) return HOST_VERIFY_TO_SIZE;
    return HOST_VERIFY_OK;
}

static int host_verify_nvm(NvmStats *st_out)
{
    NvmStats st = nvm_stats();
    if (st_out) *st_out = st;
    return nvm_safety_ok(&st) ? HOST_VERIFY_OK : HOST_VERIFY_NVM;
}

const char *a1_selfcheck(const uint8_t *blob, size_t blob_n,
                         const uint8_t *from, size_t from_n,
                         const uint8_t *to, size_t to_n)
{
    if (blob_n < 8) return "selfcheck blob too short";
    uint32_t span = from_n > to_n ? (uint32_t)from_n : (uint32_t)to_n;
    uint32_t pad_seed = 0;
    for (int k = 0; k < 8; k++) pad_seed = pad_seed * 1664525u + 1013904223u + blob[k];
    HostApply ha;
    const char *err = NULL;
    if (host_apply_blob(blob, blob_n, from, (uint32_t)from_n, span, pad_seed, &ha)) return "selfcheck out of memory";
    { NvmStats st;
      int reject = REJ_NONE;
      switch (host_verify_apply(&ha, blob_n, (uint32_t)from_n, (uint32_t)to_n, 1, &reject)) {
      case HOST_VERIFY_OK:
          if (to_n && memcmp(sc_flash, to, to_n) != 0) err = "decoded image differs from target";
          else if (host_verify_nvm(&st) == HOST_VERIFY_NVM)
              err = st.amplified ? "NVM row erased more than once (write amplification)"
                                 : "NVM erase frontier inversion";
          break;
      case HOST_VERIFY_REJECTED:
          err = reject == REJ_RESOURCE ? "reference decoder rejected the patch (resource cap)"
                                       : "reference decoder rejected the patch";
          break;
      case HOST_VERIFY_TRAILING:
          err = "decoder did not consume the whole blob";
          break;
      case HOST_VERIFY_FROM_SIZE:
      case HOST_VERIFY_TO_SIZE:
          err = "header size mismatch";
          break;
      default:
          err = "reference decoder rejected the patch";
          break;
      } }
    nvm_free();
    return err;
}

int decode_a1(const char *image_path, const char *patch_path){
    HostFile blob = {0}, image = {0};
    FILE *mf = NULL;
    int rc = 2;
    rc = host_read_file(patch_path, &blob); if(rc) goto out;
    if(blob.n<12){ fprintf(stderr,"blob too short\n"); rc = 1; goto out; }
    rc = host_read_file(image_path, &image); if(rc) goto out;
    if(image.n > UINT32_MAX){ fprintf(stderr,"image too large for host decoder: %zu\n", image.n); rc = 2; goto out; }
    HostApply ha;
    { uint32_t image_n = (uint32_t)image.n;
      uint32_t span = image_n>A1_MAX_IMAGE ? image_n : A1_MAX_IMAGE;
      if(host_apply_blob(blob.d, blob.n, image.d, image_n, span, image_n, &ha)) goto out; }

    NvmStats st;
    { int reject = REJ_NONE;
      switch(host_verify_apply(&ha, blob.n, (uint32_t)image.n, 0, 0, &reject)){
      case HOST_VERIFY_OK:
          break;
      case HOST_VERIFY_REJECTED:
        fprintf(stderr,"decode error - rejected (reason=%d: %s)\n", reject,
                reject==REJ_RESOURCE?"resource cap exceeded - firmware larger than build sizing":"corrupt/truncated patch");
        rc = 1; goto out;
      case HOST_VERIFY_TRAILING:
        fprintf(stderr,"decode error - trailing bytes after counted patch body\n");
        rc = 1; goto out;
      case HOST_VERIFY_FROM_SIZE:
        fprintf(stderr,"decode error - patch from_size=%u does not match current image size=%zu\n",
                patch_apply_from_size(&ha.pa), image.n);
        rc = 1; goto out;
      case HOST_VERIFY_TO_SIZE:
          rc = 1; goto out;
      default:
          rc = 1; goto out;
      } }
    if(host_verify_nvm(&st)==HOST_VERIFY_NVM){
        fprintf(stderr,"NVM safety gate FAILED: amplified=%u maxrowerase=%u inversions=%ld\n",
                st.amplified,st.max_row_erases,st.inversions);
        rc = 1; goto out;
    }
    uint32_t to_size=patch_apply_to_size(&ha.pa), span=patch_apply_image_span(&ha.pa);
    mf=fopen(image_path,"r+b"); if(!mf){perror(image_path);goto out;}
    rc = host_commit_image(mf, image_path, sc_flash, to_size, image.n);
    if(rc) goto out;
    rc = host_close_file(&mf, image_path);
    if(rc) goto out;
    fprintf(stderr,"ok to_size=%u dir=%s journal_used=%u slots (cap=%u)\n",to_size,patch_apply_forward(&ha.pa)?"fwd":"bwd",(unsigned)patch_apply_journal_used(&ha.pa),(unsigned)JSLOTS);
    fprintf(stderr,"NVM: erases=%ld rows=%u programs=%ld amplified=%u maxrowerase=%u inversions=%ld (span=%u rows_total=%u, ideal=span/256)\n",
            st.erases,st.rows,st.programs,st.amplified,st.max_row_erases,st.inversions,span,(span+255)/256);
    rc = 0;
out:
    if(mf && !rc) rc = host_close_file(&mf, image_path);
    else if(mf) (void)host_close_file(&mf, image_path);
    nvm_free();
    host_file_free(&image); host_file_free(&blob);
    return rc;
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
