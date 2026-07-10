#!/usr/bin/env bash
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

# Real one-face product patch metric, shared by gate and A/B compression checks.
# Usage: scripts/oneface_metrics.sh [encoder] [decoder]
# The encoder is required as argument 1 or ULTRAPATCH; argument 2 defaults to that encoder.
# Env: ULTRAPATCH, FIXTURES, ONEFACE_ROUNDTRIP=1, ONEFACE_WIRE_HASHES=1
#      BASE_ONEFACE_GROW / BASE_ONEFACE_REVERT  one-face acceptance caps (AGENTS.md calls this
#        rule authoritative): enforced only when set (exit nonzero on a breach), skipped for a
#        bare measurement run when unset — the single enforcement site, mirroring the corpus-leg
#        ratchets in check_corpus.sh.
set -eu

ENC="${1:-${ULTRAPATCH:-}}"
: "${ENC:?oneface_metrics.sh: pass an encoder or set ULTRAPATCH}"
DEC="${2:-$ENC}"
FIX="${FIXTURES:-test-bench/fixtures}"

[ -x "$ENC" ] || { echo "oneface_metrics.sh: encoder is missing or not executable: $ENC" >&2; exit 2; }
if [ "${ONEFACE_ROUNDTRIP:-0}" != 0 ] && [ ! -x "$DEC" ]; then
  echo "oneface_metrics.sh: decoder is missing or not executable: $DEC" >&2
  exit 2
fi

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

grow_sz=$(wc -c < "$tmp/grow.blob")
revert_sz=$(wc -c < "$tmp/revert.blob")
printf 'oneface_grow=%d\noneface_revert=%d\n' "$grow_sz" "$revert_sz"
if [ "${ONEFACE_WIRE_HASHES:-0}" != 0 ]; then
  printf 'oneface_grow_sha256=%s\noneface_revert_sha256=%s\n' \
    "$(sha256sum "$tmp/grow.blob" | cut -d' ' -f1)" \
    "$(sha256sum "$tmp/revert.blob" | cut -d' ' -f1)"
fi

# Acceptance ratchets (single source): enforce the authoritative one-face caps only when the
# env pins are set; a bare measurement run (pins unset/empty) just prints the sizes above.
rc=0
if [ -n "${BASE_ONEFACE_GROW:-}" ] && [ "$grow_sz" -gt "$BASE_ONEFACE_GROW" ]; then
  echo "oneface_metrics.sh: grow ratchet (oneface_grow=$grow_sz > BASE_ONEFACE_GROW=$BASE_ONEFACE_GROW)" >&2
  rc=1
fi
if [ -n "${BASE_ONEFACE_REVERT:-}" ] && [ "$revert_sz" -gt "$BASE_ONEFACE_REVERT" ]; then
  echo "oneface_metrics.sh: revert ratchet (oneface_revert=$revert_sz > BASE_ONEFACE_REVERT=$BASE_ONEFACE_REVERT)" >&2
  rc=1
fi
exit "$rc"
