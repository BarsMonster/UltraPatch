/* A1 divide-free range-coder model definitions (shared by decoder + host encoder).
 * Binary range coder (LZMA bound: bound=(range>>12)*prob; compare) — NO division anywhere,
 * required for Cortex-M0+/ARMv6-M (no hardware divide). Header-only; both rc_v3.c (device
 * decoder) and rc_v3_enc.c (host encoder) carry their own range-coder bit I/O and only share
 * the model struct/constants below, so the wire stays bit-exact between the two. */
#ifndef RC_MODELS_H
#define RC_MODELS_H
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#define RC_KTOP (1u<<24)
#define RC_PBIT 4096u
#define RC_PHALF 2048u

/* ---- 256-symbol byte via 8-level bit-tree; logical probs[1..255] are stored as p[0..254].
 * The adaptation-shift rate is NOT stored per-tree (saves SRAM): every call site passes its constant
 * rate explicitly (lit0=5, lit1=4, dval=4) — kept identical in rc_v3.c (decode) and rc_v3_enc.c
 * (encode) for wire-exactness. */
typedef struct { uint16_t p[255]; } BitTree;
static inline void bt_init(BitTree*t){ for(int i=0;i<255;i++) t->p[i]=RC_PHALF; }

/* ---- seeded Golomb context clamp (Rice/Gamma length & dist models) ----
 * UG_CTX = context clamp. A1 uses 6: the model array is sized at (UG_CTX+1)^2 u16.
 * ENCODING-AFFECTING: rc_v3.c (hy_dec) and rc_v3_enc.c (hy_enc) must use the same value. */
#define UG_CTX 6
#define UG_C(x) ((x)<UG_CTX?(x):UG_CTX)

/* ---- order-2 token flag: 4 contexts (previous 2 flags) ---- */
typedef struct { uint16_t m[4]; int h; } Flag1;
static inline void fl_init(Flag1*f){ for(int i=0;i<4;i++) f->m[i]=RC_PHALF; f->h=0; }

#endif
