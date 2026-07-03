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

# Two SYNTHETIC wire-surface pins the six in-repo blobs never exercise: a journal-BUDGET-degraded
# blob (over-budget read-after-overwrite shipped as plain extras) and an UNNATURAL-DIRECTION blob
# (apply direction flipped, signaled by an overlong size-delta uLEB). Generated deterministically
# here (fixed-seed LCG, the check_edge.sh technique) — no committed binaries. Purely additive: the
# six existing hash lines stay byte-identical (verify in the git diff on any manifest update).
SFIX="$tmp/synthfix"
python3 - "$SFIX" <<'EOF'
import os, sys
root = sys.argv[1]
def lcg(seed):
    s = seed & 0xffffffff
    while True:
        s = (s * 1664525 + 1013904223) & 0xffffffff
        yield (s >> 16) & 0xff
def rnd(n, seed):
    r = lcg(seed); return bytes(next(r) for _ in range(n))
def pair(name, frm, to):
    for role, data in (("from", frm), ("to", to)):
        d = os.path.join(root, name + "_" + role); os.makedirs(d, exist_ok=True)
        open(os.path.join(d, "watch.bin"), "wb").write(data)
# journal-degraded: swap the two 2048 B halves (block read 2048 B behind the frontier -> the
# ideal plan wants 2x the JSLOTS budget; the encoder degrades the over-budget half to extras).
b = rnd(4096, 88); pair("synth_journal_degrade", b, b[2048:] + b[:2048])
# unnatural-direction: equal-size image, region [256,3400) shifted RIGHT by 600 B; ascending
# apply would journal the whole region, so the encoder flips to descending (overlong marker).
b = rnd(4096, 444); ins = rnd(600, 444 ^ 0x5a5a5a5a)
pair("synth_unnatural_dir", b, b[:256] + ins + b[256:3400] + b[4000:])
EOF

# name / from / to — one-face product patches both ways + small/large corpus pairs both ways,
# then the two synthetic degradation pins (appended -> they sort last; existing lines untouched).
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
synth_journal_degrade $SFIX/synth_journal_degrade_from $SFIX/synth_journal_degrade_to
synth_unnatural_dir $SFIX/synth_unnatural_dir_from $SFIX/synth_unnatural_dir_to
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
