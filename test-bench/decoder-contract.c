/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 *
 * Executable public-API contract for the header-only decoder.  This one source is
 * compiled against both src/patch_apply.h and the generated single-header artifact.
 * The flash backend deliberately lives here: the production decoder remains allocator-
 * free and owns no globals, while the test can inspect every physical write.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PATCH_IMAGE_BASE 0u

static uint8_t *test_flash;
static uint32_t test_flash_n;
static uint32_t test_writes;
static uint32_t test_oob_writes;
static uint32_t test_unaligned_writes;
static uint32_t test_corrupt_addr;
static uint32_t test_reads;

#ifdef DECODER_SINGLE_HEADER
#include "patch_apply_single.h"
#else
#include "patch_apply.h"
#endif

uint8_t flash_read(uint32_t addr){
    test_reads++;
    return addr < test_flash_n ? test_flash[addr] : 0xffu;
}

void flash_write_page(uint32_t addr, const uint8_t page[OUTROW]){
    test_writes++;
    if((addr & (OUTROW-1u)) != 0u){ test_unaligned_writes++; return; }
    if(addr > test_flash_n || test_flash_n-addr < OUTROW){ test_oob_writes++; return; }
    memset(test_flash+addr,0xff,OUTROW);
    memcpy(test_flash+addr,page,OUTROW);
    if(test_corrupt_addr>=addr && test_corrupt_addr-addr<OUTROW) test_flash[test_corrupt_addr]^=1u;
}

typedef struct {
    uint8_t *d;
    size_t n;
} Bytes;

typedef struct {
    const uint8_t *d;
    size_t n;
    size_t i;
    size_t calls;
} Pull;

typedef struct {
    int rc;
    int reject;
    int touched;
    size_t consumed;
    size_t calls;
    uint32_t writes;
    uint32_t oob_writes;
    uint32_t unaligned_writes;
} Result;

#define CHECK(x) do { if(!(x)) return fail(__LINE__, #x); } while(0)

static int fail(int line, const char *expr){
    fprintf(stderr, "decoder-contract:%d: check failed: %s\n", line, expr);
    return 1;
}

static int read_file(const char *path, Bytes *out){
    FILE *f = fopen(path, "rb");
    long end;
    if(!f){ perror(path); return 1; }
    if(fseek(f, 0, SEEK_END) || (end = ftell(f)) < 0 || fseek(f, 0, SEEK_SET)){
        perror(path); fclose(f); return 1;
    }
    out->n = (size_t)end;
    out->d = (uint8_t *)malloc(out->n ? out->n : 1u);
    if(!out->d){ fprintf(stderr, "out of memory reading %s\n", path); fclose(f); return 1; }
    if(out->n && fread(out->d, 1, out->n, f) != out->n){
        perror(path); free(out->d); out->d = NULL; fclose(f); return 1;
    }
    if(fclose(f)){ perror(path); free(out->d); out->d = NULL; return 1; }
    return 0;
}

static int pull_next(void *ctx, uint8_t *out){
    Pull *p = (Pull *)ctx;
    p->calls++;
    if(p->i == p->n) return 0;
    *out = p->d[p->i++];
    return 1;
}

static int load_flash(const Bytes *from, uint32_t span, Bytes *before){
    uint32_t r = 0x91e10da5u;
    uint32_t physical = span ? (span+OUTROW-1u)&~(OUTROW-1u) : 0u;
    free(test_flash);
    test_flash = (uint8_t *)malloc(physical ? physical : 1u);
    before->d = (uint8_t *)malloc(physical ? physical : 1u);
    before->n = physical;
    if(!test_flash || !before->d) return 1;
    test_flash_n = physical;
    if(from->n) memcpy(test_flash, from->d, from->n);
    for(uint32_t i = (uint32_t)from->n; i < physical; i++){
        r = r * 1664525u + 1013904223u;
        test_flash[i] = (uint8_t)(r >> 24);
    }
    if(physical) memcpy(before->d, test_flash, physical);
    test_writes = 0;
    test_oob_writes = 0;
    test_unaligned_writes = 0;
    test_corrupt_addr = UINT32_MAX;
    return 0;
}

static Result run_blob(PatchApply *pa, const uint8_t *blob, size_t blob_n){
    Pull p = { blob, blob_n, 0, 0 };
    Result r;
    r.rc = patch_apply_run(pa, pull_next, &p);
    r.reject = patch_apply_reject(pa);
    r.touched = patch_apply_flash_touched(pa);
    r.consumed = p.i;
    r.calls = p.calls;
    r.writes = test_writes;
    r.oob_writes = test_oob_writes;
    r.unaligned_writes = test_unaligned_writes;
    return r;
}

