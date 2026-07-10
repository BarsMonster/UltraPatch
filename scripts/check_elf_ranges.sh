#!/usr/bin/env bash
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

set -eu

: "${CC:?check_elf_ranges.sh: CC not set - invoke through make check-malformed}"
: "${CFLAGS:?check_elf_ranges.sh: CFLAGS not set - invoke through make check-malformed}"

. "$(dirname "$0")/tempdir.sh"

"$CC" $CFLAGS test-bench/elf-ranges-probe.c src/enc_elf.c src/enc_util.c \
    -Wl,--gc-sections -o "$tmp/elf-ranges-probe"
"$tmp/elf-ranges-probe" "$tmp/probe.elf"
echo "elf_ranges=OK (12 file-backed range cases)"
