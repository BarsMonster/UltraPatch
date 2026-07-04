#!/usr/bin/env bash
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

set -eu

FIX="${FIXTURES:-test-bench/fixtures}"
base="$FIX/v0_base"
one="$FIX/v1_one_face"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

./hy_enc "$base" "$one" "$tmp/grow.blob" >/dev/null 2>&1

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
  head -c 8 "$tmp/grow.blob" >> "$out"   # CRC32(from)[4] | CRC32(to)[4]
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
head -c 8 "$tmp/grow.blob" >> "$tmp/unterminated_from_size.blob"   # CRC32(from)[4] | CRC32(to)[4]
printf '\200\200\200\200\200\200' >> "$tmp/unterminated_from_size.blob"
zeros 8 >> "$tmp/unterminated_from_size.blob"
expect_reject_unchanged unterminated_from_size "$tmp/unterminated_from_size.blob" "$base_bin"

mk_header_blob "$tmp/from_size_mismatch.blob" 1 0
expect_reject_unchanged from_size_mismatch "$tmp/from_size_mismatch.blob" "$base_bin"

mk_header_blob "$tmp/to_size_underflow.blob" 1 3
expect_reject_unchanged to_size_underflow "$tmp/to_size_underflow.blob" "$base_bin"

mk_header_blob "$tmp/from_size_too_large.blob" $(((64 * 1024 * 1024) + 1)) 0
expect_reject_unchanged from_size_too_large "$tmp/from_size_too_large.blob" "$base_bin"

for n in 0 1 2 3 4 5 6 7 8 11 12 16 32 64; do
  head -c "$n" "$tmp/grow.blob" > "$tmp/trunc_$n.blob"
  expect_reject_unchanged "trunc_$n" "$tmp/trunc_$n.blob" "$base_bin"
done

# CRC32(to) now lives in the HEADER (bytes 4..7), not a trailer. Corrupt it: the decoder still
# reconstructs the correct image, then the header CRC32(to) gate fails AFTER apply -> reject
# (never a silent wrong-accept). The host demo does not persist a rejected apply, so the disk
# memfile stays unchanged (on a real device the flash IS modified before the reject).
cp "$tmp/grow.blob" "$tmp/corrupt_to_crc.blob"
printf '\336\255\276\357' | dd of="$tmp/corrupt_to_crc.blob" bs=1 seek=4 count=4 conv=notrunc >/dev/null 2>&1
expect_reject_unchanged corrupt_to_crc "$tmp/corrupt_to_crc.blob" "$base_bin"

# Truncating real range-body bytes shifts the decoder's zero-fill boundary into significant
# data -> wrong image -> CRC32(to) reject (drop 4 tail bytes).
head -c "$(( $(wc -c < "$tmp/grow.blob") - 4 ))" "$tmp/grow.blob" > "$tmp/trunc_tail4.blob"
expect_reject_unchanged trunc_tail4 "$tmp/trunc_tail4.blob" "$base_bin"

# Appended garbage: the range body is the last thing on the wire; a valid blob is consumed
# exactly to EOF, so patch_apply_run drains the callback and rejects any leftover byte. Append
# well past the range-coder flush zero-fill window (<=4 bytes; see patch_apply.h) so the drain
# sees leftover bytes and rejects (REJ_CORRUPT) regardless of that window.
cp "$tmp/grow.blob" "$tmp/appended.blob"
zeros 16 >> "$tmp/appended.blob"
expect_reject_unchanged appended_garbage "$tmp/appended.blob" "$base_bin"

expect_reject_unchanged wrong_current_image "$tmp/grow.blob" "$one_bin"

printf 'malformed_rejects=%u\n' "$rejects"
