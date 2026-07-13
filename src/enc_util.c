#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 *
 * Host encoder module -- util/IO: die/allocators, Buf, transactional file IO, crc32, varints, vector + compare helpers.
 * Compiled as a normal internal encoder translation unit.
 */

#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "enc_internal.h"
/* noreturn: an allocation or parse failure terminates the process, so the allocator wrappers
 * below never return NULL. */
void die(const char *msg) {
    fprintf(stderr, "ultrapatch: %s\n", msg);
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

void *vec_reserve(void *p, size_t *cap, size_t need, size_t elem_size, size_t init_cap) {
    if (need <= *cap) return p;
    size_t nc = *cap;
    if (nc) {
        if (nc > SIZE_MAX / 2u) die("allocation too large");
        nc *= 2u;
    } else {
        nc = init_cap;
    }
    if (!nc) nc = 1;
    while (nc < need) {
        if (nc > SIZE_MAX / 2u) die("allocation too large");
        nc *= 2u;
    }
    if (elem_size && nc > SIZE_MAX / elem_size) die("allocation too large");
    *cap = nc;
    void *q = realloc(p, nc * elem_size);
    if (!q) die("out of memory");
    return q;
}

static void buf_reserve(Buf *b, size_t need) {
    b->d = (uint8_t *)vec_reserve(b->d, &b->cap, need, sizeof(b->d[0]), 256);
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

void opvec_free(OpVec *v) {
    if (!v) return;
    free(v->v); free(v->payload);
    v->v = NULL;
    v->payload = NULL;
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

OpWalkEnt *opwalk_build(const OpVec *ops, int32_t fp_start) {
    OpWalkEnt *w = (OpWalkEnt *)xmalloc((ops->n ? ops->n : 1) * sizeof(*w));
    int32_t tp = 0, fp = fp_start;
    for (size_t i = 0; i < ops->n; i++) {
        w[i] = (OpWalkEnt){tp, fp, &ops->v[i]};
        tp += ops->v[i].diff_len + ops->v[i].extra_len;
        fp += ops->v[i].diff_len + ops->v[i].adj;
    }
    return w;
}

int read_file_buf(const char *path, Buf *out, uint64_t max_size) {
    FILE *f = fopen(path, "rb");
    Buf b = {0};
    if (!f) { perror(path); return 2; }
    if (fseek(f, 0, SEEK_END)) { perror(path); goto fail; }
    long sz = ftell(f);
    if (sz < 0) { perror(path); goto fail; }
    if ((uint64_t)sz > SIZE_MAX || (max_size && (uint64_t)sz > max_size)) {
        fprintf(stderr, "%s: file too large\n", path);
        goto fail;
    }
    if (fseek(f, 0, SEEK_SET)) { perror(path); goto fail; }
    size_t alloc = sz ? (size_t)sz : 1u;     /* never leave b.d NULL: an empty image is a valid
                                              * input, and from->d/to->d flow straight into memcpy/
                                              * memcmp whose args are declared nonnull (0-len UB) */
    b.d = (uint8_t *)malloc(alloc);
    if (!b.d) { fprintf(stderr, "out of memory\n"); goto fail; }
    b.n = (size_t)sz;
    b.cap = alloc;
    if (b.n && fread(b.d, 1, b.n, f) != b.n) {
        if (ferror(f)) perror(path);
        else fprintf(stderr, "%s: short read\n", path);
        goto fail;
    }
    if (fclose(f)) { perror(path); buf_free(&b); return 2; }
    *out = b;
    return 0;
fail:
    if (fclose(f)) perror(path);
    buf_free(&b);
    return 2;
}

Buf slurp(const char *path) {
    Buf b = {0};
    /* 1 GiB ceiling: bsdiff_ops casts image sizes to int32_t for the suffix array and every
     * signed cursor, and the wire size fields are u32 — a >=2 GiB image would go negative. */
    if (read_file_buf(path, &b, 1u << 30)) exit(2);
    return b;
}

int file_alias(const char *a, const char *b) {
    struct stat sa, sb;
    if (stat(a, &sa)) {
        if (errno == ENOENT || errno == ENOTDIR) return 0;
        perror(a);
        return -1;
    }
    if (stat(b, &sb)) {
        perror(b);
        return -1;
    }
    return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

int replace_file(const char *path, const void *p, size_t n) {
    static const char suffix[] = ".tmp.XXXXXX";
    struct stat st;
    mode_t mode;
    size_t path_n = strlen(path);
    char *tmp;
    int fd = -1, rc = 2;

    if (path_n > SIZE_MAX - sizeof(suffix)) {
        fprintf(stderr, "%s: path too long\n", path);
        return 2;
    }
    tmp = (char *)malloc(path_n + sizeof(suffix));
    if (!tmp) {
        fprintf(stderr, "out of memory\n");
        return 2;
    }
    memcpy(tmp, path, path_n);
    memcpy(tmp + path_n, suffix, sizeof(suffix));

    if (stat(path, &st) == 0) {
        mode = st.st_mode & 07777u;
    } else if (errno == ENOENT || errno == ENOTDIR) {
        mode_t mask = umask(0);
        (void)umask(mask);
        mode = 0666u & ~mask;
    } else {
        perror(path);
        goto out;
    }
    fd = mkstemp(tmp);
    if (fd < 0) {
        perror(path);
        goto out;
    }
    {
        const uint8_t *d = (const uint8_t *)p;
        size_t off = 0;
        while (off < n) {
            ssize_t wrote = write(fd, d + off, n - off);
            if (wrote < 0 && errno == EINTR) continue;
            if (wrote <= 0) {
                if (wrote == 0) errno = EIO;
                perror(path);
                goto out;
            }
            off += (size_t)wrote;
        }
    }
    if (fchmod(fd, mode) || fsync(fd)) {
        perror(path);
        goto out;
    }
    if (close(fd)) {
        fd = -1;
        perror(path);
        goto out;
    }
    fd = -1;
    if (rename(tmp, path)) {
        perror(path);
        goto out;
    }
    rc = 0;
out:
    if (fd >= 0) (void)close(fd);
    if (rc) (void)unlink(tmp);
    free(tmp);
    return rc;
}

void write_file(const char *path, const void *p, size_t n) {
    if (replace_file(path, p, n)) exit(2);
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
    v->v = (int32_t *)vec_reserve(v->v, &v->cap, v->n + 1, sizeof(v->v[0]), 16);
    v->v[v->n++] = x;
}

void corr_push(CorrVec *v, int32_t off, uint8_t byte) {
    v->v = (CorrEnt *)vec_reserve(v->v, &v->cap, v->n + 1, sizeof(v->v[0]), 16);
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
    v->v = (Op *)vec_reserve(v->v, &v->cap, v->n + 1, sizeof(v->v[0]), 64);
    v->v[v->n++] = o;
}

static int cmp_fd(const void *a, const void *b) {
    const FieldDelta *x = (const FieldDelta *)a, *y = (const FieldDelta *)b;
    if (x->addr != y->addr) return (x->addr > y->addr) - (x->addr < y->addr);
    if (x->kind != y->kind) return (x->kind > y->kind) - (x->kind < y->kind);
    return (x->ord > y->ord) - (x->ord < y->ord);
}

void fd_put(FieldDeltaVec *v, uint32_t addr, int kind, int32_t delta) {
    v->v = (FieldDelta *)vec_reserve(v->v, &v->cap, v->n + 1, sizeof(v->v[0]), 256);
    v->v[v->n].addr = addr;
    v->v[v->n].kind = kind;
    v->v[v->n].delta = delta;
    v->v[v->n].ord = (uint32_t)v->n;
    v->n++;
}

void fd_finalize(FieldDeltaVec *v) {
    if (!v->n) return;
    qsort(v->v, v->n, sizeof(v->v[0]), cmp_fd);
    size_t w = 0;
    for (size_t i = 0; i < v->n; i++) {
        if (w && v->v[w - 1].addr == v->v[i].addr && v->v[w - 1].kind == v->v[i].kind) {
            v->v[w - 1] = v->v[i];
            v->v[w - 1].ord = (uint32_t)(w - 1);
        } else {
            v->v[w] = v->v[i];
            v->v[w].ord = (uint32_t)w;
            w++;
        }
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
