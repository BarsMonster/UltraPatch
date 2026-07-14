/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 *
 * Shared host decoder backend for the CLI decode path and encoder selfcheck.
 * The device artifact remains src/patch_apply.h; this file exists only so the
 * host tool links one reference-decoder copy instead of one per harness.
 */
#include "enc_internal.h"
#include "nvm_emu.inc"

#include "patch_apply.h"

typedef struct { const uint8_t *d; size_t n, i; } PullCtx;
static int pull_next(void *c, uint8_t *out){
    PullCtx *p=(PullCtx*)c;
    if(p->i>=p->n) return PATCH_PULL_END;
    *out=p->d[p->i++]; return PATCH_PULL_BYTE;
}

typedef struct {
    PatchApply pa;
    size_t consumed;
    PatchApplyResult rc;
} HostApply;

static int host_apply_blob(const uint8_t *blob, size_t blob_n,
                           const uint8_t *from, uint32_t from_n,
                           uint32_t span, uint32_t preserve_from,
                           uint32_t pad_seed,
                           HostApply *out)
{
    if (nvm_init(from, from_n, span, preserve_from, pad_seed)) return -1;
    PullCtx pc = { blob, blob_n, 0 };
    out->rc = patch_apply_run(&out->pa, pull_next, &pc);
    out->consumed = pc.i;
    return 0;
}

const char *selfcheck(const uint8_t *blob, size_t blob_n,
                         const uint8_t *from, size_t from_n,
                         const uint8_t *to, size_t to_n)
{
    if (blob_n < 8) return "selfcheck blob too short";
    uint32_t span = from_n > to_n ? (uint32_t)from_n : (uint32_t)to_n;
    uint32_t pad_seed = 0;
    for (int k = 0; k < 8; k++) pad_seed = pad_seed * 1664525u + 1013904223u + blob[k];
    HostApply ha;
    const char *err = NULL;
    if (host_apply_blob(blob, blob_n, from, (uint32_t)from_n, span,
                        (uint32_t)to_n, pad_seed, &ha)) return "selfcheck out of memory";
    if (ha.rc != PATCH_APPLY_DONE) {
        err = patch_apply_reject(&ha.pa) == REJ_RESOURCE
                  ? "reference decoder rejected the patch (resource cap)"
                  : "reference decoder rejected the patch";
    } else if (ha.consumed != blob_n) {
        err = "decoder did not consume the whole blob";
    } else if (patch_apply_from_size(&ha.pa) != (uint32_t)from_n
               || patch_apply_to_size(&ha.pa) != (uint32_t)to_n) {
        err = "header size mismatch";
    } else if (to_n && memcmp(sc_flash, to, to_n) != 0) {
        err = "decoded image differs from target";
    } else if (!(sc_amplified == 0 && sc_max_erase <= 1 && sc_finv == 0 &&
                 sc_unaligned == 0 && sc_oob_page_writes == 0 && sc_oob_reads == 0 &&
                 nvm_canary_bad() == 0)) {
        if(sc_unaligned) err = "NVM page write was unaligned";
        else if(sc_oob_page_writes) err = "NVM page write was out of bounds";
        else if(sc_oob_reads) err = "NVM read was out of bounds";
        else if(nvm_canary_bad()) err = "NVM post-target data was corrupted";
        else err = sc_amplified ? "NVM page erased more than once (write amplification)"
                                : "NVM erase frontier inversion";
    }
    nvm_free();
    return err;
}

int decode_patch(const char *image_path, const char *patch_path){
    Buf blob = {0}, image = {0};
    int rc = 2;
    int alias = file_alias(image_path, patch_path);
    if(alias < 0) goto out;
    if(alias){
        fprintf(stderr,"%s: image aliases patch %s\n",image_path,patch_path);
        goto out;
    }
    rc = read_file_buf(patch_path, &blob, 0); if(rc) goto out;
    if(blob.n<12){ fprintf(stderr,"blob too short\n"); rc = 1; goto out; }
    rc = read_file_buf(image_path, &image, MAX_IMAGE); if(rc) goto out;
    HostApply ha;
    uint32_t image_n = (uint32_t)image.n;
    if(host_apply_blob(blob.d, blob.n, image.d, image_n, MAX_IMAGE, MAX_IMAGE,
                       image_n, &ha)){ rc = 2; goto out; }

    if(ha.rc != PATCH_APPLY_DONE){
        int reject = patch_apply_reject(&ha.pa);
        if(!reject) reject = REJ_CORRUPT;
        fprintf(stderr,"decode error - rejected (reason=%d: %s)\n", reject,
                reject==REJ_RESOURCE?"configured partition or decoder resource/wire cap exceeded":"corrupt/truncated patch");
        rc = 1; goto out;
    }
    if(ha.consumed != blob.n){
        fprintf(stderr,"decode error - trailing bytes after counted patch body\n");
        rc = 1; goto out;
    }
    if(patch_apply_from_size(&ha.pa) != (uint32_t)image.n){
        fprintf(stderr,"decode error - patch from_size=%u does not match current image size=%zu\n",
                patch_apply_from_size(&ha.pa), image.n);
        rc = 1; goto out;
    }
    if(!(sc_amplified==0 && sc_max_erase<=1 && sc_finv==0 && sc_unaligned==0 &&
         sc_oob_page_writes==0 && sc_oob_reads==0 && nvm_canary_bad()==0)){
        fprintf(stderr,"NVM safety gate FAILED: amplified=%u maxpageerase=%u inversions=%ld unaligned=%u oob=%u oob_reads=%u canary=%u\n",
                sc_amplified,sc_max_erase,sc_finv,sc_unaligned,sc_oob_page_writes,
                sc_oob_reads,nvm_canary_bad());
        rc = 1; goto out;
    }
    uint32_t to_size=patch_apply_to_size(&ha.pa), span=patch_apply_image_span(&ha.pa);
    rc = replace_file(image_path, sc_flash, to_size);
    if(rc) goto out;
    fprintf(stderr,"ok to_size=%u dir=%s\n",to_size,patch_apply_forward(&ha.pa)?"fwd":"bwd");
    fprintf(stderr,"NVM: erases=%ld pages=%u pagewrites=%u programmed_bytes=%ld amplified=%u maxpageerase=%u inversions=%ld unaligned=%u oob=%u oob_reads=%u canary=%u (span=%u pages_total=%u)\n",
            sc_erases,sc_erows,sc_page_writes,sc_programs,sc_amplified,sc_max_erase,sc_finv,
            sc_unaligned,sc_oob_page_writes,sc_oob_reads,nvm_canary_bad(),span,
            (span+OUTROW-1u)/OUTROW);
    rc = 0;
out:
    nvm_free();
    buf_free(&image); buf_free(&blob);
    return rc;
}
