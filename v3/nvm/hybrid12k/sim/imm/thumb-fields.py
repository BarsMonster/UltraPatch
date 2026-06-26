"""Thumb-instruction-field model for encoding new-code INSERT literals.

Idea
----
The INSERT is brand-new ARMv6-M Thumb code, stored as 16-bit little-endian
halfwords (occasionally a 32-bit BL = two halfwords, handled transparently as
two 16-bit halfwords).  In Thumb the *top* bits of a halfword select the
opcode class, and the operand/register/immediate fields in the low bits have a
strongly skewed, learnable distribution that is highly correlated with the
opcode class.

We exploit the *from-image* (pre-patch firmware, ~113 KB of existing Thumb
code) as SHARED side-information: both encoder and decoder scan it once and
derive identical frequency models.  Nothing model-related is transmitted.

Model (per halfword h):
  hi = h >> 8                       high byte, modelled order-0 (256-bin table)
  lo = h & 0xFF                     low byte, modelled CONDITIONED on the
                                    opcode-class context  ctx = h >> (16-CTXBITS)
                                    (top CTXBITS bits of the halfword).

This captures "opcode class -> operand-byte distribution" cheaply.  Counts are
gathered from the from-image, rescaled to fit uint8 bins (MCU-friendly), and
used by an integer range coder.  Laplace +1 smoothing guarantees every symbol
is codeable (lossless for any insert, even bytes never seen in the from-image).

All arithmetic is integer-only.  Nothing but a few header bytes (the insert
byte-length) is shipped; the entire probability model is rebuilt by the decoder
from the from-image.

RAM (decoder, honest peak):
  hi table   : 256 * uint16  = 512 B  (counts) + 257 * uint16 cum = 514 B built lazily? -> we keep counts, build cum per use
  lo tables  : (1<<CTXBITS) * 256 * uint8
  With CTXBITS=4 -> 16*256 = 4096 B.  Total model ~4.6 KB.
  With CTXBITS=3 -> 8*256  = 2048 B.  Total model ~2.6 KB  (chosen default).
"""
import sys, os
sys.path[:0] = [os.path.dirname(__file__)]
import common

CTXBITS = 3                      # low-byte conditioning context width (top bits)
NCTX = 1 << CTXBITS
CTXSHIFT = 16 - CTXBITS
CAP = 255                        # uint8 count cap (MCU friendliness)

# ---------------------------------------------------------------------------
# Integer range coder (32-bit, byte-wise renormalisation, carryless Schindler)
# ---------------------------------------------------------------------------
TOP = 1 << 24
BOT = 1 << 16

class Encoder:
    def __init__(self):
        self.low = 0
        self.range = 0xFFFFFFFF
        self.out = bytearray()
    def encode(self, cum, freq, tot):
        # narrow range to the [cum, cum+freq) slice of tot
        r = self.range // tot
        self.low = (self.low + r * cum) & 0xFFFFFFFF
        self.range = r * freq
        # renormalise
        while True:
            if (self.low ^ (self.low + self.range)) < TOP:
                pass
            elif self.range < BOT:
                self.range = (-self.low) & (BOT - 1)
            else:
                break
            self.out.append((self.low >> 24) & 0xFF)
            self.low = (self.low << 8) & 0xFFFFFFFF
            self.range = (self.range << 8) & 0xFFFFFFFF
    def finish(self):
        for _ in range(4):
            self.out.append((self.low >> 24) & 0xFF)
            self.low = (self.low << 8) & 0xFFFFFFFF
        return bytes(self.out)

class Decoder:
    def __init__(self, data):
        self.data = data
        self.pos = 0
        self.low = 0
        self.range = 0xFFFFFFFF
        self.code = 0
        for _ in range(4):
            self.code = ((self.code << 8) | self._byte()) & 0xFFFFFFFF
    def _byte(self):
        if self.pos < len(self.data):
            b = self.data[self.pos]; self.pos += 1; return b
        return 0
    def getfreq(self, tot):
        r = self.range // tot
        v = ((self.code - self.low) & 0xFFFFFFFF) // r
        return tot - 1 if v >= tot else v
    def decode(self, cum, freq, tot):
        r = self.range // tot
        self.low = (self.low + r * cum) & 0xFFFFFFFF
        self.range = r * freq
        while True:
            if (self.low ^ (self.low + self.range)) < TOP:
                pass
            elif self.range < BOT:
                self.range = (-self.low) & (BOT - 1)
            else:
                break
            self.code = ((self.code << 8) | self._byte()) & 0xFFFFFFFF
            self.low = (self.low << 8) & 0xFFFFFFFF
            self.range = (self.range << 8) & 0xFFFFFFFF