static uint32_t image_span(const Bytes *a, const Bytes *b){
    size_t n = a->n > b->n ? a->n : b->n;
    return (uint32_t)n;
}

static int writes_bounded(const Result *r, uint32_t span){
    uint32_t rows = (span + OUTROW - 1u) / OUTROW;
    return r->oob_writes == 0u && r->unaligned_writes == 0u && r->writes <= rows;
}

static int tail_preserved(const Bytes *before, uint32_t span){
    return span==before->n || memcmp(test_flash+span,before->d+span,before->n-span)==0;
}

/* Exact oracle for the grow-direction sliding LDR window. This exercises the cases that a
 * whole-patch round-trip does not isolate: initial targets above the first query (1 KiB alias),
 * an eight-byte catch-up across a 2-mod-4 suppressed BL, and pristine bytes served by the journal. */
static int ldr_oracle(PatchApply *pa, int32_t fp0, int32_t dl, uint32_t fpk){
    if(!rc_ldr_target_in_op(fp0, dl, fpk)) return 0;
    for(int32_t a = rc_ldr_scan_first(fp0, fpk); a + 2 <= (int32_t)fpk; a += 2){
        uint16_t up = (uint16_t)(hy_src_peek(pa, a) | ((uint16_t)hy_src_peek(pa, a + 1) << 8));
        if(rc_thumb_ldr_lit(up) && rc_ldr_target(a, (int32_t)(up & 0xffu)) == (int32_t)fpk) return 1;
    }
    return 0;
}

static void put_u16(uint8_t *p, uint32_t at, uint16_t v){
    p[at] = (uint8_t)v; p[at + 1u] = (uint8_t)(v >> 8);
}

/* The rolling source word is peek-only: journaled pristine bytes may be cached across an
 * overwritten physical source, but FWD LDR metadata appears only when wr_copy actually consumes
 * those bytes. A suppressed 2-mod-4 BL must likewise clear its skipped target without recording
 * its cached probe; replaying the four ordinary copies records the exact raw word, with no reread. */
static int src_window_case(void){
    PatchApply pa;
    up_ApplyState s;
    up_LitCur lc;
    up_CorrCur cc;
    uint8_t packed[4];
    free(test_flash);
    test_flash_n = 64u;
    test_flash = (uint8_t *)calloc(test_flash_n, 1u);
    CHECK(test_flash != NULL);

    memset(&pa, 0, sizeof pa);
    pa.g_from_size = test_flash_n; pa.g_image_span = test_flash_n; pa.g_FWD = 1;
    pa.g_psrc_even = UINT32_MAX;
    orow_reset(&pa);
    pa.g_orow_base[0] = 0;
    /* Physical bytes have been overwritten, but the ascending FWD journal preserves an LDR at
     * source 8: 0x4801 targets 16. The other two bytes make the raw-word check unambiguous. */
    memset(test_flash + 8u, 0xa5, 4u);
    pa.g_jcount = 4;
    pa.ARENA.apply.jbuf[0] = (8u << 8) | 0x01u;
    pa.ARENA.apply.jbuf[1] = (9u << 8) | 0x48u;
    pa.ARENA.apply.jbuf[2] = (10u << 8) | 0x5au;
    pa.ARENA.apply.jbuf[3] = (11u << 8) | 0xc3u;
    test_reads = 0;
    uint32_t w = hy_word4_peek(&pa, 8);
    CHECK(w == UINT32_C(0xc35a4801) && test_reads == 0u);
    CHECK(pa.g_psrc_even == UINT32_MAX && psrc_ldr_take(&pa, 16u) == 0);

    memset(&s, 0, sizeof s); memset(&lc, 0, sizeof lc); memset(&cc, 0, sizeof cc);
    lc.nextpos = -1; cc.i = -1;
    wr_copy(&pa, &s, &lc, &cc, 32, 8, 0, 0, (uint8_t)w);
    CHECK(pa.g_psrc_even == 8u && psrc_ldr_take(&pa, 16u) == 0);
    wr_copy(&pa, &s, &lc, &cc, 32, 9, 0, 1, (uint8_t)(w >> 8));
    CHECK(psrc_ldr_take(&pa, 16u) == 1 && test_reads == 0u);

    /* A 2-mod-4 suppressed BL consumes the pending aligned target but its peek remains invisible
     * to the FWD recorder until all four cached bytes go through the ordinary-copy path. */
    memset(&pa, 0, sizeof pa);
    pa.g_from_size = test_flash_n; pa.g_image_span = test_flash_n; pa.g_FWD = 1;
    pa.g_psrc_even = UINT32_MAX;
    orow_reset(&pa); pa.g_orow_base[0] = 0;
    put_u16(test_flash, 10u, 0xf000u); put_u16(test_flash, 12u, 0xd000u);
    test_reads = 0; w = hy_word4_peek(&pa, 10);
    CHECK(test_reads == 4u);
    psrc_ldr_put(&pa, 12u);
    CHECK(field_at(&pa, 0, 10, w, packed, 0, 32) == 2);
    CHECK(pa.g_psrc_even == UINT32_MAX && psrc_ldr_take(&pa, 12u) == 0);
    memset(&s, 0, sizeof s); memset(&lc, 0, sizeof lc); memset(&cc, 0, sizeof cc);
    lc.nextpos = -1; cc.i = -1;
    for(int b = 0; b < 4; b++) wr_copy(&pa, &s, &lc, &cc, 32, 10 + b, 0, b, (uint8_t)(w >> (8*b)));
    CHECK(test_reads == 4u && pa.g_psrc_even == 12u);
    return 0;
}

