/*
 * arm-cortex-m4 data-format transform — apply side, C port of
 * detools/data_format/arm_cortex_m4.py (+ utils.py).
 *
 * Given the decompressed data-format patch ("dfpatch"), the from-image and the
 * to-size, this reconstructs the two buffers the sequential applier needs:
 *   - fromzero: the from-image with address fields zeroed   (FromReader)
 *   - dfdiff:   a to_size overlay of re-encoded to-addresses (DiffReader)
 * so that  to[i] = patch[i] + fromzero[i] + dfdiff[i].
 *
 * Host/correctness-first: buffers are malloc'd. MCU streaming is a later step.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "arm_cortex_m4.h"

/* ---------- varint (matches common.unpack_size_with_length) ---------- */
typedef struct { const uint8_t *p; size_t size; size_t pos; } rd_t;

static int rd_byte(rd_t *r, uint8_t *b) {
    if (r->pos >= r->size) return -1;
    *b = r->p[r->pos++];
    return 0;
}
/* signed size — 32-bit only: offset>28 (a 6th group) means the value exceeds 32 bits. */
static int rd_size(rd_t *r, int32_t *out) {
    uint8_t b;
    if (rd_byte(r, &b)) return -1;
    int is_signed = (b & 0x40);
    int32_t value = (b & 0x3f);
    int offset = 6;
    while (b & 0x80) {
        if (offset > 28 || rd_byte(r, &b)) return -1;
        value |= ((int32_t)(b & 0x7f) << offset);
        offset += 7;
    }
    if (is_signed) value = -value;
    *out = value;
    return 0;
}
/* unsigned size (reinterpret) — for our positive offsets == rd_size */
static int rd_usize(rd_t *r, uint32_t *out) {
    int32_t v; if (rd_size(r, &v)) return -1; *out = (uint32_t)v; return 0;
}

/* ---------- address->value maps ---------- */
typedef struct { uint32_t addr; int32_t val; } kv_t;
typedef struct { kv_t *a; size_t n, cap; } map_t;

static int map_push(map_t *m, uint32_t addr, int32_t val) {
    if (m->n == m->cap) {
        size_t nc = m->cap ? m->cap * 2 : 256;
        kv_t *na = realloc(m->a, nc * sizeof(kv_t));
        if (!na) return -1;
        m->a = na; m->cap = nc;
    }
    m->a[m->n].addr = addr; m->a[m->n].val = val; m->n++;
    return 0;
}
static int kv_cmp(const void *x, const void *y) {
    uint32_t a = ((const kv_t*)x)->addr, b = ((const kv_t*)y)->addr;
    return (a > b) - (a < b);
}
/* sort by addr, drop duplicate addrs (keep last seen, mirroring dict overwrite) */
static void map_finalize(map_t *m) {
    if (m->n == 0) return;
    /* stable-ish: stamp original index in val's high bits? values can be large.
       duplicates here always carry equal values (same memory word), so plain
       sort + unique-by-addr is equivalent to Python's dict+sorted. */
    qsort(m->a, m->n, sizeof(kv_t), kv_cmp);
    size_t w = 0;
    for (size_t i = 0; i < m->n; i++) {
        if (w > 0 && m->a[w-1].addr == m->a[i].addr) m->a[w-1] = m->a[i];
        else m->a[w++] = m->a[i];
    }
    m->n = w;
}
static void map_free(map_t *m){ free(m->a); m->a=NULL; m->n=m->cap=0; }

/* ---------- uint32 hash set (literal-pool addresses; O(1) membership) ----------
 * Replaces the old O(n^2) linear scan over the ldr/ldr_w maps. Power-of-two table,
 * open addressing, grown on 0.75 load. Sized to the number of literals (dynamic). */
