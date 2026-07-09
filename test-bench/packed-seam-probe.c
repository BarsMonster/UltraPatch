/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 *
 * Host-planner contract probe for the 24-bit packed-position seam. Including the real
 * plan module keeps its normalization helpers private in production while letting this
 * first-party TU exercise them directly.
 */
#include "enc_internal.h"
#include "../src/enc_plan.c"

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "packed-seam check failed at line %d: %s\n", __LINE__, #x); \
    return 1; \
} } while (0)

static Op zero_op(int32_t dl, int32_t el, int32_t adj) {
    return (Op){ dl, el, adj };
}

static Buf wire_blob(uint32_t from_crc, uint32_t to_crc,
                     uint32_t from_size, uint32_t to_size, int desc,
                     int32_t fp_end, int32_t fp_start, const Buf *body) {
    Buf blob = {0};
    uint32_t zd = rc_zz32((int32_t)to_size - (int32_t)from_size);
    if (body->n < 5u || body->d[0] != 0) die("bad seam-probe body");
    buf_put_u32le(&blob, from_crc); buf_put_u32le(&blob, to_crc);
    put_uleb(&blob, from_size);
    if (rc_dir_is_natural(from_size, to_size, desc)) put_uleb(&blob, zd);
    else put_uleb_overlong(&blob, zd);
    if (desc) put_uleb(&blob, rc_zz32(fp_end - (int32_t)from_size));
    else put_uleb(&blob, rc_zz32(fp_start));
    put_uleb(&blob, (uint32_t)(body->n - 1u));
    buf_write(&blob, body->d + 1, body->n - 1u);
    return blob;
}

static int verify_plan(const EncCtx *ctx, const OpVec *ops, const Buf *from, const Buf *to,
                       const FieldDeltaVec *fd, const OpPC *pc, const PlanCaps *caps,
                       int32_t fp_start, int desc) {
    int overflow = 0;
    Buf body = encode_body(ctx, ops, from->d, (uint32_t)from->n,
                           to->d, (uint32_t)to->n, fd, pc, fp_start, &overflow);
    CHECK(!overflow);
    Buf blob = wire_blob(crc32_buf(from->d, from->n), crc32_buf(to->d, to->n),
                         (uint32_t)from->n, (uint32_t)to->n, desc,
                         caps->fp_end, fp_start, &body);
    const char *err = selfcheck(blob.d, blob.n, from->d, from->n, to->d, to->n);
    CHECK(err == NULL);
    buf_free(&body); buf_free(&blob);
    return 0;
}

/* Descending apply sees one unrepresentable preserve first, then exactly JSLOTS eligible
 * lower positions. The high byte must degrade to an extra without consuming a journal slot. */
