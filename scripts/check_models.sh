#!/usr/bin/env bash
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

# Model/wire contract probe. Compiles one TU that includes the real device
# decoder header set and the host encoder internals, then asserts invariants
# that would otherwise drift silently between comments, structs, and helpers.
set -eu

# CC/CFLAGS are the project build contract; the Makefile (check-models-internal) supplies
# both. Require them rather than carrying a hardcoded flag copy here that would silently drift
# from the real CFLAGS block. Direct invocation must therefore go through `make check-models`.
: "${CC:?check_models.sh: CC not set — invoke via 'make check-models' (it supplies CC/CFLAGS)}"
: "${CFLAGS:?check_models.sh: CFLAGS not set — invoke via 'make check-models' (it supplies CC/CFLAGS)}"

. "$(dirname "$0")/tempdir.sh"

cat > "$tmp/model_probe.c" <<'EOF'
#include "enc_internal.h"

/* patch_apply.h seals the non-knob model macros (UG_*, RC_*, BT_*, LIT0_CTX, IDX_CTX, DR_HIT_INIT)
 * at the end of its include so they don't leak into integrator TUs. This probe still asserts on
 * their values, so snapshot them into enum constants BEFORE that seal; the macros are compile-time
 * constants, so the checks below are semantically identical against the snapshots. */
enum {
    PROBE_UG_CTX        = UG_CTX,
    PROBE_UG_GAMMA_MANT = UG_GAMMA_MANT,
    PROBE_RC_PROB_BITS  = RC_PROB_BITS,
    PROBE_RC_PBIT       = RC_PBIT,
    PROBE_RC_PHALF      = RC_PHALF,
    PROBE_RC_PROB_BOUND = RC_PROB_BOUND(0xffffffffu, RC_PHALF),
    PROBE_BT_PROBS      = BT_PROBS,
    PROBE_LIT0_CTX      = LIT0_CTX,
    PROBE_IDX_CTX       = IDX_CTX,
    PROBE_DR_HIT_INIT   = DR_HIT_INIT
};

#include "patch_apply.h"

#define CHECK(x) do { if(!(x)) return __LINE__; } while(0)
#define COUNT_OF(x) (sizeof(x) / sizeof((x)[0]))

_Static_assert(PATHE_W == SA_W, "encoder/decoder LZ windows must match");
_Static_assert(A1_JSLOTS == JSLOTS, "encoder/decoder journal caps must match");
_Static_assert(A1_OPC_CAP == OPC_CAP, "encoder/decoder per-op correction caps must match");
_Static_assert(A1_OUTROW == OUTROW, "encoder/decoder row sizes must match");
_Static_assert(A1_ROW_DEPTH == OUTROW_DEPTH, "encoder/decoder row depths must match");

uint8_t flash_read(uint32_t addr){ return (uint8_t)addr; }
void flash_write(uint32_t addr, uint8_t val){ (void)addr; (void)val; }

static int no_bytes(void *ctx, uint8_t *out){
    (void)ctx;
    (void)out;
    return 0;
}

static int check_gamma_index(void){
    uint8_t seen[PROBE_UG_GAMMA_MANT] = {0};
    int used = 0;
    for(int row = 1; row < PROBE_UG_CTX; row++){
        for(int pos = 0; pos < row; pos++){
            int idx = rc_ugg_mant_idx(row, pos);
            CHECK(idx >= 0 && idx < PROBE_UG_GAMMA_MANT);
            CHECK(!seen[idx]);
            seen[idx] = 1;
            used++;
        }
    }
    for(int pos = 0; pos <= PROBE_UG_CTX; pos++){
        int idx = rc_ugg_mant_idx(PROBE_UG_CTX, pos);
        CHECK(idx >= 0 && idx < PROBE_UG_GAMMA_MANT);
        CHECK(!seen[idx]);
        seen[idx] = 1;
        used++;
    }
    CHECK(used == PROBE_UG_GAMMA_MANT);
    for(int i = 0; i < PROBE_UG_GAMMA_MANT; i++) CHECK(seen[i]);
    return 0;
}

static int check_shared_models(void){
    A1BitTree bt;
    A1Flag1 fl;
    A1IdxUnary idx;
    A1DRStream ds;
    int32_t dic[4] = { 123, 456, 789, 111 };
    CHECK(PROBE_RC_PROB_BITS == 12);
    CHECK(PROBE_RC_PBIT == (1 << PROBE_RC_PROB_BITS));
    CHECK((uint32_t)PROBE_RC_PROB_BOUND == (0xffffffffu >> 12) * (uint32_t)PROBE_RC_PHALF);
    a1_bt_init(&bt);
    for(int i = 0; i < (int)PROBE_BT_PROBS; i++) CHECK(a1_bt_get(&bt, i) == PROBE_RC_PHALF);
    for(int p = 0; p < 256; p++) CHECK(rc_lit0_sel((uint8_t)p) < PROBE_LIT0_CTX);
    a1_fl_init(&fl);
    CHECK(fl.h == 0);
    for(int i = 0; i < 4; i++) CHECK(fl.m[i] == PROBE_RC_PHALF);
    a1_idx_init(&idx, 1234u);
    for(int i = 0; i < PROBE_IDX_CTX; i++) CHECK(idx.u[i] == 1234u);
    rc_dr_init(&ds, dic, PROBE_DR_HIT_INIT);
    CHECK(ds.K == 1 && dic[0] == 0 && ds.hit == PROBE_DR_HIT_INIT && ds.rh == 0);
    CHECK(rc_dr_rep_ctx(0, 0) == 2);
    CHECK(rc_dr_rep_ctx(1, 0) == 3);
    CHECK(rc_dr_rep_ctx(1, 5) == 1);
    for(int i = 0; i < 4; i++) CHECK(ds.rep[i] == PROBE_RC_PHALF);
    return 0;
}

static int check_reloc_helpers(void){
    static const uint32_t cases[] = {
        0u, 1u, 2u, 0x3ffu, 0x800u, 0x12345u, 0x7fffffu, 0x800000u, 0xffffffu
    };
    uint8_t out[4];
    for(size_t i = 0; i < COUNT_OF(cases); i++){
        rc_bl_pack(cases[i], out);
        uint16_t up = rc_u16le(out);
        uint16_t lo = rc_u16le(out + 2);
        CHECK(rc_bl_pattern(up, lo));
        CHECK(rc_bl_imm24(up, lo) == (cases[i] & 0x00ffffffu));
    }
    CHECK(rc_ldr_scan_first(0, 0) == 0);
    CHECK(rc_ldr_scan_first(0, 1025u) == 2);
    CHECK(rc_ldr_scan_first(100, 1100u) == 100);
    CHECK(rc_ldr_target(12, 3) == 28);
    CHECK(rc_ldr_target_in_op(10, 22, 16));
    CHECK(!rc_ldr_target_in_op(10, 22, 17));
    return 0;
}

int main(int argc, char **argv){
    (void)argv;
    int r;
    if((r = check_gamma_index())) return r;
    if((r = check_shared_models())) return r;
    if((r = check_reloc_helpers())) return r;
    if(argc == 12345){
        PatchApply pa;
        return patch_apply_run(&pa, no_bytes, 0);
    }
    return 0;
}
EOF

"$CC" $CFLAGS "$tmp/model_probe.c" -o "$tmp/model_probe"
"$tmp/model_probe"
echo "model_contract=OK"
