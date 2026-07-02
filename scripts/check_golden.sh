#!/usr/bin/env bash
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

# Golden-wire regression: encodes a fixed set of representative pairs and compares blob
# sha256 against the committed manifest. Catches ANY wire drift — including size-neutral
# encoding changes the size gates cannot see — and enforces the end-of-development wire
# freeze: once frozen, a mismatch here means an illegal wire change.
#
# On an INTENDED wire change, regenerate the manifest in the same commit:  make golden-update
#
# Usage: check_golden.sh [check|update] [W]   (needs ./hy_enc already built)
set -eu

MODE="${1:-check}"
W="${2:-10}"
FIX="${FIXTURES:-test-bench/fixtures}"
IMG="${IMAGES:-test-bench/images}"
MANIFEST="test-bench/golden.sha256"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# name / from / to — one-face product patches both ways + small/large corpus pairs both ways
while read -r name from to; do
  [ -n "$name" ] || continue
  ./hy_enc "$from" "$to" "$tmp/$name.blob" "$W" >/dev/null
done <<EOF
oneface_grow $FIX/v0_base $FIX/v1_one_face
oneface_revert $FIX/v1_one_face $FIX/v0_base
img00_to_img15 $IMG/img_00_n3 $IMG/img_15_n83
img15_to_img00 $IMG/img_15_n83 $IMG/img_00_n3
img04_to_img02 $IMG/img_04_n24 $IMG/img_02_n14
img07_to_img08 $IMG/img_07_n40 $IMG/img_08_n46
EOF

(cd "$tmp" && sha256sum -- *.blob | sort -k2) > "$tmp/actual"

if [ "$MODE" = update ]; then
  cp "$tmp/actual" "$MANIFEST"
  echo "golden manifest updated: $MANIFEST"
elif diff -u "$MANIFEST" "$tmp/actual" >&2; then
  echo "golden_wire=OK ($(wc -l < "$MANIFEST") blobs)"
else
  echo "golden_wire=MISMATCH — wire changed. If intended, run 'make golden-update' and commit the manifest together with the change." >&2
  exit 1
fi
