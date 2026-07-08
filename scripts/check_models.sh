#!/usr/bin/env bash
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

# Model/wire contract probe. Compiles one TU that includes the real device
# decoder header set and the host encoder internals, then asserts invariants
# that would otherwise drift silently between comments, structs, and helpers.
set -eu

CC="${CC:-gcc}"
CFLAGS="${CFLAGS:--DCORTEX_M0 -g -Wall -Wextra -Wdouble-promotion -Wfloat-equal -Wformat=2 -Wshadow -Werror -std=c99 -O2 -ffunction-sections -fdata-sections -I. -Isrc -Ivendor/libdivsufsort}"

. "$(dirname "$0")/tempdir.sh"

cat > "$tmp/model_probe.c" <<'EOF'
#include "enc_internal.h"
#include "patch_apply.h"

#define CHECK(x) do { if(!(x)) return __LINE__; } while(0)
#define COUNT_OF(x) (sizeof(x) / sizeof((x)[0]))

_Static_assert(PATHE_W == SA_W, "encoder/decoder LZ windows must match");
_Static_assert(A1_JSLOTS == JSLOTS, "encoder/decoder journal caps must match");
_Static_assert(A1_OPC_CAP == OPC_CAP, "encoder/decoder per-op correction caps must match");
_Static_assert(A1_OUTROW == OUTROW, "encoder/decoder row sizes must match");
_Static_assert(A1_ROW_DEPTH == OUTROW_DEPTH, "encoder/decoder row depths must match");
_Static_assert(sizeof(((UGE *)0)->m) == sizeof(((A1UGRice *)0)->m),
               "encoder/decoder rice mantissa tables must match");
_Static_assert(COUNT_OF(((A1UGGamma *)0)->m) == UG_GAMMA_MANT,
               "compact gamma mantissa table size must match its indexer");

uint8_t flash_read(uint32_t addr){ return (uint8_t)addr; }
void flash_write(uint32_t addr, uint8_t val){ (void)addr; (void)val; }

static int no_bytes(void *ctx, uint8_t *out){
    (void)ctx;
    (void)out;
    return 0;
}

static int check_gamma_index(void){
    uint8_t seen[UG_GAMMA_MANT] = {0};
    int used = 0;
    for(int row = 1; row < UG_CTX; row++){
        for(int pos = 0; pos < row; pos++){
            int idx = rc_ugg_mant_idx(row, pos);
            CHECK(idx >= 0 && idx < UG_GAMMA_MANT);
            CHECK(!seen[idx]);
            seen[idx] = 1;
            used++;
        }
    }
    for(int pos = 0; pos <= UG_CTX; pos++){
        int idx = rc_ugg_mant_idx(UG_CTX, pos);
        CHECK(idx >= 0 && idx < UG_GAMMA_MANT);
        CHECK(!seen[idx]);
        seen[idx] = 1;
        used++;
    }
    CHECK(used == UG_GAMMA_MANT);
    for(int i = 0; i < UG_GAMMA_MANT; i++) CHECK(seen[i]);
    return 0;
}

static int check_shared_models(void){
    A1BitTree bt;
    A1Flag1 fl;
    A1IdxUnary idx;
    A1UGRice drice;
    UGRiceE erice;
    A1UGGamma dgamma;
    UGGammaE egamma;
    uint16_t K, rep[4], hit;
    uint8_t rh;
    int32_t dic[4] = { 123, 456, 789, 111 };
    CHECK(RC_PROB_BITS == 12u);
    CHECK(RC_PBIT == (1u << RC_PROB_BITS));
    CHECK(RC_PROB_BOUND(0xffffffffu, RC_PHALF) == ((0xffffffffu >> 12) * RC_PHALF));
    a1_bt_init(&bt);
    for(int i = 0; i < (int)BT_PROBS; i++) CHECK(a1_bt_get(&bt, i) == RC_PHALF);
    for(int p = 0; p < 256; p++) CHECK(rc_lit0_sel((uint8_t)p) < LIT0_CTX);
    a1_fl_init(&fl);
    CHECK(fl.h == 0);
    for(int i = 0; i < 4; i++) CHECK(fl.m[i] == RC_PHALF);
    a1_idx_init(&idx, 1234u);
    for(int i = 0; i < IDX_CTX; i++) CHECK(idx.u[i] == 1234u);
    RC_UG_RICE_INIT(&drice.k, drice.u, drice.m, 3);
    erice.code = 'r';
    RC_UG_RICE_INIT(&erice.k, erice.u, erice.m, 3);
    CHECK(drice.k == erice.k);
    for(int i = 0; i <= UG_CTX; i++){
        CHECK(drice.u[i] == erice.u[i]);
        for(int j = 0; j <= UG_CTX; j++) CHECK(drice.m[i][j] == erice.m[i][j]);
    }
    RC_UG_GAMMA_INIT(dgamma.u, dgamma.m);
    egamma.code = 'g'; egamma.k = 0;
    RC_UG_GAMMA_INIT(egamma.u, egamma.m);
    for(int i = 0; i <= UG_CTX; i++) CHECK(dgamma.u[i] == egamma.u[i]);
    for(int i = 0; i < UG_GAMMA_MANT; i++) CHECK(dgamma.m[i] == egamma.m[i]);
    rc_dr_init(&K, rep, &hit, &rh, dic, DR_HIT_INIT);
    CHECK(K == 1 && dic[0] == 0 && hit == DR_HIT_INIT && rh == 0);
    CHECK(rc_dr_rep_ctx(0, 0) == 2);
    CHECK(rc_dr_rep_ctx(1, 0) == 3);
    CHECK(rc_dr_rep_ctx(1, 5) == 1);
    for(int i = 0; i < 4; i++) CHECK(rep[i] == RC_PHALF);
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
