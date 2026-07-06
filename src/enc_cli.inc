/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 *
 * A1 host encoder module -- CLI: encode_a1, main (RC_V3_ENC_MAIN).
 * Part of the single translation unit rooted at src/patch_generate.c, which
 * #includes the enc_*.inc modules in dependency order. NOT a standalone TU:
 * it relies on the shared prologue (typedefs, EncCtx) and on symbols
 * defined by earlier modules. Umbrella split preserves the single-TU byte-exact
 * wire and model_diff.c's #include "patch_generate.c".
 */
static void encode_a1(const char *from_dir, const char *to_dir, const char *blob_out) {
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
    EncCtx ctx = {0};
    Buf body = {0}; int32_t fp_end_s = 0; int32_t fp_start_s = 0; EncStats st = {0}; int bestv = -1; int best_desc = 0;
    size_t best_tot = 0;
    size_t sweep_opc_splits = 0;   /* max OPC_CAP splits any plan variant needed (winner or not) */
    /* Direction sweep, PRUNED: the natural direction (descending iff growing) is swept first;
     * the unnatural direction is swept ONLY when the natural winner engaged journal-budget
     * degradation or no natural plan was feasible. Measured separator (foreign study +
     * home corpus): every pair that ever preferred the flip also degraded in the natural
     * direction (insertion shifts journal-overflow ascending), while home pairs never
     * degrade — so pruning halves encode time with byte-identical home output. A clean
     * natural winner that would still lose to the flip is unobserved in any corpus; if one
     * exists, the cost is a slightly larger blob, never a bad one. */
    int natdir = rc_natural_desc(from_size, to_size);
    for (int pass = 0; pass < 2; pass++) {           /* pass 0 = natural, pass 1 = unnatural */
        int dir = pass ? !natdir : natdir;           /* 0 = ascending (FWD), 1 = descending */
        if (pass && bestv >= 0 && !st.deg_engaged) break;
        ctx.fwd = (dir == 0);
        ctx.litdump = 0;
        for (int v = 0; v < NPLANS; v++) {
            int32_t fpe = 0, fps = 0; EncStats stv = {0};
            Buf b = plan_encode(&ctx, &from, &to, &fr, &tr, PLANS[v], &fpe, &fps, &stv);
            if (stv.opc_splits > sweep_opc_splits) sweep_opc_splits = stv.opc_splits;
            if (b.n == 0) { buf_free(&b); continue; }        /* config infeasible on the wire */
            /* judge by SHIPPED total: descending additionally pays the fp_end envelope
             * varint, and the UNNATURAL direction pays the 1-byte overlong marker. The 7B
             * initial-seek varint (fp_start) rides both directions equally, so it does not
             * shift the argmin — accounted in the emitted blob, omitted from this comparison. */
            size_t tot = b.n + (dir ? (size_t)rc_uleb_len(rc_zz32(fpe - (int32_t)from_size)) : 0u)
                             + (rc_dir_is_natural(from_size, to_size, dir) ? 0u : 1u);
            if (bestv < 0 || tot < best_tot) { buf_free(&body); body = b; fp_end_s = fpe; fp_start_s = fps; st = stv; bestv = v; best_desc = dir; best_tot = tot; }
            else buf_free(&b);
        }
    }
    if (bestv < 0) die("no feasible plan: every config exceeds a decoder resource cap for this pair");
    /* WIRE-NEUTRAL scaffold: re-run the WINNING plan once with the literal dump enabled so the dump
     * file holds exactly the tag0/tag1 span literals that ship (the sweep above ran with dumping off,
     * so loser configs never pollute it). Deterministic replay reproduces the identical body. */
    if (getenv("A1_LITDUMP")) {
        ctx.fwd = (best_desc == 0); ctx.litdump = 1;
        int32_t fpe = 0, fps = 0; EncStats stv = {0};
        Buf rb = plan_encode(&ctx, &from, &to, &fr, &tr, PLANS[bestv], &fpe, &fps, &stv);
        buf_free(&rb); ctx.litdump = 0;
    }
    /* Wire-neutral degradation stat line for the SHIPPED plan. natural=1 => canonical size-delta
     * uLEB; natural=0 => unnatural apply direction signaled by the overlong marker. */
    if (getenv("A1_DEGRADE_STATS"))
        fprintf(stderr,
                "A1_DEGRADE dir=%s natural=%d deg_journal=%d pres_needed=%zu converted=%zu opc_splits=%zu opc_splits_sweep=%zu budget=%u opc_cap=%u\n",
                best_desc ? "bwd" : "fwd",
                rc_dir_is_natural(from_size, to_size, best_desc),
                st.deg_engaged, st.deg_pres_needed, st.deg_converted, st.opc_splits, sweep_opc_splits,
                (unsigned)A1_JSLOTS, (unsigned)A1_OPC_CAP);
    Buf blob = {0};
    buf_put_u32le(&blob, crc32_buf(from.d, from.n));
    /* CRC32(to) rides in the HEADER immediately after CRC32(from) (moved off the old
     * 4-byte trailer). The decoder reads it up front (g_want_to) and verifies it over the
     * final image after apply — so the range-coded body length stays implicit without
     * needing a trailer-withhold ring. */
    buf_put_u32le(&blob, crc32_buf(to.d, to.n));
    put_uleb(&blob, from_size);
    /* to_size and fp_end are zigzag-delta-coded against from_size. A real firmware update keeps the
     * image size nearly constant and the FWD walk ends near from_size, so both signed deltas are tiny
     * (to_size delta is exactly 0 on an equal-size update) — far cheaper than the absolute uLEB.
     * The apply direction rides the same field: the NATURAL direction (descending iff growing)
     * is the canonical encoding — byte-identical to the historical envelope — and the unnatural
     * direction is one redundant continuation byte (overlong uLEB, value-neutral). */
    { uint32_t zd = rc_zz32((int32_t)to_size - (int32_t)from_size);
      if (rc_dir_is_natural(from_size, to_size, best_desc)) put_uleb(&blob, zd);
      else put_uleb_overlong(&blob, zd); }
    /* fp_end is the source position at the end of the (FWD) op walk = the seed s->fp for the
     * grow (!FWD) direction. For FWD the decoder computes s->fp incrementally from 0, so fp_end is
     * only a redundant end-check there (CRC32(to) already validates) — omit it. For grow it is the
     * load-bearing start seed, so emit it (delta-coded against from_size: it lands near from_size). */
    if (best_desc) put_uleb(&blob, rc_zz32(fp_end_s - (int32_t)from_size));
    /* Feature 7B: initial source seek (fp_start). Zero-output pure-seek ops are folded off the wire
     * (so `nops` can go and the decoder frontier-terminates); a LEADING zero-op's seek is carried
     * here instead. Seeds the FWD source walk (was implicitly 0) and is the grow landing point
     * (grow's final fp must equal it). Almost always 0 (1 byte) — a leading zero-op is rare. Shipped
     * for BOTH directions, right after the (grow-only) fp_end field. */
    put_uleb(&blob, rc_zz32(fp_start_s));
    /* The LZMA-style range coder always emits a leading 0x00 cache byte (re_init sets
     * cache=0/csz=1, so the first re_shift_low outputs cache+0 = 0). It carries no
     * information, so it is dropped from the wire.
     * Bit-exact invariant: body.d[0] is always 0 here. */
    if (body.n > 0 && body.d[0] == 0) { buf_write(&blob, body.d + 1, body.n - 1); }
    else { die("range-coder leading byte not 0 — wire invariant broken"); }
    /* No trailer: CRC32(to) already sits in the header above. */
    /* Self-verification (patch_selfcheck.c): apply the finished blob to `from` on the
     * REFERENCE decoder (the real patch_apply.h + an NVM row emulator) and require the
     * exact `to` image plus clean NVM write-safety. hy_enc refuses to ship a patch it
     * cannot prove applies. */
    { size_t consumed = 0;
      const char *scerr = a1_selfcheck(blob.d, blob.n, from.d, from.n, to.d, to.n, &consumed);
      if (scerr) { fprintf(stderr, "self-verify: %s\n", scerr);
                   die("emitted patch failed reference-decoder self-verification"); }
      if (consumed == 0 || consumed > blob.n) die("self-verify consumed byte count is invalid");
      if (consumed < blob.n) {
          blob.n = consumed;
          consumed = 0;
          scerr = a1_selfcheck(blob.d, blob.n, from.d, from.n, to.d, to.n, &consumed);
          if (scerr || consumed != blob.n) {
              if (scerr) fprintf(stderr, "self-verify: %s\n", scerr);
              die("trimmed patch failed reference-decoder self-verification");
          }
      } }
    write_file(blob_out, blob.d, blob.n);
    buf_free(&body); buf_free(&blob); buf_free(&from); buf_free(&to);
    free(fbin); free(tbin); free(felf); free(telf);
}

#ifdef RC_V3_ENC_MAIN
int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <from_dir> <to_dir> <blob_out>\n", argv[0]);
        return 2;
    }
    encode_a1(argv[1], argv[2], argv[3]);
    return 0;
}
#endif
