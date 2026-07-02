/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 */

#ifndef PATCH_APPLY_PUSH_ADAPTER_H
#define PATCH_APPLY_PUSH_ADAPTER_H
/*
 * OPTIONAL push->pull adapter for event-driven producers. patch_apply.h exposes ONE decode
 * entry point, patch_apply_run(callback, ctx), which pulls blob bytes and may block. A
 * producer that instead PUSHES bytes as they arrive (a UART/BLE RX interrupt, a radio event
 * handler) adapts through this single-producer/single-consumer byte ring:
 *
 *   producer (ISR / event handler)          consumer (update task / main loop)
 *   ------------------------------          ----------------------------------
 *   patch_ring_push(&ring, byte);           patch_ring_init(&ring, buf, cap, wait, wctx);
 *   ... last byte ...                       rc = patch_apply_run(patch_ring_next, &ring);
 *   patch_ring_eof(&ring);
 *
 * patch_ring_next blocks by invoking the integrator `wait` hook while the ring is empty —
 * on a device typically WFI/WFE, an RTOS yield, or a poll of the transport. The hook MUST
 * allow the producer to run (never call patch_apply_run from the producer's own context).
 *
 * Concurrency contract: exactly one producer and one consumer. `w` and `eof` are written
 * only by the producer; `r` only by the consumer; the indices are free-running uint32 so
 * empty is w==r and full is w-r==cap (cap MUST be a power of two). `volatile` is sufficient
 * on single-core Cortex-M (no store reordering visible to the other context); on an SMP
 * host test drive both sides from one thread via the wait hook, or add real fences.
 *
 * This adapter is deliberately NOT part of the device decoder artifact: patch_apply.h
 * compiles, fuzzes, and gets sized without it.
 */
#include <stdint.h>

typedef struct {
    uint8_t *buf;                     /* caller-owned storage, cap bytes */
    uint32_t cap;                     /* power of two */
    volatile uint32_t r, w;           /* consumer-owned / producer-owned free-running indices */
    volatile uint8_t eof;             /* producer: no more bytes after the last push */
    void (*wait)(void *);             /* consumer-side idle hook while the ring is empty */
    void *wait_ctx;
} PatchRing;

static inline void patch_ring_init(PatchRing *g, uint8_t *buf, uint32_t cap_pow2,
                                   void (*wait)(void *), void *wait_ctx) {
    g->buf = buf; g->cap = cap_pow2; g->r = 0; g->w = 0; g->eof = 0;
    g->wait = wait; g->wait_ctx = wait_ctx;
}

/* producer: enqueue one byte. Returns 1 on success, 0 if the ring is full (retry after the
 * consumer drains — e.g. re-arm the RX interrupt, or apply transport flow control). */
static inline int patch_ring_push(PatchRing *g, uint8_t b) {
    if (g->w - g->r == g->cap) return 0;
    g->buf[g->w & (g->cap - 1u)] = b;
    g->w = g->w + 1u;
    return 1;
}

/* producer: signal end-of-blob. Call AFTER the last successful push. */
static inline void patch_ring_eof(PatchRing *g) { g->eof = 1; }

/* consumer: the patch_apply_run callback. Pass the PatchRing as ctx.
 * eof is sampled BEFORE the emptiness re-check, so a byte pushed before patch_ring_eof()
 * can never be lost: if eof reads 1 while a byte is pending, the data branch wins first. */
static inline int patch_ring_next(void *ctx, uint8_t *out) {
    PatchRing *g = (PatchRing *)ctx;
    for (;;) {
        uint8_t e = g->eof;
        if (g->w != g->r) {
            *out = g->buf[g->r & (g->cap - 1u)];
            g->r = g->r + 1u;
            return 1;
        }
        if (e) return 0;
        g->wait(g->wait_ctx);
    }
}

#endif /* PATCH_APPLY_PUSH_ADAPTER_H */
