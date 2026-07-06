/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 *
 * Standalone compatibility wrapper. The host CLI and encoder selfcheck link
 * src/patch_host_backend.c directly so the reference decoder appears once in
 * ultrapatch. Some tests still compile this file directly with
 * PATCH_APPLY_DEMO_MAIN to build a standalone decoder variant.
 */
#ifdef PATCH_APPLY_DEMO_MAIN
#include "patch_host_backend.c"
#endif