typedef struct { uint32_t *key; uint8_t *used; size_t cap, n; } uset_t;
static int uset_put_raw(uint32_t *key, uint8_t *used, size_t cap, uint32_t k) {
    size_t mask = cap - 1, i = (size_t)((k * 2654435761u) >> 11) & mask;
    while (used[i]) { if (key[i] == k) return 0; i = (i + 1) & mask; }
    used[i] = 1; key[i] = k; return 1;
}
static int uset_grow(uset_t *s, size_t newcap) {
    uint32_t *nk = calloc(newcap, sizeof(uint32_t)); uint8_t *nu = calloc(newcap, 1);
    if (!nk || !nu) { free(nk); free(nu); return -1; }
    for (size_t i = 0; i < s->cap; i++) if (s->used[i]) uset_put_raw(nk, nu, newcap, s->key[i]);
    free(s->key); free(s->used); s->key = nk; s->used = nu; s->cap = newcap; return 0;
}
static int uset_add(uset_t *s, uint32_t k) {
    if (s->cap == 0 || (s->n + 1) * 4 >= s->cap * 3) {
        if (uset_grow(s, s->cap ? s->cap * 2 : 256)) return -1;
    }
    if (uset_put_raw(s->key, s->used, s->cap, k)) s->n++;
    return 0;
}
static int uset_has(const uset_t *s, uint32_t k) {
    if (s->cap == 0) return 0;
    size_t mask = s->cap - 1, i = (size_t)((k * 2654435761u) >> 11) & mask;
    while (s->used[i]) { if (s->key[i] == k) return 1; i = (i + 1) & mask; }
    return 0;
}
static void uset_free(uset_t *s){ free(s->key); free(s->used); s->key=NULL; s->used=NULL; s->cap=s->n=0; }

/* ---------- little-endian helpers ---------- */
static uint16_t rd16(const uint8_t *p){ return (uint16_t)(p[0] | (p[1]<<8)); }
static uint32_t rd32(const uint8_t *p){ return (uint32_t)(p[0]|(p[1]<<8)|(p[2]<<16)|((uint32_t)p[3]<<24)); }
static void wr16(uint8_t *p, uint16_t v){ p[0]=v&0xff; p[1]=(v>>8)&0xff; }
static void wr32(uint8_t *p, uint32_t v){ p[0]=v&0xff;p[1]=(v>>8)&0xff;p[2]=(v>>16)&0xff;p[3]=(v>>24)&0xff; }

/* ---------- bl/bw (re)encoders (inverse of py unpack_bl/unpack_bw) ---------- */
static int32_t unpack_bl(uint16_t up, uint16_t lo) {
    int32_t s=(up>>10)&1, imm10=up&0x3ff, imm11=lo&0x7ff;
    int32_t j1=(lo>>13)&1, j2=(lo>>11)&1;
    int32_t i1=1-(j1^s), i2=1-(j2^s);
    int32_t v=(s<<23)|(i1<<22)|(i2<<21)|(imm10<<11)|imm11;
    if (s) v -= (1<<24);
    return v;
}
static int32_t unpack_bw(uint16_t up, uint16_t lo) {
    int32_t s=(up>>10)&1, cond=(up>>6)&0xf, imm6=up&0x3f;
    int32_t imm11=lo&0x7ff, j1=(lo>>13)&1, t=(lo>>12)&1, j2=(lo>>11)&1;
    int32_t v=(s<<24)|(j2<<23)|(j1<<22)|(imm6<<16)|(imm11<<5)|(cond<<1)|t;
    if (s) v -= (1<<25);
    return v;
}
static void pack_bl(int32_t imm32, uint8_t out[4]) {
    if (imm32 < 0) imm32 += (1<<24);
    int32_t s=(imm32>>23)&1, i1=(imm32>>22)&1, i2=(imm32>>21)&1;
    int32_t j1=1-(i1^s), j2=1-(i2^s);
    int32_t imm10=(imm32>>11)&0x3ff, imm11=imm32&0x7ff;
    uint16_t up = 0xF000 | (s<<10) | imm10;
    uint16_t lo = 0xD000 | (j1<<13) | (j2<<11) | imm11;
    wr16(out, up); wr16(out+2, lo);
}
static void pack_bw(int32_t value, uint8_t out[4]) {
    if (value < 0) value += (1<<25);
    int32_t t=value&1, cond=(value>>1)&0xf, imm32=value>>5;
    int32_t s=(imm32>>19)&1, j2=(imm32>>18)&1, j1=(imm32>>17)&1;
    int32_t imm6=(imm32>>11)&0x3f, imm11=imm32&0x7ff;
    uint16_t up = 0xF000 | (s<<10) | (cond<<6) | imm6;
    uint16_t lo = 0x8000 | (j1<<13) | (t<<12) | (j2<<11) | imm11;
    wr16(out, up); wr16(out+2, lo);
}

