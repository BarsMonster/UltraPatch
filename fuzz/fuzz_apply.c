/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 */

/* libFuzzer harness for the A1 decoder (PULL mode, ASan+UBSan).
 *
 * Input = one whole patch blob, applied to a fixed `from` image on an in-memory flash
 * emulator. Properties asserted every exec:
 *   - no crash / no sanitizer report (never-crash guarantee)
 *   - the decoder returns DONE or ERROR
 *   - on DONE, an INDEPENDENT CRC32 over the written image must equal the blob trailer
 *     (cross-checks the decoder's internal gate -> no silent-wrong accept)
 *
 * The first 4 blob bytes are overwritten with the correct CRC32(from) for the header's
 * from_size, so mutations are not all rejected at the header gate (that gate has dedicated
 * malformed-case coverage; fuzzing wants the body). A1_MAX_IMAGE is tightened to the
 * emulator span so a mutated from_size cannot stall an exec on a 64 MiB CRC scan.
 *
 * Build/run:  make fuzz            (60 s smoke, corpus persisted in fuzz-corpus/)
 * Long run:   make fuzz_apply && ./fuzz_apply -jobs=8 -max_total_time=3600 fuzz-corpus
 * From image: A1_FUZZ_FROM=<path> (default test-bench/fixtures/v0_base/watch.bin)
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define FZ_SPAN (1u<<20)                      /* emulator flash size == A1_MAX_IMAGE */
static uint8_t *fz_flash;
uint8_t flash_read(uint32_t a){ return a<FZ_SPAN ? fz_flash[a] : 0xFF; }
void flash_write(uint32_t a, uint8_t v){ if(a<FZ_SPAN) fz_flash[a]=v; }

#ifndef PATCH_APPLY_PULL
#define PATCH_APPLY_PULL 1
#endif
#ifndef A1_MAX_IMAGE
#define A1_MAX_IMAGE FZ_SPAN
#endif
#include "patch_apply.h"

static uint8_t *fz_from; static size_t fz_from_n;
static uint32_t fz_dirty;                     /* high-water of flash the previous exec could have written */

static uint32_t fz_crc_tab[256];
static void fz_crc_init(void){
    for(uint32_t i=0;i<256;i++){ uint32_t c=i;
        for(int k=0;k<8;k++) c=(c>>1)^(0xedb88320u&(uint32_t)(-(int32_t)(c&1)));
        fz_crc_tab[i]=c; }
}
static uint32_t fz_crc(const uint8_t *p, size_t n){
    uint32_t c=0xffffffffu;
    for(size_t i=0;i<n;i++) c=(c>>8)^fz_crc_tab[(c^p[i])&0xffu];
    return c^0xffffffffu;
}

int LLVMFuzzerInitialize(int *argc, char ***argv){
    (void)argc; (void)argv;
    const char *path = getenv("A1_FUZZ_FROM");
    if(!path) path = "test-bench/fixtures/v0_base/watch.bin";
    FILE *f = fopen(path,"rb");
    if(!f){ fprintf(stderr,"fuzz: cannot open from-image %s\n",path); exit(2); }
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    if(n<=0 || (size_t)n>FZ_SPAN){ fprintf(stderr,"fuzz: bad from-image size %ld\n",n); exit(2); }
    fz_from_n=(size_t)n; fz_from=(uint8_t*)malloc(fz_from_n);
    if(fread(fz_from,1,fz_from_n,f)!=fz_from_n) exit(2);
    fclose(f);
    fz_crc_init();
    fz_flash=(uint8_t*)malloc(FZ_SPAN);
    memset(fz_flash,0xFF,FZ_SPAN);
    fz_dirty=FZ_SPAN;   /* first exec resets everything once */
    return 0;
}

typedef struct { const uint8_t *d; size_t n, i; } Pull;
static int pull_next(void *c, uint8_t *out){
    Pull *p=(Pull*)c;
    if(p->i>=p->n) return 0;
    *out=p->d[p->i++]; return 1;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size){
    if(size<5 || size>FZ_SPAN) return 0;
    uint8_t *blob=(uint8_t*)malloc(size);
    memcpy(blob,data,size);

    /* fresh flash = from image, 0xFF beyond — but only re-blank what the previous exec could
     * have dirtied (the decoder writes strictly below its g_image_span; full-span memsets
     * dominated exec time). */
    memcpy(fz_flash,fz_from,fz_from_n);
    if(fz_dirty>fz_from_n) memset(fz_flash+fz_from_n,0xFF,fz_dirty-fz_from_n);

    /* parse the header's from_size (mini-uLEB) and force the CRC32(from) gate open for it */
    { uint32_t fs=0; int sh=0; size_t p=4; int ok=1; uint8_t b;
      do{ if(p>=size||sh>28){ ok=0; break; } b=blob[p++]; fs|=(uint32_t)(b&0x7fu)<<sh; sh+=7; }while(b&0x80u);
      if(ok && fs<=FZ_SPAN){
          uint32_t c = fs<=fz_from_n ? fz_crc(fz_from,fs) : fz_crc(fz_flash,fs);
          blob[0]=(uint8_t)c; blob[1]=(uint8_t)(c>>8); blob[2]=(uint8_t)(c>>16); blob[3]=(uint8_t)(c>>24);
      } }

    g_image_span=0;                        /* so a header-reject exec marks nothing dirty */
    Pull pc={blob,size,0};
    int rc=patch_apply_run(pull_next,&pc);
    fz_dirty=g_image_span;
    if(rc==PATCH_APPLY_DONE){
        /* independent silent-wrong cross-check */
        uint32_t want=(uint32_t)blob[size-4]|((uint32_t)blob[size-3]<<8)
                     |((uint32_t)blob[size-2]<<16)|((uint32_t)blob[size-1]<<24);
        if(fz_crc(fz_flash,g_to_size)!=want) abort();
    } else if(rc!=PATCH_APPLY_ERROR) abort();

    free(blob);
    return 0;
}
