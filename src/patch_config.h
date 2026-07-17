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
#define PATCH_WIRE_VERSION ((uint8_t)1u)

/* Plausibility cap on envelope image sizes; decoder cursors are signed 32-bit. */
#define MAX_IMAGE (64u<<20)

/* Canonical constants, shared by decoder and encoder through rc_models.h. PATCH_IMAGE_BASE and
 * PATCH_IMAGE_CAPACITY remain decoder/device integration settings because they are not wire
 * properties. The values below are adjustable by editing this file — e.g. retarget OUTROW to the
 * hardware erase-page size — but encoder and decoder MUST be rebuilt from the same values; when
 * retargeting for a product, also change PATCH_WIRE_VERSION so mismatched patches reject before
 * the first flash write. WINDOW_LOG is the LZ window log; DR_KCAP_* size the always-representable
 * relocation caches; OUTROW is the NVM erase-page size (flash is changed only in whole erase
 * pages; a smaller program page is the driver's concern); OUTROW x OUTROW_DEPTH is the
 * uncommitted NVM page window. */
#define WINDOW_LOG 11
#define DR_KCAP_BL 152
#define DR_KCAP_EX 88
#define OUTROW 256u
#define OUTROW_DEPTH 2u

/* Decoder-only CRC integration hook. `start` is an absolute device address that already
 * includes PATCH_IMAGE_BASE, and `size` is the number of bytes to checksum; the result must
 * be the reflected IEEE CRC-32 used by zlib (polynomial 0xedb88320, initial/final XOR
 * 0xffffffff). Platforms may define
 * CRC32_DECODE before any UltraPatch header to use a hardware or library implementation.
 * The default is the decoder's tableless, allocation-free implementation. */
#ifndef CRC32_DECODE
#define UP_CRC32_DECODE_DEFAULT 1
#define CRC32_DECODE(start,size) up_crc32_decode_default((start),(size))
#endif

/* Optional decoder-side notification hook. When defined before any UltraPatch header, the
 * decoder invokes it exactly once per apply, after the source image is fully validated
 * (revision-tagged CRC32(from) match) and the pre-body flash scans are complete — immediately
 * before the first compressed-body byte is pulled. Not called when validation fails. A streaming
 * sender can hold the patch body until the device signals ready: send the envelope header first
 * (12..27 bytes; the first 27 patch bytes are always sufficient), wait for this hook's signal,
 * then stream the remainder — the receive ring then covers only ordinary decode bursts, not the
 * full-image scans. Default: no-op. */
#ifndef CRC32_READY
#define CRC32_READY() ((void)0)
#endif

#endif /* UP_PATCH_CONFIG_H */
