/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 *
 * Host encoder module -- suffix-sort bsdiff operations.
 * Compiled as a normal internal encoder translation unit.
 */

#include "enc_internal.h"

/* ------------------------------------------------------------------------------------- */
/* bsdiff op generation.                                                                  */
/* ------------------------------------------------------------------------------------- */

static int32_t matchlen(const uint8_t *from, int32_t from_size, const uint8_t *to,
                        int32_t to_size) {
    int32_t n = from_size < to_size ? from_size : to_size;
    int32_t i;
    for (i = 0; i < n; i++) {
        if (from[i] != to[i]) break;
    }
    return i;
}

/* Compare a middle suffix with the query after `skip` bytes already proved equal by both
 * enclosing suffix-array boundaries. Return the exact LCP as well as the old comparator sign.
 * Exhausting EITHER operand is deliberately equality: suffix/query prefix exhaustion therefore
 * keeps taking the begin/left branch exactly like memcmp(min(suffix_len, query_len)). */
static int suffix_cmp_lcp(const uint8_t *from, int32_t from_size,
                          const uint8_t *to, int32_t to_size, int32_t skip,
                          int32_t *lcp) {
    int32_t n = from_size < to_size ? from_size : to_size;
    int32_t i = skip;
    while (i < n) {
        if (from[i] != to[i]) {
            *lcp = i;
            return (from[i] > to[i]) - (from[i] < to[i]);
        }
        i++;
    }
    *lcp = i;
    return 0;
}

static int32_t suffix_search_lcp(const int32_t *sa, const uint8_t *from, int32_t from_size,
                                 const uint8_t *to, int32_t to_size,
                                 int32_t begin, int32_t end,
                                 int32_t begin_lcp, int32_t end_lcp, int32_t *pos) {
    if (end - begin < 2) {
        if (begin_lcp > end_lcp) { *pos = sa[begin]; return begin_lcp; }
        *pos = sa[end]; return end_lcp;
    }
    int32_t x = begin + (end - begin) / 2;
    int32_t skip = begin_lcp < end_lcp ? begin_lcp : end_lcp, x_lcp;
    int cmp = suffix_cmp_lcp(from + sa[x], from_size - sa[x], to, to_size, skip, &x_lcp);
    if (cmp < 0)
        return suffix_search_lcp(sa, from, from_size, to, to_size,
                                 x, end, x_lcp, end_lcp, pos);
    return suffix_search_lcp(sa, from, from_size, to, to_size,
                             begin, x, begin_lcp, x_lcp, pos);
}

static int32_t suffix_search(const int32_t *sa, const uint8_t *from, int32_t from_size,
                             const uint8_t *to, int32_t to_size, int32_t begin, int32_t end,
                             int32_t *pos) {
    int32_t begin_lcp = matchlen(from + sa[begin], from_size - sa[begin], to, to_size);
    int32_t end_lcp = matchlen(from + sa[end], from_size - sa[end], to, to_size);
    return suffix_search_lcp(sa, from, from_size, to, to_size,
                             begin, end, begin_lcp, end_lcp, pos);
}

static void emit_bsdiff_op(OpVec *ops, uint8_t *payload,
                           const uint8_t *from, int32_t from_size,
                           const uint8_t *to, int32_t to_size, int32_t scan,
                           int32_t pos, int32_t *last_scan_p,
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
    int32_t extra_pos = last_scan + diff_size;
    int32_t extra_size = scan - lenb - extra_pos;
    for (int32_t i = 0; i < diff_size; i++)
        payload[last_scan + i] = (uint8_t)(to[last_scan + i] - from[last_pos + i]);
    if (extra_size) memcpy(payload + extra_pos, to + extra_pos, (size_t)extra_size);
    opvec_push(ops, (Op){ diff_size, extra_size,
                          (pos - lenb) - (last_pos + diff_size) });
    *last_scan_p = scan - lenb;
    *last_pos_p = pos - lenb;
    *last_offset_p = pos - scan;
}

OpVec bsdiff_ops(const Buf *from_buf, const Buf *to, int fuzz) {
    uint8_t empty_source = 0;
    /* Some direct seam probes represent an empty Buf with d==NULL. suffix_search performs
     * zero-offset pointer arithmetic even though it never dereferences the empty source. */
    const uint8_t *from = from_buf->d ? from_buf->d : &empty_source;
    int32_t from_size = (int32_t)from_buf->n, to_size = (int32_t)to->n;
    int32_t *sa = (int32_t *)xmalloc(((size_t)from_size + 1) * sizeof(*sa));
    sa[0] = from_size;
    if (from_size && divsufsort(from, &sa[1], from_size) != 0)
        die("divsufsort failed");
    OpVec ops = {0};
    ops.payload = (uint8_t *)xmalloc(to->n);
    int32_t scan = 0, len = 0, last_scan = 0, last_pos = 0, last_offset = 0, pos = 0;
    while (scan < to_size) {
        int32_t from_score = 0;
        scan += len;
        for (int32_t scsc = scan; scan < to_size; scan++) {
            len = suffix_search(sa, from, from_size, to->d + scan, to_size - scan, 0, from_size, &pos);
            for (; scsc < scan + len; scsc++) {
                if ((scsc + last_offset < from_size) && (from[scsc + last_offset] == to->d[scsc])) from_score++;
            }
            if (((len == from_score) && (len != 0)) || (len > from_score + fuzz)) break;
            if ((scan + last_offset < from_size) && (from[scan + last_offset] == to->d[scan])) from_score--;
        }
        if ((len != from_score) || (scan == to_size))
            emit_bsdiff_op(&ops, ops.payload, from, from_size, to->d, to_size, scan, pos,
                           &last_scan, &last_pos, &last_offset);
    }
    free(sa);
    return ops;
}