/* ---------- disassemble (port of arm_cortex_m4.disassemble) ---------- */
typedef struct {
    map_t bw, bl, ldr, ldr_w, data_ptr, code_ptr;
    uset_t lit;   /* every addr pushed to ldr/ldr_w — for O(1) literal-pool skip during the scan */
} dis_t;

static void ldr_common(const uint8_t *f, size_t fsize, uint32_t address, uint32_t imm,
                       map_t *ldr, uset_t *lit) {
    if ((address % 4) == 2) address -= 2;
    address += imm;
    if ((size_t)address + 4 > fsize) return;
    int32_t v = (int32_t)rd32(f + address);
    if (map_push(ldr, address, v) == 0) uset_add(lit, address);
}

/* The python code builds ldr/ldr_w incrementally and tests `addr in ldr` as it scans,
   to skip literal-pool words. We mirror that exactly with the `lit` hash set: every
   address pushed to ldr/ldr_w is recorded, and membership is a single O(1) lookup. */

/* emit_bw/emit_ldr_w default-on (Cortex-M4/Thumb-2). Off for Thumb-1/ARMv6-M (M0+),
 * where B.W and LDR.W cannot occur, so any match is a spurious decode of data. Byte
 * consumption is unchanged either way, so bl/ldr/data/code detection stays identical. */
static void disassemble(const uint8_t *f, size_t fsize,
                        uint32_t data_off_begin, uint32_t data_off_end,
                        uint32_t data_begin, uint32_t data_end,
                        uint32_t code_begin, uint32_t code_end,
                        int emit_bw, int emit_ldr_w,
                        dis_t *d) {
    memset(d, 0, sizeof(*d));
    uint32_t addr = 0;
    while (addr < fsize) {
        if (data_off_begin <= addr && addr < data_off_end) {
            if ((size_t)addr + 4 > fsize) break;
            uint32_t value = rd32(f + addr);
            if (data_begin <= value && value < data_end) map_push(&d->data_ptr, addr, value);
            else if (code_begin <= value && value < code_end) map_push(&d->code_ptr, addr, value);
            addr += 4;
        } else if (uset_has(&d->lit, addr)) {
            addr += 4;
        } else {
            if ((size_t)addr + 2 > fsize) break;
            uint16_t up = rd16(f + addr);
            uint32_t ins = addr;
            addr += 2;
            if ((up & 0xf800) == 0xf000) {            /* bl / b.w */
                if ((size_t)addr + 2 > fsize) continue;
                uint16_t lo = rd16(f + addr); addr += 2;
                if ((lo & 0xd000) == 0xd000) map_push(&d->bl, ins, unpack_bl(up, lo));
                else if (emit_bw && (lo & 0xc000) == 0x8000) map_push(&d->bw, ins, unpack_bw(up, lo));
                /* bw is never used for in-scan membership, so gating the push here is exact */
            } else if ((up & 0xf800) == 0x4800) {     /* ldr (literal) */
                ldr_common(f, fsize, ins, 4 * (up & 0xff) + 4, &d->ldr, &d->lit);
            } else if (up == 0xf8df) {                /* ldr.w */
                if ((size_t)addr + 2 > fsize) continue;
                uint16_t lo = rd16(f + addr); addr += 2;
                ldr_common(f, fsize, ins, (lo & 0xfff) + 4, &d->ldr_w, &d->lit);
            } else if ((up & 0xfff0) == 0xfbb0 || (up & 0xfff0) == 0xfb90 ||
                       (up & 0xfff0) == 0xf8d0 || (up & 0xfff0) == 0xf850) {
                addr += 2;
            } else if ((up & 0xffe0) == 0xfa00) {
                addr += 2;
            } else if ((up & 0xffc0) == 0xe900) {
                addr += 2;
            }
        }
    }
    /* ldr_w IS used for in-scan membership (skipping literal-pool words), so it must be
     * built during the loop; clear only the returned result. Mirrors the Python patch. */
    if (!emit_ldr_w) d->ldr_w.n = 0;
    uset_free(&d->lit);   /* only needed during the scan */
    map_finalize(&d->bw); map_finalize(&d->bl);
    map_finalize(&d->ldr); map_finalize(&d->ldr_w);
    map_finalize(&d->data_ptr); map_finalize(&d->code_ptr);
}

