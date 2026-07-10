/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 */

#ifndef UP_PATCH_CONFIG_H
#define UP_PATCH_CONFIG_H

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
#ifndef MAX_IMAGE
#define MAX_IMAGE (64u<<20)
#endif

/* Wire-affecting knobs. Each is a SINGLE shared define used by both the decoder and the host
 * encoder (encoder TUs reach these via rc_models.h -> patch_config.h), so encoder and decoder
 * cannot disagree about the wire. Each stays overridable with a matching -D, which moves BOTH
 * sides at once. WINDOW_LOG is the LZ window log; JSLOTS/OPC_CAP/DR_KCAP_* are decoder reject caps the
 * encoder plans against; OUTROW x OUTROW_DEPTH is the uncommitted NVM page window. */
#ifndef WINDOW_LOG
#define WINDOW_LOG 10
#endif
#ifndef JSLOTS
#define JSLOTS 768u
#endif
#ifndef OPC_CAP
#define OPC_CAP 80
#endif
#ifndef DR_KCAP_BL
#define DR_KCAP_BL 208
#endif
#ifndef DR_KCAP_EX
#define DR_KCAP_EX 128
#endif
#ifndef OUTROW
#define OUTROW 256u
#endif
#ifndef OUTROW_DEPTH
#define OUTROW_DEPTH 2u
#endif

#endif /* UP_PATCH_CONFIG_H */
