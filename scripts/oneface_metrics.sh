#!/usr/bin/env bash
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

# Real one-face product patch metric, shared by gate and A/B compression checks.
# Usage: scripts/oneface_metrics.sh [encoder] [decoder]
# Env: FIXTURES, ONEFACE_ROUNDTRIP=1
set -eu

ENC="${1:-./ultrapatch}"
DEC="${2:-$ENC}"
FIX="${FIXTURES:-test-bench/fixtures}"

. "$(dirname "$0")/tempdir.sh"

"$ENC" "$FIX/v0_base/watch.bin" "$FIX/v1_one_face/watch.bin" "$tmp/grow.blob" >/dev/null 2>&1
"$ENC" "$FIX/v1_one_face/watch.bin" "$FIX/v0_base/watch.bin" "$tmp/revert.blob" >/dev/null 2>&1

if [ "${ONEFACE_ROUNDTRIP:-0}" != 0 ]; then
  cp "$FIX/v0_base/watch.bin" "$tmp/grow.mem"
  "$DEC" --decode "$tmp/grow.mem" "$tmp/grow.blob" >/dev/null 2>&1
  cmp -s "$tmp/grow.mem" "$FIX/v1_one_face/watch.bin"

  cp "$FIX/v1_one_face/watch.bin" "$tmp/revert.mem"
  "$DEC" --decode "$tmp/revert.mem" "$tmp/revert.blob" >/dev/null 2>&1
  cmp -s "$tmp/revert.mem" "$FIX/v0_base/watch.bin"
fi

printf 'oneface_grow=%d\noneface_revert=%d\n' \
  "$(wc -c < "$tmp/grow.blob")" "$(wc -c < "$tmp/revert.blob")"
