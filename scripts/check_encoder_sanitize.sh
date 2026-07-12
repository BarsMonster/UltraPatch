#!/usr/bin/env bash
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

# Run pointer-rich host-encoder algorithm probes under ASan+UBSan. This stays separate from
# the plain -O2 release gate; add new focused encoder probes here as their contracts grow.
set -eu

: "${CC:?check_encoder_sanitize.sh: CC not set — invoke through make check-encoder-sanitize}"
: "${CFLAGS:?check_encoder_sanitize.sh: CFLAGS not set — invoke through make check-encoder-sanitize}"
: "${ENC_SEAM_SRCS:?check_encoder_sanitize.sh: ENC_SEAM_SRCS not set}"
. "$(dirname "$0")/tempdir.sh"

san_flags="-O1 -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
span_srcs=${ENC_SEAM_SRCS//src\/enc_lz.c/}
if ! $CC $CFLAGS $san_flags -D_POSIX_C_SOURCE=200809L \
      test-bench/span-deque-probe.c $span_srcs -Wl,--gc-sections \
      -o "$tmp/span-deque" 2>"$tmp/span-deque-build.log"; then
    echo "check_encoder_sanitize: span-deque probe build failed" >&2
    sed 's/^/    /' "$tmp/span-deque-build.log" >&2
    exit 1
fi
ASAN_OPTIONS=detect_leaks=1 "$tmp/span-deque" >"$tmp/span-deque.out"
cat "$tmp/span-deque.out"
echo "encoder_sanitizers=OK (span-deque: ASan + UBSan)"
