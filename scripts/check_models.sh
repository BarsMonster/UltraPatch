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

cat > "$tmp/seed_prob_probe.inc" <<'EOF'
/* Host-only oracle for the decoder's divide-free literal histogram seed. */
#define SEED_SCALE 4096u

static uint16_t seed_prob_oracle(uint32_t num, uint32_t den){
    uint64_t pr;
    if(!den) return 2048u;
    pr=(2u*(uint64_t)SEED_SCALE*num+den)/(2u*(uint64_t)den);
    return (uint16_t)(pr<1u ? 1u : (pr>SEED_SCALE-1u ? SEED_SCALE-1u : pr));
}

static uint16_t seed_prob_legacy(uint32_t num, uint32_t den){
    uint32_t pr=(2u*SEED_SCALE*num+den)/(2u*den);
    return (uint16_t)(pr<1u ? 1u : (pr>SEED_SCALE-1u ? SEED_SCALE-1u : pr));
}

static int check_seed_case(uint32_t num, uint32_t den){
    CHECK(rc_lit_seed_prob(num,den)==seed_prob_oracle(num,den));
    /* Where every operation in the old expression fit u32, require byte-for-byte compatibility. */
    if(den && num<=(UINT32_MAX-den)/(2u*SEED_SCALE))
        CHECK(rc_lit_seed_prob(num,den)==seed_prob_legacy(num,den));
    return 0;
}

static int check_seed_window(uint32_t center, uint32_t den){
    uint32_t lo=center>3u ? center-3u : 0u;
    uint32_t hi=den-center>3u ? center+3u : den;
    for(uint32_t num=lo;;num++){
        int r=check_seed_case(num,den);
        if(r) return r;
        if(num==hi) break;
    }
    return 0;
}

static int check_seed_prob(void){
    static const uint32_t dens[] = {
        1u,2u,3u,7u,31u,255u,256u,257u,1023u,1024u,1025u,
        524287u,524288u,524289u,1048575u,1048576u,1048577u,
        MAX_IMAGE-1u,MAX_IMAGE
    };
    int r;

    CHECK(rc_lit_seed_prob(0u,0u)==2048u);
    CHECK(rc_lit_seed_prob(MAX_IMAGE,0u)==2048u);

    /* Exhaust every valid fraction in a dense small domain. */
    for(uint32_t den=1u;den<=1024u;den++)
        for(uint32_t num=0u;num<=den;num++){
            r=check_seed_case(num,den);
            if(r) return r;
        }

    /* Exercise zeros, tails, balanced/skewed ratios, and the old 8192*num wrap seam. */
    for(size_t i=0;i<sizeof(dens)/sizeof(dens[0]);i++){
        uint32_t den=dens[i];
        static const uint32_t wrap[] = { 524287u,524288u,524289u };
        uint32_t centers[] = { 0u,1u,den/8192u,den/4096u,den/2u,den-1u,den };
        for(size_t j=0;j<sizeof(centers)/sizeof(centers[0]);j++){
            r=check_seed_window(centers[j],den);
            if(r) return r;
        }
        for(size_t j=0;j<sizeof(wrap)/sizeof(wrap[0]);j++) if(wrap[j]<=den){
            r=check_seed_window(wrap[j],den);
            if(r) return r;
        }
    }

    /* Probe both sides of every half-up quantization boundary at the 64 MiB product cap. */
    for(uint32_t q=0u;q<=SEED_SCALE;q++){
        uint32_t center=(uint32_t)(((2u*(uint64_t)q+1u)*MAX_IMAGE)/(2u*SEED_SCALE));
        r=check_seed_window(center,MAX_IMAGE);
        if(r) return r;
    }

    /* At MAX_IMAGE, sweep every numerator for which the historical u32 formula was defined. */
    for(uint32_t num=0u;num<=(UINT32_MAX-MAX_IMAGE)/(2u*SEED_SCALE);num++){
        r=check_seed_case(num,MAX_IMAGE);
        if(r) return r;
    }
    return 0;
}

#undef SEED_SCALE
EOF

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

#include "seed_prob_probe.inc"

