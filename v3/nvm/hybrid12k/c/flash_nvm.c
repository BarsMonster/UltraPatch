#include "flash_nvm.h"
#include <string.h>
#include <stdlib.h>
#ifndef NVM_PAGE
#define NVM_PAGE 64u
#endif
#ifndef NVM_ROW
#define NVM_ROW 256u                 /* SAML22 erase granularity (row = 4 pages) */
#endif
uint8_t *g_flash; uint32_t g_flash_n; uint32_t g_image_span;
static long g_erases, g_programs; static uint8_t *g_erasecnt; static uint32_t g_nrows;
/* req #2 (sequential / monotonic output frontier): the row write-back cache must finalize rows in a
 * single monotonic direction (ascending shrink / descending grow). We watch the ERASE order: each
 * decode's erased-row indices must be monotonic. Any direction reversal is a frontier INVERSION =
 * non-sequential write. This makes "sequential page writes" an explicit gate output, not an inference. */
static long g_finv; static uint32_t g_last_erow; static int g_edir, g_ecount;
void nvm_init(const uint8_t *from, uint32_t from_size, uint32_t span){
    g_image_span = span; g_flash_n = span;
    free(g_flash); g_flash = (uint8_t*)malloc(span?span:1);
    memcpy(g_flash, from, from_size);
    if(span>from_size) memset(g_flash+from_size, 0xFF, span-from_size);  /* erased = 0xFF */
    g_erases = g_programs = 0; g_nrows = (span+NVM_ROW-1)/NVM_ROW;
    g_finv = 0; g_last_erow = 0; g_edir = 0; g_ecount = 0;
    free(g_erasecnt); g_erasecnt = (uint8_t*)calloc(g_nrows?g_nrows:1, 1);  /* erases per row */
}
uint8_t flash_read(uint32_t a){ return a<g_flash_n ? g_flash[a] : 0xFF; }
void flash_write(uint32_t a, uint8_t v){
    if(a>=g_flash_n) return;
    uint8_t cur = g_flash[a];
    if(cur == v) return;                              /* redundant write: free */
    if((uint8_t)(cur & v) != v){                      /* needs a 0->1 -> ERASE the row first */
        uint32_t row = (a/NVM_ROW)*NVM_ROW, end = row+NVM_ROW; if(end>g_flash_n) end=g_flash_n;
        memset(g_flash+row, 0xFF, end-row);           /* erase DESTROYS the whole row */
        g_erases++; if(g_erasecnt[a/NVM_ROW]<255) g_erasecnt[a/NVM_ROW]++;  /* per-row erase count */
        uint32_t er = a/NVM_ROW;                       /* frontier-monotonicity: track erased-row order */
        if(g_ecount){ int d = (er>g_last_erow) ? 1 : (er<g_last_erow ? -1 : 0);
            if(d){ if(g_edir==0) g_edir=d; else if(d!=g_edir) g_finv++; } }
        g_last_erow = er; g_ecount++;
    }
    g_flash[a] = v; g_programs++;                      /* program clears bits */
}
long nvm_erases(void){ return g_erases; }
long nvm_programs(void){ return g_programs; }
uint32_t nvm_rows(void){ uint32_t n=0; for(uint32_t i=0;i<g_nrows;i++) n+=(g_erasecnt[i]>0); return n; }
/* NEW REQUIREMENT gate: every flash page/row erased 0 or 1 times — NO write amplification. */
uint32_t nvm_rows_amplified(void){ uint32_t n=0; for(uint32_t i=0;i<g_nrows;i++) n+=(g_erasecnt[i]>1); return n; }
uint32_t nvm_max_row_erases(void){ uint32_t m=0; for(uint32_t i=0;i<g_nrows;i++) if(g_erasecnt[i]>m) m=g_erasecnt[i]; return m; }
long nvm_frontier_inversions(void){ return g_finv; }  /* req #2: MUST be 0 (rows finalized in one monotonic direction) */
