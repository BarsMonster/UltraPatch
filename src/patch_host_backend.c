/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 *
 * Shared host decoder backend for the CLI decode path and encoder selfcheck.
 * The device artifact remains src/patch_apply.h; this file exists only so the
 * host tool links one reference-decoder copy instead of one per harness.
 */
#include "enc_internal.h"

/* Shared SAML22-shaped NVM page emulator for the host tool. The reusable decoder (patch_apply.h,
 * included below) only calls flash_read()/flash_write_page(); all emulation and write-safety
 * counters live here, above that include so CLI decode and encoder selfcheck share one
 * reference-decoder copy in the host tool.
 * Each page call erases and fully programs one aligned decoder-configured page. Counters reproduce
 * the product write-safety rules (each page erased at most once, monotonic erase frontier)
 * and pin aligned/in-range calls plus preservation of bytes beyond the target image. */
static uint8_t *sc_flash, *sc_erasecnt, *sc_tail_canary;
static uint32_t sc_flash_n, sc_preserve_from, sc_tail_n;
static uint32_t sc_erows, sc_amplified;
static uint32_t sc_unaligned, sc_oob_page_writes, sc_oob_reads;
static long sc_erases, sc_finv;
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
 * Allocation failure terminates via xmalloc/xcalloc; the geometry guard is unreachable from the
 * host callers (span/preserve_from are host-derived and MAX_IMAGE-bounded) but stays defensive. */
static void nvm_init(const uint8_t *from, uint32_t from_n, uint32_t span,
                     uint32_t preserve_from, uint32_t pad_seed){
    nvm_free();
    if(from_n>span || preserve_from>span || span>UINT32_MAX-(OUTROW-1u)) die("nvm_init: impossible geometry");
    sc_flash_n = span ? (span+OUTROW-1u)&~(OUTROW-1u) : 0u;
    sc_preserve_from = preserve_from;
    sc_tail_n = sc_flash_n-preserve_from;
    uint32_t nrows = sc_flash_n/OUTROW;
    sc_flash = (uint8_t*)xmalloc(sc_flash_n);
    sc_erasecnt = (uint8_t*)xcalloc(nrows, 1);
    sc_tail_canary = (uint8_t*)xmalloc(sc_tail_n);
    if(from_n) memcpy(sc_flash, from, from_n);   /* from is NULL for an empty image (memcpy nonnull UB) */
    if(sc_flash_n>from_n){ uint32_t r=pad_seed;
        for(uint32_t a=from_n;a<sc_flash_n;a++){ r=r*1664525u+1013904223u; sc_flash[a]=(uint8_t)(r>>24); } }
    if(sc_tail_n) memcpy(sc_tail_canary,sc_flash+preserve_from,sc_tail_n);
    sc_erases = sc_finv = 0;
    sc_erows = sc_amplified = 0;
    sc_unaligned = sc_oob_page_writes = sc_oob_reads = 0;
    sc_last_erow = 0; sc_edir = 0; sc_ecount = 0;
}
static uint32_t nvm_canary_bad(void){
    uint32_t bad=0;
    for(uint32_t i=0;i<sc_tail_n;i++) if(sc_flash[sc_preserve_from+i]!=sc_tail_canary[i]) bad++;
    return bad;
}
/* Single-sourced NVM write-safety gate: NULL when clean, else the first-failure cause string.
 * Shared by encoder selfcheck (returns it) and the CLI decode path (prints it). */
static const char *nvm_gate_err(void){
    if(sc_unaligned)       return "NVM page write was unaligned";
    if(sc_oob_page_writes) return "NVM page write was out of bounds";
    if(sc_oob_reads)       return "NVM read was out of bounds";
    if(nvm_canary_bad())   return "old NVM page content beyond target image was corrupted";
    if(sc_amplified)       return "NVM page erased more than once (write amplification)";
    if(sc_finv)            return "NVM erase frontier inversion";
    return NULL;
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
    sc_erases++;
    if(e<255) sc_erasecnt[er]=++e;
    if(e==1) sc_erows++;
    else if(e==2) sc_amplified++;
    if(sc_ecount){ int d=(er>sc_last_erow)?1:(er<sc_last_erow?-1:0);
        if(d){ if(sc_edir==0) sc_edir=d; else if(d!=sc_edir) sc_finv++; } }
    sc_last_erow=er; sc_ecount++;
    memset(sc_flash+row,0xFF,OUTROW);
    memcpy(sc_flash+row,page,OUTROW);
}

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

/* Self-contained uLEB reader over the raw blob prefix (the header is plain bytes, not range-coded).
 * Advances *pi; rejects truncation and a value that would overflow uint32 (mirrors the decoder's
 * UP_ULEB32_OVERFLOW cap). */
