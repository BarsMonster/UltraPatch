/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 *
 * Host encoder module -- minimal ELF32 range extraction (elf_ranges).
 * Compiled as a normal internal encoder translation unit.
 */

#include "enc_internal.h"
/* ------------------------------------------------------------------------------------- */
/* Minimal ELF32 little-endian range extraction for the firmware images.                  */
/* ------------------------------------------------------------------------------------- */
typedef struct { uint32_t begin, end; } Range;
typedef struct { uint32_t type, flags, off, addr, size, entsize; Range code, data; } Sec;

static void range_add(Range *r, uint32_t begin, uint32_t size) {
    uint32_t end = begin + size;
    if (end < begin) return;
    if (r->begin == UINT32_MAX || begin < r->begin) r->begin = begin;
    if (end > r->end) r->end = end;
}

static Range trim_data_range(Range r, Range code) {
    if (r.begin < code.begin && code.begin < r.end) {
        Range left = { r.begin, code.begin };
        if (r.begin < code.end && code.end < r.end) {
            Range right = { code.end, r.end };
            r = ((left.end - left.begin) > (right.end - right.begin)) ? left : right;
        } else {
            r = left;
        }
    } else if (r.begin < code.end && code.end < r.end) {
        r.begin = code.end;
    }
    return r;
}

static uint64_t load_base(const Buf *elf, uint32_t phoff, uint16_t phentsize, uint16_t phnum) {
    uint64_t base = UINT64_MAX;
    for (uint16_t i = 0; i < phnum; i++) {
        const uint8_t *p = elf->d + phoff + (uint32_t)i * phentsize;
        if (rc_u32le(p) == 1 && rc_u32le(p + 16) && rc_u32le(p + 12) < base) base = rc_u32le(p + 12);
    }
    return base;
}

static int range_offset_in_bin(const Buf *elf, const Buf *bin, uint32_t phoff,
                               uint16_t phentsize, uint16_t phnum, uint64_t base,
                               const Sec *sec, Range r, uint32_t *out) {
    if (r.end <= r.begin || r.begin < sec->addr ||
        (uint64_t)r.end > (uint64_t)sec->addr + sec->size) return 0;
    uint64_t sec_file = (uint64_t)sec->off + (r.begin - sec->addr);
    for (uint16_t i = 0; i < phnum; i++) {
        const uint8_t *p = elf->d + phoff + (uint32_t)i * phentsize;
        if (rc_u32le(p) != 1) continue;                 /* PT_LOAD */
        uint32_t poff = rc_u32le(p + 4), vaddr = rc_u32le(p + 8);
        uint32_t paddr = rc_u32le(p + 12), filesz = rc_u32le(p + 16);
        if ((uint64_t)paddr < base) continue;
        uint64_t bin_seg = (uint64_t)paddr - base;
        if ((uint64_t)poff + filesz > elf->n || bin_seg + filesz > bin->n ||
            vaddr > r.begin || (uint64_t)r.end > (uint64_t)vaddr + filesz ||
            (uint64_t)poff + (r.begin - vaddr) != sec_file) continue;
        *out = (uint32_t)(bin_seg + (r.begin - vaddr));
        return 1;
    }
    return 0;
}

Ranges elf_ranges(const char *elf_path, const Buf *bin, const char *which) {
    Buf e = slurp(elf_path);
    if (e.n < 52 || memcmp(e.d, "\177" "ELF", 4) || e.d[4] != 1 || e.d[5] != 1) die("expected ELF32 little-endian");
    uint32_t phoff = rc_u32le(e.d + 28), shoff = rc_u32le(e.d + 32);
    uint16_t phentsize = rc_u16le(e.d + 42), phnum = rc_u16le(e.d + 44);
    uint16_t shentsize = rc_u16le(e.d + 46), shnum = rc_u16le(e.d + 48);
    if (phnum && (!phoff || phentsize < 32 || (uint64_t)phoff + (uint64_t)phentsize * phnum > e.n)) die("bad ELF program headers");
    if (shnum && (!shoff || shentsize < 40 || (uint64_t)shoff + (uint64_t)shentsize * shnum > e.n)) die("bad ELF sections");
    if (!phnum || !shnum) { buf_free(&e); return (Ranges){0, 0}; }
    Sec *sec = (Sec *)xcalloc(shnum, sizeof(*sec));
    for (uint16_t i = 0; i < shnum; i++) {
        const uint8_t *p = e.d + shoff + (uint32_t)i * shentsize;
        sec[i].type = rc_u32le(p + 4);
        sec[i].flags = rc_u32le(p + 8);
        sec[i].addr = rc_u32le(p + 12);
        sec[i].off = rc_u32le(p + 16);
        sec[i].size = rc_u32le(p + 20);
        sec[i].entsize = rc_u32le(p + 36);
        sec[i].code.begin = sec[i].data.begin = UINT32_MAX;
    }
    for (uint16_t si = 0; si < shnum; si++) {
        if (sec[si].type != 2 || sec[si].entsize < 16 ||
            (uint64_t)sec[si].off + sec[si].size > e.n) continue;
        size_t n = sec[si].size / sec[si].entsize;
        for (size_t k = 0; k < n; k++) {
            const uint8_t *p = e.d + sec[si].off + k * sec[si].entsize;
            uint32_t value = rc_u32le(p + 4), size = rc_u32le(p + 8);
            uint8_t type = p[12] & 0x0f;
            if ((type != 1 && type != 2) || size == 0) continue;
            uint16_t dst = rc_u16le(p + 14);
            if (type == 2) value &= ~1u;                /* ELF32 ARM Thumb function marker */
            if (dst >= shnum || !(sec[dst].flags & 2u) || sec[dst].type == 8 ||
                (uint64_t)sec[dst].off + sec[dst].size > e.n || value < sec[dst].addr ||
                (uint64_t)value + size > (uint64_t)sec[dst].addr + sec[dst].size) continue;
            range_add(type == 2 ? &sec[dst].code : &sec[dst].data, value, size);
        }
    }
    Range code = {0, 0}, data = {0, 0};
    uint32_t doff = 0, off;
    uint64_t base = load_base(&e, phoff, phentsize, phnum);
    if (base == UINT64_MAX) { free(sec); buf_free(&e); return (Ranges){0, 0}; }
    for (uint16_t si = 0; si < shnum; si++) {
        Range r = sec[si].code;
        if (range_offset_in_bin(&e, bin, phoff, phentsize, phnum, base, &sec[si], r, &off) &&
            r.end - r.begin > code.end - code.begin) code = r;
    }
    for (uint16_t si = 0; si < shnum; si++) {
        Range r = trim_data_range(sec[si].data, code);
        if (range_offset_in_bin(&e, bin, phoff, phentsize, phnum, base, &sec[si], r, &off) &&
            r.end - r.begin > data.end - data.begin) { data = r; doff = off; }
    }
    Ranges r = { doff, doff + (data.end - data.begin) };
    (void)which;
    free(sec); buf_free(&e);
    return r;
}
