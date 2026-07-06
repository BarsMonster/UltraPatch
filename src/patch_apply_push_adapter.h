/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 */

#ifndef PATCH_APPLY_PUSH_ADAPTER_H
#define PATCH_APPLY_PUSH_ADAPTER_H
/*
 * OPTIONAL push->pull adapter for event-driven producers. patch_apply.h exposes ONE decode
 * entry point, patch_apply_run(state, callback, ctx), which pulls blob bytes and may block. A
 * producer that instead PUSHES bytes as they arrive (a UART/BLE RX interrupt, a radio event
 * handler) adapts through this single-producer/single-consumer byte ring:
 *
 *   producer (ISR / event handler)          consumer (update task / main loop)
 *   ------------------------------          ----------------------------------
 *   patch_ring_push(&ring, byte);           patch_ring_init(&ring, buf, cap, wait, wctx);
 *   ... counted body terminates ...         rc = patch_apply_run(&pa, patch_ring_next, &ring);
 *
 * patch_ring_next blocks by invoking the integrator `wait` hook while the ring is empty —
 * on a device typically WFI/WFE, an RTOS yield, or a poll of the transport. The hook MUST
 * allow the producer to run (never call patch_apply_run from the producer's own context).
 * A valid patch completes from its header-counted body before EOF is needed; use
 * patch_ring_eof() only to abort a stalled/truncated transfer.
 *
 * Concurrency contract: exactly one producer and one consumer. `w` is written only by the
 * producer, `r` only by the consumer; `eof` is the abort flag, normally set by the
 * consumer-side WAIT HOOK on a transport timeout via patch_ring_eof() to
 * abort a stalled decode (setting eof is the only exit from patch_ring_next's wait loop;
 * benign, all callers only ever store the constant 1). The indices are free-running uint32
 * so empty is w==r and full is w-r==cap (cap MUST be a power of two). The adapter publishes
 * through a small compiler barrier around the byte/write-index handoff; on SMP, add platform
 * acquire/release fences around PATCH_RING_BARRIER.
 *
 * This adapter is deliberately NOT part of the device decoder artifact: patch_apply.h
 * compiles, fuzzes, and gets sized without it.
 */
#include <stdint.h>

#ifndef PATCH_RING_BARRIER
#define PATCH_RING_BARRIER_LOCAL 1
#if defined(__GNUC__) || defined(__clang__)
#define PATCH_RING_BARRIER() __asm__ __volatile__("" ::: "memory")
#else
#define PATCH_RING_BARRIER() do { } while (0)
#endif
#endif

typedef struct {
    uint8_t *buf;                     /* caller-owned storage, cap bytes */
    uint32_t cap;                     /* power of two */
    volatile uint32_t r, w;           /* consumer-owned / producer-owned free-running indices */
    volatile uint8_t eof;             /* abort/timeout: no more bytes will arrive */
    void (*wait)(void *);             /* consumer-side idle hook while the ring is empty */
    void *wait_ctx;
} PatchRing;

static inline int patch_ring_valid(const PatchRing *g) {
    return g && g->buf && g->cap && ((g->cap & (g->cap - 1u)) == 0u) && g->wait;
}

static inline int patch_ring_init(PatchRing *g, uint8_t *buf, uint32_t cap_pow2,
                                  void (*wait)(void *), void *wait_ctx) {
    if (!g) return 0;
    g->buf = buf; g->cap = cap_pow2; g->r = 0; g->w = 0; g->eof = 0;
    g->wait = wait; g->wait_ctx = wait_ctx;
    if (!patch_ring_valid(g)) {
        g->buf = 0; g->cap = 0; g->r = 0; g->w = 0; g->eof = 1;
        g->wait = 0; g->wait_ctx = 0;
        return 0;
    }
    return 1;
}

/* producer: enqueue one byte. Returns 1 on success, 0 if the ring is full (retry after the
 * consumer drains — e.g. re-arm the RX interrupt, or apply transport flow control). */
static inline int patch_ring_push(PatchRing *g, uint8_t b) {
    if (!patch_ring_valid(g)) return 0;
    if (g->w - g->r == g->cap) return 0;
    g->buf[g->w & (g->cap - 1u)] = b;
    PATCH_RING_BARRIER();
    g->w = g->w + 1u;
    return 1;
}

/* Signal abort/end-of-source for a stalled or truncated transfer. A valid patch normally
 * completes from its counted body before this is needed; the store is idempotent. */
static inline void patch_ring_eof(PatchRing *g) { if (g) g->eof = 1; }

/* consumer: the patch_apply_run callback. Pass the PatchRing as ctx.
 * eof is sampled BEFORE the emptiness re-check, so a byte pushed before patch_ring_eof()
 * can never be lost: if eof reads 1 while a byte is pending, the data branch wins first. */
static inline int patch_ring_next(void *ctx, uint8_t *out) {
    PatchRing *g = (PatchRing *)ctx;
    if (!patch_ring_valid(g) || !out) return 0;
    for (;;) {
        uint8_t e = g->eof;
        if (g->w != g->r) {
            PATCH_RING_BARRIER();
            *out = g->buf[g->r & (g->cap - 1u)];
            g->r = g->r + 1u;
            return 1;
        }
        if (e) return 0;
        g->wait(g->wait_ctx);
    }
}

#ifdef PATCH_RING_BARRIER_LOCAL
#undef PATCH_RING_BARRIER
#undef PATCH_RING_BARRIER_LOCAL
#endif

#endif /* PATCH_APPLY_PUSH_ADAPTER_H */