/* Encoder and decoder share one define per knob (WINDOW_LOG, JSLOTS, OPC_CAP, OUTROW, OUTROW_DEPTH)
 * from patch_config.h, so a wire-knob mismatch is impossible by construction — no mirror asserts. */

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
    up_BitTree bt;
    up_Flag1 fl;
    up_IdxUnary idx;
    up_DRStream ds;
    int32_t dic[4] = { 123, 456, 789, 111 };
    CHECK(PROBE_RC_PROB_BITS == 12);
    CHECK(PROBE_RC_PBIT == (1 << PROBE_RC_PROB_BITS));
    CHECK((uint32_t)PROBE_RC_PROB_BOUND == (0xffffffffu >> 12) * (uint32_t)PROBE_RC_PHALF);
    bt_init(&bt);
    for(int i = 0; i < (int)PROBE_BT_PROBS; i++) CHECK(bt_get(&bt, i) == PROBE_RC_PHALF);
    for(int p = 0; p < 256; p++) CHECK(rc_lit0_sel((uint8_t)p) < PROBE_LIT0_CTX);
    fl_init(&fl);
    CHECK(fl.h == 0);
    for(int i = 0; i < 4; i++) CHECK(fl.m[i] == PROBE_RC_PHALF);
    idx_init(&idx, 1234u);
    for(int i = 0; i < PROBE_IDX_CTX; i++) CHECK(idx.u[i] == 1234u);
    rc_dr_init(&ds, dic, PROBE_DR_HIT_INIT);
    CHECK(ds.K == 1 && dic[0] == 0 && ds.hit == PROBE_DR_HIT_INIT && ds.rh == 0);
    CHECK(rc_dr_rep_ctx(0, 0) == 2);
    CHECK(rc_dr_rep_ctx(1, 0) == 3);
    CHECK(rc_dr_rep_ctx(1, 5) == 1);
    for(int i = 0; i < 4; i++) CHECK(ds.rep[i] == PROBE_RC_PHALF);
    return 0;
}

static int check_zigzag(void){
    static const int32_t cases[] = {
        INT32_MIN, INT32_MIN + 1, -123456789, -1, 0, 1, 123456789, INT32_MAX
    };
    CHECK(rc_zz32(INT32_MIN) == UINT32_MAX);
    CHECK(rc_zz32(INT32_MAX) == UINT32_MAX - 1u);
    CHECK(rc_unzz32_value(UINT32_MAX) == INT32_MIN);
    CHECK(rc_unzz32_value(UINT32_MAX - 1u) == INT32_MAX);
    for(size_t i = 0; i < COUNT_OF(cases); i++)
        CHECK(rc_unzz32_value(rc_zz32(cases[i])) == cases[i]);
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
    if((r = check_seed_prob())) return r;
    if((r = check_gamma_index())) return r;
    if((r = check_shared_models())) return r;
    if((r = check_zigzag())) return r;
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

# Exercise the same helper as end-users receive it: generate a private fresh single header, then
# compile and run the identical oracle suite without any encoder headers in the translation unit.
python3 scripts/gen_single_header.py "$tmp/patch_apply_single.h" \
    src/patch_config.h src/rc_models.h src/patch_apply.h
cat > "$tmp/single_model_probe.c" <<'EOF'
#include <stdint.h>
#include "patch_apply_single.h"

#define CHECK(x) do { if(!(x)) return __LINE__; } while(0)
#include "seed_prob_probe.inc"

uint8_t flash_read(uint32_t addr){ return (uint8_t)addr; }
void flash_write(uint32_t addr, uint8_t val){ (void)addr; (void)val; }
static int no_bytes_single(void *ctx, uint8_t *out){ (void)ctx; (void)out; return 0; }

int main(int argc, char **argv){
    (void)argv;
    int r=check_seed_prob();
    if(r) return r;
    if(argc==12345){ PatchApply pa; return patch_apply_run(&pa,no_bytes_single,0); }
    return 0;
}
EOF
"$CC" $CFLAGS -I"$tmp" "$tmp/single_model_probe.c" -o "$tmp/single_model_probe"
"$tmp/single_model_probe"
echo "model_seed_prob=OK (source + generated single header)"
echo "model_contract=OK"
