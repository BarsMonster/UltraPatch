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
lz_srcs=${ENC_SEAM_SRCS//src\/enc_lz.c/}
if ! $CC $CFLAGS $san_flags -D_POSIX_C_SOURCE=200809L \
      test-bench/encoder-lz-probe.c $lz_srcs -Wl,--gc-sections \
      -o "$tmp/encoder-lz" 2>"$tmp/encoder-lz-build.log"; then
    echo "check_encoder_sanitize: LZ probe build failed" >&2
    sed 's/^/    /' "$tmp/encoder-lz-build.log" >&2
    exit 1
fi
ASAN_OPTIONS=detect_leaks=1 "$tmp/encoder-lz" \
  "$tmp/span-deque.tsv" "$tmp/out-envelope.tsv" >"$tmp/encoder-lz.out"
cat "$tmp/encoder-lz.out"

field_srcs=${ENC_SEAM_SRCS//src\/enc_field.c/}
field_srcs=${field_srcs//src\/enc_emit.c/}
if ! $CC $CFLAGS $san_flags -D_POSIX_C_SOURCE=200809L \
      test-bench/encoder-field-probe.c $field_srcs -Wl,--gc-sections \
      -o "$tmp/encoder-field" 2>"$tmp/encoder-field-build.log"; then
    echo "check_encoder_sanitize: field probe build failed" >&2
    sed 's/^/    /' "$tmp/encoder-field-build.log" >&2
    exit 1
fi
ASAN_OPTIONS=detect_leaks=1 "$tmp/encoder-field" \
  "$tmp/ldr-index.tsv" "$tmp/smap-trim.tsv" >"$tmp/encoder-field.out"
cat "$tmp/encoder-field.out"
echo "encoder_sanitizers=OK (field + LZ kernels: ASan + UBSan)"
