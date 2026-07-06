/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 *
 * A1 host encoder module -- util/IO: die/allocators, a1_sort stable mergesort, Buf, slurp/write_file, crc32, varints, vector + compare helpers.
 * Compiled as a normal internal encoder translation unit.
 */

#include "enc_internal.h"
/* noreturn: lets the reader AND -fanalyzer know an allocation/parse failure terminates the
 * process, so the `if (!p) die(...); return p;` allocator wrappers below provably never return
 * NULL (without this the analyzer models a NULL return and reports spurious null-arg/OOB). */
void die(const char *msg) {
    fprintf(stderr, "patch_generate: %s\n", msg);
    exit(2);
}

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) die("out of memory");
    return p;
}

void *xcalloc(size_t n, size_t s) {
    void *p = calloc(n ? n : 1, s ? s : 1);
    if (!p) die("out of memory");
    return p;
}

/* Deterministic STABLE bottom-up merge sort — replaces libc qsort everywhere in the
 * encoder so the emitted wire can never depend on the host libc's tie ordering (glibc
 * qsort is an unstable introsort since 2.37; musl uses smoothsort; BSDs differ). Several
 * comparators legitimately compare equal on distinct elements (same-address ELF symbols,
 * equal-value b2j entries, equal-boundary map segments); stability pins those ties to
 * insertion order, which every producer in this file generates deterministically. */
void a1_sort(void *base, size_t n, size_t esz,
                    int (*cmp)(const void *, const void *)) {
    unsigned char *src = (unsigned char *)base, *tmp;
    if (n < 2) return;
    tmp = (unsigned char *)xmalloc(n * esz);
    memcpy(tmp, src, n * esz);
    for (size_t w = 1; w < n; w *= 2) {
        for (size_t lo = 0; lo < n; lo += 2 * w) {
            size_t mid = lo + w < n ? lo + w : n;
            size_t hi = lo + 2 * w < n ? lo + 2 * w : n;
            size_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi) {
                if (cmp(src + j * esz, src + i * esz) < 0) {   /* strict <: ties keep LEFT */
                    memcpy(tmp + k * esz, src + j * esz, esz); j++;
                } else {
                    memcpy(tmp + k * esz, src + i * esz, esz); i++;
                }
                k++;
            }
            if (i < mid) { memcpy(tmp + k * esz, src + i * esz, (mid - i) * esz); k += mid - i; }
            if (j < hi) memcpy(tmp + k * esz, src + j * esz, (hi - j) * esz);
        }
        memcpy(src, tmp, n * esz);
    }
    free(tmp);
}

void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) die("out of memory");
    return q;
}

static void buf_reserve(Buf *b, size_t need) {
    if (need <= b->cap) return;
    size_t nc = b->cap ? b->cap * 2 : 256;
    while (nc < need) nc *= 2;
    b->d = (uint8_t *)xrealloc(b->d, nc);
    b->cap = nc;
}

void buf_put(Buf *b, uint8_t v) {
    buf_reserve(b, b->n + 1);
    b->d[b->n++] = v;
}

void buf_write(Buf *b, const void *p, size_t n) {
    buf_reserve(b, b->n + n);
    memcpy(b->d + b->n, p, n);
    b->n += n;
}

void buf_put_u32le(Buf *b, uint32_t v) {
    buf_put(b, (uint8_t)v);
    buf_put(b, (uint8_t)(v >> 8));
    buf_put(b, (uint8_t)(v >> 16));
    buf_put(b, (uint8_t)(v >> 24));
}

void buf_free(Buf *b) { free(b->d); b->d = NULL; b->n = b->cap = 0; }

void opvec_free_deep(OpVec *v) {
    if (!v) return;
    for (size_t i = 0; i < v->n; i++) {
        free(v->v[i].diff);
        free(v->v[i].extra);
    }
    free(v->v);
    v->v = NULL;
    v->n = v->cap = 0;
}

void oppc_array_free(OpPC *pc, size_t n) {
    if (!pc) return;
    for (size_t i = 0; i < n; i++) {
        free(pc[i].pres.v);
        free(pc[i].corr.v);
    }
    free(pc);
}

void blockvec_array_free(BlockVec blocks[STREAM_N]) {
    if (!blocks) return;
    for (int s = 0; s < STREAM_N; s++) {
        for (size_t i = 0; i < blocks[s].n; i++) free(blocks[s].v[i].values);
        free(blocks[s].v);
        blocks[s].v = NULL;
        blocks[s].n = blocks[s].cap = 0;
    }
}

OpWalkEnt *opwalk_build(const OpVec *ops) {
    OpWalkEnt *w = (OpWalkEnt *)xmalloc((ops->n ? ops->n : 1) * sizeof(*w));
    int32_t tp = 0, fp = 0;
    for (size_t i = 0; i < ops->n; i++) {
        w[i] = (OpWalkEnt){tp, fp, &ops->v[i], i};
        tp += ops->v[i].diff_len + ops->v[i].extra_len;
        fp += ops->v[i].diff_len + ops->v[i].adj;
    }
    return w;
}

