/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 *
 * A1 host encoder CLI entry point.
 */
#include <errno.h>

#include "enc_internal.h"

static char *elf_sidecar_path(const char *image_path) {
    const char *slash = strrchr(image_path, '/');
    const char *base = slash ? slash + 1 : image_path;
    const char *dot = strrchr(base, '.');
    size_t stem = dot ? (size_t)(dot - image_path) : strlen(image_path);
    char *elf = (char *)xmalloc(stem + 5u);
    memcpy(elf, image_path, stem);
    memcpy(elf + stem, ".elf", 5u);
    return elf;
}

static int optional_sidecar_present(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return 1; }
    if (errno == ENOENT || errno == ENOTDIR) return 0;
    perror(path);
    exit(2);
}

static uint32_t checked_image_size(const Buf *b, const char *name) {
    if (b->n > MAX_IMAGE) {
        fprintf(stderr, "%s image too large for this decoder build: %zu > %u\n",
                name, b->n, (unsigned)MAX_IMAGE);
        exit(2);
    }
    return (uint32_t)b->n;
}

/* Emit the full patch envelope into a fresh `blob`. The body's leading range-coder cache
 * byte is dropped on the wire. The validity guard is an encoder-bug-only path (a well-formed
 * plan_encode body always begins with a 0 tag and is >= 5 bytes). */
static void emit_wire_blob(Buf *blob, uint32_t from_crc, uint32_t to_crc,
                           uint32_t from_size, uint32_t to_size, int desc,
                           int32_t fp_end, int32_t fp_start, const Buf *body) {
    if (body->n < 5u || body->d[0] != 0 || body->n - 1u > UINT32_MAX)
        die("encoder produced a malformed range-coder body");
    uint32_t zd = rc_zz32((int32_t)to_size - (int32_t)from_size);
    buf_put_u32le(blob, from_crc);
    buf_put_u32le(blob, to_crc);
    put_uleb(blob, from_size);
    if (rc_dir_is_natural(from_size, to_size, desc)) put_uleb(blob, zd);
    else put_uleb_overlong(blob, zd);
    if (desc) put_uleb(blob, rc_zz32(fp_end - (int32_t)from_size));
    if (!desc) put_uleb(blob, rc_zz32(fp_start));
    put_uleb(blob, (uint32_t)(body->n - 1u));
    buf_write(blob, body->d + 1, body->n - 1);
}

