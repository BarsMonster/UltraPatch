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
 * val = unpacked bl/bw imm, or the s32 word for ldr/ldr.w/data-ptr/code-ptr. */
typedef struct { uint32_t addr; int32_t val; } m4_field_t;
typedef struct { m4_field_t *a; size_t n; } m4_stream_t;
/* streams order: [0]=data-ptr [1]=code-ptr [2]=bw [3]=bl [4]=ldr [5]=ldr.w  */
enum { M4_DATA, M4_CODE, M4_BW, M4_BL, M4_LDR, M4_LDRW, M4_NSTREAMS };
/* pack kind per stream, for re-encoding a relocated value into 4 bytes */
enum { M4_PK_S32, M4_PK_BL, M4_PK_BW };
int  a1_m4_disassemble(const uint8_t *from, size_t from_size,
                       uint32_t data_offset, uint32_t data_begin, uint32_t data_end,
                       uint32_t code_begin, uint32_t code_end,
                       int emit_bw, int emit_ldr_w,
                       m4_stream_t streams[M4_NSTREAMS]);
void a1_m4_free_streams(m4_stream_t streams[M4_NSTREAMS]);
/* re-encode a relocated 32-bit field value into out[4] per pack kind */
void a1_m4_pack(int pk, int32_t value, uint8_t out[4]);

#endif