static int ldr_window_case(void){
    PatchApply pa;
    uint8_t packed[4];
    free(test_flash);
    test_flash_n = 4096u;
    test_flash = (uint8_t *)calloc(test_flash_n, 1u);
    CHECK(test_flash != NULL);

    /* Real hits at q=3000, plus an above-first-query target 4020 whose modulo slot aliases
     * q=2996 and must never enter the descending ring. */
    put_u16(test_flash, 1976u, 0x48ffu); /* +1024 -> 3000 */
    put_u16(test_flash, 2000u, 0x48f9u); /* +1000 -> 3000 */
    put_u16(test_flash, 2996u, 0x48ffu); /* +1024 -> 4020 (above first query) */
    /* This target is pending when the BL at 2502 skips aligned candidate 2500. If it is not
     * cleared, it aliases q=1476 exactly 1024 bytes later. */
    put_u16(test_flash, 1500u, 0x48f9u); /* +1000 -> 2500 */
    put_u16(test_flash, 2502u, 0xf000u);
    put_u16(test_flash, 2504u, 0xd000u);

    memset(&pa, 0, sizeof pa);
    pa.g_from_size = test_flash_n; pa.g_FWD = 0; pa.g_psrc_even = UINT32_MAX;
    for(uint32_t q = 3000u; q >= 1400u; ){
        CHECK(grow_ldr_take(&pa, 0, 4096, q) == ldr_oracle(&pa, 0, 4096, q));
        if(q == 2504u){
            CHECK(field_at(&pa, 0, 2502, hy_word4_peek(&pa, 2502), packed, 0, 4096) == 2); /* clears skipped q=2500 */
            q -= 8u;
        }else q -= 4u;
    }

    /* The sliding scan must use journal-aware pristine bytes, not overwritten flash. */
    memset(test_flash, 0, test_flash_n);
    memset(&pa, 0, sizeof pa);
    pa.g_from_size = test_flash_n; pa.g_FWD = 0; pa.g_psrc_even = UINT32_MAX;
    pa.g_jcount = 2;
    pa.ARENA.apply.jbuf[0] = (1001u << 8) | 0x48u; /* grow journal is descending */
    pa.ARENA.apply.jbuf[1] = (1000u << 8) | 0xffu; /* LDR +1024 -> 2024 */
    CHECK(ldr_oracle(&pa, 0, 4096, 2024u) == 1);
    CHECK(grow_ldr_take(&pa, 0, 4096, 2024u) == 1);
    return 0;
}

static int success_case(PatchApply *pa, const Bytes *from, const Bytes *to, const Bytes *blob,
                        int *forward){
    Bytes before = {0};
    uint32_t span = image_span(from, to);
    CHECK(from->n <= UINT32_MAX && to->n <= UINT32_MAX);
    CHECK(load_flash(from, span, &before) == 0);
    Result r = run_blob(pa, blob->d, blob->n);
    CHECK(r.rc == PATCH_APPLY_DONE);
    CHECK(r.reject == REJ_NONE);
    CHECK(r.consumed == blob->n && r.calls == blob->n);
    CHECK(r.touched == 1 && r.writes > 0u);
    CHECK(writes_bounded(&r, span));
    CHECK(patch_apply_from_size(pa) == (uint32_t)from->n);
    CHECK(patch_apply_to_size(pa) == (uint32_t)to->n);
    CHECK(patch_apply_image_span(pa) == span);
    CHECK(to->n == 0 || memcmp(test_flash, to->d, to->n) == 0);
    CHECK(tail_preserved(&before,span));
    *forward = patch_apply_forward(pa);
    free(before.d);
    return 0;
}

