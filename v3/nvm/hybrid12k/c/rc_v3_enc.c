/*
 * A1 host encoder (C) for the rc_v3 decoder wire.
 *
 * This is intentionally a host-side encoder: compression-side memory/CPU are
 * allowed to be large. It emits the final A1 blob consumed by rc_v3.c.
 */
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arm_cortex_m4.h"
#include "rc_models.h"

int divsufsort(const uint8_t *T, int32_t *SA, int32_t n);

#ifndef PATHE_W
#define PATHE_W 10
#endif
#ifndef DR_KCAP_BL
#define DR_KCAP_BL 208
#endif
#ifndef DR_KCAP_EX
#define DR_KCAP_EX 128
#endif
#define DR_HIT_INIT 512u

enum { STREAM_DATA, STREAM_CODE, STREAM_BW, STREAM_BL, STREAM_LDR, STREAM_LDRW, STREAM_N };

typedef struct { uint8_t *d; size_t n, cap; } Buf;
typedef struct { int32_t diff_len, adj; uint8_t *diff; uint8_t *extra; int32_t extra_len; } Op;
typedef struct { Op *v; size_t n, cap; } OpVec;
typedef struct { int32_t from_offset, to_address, n; int64_t *values; } Block;
typedef struct { Block *v; size_t n, cap; } BlockVec;
typedef struct { uint32_t addr; int kind; int64_t delta; } FieldDelta;
typedef struct { FieldDelta *v; size_t n, cap; } FieldDeltaVec;
typedef struct { int32_t *v; size_t n, cap; } IVec;
typedef struct { int32_t off; uint8_t byte; } CorrEnt;
typedef struct { CorrEnt *v; size_t n, cap; } CorrVec;
typedef struct { IVec pres; CorrVec corr; } OpPC;
typedef struct { uint32_t data_off_begin, data_off_end, data_begin, data_end, code_begin, code_end; } Ranges;
typedef struct {
    size_t ops, diff_bytes, extra_bytes, literals, preserves, corrections;
    size_t bl_fields, ex_fields, suppressed_bl;
} EncStats;

static void die(const char *msg) {
    fprintf(stderr, "rc_v3_enc: %s\n", msg);
    exit(2);
}

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) die("out of memory");
    return p;
}

static void *xcalloc(size_t n, size_t s) {
    void *p = calloc(n ? n : 1, s ? s : 1);
    if (!p) die("out of memory");
    return p;
}

static void *xrealloc(void *p, size_t n) {
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

static void buf_put(Buf *b, uint8_t v) {
    buf_reserve(b, b->n + 1);
    b->d[b->n++] = v;
}

static void buf_write(Buf *b, const void *p, size_t n) {
    buf_reserve(b, b->n + n);
    memcpy(b->d + b->n, p, n);
    b->n += n;
}

static void buf_put_u32le(Buf *b, uint32_t v) {
    buf_put(b, (uint8_t)v);
    buf_put(b, (uint8_t)(v >> 8));
    buf_put(b, (uint8_t)(v >> 16));
    buf_put(b, (uint8_t)(v >> 24));
}

static void buf_free(Buf *b) { free(b->d); b->d = NULL; b->n = b->cap = 0; }

static Buf slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    if (fseek(f, 0, SEEK_END)) die("seek failed");
    long sz = ftell(f);
    if (sz < 0) die("tell failed");
    if (fseek(f, 0, SEEK_SET)) die("seek failed");
    Buf b = {0};
    buf_reserve(&b, (size_t)sz);
    b.n = (size_t)sz;
    if (b.n && fread(b.d, 1, b.n, f) != b.n) die("read failed");
    fclose(f);
    return b;
}

static void write_file(const char *path, const void *p, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(2); }
    if (n && fwrite(p, 1, n, f) != n) die("write failed");
    fclose(f);
}

static char *join2(const char *a, const char *b) {
    size_t na = strlen(a), nb = strlen(b);
    char *s = (char *)xmalloc(na + nb + 2);
    memcpy(s, a, na);
    s[na] = '/';
    memcpy(s + na + 1, b, nb + 1);
    return s;
}

static uint32_t crc32_buf(const uint8_t *p, size_t n) {
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & (uint32_t)-(int32_t)(c & 1u));
    }
    return c ^ 0xffffffffu;
}

static uint32_t zz32(int32_t v) { return v >= 0 ? ((uint32_t)v << 1) : (((uint32_t)(-v) << 1) - 1u); }

static int bitlen32(uint32_t v) {
    int n = 0;
    do { n++; v >>= 1; } while (v);
    return n;
}

static void put_uleb(Buf *b, uint32_t v) {
    for (;;) {
        uint8_t x = (uint8_t)(v & 0x7fu);
        v >>= 7;
        buf_put(b, v ? (uint8_t)(x | 0x80u) : x);
        if (!v) break;
    }
}

static int uleb_len(uint32_t v) {
    int n = 1;
    while (v >>= 7) n++;
    return n;
}

static void put_pack_size(Buf *b, int64_t value) {
    uint8_t tmp[10];
    int n = 0;
    if (value == 0) {
        tmp[n++] = 0;
    } else {
        uint64_t u;
        if (value > 0) {
            tmp[n] = 0;
            u = (uint64_t)value;
        } else {
            tmp[n] = 0x40;
            u = (uint64_t)(-value);
        }
        tmp[n++] |= (uint8_t)(0x80u | (u & 0x3fu));
        u >>= 6;
        while (u > 0) {
            tmp[n++] = (uint8_t)(0x80u | (u & 0x7fu));
            u >>= 7;
        }
        tmp[n - 1] &= 0x7fu;
    }
    buf_write(b, tmp, (size_t)n);
}

static void ivec_push(IVec *v, int32_t x) {
    if (v->n == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 16;
        v->v = (int32_t *)xrealloc(v->v, v->cap * sizeof(v->v[0]));
    }
    v->v[v->n++] = x;
}

static void corr_push(CorrVec *v, int32_t off, uint8_t byte) {
    if (v->n == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 16;
        v->v = (CorrEnt *)xrealloc(v->v, v->cap * sizeof(v->v[0]));
    }
    v->v[v->n].off = off;
    v->v[v->n].byte = byte;
    v->n++;
}

static int cmp_i32(const void *a, const void *b) {
    int32_t x = *(const int32_t *)a, y = *(const int32_t *)b;
    return (x > y) - (x < y);
}

static int cmp_corr(const void *a, const void *b) {
    const CorrEnt *x = (const CorrEnt *)a, *y = (const CorrEnt *)b;
    return (x->off > y->off) - (x->off < y->off);
}

static void opvec_push(OpVec *v, Op o) {
    if (v->n == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 64;
        v->v = (Op *)xrealloc(v->v, v->cap * sizeof(v->v[0]));
    }
    v->v[v->n++] = o;
}

