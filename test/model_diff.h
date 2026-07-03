/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 */

/* Model-level encoder/decoder differential test — shared interface.
 *
 * The encoder and decoder each carry an INDEPENDENT copy of every entropy-model pair
 * (patch_generate.c re_bit / bt_encode / ug_encode / ... vs patch_apply.h s_bit / s_bt /
 * s_ug_rice / pull_delta / ...), kept bit-exact only by discipline. The golden gate
 * catches drift only for the exact
 * symbol values the corpus exercises, and only as "some blob's sha changed". This test
 * localizes any future mirror bug to the exact model, with full value-space coverage:
 * for each model pair it encodes a deterministic random symbol stream with the ENCODER-side
 * model and decodes it with the DECODER-side model, asserting exact symbol recovery.
 *
 * Two TUs, no wire touched:
 *   - test/model_diff.c      #includes src/patch_generate.c (no RC_V3_ENC_MAIN => no main);
 *                            owns the LCG, the value generators, orchestration, and every
 *                            ENCODER-side stream helper (it can reach the file-static encoder
 *                            models directly). Prints the summary and exits.
 *   - test/model_diff_dec.c  #includes src/patch_apply.h with stub flash primitives; exposes
 *                            the DECODER-side whole-stream bridges below, feeding the encoder
 *                            body straight to the callback (no trailer-withhold ring — CRC32(to)
 *                            rides in the header, so the range body is the last thing on the wire).
 * The mixed-stream opcodes are single-sourced here so the two sides can never drift on the
 * model-selection or the fixed rate/k choices that drive the shared range-coder session. */

#ifndef MODEL_DIFF_H
#define MODEL_DIFF_H
#include <stdint.h>
#include <stddef.h>

/* Mixed-stream opcodes: one persistent model object per kind, driven in the SAME order by the
 * encoder driver (enc_mixed) and the decoder bridge (md_mixed) through ONE range-coder session,
 * so adaptation state drifts identically on both sides across model kinds. */
enum { MX_RAW = 0, MX_BIT = 1, MX_BT = 2, MX_UGR = 3, MX_UGG = 4, MX_FLAG = 5, MX_N = 6 };
#define MX_UGR_K   3
#define MX_BT_RATE 5
#define MX_BIT_RATE 4

/* Decoder-side bridges (implemented in model_diff_dec.c). Each primes the byte source over the
 * encoder body, runs rc_init(), decodes exactly `ns` symbols of ONE model into `got`, and
 * returns the decoder's g_rcerr (0 == clean). */
int md_bits   (const uint8_t *b, size_t n, int rate, int ns, uint8_t  *got);
int md_bt     (const uint8_t *b, size_t n, int rate, int ns, uint32_t *got);
int md_bv     (const uint8_t *b, size_t n,           int ns, int32_t  *got);
int md_ugr    (const uint8_t *b, size_t n, int k,    int ns, uint32_t *got);
int md_ugg    (const uint8_t *b, size_t n, int depth,int ns, uint32_t *got);
int md_idx    (const uint8_t *b, size_t n,           int ns, uint32_t *got);
int md_flag   (const uint8_t *b, size_t n,           int ns, uint8_t  *got);
int md_rep0   (const uint8_t *b, size_t n,           int ns, uint8_t  *got);
int md_rawbits(const uint8_t *b, size_t n, int nb,   int ns, uint32_t *got);
int md_rawgz  (const uint8_t *b, size_t n,           int ns, uint32_t *got);
int md_mtf    (const uint8_t *b, size_t n, int cap,  int ns, int32_t  *got);
int md_mixed  (const uint8_t *b, size_t n, const uint8_t *ops, int ns, uint32_t *got);

#endif /* MODEL_DIFF_H */
