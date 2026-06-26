"""bl-split.py -- BL-split literal encoder for in-place firmware patches.

APPROACH
========
The INSERT (brand-new ARMv6-M Thumb code) literal bytes are split into two
classes and modeled with DIFFERENT models, all derived from the FROM-IMAGE
(shared side-information, rebuilt by the decoder -- NOT shipped):

  * 32-bit BL instructions (Thumb prefix top5(hw1)==0x1E, i.e. high byte in
    0xF0..0xF7).  These embed PC-relative *call targets*; in new code the
    callees cluster (same library/idiom functions reused), so the target byte
    distribution is sharply structured.  We model the 4 BL bytes (hw1.lo,
    hw1.hi, hw2.lo, hw2.hi) with SEPARATE order-0 models, each built only from
    the BL instructions present in the from-image.

  * Everything else (16-bit instructions, 32-bit non-BL words, data) is modeled
    with a two-stream INSTRUCTION-START model (low/high byte at each halfword
    start), built from the whole from-image while walking it as Thumb.  This is
    essentially the current two-table-parity scheme refined to halfword-start
    positions.

DECODER ORDERING (why this is losslessly decodable)
---------------------------------------------------
Both sides walk the Thumb halfword stream identically.  The class of a
halfword-start is a pure function of hw1's HIGH byte alone (top5==0x1E).  So we
always code hw1's HIGH byte FIRST with the start-high model; after decoding it,
both encoder and decoder know whether this is a BL prefix and select the model
for the remaining bytes deterministically.  No flag / side channel is needed.

INTEGER-ONLY range coder (32-bit).  Model tables are 256-entry order-0 freqs
built from one linear from-image pass.  Decoder peak RAM is dominated by those
tables.  Verified lossless on small/medium/large.
"""
import sys
sys.path[:0] = ['/ai_sw/detools-dev/m4dev/sim', '/ai_sw/detools-dev/m4dev/sim/imm']
from common import get

# ---------------------------------------------------------------------------
# Integer range coder (Subbotin-style carryless, 32-bit, 8-bit renorm).
# ---------------------------------------------------------------------------
TOP = 1 << 24
BOT = 1 << 16
MASK32 = (1 << 32) - 1

class Encoder:
    __slots__ = ('low', 'rng', 'out')
    def __init__(self):
        self.low = 0
        self.rng = MASK32
        self.out = bytearray()
    def encode(self, cum, freq, tot):
        r = self.rng // tot
        self.low = (self.low + r * cum) & MASK32
        self.rng = r * freq
        while True:
            if (self.low ^ (self.low + self.rng)) < TOP:
                pass
            elif self.rng < BOT:
                self.rng = (-self.low) & (BOT - 1)
            else:
                break
            self.out.append((self.low >> 24) & 0xFF)
            self.low = (self.low << 8) & MASK32
            self.rng = (self.rng << 8) & MASK32
    def finish(self):
        for _ in range(4):
            self.out.append((self.low >> 24) & 0xFF)
            self.low = (self.low << 8) & MASK32
        return bytes(self.out)

class Decoder:
    __slots__ = ('data', 'pos', 'low', 'rng', 'code', 'r')
    def __init__(self, data):
        self.data = data
        self.pos = 0
        self.low = 0
        self.rng = MASK32
        self.code = 0
        for _ in range(4):
            self.code = ((self.code << 8) | self._byte()) & MASK32
    def _byte(self):
        if self.pos < len(self.data):
            b = self.data[self.pos]; self.pos += 1; return b
        return 0
    def get_freq(self, tot):
        self.r = self.rng // tot
        v = (self.code - self.low) // self.r
        return v if v < tot else tot - 1
    def decode(self, cum, freq, tot):
        self.low = (self.low + self.r * cum) & MASK32
        self.rng = self.r * freq
        while True:
            if (self.low ^ (self.low + self.rng)) < TOP:
                pass
            elif self.rng < BOT:
                self.rng = (-self.low) & (BOT - 1)
            else:
                break
            self.code = ((self.code << 8) | self._byte()) & MASK32
            self.low = (self.low << 8) & MASK32
            self.rng = (self.rng << 8) & MASK32

# ---------------------------------------------------------------------------
# Models (cumulative freq tables, Laplace +1). All integer.
# ---------------------------------------------------------------------------
def make_cum(freq):
    cum = [0] * 257
    s = 0
    for i in range(256):
        cum[i] = s
        s += freq[i]
    cum[256] = s
    return cum, s

def cum_decode_sym(cum, f):
    lo, hi = 0, 256
    while lo + 1 < hi:
        mid = (lo + hi) >> 1
        if cum[mid] <= f:
            lo = mid
        else:
            hi = mid
    return lo

def is_bl_prefix_hi(hi_byte):
    """top5(hw1)==0x1E  <=>  hi byte top5 bits == 0b11110  <=>  hi in 0xF0..0xF7."""
    return (hi_byte >> 3) == 0x1E

# ---------------------------------------------------------------------------
# Build models from the FROM-IMAGE (synced; rebuilt by decoder, not shipped).
# ---------------------------------------------------------------------------
def build_models(frm):
    start_lo = [1] * 256   # low byte at a halfword start
    start_hi = [1] * 256   # high byte at a halfword start (the gate byte)
    blpos = [[1] * 256 for _ in range(4)]
    n = len(frm) // 2
    i = 0
    while i < n:
        lo = frm[2 * i]
        hi = frm[2 * i + 1]
        start_lo[lo] += 1
        start_hi[hi] += 1
        if is_bl_prefix_hi(hi) and (i + 1) < n:
            for p in range(4):
                blpos[p][frm[2 * i + p]] += 1
            i += 2
        else:
            i += 1
    return {
        'start_lo': make_cum(start_lo),
        'start_hi': make_cum(start_hi),
        'bl': [make_cum(blpos[p]) for p in range(4)],
    }