static void blockvec_push(BlockVec *v, int32_t fo, int32_t ta, const int64_t *vals, int32_t n) {
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

static void fd_put(FieldDeltaVec *v, uint32_t addr, int kind, int64_t delta) {
    if (v->n == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 256;
        v->v = (FieldDelta *)xrealloc(v->v, v->cap * sizeof(v->v[0]));
    }
    v->v[v->n].addr = addr;
    v->v[v->n].kind = kind;
    v->v[v->n].delta = delta;
    v->n++;
}

static void fd_finalize(FieldDeltaVec *v) {
    if (!v->n) return;
    qsort(v->v, v->n, sizeof(v->v[0]), cmp_fd);
    size_t w = 0;
    for (size_t i = 0; i < v->n; i++) {
        if (w && v->v[w - 1].addr == v->v[i].addr && v->v[w - 1].kind == v->v[i].kind) v->v[w - 1] = v->v[i];
        else v->v[w++] = v->v[i];
    }
    v->n = w;
}

static const FieldDelta *fd_find_kind(const FieldDeltaVec *v, uint32_t addr, int kind) {
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
static void wr32le(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static int cmp_sym_val(const void *a, const void *b) {
    const Sym *x = (const Sym *)a, *y = (const Sym *)b;
    if (x->sec != y->sec) return (x->sec > y->sec) - (x->sec < y->sec);
    return (x->value > y->value) - (x->value < y->value);
}

static void sym_push(SymVec *v, Sym s) {
    if (v->n == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 1024;
        v->v = (Sym *)xrealloc(v->v, v->cap * sizeof(v->v[0]));
    }
    v->v[v->n++] = s;
}

static void range_push(RangeVec *v, uint32_t begin, uint32_t end, uint32_t sec) {
    if (end <= begin) return;
    if (v->n == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 128;
        v->v = (ARange *)xrealloc(v->v, v->cap * sizeof(v->v[0]));
    }
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
    fprintf(stderr, "rc_v3_enc: data segment for %s not found in bin\n", which);
    exit(2);
}

static Ranges elf_ranges(const char *elf_path, const Buf *bin, const char *which) {
    Buf e = slurp(elf_path);
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
    qsort(syms.v, syms.n, sizeof(syms.v[0]), cmp_sym_val);
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

/* ------------------------------------------------------------------------------------- */
/* SequenceMatcher-style subset for create_patch_block().                                  */
/* ------------------------------------------------------------------------------------- */
typedef struct { int32_t val; int32_t *idx; size_t n, cap; int popular; } B2J;
typedef struct { B2J *v; size_t n, cap; } B2JVec;
typedef struct { int32_t a, b, size; } Match;
typedef struct { Match *v; size_t n, cap; } MatchVec;

static int cmp_b2j_val(const void *a, const void *b) {
    const B2J *x = (const B2J *)a, *y = (const B2J *)b;
    return (x->val > y->val) - (x->val < y->val);
}

static void b2j_add(B2JVec *m, int32_t val, int32_t idx) {
    for (size_t i = 0; i < m->n; i++) {
        if (m->v[i].val == val) {
            B2J *e = &m->v[i];
            if (e->n == e->cap) {
                e->cap = e->cap ? e->cap * 2 : 8;
                e->idx = (int32_t *)xrealloc(e->idx, e->cap * sizeof(e->idx[0]));
            }
            e->idx[e->n++] = idx;
            return;
        }
    }
    if (m->n == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 64;
        m->v = (B2J *)xrealloc(m->v, m->cap * sizeof(m->v[0]));
    }
    B2J *e = &m->v[m->n++];
    memset(e, 0, sizeof(*e));
    e->val = val;
    e->cap = 8;
    e->idx = (int32_t *)xmalloc(e->cap * sizeof(e->idx[0]));
    e->idx[e->n++] = idx;
}

static B2J *b2j_find(B2JVec *m, int32_t val) {
    size_t lo = 0, hi = m->n;
    while (lo < hi) {
        size_t mid = (lo + hi) >> 1;
        if (m->v[mid].val < val) lo = mid + 1;
        else hi = mid;
    }
    if (lo < m->n && m->v[lo].val == val && !m->v[lo].popular) return &m->v[lo];
    return NULL;
}

static void match_push(MatchVec *v, Match m) {
    if (v->n == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 32;
        v->v = (Match *)xrealloc(v->v, v->cap * sizeof(v->v[0]));
    }
    v->v[v->n++] = m;
}

static int cmp_match(const void *a, const void *b) {
    const Match *x = (const Match *)a, *y = (const Match *)b;
    if (x->a != y->a) return (x->a > y->a) - (x->a < y->a);
    if (x->b != y->b) return (x->b > y->b) - (x->b < y->b);
    return (x->size > y->size) - (x->size < y->size);
}

static Match find_longest_match(const int32_t *a, int32_t la, const int32_t *b, int32_t lb,
                                B2JVec *b2j, int32_t alo, int32_t ahi, int32_t blo, int32_t bhi) {
    (void)la;
    int32_t besti = alo, bestj = blo, bestsize = 0;
    int32_t *prev = (int32_t *)xcalloc((size_t)lb + 1, sizeof(int32_t));
    int32_t *cur = (int32_t *)xcalloc((size_t)lb + 1, sizeof(int32_t));
    for (int32_t i = alo; i < ahi; i++) {
        memset(cur, 0, ((size_t)lb + 1) * sizeof(int32_t));
        B2J *e = b2j_find(b2j, a[i]);
        if (e) {
            for (size_t jj = 0; jj < e->n; jj++) {
                int32_t j = e->idx[jj];
                if (j < blo) continue;
                if (j >= bhi) break;
                int32_t k = cur[j] = (j > 0 ? prev[j - 1] : 0) + 1;
                if (k > bestsize) {
                    besti = i - k + 1;
                    bestj = j - k + 1;
                    bestsize = k;
                }
            }
        }
        int32_t *tmp = prev; prev = cur; cur = tmp;
    }
    free(cur); free(prev);
    while (besti > alo && bestj > blo && a[besti - 1] == b[bestj - 1]) { besti--; bestj--; bestsize++; }
    while (besti + bestsize < ahi && bestj + bestsize < bhi &&
           a[besti + bestsize] == b[bestj + bestsize]) bestsize++;
    return (Match){ besti, bestj, bestsize };
}

static MatchVec sequence_matching_blocks(const int32_t *a, int32_t la, const int32_t *b, int32_t lb) {
    B2JVec b2j = {0};
    for (int32_t i = 0; i < lb; i++) b2j_add(&b2j, b[i], i);
    if (lb >= 200) {
        int32_t ntest = lb / 100 + 1;
        for (size_t i = 0; i < b2j.n; i++) if ((int32_t)b2j.v[i].n > ntest) b2j.v[i].popular = 1;
    }
    qsort(b2j.v, b2j.n, sizeof(b2j.v[0]), cmp_b2j_val);
    typedef struct { int32_t alo, ahi, blo, bhi; } Region;
    Region *q = (Region *)xmalloc(64 * sizeof(*q));
    size_t qn = 1, qcap = 64;
    q[0] = (Region){0, la, 0, lb};
    MatchVec raw = {0};
    while (qn) {
        Region r = q[--qn];
        Match m = find_longest_match(a, la, b, lb, &b2j, r.alo, r.ahi, r.blo, r.bhi);
        if (m.size) {
            match_push(&raw, m);
            if (r.alo < m.a && r.blo < m.b) {
                if (qn == qcap) { qcap *= 2; q = (Region *)xrealloc(q, qcap * sizeof(*q)); }
                q[qn++] = (Region){ r.alo, m.a, r.blo, m.b };
            }
            if (m.a + m.size < r.ahi && m.b + m.size < r.bhi) {
                if (qn == qcap) { qcap *= 2; q = (Region *)xrealloc(q, qcap * sizeof(*q)); }
                q[qn++] = (Region){ m.a + m.size, r.ahi, m.b + m.size, r.bhi };
            }
        }
    }
    qsort(raw.v, raw.n, sizeof(raw.v[0]), cmp_match);
    MatchVec out = {0};
    int32_t i1 = 0, j1 = 0, k1 = 0;
    for (size_t i = 0; i < raw.n; i++) {
        Match m = raw.v[i];
        if (i1 + k1 == m.a && j1 + k1 == m.b) {
            k1 += m.size;
        } else {
            if (k1) match_push(&out, (Match){ i1, j1, k1 });
            i1 = m.a; j1 = m.b; k1 = m.size;
        }
    }
    if (k1) match_push(&out, (Match){ i1, j1, k1 });
    match_push(&out, (Match){ la, lb, 0 });
    for (size_t i = 0; i < b2j.n; i++) free(b2j.v[i].idx);
    free(b2j.v); free(raw.v); free(q);
    return out;
}

static void create_patch_block(Buf *from_mut, Buf *to_mut, const m4_stream_t *from_s,
                               const m4_stream_t *to_s, BlockVec *out) {
    if (!from_s->n || !to_s->n) return;
    int32_t la = (int32_t)from_s->n - 1, lb = (int32_t)to_s->n - 1;
    if (la <= 0 || lb <= 0) return;
    int32_t *ao = (int32_t *)xmalloc((size_t)la * sizeof(int32_t));
    int32_t *bo = (int32_t *)xmalloc((size_t)lb * sizeof(int32_t));
    for (int32_t i = 0; i < la; i++) ao[i] = (int32_t)(from_s->a[i + 1].addr - from_s->a[i].addr);
    for (int32_t i = 0; i < lb; i++) bo[i] = (int32_t)(to_s->a[i + 1].addr - to_s->a[i].addr);
    MatchVec m = sequence_matching_blocks(ao, la, bo, lb);
    for (size_t mi = 0; mi + 1 < m.n; mi++) {
        int32_t fo = m.v[mi].a, to = m.v[mi].b, sz = m.v[mi].size;
        if (sz < 8) continue;
        sz += 1;
            int64_t *vals = (int64_t *)xmalloc((size_t)sz * sizeof(int64_t));
            int nz = 0;
            for (int32_t k = 0; k < sz; k++) {
            vals[k] = (int64_t)from_s->a[fo + k].val - (int64_t)to_s->a[to + k].val;
            if (vals[k] != 0) nz++;
        }
        if (nz < 8) { free(vals); continue; }
        blockvec_push(out, fo, (int32_t)to_s->a[to].addr, vals, sz);
        for (int32_t k = 0; k < sz; k++) {
            uint32_t a = from_s->a[fo + k].addr;
            if (a + 4 <= from_mut->n) memset(from_mut->d + a, 0, 4);
            a = to_s->a[to + k].addr;
            if (a + 4 <= to_mut->n) memset(to_mut->d + a, 0, 4);
        }
        free(vals);
    }
    free(ao); free(bo); free(m.v);
}

static void data_format_encode(const Buf *from, const Buf *to, const Ranges *fr, const Ranges *tr,
                               Buf *from_mut, Buf *to_mut, BlockVec blocks[STREAM_N]) {
    from_mut->d = (uint8_t *)xmalloc(from->n); from_mut->n = from_mut->cap = from->n; memcpy(from_mut->d, from->d, from->n);
    to_mut->d = (uint8_t *)xmalloc(to->n); to_mut->n = to_mut->cap = to->n; memcpy(to_mut->d, to->d, to->n);
    m4_stream_t fs[M4_NSTREAMS], ts[M4_NSTREAMS];
    if (a1_m4_disassemble(from->d, from->n, fr->data_off_begin, fr->data_begin, fr->data_end,
                          fr->code_begin, fr->code_end, 1, 1, fs)) die("from disassemble failed");
    if (a1_m4_disassemble(to->d, to->n, tr->data_off_begin, tr->data_begin, tr->data_end,
                          tr->code_begin, tr->code_end, 1, 1, ts)) die("to disassemble failed");
    create_patch_block(from_mut, to_mut, &fs[M4_DATA], &ts[M4_DATA], &blocks[STREAM_DATA]);
    create_patch_block(from_mut, to_mut, &fs[M4_CODE], &ts[M4_CODE], &blocks[STREAM_CODE]);
    create_patch_block(from_mut, to_mut, &fs[M4_BW], &ts[M4_BW], &blocks[STREAM_BW]);
    create_patch_block(from_mut, to_mut, &fs[M4_BL], &ts[M4_BL], &blocks[STREAM_BL]);
    create_patch_block(from_mut, to_mut, &fs[M4_LDR], &ts[M4_LDR], &blocks[STREAM_LDR]);
    create_patch_block(from_mut, to_mut, &fs[M4_LDRW], &ts[M4_LDRW], &blocks[STREAM_LDRW]);
    a1_m4_free_streams(fs); a1_m4_free_streams(ts);
}

/* ------------------------------------------------------------------------------------- */
/* bsdiff op generation.                                                                  */
/* ------------------------------------------------------------------------------------- */
static int32_t matchlen(const uint8_t *from, int32_t from_size, const uint8_t *to, int32_t to_size) {
    int32_t n = from_size < to_size ? from_size : to_size;
    int32_t i;
    for (i = 0; i < n; i++) if (from[i] != to[i]) break;
    return i;
}

static int32_t suffix_search(const int32_t *sa, const uint8_t *from, int32_t from_size,
                             const uint8_t *to, int32_t to_size, int32_t begin, int32_t end,
                             int32_t *pos) {
    if (end - begin < 2) {
        int32_t x = matchlen(from + sa[begin], from_size - sa[begin], to, to_size);
        int32_t y = matchlen(from + sa[end], from_size - sa[end], to, to_size);
        if (x > y) { *pos = sa[begin]; return x; }
        *pos = sa[end]; return y;
    }
    int32_t x = begin + (end - begin) / 2;
    int cmp = memcmp(from + sa[x], to, (size_t)((from_size - sa[x]) < to_size ? (from_size - sa[x]) : to_size));
    if (cmp < 0) return suffix_search(sa, from, from_size, to, to_size, x, end, pos);
    return suffix_search(sa, from, from_size, to, to_size, begin, x, pos);
}

static void emit_bsdiff_op(OpVec *ops, const uint8_t *from, int32_t from_size,
                           const uint8_t *to, int32_t to_size, uint8_t *debuf,
                           int32_t scan, int32_t pos, int32_t *last_scan_p,
                           int32_t *last_pos_p, int32_t *last_offset_p) {
    int32_t last_scan = *last_scan_p, last_pos = *last_pos_p;
    int32_t s = 0, sf = 0, diff_size = 0;
    for (int32_t i = 0; (last_scan + i < scan) && (last_pos + i < from_size);) {
        if (from[last_pos + i] == to[last_scan + i]) s++;
        i++;
        if (s * 2 - i > sf * 2 - diff_size) { sf = s; diff_size = i; }
    }
    int32_t lenb = 0;
    if (scan < to_size) {
        s = 0;
        int32_t sb = 0;
        for (int32_t i = 1; (scan >= last_scan + i) && (pos >= i); i++) {
            if (from[pos - i] == to[scan - i]) s++;
            if (s * 2 - i > sb * 2 - lenb) { sb = s; lenb = i; }
        }
    }
    int32_t overlap = (last_scan + diff_size) - (scan - lenb);
    if (overlap > 0) {
        s = 0;
        int32_t ss = 0, lens = 0;
        for (int32_t i = 0; i < overlap; i++) {
            if (to[last_scan + diff_size - overlap + i] == from[last_pos + diff_size - overlap + i]) s++;
            if (to[scan - lenb + i] == from[pos - lenb + i]) s--;
            if (s > ss) { ss = s; lens = i + 1; }
        }
        diff_size += (lens - overlap);
        lenb -= lens;
    }
    for (int32_t i = 0; i < diff_size; i++) debuf[i] = (uint8_t)(to[last_scan + i] - from[last_pos + i]);
    int32_t extra_pos = last_scan + diff_size;
    int32_t extra_size = scan - lenb - extra_pos;
    Op o;
    o.diff_len = diff_size;
    o.diff = (uint8_t *)xmalloc((size_t)diff_size);
    memcpy(o.diff, debuf, (size_t)diff_size);
    o.extra_len = extra_size;
    o.extra = (uint8_t *)xmalloc((size_t)extra_size);
    for (int32_t i = 0; i < extra_size; i++) o.extra[i] = to[extra_pos + i];
    o.adj = (pos - lenb) - (last_pos + diff_size);
    opvec_push(ops, o);
    *last_scan_p = scan - lenb;
    *last_pos_p = pos - lenb;
    *last_offset_p = pos - scan;
}

static OpVec bsdiff_ops(const Buf *from, const Buf *to) {
    OpVec ops = {0};
    int32_t from_size = (int32_t)from->n, to_size = (int32_t)to->n;
    int32_t *sa = (int32_t *)xmalloc(((size_t)from_size + 1) * sizeof(int32_t));
    sa[0] = from_size;
    if (from_size && divsufsort(from->d, &sa[1], from_size) != 0) die("divsufsort failed");
    uint8_t *debuf = (uint8_t *)xmalloc((size_t)to_size + 1);
    int32_t scan = 0, len = 0, last_scan = 0, last_pos = 0, last_offset = 0, pos = 0;
    while (scan < to_size) {
        int32_t from_score = 0;
        scan += len;
        for (int32_t scsc = scan; scan < to_size; scan++) {
            len = suffix_search(sa, from->d, from_size, to->d + scan, to_size - scan, 0, from_size, &pos);
            for (; scsc < scan + len; scsc++) {
                if ((scsc + last_offset < from_size) && (from->d[scsc + last_offset] == to->d[scsc])) from_score++;
            }
            if (((len == from_score) && (len != 0)) || (len > from_score + 11)) break;
            if ((scan + last_offset < from_size) && (from->d[scan + last_offset] == to->d[scan])) from_score--;
        }
        if ((len != from_score) || (scan == to_size))
            emit_bsdiff_op(&ops, from->d, from_size, to->d, to_size, debuf, scan, pos,
                           &last_scan, &last_pos, &last_offset);
    }
    free(sa); free(debuf);
    return ops;
}

/* ------------------------------------------------------------------------------------- */
/* A1 field and apply planning.                                                            */
/* ------------------------------------------------------------------------------------- */
static uint16_t u16le_at(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t u32le_at(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static int32_t unpack_bl_local(uint16_t up, uint16_t lo) {
    int32_t s = (up >> 10) & 1, imm10 = up & 0x3ff, imm11 = lo & 0x7ff;
    int32_t j1 = (lo >> 13) & 1, j2 = (lo >> 11) & 1;
    int32_t i1 = 1 - (j1 ^ s), i2 = 1 - (j2 ^ s);
    int32_t v = (s << 23) | (i1 << 22) | (i2 << 21) | (imm10 << 11) | imm11;
    if (s) v -= (1 << 24);
    return v;
}

static void pack_bl_local(int32_t imm32, uint8_t out[4]) {
    a1_m4_pack(M4_PK_BL, imm32, out);
}

static int is_local_bl(const uint8_t *frm, uint32_t from_size, uint32_t fpk) {
    if (fpk & 1u) return 0;
    if (fpk > from_size || from_size - fpk < 4u) return 0;
    uint16_t up = u16le_at(frm + fpk), lo = u16le_at(frm + fpk + 2);
    return ((up & 0xf800u) == 0xf000u) && ((lo & 0xd000u) == 0xd000u);
}

static int field_addr(int32_t fp0, int32_t k, uint32_t from_size, uint32_t *out) {
    int64_t a = (int64_t)fp0 + k;
    if (a < 0 || a + 4 > (int64_t)from_size) return 0;
    *out = (uint32_t)a;
    return 1;
}

static IVec op_ldr_set(const uint8_t *frm, int32_t fp0, int32_t dl, uint32_t from_size) {
    IVec s = {0};
    int32_t lo = fp0, hi = fp0 + dl;
    int32_t a = (lo & 1) ? lo + 1 : lo;
    while (a + 2 <= hi && a + 2 <= (int32_t)from_size) {
        uint16_t up = u16le_at(frm + a);
        if ((up & 0xf800u) == 0x4800u) {
            int32_t t = (a & ~3) + 4 * (int32_t)(up & 0xffu) + 4;
            if (lo <= t && t + 4 <= hi && t + 4 <= (int32_t)from_size) ivec_push(&s, t);
        }
        a += 2;
    }
    if (s.n) {
        qsort(s.v, s.n, sizeof(s.v[0]), cmp_i32);
        size_t w = 0;
        for (size_t i = 0; i < s.n; i++) if (!w || s.v[w - 1] != s.v[i]) s.v[w++] = s.v[i];
        s.n = w;
    }
    return s;
}

static int ivec_has(const IVec *v, int32_t x) {
    size_t lo = 0, hi = v->n;
    while (lo < hi) {
        size_t m = (lo + hi) >> 1;
        if (v->v[m] < x) lo = m + 1;
        else hi = m;
    }
    return lo < v->n && v->v[lo] == x;
}

enum { EV_NONE, EV_BL, EV_EX, EV_SBL };
typedef struct { int type; int64_t delta; } Event;

static Event classify_field(const uint8_t *frm, uint32_t from_size, const FieldDeltaVec *fd,
                            const IVec *ldr, const Op *o, int32_t fp0, int32_t k) {
    uint32_t fpk;
    if (!field_addr(fp0, k, from_size, &fpk)) return (Event){ EV_NONE, 0 };
    int pure = o->diff[k] == 0 && o->diff[k + 1] == 0 && o->diff[k + 2] == 0 && o->diff[k + 3] == 0;
    if (is_local_bl(frm, from_size, fpk)) {
        if (!pure) return (Event){ EV_SBL, 0 };
        const FieldDelta *r = fd_find_kind(fd, fpk, STREAM_BL);
        return (Event){ EV_BL, r ? r->delta : 0 };
    }
    if (pure && ivec_has(ldr, (int32_t)fpk)) {
        const FieldDelta *r = fd_find_kind(fd, fpk, STREAM_LDR);
        return (Event){ EV_EX, r ? r->delta : 0 };
    }
    return (Event){ EV_NONE, 0 };
}

static Event classify_field_assume_pure(const uint8_t *frm, uint32_t from_size, const FieldDeltaVec *fd,
                                        const IVec *ldr, int32_t fp0, int32_t k) {
    uint32_t fpk;
    if (!field_addr(fp0, k, from_size, &fpk)) return (Event){ EV_NONE, 0 };
    if (is_local_bl(frm, from_size, fpk)) {
        const FieldDelta *r = fd_find_kind(fd, fpk, STREAM_BL);
        return (Event){ EV_BL, r ? r->delta : 0 };
    }
    if (ivec_has(ldr, (int32_t)fpk)) {
        const FieldDelta *r = fd_find_kind(fd, fpk, STREAM_LDR);
        return (Event){ EV_EX, r ? r->delta : 0 };
    }
    return (Event){ EV_NONE, 0 };
}

static FieldDeltaVec build_field_deltas(const Buf *from, const Ranges *fr, const BlockVec blocks[STREAM_N]) {
    FieldDeltaVec out = {0};
    m4_stream_t st[M4_NSTREAMS];
    if (a1_m4_disassemble(from->d, from->n, fr->data_off_begin, fr->data_begin, fr->data_end,
                          fr->code_begin, fr->code_end, 0, 0, st)) die("M0 disassemble failed");
    int active[STREAM_N] = {1, 1, 0, 1, 1, 0};
    int mapidx[STREAM_N] = {M4_DATA, M4_CODE, M4_BW, M4_BL, M4_LDR, M4_LDRW};
    for (int s = 0; s < STREAM_N; s++) {
        if (!active[s]) continue;
        const m4_stream_t *ms = &st[mapidx[s]];
        for (size_t bi = 0; bi < blocks[s].n; bi++) {
            const Block *b = &blocks[s].v[bi];
            for (int32_t k = 0; k < b->n; k++) {
                size_t idx = (size_t)b->from_offset + (size_t)k;
                if (idx < ms->n) fd_put(&out, ms->a[idx].addr, s, b->values[k]);
            }
        }
    }
    a1_m4_free_streams(st);
    fd_finalize(&out);
    return out;
}

static void coerce_reloc_literals(OpVec *ops, const uint8_t *frm, uint32_t from_size,
                                  uint32_t to_size, const FieldDeltaVec *fd) {
    int FWD = to_size <= from_size;
    int32_t fp0 = 0;
    for (size_t oi = 0; oi < ops->n; oi++) {
        Op *o = &ops->v[oi];
        IVec ldr = op_ldr_set(frm, fp0, o->diff_len, from_size);
        if (FWD) {
            for (int32_t k = 0; k < o->diff_len;) {
                if (k + 4 <= o->diff_len) {
                    Event ev = classify_field_assume_pure(frm, from_size, fd, &ldr, fp0, k);
                    uint32_t fpk = 0;
                    const FieldDelta *real = NULL;
                    if (ev.type == EV_BL || ev.type == EV_EX) {
                        (void)field_addr(fp0, k, from_size, &fpk);
                        real = fd_find_kind(fd, fpk, ev.type == EV_BL ? STREAM_BL : STREAM_LDR);
                    }
                    int ok = real != NULL;
                    if (ok && (o->diff[k] || o->diff[k+1] || o->diff[k+2] || o->diff[k+3])) {
                        memset(o->diff + k, 0, 4);
                    }
                    if (ev.type) { k += 4; continue; }
                }
                k++;
            }
        } else {
            for (int32_t top = o->diff_len - 1; top >= 0;) {
                int32_t k = top - 3;
                if (k >= 0) {
                    Event ev = classify_field_assume_pure(frm, from_size, fd, &ldr, fp0, k);
                    uint32_t fpk = 0;
                    const FieldDelta *real = NULL;
                    if (ev.type == EV_BL || ev.type == EV_EX) {
                        (void)field_addr(fp0, k, from_size, &fpk);
                        real = fd_find_kind(fd, fpk, ev.type == EV_BL ? STREAM_BL : STREAM_LDR);
                    }
                    int ok = real != NULL;
                    if (ok && (o->diff[k] || o->diff[k+1] || o->diff[k+2] || o->diff[k+3])) {
                        memset(o->diff + k, 0, 4);
                    }
                    if (ev.type) { top -= 4; continue; }
                }
                top--;
            }
        }
        free(ldr.v);
        fp0 += o->diff_len + o->adj;
    }
}

static Op op_copy(int32_t diff_len, const uint8_t *diff, int32_t extra_len, const uint8_t *extra, int32_t adj) {
    Op o;
    o.diff_len = diff_len;
    o.diff = (uint8_t *)xmalloc((size_t)diff_len);
    if (diff_len) memcpy(o.diff, diff, (size_t)diff_len);
    o.extra_len = extra_len;
    o.extra = (uint8_t *)xmalloc((size_t)extra_len);
    if (extra_len) memcpy(o.extra, extra, (size_t)extra_len);
    o.adj = adj;
    return o;
}

static uint64_t dz_bits_u32(uint32_t x) {
    uint32_t m = x + 1u;
    int n = bitlen32(m);
    return (uint64_t)(2 * bitlen32((uint32_t)n) - 1 + n - 1);
}

static uint64_t uleb_proxy_bits(uint32_t v, const uint8_t L0[256]) {
    uint64_t bits = 0;
    for (;;) {
        uint8_t x = (uint8_t)(v & 0x7fu);
        v >>= 7;
        bits += L0[v ? (uint8_t)(x | 0x80u) : x];
        if (!v) return bits;
    }
}

static uint64_t diff_proxy_bits(const Op *o, int32_t begin, int32_t end,
                                int FWD, const uint8_t L0[256]) {
    int32_t dl = end - begin;
    uint32_t nlit = 0;
    uint64_t bits = 0;
    for (int32_t k = begin; k < end; k++) nlit += o->diff[k] != 0;
    bits += uleb_proxy_bits(nlit, L0);
    if (FWD) {
        int32_t prev = 0;
        for (int32_t k = begin; k < end; k++) if (o->diff[k]) {
            int32_t loc = k - begin;
            bits += uleb_proxy_bits((uint32_t)(loc - prev), L0);
            bits += L0[o->diff[k]];
            prev = loc;
        }
    } else {
        int32_t prev = dl;
        for (int32_t k = end; k-- > begin;) if (o->diff[k]) {
            int32_t loc = k - begin;
            bits += uleb_proxy_bits((uint32_t)(prev - loc), L0);
            bits += L0[o->diff[k]];
            prev = loc;
        }
    }
    return bits;
}

static uint64_t extra_proxy_bits(const Buf *to, int32_t abs_begin, int32_t len,
                                 const uint8_t L0[256], const uint8_t L1[256]) {
    uint64_t bits = 0;
    for (int32_t i = 0; i < len; i++) {
        const uint8_t *L = ((abs_begin + i) & 1) ? L1 : L0;
        bits += L[to->d[(size_t)abs_begin + (size_t)i]];
    }
    return bits;
}

static uint64_t op_proxy_bits(const Op *o, const Buf *to, int32_t tp0,
                              int32_t begin, int32_t end, int32_t extra_begin,
                              int32_t extra_len, int32_t adj, int FWD,
                              const uint8_t L0[256], const uint8_t L1[256]) {
    uint64_t bits = dz_bits_u32((uint32_t)(end - begin)) +
                    dz_bits_u32((uint32_t)extra_len) +
                    dz_bits_u32(zz32(adj));
    bits += 2; /* zero preserve-count and correction-count symbols. */
    bits += diff_proxy_bits(o, begin, end, FWD, L0);
    bits += extra_proxy_bits(to, tp0 + extra_begin, extra_len, L0, L1);
    return bits;
}

typedef struct { int32_t begin, end; } Run;

static void from_huff_lengths(const uint8_t *frm, size_t n, uint8_t L0[256], uint8_t L1[256]);

enum { SPLIT_GAIN_MARGIN_BITS = 8 };

/* Dense diff runs can be represented either as literal diff bytes or as an
 * extra span plus an equal source skip. Choose the subset analytically per op:
 * dynamic programming minimizes the modeled cost of the resulting op sequence
 * using the same seeded literal bit-length proxy used by LZ planning, plus the
 * exact raw geometry code lengths. */
static void split_nonzero_diff_runs(OpVec *ops, const Buf *from, const Buf *to) {
    uint8_t L0[256], L1[256];
    from_huff_lengths(from->d, from->n, L0, L1);
    OpVec out = {0};
    int32_t tp = 0;
    for (size_t oi = 0; oi < ops->n; oi++) {
        Op *o = &ops->v[oi];
        Run *runs = NULL;
        size_t nr = 0, rcap = 0;
        for (int32_t scan = 0; scan < o->diff_len;) {
            while (scan < o->diff_len && o->diff[scan] == 0) scan++;
            if (scan >= o->diff_len) break;
            int32_t begin = scan++;
            while (scan < o->diff_len && o->diff[scan] != 0) scan++;
            if (nr == rcap) {
                rcap = rcap ? rcap * 2 : 16;
                runs = (Run *)xrealloc(runs, rcap * sizeof(runs[0]));
            }
            runs[nr++] = (Run){ begin, scan };
        }
        if (!nr) {
            opvec_push(&out, *o);
            tp += o->diff_len + o->extra_len;
            free(runs);
            continue;
        }
        size_t states = (nr + 1) * (nr + 1);
        uint64_t *dp = (uint64_t *)xmalloc(states * sizeof(dp[0]));
        uint8_t *take = (uint8_t *)xcalloc(states, 1);
#define DP(I, P) dp[(I) * (nr + 1) + (P)]
#define TAKE(I, P) take[(I) * (nr + 1) + (P)]
        for (size_t p = 0; p <= nr; p++) {
            int32_t seg = p ? runs[p - 1].end : 0;
            DP(nr, p) = op_proxy_bits(o, to, tp, seg, o->diff_len,
                                      o->diff_len, o->extra_len, o->adj,
                                      to->n <= from->n, L0, L1);
        }
        for (size_t ri = nr; ri-- > 0;) {
            for (size_t p = 0; p <= ri; p++) {
                int32_t seg = p ? runs[p - 1].end : 0;
                int32_t len = runs[ri].end - runs[ri].begin;
                uint64_t skip = DP(ri + 1, p);
                uint64_t split = op_proxy_bits(o, to, tp, seg, runs[ri].begin,
                                               runs[ri].begin, len, len,
                                               to->n <= from->n, L0, L1) +
                                 DP(ri + 1, ri + 1);
                if (split + SPLIT_GAIN_MARGIN_BITS < skip) {
                    DP(ri, p) = split;
                    TAKE(ri, p) = 1;
                } else {
                    DP(ri, p) = skip;
                }
            }
        }
        size_t ri = 0, p = 0;
        int32_t seg = 0;
        int split_any = 0;
        while (ri < nr) {
            if (TAKE(ri, p)) {
                int32_t len = runs[ri].end - runs[ri].begin;
                int32_t pre = runs[ri].begin - seg;
                opvec_push(&out, op_copy(pre, o->diff + seg, len,
                                         to->d + (size_t)tp + (size_t)runs[ri].begin, len));
                seg = runs[ri].end;
                p = ri + 1;
                split_any = 1;
            }
            ri++;
        }
        if (split_any) {
            int32_t tail = o->diff_len - seg;
            if (tail || o->extra_len || o->adj)
                opvec_push(&out, op_copy(tail, o->diff + seg, o->extra_len, o->extra, o->adj));
            free(o->diff);
            free(o->extra);
        } else {
            opvec_push(&out, *o);
        }
        free(dp);
        free(take);
        free(runs);
#undef DP
#undef TAKE
        tp += o->diff_len + o->extra_len;
    }
    free(ops->v);
    *ops = out;
}

static uint8_t *preserve_indices(const OpVec *ops, uint32_t from_size, uint32_t to_size) {
    int FWD = to_size <= from_size;
    int32_t *readarr = (int32_t *)xmalloc((size_t)from_size * sizeof(int32_t));
    for (uint32_t i = 0; i < from_size; i++) readarr[i] = FWD ? -1 : INT_MAX;
    int32_t tp = 0, fp = 0;
    for (size_t oi = 0; oi < ops->n; oi++) {
        const Op *o = &ops->v[oi];
        for (int32_t k = 0; k < o->diff_len; k++) {
            int32_t a = fp + k;
            if (0 <= a && (uint32_t)a < from_size) {
                int32_t t = tp + k;
                if (FWD) { if (readarr[a] < t) readarr[a] = t; }
                else { if (readarr[a] > t) readarr[a] = t; }
            }
        }
        tp += o->diff_len + o->extra_len;
        fp += o->diff_len + o->adj;
    }
    uint8_t *pres = (uint8_t *)xcalloc(to_size ? to_size : 1, 1);
    int32_t wi = 0;
    if (FWD) {
        tp = fp = 0;
        for (size_t oi = 0; oi < ops->n; oi++) {
            const Op *o = &ops->v[oi];
            for (int32_t k = 0; k < o->diff_len; k++, wi++) {
                int32_t tpw = tp + k;
                if (0 <= tpw && (uint32_t)tpw < from_size && readarr[tpw] > tpw) pres[wi] = 1;
            }
            for (int32_t e = 0; e < o->extra_len; e++, wi++) {
                int32_t tpw = tp + o->diff_len + e;
                if (0 <= tpw && (uint32_t)tpw < from_size && readarr[tpw] > tpw) pres[wi] = 1;
            }
            tp += o->diff_len + o->extra_len;
            fp += o->diff_len + o->adj;
        }
    } else {
        typedef struct { int32_t tp, fp; const Op *o; } M;
        M *m = (M *)xmalloc(ops->n * sizeof(*m));
        tp = fp = 0;
        for (size_t oi = 0; oi < ops->n; oi++) {
            m[oi] = (M){tp, fp, &ops->v[oi]};
            tp += ops->v[oi].diff_len + ops->v[oi].extra_len;
            fp += ops->v[oi].diff_len + ops->v[oi].adj;
        }
        for (size_t rr = ops->n; rr-- > 0;) {
            const Op *o = m[rr].o;
            for (int32_t e = o->extra_len - 1; e >= 0; e--, wi++) {
                int32_t tpw = m[rr].tp + o->diff_len + e;
                if (0 <= tpw && (uint32_t)tpw < from_size && readarr[tpw] >= 0 && readarr[tpw] < tpw) pres[wi] = 1;
            }
            for (int32_t k = o->diff_len - 1; k >= 0; k--, wi++) {
                int32_t tpw = m[rr].tp + k;
                if (0 <= tpw && (uint32_t)tpw < from_size && readarr[tpw] >= 0 && readarr[tpw] < tpw) pres[wi] = 1;
            }
        }
        free(m);
    }
    free(readarr);
    return pres;
}

static uint8_t *corrections_hybrid(const OpVec *ops, const uint8_t *frm, const uint8_t *true_to,
                                   const FieldDeltaVec *fd, uint32_t from_size, uint32_t to_size,
                                   const uint8_t *presset) {
    int FWD = to_size <= from_size;
    size_t span = from_size > to_size ? from_size : to_size;
    uint8_t *ohas = (uint8_t *)xcalloc(span ? span : 1, 1), *oval = (uint8_t *)xcalloc(span ? span : 1, 1);
    typedef struct { int32_t tp, fp; const Op *o; } M;
    M *m = (M *)xmalloc(ops->n * sizeof(*m));
    int32_t tp = 0, fp = 0;
    for (size_t oi = 0; oi < ops->n; oi++) {
        m[oi] = (M){tp, fp, &ops->v[oi]};
        tp += ops->v[oi].diff_len + ops->v[oi].extra_len;
        fp += ops->v[oi].diff_len + ops->v[oi].adj;
    }
    for (size_t idx = 0; idx < ops->n; idx++) {
        size_t oi = FWD ? idx : ops->n - 1 - idx;
        const Op *o = m[oi].o;
        IVec ldr = op_ldr_set(frm, m[oi].fp, o->diff_len, from_size);
        if (FWD) {
            for (int32_t k = 0; k < o->diff_len;) {
                if (k + 4 <= o->diff_len) {
                    Event ev = classify_field(frm, from_size, fd, &ldr, o, m[oi].fp, k);
                    if (ev.type == EV_BL || ev.type == EV_EX) {
                        uint8_t packed[4];
                        uint32_t fpk = (uint32_t)(m[oi].fp + k);
                        if (ev.type == EV_BL) {
                            uint16_t up = u16le_at(frm + fpk), lo = u16le_at(frm + fpk + 2);
                            pack_bl_local(unpack_bl_local(up, lo) - ev.delta, packed);
                        } else {
                            uint32_t val = u32le_at(frm + fpk);
                            wr32le(packed, val - (uint32_t)ev.delta);
                        }
                        for (int b = 0; b < 4; b++) { ohas[m[oi].tp + k + b] = 1; oval[m[oi].tp + k + b] = packed[b]; }
                        k += 4; continue;
                    } else if (ev.type == EV_SBL) { k += 4; continue; }
                }
                k++;
            }
        } else {
            for (int32_t top = o->diff_len - 1; top >= 0;) {
                int32_t k = top - 3;
                if (k >= 0) {
                    Event ev = classify_field(frm, from_size, fd, &ldr, o, m[oi].fp, k);
                    if (ev.type == EV_BL || ev.type == EV_EX) {
                        uint8_t packed[4];
                        uint32_t fpk = (uint32_t)(m[oi].fp + k);
                        if (ev.type == EV_BL) {
                            uint16_t up = u16le_at(frm + fpk), lo = u16le_at(frm + fpk + 2);
                            pack_bl_local(unpack_bl_local(up, lo) - ev.delta, packed);
                        } else {
                            uint32_t val = u32le_at(frm + fpk);
                            wr32le(packed, val - (uint32_t)ev.delta);
                        }
                        for (int b = 0; b < 4; b++) { ohas[m[oi].tp + k + b] = 1; oval[m[oi].tp + k + b] = packed[b]; }
                        top -= 4; continue;
                    } else if (ev.type == EV_SBL) { top -= 4; continue; }
                }
                top--;
            }
        }
        free(ldr.v);
    }
    uint8_t *buf = (uint8_t *)xcalloc(span ? span : 1, 1);
    memcpy(buf, frm, from_size);
    uint8_t *jhas = (uint8_t *)xcalloc(from_size ? from_size : 1, 1), *jval = (uint8_t *)xcalloc(from_size ? from_size : 1, 1);
    uint8_t *corr = (uint8_t *)xcalloc(to_size ? to_size : 1, 1);
    uint8_t *chas = (uint8_t *)xcalloc(to_size ? to_size : 1, 1);
    int32_t wi = 0;
#define SIM_WRITE(TP, FP, ISD, DB) do { \
        int32_t _tp = (TP), _fp = (FP); \
        if (presset[wi] && 0 <= _tp && (uint32_t)_tp < from_size && !jhas[_tp]) { jhas[_tp] = 1; jval[_tp] = buf[_tp]; } \
        uint8_t produced; \
        if (ohas[_tp]) produced = oval[_tp]; \
        else { uint8_t src = 0; if ((ISD) && 0 <= _fp && (uint32_t)_fp < from_size) src = jhas[_fp] ? jval[_fp] : buf[_fp]; produced = (uint8_t)((DB) + src); } \
        uint8_t want = true_to[_tp]; \
        uint8_t c = (uint8_t)(want - produced); \
        if (c) { corr[_tp] = c; chas[_tp] = 1; } \
        buf[_tp] = want; wi++; \
    } while (0)
    if (FWD) {
        for (size_t oi = 0; oi < ops->n; oi++) {
            const Op *o = m[oi].o;
            for (int32_t k = 0; k < o->diff_len; k++) SIM_WRITE(m[oi].tp + k, m[oi].fp + k, 1, o->diff[k]);
            for (int32_t e = 0; e < o->extra_len; e++) SIM_WRITE(m[oi].tp + o->diff_len + e, -1, 0, o->extra[e]);
        }
    } else {
        for (size_t rr = ops->n; rr-- > 0;) {
            const Op *o = m[rr].o;
            for (int32_t e = o->extra_len - 1; e >= 0; e--) SIM_WRITE(m[rr].tp + o->diff_len + e, -1, 0, o->extra[e]);
            for (int32_t k = o->diff_len - 1; k >= 0; k--) SIM_WRITE(m[rr].tp + k, m[rr].fp + k, 1, o->diff[k]);
        }
    }
#undef SIM_WRITE
    for (uint32_t i = 0; i < to_size; i++) if (!chas[i]) corr[i] = 0;
    free(chas); free(buf); free(jhas); free(jval); free(ohas); free(oval); free(m);
    return corr;
}

static OpPC *preserve_corr_per_op(const OpVec *ops, uint32_t from_size, uint32_t to_size,
                                  const uint8_t *presset, const uint8_t *corr) {
    int FWD = to_size <= from_size;
    OpPC *out = (OpPC *)xcalloc(ops->n ? ops->n : 1, sizeof(*out));
    typedef struct { int32_t tp, fp; const Op *o; size_t orig; } M;
    M *m = (M *)xmalloc(ops->n * sizeof(*m));
    int32_t tp = 0, fp = 0;
    for (size_t oi = 0; oi < ops->n; oi++) {
        m[oi] = (M){tp, fp, &ops->v[oi], oi};
        tp += ops->v[oi].diff_len + ops->v[oi].extra_len;
        fp += ops->v[oi].diff_len + ops->v[oi].adj;
    }
    int32_t wi = 0;
    for (size_t step = 0; step < ops->n; step++) {
        size_t oi = FWD ? step : ops->n - 1 - step;
        const Op *o = m[oi].o;
        OpPC *pc = &out[step];
        if (FWD) {
            for (int32_t k = 0; k < o->diff_len; k++, wi++) {
                if (presset[wi]) ivec_push(&pc->pres, k);
                if (corr[m[oi].tp + k]) corr_push(&pc->corr, k, corr[m[oi].tp + k]);
            }
            for (int32_t e = 0; e < o->extra_len; e++, wi++) {
                int32_t off = o->diff_len + e;
                if (presset[wi]) ivec_push(&pc->pres, off);
                if (corr[m[oi].tp + off]) corr_push(&pc->corr, off, corr[m[oi].tp + off]);
            }
        } else {
            for (int32_t e = o->extra_len - 1; e >= 0; e--, wi++) {
                int32_t off = o->diff_len + e;
                if (presset[wi]) ivec_push(&pc->pres, off);
                if (corr[m[oi].tp + off]) corr_push(&pc->corr, off, corr[m[oi].tp + off]);
            }
            for (int32_t k = o->diff_len - 1; k >= 0; k--, wi++) {
                if (presset[wi]) ivec_push(&pc->pres, k);
                if (corr[m[oi].tp + k]) corr_push(&pc->corr, k, corr[m[oi].tp + k]);
            }
            if (pc->pres.n > 1) qsort(pc->pres.v, pc->pres.n, sizeof(pc->pres.v[0]), cmp_i32);
            if (pc->corr.n > 1) qsort(pc->corr.v, pc->corr.n, sizeof(pc->corr.v[0]), cmp_corr);
        }
    }
    free(m);
    return out;
}

static EncStats collect_stats(const OpVec *ops, const uint8_t *frm, uint32_t from_size,
                              uint32_t to_size, const FieldDeltaVec *fd, const OpPC *pc) {
    (void)to_size;
    EncStats st = {0};
    int32_t fp0 = 0;
    st.ops = ops->n;
    for (size_t i = 0; i < ops->n; i++) {
        st.preserves += pc[i].pres.n;
        st.corrections += pc[i].corr.n;
    }
    for (size_t oi = 0; oi < ops->n; oi++) {
        const Op *o = &ops->v[oi];
        st.diff_bytes += (size_t)o->diff_len;
        st.extra_bytes += (size_t)o->extra_len;
        for (int32_t k = 0; k < o->diff_len; k++) st.literals += o->diff[k] != 0;
        IVec ldr = op_ldr_set(frm, fp0, o->diff_len, from_size);
        for (int32_t k = 0; k + 4 <= o->diff_len;) {
            Event ev = classify_field(frm, from_size, fd, &ldr, o, fp0, k);
            if (ev.type == EV_BL) { st.bl_fields++; k += 4; continue; }
            if (ev.type == EV_EX) { st.ex_fields++; k += 4; continue; }
            if (ev.type == EV_SBL) { st.suppressed_bl++; k += 4; continue; }
            k++;
        }
        free(ldr.v);
        fp0 += o->diff_len + o->adj;
    }
    return st;
}

/* ------------------------------------------------------------------------------------- */
/* Binary range encoder and models.                                                        */
/* ------------------------------------------------------------------------------------- */
typedef struct { uint64_t low; uint32_t range; uint8_t cache; uint32_t csz; Buf out; } REnc;
typedef struct { uint8_t code, k; uint16_t u[UG_CTX + 1], m[UG_CTX + 1][UG_CTX + 1]; } UGE;
typedef struct { uint16_t p[256]; uint8_t rate; } BTE;
typedef struct { uint16_t m[4]; int h; } FLE;
typedef struct { BTE t; } BVE;
typedef struct { int64_t *dic; uint16_t cap, K; int64_t last; uint16_t rep[4], hit; uint8_t rh; } DRE;

static void re_shift_low(REnc *r) {
    if (r->low < 0xff000000ull || r->low > 0xffffffffull) {
        uint8_t c = r->cache;
        for (;;) {
            buf_put(&r->out, (uint8_t)(c + (uint8_t)(r->low >> 32)));
            c = 0xff;
            r->csz--;
            if (r->csz == 0) break;
        }
        r->cache = (uint8_t)(r->low >> 24);
    }
    r->csz++;
    r->low = (r->low << 8) & 0xffffffffull;
}

static void re_init(REnc *r) { memset(r, 0, sizeof(*r)); r->range = 0xffffffffu; r->csz = 1; }

/* Default adaptive-bit rate for Golomb (unary+mantissa), order-2 flag, and MTF rep/hit
 * streams. MUST equal RC_S_BIT_RATE in rc_v3.c (decoder) — the wire is bit-exact. */
#define RC_S_BIT_RATE 4

/* Mirrors rc_v3.c RC_REP0_INIT: rep0 prior toward 0 (P(reuse)~1/8), 3584. */
#define RC_REP0_INIT (RC_PBIT - (RC_PBIT>>3))

static void re_bit(REnc *r, uint16_t *prob, int bit, int rate) {
    uint32_t p = *prob, bound = (r->range >> 12) * p;
    if (bit == 0) { r->range = bound; *prob = (uint16_t)(p + ((RC_PBIT - p) >> rate)); }
    else { r->low += bound; r->range -= bound; *prob = (uint16_t)(p - (p >> rate)); }
    while (r->range < RC_KTOP) { r->range <<= 8; re_shift_low(r); }
}

static void re_raw(REnc *r, int bit) {
    uint32_t bound = r->range >> 1;
    if (bit == 0) r->range = bound;
    else { r->low += bound; r->range -= bound; }
    while (r->range < RC_KTOP) { r->range <<= 8; re_shift_low(r); }
}

static Buf re_flush_opt(REnc *r) {
    int t = bitlen32(r->range) - 1;
    uint64_t mask = (1ull << t) - 1ull;
    if (r->low & mask) r->low = (r->low + (1ull << t)) & ~mask;
    size_t base = r->out.n;
    for (int i = 0; i < 5; i++) re_shift_low(r);
    while (r->out.n > base && r->out.d[r->out.n - 1] == 0) r->out.n--;
    Buf b = r->out;
    r->out = (Buf){0};
    return b;
}

static void put_raw_bits(REnc *r, uint32_t v, int nb) {
    for (int sh = nb - 1; sh >= 0; sh--) re_raw(r, (int)((v >> sh) & 1u));
}

static void w_gamma(REnc *r, uint32_t m) {
    int n = bitlen32(m) - 1;
    for (int i = 0; i < n; i++) re_raw(r, 0);
    for (int i = n; i >= 0; i--) re_raw(r, (int)((m >> i) & 1u));
}

static void w_gz(REnc *r, uint32_t x) { w_gamma(r, x + 1u); }

static void bt_init_rate_e(BTE *t, int rate) { for (int i = 0; i < 256; i++) t->p[i] = RC_PHALF; t->rate = (uint8_t)rate; }

static void bt_encode(BTE *t, REnc *r, uint8_t byte) {
    int m = 1;
    for (int i = 7; i >= 0; i--) {
        int bit = (byte >> i) & 1;
        re_bit(r, &t->p[m], bit, t->rate);
        m = (m << 1) | bit;
    }
}

static void lit_tree_seed_e(const uint8_t *frm, size_t n, int parity, BTE *t, int rate) {
    uint32_t hist[256], w[512];
    for (int i = 0; i < 256; i++) hist[i] = 1;
    for (size_t i = 0; i < n; i++) if ((int)(i & 1) == parity) hist[frm[i]]++;
    for (int s = 0; s < 256; s++) w[256 + s] = hist[s];
    for (int m = 255; m >= 1; m--) w[m] = w[2*m] + w[2*m+1];
    t->p[0] = RC_PHALF; t->rate = (uint8_t)rate;
    for (int m = 1; m < 256; m++) {
        uint32_t num = w[2*m], den = w[m];
        uint32_t pr = den ? (2u * RC_PBIT * num + den) / (2u * den) : RC_PHALF;
        t->p[m] = (uint16_t)(pr < 1 ? 1 : (pr > RC_PBIT - 1 ? RC_PBIT - 1 : pr));
    }
}

static void ug_init_e(UGE *g, char code, int k) {
    g->code = (uint8_t)code; g->k = (uint8_t)k;
    for (int i = 0; i <= UG_CTX; i++) { g->u[i] = RC_PHALF; for (int j = 0; j <= UG_CTX; j++) g->m[i][j] = RC_PHALF; }
}

static int ug_c(int x) { return x < UG_CTX ? x : UG_CTX; }

/* mirror of decoder ugg_seed_cont: bias the first `depth` unary positions toward continue (bit 1). */
static void ug_seed_cont_e(UGE *g, int depth) {
    for (int i = 0; i < depth && i <= UG_CTX; i++) g->u[i] = (uint16_t)(RC_PBIT / 16);
}

static void ug_encode(UGE *g, REnc *r, uint32_t v) {
    uint32_t cl;
    if (g->code == 'r') {
        cl = v >> g->k;
        for (uint32_t pos = 0; pos < cl; pos++) re_bit(r, &g->u[ug_c((int)pos)], 1, RC_S_BIT_RATE);
        re_bit(r, &g->u[ug_c((int)cl)], 0, RC_S_BIT_RATE);
        for (int pos = 0; pos < g->k; pos++) re_bit(r, &g->m[ug_c((int)cl)][ug_c(pos)], (int)((v >> (g->k - 1 - pos)) & 1u), RC_S_BIT_RATE);
    } else {
        uint32_t mm = v + 1u;
        cl = (uint32_t)bitlen32(mm) - 1u;
        for (uint32_t pos = 0; pos < cl; pos++) re_bit(r, &g->u[ug_c((int)pos)], 1, RC_S_BIT_RATE);
        re_bit(r, &g->u[ug_c((int)cl)], 0, RC_S_BIT_RATE);
        for (uint32_t pos = 0; pos < cl; pos++) re_bit(r, &g->m[ug_c((int)cl)][ug_c((int)pos)], (int)((mm >> (cl - 1u - pos)) & 1u), RC_S_BIT_RATE);
    }
}

static void fl_init_e(FLE *f) { for (int i = 0; i < 4; i++) f->m[i] = RC_PHALF; f->h = 0; }
static void fl_encode(FLE *f, REnc *r, int b) { re_bit(r, &f->m[f->h], b, RC_S_BIT_RATE); f->h = ((f->h << 1) | b) & 3; }

/* ------------------------------------------------------------------------------------- */
/* LZSS planning and entropy models.                                                       */
/* ------------------------------------------------------------------------------------- */
typedef struct { int type; int32_t start, len, dist; } Token;
typedef struct { Token *v; size_t n, cap; } TokenVec;
typedef struct { int32_t dist, len; } Cand;

#ifndef LZ_CAND_MAX
#define LZ_CAND_MAX 128
#endif
#if LZ_CAND_MAX < 1
#error "LZ_CAND_MAX must be at least 1"
#endif

static void tok_push(TokenVec *v, Token t) {
    if (v->n == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 1024;
        v->v = (Token *)xrealloc(v->v, v->cap * sizeof(v->v[0]));
    }
    v->v[v->n++] = t;
}

static void huff_lengths(const uint32_t freq[256], uint8_t L[256]) {
    int parent[512], active[512], an = 256, m = 256;
    uint32_t wt[512];
    for (int i = 0; i < 512; i++) parent[i] = -1, wt[i] = 0;
    for (int i = 0; i < 256; i++) { wt[i] = freq[i]; active[i] = i; }
    while (an > 1) {
        int a = -1, b = -1;
        for (int ii = 0; ii < an; ii++) {
            int nid = active[ii];
            if (a < 0 || wt[nid] < wt[a] || (wt[nid] == wt[a] && nid < a)) { b = a; a = nid; }
            else if (b < 0 || wt[nid] < wt[b] || (wt[nid] == wt[b] && nid < b)) b = nid;
        }
        wt[m] = wt[a] + wt[b]; parent[a] = m; parent[b] = m;
        int wn = 0;
        for (int ii = 0; ii < an; ii++) if (active[ii] != a && active[ii] != b) active[wn++] = active[ii];
        active[wn++] = m++;
        an = wn;
    }
    for (int s = 0; s < 256; s++) {
        int d = 0, p = s;
        while (parent[p] != -1) { p = parent[p]; d++; }
        L[s] = (uint8_t)d;
    }
}

static void from_huff_lengths(const uint8_t *frm, size_t n, uint8_t L0[256], uint8_t L1[256]) {
    uint32_t f0[256], f1[256];
    for (int i = 0; i < 256; i++) f0[i] = f1[i] = 1;
    for (size_t i = 0; i < n; i++) { if (i & 1) f1[frm[i]]++; else f0[frm[i]]++; }
    huff_lengths(f0, L0);
    huff_lengths(f1, L1);
}

static uint64_t gammalen_u32(uint32_t x) { return (uint64_t)(2 * bitlen32(x) - 1); }

static int cand_value(Cand c) {
    int v = c.len * 16;
    for (int32_t d = c.dist; d > 1; d >>= 1) v--;
    return v;
}

static void cand_add(Cand cands[LZ_CAND_MAX], uint8_t *ncand, Cand c) {
    if (c.len < 3) return;
    for (uint8_t i = 0; i < *ncand; i++)
        if (cands[i].dist <= c.dist && cands[i].len >= c.len)
            return;
    uint8_t w = 0;
    for (uint8_t i = 0; i < *ncand; i++)
        if (!(c.dist <= cands[i].dist && c.len >= cands[i].len))
            cands[w++] = cands[i];
    *ncand = w;
    if (*ncand < LZ_CAND_MAX) {
        cands[(*ncand)++] = c;
        return;
    }
    int worst = 0, worstv = cand_value(cands[0]);
    for (int i = 1; i < LZ_CAND_MAX; i++) {
        int v = cand_value(cands[i]);
        if (v < worstv || (v == worstv && cands[i].dist > cands[worst].dist)) {
            worst = i;
            worstv = v;
        }
    }
    int cv = cand_value(c);
    if (cv > worstv || (cv == worstv && c.dist < cands[worst].dist)) cands[worst] = c;
}

/* ---- price feedback: fractional bit-prices measured from the real adaptive models ----
 * The DP proxy (1-bit flag + gamma/rice bit-length + static-Huffman litbits) systematically
 * mis-prices tokens because the wire uses *adaptive* range-coder models whose steady-state
 * probabilities diverge from the from-image Huffman seed and from a flat 1-bit flag. We run a
 * trial encode of the previous-pass token stream, read off the resulting model probabilities,
 * and turn them into static per-symbol prices (PR_SCALE-ths of a bit) for the next DP pass.
 * Encoder-only: the wire bytes are unchanged; only token *selection* improves. */
#define PR_SCALE 64        /* fixed-point: price units are 1/64 bit */

/* cost in 1/64-bit units of coding a single adaptive bit with P(0)=p/4096. */
/* log2(1+i/32)*2^16, i=0..32, for the fractional part of the fixed-point log2. */
static const uint16_t LOG2_FRAC[33] = {
    0,2909,5732,8473,11136,13727,16248,18704,21098,23433,25711,27936,30109,
    32234,34312,36346,38336,40286,42196,44068,45904,47705,49472,51207,52911,
    54584,56229,57845,59434,60997,62534,64047,65535
};
static uint32_t bit_price(uint32_t p, int bit) {
    /* -log2(prob) * PR_SCALE, prob = (bit? 4096-p : p)/4096 = pr/4096. */
    uint32_t pr = bit ? (RC_PBIT - p) : p;
    if (pr < 1) pr = 1;
    int e = bitlen32(pr) - 1;                  /* floor(log2(pr)), 0..11 */
    uint32_t intbits = (uint32_t)(12 - e) * PR_SCALE;
    uint32_t mant = pr << (16 - e);            /* pr*2^(16-e) in [2^16, 2^17) */
    uint32_t frac16 = mant - (1u << 16);       /* mantissa fraction, [0, 2^16) */
    uint32_t idx = frac16 >> 11;               /* table bucket 0..31 */
    uint32_t sub = frac16 & 2047;
    uint32_t l2 = LOG2_FRAC[idx] + ((LOG2_FRAC[idx + 1] - LOG2_FRAC[idx]) * sub >> 11); /* log2(1+f)*2^16 */
    uint32_t fracbits = (l2 * PR_SCALE) >> 16;
    return intbits - fracbits;                 /* (12 - log2(pr)) * PR_SCALE */
}

typedef struct {
    uint32_t flag_span, flag_match;          /* avg measured flag price, per chosen token kind */
    uint32_t rep0_yes, rep0_no;              /* avg measured rep0-flag price (reuse vs fresh distance) */
    uint32_t lit[2][256];                    /* per-parity per-byte literal price */
    UGE gs, gl, gd;                          /* snapshot of length/dist models for value pricing */
    int dk;
    int valid;
} PriceTab;

/* price a ug value under a frozen model snapshot (probabilities held constant across the value) */
static uint32_t ug_price(const UGE *g, uint32_t v) {
    uint32_t cl, cost = 0;
    if (g->code == 'r') {
        cl = v >> g->k;
        for (uint32_t pos = 0; pos < cl; pos++) cost += bit_price(g->u[ug_c((int)pos)], 1);
        cost += bit_price(g->u[ug_c((int)cl)], 0);
        for (int pos = 0; pos < g->k; pos++)
            cost += bit_price(g->m[ug_c((int)cl)][ug_c(pos)], (int)((v >> (g->k - 1 - pos)) & 1u));
    } else {
        uint32_t mm = v + 1u;
        cl = (uint32_t)bitlen32(mm) - 1u;
        for (uint32_t pos = 0; pos < cl; pos++) cost += bit_price(g->u[ug_c((int)pos)], 1);
        cost += bit_price(g->u[ug_c((int)cl)], 0);
        for (uint32_t pos = 0; pos < cl; pos++)
            cost += bit_price(g->m[ug_c((int)cl)][ug_c((int)pos)], (int)((mm >> (cl - 1u - pos)) & 1u));
    }
    return cost;
}

/* per-byte literal price under a frozen bit-tree snapshot */
static uint32_t bt_price(const BTE *t, uint8_t byte) {
    int m = 1; uint32_t cost = 0;
    for (int i = 7; i >= 0; i--) {
        int bit = (byte >> i) & 1;
        cost += bit_price(t->p[m], bit);
        m = (m << 1) | bit;
    }
    return cost;
}

/* Simulate the real adaptive content models over a token sequence to obtain steady-state
 * probabilities and average flag prices; fill a PriceTab for the next DP pass. */
static void measure_prices(const TokenVec *seq, const uint8_t *content, const uint8_t *tags,
                           const uint8_t *frm, size_t from_size, int dk, PriceTab *pt) {
    BTE lit[2];
    FLE flag;
    UGE gs, gl, gd;
    lit_tree_seed_e(frm, from_size, 0, &lit[0], 5);
    lit_tree_seed_e(frm, from_size, 1, &lit[1], 4);
    fl_init_e(&flag);
    ug_init_e(&gd, 'r', dk);
    ug_init_e(&gl, 'g', 0);
    ug_init_e(&gs, 'g', 0);
    REnc r; re_init(&r);                 /* drives adaptation; emitted bytes discarded */
    uint64_t fs_cost = 0, fm_cost = 0; uint32_t fs_n = 0, fm_n = 0;
    /* Mirror the rep0 last-distance flag so its price reflects real adaptation. */
    uint16_t rep0 = RC_REP0_INIT; int32_t last_dist = 0;
    uint64_t r0y_cost = 0, r0n_cost = 0; uint32_t r0y_n = 0, r0n_n = 0;
    for (size_t i = 0; i < seq->n; i++) {
        Token t = seq->v[i];
        if (t.type == 'S') {
            fs_cost += bit_price(flag.m[flag.h], 0); fs_n++;
            fl_encode(&flag, &r, 0);
            ug_encode(&gs, &r, (uint32_t)t.len - 1u);
            for (int32_t j = 0; j < t.len; j++) {
                size_t cp = (size_t)t.start + (size_t)j;
                bt_encode(&lit[tags[cp]], &r, content[cp]);
            }
        } else {
            fm_cost += bit_price(flag.m[flag.h], 1); fm_n++;
            fl_encode(&flag, &r, 1);
            if (t.dist == last_dist) {
                r0y_cost += bit_price(rep0, 1); r0y_n++;
                re_bit(&r, &rep0, 1, RC_S_BIT_RATE);
            } else {
                r0n_cost += bit_price(rep0, 0); r0n_n++;
                re_bit(&r, &rep0, 0, RC_S_BIT_RATE);
                ug_encode(&gd, &r, (uint32_t)t.dist - 1u);
                last_dist = t.dist;
            }
            ug_encode(&gl, &r, (uint32_t)t.len - 1u);
        }
    }
    buf_free(&r.out);
    pt->flag_span = fs_n ? (uint32_t)(fs_cost / fs_n) : PR_SCALE;
    pt->flag_match = fm_n ? (uint32_t)(fm_cost / fm_n) : PR_SCALE;
    /* Fall back to the prior-implied price when a flavor was never used in this parse. */
    pt->rep0_yes = r0y_n ? (uint32_t)(r0y_cost / r0y_n) : bit_price(RC_REP0_INIT, 1);
    pt->rep0_no  = r0n_n ? (uint32_t)(r0n_cost / r0n_n) : bit_price(RC_REP0_INIT, 0);
    for (int par = 0; par < 2; par++)
        for (int b = 0; b < 256; b++) pt->lit[par][b] = bt_price(&lit[par], (uint8_t)b);
    pt->gs = gs; pt->gl = gl; pt->gd = gd; pt->dk = dk;
    pt->valid = 1;
}

static TokenVec lz_parse_once(size_t n, const uint16_t *litbits,
                              Cand (*cands)[LZ_CAND_MAX], uint8_t *ncand, int (*dist_bits)(int32_t, int),
                              int dk) {
    uint32_t maxrun = 1024;
    uint64_t *cost = (uint64_t *)xmalloc((n + 1) * sizeof(uint64_t));
    Token *nxt = (Token *)xcalloc(n + 1, sizeof(Token));
    const uint64_t INF = UINT64_MAX / 4;
    cost[n] = 0;
    for (size_t ri = n; ri-- > 0;) {
        uint64_t best = INF;
        Token bt = {0};
        uint64_t runbits = 0;
        size_t lim = n < ri + maxrun ? n : ri + maxrun;
        for (size_t j = ri + 1; j <= lim; j++) {
            runbits += litbits[j - 1];
            uint64_t c = 1 + gammalen_u32((uint32_t)(j - ri)) + runbits + cost[j];
            if (c < best) { best = c; bt = (Token){ 'S', (int32_t)ri, (int32_t)(j - ri), 0 }; }
        }
        for (int ci = 0; ci < ncand[ri]; ci++) {
            int32_t bd = cands[ri][ci].dist, bl = cands[ri][ci].len;
            uint64_t db = (uint64_t)dist_bits(bd, dk);
            for (int32_t l = 3; l <= bl; l++) {
                uint64_t c = 1 + db + gammalen_u32((uint32_t)l) + cost[ri + (size_t)l];
                if (c < best) { best = c; bt = (Token){ 'R', (int32_t)ri, l, bd }; }
            }
        }
        cost[ri] = best;
        nxt[ri] = bt;
    }
    TokenVec tv = {0};
    for (size_t i = 0; i < n;) {
        Token t = nxt[i];
        tok_push(&tv, t);
        i += (size_t)t.len;
    }
    free(cost); free(nxt);
    return tv;
}

/* DP parse using measured fractional prices (PR_SCALE-ths of a bit), made rep0-aware:
 * the wire lets a match REUSE the immediately-previous match distance for one cheap flag
 * bit (rep0) instead of re-coding the whole gd distance value. That is a forward dependency
 * (the price of a match depends on the distance chosen earlier), so this is a FORWARD DP
 * carrying, per reachable position, the cheapest arrival cost plus the rep distance in effect
 * there. One state per position (cheapest arrival wins) keeps it linear; the chosen parse is
 * only ever ACCEPTED by the exact seq_real_cost gate in lz_optimal_c, so any approximation
 * here can never corrupt the wire -- it only changes which legal parse is tried.
 * Length/dist value prices are precomputed once per pass (frozen model snapshot). */
static TokenVec lz_parse_priced(size_t n, const uint8_t *content, const uint8_t *tags,
                                Cand (*cands)[LZ_CAND_MAX], uint8_t *ncand, const PriceTab *pt,
                                int W) {
    uint32_t maxrun = 1024, win = 1u << W;
    /* slen[L]=span flag+len price, mlen[L]=match len price; dpr[D]=fresh-distance value price. */
    size_t maxlen = n + 1;
    uint32_t *slen = (uint32_t *)xmalloc((maxlen + 1) * sizeof(uint32_t));
    uint32_t *mlen = (uint32_t *)xmalloc((maxlen + 1) * sizeof(uint32_t));
    uint32_t *dpr  = (uint32_t *)xmalloc(((size_t)win + 1) * sizeof(uint32_t));
    for (size_t L = 1; L <= maxlen; L++) {
        slen[L] = pt->flag_span + ug_price(&pt->gs, (uint32_t)L - 1u);
        mlen[L] = ug_price(&pt->gl, (uint32_t)L - 1u);
    }
    for (uint32_t D = 1; D <= win; D++) dpr[D] = ug_price(&pt->gd, D - 1u);
    /* match flag + fresh-distance flag + value, vs. match flag + reuse flag (rep0). */
    uint64_t fresh_extra = (uint64_t)pt->flag_match + pt->rep0_no;
    uint64_t reuse_extra = (uint64_t)pt->flag_match + pt->rep0_yes;
    const uint64_t INF = UINT64_MAX / 4;
    uint64_t *cost = (uint64_t *)xmalloc((n + 1) * sizeof(uint64_t));
    int32_t  *rep  = (int32_t *)xmalloc((n + 1) * sizeof(int32_t)); /* rep distance arriving at pos */
    Token *via = (Token *)xcalloc(n + 1, sizeof(Token));            /* token that arrives at pos */
    for (size_t i = 0; i <= n; i++) { cost[i] = INF; rep[i] = 0; }
    cost[0] = 0; rep[0] = 0; /* wire seeds last_dist = 0 (never equals a real distance >= 1) */
    for (size_t i = 0; i < n; i++) {
        if (cost[i] >= INF) continue;            /* unreachable along any cheap path */
        uint64_t ci = cost[i]; int32_t ri = rep[i];
        /* spans: rep distance unchanged */
        uint64_t runbits = 0;
        size_t lim = n < i + maxrun ? n : i + maxrun;
        for (size_t j = i + 1; j <= lim; j++) {
            runbits += pt->lit[tags[j - 1]][content[j - 1]];
            uint64_t c = ci + (uint64_t)slen[j - i] + runbits;
            if (c < cost[j]) { cost[j] = c; rep[j] = ri; via[j] = (Token){ 'S', (int32_t)i, (int32_t)(j - i), 0 }; }
        }
        /* matches: rep0 reuse when the candidate distance equals the incoming rep distance */
        for (int cix = 0; cix < ncand[i]; cix++) {
            int32_t bd = cands[i][cix].dist, bl = cands[i][cix].len;
            uint64_t dext = (bd == ri) ? reuse_extra : (fresh_extra + dpr[bd]);
            for (int32_t l = 3; l <= bl; l++) {
                size_t j = i + (size_t)l;
                uint64_t c = ci + dext + mlen[l];
                if (c < cost[j]) { cost[j] = c; rep[j] = bd; via[j] = (Token){ 'R', (int32_t)i, l, bd }; }
            }
        }
    }
    /* reconstruct backward from n */
    TokenVec tv = {0};
    size_t pos = n;
    while (pos > 0) { tok_push(&tv, via[pos]); pos = (size_t)via[pos].start; }
    for (size_t a = 0, b = tv.n; a + 1 < b; a++, b--) { Token t = tv.v[a]; tv.v[a] = tv.v[b - 1]; tv.v[b - 1] = t; }
    free(cost); free(rep); free(via); free(slen); free(mlen); free(dpr);
    return tv;
}

static void merge_adjacent_spans(TokenVec *tv) {
    size_t w = 0;
    for (size_t i = 0; i < tv->n; i++) {
        Token t = tv->v[i];
        if (w > 0 && t.type == 'S') {
            Token *p = &tv->v[w - 1];
            if (p->type == 'S' && p->start <= INT32_MAX - p->len &&
                p->start + p->len == t.start &&
                p->len <= INT32_MAX - t.len) {
                p->len += t.len;
                continue;
            }
        }
        tv->v[w++] = t;
    }
    tv->n = w;
}

static int fixed_dist_bits(int32_t d, int k) { (void)d; return k; }
static int rice_dist_bits(int32_t d, int k) { uint32_t v = (uint32_t)d - 1u; return (int)((v >> k) + 1u + (uint32_t)k); }

static int fit_k_tokens(const TokenVec *tv) {
    int best = 0;
    uint64_t bestc = UINT64_MAX;
    for (int k = 0; k < 16; k++) {
        uint64_t c = 0;
        for (size_t i = 0; i < tv->n; i++) if (tv->v[i].type == 'R') {
            uint32_t v = (uint32_t)tv->v[i].dist - 1u;
            c += (v >> k) + 1u + (uint32_t)k;
        }
        if (c < bestc) { bestc = c; best = k; }
    }
    return best;
}

/* exact body bit-cost of a token sequence under the real adaptive content models, in
 * 1/64-bit units; used only to validate that a price-feedback re-parse is a real win.
 * Adaptation is driven by the real encode functions on a throwaway range coder; each symbol
 * is priced against the model state captured just before it is encoded. */
static uint64_t seq_real_cost(const TokenVec *seq, const uint8_t *content, const uint8_t *tags,
                              const uint8_t *frm, size_t from_size, int dk) {
    BTE lit[2]; FLE flag; UGE gs, gl, gd;
    lit_tree_seed_e(frm, from_size, 0, &lit[0], 5);
    lit_tree_seed_e(frm, from_size, 1, &lit[1], 4);
    fl_init_e(&flag); ug_init_e(&gd, 'r', dk); ug_init_e(&gl, 'g', 0); ug_init_e(&gs, 'g', 0);
    uint16_t rep0 = RC_REP0_INIT; int32_t last_dist = 0;   /* mirror the wire's rep0 reuse */
    REnc r; re_init(&r);
    uint64_t cost = 0;
    for (size_t i = 0; i < seq->n; i++) {
        Token t = seq->v[i];
        if (t.type == 'S') {
            cost += bit_price(flag.m[flag.h], 0); fl_encode(&flag, &r, 0);
            cost += ug_price(&gs, (uint32_t)t.len - 1u); ug_encode(&gs, &r, (uint32_t)t.len - 1u);
            for (int32_t j = 0; j < t.len; j++) {
                size_t cp = (size_t)t.start + (size_t)j;
                cost += bt_price(&lit[tags[cp]], content[cp]); bt_encode(&lit[tags[cp]], &r, content[cp]);
            }
        } else {
            cost += bit_price(flag.m[flag.h], 1); fl_encode(&flag, &r, 1);
            if (t.dist == last_dist) {
                cost += bit_price(rep0, 1); re_bit(&r, &rep0, 1, RC_S_BIT_RATE);
            } else {
                cost += bit_price(rep0, 0); re_bit(&r, &rep0, 0, RC_S_BIT_RATE);
                cost += ug_price(&gd, (uint32_t)t.dist - 1u); ug_encode(&gd, &r, (uint32_t)t.dist - 1u);
                last_dist = t.dist;
            }
            cost += ug_price(&gl, (uint32_t)t.len - 1u); ug_encode(&gl, &r, (uint32_t)t.len - 1u);
        }
    }
    buf_free(&r.out);
    return cost;
}

static TokenVec lz_optimal_c(const uint8_t *data, size_t n, const uint16_t *litbits,
                             const uint8_t *tags, const uint8_t *frm, size_t from_size,
                             int W, int *k_out) {
    int32_t win = 1 << W, maxm = 2048;
    Cand (*cands)[LZ_CAND_MAX] = (Cand (*)[LZ_CAND_MAX])xcalloc(n ? n : 1, sizeof(Cand[LZ_CAND_MAX]));
    uint8_t *ncand = (uint8_t *)xcalloc(n ? n : 1, 1);
    int32_t *head = (int32_t *)xmalloc((1u << 24) * sizeof(int32_t));
    for (size_t i = 0; i < (1u << 24); i++) head[i] = -1;
    int32_t *prev = (int32_t *)xmalloc((n ? n : 1) * sizeof(int32_t));
    for (size_t i = 0; i < n; i++) prev[i] = -1;
    for (size_t i = 0; i < n; i++) {
        if (i + 3 <= n) {
            uint32_t key = (uint32_t)data[i] | ((uint32_t)data[i+1] << 8) | ((uint32_t)data[i+2] << 16);
            for (int32_t pj = head[key]; pj >= 0;) {
                int32_t dist = (int32_t)i - pj;
                if (dist > win) break;
                int32_t ml = (int32_t)((n - i) < (size_t)maxm ? (n - i) : (size_t)maxm), l = 0;
                while (l < ml && data[(size_t)pj + (size_t)l] == data[i + (size_t)l]) l++;
                cand_add(cands[i], &ncand[i], (Cand){ dist, l });
                pj = prev[pj];
            }
            prev[i] = head[key];
            head[key] = (int32_t)i;
        }
    }
    free(head); free(prev);
    TokenVec seq = lz_parse_once(n, litbits, cands, ncand, fixed_dist_bits, W);
    int k = fit_k_tokens(&seq);
    int parsed_k = -1;
    for (int pass = 0; pass < 8; pass++) {
        free(seq.v);
        seq = lz_parse_once(n, litbits, cands, ncand, rice_dist_bits, k);
        parsed_k = k;
        int nk = fit_k_tokens(&seq);
        if (nk == k) break;
        k = nk;
    }
    if (parsed_k != k) {
        free(seq.v);
        seq = lz_parse_once(n, litbits, cands, ncand, rice_dist_bits, k);
    }
    /* Price-feedback: re-parse using bit-prices measured from the real adaptive models, and
     * keep the result only if its exact modeled cost is strictly lower. Iterate to a fixpoint. */
    if (n) {
        uint64_t cur_cost = seq_real_cost(&seq, data, tags, frm, from_size, k);
        for (int pass = 0; pass < 4; pass++) {
            PriceTab pt;
            measure_prices(&seq, data, tags, frm, from_size, k, &pt);
            TokenVec cand_seq = lz_parse_priced(n, data, tags, cands, ncand, &pt, W);
            int nk = fit_k_tokens(&cand_seq);
            uint64_t cand_cost = seq_real_cost(&cand_seq, data, tags, frm, from_size, nk);
            if (cand_cost + 1 < cur_cost) {     /* require a real, non-noise improvement */
                free(seq.v); seq = cand_seq; cur_cost = cand_cost; k = nk;
            } else {
                free(cand_seq.v);
                break;
            }
        }
    }
    merge_adjacent_spans(&seq);
    *k_out = k;
    free(cands); free(ncand);
    return seq;
}

static void build_content(const OpVec *ops, uint32_t from_size, uint32_t to_size, Buf *content, Buf *tags, size_t **ends_out) {
    int FWD = to_size <= from_size;
    size_t nops = ops->n;
    size_t *ends = (size_t *)xmalloc((nops ? nops : 1) * sizeof(size_t));
    int32_t *tp0 = (int32_t *)xmalloc((nops ? nops : 1) * sizeof(int32_t));
    int32_t tp = 0;
    for (size_t i = 0; i < nops; i++) { tp0[i] = tp; tp += ops->v[i].diff_len + ops->v[i].extra_len; }
    for (size_t step = 0; step < nops; step++) {
        size_t oi = FWD ? step : nops - 1 - step;
        const Op *o = &ops->v[oi];
        IVec lits = {0};
        for (int32_t k = 0; k < o->diff_len; k++) if (o->diff[k]) ivec_push(&lits, k);
        Buf tmp = {0};
        put_uleb(&tmp, (uint32_t)lits.n);
        buf_write(content, tmp.d, tmp.n);
        for (size_t i = 0; i < tmp.n; i++) buf_put(tags, 0);
        tmp.n = 0;
        if (FWD) {
            int32_t prev = 0;
            for (size_t li = 0; li < lits.n; li++) {
                int32_t k = lits.v[li];
                put_uleb(&tmp, (uint32_t)(k - prev));
                buf_write(content, tmp.d, tmp.n);
                for (size_t i = 0; i < tmp.n; i++) buf_put(tags, 0);
                tmp.n = 0;
                buf_put(content, o->diff[k]); buf_put(tags, 0);
                prev = k;
            }
            int32_t exstart = tp0[oi] + o->diff_len;
            for (int32_t e = 0; e < o->extra_len; e++) { buf_put(content, o->extra[e]); buf_put(tags, (uint8_t)((exstart + e) & 1)); }
        } else {
            int32_t exstart = tp0[oi] + o->diff_len;
            for (int32_t e = o->extra_len - 1; e >= 0; e--) { buf_put(content, o->extra[e]); buf_put(tags, (uint8_t)((exstart + e) & 1)); }
            int32_t prev = o->diff_len;
            for (size_t r = lits.n; r-- > 0;) {
                int32_t k = lits.v[r];
                put_uleb(&tmp, (uint32_t)(prev - k));
                buf_write(content, tmp.d, tmp.n);
                for (size_t i = 0; i < tmp.n; i++) buf_put(tags, 0);
                tmp.n = 0;
                buf_put(content, o->diff[k]); buf_put(tags, 0);
                prev = k;
            }
        }
        ends[step] = content->n;
        free(lits.v); buf_free(&tmp);
    }
    free(tp0);
    *ends_out = ends;
}

typedef struct { uint32_t cc; int kind; int64_t delta; } Inj;
typedef struct { Inj *v; size_t n, cap; } InjVec;

static void inj_push(InjVec *v, uint32_t cc, int kind, int64_t delta) {
    if (v->n == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 16;
        v->v = (Inj *)xrealloc(v->v, v->cap * sizeof(v->v[0]));
    }
    v->v[v->n++] = (Inj){cc, kind, delta};
}

static InjVec op_delta_content_pos(const Op *o, int FWD, const uint8_t *frm, uint32_t from_size,
                                   int32_t fp0, const FieldDeltaVec *fd, const IVec *ldr) {
    IVec lits = {0};
    for (int32_t k = 0; k < o->diff_len; k++) if (o->diff[k]) ivec_push(&lits, k);
    InjVec out = {0};
    uint32_t cc = (uint32_t)uleb_len((uint32_t)lits.n);
    if (FWD) {
        size_t li_pf = 0;
        int32_t nextpos = -1;
        if (lits.n) {
            cc += (uint32_t)uleb_len((uint32_t)lits.v[0]) + 1u;
            nextpos = lits.v[0];
            li_pf = 1;
        }
#define CONSUME_F(KK) do { if ((KK) == nextpos) { if (li_pf < lits.n) { \
            cc += (uint32_t)uleb_len((uint32_t)(lits.v[li_pf] - lits.v[li_pf - 1])) + 1u; \
            nextpos = lits.v[li_pf++]; } else nextpos = -1; } } while (0)
        for (int32_t k = 0; k < o->diff_len;) {
            if (k + 4 <= o->diff_len) {
                Event ev = classify_field(frm, from_size, fd, ldr, o, fp0, k);
                if (ev.type == EV_BL || ev.type == EV_EX) { inj_push(&out, cc, ev.type, ev.delta); k += 4; continue; }
                if (ev.type == EV_SBL) { for (int b = 0; b < 4; b++) CONSUME_F(k + b); k += 4; continue; }
            }
            CONSUME_F(k); k++;
        }
#undef CONSUME_F
    } else {
        size_t li_pf = 0;
        int32_t nextpos = -1;
        if (lits.n) {
            cc += (uint32_t)uleb_len((uint32_t)(o->diff_len - lits.v[lits.n - 1])) + 1u;
            nextpos = lits.v[lits.n - 1];
            li_pf = 1;
        }
#define CONSUME_D(KK) do { if ((KK) == nextpos) { if (li_pf < lits.n) { \
            size_t idx = lits.n - 1 - li_pf; \
            cc += (uint32_t)uleb_len((uint32_t)(lits.v[idx + 1] - lits.v[idx])) + 1u; \
            nextpos = lits.v[idx]; li_pf++; } else nextpos = -1; } } while (0)
        cc += (uint32_t)o->extra_len;
        for (int32_t k = o->diff_len - 1; k >= 0;) {
            int32_t ks = k - 3;
            if (ks >= 0) {
                Event ev = classify_field(frm, from_size, fd, ldr, o, fp0, ks);
                if (ev.type == EV_BL || ev.type == EV_EX) { inj_push(&out, cc, ev.type, ev.delta); k -= 4; continue; }
                if (ev.type == EV_SBL) { for (int b = 3; b >= 0; b--) CONSUME_D(ks + b); k -= 4; continue; }
            }
            CONSUME_D(k); k--;
        }
#undef CONSUME_D
    }
    free(lits.v);
    return out;
}