static int early_eof_case(PatchApply *pa, const Bytes *from, const Bytes *to, const Bytes *blob){
    Bytes before = {0};
    uint32_t span = image_span(from, to);
    size_t n = blob->n < 7u ? blob->n : 7u;
    CHECK(load_flash(from, span, &before) == 0);
    Result r = run_blob(pa, blob->d, n);
    CHECK(r.rc == PATCH_APPLY_ERROR && r.reject == REJ_CORRUPT);
    CHECK(r.consumed == n && r.calls == n + 1u);
    CHECK(r.touched == 0 && r.writes == 0u && r.oob_writes == 0u && r.unaligned_writes == 0u);
    CHECK(before.n == 0u || memcmp(test_flash, before.d, before.n) == 0);
    free(before.d);
    return 0;
}

static int outer_framing_case(PatchApply *pa, const Bytes *from, const Bytes *to, const Bytes *blob){
    Bytes before = {0};
    uint32_t span = image_span(from, to);
    uint8_t *framed = (uint8_t *)malloc(blob->n + 1u);
    CHECK(framed != NULL);
    memcpy(framed, blob->d, blob->n);
    framed[blob->n] = 0x5au;
    CHECK(load_flash(from, span, &before) == 0);
    Result r = run_blob(pa, framed, blob->n + 1u);
    CHECK(r.rc == PATCH_APPLY_DONE && r.reject == REJ_NONE);
    CHECK(r.consumed == blob->n && r.calls == blob->n);
    CHECK(r.touched == 1 && writes_bounded(&r, span));
    CHECK(to->n == 0 || memcmp(test_flash, to->d, to->n) == 0);
    CHECK(tail_preserved(&before,span));
    free(framed);
    free(before.d);
    return 0;
}

static int early_crc_case(PatchApply *pa, const Bytes *from, const Bytes *to, const Bytes *blob){
    Bytes before = {0};
    uint32_t span = image_span(from, to);
    uint8_t *bad = (uint8_t *)malloc(blob->n);
    CHECK(blob->n >= 8u && bad != NULL);
    memcpy(bad, blob->d, blob->n);
    bad[0] ^= 0x80u;
    CHECK(load_flash(from, span, &before) == 0);
    Result r = run_blob(pa, bad, blob->n);
    CHECK(r.rc == PATCH_APPLY_ERROR && r.reject == REJ_CORRUPT);
    CHECK(r.consumed >= 12u && r.consumed < blob->n && r.calls == r.consumed);
    CHECK(r.touched == 0 && r.writes == 0u && r.oob_writes == 0u && r.unaligned_writes == 0u);
    CHECK(before.n == 0u || memcmp(test_flash, before.d, before.n) == 0);
    free(bad);
    free(before.d);
    return 0;
}

static int late_crc_case(PatchApply *pa, const Bytes *from, const Bytes *to, const Bytes *blob){
    Bytes before = {0};
    uint32_t span = image_span(from, to);
    uint8_t *bad = (uint8_t *)malloc(blob->n);
    CHECK(blob->n >= 8u && bad != NULL);
    memcpy(bad, blob->d, blob->n);
    bad[4] ^= 0x80u;
    CHECK(load_flash(from, span, &before) == 0);
    Result r = run_blob(pa, bad, blob->n);
    CHECK(r.rc == PATCH_APPLY_ERROR && r.reject == REJ_CORRUPT);
    CHECK(r.consumed == blob->n && r.calls == blob->n);
    CHECK(r.touched == 1 && r.writes > 0u);
    CHECK(writes_bounded(&r, span));
    CHECK(to->n == 0 || memcmp(test_flash, to->d, to->n) == 0);
    CHECK(tail_preserved(&before,span));
    free(bad);
    free(before.d);
    return 0;
}

/* Corrupt one byte in the final FWD page, which is committed only after the range stream has been
 * consumed. The terminal CRC must reject the physical read-back rather than intended output. */