# ---------------------------------------------------------------------------
# Model build (identical on encoder and decoder; one from-image pass)
# ---------------------------------------------------------------------------
def build_model(frm):
    """Return (hi_counts[256], lo_counts[NCTX][256]) as plain lists of small
    ints.  Counts are rescaled per-distribution so the max bin == CAP, which is
    what an MCU would store in uint8 arrays.  +1 Laplace smoothing is applied at
    coding time (not stored)."""
    hi = [0] * 256
    lo = [[0] * 256 for _ in range(NCTX)]
    n = len(frm) - 1
    i = 0
    while i < n:
        h = frm[i] | (frm[i + 1] << 8)
        i += 2
        hi[h >> 8] += 1
        lo[h >> CTXSHIFT][h & 0xFF] += 1
    def rescale(arr):
        mx = max(arr)
        if mx > CAP:
            for k in range(len(arr)):
                if arr[k]:
                    arr[k] = max(1, (arr[k] * CAP) // mx)
    rescale(hi)
    for ctx in lo:
        rescale(ctx)
    return hi, lo

def _cum(counts, sym):
    """cumulative-before, freq, total over a 256-bin distribution with +1
    smoothing (so total = sum(counts)+256, every symbol freq>=1)."""
    c = 0
    for k in range(sym):
        c += counts[k] + 1
    freq = counts[sym] + 1
    tot = sum(counts) + 256
    return c, freq, tot

# ---------------------------------------------------------------------------
# Public encode / decode
# ---------------------------------------------------------------------------
def encode(insert, frm):
    assert len(insert) % 2 == 0, "insert must be halfword-aligned"
    hi, lo = build_model(frm)
    enc = Encoder()
    n = len(insert)
    i = 0
    while i < n:
        h = insert[i] | (insert[i + 1] << 8)
        i += 2
        hb = h >> 8
        lb = h & 0xFF
        ctx = h >> CTXSHIFT
        c, f, t = _cum(hi, hb)
        enc.encode(c, f, t)
        c, f, t = _cum(lo[ctx], lb)
        enc.encode(c, f, t)
    body = enc.finish()
    # header: 2-byte little-endian halfword count
    nh = n // 2
    header = bytes([nh & 0xFF, (nh >> 8) & 0xFF])
    return header + body

def _find_sym(counts, target, tot):
    """given a target cumulative value in [0,tot), find the symbol whose
    +1-smoothed slice contains it; return (sym, cum_before, freq)."""
    c = 0
    for sym in range(256):
        f = counts[sym] + 1
        if target < c + f:
            return sym, c, f
        c += f
    return 255, c - (counts[255] + 1), counts[255] + 1

def decode(data, frm):
    nh = data[0] | (data[1] << 8)
    body = data[2:]
    hi, lo = build_model(frm)
    hi_tot = sum(hi) + 256
    lo_tot = [sum(c) + 256 for c in lo]
    dec = Decoder(body)
    out = bytearray()
    for _ in range(nh):
        # high byte
        target = dec.getfreq(hi_tot)
        hb, c, f = _find_sym(hi, target, hi_tot)
        dec.decode(c, f, hi_tot)
        # low byte conditioned on context = top CTXBITS bits of halfword.
        # ctx top bits come from hb's top (CTXBITS) bits since CTXSHIFT>=8.
        ctx = hb >> (CTXSHIFT - 8)
        t = lo_tot[ctx]
        target = dec.getfreq(t)
        lb, c, f = _find_sym(lo[ctx], target, t)
        dec.decode(c, f, t)
        out.append(lb)
        out.append(hb)
    return bytes(out)

# ---------------------------------------------------------------------------
# Self-test / measurement
# ---------------------------------------------------------------------------
if __name__ == "__main__":
    import tracemalloc
    results = {}
    for name in ['small', 'medium', 'large']:
        ins, frm = common.get(name)
        enc = encode(ins, frm)
        dec = decode(enc, frm)
        assert dec == ins, f"ROUNDTRIP FAIL {name}: {len(dec)} vs {len(ins)}"
        results[name] = len(enc)
        print(f"{name}: insert={len(ins)}B -> compressed={len(enc)}B  (roundtrip OK)")

    # honest peak decoder RAM: model tables + cum totals + coder state
    hi_bytes = 256 * 2           # uint16 counts (max can exceed 255 before rescale; stored uint16 OR uint8 after rescale -> use uint8=256)
    hi_bytes = 256               # rescaled to <=255 -> uint8
    lo_bytes = NCTX * 256        # uint8
    lo_tot_bytes = NCTX * 2      # uint16 per-context totals
    hi_tot_bytes = 2
    coder_state = 4 * 4          # low/range/code/pos ~ 4 words
    ram = hi_bytes + lo_bytes + lo_tot_bytes + hi_tot_bytes + coder_state
    print(f"\nCTXBITS={CTXBITS} NCTX={NCTX}")
    print(f"decoder model+state RAM ~= {ram} B (hi={hi_bytes}, lo={lo_bytes}, totals={lo_tot_bytes+hi_tot_bytes}, state={coder_state})")
    print(f"shipped table bytes = 0 (model rebuilt from from-image); header=2B/stream")
    print(f"sizes_B = {results}")
    print(f"baselines: parity small=300 large=3388 ; halfword_o0 267/3041")
