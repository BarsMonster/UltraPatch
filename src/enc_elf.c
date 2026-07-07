/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 *
 * A1 host encoder module -- minimal ELF32 range extraction (elf_ranges).
 * Compiled as a normal internal encoder translation unit.
 */

#include "enc_internal.h"
/* ------------------------------------------------------------------------------------- */
/* Minimal ELF32 little-endian range extraction for the firmware images.                  */
/* ------------------------------------------------------------------------------------- */
typedef struct { uint32_t type, off, addr, size, entsize; } Shdr;
typedef struct { uint32_t begin, end, sec; } Range;

static uint16_t rd16le(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32le(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static void range_add(Range *r, uint32_t begin, uint32_t size, uint32_t sec) {
    uint32_t end = begin + size;
    if (end < begin) return;
    if (r->begin == UINT32_MAX || begin < r->begin) r->begin = begin;
    if (end > r->end) r->end = end;
    r->sec = sec;
}

static void best_range(Range *best, Range r) {
    if (r.begin != UINT32_MAX && r.end > r.begin && r.end - r.begin > best->end - best->begin) *best = r;
}

static Range trim_data_range(Range r, Range code) {
    if (r.begin < code.begin && code.begin < r.end) {
        Range left = { r.begin, code.begin, r.sec };
        if (r.begin < code.end && code.end < r.end) {
            Range right = { code.end, r.end, r.sec };
            r = ((left.end - left.begin) > (right.end - right.begin)) ? left : right;
        } else {
            r = left;
        }
    } else if (r.begin < code.end && code.end < r.end) {
        r.begin = code.end;
    }
    return r;
}

static uint32_t data_offset_in_bin(const Buf *elf, const Buf *bin, uint32_t phoff,
                                   uint16_t phentsize, uint16_t phnum, Range data,
                                   const char *which) {
    if (data.end <= data.begin) return 0;
    uint64_t base = UINT64_MAX;
    for (uint16_t i = 0; i < phnum; i++) {
        const uint8_t *p = elf->d + phoff + (uint32_t)i * phentsize;
        if (rd32le(p) == 1 && rd32le(p + 16) && rd32le(p + 12) < base) base = rd32le(p + 12);
    }
    if (base == UINT64_MAX) die("ELF has no loadable data");
    for (uint16_t i = 0; i < phnum; i++) {
        const uint8_t *p = elf->d + phoff + (uint32_t)i * phentsize;
        if (rd32le(p) != 1) continue;                 /* PT_LOAD */
        uint32_t vaddr = rd32le(p + 8), paddr = rd32le(p + 12), filesz = rd32le(p + 16);
        if (vaddr <= data.begin && (uint64_t)data.end <= (uint64_t)vaddr + filesz) {
            uint64_t off = (uint64_t)paddr - base + (data.begin - vaddr);
            if (off + (data.end - data.begin) <= bin->n) return (uint32_t)off;
            break;
        }
    }
    fprintf(stderr, "patch_generate: data segment for %s outside bin load image\n", which);
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
    uint32_t phoff = rd32le(e.d + 28), shoff = rd32le(e.d + 32);
    uint16_t phentsize = rd16le(e.d + 42), phnum = rd16le(e.d + 44);
    uint16_t shentsize = rd16le(e.d + 46), shnum = rd16le(e.d + 48);
    if (!phoff || !phnum || phentsize < 32 || (uint64_t)phoff + (uint64_t)phentsize * phnum > e.n) die("bad ELF program headers");
    if (!shoff || !shnum || shentsize < 40 || (uint64_t)shoff + (uint64_t)shentsize * shnum > e.n) die("bad ELF sections");
    Shdr *sh = (Shdr *)xcalloc(shnum, sizeof(*sh));
    for (uint16_t i = 0; i < shnum; i++) {
        const uint8_t *p = e.d + shoff + (uint32_t)i * shentsize;
        sh[i].type = rd32le(p + 4);
        sh[i].addr = rd32le(p + 12);
        sh[i].off = rd32le(p + 16);
        sh[i].size = rd32le(p + 20);
        sh[i].entsize = rd32le(p + 36);
    }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
    Range *code_by_sec = (Range *)xcalloc(shnum, sizeof(*code_by_sec));
    Range *data_by_sec = (Range *)xcalloc(shnum, sizeof(*data_by_sec));
    for (uint16_t si = 0; si < shnum; si++) code_by_sec[si].begin = data_by_sec[si].begin = UINT32_MAX;
    for (uint16_t si = 0; si < shnum; si++) {
        if (sh[si].type != 2 || sh[si].entsize < 16 ||
            (uint64_t)sh[si].off + sh[si].size > e.n) continue;
        size_t n = sh[si].size / sh[si].entsize;
        for (size_t k = 0; k < n; k++) {
            const uint8_t *p = e.d + sh[si].off + k * sh[si].entsize;
            uint32_t value = rd32le(p + 4), size = rd32le(p + 8);
            uint8_t type = p[12] & 0x0f;
            if ((type != 1 && type != 2) || size == 0) continue;
            uint16_t sec = rd16le(p + 14);
            if (sec >= shnum || value < sh[sec].addr || (uint64_t)value >= (uint64_t)sh[sec].addr + sh[sec].size) continue;
            range_add(type == 2 ? &code_by_sec[sec] : &data_by_sec[sec], value, size, sec);
        }
    }
    Range code = {0, 0, 0}, data = {0, 0, 0};
    for (uint16_t si = 0; si < shnum; si++) best_range(&code, code_by_sec[si]);
    for (uint16_t si = 0; si < shnum; si++) best_range(&data, trim_data_range(data_by_sec[si], code));
    uint32_t doff = data_offset_in_bin(&e, bin, phoff, phentsize, phnum, data, which);
    Ranges r = { doff, data.begin, data.end, code.begin, code.end };
    free(sh); free(code_by_sec); free(data_by_sec); buf_free(&e);
    return r;
}
