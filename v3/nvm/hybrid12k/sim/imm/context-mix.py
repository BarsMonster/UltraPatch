"""context-mix.py -- Lightweight INTEGER context-mixing coder for INSERT
(new ARMv6-M Thumb code) literal bytes, using the from-image (pre-patch
firmware) as SHARED side-information known to both encoder and decoder.

Beats the TWO-table parity baseline (small 300 / large 3388) and approaches the
full-16-bit-halfword entropy floor (267 / 3041) while staying MCU-deployable:
integer-only, <=4 KiB resident decoder RAM, model DERIVED from the from-image
(one flash pass), nothing shipped except a 2-byte length prefix.

---------------------------------------------------------------------------
ALGORITHM
---------------------------------------------------------------------------
Bitwise binary arithmetic coder.  For each of the 8 bits of a byte (MSB first)
we mix two from-image context models in the logit (stretch) domain with an
adaptive integer logistic mixer, squash() back to a 12-bit probability, and
code the bit.

  Context models (both built from the from-image, NOT shipped):
    m0  order-0 bit-tree   ctx = partial-bits cb            -> 256 contexts
    m1  order-1 bit-tree   ctx = (prevByte>>4, partial-bits)-> 16*256 contexts

  Each context stores a STATIC quantized "stretched probability" (a logit):
    m0  1 signed byte per context              (256 bytes)
    m1  signed 4-bit, packed 2 per byte        (2048 bytes)
  built by ONE from-image pass that accumulates bit counts then collapses them.
  The wide count arrays are build-time scratch and are freed before coding.

The static models are FROZEN during coding; only the two int32 mixer weights
adapt online.  Freezing is what lets the 346-byte 'small' insert win: there is
no room to *re-learn* a model from 346 bytes, but the 113 KB from-image already
encodes the Thumb statistics, and the cheap mixer just learns how much to trust
each order on this particular insert.

---------------------------------------------------------------------------
INTEGER FIXED-POINT
---------------------------------------------------------------------------
  * logits carried in units of 1/256 natural-log-odds ("d"), clamped to +-2047
  * squash(d) -> 12-bit prob via a 256-entry interpolated table (no float/exp
    at runtime); table is BUILT on the MCU at startup, not shipped
  * mixer weights are 16.16 fixed point; dot product + gradient update are pure
    int32 shifts/mults
  * arithmetic coder is a 32-bit carry-handling (E3) binary range coder

Roundtrip-verified lossless on small/medium/large.   Run: python3 context-mix.py
"""
import sys, math
sys.path.insert(0, '/ai_sw/detools-dev/m4dev/sim/imm')
from common import get

# ---------------------------------------------------------------------------
# squash: 256-entry interpolated table over logit domain [-2048,2048) (units
# 1/256 ln-odds) -> 12-bit probability [1,4095].  Built at startup; ~520 B RAM.
# ---------------------------------------------------------------------------
_SQ_N = 256                       # table entries spanning the clamped range
_SQ_LIM = 2047                    # logits clamped to [-2047, 2047]
_SQ_STEP = (2 * (_SQ_LIM + 1)) // _SQ_N   # = 16 logit-units per table cell
def _build_squash():
    t = [0] * (_SQ_N + 1)
    for i in range(_SQ_N + 1):
        d = -(_SQ_LIM + 1) + i * _SQ_STEP
        p = 1.0 / (1.0 + math.exp(-d / 256.0))
        q = int(round(p * 4096))
        t[i] = 1 if q < 1 else (4095 if q > 4095 else q)
    return t
_SQUASH = _build_squash()         # 257 int16 entries
def squash(d):
    if d < -_SQ_LIM: d = -_SQ_LIM
    if d >  _SQ_LIM: d =  _SQ_LIM
    pos = d + (_SQ_LIM + 1)        # 0 .. 4095
    idx = pos // _SQ_STEP
    frac = pos - idx * _SQ_STEP
    a = _SQUASH[idx]; b = _SQUASH[idx + 1]
    return a + ((b - a) * frac) // _SQ_STEP

