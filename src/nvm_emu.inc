/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 */

/* Shared SAML22-shaped NVM page emulator for the host tool. The reusable
 * decoder (patch_apply.h) only calls flash_read()/flash_write_page(); all emulation and
 * write-safety counters live here, outside the production header. #included by
 * patch_host_backend.c BEFORE patch_apply.h, so CLI decode and encoder selfcheck
 * share one reference-decoder copy in the host tool.
 * Each page call erases and fully programs one aligned decoder-configured page. Counters reproduce
 * the product write-safety rules (each page erased at most once, monotonic erase frontier)
 * and pin aligned/in-range calls plus preservation of bytes beyond the target image.
 * Requires <stdint.h>, <stdlib.h>, <string.h> from the includer. */
static uint8_t *sc_flash, *sc_erasecnt, *sc_tail_canary;
static uint32_t sc_flash_n, sc_preserve_from, sc_tail_n;
static uint32_t sc_erows, sc_amplified, sc_max_erase;
static uint32_t sc_page_writes, sc_unaligned, sc_oob_page_writes, sc_oob_reads;
static long sc_erases, sc_programs, sc_finv;
static uint32_t sc_last_erow;
static int sc_edir, sc_ecount;

static void nvm_free(void){
    free(sc_flash); free(sc_erasecnt); free(sc_tail_canary);
    sc_flash = NULL; sc_erasecnt = NULL; sc_tail_canary = NULL;
    sc_flash_n = sc_preserve_from = sc_tail_n = 0;
}

/* Load `from` into fresh page-rounded flash and reset every counter. The beyond-`from`
 * physical span is filled with a deterministic LCG pad (NOT 0xFF) seeded by pad_seed, so a decode
 * that reads not-yet-written output cannot coast on the 0xFF that both this emulator and
 * 0xFF-erased firmware would return. Bytes at and beyond preserve_from are snapshotted as a
 * canary: a correct final partial-page write must preserve the old physical-page content beyond
 * the new logical image exactly. For a shrinking image this includes old firmware bytes; for a
 * growing image it includes the pre-existing page padding after the new target.
 * Returns -1 on invalid geometry or allocation failure. */
static int nvm_init(const uint8_t *from, uint32_t from_n, uint32_t span,
                    uint32_t preserve_from, uint32_t pad_seed){
    nvm_free();
    if(from_n>span || preserve_from>span || span>UINT32_MAX-(OUTROW-1u)) return -1;
    sc_flash_n = span ? (span+OUTROW-1u)&~(OUTROW-1u) : 0u;
    sc_preserve_from = preserve_from;
    sc_tail_n = sc_flash_n-preserve_from;
    uint32_t nrows = sc_flash_n/OUTROW;
    sc_flash = (uint8_t*)malloc(sc_flash_n?sc_flash_n:1u);
    sc_erasecnt = (uint8_t*)calloc(nrows?nrows:1, 1);
    sc_tail_canary = (uint8_t*)malloc(sc_tail_n?sc_tail_n:1u);
    if(!sc_flash || !sc_erasecnt || !sc_tail_canary){ nvm_free(); return -1; }
    if(from_n) memcpy(sc_flash, from, from_n);   /* from is NULL for an empty image (memcpy nonnull UB) */
    if(sc_flash_n>from_n){ uint32_t r=pad_seed;
        for(uint32_t a=from_n;a<sc_flash_n;a++){ r=r*1664525u+1013904223u; sc_flash[a]=(uint8_t)(r>>24); } }
    if(sc_tail_n) memcpy(sc_tail_canary,sc_flash+preserve_from,sc_tail_n);
    sc_erases = sc_programs = sc_finv = 0;
    sc_erows = sc_amplified = sc_max_erase = 0;
    sc_page_writes = sc_unaligned = sc_oob_page_writes = sc_oob_reads = 0;
    sc_last_erow = 0; sc_edir = 0; sc_ecount = 0;
    return 0;
}
static uint32_t nvm_canary_bad(void){
    uint32_t bad=0;
    for(uint32_t i=0;i<sc_tail_n;i++) if(sc_flash[sc_preserve_from+i]!=sc_tail_canary[i]) bad++;
    return bad;
}
static int nvm_relative_addr(uint32_t absolute, uint32_t *relative){
    uint32_t base=(uint32_t)PATCH_IMAGE_BASE;
    if(absolute<base) return 0;
    *relative=absolute-base;
    return 1;
}
uint8_t flash_read(uint32_t a){
    if(!nvm_relative_addr(a,&a)){ sc_oob_reads++; return 0xFFu; }
    if(a<sc_flash_n) return sc_flash[a];
    sc_oob_reads++;
    return 0xFFu;
}
void flash_write_page(uint32_t a, const uint8_t page[OUTROW]){
    if(!nvm_relative_addr(a,&a)){ sc_oob_page_writes++; return; }
    if((a&(OUTROW-1u))!=0u){ sc_unaligned++; return; }
    uint32_t row=a;
    if(row>sc_flash_n || sc_flash_n-row<OUTROW){ sc_oob_page_writes++; return; }
    uint32_t er=row/OUTROW;
    uint8_t e=sc_erasecnt[er];
    sc_erases++; sc_page_writes++; sc_programs+=OUTROW;
    if(e<255) sc_erasecnt[er]=++e;
    if(e==1) sc_erows++;
    else if(e==2) sc_amplified++;
    if(e>sc_max_erase) sc_max_erase=e;
    if(sc_ecount){ int d=(er>sc_last_erow)?1:(er<sc_last_erow?-1:0);
        if(d){ if(sc_edir==0) sc_edir=d; else if(d!=sc_edir) sc_finv++; } }
    sc_last_erow=er; sc_ecount++;
    memset(sc_flash+row,0xFF,OUTROW);
    memcpy(sc_flash+row,page,OUTROW);
}
