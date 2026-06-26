"""Whole-from-image LZ dictionary + context-modeled arithmetic-coded literals.

The INSERT (new Thumb code) is encoded as a token stream through ONE integer range
coder (Subbotin carryless, 32-bit):

  - literal byte : entropy-coded with a small CONTEXT model SEEDED from the from-image.
      context = (position_parity, top-2-bits-of-previous-byte)  -> 8 contexts.
      Thumb instructions are 16-bit halfwords, so byte position parity (low/high
      halfword byte) plus a coarse previous-byte class captures most of the order-1
      structure while keeping the model tiny.
  - copy(offset,len) : references the ENTIRE from-image as a flash-resident LZ
      dictionary. Emitted only when the bytes saved out-earn the offset+length cost.

The from-image is SHARED side information: the decoder rebuilds the identical 8x256
histogram by scanning flash once. Nothing model-related is shipped; only 3 integer
constants (offset-bit width derived from from-image length, MIN_MATCH, LEN_FIELD)
are baked into both sides.

Decoder peak RAM (this stage), honest:
  - literal freq table  : 8 ctx * 256 * uint16 = 4096 B   (built from one flash scan)
  - context totals       : 8 * uint32           = 32 B
  - flag model           : 2 * uint16           = 4 B
  - range coder state    : ~24 B
  - misc                 : ~16 B
  Peak ~= 4.1 KiB. (cum sums are computed on the fly from freq, not stored.)
  Dictionary = from-image, in flash (random access), NOT counted in RAM.

  NOTE: 4096 B slightly exceeds a strict 4096 budget once state is added. A 4-context
  variant (parity + top-1-bit prev) halves the table to 2 KiB at a small size cost;
  see CTX_BITS. Default uses 8 contexts (best size, ~4.1 KiB).

Integer-only: YES. Model derived from from-image (one pass). Shipped table: 0 bytes.
"""
import sys, math
sys.path[:0] = ['/ai_sw/detools-dev/m4dev/sim/imm']
import common
from collections import defaultdict

# ---------------------------------------------------------------- range coder
TOP = 1 << 24
BOT = 1 << 16
MASK = 0xFFFFFFFF

class Encoder:
    def __init__(self):
        self.low = 0; self.rng = MASK; self.out = bytearray()
    def encode(self, cum, freq, tot):
        r = self.rng // tot
        self.low = (self.low + r * cum) & MASK
        self.rng = r * freq
        while True:
            if (self.low ^ (self.low + self.rng)) < TOP: pass
            elif self.rng < BOT: self.rng = (-self.low) & (BOT - 1)
            else: break
            self.out.append((self.low >> 24) & 0xFF)
            self.low = (self.low << 8) & MASK
            self.rng = (self.rng << 8) & MASK
    def flush(self):
        for _ in range(4):
            self.out.append((self.low >> 24) & 0xFF)
            self.low = (self.low << 8) & MASK
        return bytes(self.out)

