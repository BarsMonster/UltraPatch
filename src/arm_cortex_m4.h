/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 */

#ifndef A1_ARM_CORTEX_M4_H
#define A1_ARM_CORTEX_M4_H

#include <stdint.h>
#include <stddef.h>

/* Sorted (addr, val) field list for one relocation stream.
 * val = unpacked bl imm, or the s32 literal-pool word for ldr. */
typedef struct { uint32_t addr; int32_t val; } m4_field_t;
typedef struct { m4_field_t *a; size_t n; } m4_stream_t;
/* streams order: [0]=bl [1]=ldr */
enum { M4_BL, M4_LDR, M4_NSTREAMS };
void a1_m4_disassemble(const uint8_t *from, size_t from_size,
                       uint32_t data_offset, uint32_t data_begin, uint32_t data_end,
                       m4_stream_t streams[M4_NSTREAMS]);
void a1_m4_free_streams(m4_stream_t streams[M4_NSTREAMS]);

#endif
