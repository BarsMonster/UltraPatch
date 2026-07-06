/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 *
 * A1 host encoder module -- minimal ELF32 range extraction (elf_ranges, largest_code_range/largest_data_range, find_data_offset_in_bin).
 * Compiled as a normal internal encoder translation unit.
 */

#include "enc_internal.h"
/* ------------------------------------------------------------------------------------- */
/* Minimal ELF32 little-endian range extraction for the firmware images.                  */
/* ------------------------------------------------------------------------------------- */
typedef struct { uint32_t type, off, addr, size, entsize, link; } Shdr;
typedef struct { uint32_t value, size; uint8_t type; uint16_t sec; } Sym;
typedef struct { uint32_t begin, end, sec; } ARange;
typedef struct { Sym *v; size_t n, cap; } SymVec;
typedef struct { ARange *v; size_t n, cap; } RangeVec;

static uint16_t rd16le(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32le(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static int cmp_sym_val(const void *a, const void *b) {
    const Sym *x = (const Sym *)a, *y = (const Sym *)b;
    if (x->sec != y->sec) return (x->sec > y->sec) - (x->sec < y->sec);
    return (x->value > y->value) - (x->value < y->value);
}

static void sym_push(SymVec *v, Sym s) {
    v->v = (Sym *)vec_reserve(v->v, &v->cap, v->n + 1, sizeof(v->v[0]), 1024);
    v->v[v->n++] = s;
}

static void range_push(RangeVec *v, uint32_t begin, uint32_t end, uint32_t sec) {
    if (end <= begin) return;
    v->v = (ARange *)vec_reserve(v->v, &v->cap, v->n + 1, sizeof(v->v[0]), 128);
    v->v[v->n++] = (ARange){ begin, end, sec };
}

static int section_for_addr(const Shdr *sh, size_t n, uint32_t addr) {
    for (size_t i = 0; i < n; i++)
        if (sh[i].addr <= addr && addr < sh[i].addr + sh[i].size)
            return (int)i;
    return -1;
}

static ARange largest_code_range(const RangeVec *funcs) {
    ARange best = {0, 0, 0};
    size_t i = 0;
    while (i < funcs->n) {
        uint32_t sec = funcs->v[i].sec;
        ARange r = { funcs->v[i].begin, funcs->v[i].end, sec };
        while (i + 1 < funcs->n && funcs->v[i + 1].sec == sec) {
            i++;
            r.end = funcs->v[i].end;
        }
        if (r.end - r.begin > best.end - best.begin) best = r;
        i++;
    }
    return best;
}

static ARange largest_data_range(const RangeVec *objs, ARange code) {
    ARange best = {0, 0, 0};
    for (size_t i = 0; i < objs->n; i++) {
        ARange r = objs->v[i];
        if (r.begin < code.begin && code.begin < r.end) {
            ARange left = { r.begin, code.begin, r.sec };
            if (r.begin < code.end && code.end < r.end) {
                ARange right = { code.end, r.end, r.sec };
                r = ((left.end - left.begin) > (right.end - right.begin)) ? left : right;
            } else {
                r = left;
            }
        } else if (r.begin < code.end && code.end < r.end) {
            r.begin = code.end;
        }
        if (r.end > r.begin && r.end - r.begin > best.end - best.begin) best = r;
    }
    return best;
}

static uint32_t find_data_offset_in_bin(const Buf *elf, const Shdr *sh, const Buf *bin, ARange data, const char *which) {
    if (data.end <= data.begin) return 0;
    if (data.sec == UINT32_MAX || data.sec >= 65535) die("bad ELF data section");
    const Shdr *s = &sh[data.sec];
    uint32_t in_sec = data.begin - s->addr;
    if ((uint64_t)s->off + in_sec + (data.end - data.begin) > elf->n) die("ELF data range outside file");
    const uint8_t *needle = elf->d + s->off + in_sec;
    size_t nlen = data.end - data.begin;
    for (size_t i = 0; i + nlen <= bin->n; i++)
        if (memcmp(bin->d + i, needle, nlen) == 0)
            return (uint32_t)i;
    fprintf(stderr, "patch_generate: data segment for %s not found in bin\n", which);
    exit(2);
}

Ranges elf_ranges(const char *elf_path, const Buf *bin, const char *which) {
    Buf e = slurp(elf_path);
    /* -fanalyzer FALSE POSITIVES suppressed within this validated header/section-table parse only
     * (checkers stay active for the rest of the function): the analyzer cannot see through slurp's
     * allocator wrappers, so it models `e.d` as possibly-NULL at the memcmp (it is not: slurp always
     * allocates >=1 byte and the `e.n < 52` guard short-circuits before the memcmp); and it cannot
     * relate `sh = xcalloc(shnum, sizeof *sh)` to the `i < shnum` loop, so it reports the in-bounds
     * `sh[i]` field stores as heap overflow. Verified by construction + the ASan encoder-fuzz
     * campaign (hostile/truncated/oversized ELFs: clean die() or round-trip, zero ASan reports). */
#if defined(__GNUC__) && !defined(__clang__)   /* -Wanalyzer-* is gcc-only; clang -Werror rejects it */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-null-argument"
#pragma GCC diagnostic ignored "-Wanalyzer-out-of-bounds"
#endif
    if (e.n < 52 || memcmp(e.d, "\177" "ELF", 4) || e.d[4] != 1 || e.d[5] != 1) die("expected ELF32 little-endian");
    uint32_t shoff = rd32le(e.d + 32);
    uint16_t shentsize = rd16le(e.d + 46), shnum = rd16le(e.d + 48);
    if (!shoff || !shnum || shentsize < 40 || (uint64_t)shoff + (uint64_t)shentsize * shnum > e.n) die("bad ELF sections");
    Shdr *sh = (Shdr *)xcalloc(shnum, sizeof(*sh));
    for (uint16_t i = 0; i < shnum; i++) {
        const uint8_t *p = e.d + shoff + (uint32_t)i * shentsize;
        sh[i].type = rd32le(p + 4);
        sh[i].addr = rd32le(p + 12);
        sh[i].off = rd32le(p + 16);
        sh[i].size = rd32le(p + 20);
        sh[i].link = rd32le(p + 24);
        sh[i].entsize = rd32le(p + 36);
    }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
    SymVec syms = {0};
    for (uint16_t si = 0; si < shnum; si++) {
        if (sh[si].type != 2 || sh[si].entsize < 16 ||
            (uint64_t)sh[si].off + sh[si].size > e.n) continue;
        size_t n = sh[si].size / sh[si].entsize;
        for (size_t k = 0; k < n; k++) {
            const uint8_t *p = e.d + sh[si].off + k * sh[si].entsize;
            uint32_t value = rd32le(p + 4), size = rd32le(p + 8);
            uint8_t type = p[12] & 0x0f;
            if ((type != 1 && type != 2) || size == 0) continue;
            int sec = section_for_addr(sh, shnum, value);
            if (sec < 0) continue;
            sym_push(&syms, (Sym){ value, size, type, (uint16_t)sec });
        }
    }
    a1_sort(syms.v, syms.n, sizeof(syms.v[0]), cmp_sym_val);
    RangeVec funcs = {0}, objs = {0};
    size_t i = 0;
    while (i < syms.n) {
        uint16_t sec = syms.v[i].sec;
        size_t j = i;
        while (j < syms.n && syms.v[j].sec == sec) j++;
        uint8_t bt = syms.v[i].type;
        uint32_t bb = syms.v[i].value, be = syms.v[i].size;
        for (size_t k = i; k < j; k++) {
            if (syms.v[k].type != bt) {
                if (bt == 2) range_push(&funcs, bb, be, sec);
                else range_push(&objs, bb, be, sec);
                bt = syms.v[k].type;
                bb = syms.v[k].value;
            }
            be = syms.v[k].value + syms.v[k].size;
        }
        if (bt == 2) range_push(&funcs, bb, be, sec);
        else range_push(&objs, bb, be, sec);
        i = j;
    }
    ARange code = largest_code_range(&funcs);
    ARange data = largest_data_range(&objs, code);
    uint32_t doff = find_data_offset_in_bin(&e, sh, bin, data, which);
    Ranges r = { doff, doff + (data.end - data.begin), data.begin, data.end, code.begin, code.end };
    free(sh); free(syms.v); free(funcs.v); free(objs.v); buf_free(&e);
    return r;
}