static void bv_encode(BVE *v, REnc *r, int64_t x) {
    Buf tmp = {0};
    put_pack_size(&tmp, x);
    for (size_t i = 0; i < tmp.n; i++) bt_encode(&v->t, r, tmp.d[i]);
    buf_free(&tmp);
}

static void dr_init_e(DRE *d, int64_t *dic, int cap) {
    d->dic = dic; d->cap = (uint16_t)cap; d->K = 1; d->dic[0] = 0; d->last = 0; d->rh = 0; d->hit = DR_HIT_INIT;
    for (int i = 0; i < 4; i++) d->rep[i] = RC_PHALF;
}

/* tag0 literal tree split by previous-literal range (LIT0_SEL, LIT0_CTX contexts); tag1 single tree.
 * Mirrors rc_v3.c M_lit0[]/M_lit1 + g_litprev. */
#define LIT0_CTX 5
/* Mirrors rc_v3.c LIT0_SEL: 5-context tag0 selector, corpus-derived boundaries 0x20/0x3d/0x8e/0xf7. */
#define LIT0_SEL(p) ( (p)<0x20 ? 0 : (p)<0x3d ? 1 : (p)<0x8e ? 2 : (p)<0xf7 ? 3 : 4 )
typedef struct {
    BTE lit0[LIT0_CTX], lit1;
    FLE flag;
    BVE dval;
    UGE tc, gd, gl, gs, pg, cg, pgn, cgn, pg2, cg2, gdl, gel, gadj, dibl, diex;
    DRE dr_bl, dr_ex;
    int64_t dic_bl[DR_KCAP_BL], dic_ex[DR_KCAP_EX];
    uint16_t rep0; int32_t last_dist;   /* rep0 flag + last match distance (mirror rc_v3.c M_rep0/g_lastdist) */
} Models;

