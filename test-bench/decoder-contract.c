/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 *
 * Executable public-API contract for the header-only decoder. The flash backend
 * deliberately lives here: the production decoder remains allocator-
 * free and owns no globals, while the test can inspect every physical write.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PATCH_IMAGE_BASE
#define PATCH_IMAGE_BASE 0u
#endif
#ifndef PATCH_IMAGE_CAPACITY
#define PATCH_IMAGE_CAPACITY 67108864u
#endif

static uint8_t *test_flash;
static uint32_t test_flash_n;
static uint32_t test_writes;
static uint32_t test_oob_writes;
static uint32_t test_unaligned_writes;
static uint32_t test_corrupt_addr;
static uint32_t test_reads;
static uint32_t test_oob_reads;

#include "patch_apply.h"

uint8_t flash_read(uint32_t addr){
    uint32_t offset;
    test_reads++;
    offset = addr - PATCH_IMAGE_BASE;
    if(offset >= PATCH_IMAGE_CAPACITY || offset >= test_flash_n){
        test_oob_reads++;
        return 0xffu;
    }
    return test_flash[offset];
}

void flash_write_page(uint32_t addr, const uint8_t page[OUTROW]){
    uint32_t offset;
    test_writes++;
    if((addr & (OUTROW-1u)) != 0u){ test_unaligned_writes++; return; }
    offset = addr - PATCH_IMAGE_BASE;
    if(offset >= PATCH_IMAGE_CAPACITY || offset > test_flash_n ||
       test_flash_n-offset < OUTROW){ test_oob_writes++; return; }
    memset(test_flash+offset,0xff,OUTROW);
    memcpy(test_flash+offset,page,OUTROW);
    if(test_corrupt_addr>=offset && test_corrupt_addr-offset<OUTROW) test_flash[test_corrupt_addr]^=1u;
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
    PatchApplyResult rc;
    int reject;
    int touched;
    size_t consumed;
    size_t calls;
    uint32_t writes;
    uint32_t reads;
    uint32_t oob_writes;
    uint32_t oob_reads;
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
    if(p->i == p->n) return PATCH_PULL_END;
    *out = p->d[p->i++];
    return PATCH_PULL_BYTE;
}

typedef struct { int result; size_t calls; } PullResult;

static int pull_result(void *ctx, uint8_t *out){
    PullResult *p = (PullResult *)ctx;
    p->calls++;
    *out = 0xa5u;
    return p->result;
}

typedef struct {
    const uint8_t *d;
    size_t n;
    size_t i;
    size_t calls;
    int abort_result;
} PullAbort;

static int pull_abort(void *ctx, uint8_t *out){
    PullAbort *p = (PullAbort *)ctx;
    p->calls++;
    if(p->i == p->n) return p->abort_result;
    *out = p->d[p->i++];
    return PATCH_PULL_BYTE;
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
    test_reads = 0;
    test_oob_reads = 0;
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
    r.reads = test_reads;
    r.oob_writes = test_oob_writes;
    r.oob_reads = test_oob_reads;
    r.unaligned_writes = test_unaligned_writes;
    return r;
}

static uint32_t image_span(const Bytes *a, const Bytes *b){
    size_t n = a->n > b->n ? a->n : b->n;
    return (uint32_t)n;
}

static int writes_bounded(const Result *r, uint32_t span){
    uint32_t rows = (span + OUTROW - 1u) / OUTROW;
    return r->oob_writes == 0u && r->oob_reads == 0u && r->unaligned_writes == 0u &&
           r->writes <= rows;
}

static size_t put_uleb(uint8_t *out, uint32_t v){
    size_t n = 0;
    do{
        uint8_t b = (uint8_t)(v & 0x7fu);
        v >>= 7;
        out[n++] = (uint8_t)(b | (v ? 0x80u : 0u));
    }while(v);
    return n;
}

/* Every envelope has four uLEBs after its CRC pair: source size, signed size delta,
 * direction-specific source seed, and counted body length. */
static int body_offset(const Bytes *blob, size_t *out){
    size_t at = 8u;
    if(blob->n < at) return 1;
    for(int field = 0; field < 4; field++){
        uint8_t b;
        do{
            if(at == blob->n) return 1;
            b = blob->d[at++];
        }while(b & 0x80u);
    }
    *out = at;
    return 0;
}

/* A page-plus-one logical span would make the old decoder scan one byte beyond this backend
 * before rejecting the bad source CRC. The capacity gate must stop immediately after the two
 * size fields, before any flash read or page write. This mode is compiled with a one-page
 * PATCH_IMAGE_CAPACITY by check_decoder_api.sh. */
