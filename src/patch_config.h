/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 */

#ifndef UP_PATCH_CONFIG_H
#define UP_PATCH_CONFIG_H

#include <stdint.h>

/* The installed header set is the Cortex-M0 wire contract. Encoder and decoder include this
 * same file, so production builds have no independently selectable wire configuration. */
#if defined(CORTEX_M4)
#error "CORTEX_M4 is reserved for a future wire revision; only CORTEX_M0 is implemented"
#endif

#if defined(PATCH_WIRE_VERSION) || defined(MAX_IMAGE) || defined(WINDOW_LOG) || \
    defined(DR_KCAP_BL) || defined(DR_KCAP_EX) || defined(OUTROW) || defined(OUTROW_DEPTH)
#error "wire constants are owned by patch_config.h and cannot be overridden"
#endif

/* Unsigned 8-bit wire revision. A nonzero value is folded into the source CRC so mismatched
 * encoder/decoder revisions reject before the first flash write without growing the envelope. */
#define PATCH_WIRE_VERSION ((uint8_t)8u)

/* Plausibility cap on envelope image sizes; decoder cursors are signed 32-bit. */
#define MAX_IMAGE (64u<<20)

/* Canonical constants, shared by decoder and encoder through rc_models.h. PATCH_IMAGE_BASE and
 * PATCH_IMAGE_CAPACITY remain decoder/device integration settings because they are not wire
 * properties. WINDOW_LOG is the LZ window log; DR_KCAP_* size the always-representable
 * relocation caches; OUTROW x OUTROW_DEPTH is the uncommitted
 * NVM page window. */
#define WINDOW_LOG 10
#define DR_KCAP_BL 152
#define DR_KCAP_EX 88
#define OUTROW 256u
#define OUTROW_DEPTH 2u

/* Decoder-only CRC integration hook. `start` and `end` are image-relative byte offsets
 * describing the half-open range [start,end); the result must be the reflected IEEE CRC-32
 * used by zlib (polynomial 0xedb88320, initial/final XOR 0xffffffff). Platforms may define
 * CRC32_DECODE before any UltraPatch header to use a hardware or library implementation.
 * The default is the decoder's tableless, allocation-free implementation. */
#ifndef CRC32_DECODE
#define UP_CRC32_DECODE_DEFAULT 1
#define CRC32_DECODE(start,end) up_crc32_decode_default((start),(end))
#endif

#endif /* UP_PATCH_CONFIG_H */
