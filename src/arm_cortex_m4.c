/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 *
 * This module is a C reimplementation written after studying detools'
 * detools/data_format/arm_cortex_m4.py ARM Cortex-M data-format logic.
 */

/* ARM Cortex-M relocation field scanner and packers for the final A1 encoder. */
#include "enc_internal.h"

/* ---------- address->value maps ---------- */
typedef struct { m4_field_t *a; size_t n, cap; } map_t;

static void map_push(map_t *m, uint32_t addr, int32_t val) {
    m->a = (m4_field_t *)vec_reserve(m->a, &m->cap, m->n + 1, sizeof(m->a[0]), 256);
    m->a[m->n].addr = addr; m->a[m->n].val = val; m->n++;
}
static int kv_cmp(const void *x, const void *y) {
    uint32_t a = ((const m4_field_t*)x)->addr, b = ((const m4_field_t*)y)->addr;
    return (a > b) - (a < b);
}
/* sort by addr, drop duplicate addrs (keep last seen, mirroring dict overwrite) */
static void map_finalize(map_t *m) {
    if (m->n == 0) return;
    /* Duplicate addresses here carry equal values, so sort + unique-by-address
       preserves the scanner result. */
    a1_sort(m->a, m->n, sizeof(*m->a), kv_cmp);
    size_t w = 0;
    for (size_t i = 0; i < m->n; i++) {
        if (w > 0 && m->a[w-1].addr == m->a[i].addr) m->a[w-1] = m->a[i];
        else m->a[w++] = m->a[i];
    }
    m->n = w;
}
static void map_free(map_t *m){ free(m->a); m->a=NULL; m->n=m->cap=0; }

/* ---------- literal-pool address bitset (host-only; one bit per 4-byte word) ---------- */
typedef struct { uint8_t *bits; size_t nbits; } litset_t;
static void litset_init(litset_t *s, size_t fsize) {
    s->nbits = (fsize + 3u) >> 2;
    s->bits = (uint8_t *)xcalloc((s->nbits + 7u) >> 3, 1);
}
static void litset_add(litset_t *s, uint32_t k) {
    size_t i = (size_t)(k >> 2);
    if (s->bits && i < s->nbits) s->bits[i >> 3] |= (uint8_t)(1u << (i & 7u));
}
static int litset_has(const litset_t *s, uint32_t k) {
    size_t i = (size_t)(k >> 2);
    return !(k & 3u) && s->bits && i < s->nbits && (s->bits[i >> 3] & (uint8_t)(1u << (i & 7u)));
}
static void litset_free(litset_t *s){ free(s->bits); s->bits=NULL; s->nbits=0; }

/* ---------- disassemble (port of arm_cortex_m4.disassemble) ---------- */
typedef struct {
    map_t bl, ldr;
    litset_t lit;   /* every literal target addr, skipped later in the same scan */
} dis_t;

static void ldr_common(const uint8_t *f, size_t fsize, uint32_t address, uint32_t imm,
                       map_t *ldr, litset_t *lit) {
    if ((address % 4) == 2) address -= 2;
    address += imm;
    if ((size_t)address + 4 > fsize) return;
    int32_t v = (int32_t)rc_u32le(f + address);
    map_push(ldr, address, v);
    litset_add(lit, address);
}

/* The scanner records literal-pool target addresses as it finds them, then skips
   those words later in the same pass. */

static void disassemble(const uint8_t *f, size_t fsize,
                        uint32_t data_off_begin, uint32_t data_off_end,
                        dis_t *d) {
    memset(d, 0, sizeof(*d));
    litset_init(&d->lit, fsize);
    uint32_t addr = 0;
    while (addr < fsize) {
        if (data_off_begin <= addr && addr < data_off_end) {
            if ((size_t)addr + 4 > fsize) break;
            addr += 4;
        } else if (litset_has(&d->lit, addr)) {
            addr += 4;
        } else {
            if ((size_t)addr + 2 > fsize) break;
            uint16_t up = rc_u16le(f + addr);
            uint32_t ins = addr;
            addr += 2;
            if ((up & 0xf800) == 0xf000) {            /* bl / b.w */
                if ((size_t)addr + 2 > fsize) continue;
                uint16_t lo = rc_u16le(f + addr);
                if ((lo & 0xd000) == 0xd000) { addr += 2; map_push(&d->bl, ins, rc_bl_imm24s(up, lo)); }
            } else if (rc_thumb_ldr_lit(up)) {        /* ldr (literal) */
                ldr_common(f, fsize, ins, 4 * (up & 0xff) + 4, &d->ldr, &d->lit);
            }
        }
    }
    litset_free(&d->lit);   /* only needed during the scan */
    map_finalize(&d->bl); map_finalize(&d->ldr);
}

void a1_m4_disassemble(const uint8_t *from, size_t from_size,
                       uint32_t data_offset, uint32_t data_begin, uint32_t data_end,
                       m4_stream_t streams[M4_NSTREAMS]) {
    dis_t d;
    uint32_t data_off_end = data_offset + (data_end - data_begin);
    disassemble(from, from_size, data_offset, data_off_end, &d);
    map_t *src[M4_NSTREAMS] = { &d.bl, &d.ldr };
    for (int s = 0; s < M4_NSTREAMS; s++) { streams[s].a = NULL; streams[s].n = 0; }
    for (int s = 0; s < M4_NSTREAMS; s++) {
        streams[s].a = src[s]->a;
        streams[s].n = src[s]->n;
        src[s]->a = NULL; src[s]->n = src[s]->cap = 0;
    }
    map_free(&d.bl); map_free(&d.ldr);
}
void a1_m4_free_streams(m4_stream_t streams[M4_NSTREAMS]) {
    for (int s = 0; s < M4_NSTREAMS; s++) { free(streams[s].a); streams[s].a = NULL; streams[s].n = 0; }
}
