# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

# Deterministic generator for the two SYNTHETIC wire-surface pin pairs (fixed-seed LCG,
# the check_edge.sh technique) — no committed binaries. Shared by check_golden.sh (hash
# pins). (The qemu-arm apply leg that also consumed these pairs was removed 2026-07-03.)
# Usage: gen_synth_pins.py <outdir>   (writes <outdir>/<name>_{from,to}/watch.bin)
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