static void emit_delta(Models *M, REnc *r, int kind, int64_t delta) {
    DRE *D = kind == EV_BL ? &M->dr_bl : &M->dr_ex;
    UGE *gix = kind == EV_BL ? &M->dibl : &M->diex;
    int ri = D->rh | (D->last == 0 ? 2 : 0);
    if (delta == D->last) {
        re_bit(r, &D->rep[ri], 1, RC_S_BIT_RATE);
        D->rh = 1;
        return;
    }
    re_bit(r, &D->rep[ri], 0, RC_S_BIT_RATE);
    D->rh = 0;
    D->last = delta;
    int j = -1;
    for (int i = 0; i < D->K; i++) if (D->dic[i] == delta) { j = i; break; }
    if (j >= 0) {
        if (j == 0) die("unexpected delta dict index 0");
        re_bit(r, &D->hit, 1, RC_S_BIT_RATE);
        ug_encode(gix, r, (uint32_t)(j - 1));
        int64_t t = D->dic[j];
        for (int i = j; i > 0; i--) D->dic[i] = D->dic[i - 1];
        D->dic[0] = t;
    } else {
        if (D->K >= D->cap) die("delta dictionary cap exceeded");
        re_bit(r, &D->hit, 0, RC_S_BIT_RATE);
        bv_encode(&M->dval, r, delta);
        for (int i = D->K; i > 0; i--) D->dic[i] = D->dic[i - 1];
        D->dic[0] = delta;
        D->K++;
    }
}

