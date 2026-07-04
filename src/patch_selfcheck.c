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

/* SAML22 NVM row emulator + write-safety counters, shared byte-for-byte with the gate
 * emulator in patch_apply_demo.c. Provides flash_read()/flash_write()/nvm_init(); must
 * be #included BEFORE patch_apply.h. */
#include "nvm_emu.inc"

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
     * its own authoritative parse; this validates the fields were EMITTED correctly).
     * Layout: CRC32(from)[4] | CRC32(to)[4] | from_size uLEB | zz(to-from) | [zz(fp_end)] |
     * range body. Skip both 4-byte CRCs (the decoder gates on them) and validate the sizes. */
    if (blob_n < 10) return "blob too short";
    size_t p = 8;
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
    if (blob_n < p) return "header runs past end of blob";

    /* ---- fresh emulator: flash = from image, the beyond-`from` span filled with a per-blob
     * deterministic pseudo-random pad (NOT 0xFF). An encoder out-match that reads not-yet-written
     * output falls through to raw flash_read: with a 0xFF pad it returns 0xFF on the emulator AND on
     * 0xFF-erased firmware, so a broken blob passes here yet bricks the device once real flash
     * diverges; random bytes corrupt such a read so the decoder's CRC32(to) rejects it. LCG seeded
     * from the two header CRCs (blob[0..7]) -> reproducible. ---- */
    uint32_t span = from_size > to_size ? from_size : to_size;
    uint32_t pad_seed = 0;
    for (int k = 0; k < 8; k++) pad_seed = pad_seed * 1664525u + 1013904223u + blob[k];
    if (nvm_init(from, (uint32_t)from_n, span, pad_seed)) return "selfcheck out of memory";

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