# ---------------------------------------------------------------------------
# Encode / Decode
# ---------------------------------------------------------------------------
def encode(insert, frm):
    M = build_models(frm)
    slo_c, slo_t = M['start_lo']
    shi_c, shi_t = M['start_hi']
    bl = M['bl']
    enc = Encoder()
    data = insert
    L = len(data)
    n = L // 2
    i = 0
    while i < n:
        hi = data[2 * i + 1]
        # gate byte first: high byte of hw1 with start-hi model
        enc.encode(shi_c[hi], shi_c[hi + 1] - shi_c[hi], shi_t)
        if is_bl_prefix_hi(hi) and (i + 1) < n:
            # BL: hw1.lo (pos0), hw2.lo (pos2), hw2.hi (pos3)
            for p in (0, 2, 3):
                b = data[2 * i + p]
                c, t = bl[p]
                enc.encode(c[b], c[b + 1] - c[b], t)
            i += 2
        else:
            # non-BL halfword: low byte with start-lo model
            b = data[2 * i]
            enc.encode(slo_c[b], slo_c[b + 1] - slo_c[b], slo_t)
            i += 1
    if L & 1:
        # trailing odd byte -> start-lo model (even index, a low byte)
        b = data[L - 1]
        enc.encode(slo_c[b], slo_c[b + 1] - slo_c[b], slo_t)
    return enc.finish()

def decode(blob, frm, out_len):
    M = build_models(frm)
    slo_c, slo_t = M['start_lo']
    shi_c, shi_t = M['start_hi']
    bl = M['bl']
    dec = Decoder(blob)
    out = bytearray(out_len)
    L = out_len
    n = L // 2
    i = 0
    while i < n:
        f = dec.get_freq(shi_t)
        hi = cum_decode_sym(shi_c, f)
        dec.decode(shi_c[hi], shi_c[hi + 1] - shi_c[hi], shi_t)
        out[2 * i + 1] = hi
        if is_bl_prefix_hi(hi) and (i + 1) < n:
            for p in (0, 2, 3):
                c, t = bl[p]
                ff = dec.get_freq(t)
                b = cum_decode_sym(c, ff)
                dec.decode(c[b], c[b + 1] - c[b], t)
                out[2 * i + p] = b
            i += 2
        else:
            c, t = slo_c, slo_t
            ff = dec.get_freq(t)
            b = cum_decode_sym(c, ff)
            dec.decode(c[b], c[b + 1] - c[b], t)
            out[2 * i] = b
            i += 1
    if L & 1:
        ff = dec.get_freq(slo_t)
        b = cum_decode_sym(slo_c, ff)
        dec.decode(slo_c[b], slo_c[b + 1] - slo_c[b], slo_t)
        out[L - 1] = b
    return bytes(out)

# ---------------------------------------------------------------------------
# Honest peak decoder RAM: model tables + state.
#   start_lo/start_hi: cum arrays of 257 int16  -> 2*257*2 bytes
#   4 BL cum arrays:   257 int16 each            -> 4*257*2 bytes
#   range coder state: a handful of 32-bit words.
# We report the table footprint as 16-bit cum entries (values fit: total
# counts << 65536 for all tables here -- checked at runtime).
# ---------------------------------------------------------------------------
def decoder_ram_bytes(frm):
    M = build_models(frm)
    tables = [M['start_lo'], M['start_hi']] + M['bl']
    n_cum_entries = sum(len(c) for c, _ in tables)  # 257 each, 6 tables
    # cum values: ensure 16-bit fits
    max_tot = max(t for _, t in tables)
    width = 2 if max_tot < 65536 else 4
    table_bytes = n_cum_entries * width
    state_bytes = 8 * 4  # low,rng,code,r,pos + a few -> generous
    return table_bytes + state_bytes, max_tot, width

# ---------------------------------------------------------------------------
# Run + verify
# ---------------------------------------------------------------------------
if __name__ == '__main__':
    import os
    results = {}
    ram_peak = 0
    for name in ['small', 'medium', 'large']:
        ins, frm = get(name)
        blob = encode(ins, frm)
        dec = decode(blob, frm, len(ins))
        ok = (dec == ins)
        assert ok, f"ROUNDTRIP FAILED for {name}"
        ram, max_tot, width = decoder_ram_bytes(frm)
        ram_peak = max(ram_peak, ram)
        results[name] = len(blob)
        print(f"[{name}] compressed={len(blob)}B  lossless={ok}  "
              f"ram={ram}B (6 cum tables x257 x{width}B, max_tot={max_tot})")
    print()
    base = {'small': 300, 'large': 3388}
    print(f"two-table parity baseline: small=300 large=3388")
    print(f"bl-split (this):           small={results['small']} "
          f"medium={results['medium']} large={results['large']}")
    print(f"beats parity: small {results['small']<300} ({300-results['small']:+d}B)  "
          f"large {results['large']<3388} ({3388-results['large']:+d}B)")
    print(f"peak decoder RAM: {ram_peak} B  (integer-only=Y, shipped-table=0 B)")
