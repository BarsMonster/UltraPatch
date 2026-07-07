/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 *
 * This module is a C reimplementation written after studying detools'
 * detools/data_format/arm_cortex_m4.py ARM Cortex-M data-format logic.
 */

/* ARM Cortex-M relocation field scanner and packers for the final A1 encoder. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "arm_cortex_m4.h"
#include "rc_models.h"   /* rc_bl_imm24 / rc_bl_pack: single-sourced BL unpack/pack (decoder mirror) */

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
 * Power-of-two table, open addressing, grown on 0.75 load. Sized to the number of
 * literals (dynamic). */
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

/* ---------- disassemble (port of arm_cortex_m4.disassemble) ---------- */
typedef struct {
    map_t bl, ldr, data_ptr, code_ptr;
    uset_t lit;   /* every literal target addr — for O(1) literal-pool skip during the scan */
} dis_t;

static void ldr_common(const uint8_t *f, size_t fsize, uint32_t address, uint32_t imm,
                       map_t *ldr, uset_t *lit) {
    if ((address % 4) == 2) address -= 2;
    address += imm;
    if ((size_t)address + 4 > fsize) return;
    int32_t v = (int32_t)rc_u32le(f + address);
    if (map_push(ldr, address, v) == 0) uset_add(lit, address);
}

/* The scanner records literal-pool target addresses as it finds them, then skips
   those words later in the same pass. */

static void disassemble(const uint8_t *f, size_t fsize,
                        uint32_t data_off_begin, uint32_t data_off_end,
                        uint32_t data_begin, uint32_t data_end,
                        uint32_t code_begin, uint32_t code_end,
                        dis_t *d) {
    memset(d, 0, sizeof(*d));
    uint32_t addr = 0;
    while (addr < fsize) {
        if (data_off_begin <= addr && addr < data_off_end) {
            if ((size_t)addr + 4 > fsize) break;
            uint32_t value = rc_u32le(f + addr);
            if (data_begin <= value && value < data_end) map_push(&d->data_ptr, addr, value);
            else if (code_begin <= value && value < code_end) map_push(&d->code_ptr, addr, value);
            addr += 4;
        } else if (uset_has(&d->lit, addr)) {
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
    uset_free(&d->lit);   /* only needed during the scan */
    map_finalize(&d->bl); map_finalize(&d->ldr);
    map_finalize(&d->data_ptr); map_finalize(&d->code_ptr);
}

int a1_m4_disassemble(const uint8_t *from, size_t from_size,
                      uint32_t data_offset, uint32_t data_begin, uint32_t data_end,
                      uint32_t code_begin, uint32_t code_end,
                      m4_stream_t streams[M4_NSTREAMS]) {
    dis_t d;
    uint32_t data_off_end = data_offset + (data_end - data_begin);
    disassemble(from, from_size, data_offset, data_off_end, data_begin, data_end,
                code_begin, code_end, &d);
    map_t *src[M4_NSTREAMS] = { &d.data_ptr, &d.code_ptr, &d.bl, &d.ldr };
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
    map_free(&d.bl); map_free(&d.ldr);
    map_free(&d.data_ptr); map_free(&d.code_ptr);
    return rc;
}
void a1_m4_free_streams(m4_stream_t streams[M4_NSTREAMS]) {
    for (int s = 0; s < M4_NSTREAMS; s++) { free(streams[s].a); streams[s].a = NULL; streams[s].n = 0; }
}
