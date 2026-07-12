#!/usr/bin/env bash
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

# Transactional host CLI output and input/output alias regression checks.
set -eu

FIX="${FIXTURES:-test-bench/fixtures}"
: "${ULTRAPATCH:?check_transactional.sh: ULTRAPATCH not set; invoke through make check-malformed}"
CC="${CC:-gcc}"
CFLAGS="${CFLAGS:--std=c99 -Wall -Wextra -Werror}"
base="$FIX/v0_base/watch.bin"
one="$FIX/v1_one_face/watch.bin"

. "$(dirname "$0")/tempdir.sh"

if [ ! -x "$ULTRAPATCH" ]; then
  echo "transaction infrastructure failure: $ULTRAPATCH is missing or not executable" >&2
  exit 2
fi

# shellcheck disable=SC2086
"$CC" $CFLAGS -fPIC -shared test-bench/txn-rename-fail.c -o "$tmp/rename-fail.so"
"$ULTRAPATCH" "$base" "$one" "$tmp/grow.blob" >/dev/null 2>&1

expect_status_2() {
  status=$1
  name=$2
  if [ "$status" -ne 2 ]; then
    echo "transaction case returned status $status instead of 2: $name" >&2
    exit 1
  fi
}

expect_no_temp() {
  path=$1
  if find "$(dirname "$path")" -maxdepth 1 -name "$(basename "$path").tmp.*" -print -quit | grep -q .; then
    echo "transaction case leaked a temporary file for $path" >&2
    exit 1
  fi
}

printf 'existing patch destination\n' > "$tmp/patch.before"
cp "$tmp/patch.before" "$tmp/output.blob"
status=0
LD_PRELOAD="$tmp/rename-fail.so" "$ULTRAPATCH" "$base" "$one" "$tmp/output.blob" \
  >"$tmp/encode.out" 2>"$tmp/encode.err" || status=$?
expect_status_2 "$status" encode_rename_failure
cmp "$tmp/output.blob" "$tmp/patch.before" >/dev/null
expect_no_temp "$tmp/output.blob"

cp "$base" "$tmp/image.before"
cp "$base" "$tmp/image.bin"
status=0
LD_PRELOAD="$tmp/rename-fail.so" "$ULTRAPATCH" --decode "$tmp/image.bin" "$tmp/grow.blob" \
  >"$tmp/decode.out" 2>"$tmp/decode.err" || status=$?
expect_status_2 "$status" decode_rename_failure
cmp "$tmp/image.bin" "$tmp/image.before" >/dev/null
expect_no_temp "$tmp/image.bin"

cp "$base" "$tmp/from.bin"
cp "$one" "$tmp/to.bin"
cp "${base%.bin}.elf" "$tmp/from.elf"
cp "${one%.bin}.elf" "$tmp/to.elf"
ln "$tmp/from.bin" "$tmp/from-output.blob"
status=0
"$ULTRAPATCH" "$tmp/from.bin" "$tmp/to.bin" "$tmp/from-output.blob" \
  >"$tmp/from-alias.out" 2>"$tmp/from-alias.err" || status=$?
expect_status_2 "$status" encode_from_alias
grep -Fq 'patch output aliases input' "$tmp/from-alias.err"
cmp "$tmp/from.bin" "$base" >/dev/null

ln -s "$tmp/to.bin" "$tmp/to-output.blob"
status=0
"$ULTRAPATCH" "$tmp/from.bin" "$tmp/to.bin" "$tmp/to-output.blob" \
  >"$tmp/to-alias.out" 2>"$tmp/to-alias.err" || status=$?
expect_status_2 "$status" encode_to_alias
grep -Fq 'patch output aliases input' "$tmp/to-alias.err"
cmp "$tmp/to.bin" "$one" >/dev/null

cp "$tmp/from.elf" "$tmp/from-sidecar.before"
status=0
"$ULTRAPATCH" "$tmp/from.bin" "$tmp/to.bin" "$tmp/from.elf" \
  >"$tmp/from-sidecar.out" 2>"$tmp/from-sidecar.err" || status=$?
expect_status_2 "$status" encode_from_sidecar_direct
grep -Fq 'patch output aliases input' "$tmp/from-sidecar.err"
cmp "$tmp/from.elf" "$tmp/from-sidecar.before" >/dev/null

ln "$tmp/to.elf" "$tmp/to-sidecar-hardlink.blob"
status=0
"$ULTRAPATCH" "$tmp/from.bin" "$tmp/to.bin" "$tmp/to-sidecar-hardlink.blob" \
  >"$tmp/to-sidecar-hardlink.out" 2>"$tmp/to-sidecar-hardlink.err" || status=$?
expect_status_2 "$status" encode_to_sidecar_hardlink
grep -Fq 'patch output aliases input' "$tmp/to-sidecar-hardlink.err"
cmp "$tmp/to.elf" "${one%.bin}.elf" >/dev/null

ln -s "$tmp/from.elf" "$tmp/from-sidecar-symlink.blob"
status=0
"$ULTRAPATCH" "$tmp/from.bin" "$tmp/to.bin" "$tmp/from-sidecar-symlink.blob" \
  >"$tmp/from-sidecar-symlink.out" 2>"$tmp/from-sidecar-symlink.err" || status=$?
expect_status_2 "$status" encode_from_sidecar_symlink
grep -Fq 'patch output aliases input' "$tmp/from-sidecar-symlink.err"
cmp "$tmp/from.elf" "$tmp/from-sidecar.before" >/dev/null

cp "$base" "$tmp/raw.bin"
"$ULTRAPATCH" "$tmp/raw.bin" "$tmp/to.bin" "$tmp/./raw.elf" \
  >"$tmp/absent-sidecar.out" 2>"$tmp/absent-sidecar.err"
[ -s "$tmp/raw.elf" ]
expect_no_temp "$tmp/raw.elf"
cp "$tmp/raw.bin" "$tmp/raw-decoded.bin"
"$ULTRAPATCH" --decode "$tmp/raw-decoded.bin" "$tmp/raw.elf" \
  >"$tmp/absent-sidecar-decode.out" 2>"$tmp/absent-sidecar-decode.err"
cmp "$tmp/raw-decoded.bin" "$tmp/to.bin" >/dev/null

cp "$tmp/grow.blob" "$tmp/aliased-image"
ln "$tmp/aliased-image" "$tmp/aliased-patch"
status=0
"$ULTRAPATCH" --decode "$tmp/aliased-image" "$tmp/aliased-patch" \
  >"$tmp/decode-alias.out" 2>"$tmp/decode-alias.err" || status=$?
expect_status_2 "$status" decode_alias
grep -Fq 'image aliases patch' "$tmp/decode-alias.err"
cmp "$tmp/aliased-image" "$tmp/grow.blob" >/dev/null

echo 'transactional_output=OK (rename failures preserve files; present aliases rejected; absent sidecar slots usable)'
