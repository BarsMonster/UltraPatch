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

/* patch_apply.h seals the non-knob model macros (UP_UG_*, RC_*, UP_BT_*, UP_LIT0_CTX, UP_IDX_CTX, UP_DR_HIT_INIT)
 * at the end of its include so they don't leak into integrator TUs. This probe still asserts on
 * their values, so snapshot them into enum constants BEFORE that seal; the macros are compile-time
 * constants, so the checks below are semantically identical against the snapshots. */
enum {
    PROBE_UG_CTX        = UP_UG_CTX,
    PROBE_UG_GAMMA_MANT = UP_UG_GAMMA_MANT,
    PROBE_RC_PROB_BITS  = RC_PROB_BITS,
    PROBE_RC_PBIT       = RC_PBIT,
    PROBE_RC_PHALF      = RC_PHALF,
    PROBE_RC_PROB_BOUND = RC_PROB_BOUND(0xffffffffu, RC_PHALF),
    PROBE_RC_RICE_MAX   = RC_RICE_UNARY_MAX,
    PROBE_BT_PROBS      = UP_BT_PROBS,
    PROBE_LIT0_CTX      = UP_LIT0_CTX,
    PROBE_IDX_CTX       = UP_IDX_CTX,
    PROBE_DR_HIT_INIT   = UP_DR_HIT_INIT
};

#include "patch_apply.h"

#define CHECK(x) do { if(!(x)) return __LINE__; } while(0)
#define COUNT_OF(x) (sizeof(x) / sizeof((x)[0]))

#include "seed_prob_probe.inc"

/* Encoder and decoder consume the same canonical constants from patch_config.h, so this combined
 * probe needs no per-side mirror assertions. */

uint8_t flash_read(uint32_t addr){ return (uint8_t)addr; }
void flash_write_page(uint32_t addr, const uint8_t page[OUTROW]){ (void)addr; (void)page; }

static int no_bytes(void *ctx, uint8_t *out){
    (void)ctx;
    (void)out;
    return PATCH_PULL_END;
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
    CHECK(sizeof(PATCH_WIRE_VERSION) == sizeof(uint8_t));
    CHECK(PATCH_WIRE_VERSION != 0u);
    CHECK(rc_wire_from_crc(rc_wire_from_crc(0x12345678u)) == 0x12345678u);
    CHECK(PROBE_RC_PROB_BITS == 12);
    CHECK(PROBE_RC_PBIT == (1 << PROBE_RC_PROB_BITS));
    CHECK((uint32_t)PROBE_RC_PROB_BOUND == (0xffffffffu >> 12) * (uint32_t)PROBE_RC_PHALF);
    up_bt_init(&bt);
    for(int i = 0; i < (int)PROBE_BT_PROBS; i++) CHECK(up_bt_get(&bt, i) == PROBE_RC_PHALF);
    for(int p = 0; p < 256; p++) CHECK(rc_lit0_sel((uint8_t)p) < PROBE_LIT0_CTX);
    up_fl_init(&fl);
    CHECK(fl.h == 0);
    for(int i = 0; i < 4; i++) CHECK(fl.m[i] == PROBE_RC_PHALF);
    up_idx_init(&idx, 1234u);
    for(int i = 0; i < PROBE_IDX_CTX; i++) CHECK(idx.u[i] == 1234u);
    rc_dr_init(&ds, dic, PROBE_DR_HIT_INIT);
    CHECK(ds.K == 1 && dic[0] == 0 && ds.hit == PROBE_DR_HIT_INIT && ds.rh == 0);
    rc_mtf_insert_i32(dic, &ds.K, 4, 1);
    rc_mtf_insert_i32(dic, &ds.K, 4, 2);
    rc_mtf_insert_i32(dic, &ds.K, 4, 3);
    CHECK(ds.K == 4 && dic[0] == 3 && dic[1] == 2 && dic[2] == 1 && dic[3] == 0);
    rc_mtf_insert_i32(dic, &ds.K, 4, 4);
    CHECK(ds.K == 4 && dic[0] == 4 && dic[1] == 3 && dic[2] == 2 && dic[3] == 1);
    CHECK(rc_dr_rep_ctx(0, 0) == 2);
    CHECK(rc_dr_rep_ctx(1, 0) == 3);
    CHECK(rc_dr_rep_ctx(1, 5) == 1);
    for(int i = 0; i < 4; i++) CHECK(ds.rep[i] == PROBE_RC_PHALF);
    CHECK(PROBE_RC_RICE_MAX == (1 << 20));
    CHECK(rc_rice_feasible(PROBE_RC_RICE_MAX - 1u, 0u));
    CHECK(rc_rice_feasible(PROBE_RC_RICE_MAX, 0u));
    CHECK(!rc_rice_feasible(PROBE_RC_RICE_MAX + 1u, 0u));
    CHECK(!rc_rice_feasible(UINT32_MAX, 11u));
    CHECK(rc_rice_feasible(UINT32_MAX, 12u));
    return 0;
}