class Decoder:
    def __init__(self, data):
        self.data = data; self.pos = 0; self.low = 0; self.rng = MASK; self.code = 0
        for _ in range(4):
            self.code = ((self.code << 8) | self._byte()) & MASK
    def _byte(self):
        if self.pos < len(self.data):
            b = self.data[self.pos]; self.pos += 1; return b
        return 0
    def get_freq(self, tot):
        self.r = self.rng // tot
        return min(tot - 1, ((self.code - self.low) & MASK) // self.r)
    def decode(self, cum, freq, tot):
        r = self.r
        self.low = (self.low + r * cum) & MASK
        self.rng = r * freq
        while True:
            if (self.low ^ (self.low + self.rng)) < TOP: pass
            elif self.rng < BOT: self.rng = (-self.low) & (BOT - 1)
            else: break
            self.code = ((self.code << 8) | self._byte()) & MASK
            self.low = (self.low << 8) & MASK
            self.rng = (self.rng << 8) & MASK

# ---------------------------------------------------------------- flag model
class Flag:
    __slots__ = ('c0', 'c1')
    def __init__(self): self.c0 = 1; self.c1 = 1
    def enc(self, e, bit):
        tot = self.c0 + self.c1
        if bit: e.encode(self.c0, self.c1, tot)
        else:   e.encode(0, self.c0, tot)
        self._upd(bit)
    def dec(self, d):
        tot = self.c0 + self.c1
        f = d.get_freq(tot)
        if f < self.c0: d.decode(0, self.c0, tot); bit = 0
        else:           d.decode(self.c0, self.c1, tot); bit = 1
        self._upd(bit); return bit
    def _upd(self, bit):
        if bit: self.c1 += 32
        else:   self.c0 += 32
        if self.c0 + self.c1 > 1 << 14:
            self.c0 = (self.c0 + 1) >> 1; self.c1 = (self.c1 + 1) >> 1

# bypass bits
def put_bit(e, bit): e.encode(bit, 1, 2)
def get_bit(d):
    bit = 1 if d.get_freq(2) >= 1 else 0
    d.decode(bit, 1, 2); return bit
def put_bits(e, val, n):
    for i in range(n - 1, -1, -1): put_bit(e, (val >> i) & 1)
def get_bits(d, n):
    v = 0
    for _ in range(n): v = (v << 1) | get_bit(d)
    return v

# ---------------------------------------------------------------- literal ctx model
# context = (position_parity << CTX_PREV_BITS) | (prev_byte >> (8-CTX_PREV_BITS))
CTX_PREV_BITS = 2          # top-2 bits of previous byte
NCTX = (1 << CTX_PREV_BITS) * 2   # *2 for position parity  -> 8

def _ctx(parity, prev):
    return (parity << CTX_PREV_BITS) | (prev >> (8 - CTX_PREV_BITS))

def build_lit_model(frm):
    freq = [[1] * 256 for _ in range(NCTX)]
    prev = 0
    for pos, b in enumerate(frm):
        c = _ctx(pos & 1, prev)
        freq[c][b] += 1
        prev = b
    tot = [sum(f) for f in freq]
    # cap totals below BOT for range-coder safety
    for c in range(NCTX):
        while tot[c] >= BOT:
            freq[c] = [(x + 1) >> 1 for x in freq[c]]
            tot[c] = sum(freq[c])
    return freq, tot

def lit_encode(e, freq, tot, c, b):
    f = freq[c]
    cum = 0
    for i in range(b): cum += f[i]
    e.encode(cum, f[b], tot[c])
def lit_decode(d, freq, tot, c):
    f = freq[c]
    target = d.get_freq(tot[c])
    cum = 0; b = 0
    while cum + f[b] <= target:
        cum += f[b]; b += 1
    d.decode(cum, f[b], tot[c])
    return b

# ---------------------------------------------------------------- LZ
MIN_MATCH = 4
LEN_FIELD = 12

def offset_bits(frm):
    return max(1, (len(frm) - 1).bit_length())

def parse(ins, frm, obits, rate):
    K = MIN_MATCH
    idx = defaultdict(list)
    for i in range(len(frm) - K + 1):
        idx[bytes(frm[i:i + K])].append(i)
    tokens = []; i = 0; n = len(ins)
    copy_overhead = 0.2 + obits + LEN_FIELD
    cap = (1 << LEN_FIELD) - 1 + K
    while i < n:
        best_len = 0; best_off = 0
        if i + K <= n:
            key = bytes(ins[i:i + K])
            for p in idx.get(key, ())[:256]:
                l = K
                maxl = min(n - i, cap, len(frm) - p)
                while l < maxl and frm[p + l] == ins[i + l]: l += 1
                if l > best_len: best_len = l; best_off = p
        if best_len >= K and best_len * rate > copy_overhead:
            tokens.append(('copy', best_off, best_len)); i += best_len
        else:
            tokens.append(('lit', ins[i])); i += 1
    return tokens

def _avg_rate(freq, tot):
    # average bits/byte over the from-image's own distribution (rough literal cost)
    tb = sum(tot); s = 0.0
    for c in range(NCTX):
        for x in freq[c]:
            if x: s += -x * math.log2(x / tot[c])
    return s / tb

def encode(ins, frm):
    obits = offset_bits(frm)
    freq, tot = build_lit_model(frm)
    rate = _avg_rate(freq, tot)
    tokens = parse(ins, frm, obits, rate)
    e = Encoder(); flag = Flag()
    prev = 0; pos = 0
    for tk in tokens:
        if tk[0] == 'lit':
            flag.enc(e, 0)
            c = _ctx(pos & 1, prev)
            b = tk[1]
            lit_encode(e, freq, tot, c, b)
            prev = b; pos += 1
        else:
            _, off, length = tk
            flag.enc(e, 1)
            put_bits(e, off, obits)
            put_bits(e, length - MIN_MATCH, LEN_FIELD)
            # advance context state across the copied span
            for k in range(length):
                prev = frm[off + k]; pos += 1
    return e.flush()

def decode(data, frm, out_len):
    obits = offset_bits(frm)
    freq, tot = build_lit_model(frm)
    d = Decoder(data); flag = Flag()
    out = bytearray(); prev = 0; pos = 0
    while len(out) < out_len:
        if flag.dec(d) == 0:
            c = _ctx(pos & 1, prev)
            b = lit_decode(d, freq, tot, c)
            out.append(b); prev = b; pos += 1
        else:
            off = get_bits(d, obits)
            length = get_bits(d, LEN_FIELD) + MIN_MATCH
            for k in range(length):
                b = frm[off + k]; out.append(b); prev = b; pos += 1
    return bytes(out)

if __name__ == '__main__':
    results = {}
    for name in ['small', 'medium', 'large']:
        ins, frm = common.get(name)
        data = encode(ins, frm)
        dec = decode(data, frm, len(ins))
        assert dec == ins, f"ROUNDTRIP FAIL {name}"
        results[name] = len(data)
        print(f"{name}: insert={len(ins)} -> {len(data)} bytes (lossless OK)")
    ram = NCTX * 256 * 2 + NCTX * 4 + 4 + 24 + 16
    print(f"decoder peak RAM (this stage) = {ram} B  (NCTX={NCTX})")
    print("results:", results, " baseline parity small/large = 300/3388")