static int check_preserve_seam(void) {
    const int32_t lim = (int32_t)RC_PACKED_POS_LIMIT;
    const int32_t kept = (int32_t)JSLOTS;
    const int32_t a0 = lim - kept;
    const int32_t hazard_n = kept + 1;
    const int32_t tail_n = lim + 1 - hazard_n;
    const uint32_t to_n = (uint32_t)lim + 1u;
    const uint32_t from_n = (uint32_t)(a0 + hazard_n + tail_n);
    Buf from = { (uint8_t *)xcalloc(from_n, 1), from_n, from_n };
    Buf to = { (uint8_t *)xcalloc(to_n, 1), to_n, to_n };
    to.d[kept] = 0xa5u;
    FieldDeltaVec fd = {0};
    EncCtx ctx = {0}; ctx.fwd = 0;
    OpVec ops = {0};
    ops.payload = (uint8_t *)xcalloc(to_n, 1);
    ops.payload[kept] = 0x3cu; /* normalized residual, deliberately unlike true target */
    opvec_push(&ops, zero_op(0, 0, a0));       /* folded into fp_start after degradation */
    opvec_push(&ops, zero_op(hazard_n, 0, 0)); /* source [a0,lim] -> low output */
    opvec_push(&ops, zero_op(tail_n, 0, 0));   /* descending writer crosses [a0,lim] first */

    PlanCaps before;
    OpPC *pc = preserve_corrections_pc(&ctx, &ops, 0, from.d, to.d, &fd,
                                        from_n, to_n, &before);
    CHECK(!before.ok && before.pres_total == JSLOTS + 1u);
    CHECK(before.pres_kept == JSLOTS && before.pres_cutoff < 0);
    oppc_array_free(pc, ops.n);

    degrade_ops_to_journal_budget(&ctx, &ops, &to, from.d, &fd, from_n, to_n);
    CHECK(ctx.deg_engaged && ctx.deg_converted == 1u);
    CHECK(ops.payload[kept] == to.d[kept]); /* degraded extras come from true `to` */
    int32_t fp_start = fold_zero_ops(&ops);
    CHECK(fp_start == a0);
    PlanCaps caps;
    pc = build_pc_fixpoint(&ctx, &ops, fp_start, &from, &to, &fd, &caps);
    CHECK(caps.ok && caps.pres_total == JSLOTS && caps.pres_kept == JSLOTS);
    CHECK(verify_plan(&ctx, &ops, &from, &to, &fd, pc, &caps, fp_start, 1) == 0);

    oppc_array_free(pc, ops.n); opvec_free(&ops);
    buf_free(&from); buf_free(&to);
    return 0;
}

/* The shared arena's byte domain follows geometry: bsdiff/split extras carry normalized
 * target bytes, while the journal-degraded case above deliberately checks true target bytes. */
static int check_normalized_extra_origins(void) {
    Buf empty = { (uint8_t *)xcalloc(1, 1), 0, 1 };
    Buf norm = { (uint8_t *)xmalloc(8), 8, 8 };
    memset(norm.d, 0x6du, norm.n);
    OpVec ops = bsdiff_ops(&empty, &norm, 11);
    CHECK(ops.n == 1u && ops.v[0].diff_len == 0 && ops.v[0].extra_len == 8);
    CHECK(memcmp(ops.payload, norm.d, norm.n) == 0); /* original extras use normalized `to_df` */
    opvec_free(&ops); buf_free(&empty); buf_free(&norm);

    enum { N = 256 };
    Buf from = { (uint8_t *)xmalloc(N), N, N };
    norm = (Buf){ (uint8_t *)xmalloc(N), N, N };
    memset(from.d, 0x11, N); memset(norm.d, 0x55, N);
    ops = (OpVec){0}; ops.payload = (uint8_t *)xmalloc(N);
    memset(ops.payload, 0x44, N); opvec_push(&ops, zero_op(N, 0, 0));
    EncCtx ctx = {0}; ctx.fwd = 1;
    split_nonzero_diff_runs(&ctx, &ops, &from, &norm);
    size_t extras = 0; int32_t tp = 0;
    for (size_t i = 0; i < ops.n; i++) {
        for (int32_t e = 0; e < ops.v[i].extra_len; e++)
            CHECK(ops.payload[tp + ops.v[i].diff_len + e] == 0x55u);
        extras += (size_t)ops.v[i].extra_len;
        tp += ops.v[i].diff_len + ops.v[i].extra_len;
    }
    CHECK(extras != 0); /* split-generated extras were actually selected */
    opvec_free(&ops); buf_free(&from); buf_free(&norm);
    return 0;
}

/* A correction at local offset 2^24 is valid image geometry but cannot be packed. The
 * correction fixpoint must split the op at that boundary and rebase it to offset zero. */
