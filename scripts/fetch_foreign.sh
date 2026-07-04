#!/usr/bin/env bash
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT
#
# Reproduce the tracked foreign-firmware corpus under test-bench/foreign/.
#
# The corpus is a SECOND, unrelated Cortex-M0+ lineage (Adafruit CircuitPython
# for feather_m0_express, ATSAMD21) used to gate A1 against firmware it was not
# tuned on. 18 official release images, pinned to two contiguous families split
# by one cross-major jump:
#   old (raw .bin, app already at 0x2000): 2.2.0 2.2.1 2.2.2 2.2.3 2.2.4
#                                          2.3.0 2.3.1 3.0.0 3.0.1 3.0.2 3.0.3
#   new (UF2, unpacked to app base 0x2000): 10.0.0 10.0.1 10.0.2 10.0.3
#                                           10.1.1 10.1.2 10.1.3
# The 3.0.3 -> 10.0.0 boundary is the cross-major pair (effectively unrelated
# programs; exercises the encoder's extreme-degradation path).
#
# Binaries are MIT-licensed third-party build artifacts and ARE tracked in-repo
# (test-bench/foreign/<ver>/watch.bin). This script only needs re-running to
# regenerate them from scratch; test-bench/foreign.sha256 pins the exact bytes.
#
# Usage: scripts/fetch_foreign.sh          (writes into test-bench/foreign/)
#        scripts/fetch_foreign.sh --verify (regenerate to a temp dir, diff vs manifest)

set -eu

S3=https://adafruit-circuit-python.s3.amazonaws.com/bin/feather_m0_express
OLD_BASE="$S3/en_US/OLD"     # pre-locale-split raw .bin releases
NEW_BASE="$S3/en_US"         # UF2 releases
NAME=adafruit-circuitpython-feather_m0_express

OLD_VERS="2.2.0 2.2.1 2.2.2 2.2.3 2.2.4 2.3.0 2.3.1 3.0.0 3.0.1 3.0.2 3.0.3"
NEW_VERS="10.0.0 10.0.1 10.0.2 10.0.3 10.1.1 10.1.2 10.1.3"

here="$(cd "$(dirname "$0")/.." && pwd)"
unpack="$here/scripts/uf2_unpack.py"

verify=0
[ "${1:-}" = "--verify" ] && verify=1
dest="$here/test-bench/foreign"
if [ "$verify" -eq 1 ]; then
  dest="$(mktemp -d)"
  trap 'rm -rf "$dest"' EXIT
fi

fetch() {   # url -> stdout, with a hard timeout (owner rule: bound every op)
  curl -fsSL --max-time 120 "$1"
}

for v in $OLD_VERS; do
  mkdir -p "$dest/$v"
  fetch "$OLD_BASE/$NAME-$v.bin" > "$dest/$v/watch.bin"
done
for v in $NEW_VERS; do
  mkdir -p "$dest/$v"
  tmp="$(mktemp)"
  fetch "$NEW_BASE/$NAME-en_US-$v.uf2" > "$tmp"
  python3 "$unpack" "$tmp" "$dest/$v/watch.bin" 0x2000
  rm -f "$tmp"
done

if [ "$verify" -eq 1 ]; then
  # rewrite manifest paths onto the temp tree and check
  sed "s#test-bench/foreign#$dest#" "$here/test-bench/foreign.sha256" | sha256sum -c -
  echo "foreign corpus reproduces the tracked manifest byte-for-byte"
else
  echo "wrote 18 foreign images to $dest"
  echo "re-pin with: find test-bench/foreign -name watch.bin | sort | xargs sha256sum > test-bench/foreign.sha256"
fi
