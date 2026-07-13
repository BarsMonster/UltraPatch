#!/usr/bin/env bash
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

# Build and run the focused behavioral probes against the real private encoder modules.
# Sanitizer mode reuses the same probe list and source filtering with ASan+UBSan enabled.
set -eu

: "${CC:?check_encoder_kernels.sh: CC not set}"
: "${CFLAGS:?check_encoder_kernels.sh: CFLAGS not set}"
: "${ENC_SEAM_SRCS:?check_encoder_kernels.sh: ENC_SEAM_SRCS not set}"
mode=${1:-plain}
case "$mode" in
  plain) probe_cflags="" ;;
  sanitize) probe_cflags="-O1 -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all" ;;
  *) echo "usage: $0 [plain|sanitize]" >&2; exit 2 ;;
esac
. "$(dirname "$0")/tempdir.sh"

field_srcs=${ENC_SEAM_SRCS//src\/enc_field.c/}
field_srcs=${field_srcs//src\/enc_emit.c/}
lz_srcs=${ENC_SEAM_SRCS//src\/enc_lz.c/}
bsdiff_srcs=${ENC_SEAM_SRCS//src\/enc_bsdiff.c/}

build_probe() {
  name=$1; src=$2; sources=$3
  if ! $CC $CFLAGS $probe_cflags -D_POSIX_C_SOURCE=200809L "$src" $sources \
       -Wl,--gc-sections -o "$tmp/$name" 2>"$tmp/$name-build.log"; then
    echo "encoder kernels: $name probe build failed" >&2
    sed 's/^/    /' "$tmp/$name-build.log" >&2
    exit 1
  fi
  if [ "$mode" = sanitize ]; then
    ASAN_OPTIONS=detect_leaks=1 "$tmp/$name"
  else
    "$tmp/$name"
  fi
}

build_probe field test-bench/encoder-field-probe.c "$field_srcs"
build_probe lz test-bench/encoder-lz-probe.c "$lz_srcs"
build_probe bsdiff test-bench/encoder-bsdiff-probe.c "$bsdiff_srcs"
if [ "$mode" = sanitize ]; then
  echo "encoder_sanitizers=OK (field + LZ + suffix/LCP kernels: ASan + UBSan)"
else
  echo "encoder_kernels=OK (field + LZ + suffix/LCP behavioral probes)"
fi