static void emit_geom_pc(REnc *r, Models *M, const Op *o, const OpPC *pc) {
    ug_encode(&M->gdl, r, (uint32_t)o->diff_len);
    ug_encode(&M->gel, r, (uint32_t)o->extra_len);
    ug_encode(&M->gadj, r, zz32(o->adj));
    ug_encode(&M->pgn, r, (uint32_t)pc->pres.n);
    int32_t prev = 0;
    for (size_t i = 0; i < pc->pres.n; i++) { ug_encode(i ? &M->pg2 : &M->pg, r, (uint32_t)(pc->pres.v[i] - prev)); prev = pc->pres.v[i]; }
    ug_encode(&M->cgn, r, (uint32_t)pc->corr.n);
    prev = 0;
    for (size_t i = 0; i < pc->corr.n; i++) {
        ug_encode(i ? &M->cg2 : &M->cg, r, (uint32_t)(pc->corr.v[i].off - prev));
        prev = pc->corr.v[i].off;
        bt_encode(&M->dval.t, r, pc->corr.v[i].byte);
    }
}

static Buf encode_body(const OpVec *ops, const uint8_t *frm, uint32_t from_size, uint32_t to_size,
                       const FieldDeltaVec *fd, const OpPC *pc, int W) {
    int FWD = to_size <= from_size;
    Buf content = {0}, tags = {0};
    size_t *ends = NULL;
    build_content(ops, from_size, to_size, &content, &tags, &ends);
    uint8_t L0[256], L1[256];
    from_huff_lengths(frm, from_size, L0, L1);
    uint16_t *litbits = (uint16_t *)xmalloc((content.n ? content.n : 1) * sizeof(uint16_t));
    for (size_t i = 0; i < content.n; i++) litbits[i] = tags.d[i] ? L1[content.d[i]] : L0[content.d[i]];
    int kd = 0;
    TokenVec seq = lz_optimal_c(content.d, content.n, litbits, tags.d, frm, from_size, W, &kd);
    InjVec *inj = (InjVec *)xcalloc(ops->n ? ops->n : 1, sizeof(*inj));
    int32_t *fp0s = (int32_t *)xmalloc((ops->n ? ops->n : 1) * sizeof(int32_t));
    int32_t fp = 0;
    for (size_t i = 0; i < ops->n; i++) { fp0s[i] = fp; fp += ops->v[i].diff_len + ops->v[i].adj; }
    for (size_t step = 0; step < ops->n; step++) {
        size_t oi = FWD ? step : ops->n - 1 - step;
        IVec ldr = op_ldr_set(frm, fp0s[oi], ops->v[oi].diff_len, from_size);
        inj[step] = op_delta_content_pos(&ops->v[oi], FWD, frm, from_size, fp0s[oi], fd, &ldr);
        free(ldr.v);
    }
    Models M;
    memset(&M, 0, sizeof(M));
    for (int c = 0; c < LIT0_CTX; c++) lit_tree_seed_e(frm, from_size, 0, &M.lit0[c], 5);
    lit_tree_seed_e(frm, from_size, 1, &M.lit1, 4);
    fl_init_e(&M.flag);
    bt_init_rate_e(&M.dval.t, 4);
    ug_init_e(&M.tc, 'r', 11);
    ug_init_e(&M.gd, 'r', kd);
    ug_init_e(&M.gl, 'g', 0);
    ug_init_e(&M.gs, 'g', 0);
    ug_init_e(&M.pg, 'g', 0);
    ug_init_e(&M.cg, 'g', 0);
    ug_init_e(&M.pgn, 'g', 0);
    ug_init_e(&M.cgn, 'g', 0);
    ug_init_e(&M.pg2, 'g', 0);
    ug_init_e(&M.cg2, 'g', 0);
    ug_init_e(&M.gdl, 'g', 0);
    ug_init_e(&M.gel, 'g', 0);
    ug_init_e(&M.gadj, 'g', 0);
    ug_seed_cont_e(&M.gdl, 6);
    ug_seed_cont_e(&M.gadj, 3);
    ug_init_e(&M.dibl, 'g', 0);
    ug_init_e(&M.diex, 'g', 0);
    dr_init_e(&M.dr_bl, M.dic_bl, DR_KCAP_BL);
    dr_init_e(&M.dr_ex, M.dic_ex, DR_KCAP_EX);
    M.rep0 = RC_REP0_INIT; M.last_dist = 0;   /* rep0 prior toward 0; mirror rc_v3.c */
    REnc rc;
    re_init(&rc);
    ug_encode(&M.tc, &rc, (uint32_t)seq.n);
    put_raw_bits(&rc, (uint32_t)kd, 4);
    w_gz(&rc, (uint32_t)ops->n);
    size_t tok_i = 0, pos = 0, span_pos = 0;
    int tok_mode = 0;
    int32_t tok_left = 0;
    uint8_t prevlit = 0;   /* last literal byte emitted (any tag); seeds tag0 context. */
    Token cur = {0};
    #define START_TOKEN() do { \
        if (tok_i >= seq.n) die("content token underrun"); \
        cur = seq.v[tok_i++]; \
        if (cur.type == 'S') { fl_encode(&M.flag, &rc, 0); ug_encode(&M.gs, &rc, (uint32_t)cur.len - 1u); tok_mode = 'S'; tok_left = cur.len; span_pos = 0; } \
        else { fl_encode(&M.flag, &rc, 1); \
            if ((int32_t)cur.dist == M.last_dist) re_bit(&rc, &M.rep0, 1, RC_S_BIT_RATE); \
            else { re_bit(&rc, &M.rep0, 0, RC_S_BIT_RATE); ug_encode(&M.gd, &rc, (uint32_t)cur.dist - 1u); M.last_dist = (int32_t)cur.dist; } \
            ug_encode(&M.gl, &rc, (uint32_t)cur.len - 1u); tok_mode = 'R'; tok_left = cur.len; } \
    } while (0)
    #define EMIT_TO(ENDPOS) do { \
        size_t _end = (ENDPOS); \
        if (_end < pos || _end > content.n) die("invalid content cursor"); \
        while (pos < _end) { \
            if (!tok_mode) START_TOKEN(); \
            size_t n = (size_t)tok_left < (_end - pos) ? (size_t)tok_left : (_end - pos); \
            if (tok_mode == 'S') { \
                for (size_t _em_i = 0; _em_i < n; _em_i++) { \
                    uint8_t byte = content.d[(size_t)cur.start + span_pos + _em_i]; \
                    bt_encode(tags.d[pos] ? &M.lit1 : &M.lit0[LIT0_SEL(prevlit)], &rc, byte); \
                    prevlit = byte; pos++; \
                } \
                span_pos += n; \
            } else pos += n; \
            tok_left -= (int32_t)n; \
            if (tok_left == 0) tok_mode = 0; \
        } \
    } while (0)
    for (size_t step = 0; step < ops->n; step++) {
        size_t oi = FWD ? step : ops->n - 1 - step;
        emit_geom_pc(&rc, &M, &ops->v[oi], &pc[step]);
        size_t base = step == 0 ? 0 : ends[step - 1], op_end = ends[step];
        for (size_t ii = 0; ii < inj[step].n; ii++) {
            EMIT_TO(base + inj[step].v[ii].cc);
            emit_delta(&M, &rc, inj[step].v[ii].kind, inj[step].v[ii].delta);
        }
        EMIT_TO(op_end);
    }
    if (pos != content.n || tok_i != seq.n || tok_mode) die("content token cursor out of sync");
    #undef START_TOKEN
    #undef EMIT_TO
    Buf body = re_flush_opt(&rc);
    for (size_t i = 0; i < ops->n; i++) free(inj[i].v);
    free(inj); free(fp0s); free(ends); free(litbits); free(seq.v); buf_free(&content); buf_free(&tags);
    return body;
}

