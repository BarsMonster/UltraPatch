/* ARM Cortex-M relocation field scanner and packers for the final A1 encoder. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "arm_cortex_m4.h"

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
    /* Duplicate addresses here carry equal values, so sort + unique-by-address
       preserves the scanner result. */
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

/* The scanner records literal-pool target addresses as it finds them, then skips
   those words later in the same pass. */

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
    /* ldr_w is used for in-scan literal-pool membership even when it is not
     * returned to the caller. */
    if (!emit_ldr_w) d->ldr_w.n = 0;
    uset_free(&d->lit);   /* only needed during the scan */
    map_finalize(&d->bw); map_finalize(&d->bl);
    map_finalize(&d->ldr); map_finalize(&d->ldr_w);
    map_finalize(&d->data_ptr); map_finalize(&d->code_ptr);
}

int a1_m4_disassemble(const uint8_t *from, size_t from_size,
                      uint32_t data_offset, uint32_t data_begin, uint32_t data_end,
                      uint32_t code_begin, uint32_t code_end,
                      int emit_bw, int emit_ldr_w,
                      m4_stream_t streams[M4_NSTREAMS]) {
    dis_t d;
    uint32_t data_off_end = data_offset + (data_end - data_begin);
    disassemble(from, from_size, data_offset, data_off_end, data_begin, data_end,
                code_begin, code_end, emit_bw, emit_ldr_w, &d);
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
    if (rc) a1_m4_free_streams(streams);   /* release any partial allocation on OOM */
    map_free(&d.bw); map_free(&d.bl); map_free(&d.ldr); map_free(&d.ldr_w);
    map_free(&d.data_ptr); map_free(&d.code_ptr);
    return rc;
}
void a1_m4_free_streams(m4_stream_t streams[M4_NSTREAMS]) {
    for (int s = 0; s < M4_NSTREAMS; s++) { free(streams[s].a); streams[s].a = NULL; streams[s].n = 0; }
}
void a1_m4_pack(int pk, int32_t value, uint8_t out[4]) {
    if (pk == M4_PK_BL) pack_bl(value, out);
    else if (pk == M4_PK_BW) pack_bw(value, out);
    else wr32(out, (uint32_t)value);
}
