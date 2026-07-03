/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 *
 * A1 host encoder module -- CLI: encode_a1, main (RC_V3_ENC_MAIN).
 * Part of the single translation unit rooted at src/patch_generate.c, which
 * #includes the enc_*.inc modules in dependency order. NOT a standalone TU:
 * it relies on the shared prologue (typedefs, file-scope state) and on symbols
 * defined by earlier modules. Umbrella split preserves the single-TU byte-exact
 * wire and model_diff.c's #include "patch_generate.c".
 */
static void encode_a1(const char *from_dir, const char *to_dir, const char *blob_out, int W) {
    char *fbin = join2(from_dir, "watch.bin"), *tbin = join2(to_dir, "watch.bin");
    char *felf = join2(from_dir, "watch.elf"), *telf = join2(to_dir, "watch.elf");
    Buf from = slurp(fbin), to = slurp(tbin);
    /* watch.elf is OPTIONAL: without it the whole image is treated as opaque data (zeroed
     * Ranges -> no code/data windows for the disassembler filter; plain bsdiff+LZ path).
     * A present-but-malformed ELF still dies loudly — only absence is tolerated. This
     * enables raw-binary pairs (edge gate, foreign firmware without build artifacts). */
    Ranges fr = {0}, tr = {0};
    { FILE *fe = fopen(felf, "rb");
      if (fe) { fclose(fe); fr = elf_ranges(felf, &from, "from"); } }
    { FILE *te = fopen(telf, "rb");
      if (te) { fclose(te); tr = elf_ranges(telf, &to, "to"); } }
    uint32_t from_size = (uint32_t)from.n, to_size = (uint32_t)to.n;
    int stats_on = getenv("A1_ENC_STATS") != NULL;
    /* Op-plan sweep: every config runs the full pipeline; the smallest exact body ships and
     * ties keep the earliest entry, so added configs can never regress. A config whose plan
     * exceeds a decoder resource cap (journal/corrections/DR dict) returns an empty body and
     * is skipped — including the legacy config 0, whose feasibility is only guaranteed on
     * in-family firmware. Every config below won pairs in the corpus sweep (the bsdiff fuzz
     * axis dominates: 74 pairs preferred fuzz=6, 92 preferred fuzz=20); variant 3
     * additionally masks literal-pool words (pointer churn) in the bsdiff inputs. */
    static const PlanCfg PLANS[] = {
        {0, 11}, {1, 11}, {2, 11}, {1, 6}, {1, 20}, {3, 11},
    };
    enum { NPLANS = (int)(sizeof(PLANS) / sizeof(PLANS[0])) };
    Buf body = {0}; int32_t fp_end_s = 0; EncStats st = {0}; int bestv = -1; int best_desc = 0;
    size_t best_tot = 0;
    size_t sweep_opc_splits = 0;   /* max OPC_CAP splits any plan variant needed (winner or not) */
    for (int dir = 0; dir < 2; dir++) {              /* 0 = ascending (FWD), 1 = descending */
        g_enc_fwd = (dir == 0);
        for (int v = 0; v < NPLANS; v++) {
            int32_t fpe = 0; EncStats stv = {0};
            Buf b = plan_encode(&from, &to, &fr, &tr, W, PLANS[v], &fpe, &stv, stats_on);
            if (stv.opc_splits > sweep_opc_splits) sweep_opc_splits = stv.opc_splits;
            if (b.n == 0) { buf_free(&b); continue; }        /* config infeasible on the wire */
            /* judge by SHIPPED total: descending additionally pays the fp_end envelope
             * varint, and the UNNATURAL direction pays the 1-byte overlong marker */
            size_t tot = b.n + (dir ? (size_t)uleb_len(zz32(fpe - (int32_t)from_size)) : 0u)
                             + (((to_size > from_size) ? 1 : 0) != dir ? 1u : 0u);
            if (bestv < 0 || tot < best_tot) { buf_free(&body); body = b; fp_end_s = fpe; st = stv; bestv = v; best_desc = dir; best_tot = tot; }
            else buf_free(&b);
        }
    }
    if (bestv < 0) die("no feasible plan: every config exceeds a decoder resource cap for this pair");
    if (getenv("A1_PLAN_DBG")) fprintf(stderr, "A1_PLAN variant=%d desc=%d\n", bestv, best_desc);
    /* Wire-neutral degradation stat line for the SHIPPED plan. natural=1 => canonical size-delta
     * uLEB; natural=0 => unnatural apply direction signaled by the overlong marker. */
    if (getenv("A1_DEGRADE_STATS"))
        fprintf(stderr,
                "A1_DEGRADE dir=%s natural=%d deg_journal=%d pres_needed=%zu converted=%zu opc_splits=%zu opc_splits_sweep=%zu budget=%u opc_cap=%u\n",
                best_desc ? "bwd" : "fwd",
                best_desc == (to_size > from_size ? 1 : 0),
                st.deg_engaged, st.deg_pres_needed, st.deg_converted, st.opc_splits, sweep_opc_splits,
                (unsigned)A1_JSLOTS, (unsigned)A1_OPC_CAP);
    Buf blob = {0};
    buf_put_u32le(&blob, crc32_buf(from.d, from.n));
    /* CRC32(to) rides in the HEADER immediately after CRC32(from) (moved off the old
     * 4-byte trailer). The decoder reads it up front (g_want_to) and verifies it over the
     * final image after apply — so the range-coded body length stays implicit (optimal
     * flush) WITHOUT the decoder needing a trailer-withhold ring. */
    buf_put_u32le(&blob, crc32_buf(to.d, to.n));
    put_uleb(&blob, from_size);
    /* to_size and fp_end are zigzag-delta-coded against from_size. A real firmware update keeps the
     * image size nearly constant and the FWD walk ends near from_size, so both signed deltas are tiny
     * (to_size delta is exactly 0 on an equal-size update) — far cheaper than the absolute uLEB.
     * The apply direction rides the same field: the NATURAL direction (descending iff growing)
     * is the canonical encoding — byte-identical to the historical envelope — and the unnatural
     * direction is one redundant continuation byte (overlong uLEB, value-neutral). */
    { uint32_t zd = zz32((int32_t)to_size - (int32_t)from_size);
      if (best_desc == (to_size > from_size ? 1 : 0)) put_uleb(&blob, zd);
      else put_uleb_overlong(&blob, zd); }
    /* fp_end is the source position at the end of the (FWD) op walk = the seed s->fp for the
     * grow (!FWD) direction. For FWD the decoder computes s->fp incrementally from 0, so fp_end is
     * only a redundant end-check there (CRC32(to) already validates) — omit it. For grow it is the
     * load-bearing start seed, so emit it (delta-coded against from_size: it lands near from_size). */
    if (best_desc) put_uleb(&blob, zz32(fp_end_s - (int32_t)from_size));
    /* The LZMA-style range coder always emits a leading 0x00 cache byte (re_init sets
     * cache=0/csz=1, so the first re_shift_low outputs cache+0 = 0). It carries no
     * information, so it is dropped from the wire.
     * Bit-exact invariant: body.d[0] is always 0 here. */
    if (body.n > 0 && body.d[0] == 0) { buf_write(&blob, body.d + 1, body.n - 1); }
    else { die("range-coder leading byte not 0 — wire invariant broken"); }
    /* No trailer: CRC32(to) already sits in the header above. The range body is the last thing
     * on the wire; its length is implicit and the decoder consumes it to EOF. */
    /* Self-verification (patch_selfcheck.c): apply the finished blob to `from` on the
     * REFERENCE decoder (the real patch_apply.h + an NVM row emulator) and require the
     * exact `to` image plus clean NVM write-safety. hy_enc refuses to ship a patch it
     * cannot prove applies. */
    { const char *scerr = a1_selfcheck(blob.d, blob.n, from.d, from.n, to.d, to.n);
      if (scerr) { fprintf(stderr, "self-verify: %s\n", scerr);
                   die("emitted patch failed reference-decoder self-verification"); } }
    write_file(blob_out, blob.d, blob.n);
    if (stats_on) {
        fprintf(stderr,
                "A1_STATS from=%s to=%s bytes=%zu ops=%zu diff=%zu extra=%zu lit=%zu pres=%zu corr=%zu bl=%zu ex=%zu sbl=%zu\n",
                from_dir, to_dir, blob.n, st.ops, st.diff_bytes, st.extra_bytes, st.literals,
                st.preserves, st.corrections, st.bl_fields, st.ex_fields, st.suppressed_bl);
    }
    buf_free(&body); buf_free(&blob); buf_free(&from); buf_free(&to);
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
