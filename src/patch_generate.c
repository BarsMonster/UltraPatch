/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 *
 * Host encoder CLI entry point.
 */
#include "enc_internal.h"

static void reject_patch_output(const char *patch_out, const char *input) {
    int conflict = file_alias(patch_out, input);
    if (conflict < 0) exit(2);
    if (conflict) {
        fprintf(stderr, "%s: patch output aliases input %s\n", patch_out, input);
        exit(2);
    }
}

/* Emit the full patch envelope into a fresh `blob`. The body's leading range-coder cache
 * byte is dropped on the wire. A short body is valid: the decoder zero-pads after the counted
 * bytes while initializing and consuming the range stream. */
static void emit_wire_blob(Buf *blob, uint32_t from_crc, uint32_t to_crc,
                           uint32_t from_size, uint32_t to_size, int desc,
                           int32_t fp_end, int32_t fp_start, const Buf *body) {
    if (body->n == 0 || body->d[0] != 0 || body->n - 1u > UINT32_MAX)
        die("encoder produced a malformed range-coder body");
    uint32_t zd = rc_zz32((int32_t)to_size - (int32_t)from_size);
    buf_put_u32le(blob, rc_wire_from_crc(from_crc));
    buf_put_u32le(blob, to_crc);
    put_uleb(blob, from_size);
    if (rc_dir_is_natural(from_size, to_size, desc)) put_uleb(blob, zd);
    else put_uleb_overlong(blob, zd);
    if (desc) put_uleb(blob, rc_zz32(fp_end - (int32_t)from_size));
    if (!desc) put_uleb(blob, rc_zz32(fp_start));
    put_uleb(blob, (uint32_t)(body->n - 1u));
    buf_write(blob, body->d + 1, body->n - 1);
}

void encode_patch(const char *from_image, const char *to_image, const char *patch_out) {
    reject_patch_output(patch_out, from_image);
    reject_patch_output(patch_out, to_image);
    Buf from = {0}, to = {0};
    if (read_file_buf(from_image, &from, MAX_IMAGE) ||
        read_file_buf(to_image, &to, MAX_IMAGE)) exit(2);
    uint32_t from_size = (uint32_t)from.n, to_size = (uint32_t)to.n;
    PlanPrep prep;
    plan_prepare(&prep, &from, &to);
    /* Op-plan sweep: every config runs the full pipeline in the natural apply direction; the
     * smallest complete envelope ships and ties keep the earliest entry. An infeasible config
     * returns an empty body and is skipped. */
    uint32_t from_crc = crc32_buf(from.d, from.n), to_crc = crc32_buf(to.d, to.n);
    EncCtx ctx = {0};
    Buf best_blob = {0}; EncStats best_st = {0}; int bestv = -1;
    int natural_bestv = -1;
    /* The opposite direction is a fallback: admit it only when no natural plan was feasible or
     * the natural winner needed hazard literalization. Once admitted, retry only that winner;
     * if no natural plan was feasible, try every variant. A clean natural winner is already
     * applicable, so skipping the opposite pass can only affect blob size. */
    int natdir = rc_natural_desc(from_size, to_size);
    for (int pass = 0; pass < 2; pass++) {           /* pass 0 = natural, pass 1 = unnatural */
        int dir = pass ? !natdir : natdir;           /* 0 = ascending (FWD), 1 = descending */
        if (pass && bestv >= 0 && !best_st.deg_engaged) break;
        if (pass) natural_bestv = bestv;
        ctx.fwd = (dir == 0);
        for (int v = 0; v < PLAN_SPEC_N; v++) {
            if (pass && natural_bestv >= 0 && v != natural_bestv) continue;
            PlanResult pr = plan_encode(&ctx, &from, &to, &prep, &PLAN_SPECS[v]);
            if (pr.body.n == 0) { buf_free(&pr.body); continue; }        /* config infeasible on the wire */
            Buf cand = {0};
            emit_wire_blob(&cand, from_crc, to_crc, from_size, to_size, dir, pr.fp_end, pr.fp_start, &pr.body);
            buf_free(&pr.body);                                          /* blob holds the copy */
            if (bestv < 0 || cand.n < best_blob.n) {
                buf_free(&best_blob); best_blob = cand; best_st = pr.st; bestv = v;
            } else buf_free(&cand);
        }
    }
    plan_prepare_free(&prep);
    if (bestv < 0) die("no feasible plan for this pair");
    /* Self-verification (patch_host_backend.c): apply the finished blob to `from` on the
     * REFERENCE decoder (the real patch_apply.h + an NVM page emulator) and require the
     * exact `to` image plus clean NVM write-safety. ultrapatch refuses to ship a patch it
     * cannot prove applies. */
    { const char *scerr = selfcheck(best_blob.d, best_blob.n, from.d, from.n, to.d, to.n);
      if (scerr) { fprintf(stderr, "self-verify: %s\n", scerr);
                   die("emitted patch failed reference-decoder self-verification"); } }
    write_file(patch_out, best_blob.d, best_blob.n);
    buf_free(&best_blob); buf_free(&from); buf_free(&to);
}

static void usage(FILE *out, const char *prog) {
    fprintf(out,
            "usage: %s [--encode] <from_image> <to_image> <patch>\n"
            "       %s --decode <image> <patch>\n"
            "\n"
            "Modes:\n"
            "  --encode      Create a patch; this is the default mode.\n"
            "  --decode      Apply a patch to <image> in place using the host decoder wrapper.\n"
            "\n"
            "Options:\n"
            "  -h, --help    Show this usage text.\n",
            prog, prog);
}

int main(int argc, char **argv) {
    int decode = 0, mode_set = 0;
    const char *pos[3] = {0};
    int npos = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--decode") == 0) {
            if (mode_set && !decode) { usage(stderr, argv[0]); return 2; }
            decode = 1; mode_set = 1;
        } else if (strcmp(argv[i], "--encode") == 0) {
            if (mode_set && decode) { usage(stderr, argv[0]); return 2; }
            decode = 0; mode_set = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(stdout, argv[0]);
            return 0;
        } else if (argv[i][0] == '-' && argv[i][1]) {
            usage(stderr, argv[0]);
            return 2;
        } else {
            if (npos == 3) { usage(stderr, argv[0]); return 2; }
            pos[npos++] = argv[i];
        }
    }
    if (decode) {
        if (npos != 2) { usage(stderr, argv[0]); return 2; }
        return decode_patch(pos[0], pos[1]);
    }
    if (npos != 3) {
        usage(stderr, argv[0]);
        return 2;
    }
    encode_patch(pos[0], pos[1], pos[2]);
    return 0;
}