Buf slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    if (fseek(f, 0, SEEK_END)) die("seek failed");
    long sz = ftell(f);
    if (sz < 0) die("tell failed");
    /* 1 GiB ceiling: bsdiff_ops casts image sizes to int32_t for the suffix array and every
     * signed cursor, and the wire size fields are u32 — a >=2 GiB image would go negative. */
    if ((uint64_t)sz > (1u << 30)) die("image too large");
    if (fseek(f, 0, SEEK_SET)) die("seek failed");
    Buf b = {0};
    buf_reserve(&b, sz ? (size_t)sz : 1);   /* never leave b.d NULL: an empty image is a valid
                                             * input, and from->d/to->d flow straight into memcpy/
                                             * memcmp whose args are declared nonnull (0-len UB) */
    b.n = (size_t)sz;
    if (b.n && fread(b.d, 1, b.n, f) != b.n) die("read failed");
    fclose(f);
    return b;
}

void write_file(const char *path, const void *p, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(2); }
    if (n && fwrite(p, 1, n, f) != n) die("write failed");
    fclose(f);
}

uint32_t crc32_buf(const uint8_t *p, size_t n) {
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & (uint32_t)-(int32_t)(c & 1u));
    }
    return c ^ 0xffffffffu;
}

int bitlen32(uint32_t v) {
    int n = 0;
    do { n++; v >>= 1; } while (v);
    return n;
}

void put_uleb(Buf *b, uint32_t v) {
    for (;;) {
        uint8_t x = (uint8_t)(v & 0x7fu);
        v >>= 7;
        buf_put(b, v ? (uint8_t)(x | 0x80u) : x);
        if (!v) break;
    }
}

/* uLEB with one redundant trailing continuation byte — value-identical, non-canonical.
 * The decoder reads the redundancy as a 1-bit flag (the unnatural apply direction). */
void put_uleb_overlong(Buf *b, uint32_t v) {
    for (;;) {
        uint8_t x = (uint8_t)(v & 0x7fu);
        v >>= 7;
        buf_put(b, (uint8_t)(x | 0x80u));
        if (!v) break;
    }
    buf_put(b, 0);
}

void ivec_push(IVec *v, int32_t x) {
    if (v->n == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 16;
        v->v = (int32_t *)xrealloc(v->v, v->cap * sizeof(v->v[0]));
    }
    v->v[v->n++] = x;
}

void corr_push(CorrVec *v, int32_t off, uint8_t byte) {
    if (v->n == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 16;
        v->v = (CorrEnt *)xrealloc(v->v, v->cap * sizeof(v->v[0]));
    }
    v->v[v->n].off = off;
    v->v[v->n].byte = byte;
    v->n++;
}

int cmp_i32(const void *a, const void *b) {
    int32_t x = *(const int32_t *)a, y = *(const int32_t *)b;
    return (x > y) - (x < y);
}

int cmp_corr(const void *a, const void *b) {
    const CorrEnt *x = (const CorrEnt *)a, *y = (const CorrEnt *)b;
    return (x->off > y->off) - (x->off < y->off);
}

void opvec_push(OpVec *v, Op o) {
    if (v->n == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 64;
        v->v = (Op *)xrealloc(v->v, v->cap * sizeof(v->v[0]));
    }
    v->v[v->n++] = o;
}

void blockvec_push(BlockVec *v, int32_t fo, int32_t ta, const int64_t *vals, int32_t n) {
    if (v->n == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 8;
        v->v = (Block *)xrealloc(v->v, v->cap * sizeof(v->v[0]));
    }
    Block *b = &v->v[v->n++];
    b->from_offset = fo;
    b->to_address = ta;
    b->n = n;
    b->values = (int64_t *)xmalloc((size_t)n * sizeof(int64_t));
    memcpy(b->values, vals, (size_t)n * sizeof(int64_t));
}

static int cmp_fd(const void *a, const void *b) {
    const FieldDelta *x = (const FieldDelta *)a, *y = (const FieldDelta *)b;
    if (x->addr != y->addr) return (x->addr > y->addr) - (x->addr < y->addr);
    return (x->kind > y->kind) - (x->kind < y->kind);
}

void fd_put(FieldDeltaVec *v, uint32_t addr, int kind, int64_t delta) {
    if (v->n == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 256;
        v->v = (FieldDelta *)xrealloc(v->v, v->cap * sizeof(v->v[0]));
    }
    v->v[v->n].addr = addr;
    v->v[v->n].kind = kind;
    v->v[v->n].delta = delta;
    v->n++;
}

void fd_finalize(FieldDeltaVec *v) {
    if (!v->n) return;
    a1_sort(v->v, v->n, sizeof(v->v[0]), cmp_fd);
    size_t w = 0;
    for (size_t i = 0; i < v->n; i++) {
        if (w && v->v[w - 1].addr == v->v[i].addr && v->v[w - 1].kind == v->v[i].kind) v->v[w - 1] = v->v[i];
        else v->v[w++] = v->v[i];
    }
    v->n = w;
}

const FieldDelta *fd_find_kind(const FieldDeltaVec *v, uint32_t addr, int kind) {
    size_t lo = 0, hi = v->n;
    while (lo < hi) {
        size_t m = (lo + hi) >> 1;
        if (v->v[m].addr < addr) lo = m + 1;
        else hi = m;
    }
    while (lo < v->n && v->v[lo].addr == addr) {
        if (v->v[lo].kind == kind) return &v->v[lo];
        lo++;
    }
    return NULL;
}
