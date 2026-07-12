#!/usr/bin/env bash
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

# Build the three source-including encoder probes, require GCC/Clang semantic identity, and
# either check the five pinned result streams or write a candidate manifest for explicit publish.
set -eu

: "${CC:?check_encoder_kernels.sh: CC not set}"
: "${CLANG:?check_encoder_kernels.sh: CLANG not set}"
: "${CFLAGS:?check_encoder_kernels.sh: CFLAGS not set}"
: "${ENC_SEAM_SRCS:?check_encoder_kernels.sh: ENC_SEAM_SRCS not set}"
: "${ENCODER_KERNEL_BASELINE:?check_encoder_kernels.sh: baseline not set}"
mode=${1:-check}
case "$mode" in check|candidate) ;; *) echo "usage: $0 check|candidate" >&2; exit 2 ;; esac
. "$(dirname "$0")/tempdir.sh"

surfaces="ldr-index smap-trim span-deque out-envelope suffix-lcp"

build_and_run() {
  label=$1; compiler=$2; out="$tmp/$label"
  mkdir "$out"
  field_srcs=${ENC_SEAM_SRCS//src\/enc_field.c/}
  field_srcs=${field_srcs//src\/enc_emit.c/}
  lz_srcs=${ENC_SEAM_SRCS//src\/enc_lz.c/}
  bsdiff_srcs=${ENC_SEAM_SRCS//src\/enc_bsdiff.c/}
  if ! $compiler $CFLAGS -D_POSIX_C_SOURCE=200809L test-bench/encoder-field-probe.c \
       $field_srcs -Wl,--gc-sections -o "$out/field" 2>"$out/field-build.log"; then
    echo "encoder kernels: $label field probe build failed" >&2
    sed 's/^/    /' "$out/field-build.log" >&2; exit 1
  fi
  if ! $compiler $CFLAGS -D_POSIX_C_SOURCE=200809L test-bench/encoder-lz-probe.c \
       $lz_srcs -Wl,--gc-sections -o "$out/lz" 2>"$out/lz-build.log"; then
    echo "encoder kernels: $label LZ probe build failed" >&2
    sed 's/^/    /' "$out/lz-build.log" >&2; exit 1
  fi
  if ! $compiler $CFLAGS -D_POSIX_C_SOURCE=200809L test-bench/encoder-bsdiff-probe.c \
       $bsdiff_srcs -Wl,--gc-sections -o "$out/bsdiff" 2>"$out/bsdiff-build.log"; then
    echo "encoder kernels: $label bsdiff probe build failed" >&2
    sed 's/^/    /' "$out/bsdiff-build.log" >&2; exit 1
  fi
  "$out/field" "$out/ldr-index.tsv" "$out/smap-trim.tsv" >"$out/field.out"
  "$out/lz" "$out/span-deque.tsv" "$out/out-envelope.tsv" >"$out/lz.out"
  "$out/bsdiff" "$out/suffix-lcp.tsv" >"$out/bsdiff.out"
}

build_and_run cc "$CC"
build_and_run clang "$CLANG"
for surface in $surfaces; do
  if ! cmp -s "$tmp/cc/$surface.tsv" "$tmp/clang/$surface.tsv"; then
    keep=$(mktemp -d "${TMPDIR:-/tmp}/ultrapatch-kernel-mismatch.XXXXXX")
    cp "$tmp/cc/$surface.tsv" "$keep/cc-$surface.tsv"
    cp "$tmp/clang/$surface.tsv" "$keep/clang-$surface.tsv"
    echo "encoder kernel compiler mismatch: $surface (streams preserved at $keep)" >&2
    exit 1
  fi
done

candidate="$tmp/encoder-kernel-baseline.tsv"
{
  printf 'format\tencoder-kernel-baseline-v1\n'
  printf 'surface\trecords\tsha256\n'
  for surface in $surfaces; do
    records=$(wc -l <"$tmp/cc/$surface.tsv")
    hash=$(sha256sum "$tmp/cc/$surface.tsv"); hash=${hash%% *}
    printf '%s\t%s\t%s\n' "$surface" "$records" "$hash"
  done
} >"$candidate"

if [ "$mode" = candidate ]; then
  : "${ENCODER_KERNEL_BASELINE_DUMP:?candidate mode requires ENCODER_KERNEL_BASELINE_DUMP}"
  cp "$candidate" "$ENCODER_KERNEL_BASELINE_DUMP"
else
  if ! cmp -s "$candidate" "$ENCODER_KERNEL_BASELINE"; then
    keep=$(mktemp -d "${TMPDIR:-/tmp}/ultrapatch-kernel-mismatch.XXXXXX")
    cp "$candidate" "$keep/actual-baseline.tsv"
    for surface in $surfaces; do cp "$tmp/cc/$surface.tsv" "$keep/$surface.tsv"; done
    echo "encoder kernel baseline mismatch (actual manifest and streams preserved at $keep)" >&2
    diff -u "$ENCODER_KERNEL_BASELINE" "$candidate" >&2 || :
    exit 1
  fi
fi

cat "$tmp/cc/field.out" "$tmp/cc/lz.out" "$tmp/cc/bsdiff.out"
echo "encoder_kernel_baseline=OK surfaces=5 compilers=cc/clang"
