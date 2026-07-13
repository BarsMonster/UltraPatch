#!/usr/bin/env bash
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

set -eu

: "${FIXTURES:?check_malformed.sh: FIXTURES not set to the profile-scoped corpus}"
FIX="$FIXTURES"
base="$FIX/v0_base"
one="$FIX/v1_one_face"

. "$(dirname "$0")/tempdir.sh"

: "${ULTRAPATCH:?check_malformed.sh: ULTRAPATCH not set; invoke through make check-malformed}"
if [ ! -x "$ULTRAPATCH" ]; then
  echo "malformed infrastructure failure: $ULTRAPATCH is missing or not executable" >&2
  exit 2
fi

"$ULTRAPATCH" "$base/watch.bin" "$one/watch.bin" "$tmp/grow.blob" >/dev/null 2>&1

rejects=0

# Shared wire-envelope helper: the single source of the header uLEB layout (field walk, overlong
# synthesis/detection, header synthesis). See scripts/wire_envelope.py.
wire() { python3 "$(dirname "$0")/wire_envelope.py" "$@"; }

zeros() {
  dd if=/dev/zero bs=1 count="$1" 2>/dev/null
}

controlled_decoder_rejection() {
  status=$1
  err=$2
  [ "$status" -eq 1 ] || return 1
  [ "$(wc -l < "$err")" -eq 1 ] || return 1
  grep -Eq '^(blob too short|decode error - rejected \(reason=[12]: (corrupt/truncated patch|configured partition or decoder resource/wire cap exceeded)\)|decode error - trailing bytes after counted patch body|decode error - patch from_size=[0-9]+ does not match current image size=[0-9]+)$' "$err"
}

expect_reject_unchanged() {
  name=$1
  blob=$2
  source_bin=$3
  cp "$source_bin" "$tmp/$name.mem"
  status=0
  if "$ULTRAPATCH" --decode "$tmp/$name.mem" "$blob" >"$tmp/$name.out" 2>"$tmp/$name.err"; then
    :
  else
    status=$?
  fi
  if ! cmp "$tmp/$name.mem" "$source_bin" >/dev/null; then
    echo "malformed case modified the image: $name" >&2
    exit 1
  fi
  if [ "$status" -eq 0 ]; then
    echo "malformed case accepted: $name" >&2
    cat "$tmp/$name.err" >&2
    exit 1
  fi
  if ! controlled_decoder_rejection "$status" "$tmp/$name.err"; then
    echo "malformed case dispatcher failure: $name (status=$status)" >&2
    cat "$tmp/$name.err" >&2
    exit 1
  fi
  rejects=$((rejects + 1))
}

# Synthesize a header blob with a chosen from_size / zigzag size-delta, borrowing the real blob's
# tagged-source-CRC|CRC32(to) pair. (16 zero pad bytes follow, per wire_envelope.py header.)
mk_header_blob() { wire header "$1" "$tmp/grow.blob" "$2" "$3"; }

base_bin="$base/watch.bin"
one_bin="$one/watch.bin"

: > "$tmp/empty.blob"
expect_reject_unchanged empty "$tmp/empty.blob" "$base_bin"

head -c 11 "$tmp/grow.blob" > "$tmp/short.blob"
expect_reject_unchanged short "$tmp/short.blob" "$base_bin"

: > "$tmp/unterminated_from_size.blob"
head -c 8 "$tmp/grow.blob" >> "$tmp/unterminated_from_size.blob"   # tagged source CRC[4] | CRC32(to)[4]
printf '\200\200\200\200\200\200' >> "$tmp/unterminated_from_size.blob"
zeros 8 >> "$tmp/unterminated_from_size.blob"
expect_reject_unchanged unterminated_from_size "$tmp/unterminated_from_size.blob" "$base_bin"

: > "$tmp/overflow5_from_size.blob"
head -c 8 "$tmp/grow.blob" >> "$tmp/overflow5_from_size.blob"   # tagged source CRC[4] | CRC32(to)[4]
printf '\200\200\200\200\020' >> "$tmp/overflow5_from_size.blob"
zeros 8 >> "$tmp/overflow5_from_size.blob"
expect_reject_unchanged overflow5_from_size "$tmp/overflow5_from_size.blob" "$base_bin"

# grow.blob is a DESCENDING grow patch, so its header ulebs are (0) from_size, (1) size-delta,
# (2) fp_end, (3) body_len — fp_start is absent (descending). Overlong-encoding any of the three
# plain-uleb fields (from_size/fp_end/body_len) is malformed and must reject; the size-delta uleb
# is NOT tested here because an overlong size-delta is the legitimate direction-flip marker
# (exercised by check_degrade case (c)), not a malformation.
wire overlong "$tmp/grow.blob" 0 "$tmp/overlong_from_size.blob"
expect_reject_unchanged overlong_from_size "$tmp/overlong_from_size.blob" "$base_bin"

wire overlong "$tmp/grow.blob" 2 "$tmp/overlong_fp_end.blob"
expect_reject_unchanged overlong_fp_end "$tmp/overlong_fp_end.blob" "$base_bin"

wire overlong "$tmp/grow.blob" 3 "$tmp/overlong_body_len.blob"
expect_reject_unchanged overlong_body_len "$tmp/overlong_body_len.blob" "$base_bin"

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

cp "$tmp/grow.blob" "$tmp/trailing_junk.blob"
printf '\000' >> "$tmp/trailing_junk.blob"
expect_reject_unchanged trailing_junk "$tmp/trailing_junk.blob" "$base_bin"

# CRC32(to) now lives in the HEADER (bytes 4..7), not a trailer. Corrupt it: the decoder still
# reconstructs the correct image, then the header CRC32(to) gate fails AFTER apply -> reject
# (never a silent wrong-accept). The host demo does not persist a rejected apply, so the disk
# memfile stays unchanged (on a real device the flash IS modified before the reject).
cp "$tmp/grow.blob" "$tmp/corrupt_to_crc.blob"
printf '\336\255\276\357' | dd of="$tmp/corrupt_to_crc.blob" bs=1 seek=4 count=4 conv=notrunc >/dev/null 2>&1
expect_reject_unchanged corrupt_to_crc "$tmp/corrupt_to_crc.blob" "$base_bin"

# A single interior range-body byte flip makes the decoder reconstruct a different image; the
# post-apply CRC32(to) gate then rejects it (offset 40 lands well inside the body).
cp "$tmp/grow.blob" "$tmp/body_flip.blob"
printf '\377' | dd of="$tmp/body_flip.blob" bs=1 seek=40 count=1 conv=notrunc >/dev/null 2>&1
expect_reject_unchanged body_flip "$tmp/body_flip.blob" "$base_bin"

# Truncating real range-body bytes shifts the decoder's zero-fill boundary into significant
# data -> wrong image -> CRC32(to) reject (drop 4 tail bytes).
head -c "$(( $(wc -c < "$tmp/grow.blob") - 4 ))" "$tmp/grow.blob" > "$tmp/trunc_tail4.blob"
expect_reject_unchanged trunc_tail4 "$tmp/trunc_tail4.blob" "$base_bin"

expect_reject_unchanged wrong_current_image "$tmp/grow.blob" "$one_bin"

printf 'malformed_rejects=%u\n' "$rejects"
test "$rejects" -eq 29
