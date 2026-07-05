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

/* Verify that `blob` applies from -> to on the reference decoder.
 * Returns NULL on success, else a short static error message. */
const char *a1_selfcheck(const uint8_t *blob, size_t blob_n,
                         const uint8_t *from, size_t from_n,
                         const uint8_t *to, size_t to_n)
{
    /* ---- fresh emulator: flash = from image, the beyond-`from` span filled with a per-blob
     * deterministic pseudo-random pad (NOT 0xFF). An encoder out-match that reads not-yet-written
     * output falls through to raw flash_read: with a 0xFF pad it returns 0xFF on the emulator AND on
     * 0xFF-erased firmware, so a broken blob passes here yet bricks the device once real flash
     * diverges; random bytes corrupt such a read so the decoder's CRC32(to) rejects it. LCG seeded
     * from the two header CRCs (blob[0..7]) -> reproducible. ---- */
    uint32_t span = from_n > to_n ? (uint32_t)from_n : (uint32_t)to_n;
    uint32_t pad_seed = 0;
    for (int k = 0; k < 8; k++) pad_seed = pad_seed * 1664525u + 1013904223u + blob[k];
    if (nvm_init(from, (uint32_t)from_n, span, pad_seed)) return "selfcheck out of memory";

    /* ---- run the reference decoder over the WHOLE blob (it parses the envelope and
     * gates on both CRCs itself; DONE implies CRC32(from) and CRC32(to) both verified) ---- */
    ScPull pc = { blob, blob_n, 0 };
    int rc = patch_apply_run(sc_pull_next, &pc);
    if (rc != PATCH_APPLY_DONE)
        return patch_apply_reject() == REJ_RESOURCE ? "reference decoder rejected the patch (resource cap)"
                                                    : "reference decoder rejected the patch";
    if (pc.i != blob_n) return "decoder did not consume the whole blob";
    /* header cross-check: decoder's own parsed sizes must equal the encoder's ground truth */
    if (patch_apply_from_size() != (uint32_t)from_n || patch_apply_to_size() != (uint32_t)to_n) return "header size mismatch";

    /* ---- exact output + NVM write-safety ---- */
    if (to_n && memcmp(sc_flash, to, to_n) != 0) return "decoded image differs from target";
    for (uint32_t r = 0; r < sc_nrows; r++)
        if (sc_erasecnt[r] > 1) return "NVM row erased more than once (write amplification)";
    if (sc_finv) return "NVM erase frontier inversion";
    return NULL;
}
