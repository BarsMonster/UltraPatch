/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 */

/* Shared SAML22-shaped NVM row emulator for the host gate binaries. The reusable
 * decoder (patch_apply.h) only calls flash_read()/flash_write(); all emulation and
 * write-safety counters live here, outside the production header. #included by BOTH
 * patch_apply_demo.c and patch_selfcheck.c BEFORE their patch_apply.h -- separate
 * binaries, so the static state stays private to each. The selfcheck copy is the
 * ship/no-ship oracle: this file keeps the two byte-identical instead of by eyeball.
 * Semantics: a byte program is a free 1->0 transition; a program needing any 0->1
 * erases the whole 256 B row to 0xFF. Counters reproduce the product write-safety
 * gate (each row erased at most once, monotonic erase frontier). Requires <stdint.h>,
 * <stdlib.h>, <string.h> from the includer. */
#ifndef NVM_ROW
#define NVM_ROW 256u
#endif
static uint8_t *sc_flash, *sc_erasecnt;
static uint32_t sc_flash_n, sc_nrows;
static long sc_erases, sc_programs, sc_finv;
static uint32_t sc_last_erow;
static int sc_edir, sc_ecount;

/* Load `from` into a fresh span-sized flash and reset every counter. The beyond-`from`
 * span is filled with a deterministic LCG pad (NOT 0xFF) seeded by pad_seed, so a decode
 * that reads not-yet-written output cannot coast on the 0xFF that both this emulator and
 * 0xFF-erased firmware would return -- each user seeds pad_seed its own way (task 3).
 * Returns -1 on allocation failure. */
static int nvm_init(const uint8_t *from, uint32_t from_n, uint32_t span, uint32_t pad_seed){
    free(sc_flash); free(sc_erasecnt);
    sc_flash_n = span; sc_nrows = (span+NVM_ROW-1)/NVM_ROW;
    sc_flash = (uint8_t*)malloc(span?span:1);
    sc_erasecnt = (uint8_t*)calloc(sc_nrows?sc_nrows:1, 1);
    if(!sc_flash || !sc_erasecnt) return -1;
    if(from_n) memcpy(sc_flash, from, from_n);   /* from is NULL for an empty image (memcpy nonnull UB) */
    if(span>from_n){ uint32_t r=pad_seed;
        for(uint32_t a=from_n;a<span;a++){ r=r*1664525u+1013904223u; sc_flash[a]=(uint8_t)(r>>24); } }
    sc_erases = sc_programs = sc_finv = 0;
    sc_last_erow = 0; sc_edir = 0; sc_ecount = 0;
    return 0;
}
uint8_t flash_read(uint32_t a){ return a<sc_flash_n ? sc_flash[a] : 0xFF; }
void flash_write(uint32_t a, uint8_t v){
    if(a>=sc_flash_n) return;
    uint8_t cur = sc_flash[a];
    if(cur == v) return;
    if((uint8_t)(cur & v) != v){          /* 0->1 needed: erase the whole row to 0xFF */
        uint32_t row = (a/NVM_ROW)*NVM_ROW, end = row+NVM_ROW; if(end>sc_flash_n) end=sc_flash_n;
        memset(sc_flash+row, 0xFF, end-row);
        sc_erases++; if(sc_erasecnt[a/NVM_ROW]<255) sc_erasecnt[a/NVM_ROW]++;
        uint32_t er = a/NVM_ROW;
        if(sc_ecount){ int d = (er>sc_last_erow) ? 1 : (er<sc_last_erow ? -1 : 0);
            if(d){ if(sc_edir==0) sc_edir=d; else if(d!=sc_edir) sc_finv++; } }
        sc_last_erow = er; sc_ecount++;
    }
    sc_flash[a] = v; sc_programs++;
}