/* ---------- block header/body ---------- */
typedef struct { int32_t from_offset; int32_t to_address; int32_t n; int32_t *values; } block_t;
typedef struct { block_t *b; size_t n; } blocks_t;

static int unpack_block_header(rd_t *r, blocks_t *bl) {
    int32_t nb; if (rd_size(r, &nb)) return -1;
    /* corrupt-input guard: a block costs >=3 size bytes, so nb can't exceed bytes left */
    if (nb < 0 || (size_t)nb > r->size) return -1;
    bl->n = (size_t)nb;
    bl->b = nb ? calloc(nb, sizeof(block_t)) : NULL;
    if (nb && !bl->b) return -1;
    for (size_t i=0;i<bl->n;i++) {
        if (rd_size(r,&bl->b[i].from_offset)||rd_size(r,&bl->b[i].to_address)||rd_size(r,&bl->b[i].n))
            return -1;
        if (bl->b[i].n < 0 || (size_t)bl->b[i].n > r->size) return -1;  /* >=1 byte/value */
    }
    return 0;
}
static int load_block_values(rd_t *r, blocks_t *bl) {
    for (size_t i=0;i<bl->n;i++) {
        bl->b[i].values = bl->b[i].n ? malloc(bl->b[i].n*sizeof(int32_t)) : NULL;
        if (bl->b[i].n && !bl->b[i].values) return -1;
        for (int32_t k=0;k<bl->b[i].n;k++)
            if (rd_size(r,&bl->b[i].values[k])) return -1;
    }
    return 0;
}
static void blocks_free(blocks_t *bl){ for(size_t i=0;i<bl->n;i++) free(bl->b[i].values); free(bl->b); bl->b=NULL; bl->n=0; }

/* apply a block set: zero from-fields, overlay re-encoded to-values into dfdiff */
enum pack_kind { PK_S32, PK_BL, PK_BW };
static void apply_blocks(const blocks_t *bl, const map_t *sorted,
                         uint8_t *fromzero, size_t from_size,
                         uint8_t *dfdiff, size_t to_size, enum pack_kind pk) {
    for (size_t i=0;i<bl->n;i++) {
        block_t *b = &bl->b[i];
        if ((size_t)b->from_offset >= sorted->n) continue;
        uint32_t base = sorted->a[b->from_offset].addr;
        for (int32_t k=0;k<b->n;k++) {
            size_t idx = (size_t)b->from_offset + k;
            if (idx >= sorted->n) break;
            uint32_t faddr = sorted->a[idx].addr;
            int32_t fval = sorted->a[idx].val;
            /* FromReader: zero 4 bytes at faddr */
            if ((size_t)faddr + 4 <= from_size) memset(fromzero + faddr, 0, 4);
            /* DiffReader: write packed(fval - value) at to_address+faddr-base */
            int32_t to_pos = b->to_address + (int32_t)faddr - (int32_t)base;
            if (to_pos < 0 || (size_t)to_pos + 4 > to_size) continue;
            int32_t diff = (int32_t)((uint32_t)fval - (uint32_t)b->values[k]);
            uint8_t enc[4];
            if (pk == PK_S32) wr32(enc, (uint32_t)diff);
            else if (pk == PK_BL) pack_bl(diff, enc);
            else pack_bw(diff, enc);
            memcpy(dfdiff + to_pos, enc, 4);
        }
    }
}

