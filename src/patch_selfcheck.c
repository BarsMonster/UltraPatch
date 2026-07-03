/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 */

/* Encoder self-verification: apply an emitted A1 blob to the `from` image on the
 * REFERENCE decoder (the real, unmodified src/patch_apply.h) against an in-memory
 * NVM row emulator, and require an exact `to` image plus clean NVM write-safety.
 * Compiled ONLY into hy_enc; the device artifact is untouched. This turns
 * "hy_enc succeeded" into "the emitted patch provably applies on the reference
 * decoder" — an encoder bug can no longer ship a broken patch silently.
 *
 * The emulator mirrors the SAML22 semantics of patch_apply_demo.c: byte programs
 * are free 1->0 transitions; a program needing 0->1 erases the whole row to 0xFF.
 * Safety counters reproduce the product gate (each row erased at most once, no
 * write amplification, monotonic erase frontier). */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef NVM_ROW
#define NVM_ROW 256u
#endif
static uint8_t *sc_flash, *sc_erasecnt;
static uint32_t sc_flash_n, sc_nrows;
static uint32_t sc_last_erow;
static int sc_edir, sc_ecount;
static long sc_finv;

uint8_t flash_read(uint32_t a) { return a < sc_flash_n ? sc_flash[a] : 0xFF; }
void flash_write(uint32_t a, uint8_t v) {
    if (a >= sc_flash_n) return;
    uint8_t cur = sc_flash[a];
    if (cur == v) return;
    if ((uint8_t)(cur & v) != v) {          /* 0->1 needed: erase the whole row */
        uint32_t row = (a / NVM_ROW) * NVM_ROW, end = row + NVM_ROW;
        if (end > sc_flash_n) end = sc_flash_n;
        memset(sc_flash + row, 0xFF, end - row);
        uint32_t er = a / NVM_ROW;
        if (sc_erasecnt[er] < 255) sc_erasecnt[er]++;
        if (sc_ecount) {
            int d = (er > sc_last_erow) ? 1 : (er < sc_last_erow ? -1 : 0);
            if (d) { if (sc_edir == 0) sc_edir = d; else if (d != sc_edir) sc_finv++; }
        }
        sc_last_erow = er; sc_ecount++;
    }
    sc_flash[a] = v;
}

#include "patch_apply.h"

/* pull-mode byte source: serve the blob buffer to patch_apply_run() */
typedef struct { const uint8_t *d; size_t n, i; } ScPull;
static int sc_pull_next(void *c, uint8_t *out) {
    ScPull *p = (ScPull *)c;
    if (p->i >= p->n) return 0;
    *out = p->d[p->i++];
    return 1;
}

static int sc_uleb(const uint8_t *d, size_t n, size_t *p, uint32_t *out) {
    uint32_t v = 0; int sh = 0; uint8_t b;
    do {
        if (*p >= n || sh > 28) return -1;
        b = d[(*p)++]; v |= (uint32_t)(b & 0x7f) << sh; sh += 7;
    } while (b & 0x80);
    *out = v; return 0;
}
/* uLEB + overlong flag (mirror of patch_apply env_uleb_ov: a redundant trailing 0x00
 * continuation byte marks the unnatural apply direction). */
static int sc_uleb_ov(const uint8_t *d, size_t n, size_t *p, uint32_t *out, int *ov) {
    uint32_t v = 0; int sh = 0, cnt = 0; uint8_t b;
    do {
        if (*p >= n || sh > 28) return -1;
        b = d[(*p)++]; v |= (uint32_t)(b & 0x7f) << sh; sh += 7; cnt++;
    } while (b & 0x80);
    *ov = (cnt > 1 && b == 0);
    *out = v; return 0;
}
static int32_t sc_unzz(uint32_t z) { return (z & 1u) ? -(int32_t)((z >> 1) + 1u) : (int32_t)(z >> 1); }

/* Verify that `blob` applies from -> to on the reference decoder.
 * Returns NULL on success, else a short static error message. */
const char *a1_selfcheck(const uint8_t *blob, size_t blob_n,
                         const uint8_t *from, size_t from_n,
                         const uint8_t *to, size_t to_n)
{
    /* ---- envelope cross-check against the encoder's ground truth (the decoder performs
     * its own authoritative parse; this validates the fields were EMITTED correctly) ---- */
    if (blob_n < 12) return "blob too short";
    size_t p = 4;
    uint32_t from_size, zz;
    if (sc_uleb(blob, blob_n, &p, &from_size)) return "bad from_size uleb";
    if (from_size != (uint32_t)from_n) return "header from_size != actual from size";
    int ov = 0;
    if (sc_uleb_ov(blob, blob_n, &p, &zz, &ov)) return "bad to_size delta uleb";
    int64_t to_size_s = (int64_t)from_size + sc_unzz(zz);
    if (to_size_s < 0 || to_size_s != (int64_t)to_n) return "header to_size != actual to size";
    uint32_t to_size = (uint32_t)to_size_s;
    int desc = (to_size_s > (int64_t)from_size) != ov;   /* natural direction XOR overlong marker */
    if (desc) {
        if (sc_uleb(blob, blob_n, &p, &zz)) return "bad fp_end delta uleb";
        int64_t fpe = (int64_t)from_size + sc_unzz(zz);
        if (fpe < 0) return "header fp_end out of range";
    }
    if (blob_n < p || blob_n - p < 4) return "no room for CRC32(to) trailer";

    /* ---- fresh emulator: flash = from image, padded to span with 0xFF ---- */
    uint32_t span = from_size > to_size ? from_size : to_size;
    free(sc_flash); free(sc_erasecnt);
    sc_nrows = (span + NVM_ROW - 1) / NVM_ROW;
    sc_flash = (uint8_t *)malloc(span ? span : 1);
    sc_erasecnt = (uint8_t *)calloc(sc_nrows ? sc_nrows : 1, 1);
    if (!sc_flash || !sc_erasecnt) return "selfcheck out of memory";
    memcpy(sc_flash, from, from_n);
    if (span > from_n) memset(sc_flash + from_n, 0xFF, span - from_n);
    sc_flash_n = span;
    sc_last_erow = 0; sc_edir = 0; sc_ecount = 0; sc_finv = 0;

    /* ---- run the reference decoder over the WHOLE blob (it parses the envelope and
     * gates on both CRCs itself; DONE implies CRC32(from) and CRC32(to) both verified) ---- */
    ScPull pc = { blob, blob_n, 0 };
    int rc = patch_apply_run(sc_pull_next, &pc);
    if (rc != PATCH_APPLY_DONE)
        return g_reject == REJ_RESOURCE ? "reference decoder rejected the patch (resource cap)"
                                        : "reference decoder rejected the patch";
    if (pc.i != blob_n) return "decoder did not consume the whole blob";

    /* ---- exact output + NVM write-safety ---- */
    if (to_n && memcmp(sc_flash, to, to_n) != 0) return "decoded image differs from target";
    for (uint32_t r = 0; r < sc_nrows; r++)
        if (sc_erasecnt[r] > 1) return "NVM row erased more than once (write amplification)";
    if (sc_finv) return "NVM erase frontier inversion";
    return NULL;
}