static int check_zigzag(void){
    static const int32_t cases[] = {
        INT32_MIN, INT32_MIN + 1, -123456789, -1, 0, 1, 123456789, INT32_MAX
    };
    CHECK(rc_i32_from_u32(0u) == 0);
    CHECK(rc_i32_from_u32((uint32_t)INT32_MAX) == INT32_MAX);
    CHECK(rc_i32_from_u32((uint32_t)INT32_MAX + 1u) == INT32_MIN);
    CHECK(rc_i32_from_u32(UINT32_MAX) == -1);
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
    {
        static const struct { uint32_t bits; int32_t value; } signed_cases[] = {
            {0u, 0}, {1u, 1}, {0x7fffffu, 8388607},
            {0x800000u, -8388608}, {0xfffffeu, -2}, {0xffffffu, -1}
        };
        for(size_t i = 0; i < COUNT_OF(signed_cases); i++){
            rc_bl_pack(signed_cases[i].bits, out);
            CHECK(rc_bl_imm24s(rc_u16le(out), rc_u16le(out + 2)) == signed_cases[i].value);
        }
    }
    CHECK(rc_ldr_scan_first(0, 0) == 0);
    CHECK(rc_ldr_scan_first(0, 1025u) == 2);
    CHECK(rc_ldr_scan_first(100, 1100u) == 100);
    CHECK(rc_ldr_target(12u, 3u) == 28u);
    CHECK(rc_ldr_target(0x7ffffffeu, 255u) == 0x800003fcu);
    CHECK(rc_ldr_target_in_op(10, 22, 16));
    CHECK(!rc_ldr_target_in_op(10, 22, 17));
    CHECK(rc_ldr_target_in_op(10, 22, 28u));
    CHECK(!rc_ldr_target_in_op(10, 21, 28u));
    CHECK(!rc_ldr_target_in_op(INT32_MAX, 1, 0u));
    CHECK(!rc_ldr_target_in_op(0, INT32_MAX, UINT32_MAX - 3u));
    CHECK(rc_ldr_target_in_op(-7, 15, 4u));
    return 0;
}

/* The flat arena is stored in decoder apply order. FWD fitting consumes it directly; grow
 * fitting globally reverses it, reproducing the old reversal of both op rows and fields/row. */
static int check_field_inj_order(void){
    FieldInj fwd_v[] = {
        {1u,EV_BL,10u,1,100u},{2u,EV_EX,11u,2,101u},
        {4u,EV_BL,20u,3,102u},{5u,EV_EX,21u,4,103u},{6u,EV_BL,22u,5,104u}
    };
    FieldInj grow_v[] = {
        {1u,EV_BL,22u,5,104u},{2u,EV_EX,21u,4,103u},{3u,EV_BL,20u,3,102u},
        {5u,EV_EX,11u,2,101u},{6u,EV_BL,10u,1,100u}
    };
    OpEmitRow fwd_rows[]={{3u,2u},{7u,5u}}, grow_rows[]={{4u,3u},{7u,5u}};
    FieldInjArena fwd={fwd_v,COUNT_OF(fwd_v),COUNT_OF(fwd_v)};
    FieldInjArena grow={grow_v,COUNT_OF(grow_v),COUNT_OF(grow_v)};
    static const uint32_t expect[]={10u,11u,20u,21u,22u};
    CHECK(fwd_rows[0].inj_end==2u && fwd_rows[1].inj_end==5u);
    CHECK(grow_rows[0].inj_end==3u && grow_rows[1].inj_end==5u);
    for(size_t i=0;i<COUNT_OF(expect);i++){
        CHECK(field_inj_key(&fwd,1,i)->k1==expect[i]);
        CHECK(field_inj_key(&grow,0,i)->k1==expect[i]);
    }
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
    if((r = check_field_inj_order())) return r;
    if(argc == 12345){
        PatchApply pa;
        return patch_apply_run(&pa, no_bytes, 0);
    }
    return 0;
}
EOF

"$CC" $CFLAGS "$tmp/model_probe.c" -o "$tmp/model_probe"
"$tmp/model_probe"

# Exercise the host contract at all three decision layers: frozen-model pricing and emission stop
# before an over-cap unary prefix, and both existing k sweeps choose a decoder-feasible alternative
# even when the raw codelength optimum would be the infeasible k=0. The half-million cheap tokens
# make that latter case real without weakening the production cap for a test build.
cat > "$tmp/rice_fit_probe.c" <<'EOF'
#include "enc_internal.h"

#define CHECK(x) do { if(!(x)) return __LINE__; } while(0)

static int check_chosen(const TokenVec *tv,int k,int out){
    uint32_t exp=0;
    for(size_t i=0;i<tv->n;i++) if(tv->v[i].type==(out?'O':'R')){
        uint32_t v=out ? rc_outmatch_delta((uint32_t)tv->v[i].dist,exp)
                       : (uint32_t)tv->v[i].dist-1u;
        if(out) exp=rc_outmatch_next_expect(1,(uint32_t)tv->v[i].dist,(uint32_t)tv->v[i].len);
        CHECK(rc_rice_feasible(v,(uint32_t)k));
    }
    return 0;
}

