#ifndef DETOOLS_ARM_CORTEX_M4_H
#define DETOOLS_ARM_CORTEX_M4_H

#include <stdint.h>
#include <stddef.h>

/*
 * Build the apply-side data-format buffers for an arm-cortex-m4 patch.
 *
 *   dfpatch / dfpatch_size : decompressed data-format patch blob
 *   from / from_size       : the from-image
 *   to_size                : size of the to-image
 *   *fromzero_out          : (malloc'd, from_size) from-image with fields zeroed
 *   *dfdiff_out            : (malloc'd, to_size)   overlay of re-encoded to-addresses
 *
 * Reconstruction: to[i] = patch[i] + (*fromzero_out)[i] + (*dfdiff_out)[i].
 * Caller frees the two output buffers. Returns 0 on success, <0 on error.
 */
int detools_m4_build(const uint8_t *dfpatch, size_t dfpatch_size,
                     const uint8_t *from, size_t from_size, size_t to_size,
                     uint8_t **fromzero_out, uint8_t **dfdiff_out);

/* ---- ultrapatch v2: expose the disassembler (no detools_m4_build) ---- */
/* Sorted (addr, val) field list for one relocation stream.
 * val = unpacked bl/bw imm, or the s32 word for ldr/ldr.w/data-ptr/code-ptr. */
typedef struct { uint32_t addr; int32_t val; } m4_field_t;
typedef struct { m4_field_t *a; size_t n; } m4_stream_t;
/* streams order: [0]=data-ptr [1]=code-ptr [2]=bw [3]=bl [4]=ldr [5]=ldr.w  */
enum { M4_DATA, M4_CODE, M4_BW, M4_BL, M4_LDR, M4_LDRW, M4_NSTREAMS };
/* pack kind per stream, for re-encoding a relocated value into 4 bytes */
enum { M4_PK_S32, M4_PK_BL, M4_PK_BW };
int  detools_m4_disassemble(const uint8_t *from, size_t from_size,
                            uint32_t data_offset, uint32_t data_begin, uint32_t data_end,
                            uint32_t code_begin, uint32_t code_end,
                            m4_stream_t streams[M4_NSTREAMS]);
int  detools_m4_disassemble_ex(const uint8_t *from, size_t from_size,
                               uint32_t data_offset, uint32_t data_begin, uint32_t data_end,
                               uint32_t code_begin, uint32_t code_end,
                               int emit_bw, int emit_ldr_w,
                               m4_stream_t streams[M4_NSTREAMS]);
void detools_m4_free_streams(m4_stream_t streams[M4_NSTREAMS]);
/* re-encode a relocated 32-bit field value into out[4] per pack kind */
void detools_m4_pack(int pk, int32_t value, uint8_t out[4]);

#endif
