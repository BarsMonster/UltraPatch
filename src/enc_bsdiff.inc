/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 *
 * A1 host encoder module -- diff core: SequenceMatcher data blocks (create_patch_block/data_format_encode) + suffix-sort bsdiff ops (emit_bsdiff_op, bsdiff_ops).
 * Part of the single translation unit rooted at src/patch_generate.c, which
 * #includes the enc_*.inc modules in dependency order. NOT a standalone TU:
 * it relies on the shared prologue (typedefs, file-scope state) and on symbols
 * defined by earlier modules. Umbrella split preserves the single-TU byte-exact
 * wire and model_diff.c's #include "patch_generate.c".
 */
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
    a1_sort(b2j.v, b2j.n, sizeof(b2j.v[0]), cmp_b2j_val);
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
    a1_sort(raw.v, raw.n, sizeof(raw.v[0]), cmp_match);
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
        if (sz < 6) continue;
        sz += 1;
            int64_t *vals = (int64_t *)xmalloc((size_t)sz * sizeof(int64_t));
            int nz = 0;
            for (int32_t k = 0; k < sz; k++) {
            vals[k] = (int64_t)from_s->a[fo + k].val - (int64_t)to_s->a[to + k].val;
            if (vals[k] != 0) nz++;
        }
        if (nz < 5) { free(vals); continue; }
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

static void mask_bl_imms(const uint8_t *real, uint8_t *mut, size_t n);

/* Family gate for the Thumb-2 WIDE streams (B.W / LDR.W). On CORTEX_M0 these encodings
 * cannot occur in real code — any disassembler match is a spurious decode of data — so
 * their emission and block-matching are compiled out. The disassembler's Thumb-2 walk
 * recognition stays ON regardless (byte consumption defines field classification; the
 * CORTEX_M0 wire is frozen against it). */
#ifdef CORTEX_M0
enum { A1_EMIT_WIDE = 0 };
#else
enum { A1_EMIT_WIDE = 1 };
#endif

static void data_format_encode(const Buf *from, const Buf *to, const Ranges *fr, const Ranges *tr,
                               Buf *from_mut, Buf *to_mut, BlockVec blocks[STREAM_N], int mask_bl) {
    from_mut->d = (uint8_t *)xmalloc(from->n); from_mut->n = from_mut->cap = from->n;
    if (from->n) memcpy(from_mut->d, from->d, from->n);   /* from->d is NULL for an empty image (memcpy nonnull UB) */
    to_mut->d = (uint8_t *)xmalloc(to->n); to_mut->n = to_mut->cap = to->n;
    if (to->n) memcpy(to_mut->d, to->d, to->n);
    m4_stream_t fs[M4_NSTREAMS], ts[M4_NSTREAMS];
    if (a1_m4_disassemble(from->d, from->n, fr->data_off_begin, fr->data_begin, fr->data_end,
                          fr->code_begin, fr->code_end, A1_EMIT_WIDE, A1_EMIT_WIDE, fs)) die("from disassemble failed");
    if (a1_m4_disassemble(to->d, to->n, tr->data_off_begin, tr->data_begin, tr->data_end,
                          tr->code_begin, tr->code_end, A1_EMIT_WIDE, A1_EMIT_WIDE, ts)) die("to disassemble failed");
    create_patch_block(from_mut, to_mut, &fs[M4_DATA], &ts[M4_DATA], &blocks[STREAM_DATA]);
    create_patch_block(from_mut, to_mut, &fs[M4_CODE], &ts[M4_CODE], &blocks[STREAM_CODE]);
    if (A1_EMIT_WIDE) create_patch_block(from_mut, to_mut, &fs[M4_BW], &ts[M4_BW], &blocks[STREAM_BW]);
    create_patch_block(from_mut, to_mut, &fs[M4_BL], &ts[M4_BL], &blocks[STREAM_BL]);
    create_patch_block(from_mut, to_mut, &fs[M4_LDR], &ts[M4_LDR], &blocks[STREAM_LDR]);
    if (A1_EMIT_WIDE) create_patch_block(from_mut, to_mut, &fs[M4_LDRW], &ts[M4_LDRW], &blocks[STREAM_LDRW]);
    if (mask_bl & 1) {
        mask_bl_imms(from->d, from_mut->d, from->n);
        mask_bl_imms(to->d, to_mut->d, to->n);
    }
    if (mask_bl & 2) {
        /* zero ALL literal-pool words (each side's own disassembly, proper pool-skip logic) so
         * copies extend through pointer churn; the decoder-side EX mechanism + op-derived deltas
         * supply the values, and corrections absorb any mask-induced diff mismatch. */
        for (size_t i = 0; i < fs[M4_LDR].n; i++) {
            uint32_t a = fs[M4_LDR].a[i].addr;
            if ((size_t)a + 4 <= from_mut->n) memset(from_mut->d + a, 0, 4);
        }
        for (size_t i = 0; i < ts[M4_LDR].n; i++) {
            uint32_t a = ts[M4_LDR].a[i].addr;
            if ((size_t)a + 4 <= to_mut->n) memset(to_mut->d + a, 0, 4);
        }
    }
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
        if (s * 3 - i > sf * 3 - diff_size) { sf = s; diff_size = i; }
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

static OpVec bsdiff_ops(const Buf *from, const Buf *to, int fuzz) {
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
            if (((len == from_score) && (len != 0)) || (len > from_score + fuzz)) break;
            if ((scan + last_offset < from_size) && (from->d[scan + last_offset] == to->d[scan])) from_score--;
        }
        if ((len != from_score) || (scan == to_size))
            emit_bsdiff_op(&ops, from->d, from_size, to->d, to_size, debuf, scan, pos,
                           &last_scan, &last_pos, &last_offset);
    }
    free(sa); free(debuf);
    return ops;
}