static int check_correction_seam(void) {
    const uint32_t lim = RC_PACKED_POS_LIMIT;
    const uint32_t n = lim + 17u;
    Buf from = { (uint8_t *)xcalloc(n, 1), n, n };
    Buf to = { (uint8_t *)xcalloc(n, 1), n, n };
    to.d[lim] = 1;
    FieldDeltaVec fd = {0};
    EncCtx ctx = {0}; ctx.fwd = 1;
    OpVec ops = {0}; ops.payload = (uint8_t *)xcalloc(n, 1);
    opvec_push(&ops, zero_op((int32_t)n, 0, 0));

    PlanCaps before;
    OpPC *pc = preserve_corrections_pc(&ctx, &ops, 0, from.d, to.d,
                                       &fd, n, n, &before);
    CHECK(!before.ok && pc[0].corr.n == 1u && (uint32_t)pc[0].corr.v[0].off == lim);
    oppc_array_free(pc, ops.n);

    PlanCaps caps;
    pc = build_pc_fixpoint(&ctx, &ops, 0, &from, &to, &fd, &caps);
    CHECK(caps.ok && ctx.opc_splits == 1u && ops.n == 2u);
    CHECK(pc[1].corr.n == 1u && pc[1].corr.v[0].off == 0);
    CHECK(verify_plan(&ctx, &ops, &from, &to, &fd, pc, &caps, 0, 0) == 0);

    oppc_array_free(pc, ops.n); opvec_free(&ops);
    buf_free(&from); buf_free(&to);
    return 0;
}

/* Repeated boundary cuts must also work after the first cut leaves an extra-only op.
 * Hand-built PC rows isolate split geometry without asking the LZ planner to materialize
 * a 16 MiB extra stream; the selfchecked case above covers the real PC/fixpoint path. */
static int check_repeated_extra_split(void) {
    const int32_t lim = (int32_t)RC_PACKED_POS_LIMIT;
    EncCtx ctx = {0}; ctx.fwd = 1;
    OpVec ops = {0}; opvec_push(&ops, zero_op(lim, lim + 17, 7));
    OpPC *pc = (OpPC *)xcalloc(1, sizeof(*pc));
    corr_push(&pc[0].corr, 2 * lim, 1);
    CHECK(split_overfull_corrections(&ctx, &ops, pc, 0));
    oppc_array_free(pc, 1);
    CHECK(ops.n == 2u && ops.v[0].diff_len == lim && ops.v[0].extra_len == 0);
    CHECK(ops.v[1].diff_len == 0 && ops.v[1].extra_len == lim + 17 && ops.v[1].adj == 7);

    pc = (OpPC *)xcalloc(ops.n, sizeof(*pc));
    corr_push(&pc[1].corr, lim, 1);
    CHECK(split_overfull_corrections(&ctx, &ops, pc, 1));
    oppc_array_free(pc, 2);
    CHECK(ctx.opc_splits == 2u && ops.n == 3u);
    CHECK(ops.v[1].diff_len == 0 && ops.v[1].extra_len == lim && ops.v[1].adj == 0);
    CHECK(ops.v[2].diff_len == 0 && ops.v[2].extra_len == 17 && ops.v[2].adj == 7);
    OpWalkEnt *walk = opwalk_build(&ops, 13);
    CHECK(walk[0].tp == 0 && walk[0].fp == 13);
    CHECK(walk[1].tp == lim && walk[1].fp == 13 + lim);
    CHECK(walk[2].tp == 2 * lim && walk[2].fp == 13 + lim);
    free(walk); opvec_free(&ops);
    return 0;
}

int main(void) {
    if (check_normalized_extra_origins()) return 1;
    printf("payload_origins=OK original=normalized split=normalized journal=true\n");
    if (check_preserve_seam()) return 1;
    printf("packed_seam_preserve=OK high_unpacked=1 journal_kept=%u converted=1\n",
           (unsigned)JSLOTS);
    if (check_correction_seam()) return 1;
    if (check_repeated_extra_split()) return 1;
    printf("packed_seam_correction=OK high_offset=%u rebased_offset=0 splits=1 extra_splits=2\n",
           (unsigned)RC_PACKED_POS_LIMIT);
    return 0;
}
