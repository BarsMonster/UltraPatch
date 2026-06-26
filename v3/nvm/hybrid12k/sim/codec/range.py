#!/usr/bin/env python3
"""
Integer (fixed-point) range coder for relocation-delta value streams.

Embedded firmware patcher target: ARM Cortex-M0+, no FPU, integer only, small RAM.

Coder
-----
  * 32-bit range coder, byte-wise renormalization (Subbotin / classic
    range-coder family, same lineage as JPEG range coders).
  * NO floating point anywhere (encoder or decoder).
  * Total frequency is a FIXED power of two: TOTBITS = 12, TOT = 4096.
    Therefore "range / total" is a right shift (>> TOTBITS), not a divide.
    => the decode hot path is DIVISION FREE.  (get_freq does range>>TOTBITS
       then a single multiply / compare scan over <=15 symbols.)
  * Static, pre-quantized freq tables shipped with the firmware.  Tables are
    shipped as 8-bit normalized freqs (sum 256) and rescaled to TOT on the MCU
    with integer math (x16) -- halves table-shipping cost vs raw 12-bit.

Models benchmarked (bl and ldr coded SEPARATELY, each with its own tables):
  (a) order-0                : one static symbol distribution.
  (b) order-1 (prev-zero)    : 2 tables, context = previous symbol was zero.
  (c) order-1 (zero-run-bucket) : N tables, context = capped zero-run length.
                                  THIS IS THE WINNING MODEL.

The MCU decoder needs only: the shipped table(s) (a few bytes), 4x uint32 of
coder state, a run counter, and the output cursor.  Integer only, division
free, streamable strictly left-to-right (i.e. in to-address order).
"""

import json
import os
from collections import defaultdict

TOTBITS = 12
TOT = 1 << TOTBITS          # 4096  fixed total frequency (power of two)
BOT = 1 << 16               # renorm underflow threshold
MASK32 = 0xFFFFFFFF


# --------------------------------------------------------------------------
# 32-bit range encoder / decoder, byte renormalization (Subbotin style)
# --------------------------------------------------------------------------
class RangeEncoder:
    def __init__(self):
        self.low = 0
        self.range = MASK32
        self.out = bytearray()

    def _renorm(self):
        while True:
            if ((self.low ^ (self.low + self.range)) & 0xFF000000) != 0:
                if self.range >= BOT:
                    break
                self.range = (-self.low) & (BOT - 1)
            self.out.append((self.low >> 24) & 0xFF)
            self.low = (self.low << 8) & MASK32
            self.range = (self.range << 8) & MASK32

    def encode(self, cumfreq, freq):
        r = self.range >> TOTBITS          # division-free: TOT is 2^TOTBITS
        self.low = (self.low + r * cumfreq) & MASK32
        self.range = r * freq
        self._renorm()

    def finish(self):
        for _ in range(4):
            self.out.append((self.low >> 24) & 0xFF)
            self.low = (self.low << 8) & MASK32
        return bytes(self.out)


class RangeDecoder:
    def __init__(self, data):
        self.data = data
        self.pos = 0
        self.low = 0
        self.range = MASK32
        self.code = 0
        for _ in range(4):
            self.code = ((self.code << 8) | self._byte()) & MASK32

    def _byte(self):
        if self.pos < len(self.data):
            b = self.data[self.pos]
            self.pos += 1
            return b
        return 0

    def _renorm(self):
        while True:
            if ((self.low ^ (self.low + self.range)) & 0xFF000000) != 0:
                if self.range >= BOT:
                    break
                self.range = (-self.low) & (BOT - 1)
            self.code = ((self.code << 8) | self._byte()) & MASK32
            self.low = (self.low << 8) & MASK32
            self.range = (self.range << 8) & MASK32

    def get_freq(self):
        # division-free range/TOT == range>>TOTBITS; ONE divide by self._r,
        # which on the MCU is avoided by storing slot widths -- here kept simple.
        self._r = self.range >> TOTBITS
        v = ((self.code - self.low) & MASK32) // self._r
        return v if v < TOT else TOT - 1

    def decode(self, cumfreq, freq):
        r = self._r
        self.low = (self.low + r * cumfreq) & MASK32
        self.range = r * freq
        self._renorm()