static int capacity_case(PatchApply *pa){
    Bytes empty = {0}, before = {0};
    uint8_t blob[32] = {0};
    size_t n = 8u;
    CHECK(PATCH_IMAGE_CAPACITY < MAX_IMAGE);
    CHECK(load_flash(&empty, PATCH_IMAGE_CAPACITY, &before) == 0);
    n += put_uleb(blob+n, PATCH_IMAGE_CAPACITY+1u);
    n += put_uleb(blob+n, 0u); /* to_size == from_size */
    { size_t geometry_n = n;
      n += put_uleb(blob+n, 0u); /* ascending fp_start */
      n += put_uleb(blob+n, 4u); /* body size */
      n += 4u;
      Result r = run_blob(pa, blob, n);
      CHECK(r.rc == PATCH_APPLY_ERROR && r.reject == REJ_RESOURCE);
      CHECK(r.consumed == geometry_n && r.calls == geometry_n);
      CHECK(r.reads == 0u && r.oob_reads == 0u);
      CHECK(r.touched == 0 && r.writes == 0u && r.oob_writes == 0u &&
            r.unaligned_writes == 0u);
      CHECK(before.n == 0u || memcmp(test_flash, before.d, before.n) == 0); }
    free(before.d);
    return 0;
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
        uint16_t up = (uint16_t)(up_hy_src_peek(pa, a) | ((uint16_t)up_hy_src_peek(pa, a + 1) << 8));
        if(rc_thumb_ldr_lit(up) && rc_ldr_target((uint32_t)a, up & 0xffu) == fpk) return 1;
    }
    return 0;
}

static void put_u16(uint8_t *p, uint32_t at, uint16_t v){
    p[at] = (uint8_t)v; p[at + 1u] = (uint8_t)(v >> 8);
}

/* Exercise every succinct-journal slot across all 256 high buckets in both directions. This is
 * deliberately independent of patch generation: it catches unary-marker/sample reconstruction
 * errors even when a corpus happens to cluster all preserve positions in one 64 KiB region. */
static int journal_codec_case(void){
    enum { POS_LIMIT = 1u << 24 };
    PatchApply pa;
    CHECK(JSLOTS >= 2u);
    const uint32_t step=(POS_LIMIT-1u)/(JSLOTS-1u);
    for(int fwd=0;fwd<=1;fwd++){
        memset(&pa,0,sizeof pa); pa.g_FWD=fwd;
        for(uint32_t i=0;i<JSLOTS;i++){
            uint32_t key=i*step;
            uint32_t pos=fwd?key:(POS_LIMIT-1u-key);
            up_jr_put(&pa,i,pos,(uint8_t)(i*37u+11u));
        }
        pa.g_jcount=(uint16_t)JSLOTS;
        for(uint32_t i=0;i<JSLOTS;i++){
            uint32_t key=i*step;
            uint32_t pos=fwd?key:(POS_LIMIT-1u-key);
            uint8_t value=0;
            CHECK(up_jr_get(&pa,pos,&value)==1 && value==(uint8_t)(i*37u+11u));
            if(i+1u<JSLOTS) CHECK(up_jr_get(&pa,fwd?pos+1u:pos-1u,&value)==0);
        }
        { uint8_t value=0; CHECK(up_jr_get(&pa,POS_LIMIT,&value)==0); }
    }
    return 0;
}