int main(void){
    const uint32_t bad=RC_RICE_UNARY_MAX+2u;
    const size_t n=(size_t)(bad/2u)+2u;
    Token *v=(Token *)xcalloc(n,sizeof(*v));
    TokenVec tv={v,n,n};
    up_UGRice g,before_g;
    up_UGGamma gg,gg_before,gm,gc;
    REnc rc,before,mr,cr;
    Buf out,mout,cout;
    int k,r;

    ugr_init_e(&g,0);
    CHECK(ugr_price(&g,RC_RICE_UNARY_MAX-1u)!=UINT32_MAX);
    CHECK(ugr_price(&g,RC_RICE_UNARY_MAX)!=UINT32_MAX);
    CHECK(ugr_price(&g,RC_RICE_UNARY_MAX+1u)==UINT32_MAX);
    re_init(&rc);
    ugr_encode(&g,&rc,RC_RICE_UNARY_MAX);
    CHECK(!rc.coding_overflow);
    out=re_flush_opt(&rc); buf_free(&out);
    ugr_init_e(&g,0); re_init(&rc); before=rc; before_g=g;
    ugr_encode(&g,&rc,RC_RICE_UNARY_MAX+1u);
    CHECK(rc.coding_overflow);
    CHECK(rc.low==before.low && rc.range==before.range && rc.cache==before.cache &&
          rc.csz==before.csz && rc.out.d==before.out.d && rc.out.n==before.out.n);
    CHECK(memcmp(&g,&before_g,sizeof(g))==0);
    buf_free(&rc.out);

    ugg_init_e(&gg);
    CHECK(ugg_price(&gg,UINT32_MAX-1u)!=UINT32_MAX);
    gg_before=gg;
    CHECK(ugg_price(&gg,UINT32_MAX)==UINT32_MAX);
    CHECK(memcmp(&gg,&gg_before,sizeof(gg))==0);

    ugg_init_e(&gm); ugg_init_e(&gc); re_init(&mr); re_init_count(&cr);
    ugg_encode(&gm,&mr,UINT32_MAX-1u);
    ugg_encode(&gc,&cr,UINT32_MAX-1u);
    CHECK(!mr.coding_overflow && !cr.coding_overflow);
    CHECK(memcmp(&gm,&gc,sizeof(gm))==0);
    CHECK(mr.low==cr.low && mr.range==cr.range && mr.cache==cr.cache &&
          mr.csz==cr.csz && mr.out.n==cr.out.n);
    mout=re_flush_opt(&mr); cout=re_flush_opt(&cr);
    CHECK(mout.n==cout.n && cout.d==NULL && cout.cap==0);
    buf_free(&mout);

    ugg_init_e(&gg); re_init(&rc); before=rc; gg_before=gg;
    ugg_encode(&gg,&rc,UINT32_MAX);
    CHECK(rc.coding_overflow);
    CHECK(rc.low==before.low && rc.range==before.range && rc.cache==before.cache &&
          rc.csz==before.csz && rc.out.d==before.out.d && rc.out.n==before.out.n);
    CHECK(memcmp(&gg,&gg_before,sizeof(gg))==0);
    buf_free(&rc.out);

    for(size_t i=0;i<n;i++) v[i]=(Token){'R',0,1,1};
    v[n-1u].dist=(int32_t)bad+1;
    CHECK(!rc_rice_feasible((uint32_t)v[n-1u].dist-1u,0u));
    k=fit_k_tokens(&tv);
    CHECK(k==1);
    if((r=check_chosen(&tv,k,0))) return r;

    { uint32_t exp=0;
      for(size_t i=0;i<n-1u;i++){
          v[i]=(Token){'O',0,(int32_t)RC_OUTMATCH_MIN,(int32_t)exp};
          exp=rc_outmatch_next_expect(1,exp,RC_OUTMATCH_MIN);
      }
      v[n-1u]=(Token){'O',0,(int32_t)RC_OUTMATCH_MIN,(int32_t)(exp+bad/2u)};
    }
    CHECK(!rc_rice_feasible(rc_outmatch_delta((uint32_t)v[n-1u].dist,
          (uint32_t)(RC_OUTMATCH_MIN*(n-1u))),0u));
    k=fit_k_out(&tv,15,0u,1);
    CHECK(k==1);
    if((r=check_chosen(&tv,k,1))) return r;
    free(v);
    return 0;
}
EOF
"$CC" $CFLAGS "$tmp/rice_fit_probe.c" src/enc_util.c src/enc_rc.c src/enc_lz.c \
    -Wl,--gc-sections -o "$tmp/rice_fit_probe"
"$tmp/rice_fit_probe"

"$CC" $CFLAGS test-bench/range-sink-contract.c src/enc_util.c src/enc_rc.c \
    -Wl,--gc-sections -o "$tmp/range_sink_contract"
"$tmp/range_sink_contract"
echo "range_sink_contract=OK (carry chain + randomized material/count equivalence)"

echo "model_seed_prob=OK (shared encoder/decoder model)"
echo "model_rice_cap=OK (boundary + crafted fit + emission feasibility)"
echo "model_gamma_cap=OK (UINT32 boundary + material/count equivalence)"
echo "model_contract=OK"