static int nvm_failure_case(PatchApply *pa, const Bytes *from, const Bytes *to, const Bytes *blob){
    Bytes before = {0};
    uint32_t span = image_span(from, to);
    CHECK(to->n > 0u);
    CHECK(load_flash(from, span, &before) == 0);
    test_corrupt_addr = (uint32_t)to->n - 1u;
    Result r = run_blob(pa, blob->d, blob->n);
    test_corrupt_addr = UINT32_MAX;
    CHECK(r.rc == PATCH_APPLY_ERROR && r.reject == REJ_CORRUPT);
    CHECK(r.consumed == blob->n && r.calls == blob->n);
    CHECK(r.touched == 1 && r.writes > 0u && writes_bounded(&r, span));
    CHECK(memcmp(test_flash, to->d, to->n) != 0);
    free(before.d);
    return 0;
}

static int resource_case(PatchApply *pa, const Bytes *from, const Bytes *to,
                         const Bytes *blob, int want_touched){
    Bytes before = {0};
    uint32_t span = image_span(from, to);
    CHECK(load_flash(from, span, &before) == 0);
    Result r = run_blob(pa, blob->d, blob->n);
    CHECK(r.rc == PATCH_APPLY_ERROR && r.reject == REJ_RESOURCE);
    CHECK(r.consumed > 8u && r.consumed <= blob->n && r.calls == r.consumed);
    CHECK(r.touched == want_touched);
    CHECK(writes_bounded(&r, span));
    if(want_touched){
        CHECK(r.writes > 0u);
    }else{
        CHECK(r.writes == 0u);
        CHECK(before.n == 0u || memcmp(test_flash, before.d, before.n) == 0);
    }
    free(before.d);
    return 0;
}

int main(int argc, char **argv){
    Bytes from = {0}, to = {0}, blob = {0}, from2 = {0}, to2 = {0}, blob2 = {0};
    PatchApply pa;
    int forward1 = 0, forward2 = 0;
    int rc = 1;
    memset(&pa, 0xa5, sizeof pa);
    if(src_window_case()) goto out;
    printf("decoder_src_window=OK (journal + cached replay + FWD timing)\n");
    if(ldr_window_case()) goto out;
    printf("decoder_ldr_window=OK (alias filter + BL skip + journal)\n");

    if(argc == 5 && (strcmp(argv[1], "resource-clean") == 0 ||
                     strcmp(argv[1], "resource-touched") == 0)){
        if(read_file(argv[2], &from) || read_file(argv[3], &to) || read_file(argv[4], &blob)) goto out;
        rc = resource_case(&pa, &from, &to, &blob, strcmp(argv[1], "resource-touched") == 0);
        if(!rc) printf("decoder_resource_contract=OK (%s)\n", argv[1]);
        goto out;
    }
    if(argc != 7){
        fprintf(stderr, "usage: %s <from1> <to1> <blob1> <from2> <to2> <blob2>\n", argv[0]);
        goto out;
    }
    if(read_file(argv[1], &from) || read_file(argv[2], &to) || read_file(argv[3], &blob) ||
       read_file(argv[4], &from2) || read_file(argv[5], &to2) || read_file(argv[6], &blob2)) goto out;

    /* One caller-owned state object is deliberately reused across success -> failure ->
     * success, then across clean-early and touched-late failures. */
    if(success_case(&pa, &from, &to, &blob, &forward1)) goto out;
    if(outer_framing_case(&pa, &from, &to, &blob)) goto out;
    if(early_eof_case(&pa, &from, &to, &blob)) goto out;
    if(success_case(&pa, &from2, &to2, &blob2, &forward2)) goto out;
    if(forward1 == forward2){ rc = fail(__LINE__, "grow/revert directions differ"); goto out; }
    if(early_crc_case(&pa, &from, &to, &blob)) goto out;
    if(late_crc_case(&pa, &from, &to, &blob)) goto out;
    if(nvm_failure_case(&pa, &from2, &to2, &blob2)) goto out;
    printf("decoder_nvm_readback=OK (corrupt final-page write rejected)\n");
    printf("decoder_page_contract=OK (aligned full-page calls + trailing canary preserved)\n");
    printf("decoder_api_contract=OK (state reuse + counted framing + early-clean/late-touched rejects)\n");
    rc = 0;
out:
    free(test_flash);
    free(from.d); free(to.d); free(blob.d);
    free(from2.d); free(to2.d); free(blob2.d);
    return rc;
}