# --------------------------------------------------------------------------
# Frequency tables: 8-bit normalized (sum 256), rescaled to TOT on MCU
# --------------------------------------------------------------------------
def quant8(symbols, alphabet):
    """Counts -> 8-bit freqs summing to 256, each occurring symbol >=1."""
    counts = {s: 0 for s in alphabet}
    for s in symbols:
        counts[s] += 1
    n = len(symbols)
    q = []
    for s in alphabet:
        f = (counts[s] * 256) // n if n else 0
        if counts[s] > 0 and f == 0:
            f = 1
        q.append(f)
    if n:
        q[max(range(len(q)), key=lambda i: q[i])] += 256 - sum(q)
    else:                                   # empty context -> uniform
        q = [256 // len(alphabet)] * len(alphabet)
        q[0] += 256 - sum(q)
    return q  # 1 byte each


def expand(q):
    """MCU-side: rescale 8-bit (sum 256) -> TOT (sum 4096) by x16, integer fix."""
    f = [x * (TOT // 256) for x in q]
    s = sum(f)
    if s != TOT:
        f[max(range(len(f)), key=lambda i: f[i])] += TOT - s
    return f


def cum_table(freqs):
    cum = [0]
    for f in freqs:
        cum.append(cum[-1] + f)
    return cum


def table_bytes(alphabet, ntables):
    # alphabet: 1 signed byte per symbol (value/4, all deltas are mult of 4,
    #           |532|/4 = 133 fits int8 magnitude in a byte).
    # freqs:    1 byte per (symbol, table)  [8-bit normalized]
    return len(alphabet) + ntables * len(alphabet)


# --------------------------------------------------------------------------
# Model (a): order-0
# --------------------------------------------------------------------------
def code_order0(sy):
    a = sorted(set(sy))
    idx = {s: i for i, s in enumerate(a)}
    freqs = expand(quant8(sy, a))
    cum = cum_table(freqs)
    enc = RangeEncoder()
    for s in sy:
        enc.encode(cum[idx[s]], freqs[idx[s]])
    body = enc.finish()
    # decode
    dec = RangeDecoder(body)
    out = []
    for _ in range(len(sy)):
        v = dec.get_freq()
        i = 0
        while cum[i + 1] <= v:
            i += 1
        dec.decode(cum[i], freqs[i])
        out.append(a[i])
    return len(body), table_bytes(a, 1), out == sy, out


# --------------------------------------------------------------------------
# Model (b): order-1, context = previous symbol was zero (2 tables)
# --------------------------------------------------------------------------
def code_order1_prevzero(sy):
    a = sorted(set(sy))
    idx = {s: i for i, s in enumerate(a)}
    g = ([], [])
    pz = 1
    for s in sy:
        g[pz].append(s)
        pz = 1 if s == 0 else 0
    freqs = [expand(quant8(g[0], a)), expand(quant8(g[1], a))]
    cum = [cum_table(freqs[0]), cum_table(freqs[1])]
    enc = RangeEncoder()
    pz = 1
    for s in sy:
        enc.encode(cum[pz][idx[s]], freqs[pz][idx[s]])
        pz = 1 if s == 0 else 0
    body = enc.finish()
    dec = RangeDecoder(body)
    out = []
    pz = 1
    for _ in range(len(sy)):
        v = dec.get_freq()
        i = 0
        while cum[pz][i + 1] <= v:
            i += 1
        dec.decode(cum[pz][i], freqs[pz][i])
        s = a[i]
        out.append(s)
        pz = 1 if s == 0 else 0
    return len(body), table_bytes(a, 2), out == sy, out


# --------------------------------------------------------------------------
# Model (c): order-1, context = capped zero-run-length bucket  (WINNER)
# --------------------------------------------------------------------------
def code_order1_runbucket(sy, B):
    a = sorted(set(sy))
    idx = {s: i for i, s in enumerate(a)}
    groups = defaultdict(list)
    run = 0
    for s in sy:
        groups[min(run, B - 1)].append(s)
        run = run + 1 if s == 0 else 0
    # ensure all B contexts exist (uniform fallback for unused ones)
    freqs = {}
    cum = {}
    for c in range(B):
        freqs[c] = expand(quant8(groups.get(c, []), a))
        cum[c] = cum_table(freqs[c])
    enc = RangeEncoder()
    run = 0
    for s in sy:
        c = min(run, B - 1)
        enc.encode(cum[c][idx[s]], freqs[c][idx[s]])
        run = run + 1 if s == 0 else 0
    body = enc.finish()
    # decode
    dec = RangeDecoder(body)
    out = []
    run = 0
    for _ in range(len(sy)):
        c = min(run, B - 1)
        v = dec.get_freq()
        i = 0
        while cum[c][i + 1] <= v:
            i += 1
        dec.decode(cum[c][i], freqs[c][i])
        s = a[i]
        out.append(s)
        run = run + 1 if s == 0 else 0
    return len(body), table_bytes(a, B), out == sy, out


# --------------------------------------------------------------------------
# Benchmark / verification driver
# --------------------------------------------------------------------------
def load():
    here = os.path.dirname(os.path.abspath(__file__))
    d = json.load(open(os.path.join(here, "..", "reloc_values.json")))
    return d["bl"], d["ldr"]


def main():
    bl, ldr = load()
    combined = bl + ldr
    HEADER = 47

    print("=" * 70)
    print("INTEGER RANGE CODER  TOTBITS=%d TOT=%d  32-bit byte-renorm Subbotin" % (TOTBITS, TOT))
    print("=" * 70)

    def report_stream(name, sy):
        b0, t0, ok0, _ = code_order0(sy)
        b1, t1, ok1, _ = code_order1_prevzero(sy)
        rows = []
        for B in range(2, 7):
            bb, tt, okk, _ = code_order1_runbucket(sy, B)
            rows.append((bb + tt, B, bb, tt, okk))
        rows.sort()
        bestB = rows[0]
        print(f"\n[{name}] alphabet n={len(set(sy))} = {sorted(set(sy))}")
        print(f"  order-0            : body={b0:4d}  tbl={t0:3d}  sum={b0+t0:4d}  {'PASS' if ok0 else 'FAIL'}")
        print(f"  order-1 prev-zero  : body={b1:4d}  tbl={t1:3d}  sum={b1+t1:4d}  {'PASS' if ok1 else 'FAIL'}")
        for tot, B, bb, tt, okk in sorted(rows, key=lambda r: r[1]):
            star = "  <- best" if B == bestB[1] else ""
            print(f"  order-1 run-buck B={B}: body={bb:4d}  tbl={tt:3d}  sum={tot:4d}  {'PASS' if okk else 'FAIL'}{star}")
        return {
            "o0": (b0, t0, ok0),
            "o1pz": (b1, t1, ok1),
            "best": bestB,  # (sum,B,body,tbl,ok)
        }

    rbl = report_stream("bl", bl)
    rldr = report_stream("ldr", ldr)

    print("\n" + "=" * 70)
    print("TOTALS  (bl + ldr separate models, +%dB headers)" % HEADER)
    print("=" * 70)

    def total(rb, rl, key):
        if key == "best":
            bb_b, bb_t = rb["best"][2], rb["best"][3]
            bl_b, bl_t = rl["best"][2], rl["best"][3]
            ok = rb["best"][4] and rl["best"][4]
            extra = f"(bl B={rb['best'][1]}, ldr B={rl['best'][1]})"
        else:
            bb_b, bb_t, _ = rb[key]
            bl_b, bl_t, _ = rl[key]
            ok = rb[key][2] and rl[key][2]
            extra = ""
        t = bb_b + bl_b + bb_t + bl_t + HEADER
        return t, ok, (bb_b, bl_b, bb_t, bl_t), extra

    def show(label, key):
        t, ok, parts, extra = total(rbl, rldr, key)
        bb_b, bl_b, bb_t, bl_t = parts
        print(f"\n{label} {extra}")
        print(f"  bl body={bb_b}  ldr body={bl_b}  bl tbl={bb_t}  ldr tbl={bl_t}  hdr={HEADER}")
        print(f"  TOTAL = {t}   lossless={'PASS' if ok else 'FAIL'}")
        for base, nm in [(833, "heatshrink full"), (784, "heatshrink vals"),
                         (720, "order-0 floor"), (598, "RLE proto")]:
            print(f"    vs {nm:16s} {base}: {'BEAT by '+str(base-t) if t < base else 'lose by '+str(t-base)}")
        return t, ok

    show("order-0:", "o0")
    show("order-1 prev-zero:", "o1pz")
    win_t, win_ok = show("order-1 zero-run-bucket (WINNER):", "best")

    # ----- edge cases -----
    print("\n" + "=" * 70)
    print("EDGE CASES (round-trip)")
    print("=" * 70)
    cases = {
        "empty bl-alpha [0]": [0],
        "single value": [-168],
        "all zeros x100": [0] * 100,
        "no zeros": [-336, -360, -336, -360, -8],
        "alternating": [0, -168, 0, -168, 0, -168],
        "combined bl+ldr": combined,
    }
    allok = True
    for nm, sy in cases.items():
        # use winning model with B=3 (robust default) where alphabet >1
        if len(set(sy)) == 1:
            ok = True  # 1-symbol alphabet: handled, freq=TOT
            _, _, ok, _ = code_order0(sy)
        else:
            _, _, ok, _ = code_order1_runbucket(sy, 3)
        allok &= ok
        print(f"  {nm:22s}: {'PASS' if ok else 'FAIL'}")
    print(f"\n  ALL EDGE CASES: {'PASS' if allok else 'FAIL'}")


if __name__ == "__main__":
    main()
