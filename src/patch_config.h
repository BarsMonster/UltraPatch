/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 */

#ifndef A1_PATCH_CONFIG_H
#define A1_PATCH_CONFIG_H

/* ---- target-family wire contract ----
 * The A1 wire is target-family-specific. CORTEX_M0 (Thumb-1/ARMv6-M, the implemented
 * family) must be defined for BOTH the encoder and the decoder TU. CORTEX_M4 is RESERVED
 * for a future Thumb-2 wire revision and may change the wire format. */
#if !defined(CORTEX_M0) && !defined(CORTEX_M4)
#error "define CORTEX_M0 for both the encoder and the decoder build (CORTEX_M4 is reserved)"
#endif
#ifdef CORTEX_M4
#error "CORTEX_M4 is reserved for a future wire revision; only CORTEX_M0 is implemented"
#endif

/* Plausibility cap on envelope image sizes. A deployment may tighten this to its real
 * flash size; keep it <2^31 because decoder cursors are signed 32-bit. */
#ifndef A1_MAX_IMAGE
#define A1_MAX_IMAGE (64u<<20)
#endif

/* Decoder resource-cap / window DEFAULTS. Encoder and decoder expose separate override
 * names where compatibility allows it, but every default lives here. */
#define RC_JSLOTS_DEFAULT      768u
#define RC_OPC_CAP_DEFAULT     80
#define RC_DR_KCAP_BL_DEFAULT  208
#define RC_DR_KCAP_EX_DEFAULT  128
#define RC_WINDOW_LOG_DEFAULT  10

/* NVM row-window DEFAULTS. A decoder may be a monotone-compatible superset of the
 * encoder assumption, so encoder and decoder override names remain separate. */
#define RC_OUTROW_DEFAULT      256u
#define RC_ROW_DEPTH_DEFAULT   2u

/* Encoder mirror knobs. */
#ifndef PATHE_W
#define PATHE_W RC_WINDOW_LOG_DEFAULT
#endif
#ifndef A1_JSLOTS
#define A1_JSLOTS RC_JSLOTS_DEFAULT
#endif
#ifndef A1_OPC_CAP
#define A1_OPC_CAP RC_OPC_CAP_DEFAULT
#endif
#ifndef A1_OUTROW
#define A1_OUTROW RC_OUTROW_DEFAULT
#endif
#ifndef A1_ROW_DEPTH
#define A1_ROW_DEPTH RC_ROW_DEPTH_DEFAULT
#endif

/* Decoder knobs. */
#ifndef SA_W
#define SA_W RC_WINDOW_LOG_DEFAULT
#endif
#ifndef JSLOTS
#define JSLOTS RC_JSLOTS_DEFAULT
#endif
#ifndef OPC_CAP
#define OPC_CAP RC_OPC_CAP_DEFAULT
#endif
#ifndef DR_KCAP_BL
#define DR_KCAP_BL RC_DR_KCAP_BL_DEFAULT
#endif
#ifndef DR_KCAP_EX
#define DR_KCAP_EX RC_DR_KCAP_EX_DEFAULT
#endif
#ifndef OUTROW
#ifdef NVM_ROW
#define OUTROW NVM_ROW
#else
#define OUTROW RC_OUTROW_DEFAULT
#endif
#endif
#ifndef OUTROW_DEPTH
#define OUTROW_DEPTH RC_ROW_DEPTH_DEFAULT
#endif

#endif /* A1_PATCH_CONFIG_H */