/* The rolling source word is peek-only: journaled pristine bytes may be cached across an
 * overwritten physical source, but FWD LDR metadata appears only when up_wr_copy actually consumes
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
    up_orow_reset(&pa);
    pa.g_orow_base[0] = 0;
    /* Physical bytes have been overwritten, but the ascending FWD journal preserves an LDR at
     * source 8: 0x4801 targets 16. The other two bytes make the raw-word check unambiguous. */
    memset(test_flash + 8u, 0xa5, 4u);
    up_jr_put(&pa, 0, 8u, 0x01u);
    up_jr_put(&pa, 1, 9u, 0x48u);
    up_jr_put(&pa, 2, 10u, 0x5au);
    up_jr_put(&pa, 3, 11u, 0xc3u);
    pa.g_jcount = 4;
    test_reads = 0;
    uint32_t w = up_hy_word4_peek(&pa, 8);
    CHECK(w == UINT32_C(0xc35a4801) && test_reads == 0u);
    CHECK(pa.g_psrc_even == UINT32_MAX && up_psrc_ldr_take(&pa, 16u) == 0);

    memset(&s, 0, sizeof s); memset(&lc, 0, sizeof lc); memset(&cc, 0, sizeof cc);
    lc.nextpos = -1; cc.i = -1;
    up_wr_copy(&pa, &s, &lc, &cc, 32, 8, 0, 0, (uint8_t)w);
    CHECK(pa.g_psrc_even == 8u && up_psrc_ldr_take(&pa, 16u) == 0);
    up_wr_copy(&pa, &s, &lc, &cc, 32, 9, 0, 1, (uint8_t)(w >> 8));
    CHECK(up_psrc_ldr_take(&pa, 16u) == 1 && test_reads == 0u);

    /* A 2-mod-4 suppressed BL consumes the pending aligned target but its peek remains invisible
     * to the FWD recorder until all four cached bytes go through the ordinary-copy path. */
    memset(&pa, 0, sizeof pa);
    pa.g_from_size = test_flash_n; pa.g_image_span = test_flash_n; pa.g_FWD = 1;
    pa.g_psrc_even = UINT32_MAX;
    up_orow_reset(&pa); pa.g_orow_base[0] = 0;
    put_u16(test_flash, 10u, 0xf000u); put_u16(test_flash, 12u, 0xd000u);
    test_reads = 0; w = up_hy_word4_peek(&pa, 10);
    CHECK(test_reads == 4u);
    up_psrc_ldr_put(&pa, 12u);
    CHECK(up_field_at(&pa, 0, 10, w, packed, 0, 32) == 2);
    CHECK(pa.g_psrc_even == UINT32_MAX && up_psrc_ldr_take(&pa, 12u) == 0);
    memset(&s, 0, sizeof s); memset(&lc, 0, sizeof lc); memset(&cc, 0, sizeof cc);
    lc.nextpos = -1; cc.i = -1;
    for(int b = 0; b < 4; b++) up_wr_copy(&pa, &s, &lc, &cc, 32, 10 + b, 0, b, (uint8_t)(w >> (8*b)));
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
        CHECK(up_grow_ldr_take(&pa, 0, 4096, q) == ldr_oracle(&pa, 0, 4096, q));
        if(q == 2504u){
            CHECK(up_field_at(&pa, 0, 2502, up_hy_word4_peek(&pa, 2502), packed, 0, 4096) == 2); /* clears skipped q=2500 */
            q -= 8u;
        }else q -= 4u;
    }

    /* The sliding scan must use journal-aware pristine bytes, not overwritten flash. */
    memset(test_flash, 0, test_flash_n);
    memset(&pa, 0, sizeof pa);
    pa.g_from_size = test_flash_n; pa.g_FWD = 0; pa.g_psrc_even = UINT32_MAX;
    up_jr_put(&pa, 0, 1001u, 0x48u); /* grow journal is descending */
    up_jr_put(&pa, 1, 1000u, 0xffu); /* LDR +1024 -> 2024 */
    pa.g_jcount = 2;
    CHECK(ldr_oracle(&pa, 0, 4096, 2024u) == 1);
    CHECK(up_grow_ldr_take(&pa, 0, 4096, 2024u) == 1);
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
    CHECK(r.touched == 1 && r.reads > 0u && r.writes > 0u);
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

static int pull_result_case(PatchApply *pa, const Bytes *from, const Bytes *to){
    Bytes before = {0};
    int invalid_results[3] = {PATCH_PULL_END, -1, 2};
    uint32_t span = image_span(from, to);
    for(size_t i=0;i<sizeof invalid_results/sizeof invalid_results[0];i++){
        PullResult pull = { invalid_results[i], 0 };
        CHECK(load_flash(from, span, &before) == 0);
        PatchApplyResult result = patch_apply_run(pa, pull_result, &pull);
        CHECK(result == PATCH_APPLY_ERROR && patch_apply_reject(pa) == REJ_CORRUPT);
        CHECK(pull.calls == 1u);
        CHECK(patch_apply_flash_touched(pa) == 0 && test_reads == 0u && test_writes == 0u);
        CHECK(test_oob_reads == 0u && test_oob_writes == 0u && test_unaligned_writes == 0u);
        CHECK(before.n == 0u || memcmp(test_flash, before.d, before.n) == 0);
        free(before.d); before.d = NULL; before.n = 0;
    }
    return 0;
}