static int peek_uleb(const uint8_t *blob, size_t blob_n, size_t *pi, uint32_t *out){
    uint32_t v = 0; int sh = 0; size_t i = *pi;
    for(;;){
        if(i >= blob_n) return 0;
        uint8_t b = blob[i++];
        if(sh == 28 && (b & 0x7fu) > 15u) return 0;      /* would overflow uint32_t */
        v |= (uint32_t)(b & 0x7fu) << sh; sh += 7;
        if(!(b & 0x80u)) break;
        if(sh > 28) return 0;                            /* > 5 bytes: malformed */
    }
    *pi = i; *out = v; return 1;
}
/* Strict envelope peek mirroring emit_wire_blob's plain-byte header prefix: skip the 8 CRC bytes
 * (4 from^ver, 4 to), read uLEB from_size, then the zigzag-uLEB size delta and reconstruct
 * to_size exactly as up_decode_header / rc_zz_abs do (the overlong "unnatural direction" marker
 * on the delta is value-neutral, so a plain uLEB read recovers the magnitude). Returns 0 on any
 * truncation or implausible size (both capped at MAX_IMAGE), leaving the outputs untouched. */
static int peek_envelope_sizes(const uint8_t *blob, size_t blob_n, uint32_t *fs, uint32_t *ts){
    size_t i = 8;
    uint32_t f, z;
    if(!peek_uleb(blob, blob_n, &i, &f) || f > MAX_IMAGE) return 0;
    if(!peek_uleb(blob, blob_n, &i, &z)) return 0;
    uint32_t t;
    if(z & 1u){ uint32_t m = (z>>1)+1u; if(m > f) return 0; t = f - m; }
    else      { uint32_t d = z>>1;      if(d > MAX_IMAGE || f > MAX_IMAGE - d) return 0; t = f + d; }
    *fs = f; *ts = t;
    return 1;
}

/* Owns the NVM geometry policy so the two callers no longer disagree (the CLI path previously
 * forced preserve_from=MAX_IMAGE, making the tail canary vacuous and mallocing 64 MiB). Both
 * callers require blob_n>=8 (pad_seed reads blob[0..8)). */
static void host_apply_blob(const uint8_t *blob, size_t blob_n,
                            const uint8_t *from, uint32_t from_n,
                            HostApply *out)
{
    uint32_t pad_seed = 0;
    for (int k = 0; k < 8; k++) pad_seed = pad_seed * 1664525u + 1013904223u + blob[k];
    /* Recover from/to sizes from the envelope; a doomed blob (bad header) falls back to from_n and
     * is rejected by the real decoder before any flash write. Span includes fs so a bogus fs CRC
     * probe stays in-bounds (no spurious sc_oob_reads); preserve_from=ts makes the canary live. */
    uint32_t fs, ts;
    if (!peek_envelope_sizes(blob, blob_n, &fs, &ts)) fs = ts = from_n;
    uint32_t span = from_n;
    if (fs > span) span = fs;
    if (ts > span) span = ts;
    nvm_init(from, from_n, span, ts, pad_seed);
    PullCtx pc = { blob, blob_n, 0 };
    out->rc = patch_apply_run(&out->pa, pull_next, &pc);
    out->consumed = pc.i;
}

const char *selfcheck(const uint8_t *blob, size_t blob_n,
                         const uint8_t *from, size_t from_n,
                         const uint8_t *to, size_t to_n)
{
    if (blob_n < 8) return "selfcheck blob too short";
    HostApply ha;
    const char *err = NULL;
    host_apply_blob(blob, blob_n, from, (uint32_t)from_n, &ha);
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
    } else {
        err = nvm_gate_err();
    }
    nvm_free();
    return err;
}

int decode_patch(const char *image_path, const char *patch_path){
    Buf blob = {0}, image = {0};
    int rc = 2;
    reject_if_alias(image_path, patch_path, "image aliases patch");
    read_file_buf(patch_path, &blob, 0);
    if(blob.n<12){ fprintf(stderr,"blob too short\n"); rc = 1; goto out; }
    read_file_buf(image_path, &image, MAX_IMAGE);
    HostApply ha;
    host_apply_blob(blob.d, blob.n, image.d, (uint32_t)image.n, &ha);

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
    { const char *gate = nvm_gate_err();
      if(gate){
          fprintf(stderr,"NVM safety gate FAILED: %s (amplified=%u inversions=%ld unaligned=%u oob=%u oob_reads=%u canary=%u)\n",
                  gate,sc_amplified,sc_finv,sc_unaligned,sc_oob_page_writes,
                  sc_oob_reads,nvm_canary_bad());
          rc = 1; goto out;
      } }
    uint32_t to_size=patch_apply_to_size(&ha.pa), span=patch_apply_image_span(&ha.pa);
    replace_file(image_path, sc_flash, to_size);
    fprintf(stderr,"ok to_size=%u dir=%s\n",to_size,patch_apply_forward(&ha.pa)?"fwd":"bwd");
    fprintf(stderr,"NVM: erases=%ld pages=%u programmed_bytes=%ld amplified=%u inversions=%ld unaligned=%u oob=%u oob_reads=%u canary=%u (span=%u pages_total=%u)\n",
            sc_erases,sc_erows,sc_erases*(long)OUTROW,sc_amplified,sc_finv,
            sc_unaligned,sc_oob_page_writes,sc_oob_reads,nvm_canary_bad(),span,
            (span+OUTROW-1u)/OUTROW);
    rc = 0;
out:
    nvm_free();
    buf_free(&image); buf_free(&blob);
    return rc;
}
