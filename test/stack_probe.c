/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 *
 * Runtime stack high-water cross-check for patch_apply_run(), run under qemu-arm.
 *
 * This is NOT a device build and NOT a gate: it is an order-of-magnitude sanity check on
 * the STATIC bound produced by scripts/stack_bound.py (which is authoritative for the real
 * Cortex-M0+ device object). Here the decoder is compiled as hosted ARM Thumb (arm-linux-
 * gnueabi, ARMv7-A) and run under qemu user-mode, so its frame sizes differ from the M0+
 * -O2 object; the point is only to confirm a full decode's caller-stack cost is a few
 * hundred bytes (same order as the static bound), never thousands -- i.e. the static
 * call-graph method is not missing a deep or unbounded path.
 *
 * Method (classic painted-canary): paint()'s own local frame is filled with a sentinel and
 * then RETURNED (freeing it but leaving the bytes in place); the subsequent decode reuses
 * that same freed stack region, overwriting its top down to the decode's deepest frame.
 * Scanning the window from its low end to the first overwritten byte recovers the high-water
 * mark. Every write is in-bounds of a live frame; we only READ the (now-below-SP but mapped)
 * window afterward -- safe under qemu user-mode.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- the decoder's two externs, backed by a RAM buffer --- */
static uint8_t *g_flash;
static uint32_t g_flash_n;
uint8_t flash_read(uint32_t a) { return a < g_flash_n ? g_flash[a] : 0xFFu; }
void    flash_write(uint32_t a, uint8_t v) { if (a < g_flash_n) g_flash[a] = v; }

#include "patch_apply.h"

typedef struct { const uint8_t *d; size_t n, i; } PullCtx;
static int pull_next(void *c, uint8_t *out) {
    PullCtx *p = (PullCtx *)c;
    if (p->i >= p->n) return 0;
    *out = p->d[p->i++];
    return 1;
}

#define PAINT_BYTES 65536u
#define SENT 0xA5u

static volatile unsigned char *g_lo, *g_hi;
static PatchApply g_pa;

/* Paint a deep window on the stack, record its bounds, then return (the bytes survive).
 * Recording the address of a local that outlives the frame is the whole technique here, so
 * the dangling-pointer diagnostic is expected and deliberately suppressed. */
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdangling-pointer"
#endif
static void __attribute__((noinline)) paint(void) {
    volatile unsigned char buf[PAINT_BYTES];
    for (unsigned i = 0; i < PAINT_BYTES; i++) buf[i] = (unsigned char)SENT;
    g_lo = &buf[0];
    g_hi = &buf[PAINT_BYTES - 1u];
    __asm__ volatile("" : : "r"(g_lo), "r"(g_hi) : "memory");
}
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

/* Run one full decode; kept noinline so its frame sits at a stable, comparable depth. */
static int __attribute__((noinline)) decode_once(const uint8_t *blob, size_t bsz) {
    PullCtx pc = { blob, bsz, 0 };
    return patch_apply_run(&g_pa, pull_next, &pc);
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: %s <memfile> <blob>\n", argv[0]); return 2; }
    FILE *mf = fopen(argv[1], "rb");
    if (!mf) { perror("mem"); return 2; }
    fseek(mf, 0, SEEK_END); long fsz = ftell(mf); fseek(mf, 0, SEEK_SET);
    if (fsz <= 0) { fprintf(stderr, "empty memfile\n"); fclose(mf); return 2; }
    /* size flash generously so a grow decode stays in-bounds; tail is erased (0xFF). */
    g_flash_n = (uint32_t)fsz < (1u << 20) ? (1u << 20) : (uint32_t)fsz;
    g_flash = (uint8_t *)malloc(g_flash_n);
    memset(g_flash, 0xFF, g_flash_n);
    if (fread(g_flash, 1, (size_t)fsz, mf) != (size_t)fsz) { fclose(mf); return 2; }
    fclose(mf);

    FILE *bf = fopen(argv[2], "rb");
    if (!bf) { perror("blob"); return 2; }
    fseek(bf, 0, SEEK_END); long bsz = ftell(bf); fseek(bf, 0, SEEK_SET);
    if (bsz <= 0) { fprintf(stderr, "empty blob\n"); fclose(bf); return 2; }
    uint8_t *blob = (uint8_t *)malloc((size_t)bsz);
    if (fread(blob, 1, (size_t)bsz, bf) != (size_t)bsz) { fclose(bf); return 2; }
    fclose(bf);

    paint();
    int rc = decode_once(blob, (size_t)bsz);

    /* scan the window low->high for the first overwritten byte = deepest touched address. */
    const volatile unsigned char *p = g_lo;
    while (p <= g_hi && *p == (unsigned char)SENT) p++;
    size_t used = (p > g_hi) ? 0 : (size_t)(g_hi - p) + 1u;

    printf("stack_probe_rc=%d\n", rc);                 /* 0==DONE (see PATCH_APPLY_DONE) */
    printf("stack_probe_highwater_bytes=%zu\n", used);
    printf("stack_probe_paint_window=%u\n", (unsigned)PAINT_BYTES);
    free(blob); free(g_flash);
    /* fail only if the paint window was fully consumed (measurement invalid) */
    return (used >= PAINT_BYTES - 64u) ? 3 : 0;
}