static int body_abort_case(PatchApply *pa, const Bytes *from, const Bytes *to,
                           const Bytes *blob){
    Bytes before = {0};
    int abort_results[2] = {PATCH_PULL_END, 2};
    uint32_t span = image_span(from, to);
    size_t header_n;
    CHECK(body_offset(blob, &header_n) == 0);
    CHECK(header_n + 1u < blob->n);
    for(size_t i = 0; i < sizeof abort_results/sizeof abort_results[0]; i++){
        size_t available = header_n + 1u;
        PullAbort pull = {blob->d, available, 0, 0, abort_results[i]};
        CHECK(load_flash(from, span, &before) == 0);
        PatchApplyResult result = patch_apply_run(pa, pull_abort, &pull);
        CHECK(result == PATCH_APPLY_ERROR && patch_apply_reject(pa) == REJ_CORRUPT);
        CHECK(pull.i == available && pull.calls == available + 1u);
        CHECK(pa->g_body_left == 0u && pa->g_rcerr == 1u);
        CHECK(up_next_byte(pa) == 0u && pull.calls == available + 1u);
        CHECK(patch_apply_flash_touched(pa) == 0 && test_reads > 0u && test_writes == 0u);
        CHECK(test_oob_reads == 0u && test_oob_writes == 0u && test_unaligned_writes == 0u);
        CHECK(before.n == 0u || memcmp(test_flash, before.d, before.n) == 0);
        free(before.d); before.d = NULL; before.n = 0;
    }
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

/* Retag the current envelope as legacy revision zero. A decoder from this revision must reject
 * that otherwise-valid patch at the source CRC gate, before the first physical write. */
static int wire_revision_case(PatchApply *pa, const Bytes *from, const Bytes *to, const Bytes *blob){
    Bytes before = {0};
    uint32_t span = image_span(from, to);
    uint8_t *legacy = (uint8_t *)malloc(blob->n);
    CHECK(blob->n >= 8u && legacy != NULL);
    CHECK(sizeof(PATCH_WIRE_VERSION) == sizeof(uint8_t) && PATCH_WIRE_VERSION != 0u);
    memcpy(legacy, blob->d, blob->n);
    legacy[0] ^= PATCH_WIRE_VERSION;
    CHECK(load_flash(from, span, &before) == 0);
    Result r = run_blob(pa, legacy, blob->n);
    CHECK(r.rc == PATCH_APPLY_ERROR && r.reject == REJ_CORRUPT);
    CHECK(r.consumed >= 12u && r.consumed < blob->n && r.calls == r.consumed);
    CHECK(r.touched == 0 && r.writes == 0u && r.oob_writes == 0u && r.unaligned_writes == 0u);
    CHECK(before.n == 0u || memcmp(test_flash, before.d, before.n) == 0);
    free(legacy);
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
    CHECK(PATCH_APPLY_DONE == 0 && PATCH_APPLY_ERROR != 0);
    CHECK(PATCH_PULL_END == 0 && PATCH_PULL_BYTE == 1);
    if(argc == 2 && strcmp(argv[1], "capacity") == 0){
        rc = capacity_case(&pa);
        if(!rc) printf("decoder_capacity_contract=OK (oob_reads=0 oob_writes=0)\n");
        goto out;
    }
    if(argc == 5 && strcmp(argv[1], "success") == 0){
        if(read_file(argv[2], &from) || read_file(argv[3], &to) ||
           read_file(argv[4], &blob)) goto out;
        rc = success_case(&pa, &from, &to, &blob, &forward1);
        if(!rc) printf("decoder_nonzero_base_contract=OK (reads + writes translated; oob=0)\n");
        goto out;
    }
    if(journal_codec_case()) goto out;
    printf("decoder_journal_codec=OK (%u slots + 256 high buckets + both directions)\n",
           (unsigned)JSLOTS);
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

    /* One caller-owned state object is deliberately reused across success -> envelope/body
     * failures -> success, then across clean-early and touched-late failures. */
    if(success_case(&pa, &from, &to, &blob, &forward1)) goto out;
    if(outer_framing_case(&pa, &from, &to, &blob)) goto out;
    if(early_eof_case(&pa, &from, &to, &blob)) goto out;
    if(pull_result_case(&pa, &from, &to)) goto out;
    if(body_abort_case(&pa, &from, &to, &blob)) goto out;
    if(success_case(&pa, &from2, &to2, &blob2, &forward2)) goto out;
    if(forward1 == forward2){ rc = fail(__LINE__, "grow/revert directions differ"); goto out; }
    if(wire_revision_case(&pa, &from, &to, &blob)) goto out;
    if(early_crc_case(&pa, &from, &to, &blob)) goto out;
    if(late_crc_case(&pa, &from, &to, &blob)) goto out;
    if(nvm_failure_case(&pa, &from2, &to2, &blob2)) goto out;
    printf("decoder_nvm_readback=OK (corrupt final-page write rejected)\n");
    printf("decoder_page_contract=OK (aligned full-page calls + trailing canary preserved)\n");
    printf("decoder_api_contract=OK (state reuse + wire-version/early-clean/late-touched rejects)\n");
    printf("decoder_result_contract=OK (DONE=0 ERROR!=0; pull END=0 BYTE=1 exact)\n");
    printf("decoder_pull_abort=OK (envelope + body END/non-BYTE; callback once)\n");
    rc = 0;
out:
    free(test_flash);
    free(from.d); free(to.d); free(blob.d);
    free(from2.d); free(to2.d); free(blob2.d);
    return rc;
}