/* ---------- top-level ---------- */
int detools_m4_build(const uint8_t *dfpatch, size_t dfpatch_size,
                     const uint8_t *from, size_t from_size, size_t to_size,
                     uint8_t **fromzero_out, uint8_t **dfdiff_out) {
    rd_t r = { dfpatch, dfpatch_size, 0 };

    /* All allocations are released once at `cleanup`; every error path goes through it.
       Variables referenced by cleanup are declared+initialized up front so no `goto`
       jumps over their initialization. */
    int rc = -1, dis_live = 0;
    uint8_t *fromzero = NULL, *dfdiff = NULL;
    blocks_t data_blk={0}, code_blk={0}, bw_blk={0}, bl_blk={0}, ldr_blk={0}, ldrw_blk={0};
    dis_t d;

    /* pointers header */
    uint8_t present;
    uint32_t from_data_offset=0, from_data_begin=0, from_data_end=0;
    uint32_t from_code_begin=0, from_code_end=0;
    int data_present, code_present;
    if (rd_byte(&r,&present)) goto cleanup;
    data_present = (present==1);
    if (data_present) { if(rd_usize(&r,&from_data_offset)||rd_usize(&r,&from_data_begin)||rd_usize(&r,&from_data_end)) goto cleanup; }
    if (rd_byte(&r,&present)) goto cleanup;
    code_present = (present==1);
    if (code_present) { if(rd_usize(&r,&from_code_begin)||rd_usize(&r,&from_code_end)) goto cleanup; }

    if (data_present) { if (unpack_block_header(&r,&data_blk)) goto cleanup; }
    if (code_present) { if (unpack_block_header(&r,&code_blk)) goto cleanup; }
    if (unpack_block_header(&r,&bw_blk)) goto cleanup;
    if (unpack_block_header(&r,&bl_blk)) goto cleanup;
    if (unpack_block_header(&r,&ldr_blk)) goto cleanup;
    if (unpack_block_header(&r,&ldrw_blk)) goto cleanup;
    /* bodies, in the order create wrote them: data, code, bw, bl, ldr, ldr_w */
    if (data_present && load_block_values(&r,&data_blk)) goto cleanup;
    if (code_present && load_block_values(&r,&code_blk)) goto cleanup;
    if (load_block_values(&r,&bw_blk)) goto cleanup;
    if (load_block_values(&r,&bl_blk)) goto cleanup;
    if (load_block_values(&r,&ldr_blk)) goto cleanup;
    if (load_block_values(&r,&ldrw_blk)) goto cleanup;

    /* disassemble the from-image */
    uint32_t data_off_end = (uint32_t)(from_data_offset + (from_data_end - from_data_begin));
    disassemble(from, from_size,
                (uint32_t)from_data_offset, data_off_end,
                (uint32_t)from_data_begin, (uint32_t)from_data_end,
                (uint32_t)from_code_begin, (uint32_t)from_code_end,
                /*emit_bw=*/1, /*emit_ldr_w=*/1, &d);   /* legacy M4 build path */
    dis_live = 1;

    fromzero = malloc(from_size ? from_size : 1);
    dfdiff = calloc(to_size ? to_size : 1, 1);
    if (!fromzero || !dfdiff) goto cleanup;
    memcpy(fromzero, from, from_size);

    /* order matches DiffReader.__init__: ldr, ldr_w, bl, bw, data, code */
    apply_blocks(&ldr_blk,  &d.ldr,      fromzero, from_size, dfdiff, to_size, PK_S32);
    apply_blocks(&ldrw_blk, &d.ldr_w,    fromzero, from_size, dfdiff, to_size, PK_S32);
    apply_blocks(&bl_blk,   &d.bl,       fromzero, from_size, dfdiff, to_size, PK_BL);
    apply_blocks(&bw_blk,   &d.bw,       fromzero, from_size, dfdiff, to_size, PK_BW);
    if (data_present) apply_blocks(&data_blk, &d.data_ptr, fromzero, from_size, dfdiff, to_size, PK_S32);
    if (code_present) apply_blocks(&code_blk, &d.code_ptr, fromzero, from_size, dfdiff, to_size, PK_S32);

    *fromzero_out = fromzero; *dfdiff_out = dfdiff;
    fromzero = dfdiff = NULL;   /* ownership transferred to caller */
    rc = 0;

cleanup:
    if (dis_live) {
        map_free(&d.bw); map_free(&d.bl); map_free(&d.ldr); map_free(&d.ldr_w);
        map_free(&d.data_ptr); map_free(&d.code_ptr);
    }
    blocks_free(&data_blk); blocks_free(&code_blk); blocks_free(&bw_blk);
    blocks_free(&bl_blk); blocks_free(&ldr_blk); blocks_free(&ldrw_blk);
    free(fromzero); free(dfdiff);
    return rc;
}

