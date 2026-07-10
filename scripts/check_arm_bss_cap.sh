#!/usr/bin/env bash
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

set -eu

MAKE_CMD="${MAKE:-make}"
. "$(dirname "$0")/tempdir.sh"

harness="printf '%s\\n' '#include \"patch_apply.h\"' 'static PatchApply g_patch_apply_state;' 'uint8_t arm_bss_limit_probe[12289];' 'int rcv3_run(int (*next)(void*, uint8_t*), void *ctx){ return patch_apply_run(&g_patch_apply_state, next, ctx); }' > \"\$\$tmp/patch_apply_arm.c\""

if "$MAKE_CMD" --no-print-directory \
    ARM_BSS_HARD_CAP=999999 BASE_ARM_BSS=999999 ARM_APPLY_HARNESS="$harness" \
    check-arm-measure-internal >"$tmp/command-line.out" 2>&1; then
  echo "command-line override disabled the ARM .bss hard cap" >&2
  exit 1
fi
grep -q '^ARM \.bss hard cap exceeded: [0-9][0-9]* > 12288$' "$tmp/command-line.out"

if ARM_BSS_HARD_CAP= BASE_ARM_BSS=999999 ARM_APPLY_HARNESS="$harness" \
    "$MAKE_CMD" -e --no-print-directory check-arm-measure-internal \
    >"$tmp/environment.out" 2>&1; then
  echo "environment override disabled the ARM .bss hard cap" >&2
  exit 1
fi
grep -q '^ARM \.bss hard cap exceeded: [0-9][0-9]* > 12288$' "$tmp/environment.out"

linked_stubs="$tmp/arm_link_bss_probe.c"
printf '%s\n' '#include <stdint.h>' \
    '#include "patch_config.h"' \
    'uint8_t arm_link_bss_limit_probe[12289];' \
    'uint8_t flash_read(uint32_t addr) { (void)addr; return 0; }' \
    'void flash_write_page(uint32_t addr, const uint8_t page[OUTROW]) { (void)addr; (void)page; }' \
    >"$linked_stubs"

if "$MAKE_CMD" --no-print-directory \
    ARM_BSS_HARD_CAP=999999 BASE_ARM_LINKED_BSS=999999 ARM_LINK_STUBS="$linked_stubs" \
    check-arm-measure-internal >"$tmp/linked-command-line.out" 2>&1; then
  echo "command-line override disabled the ARM linked .bss hard cap" >&2
  exit 1
fi
grep -q '^ARM linked \.bss hard cap exceeded: [0-9][0-9]* > 12288$' "$tmp/linked-command-line.out"

if ARM_BSS_HARD_CAP= BASE_ARM_LINKED_BSS=999999 ARM_LINK_STUBS="$linked_stubs" \
    "$MAKE_CMD" -e --no-print-directory check-arm-measure-internal \
    >"$tmp/linked-environment.out" 2>&1; then
  echo "environment override disabled the ARM linked .bss hard cap" >&2
  exit 1
fi
grep -q '^ARM linked \.bss hard cap exceeded: [0-9][0-9]* > 12288$' "$tmp/linked-environment.out"

echo "arm_bss_hard_cap_overrides=REJECTED (object + linked)"