# ---------------------------------------------------------------------------
# Model configuration
# ---------------------------------------------------------------------------
PBITS = 4                         # prev-byte resolution kept for order-1
PB_SH = 8 - PBITS                 # = 4  (use high nibble of prev byte)
S0 = 256                          # order-0 contexts
S1 = (1 << PBITS) * 256           # order-1 contexts = 4096
MUL0 = 16                         # order-0 stretch stored *16 -> *16 == logit*256
SC1  = 2                          # order-1 4-bit stretch step (logit*2), clamp +-7
MUL1 = 256 // SC1                 # = 128, so value*128 == logit*256
CLAMP0 = 127
CLAMP1 = 7
W_FIX = 16
LRSH = 10                         # mixer learning-rate shift

# ---------------------------------------------------------------------------
# Build static stretched-prob tables from the from-image (one pass).
#   t0 : bytearray(256)  signed-byte logits (*16)
#   t1 : bytearray(2048) two signed 4-bit logits (*2) packed per byte
# Build scratch = count lists, freed on return.
# ---------------------------------------------------------------------------
def build_model(frm):
    n0_0 = [0] * S0; n0_1 = [0] * S0
    n1_0 = [0] * S1; n1_1 = [0] * S1
    pb = 0
    for byte in frm:
        cb = 1
        for i in range(7, -1, -1):
            bit = (byte >> i) & 1
            k0 = cb & 255
            k1 = ((pb >> PB_SH) << 8) | cb
            if bit:
                n0_1[k0] += 1; n1_1[k1] += 1
            else:
                n0_0[k0] += 1; n1_0[k1] += 1
            cb = (cb << 1) | bit
        pb = byte
    t0 = bytearray(S0)
    for k in range(S0):
        p = (n0_1[k] + 1) / (n0_0[k] + n0_1[k] + 2)
        v = int(round(math.log(p / (1.0 - p)) * 16))
        if v < -CLAMP0: v = -CLAMP0
        if v >  CLAMP0: v =  CLAMP0
        t0[k] = v & 0xFF
    t1 = bytearray(S1 // 2)        # 2 nibbles per byte
    for k in range(S1):
        p = (n1_1[k] + 1) / (n1_0[k] + n1_1[k] + 2)
        v = int(round(math.log(p / (1.0 - p)) * SC1))
        if v < -CLAMP1: v = -CLAMP1
        if v >  CLAMP1: v =  CLAMP1
        nib = v & 0xF                 # signed 4-bit two's complement
        if k & 1:
            t1[k >> 1] = (t1[k >> 1] & 0x0F) | (nib << 4)
        else:
            t1[k >> 1] = (t1[k >> 1] & 0xF0) | nib
    return t0, t1

def _s8(v):  return v - 256 if v >= 128 else v
def _s4(v):  return v - 16 if v >= 8 else v
def _get1(t1, k):
    byte = t1[k >> 1]
    nib = (byte >> 4) if (k & 1) else (byte & 0xF)
    return _s4(nib)

# ---------------------------------------------------------------------------
# Mixer (2 int32 weights, 16.16).  All integer.
# ---------------------------------------------------------------------------
def predict(s0, s1, w):
    dot = (w[0] * (s0 * MUL0) + w[1] * (s1 * MUL1)) >> W_FIX
    return squash(dot)

def update(s0, s1, w, bit, p1):
    err = (bit << 12) - p1
    w[0] += (err * (s0 * MUL0)) >> LRSH
    w[1] += (err * (s1 * MUL1)) >> LRSH

# ---------------------------------------------------------------------------
# 32-bit carry-handling binary arithmetic coder (E3 / Witten-Neal-Cleary).
# p1 = P(bit==1) in 12-bit [1,4095].
# ---------------------------------------------------------------------------
class Encoder:
    def __init__(self):
        self.low = 0; self.high = 0xFFFFFFFF; self.pending = 0
        self.out = bytearray(); self._buf = 0; self._cnt = 0
    def _emit(self, b):
        self._buf = (self._buf << 1) | b; self._cnt += 1
        if self._cnt == 8:
            self.out.append(self._buf & 0xFF); self._buf = 0; self._cnt = 0
    def _emitp(self, b):
        self._emit(b)
        while self.pending:
            self._emit(b ^ 1); self.pending -= 1
    def encode_bit(self, p1, bit):
        rng = self.high - self.low + 1
        mid = self.low + ((rng * (4096 - p1)) >> 12) - 1
        if bit: self.low = mid + 1
        else:   self.high = mid
        while True:
            if (self.high & 0x80000000) == (self.low & 0x80000000):
                self._emitp((self.high >> 31) & 1)
                self.low = (self.low << 1) & 0xFFFFFFFF
                self.high = ((self.high << 1) & 0xFFFFFFFF) | 1
            elif (self.low & 0x40000000) and not (self.high & 0x40000000):
                self.pending += 1
                self.low = (self.low << 1) & 0x7FFFFFFF
                self.high = ((self.high << 1) & 0xFFFFFFFF) | 0x80000001
            else:
                break
    def finish(self):
        self.pending += 1
        self._emitp(1 if (self.low & 0x40000000) else 0)
        while self._cnt != 0: self._emit(0)
        return bytes(self.out)

class Decoder:
    def __init__(self, data):
        self.low = 0; self.high = 0xFFFFFFFF
        self.data = data; self.pos = 0; self._buf = 0; self._cnt = 0
        self.code = 0
        for _ in range(32):
            self.code = ((self.code << 1) | self._rb()) & 0xFFFFFFFF
    def _rb(self):
        if self._cnt == 0:
            self._buf = self.data[self.pos] if self.pos < len(self.data) else 0
            self.pos += 1; self._cnt = 8
        self._cnt -= 1
        return (self._buf >> self._cnt) & 1
    def decode_bit(self, p1):
        rng = self.high - self.low + 1
        mid = self.low + ((rng * (4096 - p1)) >> 12) - 1
        if self.code <= mid:
            bit = 0; self.high = mid
        else:
            bit = 1; self.low = mid + 1
        while True:
            if (self.high & 0x80000000) == (self.low & 0x80000000):
                self.low = (self.low << 1) & 0xFFFFFFFF
                self.high = ((self.high << 1) & 0xFFFFFFFF) | 1
                self.code = ((self.code << 1) & 0xFFFFFFFF) | self._rb()
            elif (self.low & 0x40000000) and not (self.high & 0x40000000):
                self.low = (self.low << 1) & 0x7FFFFFFF
                self.high = ((self.high << 1) & 0xFFFFFFFF) | 0x80000001
                self.code = (((self.code << 1) & 0xFFFFFFFF) ^ 0x80000000) | self._rb()
            else:
                break
        return bit

# ---------------------------------------------------------------------------
# encode / decode (2-byte little-endian length prefix; inserts < 64 KiB)
# ---------------------------------------------------------------------------
def encode(insert, frm):
    t0, t1 = build_model(frm)
    enc = Encoder()
    w = [1 << (W_FIX - 1), 1 << (W_FIX - 1)]
    pb = 0
    for byte in insert:
        cb = 1
        for i in range(7, -1, -1):
            bit = (byte >> i) & 1
            s0 = _s8(t0[cb & 255])
            s1 = _get1(t1, ((pb >> PB_SH) << 8) | cb)
            p1 = predict(s0, s1, w)
            enc.encode_bit(p1, bit)
            update(s0, s1, w, bit, p1)
            cb = (cb << 1) | bit
        pb = byte
    body = enc.finish()
    n = len(insert)
    return bytes([n & 0xFF, (n >> 8) & 0xFF]) + body

def decode(blob, frm):
    n = blob[0] | (blob[1] << 8)
    t0, t1 = build_model(frm)
    dec = Decoder(blob[2:])
    w = [1 << (W_FIX - 1), 1 << (W_FIX - 1)]
    out = bytearray(); pb = 0
    for _ in range(n):
        byte = 0; cb = 1
        for _i in range(8):
            s0 = _s8(t0[cb & 255])
            s1 = _get1(t1, ((pb >> PB_SH) << 8) | cb)
            p1 = predict(s0, s1, w)
            bit = dec.decode_bit(p1)
            update(s0, s1, w, bit, p1)
            byte = (byte << 1) | bit
            cb = (cb << 1) | bit
        out.append(byte); pb = byte
    return bytes(out)

# ---------------------------------------------------------------------------
# Honest RAM accounting
# ---------------------------------------------------------------------------
def ram_report():
    model   = S0 + (S1 // 2)        # 256 + 2048 = 2304 B resident model tables
    squash  = (_SQ_N + 1) * 2       # 514 B int16 squash table (startup-built)
    mixer   = 2 * 4                 # two int32 weights
    coder   = 4 * 4 + 4             # low/high/code/pos + small bit buffers
    state   = 8                     # cb, pb, loop counters, etc.
    resident = model + squash + mixer + coder + state
    # build-time scratch: count arrays for the single from-image pass.
    #   order-0: max count ~76652 over 113 KB -> needs uint32 (2*256*4 = 2048 B)
    #   order-1: max count ~18532          -> uint16 ok    (2*4096*2 = 16384 B)
    # If order-0 and order-1 are accumulated in SEPARATE passes (2 flash scans)
    # the peak is max(order0_scratch, order1_scratch) instead of the sum.
    o0_scratch = 2 * S0 * 4
    o1_scratch = 2 * S1 * 2
    build_scratch_1pass = o0_scratch + o1_scratch          # single pass
    build_scratch_2pass = max(o0_scratch, o1_scratch)      # two passes, lower peak
    return dict(model=model, squash=squash, mixer=mixer, coder=coder,
                state=state, resident=resident,
                build_scratch=build_scratch_1pass,
                build_scratch_2pass=build_scratch_2pass,
                build_peak=resident + build_scratch_1pass,
                build_peak_2pass=resident + build_scratch_2pass)

if __name__ == '__main__':
    sizes = {}; ok_all = True
    for name in ['small', 'medium', 'large']:
        ins, frm = get(name)
        blob = encode(ins, frm)
        dec = decode(blob, frm)
        ok = (dec == ins); ok_all &= ok
        sizes[name] = len(blob)
        print(f"[{name}] insert={len(ins):>5}  compressed={len(blob):>5}  lossless={ok}")
    r = ram_report()
    print("\nRAM (bytes):")
    print(f"  resident model tables : {r['model']}")
    print(f"  squash table (startup): {r['squash']}")
    print(f"  mixer+coder+state     : {r['mixer']+r['coder']+r['state']}")
    print(f"  RESIDENT TOTAL        : {r['resident']}   (<=4096 budget: {r['resident']<=4096})")
    print(f"  build scratch (1-pass): {r['build_scratch']} (freed before coding)")
    print(f"  build PEAK   (1-pass) : {r['build_peak']}")
    print(f"  build scratch (2-pass): {r['build_scratch_2pass']}  build PEAK: {r['build_peak_2pass']}")
    print(f"\nALL LOSSLESS: {ok_all}")
    print(f"sizes: small={sizes['small']} medium={sizes['medium']} large={sizes['large']}")
    print(f"parity to beat: small<300 large<3388  -> "
          f"small {'OK' if sizes['small']<300 else 'NO'}, "
          f"large {'OK' if sizes['large']<3388 else 'NO'}")