/* ---- ultrapatch v2: expose disassembler + re-encoder ---- */
int detools_m4_disassemble(const uint8_t *from, size_t from_size,
                           uint32_t data_offset, uint32_t data_begin, uint32_t data_end,
                           uint32_t code_begin, uint32_t code_end,
                           m4_stream_t streams[M4_NSTREAMS]) {
    dis_t d;
    uint32_t data_off_end = data_offset + (data_end - data_begin);
    /* ultrapatch M0+ config: suppress spurious B.W / LDR.W (Thumb-2, impossible on ARMv6-M) */
    disassemble(from, from_size, data_offset, data_off_end, data_begin, data_end,
                code_begin, code_end, /*emit_bw=*/0, /*emit_ldr_w=*/0, &d);
    map_t *src[M4_NSTREAMS] = { &d.data_ptr, &d.code_ptr, &d.bw, &d.bl, &d.ldr, &d.ldr_w };
    for (int s = 0; s < M4_NSTREAMS; s++) { streams[s].a = NULL; streams[s].n = 0; }
    int rc = 0;
    for (int s = 0; s < M4_NSTREAMS && !rc; s++) {
        size_t n = src[s]->n;
        streams[s].a = malloc((n ? n : 1) * sizeof(m4_field_t));
        if (!streams[s].a) { rc = -1; break; }
        streams[s].n = n;
        for (size_t i = 0; i < n; i++) {
            streams[s].a[i].addr = src[s]->a[i].addr;
            streams[s].a[i].val  = src[s]->a[i].val;   /* maps already sorted by addr */
        }
    }
    if (rc) detools_m4_free_streams(streams);   /* release any partial allocation on OOM */
    map_free(&d.bw); map_free(&d.bl); map_free(&d.ldr); map_free(&d.ldr_w);
    map_free(&d.data_ptr); map_free(&d.code_ptr);
    return rc;
}
void detools_m4_free_streams(m4_stream_t streams[M4_NSTREAMS]) {
    for (int s = 0; s < M4_NSTREAMS; s++) { free(streams[s].a); streams[s].a = NULL; streams[s].n = 0; }
}
void detools_m4_pack(int pk, int32_t value, uint8_t out[4]) {
    if (pk == M4_PK_BL) pack_bl(value, out);
    else if (pk == M4_PK_BW) pack_bw(value, out);
    else wr32(out, (uint32_t)value);
}

#ifdef M4_TEST_MAIN
#include <stdio.h>
static uint8_t* slurp(const char *path, size_t *n){
    FILE *f=fopen(path,"rb"); if(!f){perror(path);exit(2);}
    fseek(f,0,SEEK_END); long s=ftell(f); fseek(f,0,SEEK_SET);
    uint8_t *b=malloc(s); fread(b,1,s,f); fclose(f); *n=s; return b;
}
int main(int argc, char **argv){
    if (argc != 6){ fprintf(stderr,"usage: %s dfpatch from.bin to_size out.fromzero out.dfdiff\n",argv[0]); return 2; }
    size_t dn, fn; uint8_t *df=slurp(argv[1],&dn), *fr=slurp(argv[2],&fn);
    size_t to_size=(size_t)strtoul(argv[3],NULL,10);
    uint8_t *fz=NULL,*dd=NULL;
    if (detools_m4_build(df,dn,fr,fn,to_size,&fz,&dd)){ fprintf(stderr,"build failed\n"); return 1; }
    FILE *a=fopen(argv[4],"wb"); fwrite(fz,1,fn,a); fclose(a);
    FILE *b=fopen(argv[5],"wb"); fwrite(dd,1,to_size,b); fclose(b);
    return 0;
}
#endif