static void encode_a1(const char *from_dir, const char *to_dir, const char *blob_out, int W) {
    char *fbin = join2(from_dir, "watch.bin"), *tbin = join2(to_dir, "watch.bin");
    char *felf = join2(from_dir, "watch.elf"), *telf = join2(to_dir, "watch.elf");
    Buf from = slurp(fbin), to = slurp(tbin);
    Ranges fr = elf_ranges(felf, &from, "from");
    Ranges tr = elf_ranges(telf, &to, "to");
    BlockVec blocks[STREAM_N] = {{0}};
    Buf from_df = {0}, to_df = {0};
    data_format_encode(&from, &to, &fr, &tr, &from_df, &to_df, blocks);
    OpVec ops = bsdiff_ops(&from_df, &to_df);
    uint32_t from_size = (uint32_t)from.n, to_size = (uint32_t)to.n;
    FieldDeltaVec fd = build_field_deltas(&from, &fr, blocks);
    split_nonzero_diff_runs(&ops, &from_df, &to_df);
    coerce_reloc_literals(&ops, from.d, from_size, to_size, &fd);
    int32_t fp_end_s = 0;
    for (size_t i = 0; i < ops.n; i++) fp_end_s += ops.v[i].diff_len + ops.v[i].adj;
    uint8_t *presset = preserve_indices(&ops, from_size, to_size);
    uint8_t *corr = corrections_hybrid(&ops, from.d, to.d, &fd, from_size, to_size, presset);
    OpPC *pc = preserve_corr_per_op(&ops, from_size, to_size, presset, corr);
    int stats_on = getenv("A1_ENC_STATS") != NULL;
    EncStats st = stats_on ? collect_stats(&ops, from.d, from_size, to_size, &fd, pc) : (EncStats){0};
    Buf body = encode_body(&ops, from.d, from_size, to_size, &fd, pc, W);
    Buf blob = {0};
    buf_put_u32le(&blob, crc32_buf(from.d, from.n));
    put_uleb(&blob, from_size);
    put_uleb(&blob, to_size);
    put_uleb(&blob, (uint32_t)fp_end_s);
    /* The LZMA-style range coder always emits a leading 0x00 cache byte (re_init sets
     * cache=0/csz=1, so the first re_shift_low outputs cache+0 = 0). It carries no
     * information; the decoder used to skip it in rc_init. Drop it on the wire (-1 B/patch).
     * Bit-exact invariant: body.d[0] is always 0 here. */
    if (body.n > 0 && body.d[0] == 0) { buf_write(&blob, body.d + 1, body.n - 1); }
    else { die("range-coder leading byte not 0 — wire invariant broken"); }
    buf_put_u32le(&blob, crc32_buf(to.d, to.n));
    write_file(blob_out, blob.d, blob.n);
    if (stats_on) {
        fprintf(stderr,
                "A1_STATS from=%s to=%s bytes=%zu ops=%zu diff=%zu extra=%zu lit=%zu pres=%zu corr=%zu bl=%zu ex=%zu sbl=%zu\n",
                from_dir, to_dir, blob.n, st.ops, st.diff_bytes, st.extra_bytes, st.literals,
                st.preserves, st.corrections, st.bl_fields, st.ex_fields, st.suppressed_bl);
    }
    buf_free(&body); buf_free(&blob); buf_free(&from); buf_free(&to); buf_free(&from_df); buf_free(&to_df);
    free(presset); free(corr); free(fd.v);
    for (size_t i = 0; i < ops.n; i++) { free(ops.v[i].diff); free(ops.v[i].extra); free(pc[i].pres.v); free(pc[i].corr.v); }
    free(pc); free(ops.v);
    for (int s = 0; s < STREAM_N; s++) { for (size_t i = 0; i < blocks[s].n; i++) free(blocks[s].v[i].values); free(blocks[s].v); }
    free(fbin); free(tbin); free(felf); free(telf);
}

#ifdef RC_V3_ENC_MAIN
int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: %s <from_dir> <to_dir> <blob_out> <W>\n", argv[0]);
        return 2;
    }
    char *end = NULL;
    errno = 0;
    long Wl = strtol(argv[4], &end, 10);
    if (errno || end == argv[4] || *end || Wl <= 0 || Wl > 15)
        die("W must be an integer in 1..15 and must match decoder SA_W");
    int W = (int)Wl;
    encode_a1(argv[1], argv[2], argv[3], W);
    return 0;
}
#endif
