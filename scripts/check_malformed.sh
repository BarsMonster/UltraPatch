#!/usr/bin/env bash
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

set -eu

W="${1:-10}"
FIX="${FIXTURES:-test-bench/fixtures}"
base="$FIX/v0_base"
one="$FIX/v1_one_face"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

./hy_enc "$base" "$one" "$tmp/grow.blob" "$W" >/dev/null 2>&1

rejects=0

emit_byte() {
  printf "\\$(printf '%03o' "$1")" >> "$2"
}

put_uleb() {
  v=$1
  out=$2
  while :; do
    b=$((v & 127))
    v=$((v >> 7))
    if [ "$v" -ne 0 ]; then b=$((b | 128)); fi
    emit_byte "$b" "$out"
    [ "$v" -eq 0 ] && break
  done
}

zeros() {
  dd if=/dev/zero bs=1 count="$1" 2>/dev/null
}

expect_reject_unchanged() {
  name=$1
  blob=$2
  source_bin=$3
  cp "$source_bin" "$tmp/$name.mem"
  if ./hy_dec "$tmp/$name.mem" "$blob" 1 >"$tmp/$name.out" 2>"$tmp/$name.err"; then
    echo "malformed case accepted: $name" >&2
    cat "$tmp/$name.err" >&2
    exit 1
  fi
  cmp "$tmp/$name.mem" "$source_bin" >/dev/null
  rejects=$((rejects + 1))
}

mk_header_blob() {
  out=$1
  from_size=$2
  to_delta_zz=$3
  : > "$out"
  head -c 4 "$tmp/grow.blob" >> "$out"
  put_uleb "$from_size" "$out"
  put_uleb "$to_delta_zz" "$out"
  zeros 16 >> "$out"
}

base_bin="$base/watch.bin"
one_bin="$one/watch.bin"

: > "$tmp/empty.blob"
expect_reject_unchanged empty "$tmp/empty.blob" "$base_bin"

head -c 11 "$tmp/grow.blob" > "$tmp/short.blob"
expect_reject_unchanged short "$tmp/short.blob" "$base_bin"

: > "$tmp/unterminated_from_size.blob"
head -c 4 "$tmp/grow.blob" >> "$tmp/unterminated_from_size.blob"
printf '\200\200\200\200\200\200' >> "$tmp/unterminated_from_size.blob"
zeros 8 >> "$tmp/unterminated_from_size.blob"
expect_reject_unchanged unterminated_from_size "$tmp/unterminated_from_size.blob" "$base_bin"

mk_header_blob "$tmp/from_size_mismatch.blob" 1 0
expect_reject_unchanged from_size_mismatch "$tmp/from_size_mismatch.blob" "$base_bin"

mk_header_blob "$tmp/to_size_underflow.blob" 1 3
expect_reject_unchanged to_size_underflow "$tmp/to_size_underflow.blob" "$base_bin"

mk_header_blob "$tmp/from_size_too_large.blob" $(((64 * 1024 * 1024) + 1)) 0
expect_reject_unchanged from_size_too_large "$tmp/from_size_too_large.blob" "$base_bin"

for n in 0 1 2 3 4 5 8 11 12 16 32 64; do
  head -c "$n" "$tmp/grow.blob" > "$tmp/trunc_$n.blob"
  expect_reject_unchanged "trunc_$n" "$tmp/trunc_$n.blob" "$base_bin"
done

blob_sz=$(wc -c < "$tmp/grow.blob")
head -c "$((blob_sz - 4))" "$tmp/grow.blob" > "$tmp/no_to_crc.blob"
expect_reject_unchanged no_to_crc "$tmp/no_to_crc.blob" "$base_bin"

cp "$tmp/grow.blob" "$tmp/appended.blob"
printf '\000\000\000\000' >> "$tmp/appended.blob"
expect_reject_unchanged appended_garbage "$tmp/appended.blob" "$base_bin"

expect_reject_unchanged wrong_current_image "$tmp/grow.blob" "$one_bin"

printf 'malformed_rejects=%u\n' "$rejects"