void encode_a1(const char *from_image, const char *to_image, const char *patch_out) {
    int alias = file_alias(patch_out, from_image);
    if (alias < 0) exit(2);
    if (alias) {
        fprintf(stderr, "%s: patch output aliases input %s\n", patch_out, from_image);
        exit(2);
    }
    alias = file_alias(patch_out, to_image);
    if (alias < 0) exit(2);
    if (alias) {
        fprintf(stderr, "%s: patch output aliases input %s\n", patch_out, to_image);
        exit(2);
    }
    char *felf = elf_sidecar_path(from_image), *telf = elf_sidecar_path(to_image);
    Buf from = slurp(from_image), to = slurp(to_image);
    uint32_t from_size = checked_image_size(&from, "from"), to_size = checked_image_size(&to, "to");
    /* A same-basename .elf sidecar is OPTIONAL: without it the whole image is treated as opaque data (zeroed
     * Ranges -> no code/data windows for the disassembler filter; plain bsdiff+LZ path).
     * A present-but-malformed ELF still dies loudly — only absence is tolerated. This
     * enables raw-binary pairs (edge gate, foreign firmware without build artifacts). */
    Ranges fr = {0}, tr = {0};
    if (optional_sidecar_present(felf)) fr = elf_ranges(felf, &from, "from");
    if (optional_sidecar_present(telf)) tr = elf_ranges(telf, &to, "to");
    PairAnalysis pa;
    pair_analysis_init(&pa, &from, &to, &fr, &tr);
    PlanPrep prep;
    plan_prepare(&prep, &from, &to, &pa);
    /* Op-plan sweep: every config runs the full pipeline; the smallest exact body ships and
     * ties keep the earliest entry, so added configs can never regress. A config whose plan
     * exceeds a decoder resource cap (journal/corrections/DR dict) returns an empty body and
     * is skipped — including the legacy config 0, whose feasibility is only guaranteed on
     * in-family firmware. Every config below won pairs in the corpus sweep (the bsdiff fuzz
     * axis dominates: 74 pairs preferred fuzz=6, 92 preferred fuzz=20). */
    static const PlanCfg PLANS[] = {
        {0, 11}, {1, 11}, {2, 11}, {1, 6}, {1, 20},
    };
    enum { NPLANS = (int)(sizeof(PLANS) / sizeof(PLANS[0])) };
    uint32_t from_crc = crc32_buf(from.d, from.n), to_crc = crc32_buf(to.d, to.n);
    EncCtx ctx = {0};
    Buf best_blob = {0}; EncStats best_st = {0}; int bestv = -1; int best_desc = 0;
    int natural_bestv = -1;
    unsigned char natural_opc[NPLANS] = {0};
    size_t sweep_opc_splits = 0;   /* max OPC_CAP splits any plan variant needed (winner or not) */
    /* Direction sweep, PRUNED: the natural direction (descending iff growing) is swept first;
     * the unnatural direction is swept ONLY when the natural winner engaged journal-budget
     * degradation or no natural plan was feasible. Measured separator (foreign study +
     * home corpus): every pair that ever preferred the flip also degraded in the natural
     * direction (insertion shifts journal-overflow ascending), while home pairs never
     * degrade — so pruning halves encode time with byte-identical home output. A clean
     * natural winner that would still lose to the flip is unobserved in any corpus; if one
     * exists, the cost is a slightly larger blob, never a bad one. Within an admitted
     * unnatural pass, rerun the natural winner plus plans that needed OPC splitting;
     * clean natural nonwinners did not win a flip in the home/foreign matrix. */
    int natdir = rc_natural_desc(from_size, to_size);
    for (int pass = 0; pass < 2; pass++) {           /* pass 0 = natural, pass 1 = unnatural */
        int dir = pass ? !natdir : natdir;           /* 0 = ascending (FWD), 1 = descending */
        if (pass && bestv >= 0 && !best_st.deg_engaged) break;
        if (pass) natural_bestv = bestv;
        ctx.fwd = (dir == 0);
        for (int v = 0; v < NPLANS; v++) {
            if (pass && natural_bestv >= 0 && v != natural_bestv && !natural_opc[v]) continue;
            PlanResult pr = plan_encode(&ctx, &from, &to, &prep, PLANS[v]);
            if (!pass) natural_opc[v] = (unsigned char)(pr.st.opc_splits != 0);
            if (pr.st.opc_splits > sweep_opc_splits) sweep_opc_splits = pr.st.opc_splits;
            if (pr.body.n == 0) { buf_free(&pr.body); continue; }        /* config infeasible on the wire */
            Buf cand = {0};
            emit_wire_blob(&cand, from_crc, to_crc, from_size, to_size, dir, pr.fp_end, pr.fp_start, &pr.body);
            buf_free(&pr.body);                                          /* blob holds the copy */
            if (bestv < 0 || cand.n < best_blob.n) {
                buf_free(&best_blob); best_blob = cand; best_st = pr.st; bestv = v; best_desc = dir;
            } else buf_free(&cand);
        }
    }
    plan_prepare_free(&prep);
    if (bestv < 0) die("no feasible plan: every config exceeds a decoder resource cap for this pair");
    /* Wire-neutral degradation stat line for the SHIPPED plan. natural=1 => canonical size-delta
     * uLEB; natural=0 => unnatural apply direction signaled by the overlong marker. */
    if (getenv("DEGRADE_STATS"))
        fprintf(stderr,
                "DEGRADE dir=%s natural=%d deg_journal=%d pres_needed=%zu converted=%zu opc_splits=%zu opc_splits_sweep=%zu budget=%u opc_cap=%u\n",
                best_desc ? "bwd" : "fwd",
                rc_dir_is_natural(from_size, to_size, best_desc),
                best_st.deg_engaged, best_st.deg_pres_needed, best_st.deg_converted, best_st.opc_splits, sweep_opc_splits,
                (unsigned)JSLOTS, (unsigned)OPC_CAP);
    /* Self-verification (patch_host_backend.c): apply the finished blob to `from` on the
     * REFERENCE decoder (the real patch_apply.h + an NVM page emulator) and require the
     * exact `to` image plus clean NVM write-safety. ultrapatch refuses to ship a patch it
     * cannot prove applies. */
    { const char *scerr = selfcheck(best_blob.d, best_blob.n, from.d, from.n, to.d, to.n);
      if (scerr) { fprintf(stderr, "self-verify: %s\n", scerr);
                   die("emitted patch failed reference-decoder self-verification"); } }
    write_file(patch_out, best_blob.d, best_blob.n);
    buf_free(&best_blob); buf_free(&from); buf_free(&to);
    pair_analysis_free(&pa);
    free(felf); free(telf);
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
        return decode_a1(pos[0], pos[1]);
    }
    if (npos != 3) {
        usage(stderr, argv[0]);
        return 2;
    }
    encode_a1(pos[0], pos[1], pos[2]);
    return 0;
}
