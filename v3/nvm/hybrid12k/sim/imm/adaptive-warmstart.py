"""Adaptive arithmetic (range) coder WARM-STARTED from the from-image histogram.

Idea: the decoder and encoder both scan the from-image (shared side-info) once to
build an order-0 byte histogram. Those counts seed an adaptive model. As the new
code (insert) is coded, counts adapt. No shipped table: only a tiny header (none
needed here, both sides derive identical model from the from-image).

Integer-only binary range coder (carryless, 32-bit) with a frequency model.
We also evaluate an order-1 variant (context = previous byte) warm-started from
the from-image order-1 histogram, RAM-bounded by quantizing context.

All arithmetic is integer-only. Decoder peak RAM is reported honestly including
the model tables it must build & hold.
"""
import sys, os
sys.path[:0] = ['/ai_sw/detools-dev/m4dev/sim/imm']
import common

# ---------------------------------------------------------------------------
# Integer range coder (32-bit, byte-oriented, carry handling via cache/under).
# Classic "Subbotin"-style range coder. Integer-only.
# ---------------------------------------------------------------------------
TOP = 1 << 24
BOT = 1 << 16
MASK32 = (1 << 32) - 1

class REnc:
    def __init__(self):
        self.low = 0
        self.rng = MASK32
        self.out = bytearray()
    def encode(self, cum, freq, tot):
        # narrow range
        self.rng //= tot
        self.low = (self.low + cum * self.rng) & MASK32
        self.rng *= freq
        # renormalize
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

class RDec:
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
        self.rng //= tot
        return (self.code - self.low) // self.rng
    def decode(self, cum, freq, tot):
        self.low = (self.low + cum * self.rng) & MASK32
        self.rng *= freq
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
# Order-0 adaptive model warm-started from from-image histogram.
# Frequencies kept small (scaled) so total stays < BOT for range-coder safety.
# ---------------------------------------------------------------------------
MAXTOT = BOT  # keep total under 2^16

def build_o0(frm, init_scale, floor):
    """Build 256 freq counts from from-image histogram, scaled + floored."""
    hist = [0]*256
    for b in frm:
        hist[b] += 1
    n = len(frm)
    freq = [0]*256
    # scale so sum ~= init_scale, each at least `floor`
    for i in range(256):
        freq[i] = floor + (hist[i]*init_scale)//n
    return freq

class O0Model:
    def __init__(self, freq, inc, limit):
        self.freq = freq[:]
        self.inc = inc
        self.limit = limit
        self.tot = sum(self.freq)
    def cum_of(self, sym):
        c = 0
        f = self.freq
        for i in range(sym):
            c += f[i]
        return c
    def find(self, target):
        c = 0
        f = self.freq
        for i in range(256):
            if c + f[i] > target:
                return i, c
            c += f[i]
        return 255, c  # unreachable
    def update(self, sym):
        self.freq[sym] += self.inc
        self.tot += self.inc
        if self.tot >= self.limit:
            t = 0
            f = self.freq
            for i in range(256):
                f[i] = (f[i] >> 1) | 1
                t += f[i]
            self.tot = t

def enc_o0(insert, freq, inc, limit):
    m = O0Model(freq, inc, limit)
    rc = REnc()
    for b in insert:
        cum = m.cum_of(b)
        rc.encode(cum, m.freq[b], m.tot)
        m.update(b)
    return rc.finish()

def dec_o0(data, n, freq, inc, limit):
    m = O0Model(freq, inc, limit)
    rc = RDec(data)
    out = bytearray()
    for _ in range(n):
        target = rc.get_freq(m.tot)
        sym, cum = m.find(target)
        rc.decode(cum, m.freq[sym], m.tot)
        m.update(sym)
        out.append(sym)
    return bytes(out)

# ---------------------------------------------------------------------------
# Order-1 adaptive model warm-started from from-image order-1 histogram.
# 256 contexts x 256 symbols. To bound decoder RAM we lazily build a context's
# table from the from-image only when first needed... but that needs a 2nd scan.
# Instead: precompute compact per-context seed via single pass; store as list of
# 256 small arrays. RAM honest accounting below.
# ---------------------------------------------------------------------------
def enc_o1(insert, frm, init_scale, floor, inc, limit):
    # build order-1 counts from from-image
    ctx = [[0]*256 for _ in range(256)]
    prev = 0
    for b in frm:
        ctx[prev][b] += 1
        prev = b
    # seed models
    models = []
    for c in range(256):
        row = ctx[c]
        s = sum(row)
        if s == 0:
            fr = [floor]*256
        else:
            fr = [floor + (row[i]*init_scale)//s for i in range(256)]
        models.append(fr)
    tots = [sum(fr) for fr in models]
    rc = REnc()
    prev = 0
    for b in insert:
        fr = models[prev]; tot = tots[prev]
        cum = 0
        for i in range(b):
            cum += fr[i]
        rc.encode(cum, fr[b], tot)
        fr[b] += inc; tots[prev] += inc
        if tots[prev] >= limit:
            t = 0
            for i in range(256):
                fr[i] = (fr[i] >> 1) | 1; t += fr[i]
            tots[prev] = t
        prev = b
    return rc.finish()

def dec_o1(data, n, frm, init_scale, floor, inc, limit):
    ctx = [[0]*256 for _ in range(256)]
    prev = 0
    for b in frm:
        ctx[prev][b] += 1
        prev = b
    models = []
    for c in range(256):
        row = ctx[c]
        s = sum(row)
        if s == 0:
            fr = [floor]*256
        else:
            fr = [floor + (row[i]*init_scale)//s for i in range(256)]
        models.append(fr)
    tots = [sum(fr) for fr in models]
    rc = RDec(data)
    out = bytearray()
    prev = 0
    for _ in range(n):
        fr = models[prev]; tot = tots[prev]
        target = rc.get_freq(tot)
        c = 0; sym = 255
        for i in range(256):
            if c + fr[i] > target:
                sym = i; break
            c += fr[i]
        cum = 0
        for i in range(sym):
            cum += fr[i]
        rc.decode(cum, fr[sym], tot)
        fr[sym] += inc; tots[prev] += inc
        if tots[prev] >= limit:
            t = 0
            for i in range(256):
                fr[i] = (fr[i] >> 1) | 1; t += fr[i]
            tots[prev] = t
        out.append(sym); prev = sym
    return bytes(out)

# ---------------------------------------------------------------------------
# Bucketed order-1: context = prev_byte >> shift. RAM-bounded variant.
#   NCTX = 256 >> shift contexts; table RAM = NCTX*256*2 bytes (uint16 counts).
# ---------------------------------------------------------------------------
def _build_o1b(frm, nctx, shift, init_scale, floor):
    ctx = [[0]*256 for _ in range(nctx)]
    prev = 0
    for b in frm:
        ctx[prev >> shift][b] += 1
        prev = b
    models = []
    for c in range(nctx):
        row = ctx[c]; s = sum(row)
        fr = [floor]*256 if s == 0 else [floor + (row[i]*init_scale)//s for i in range(256)]
        models.append(fr)
    return models, [sum(fr) for fr in models]

def enc_o1b(ins, frm, nctx, shift, init_scale, floor, inc, limit):
    models, tots = _build_o1b(frm, nctx, shift, init_scale, floor)
    rc = REnc(); prev = 0
    for b in ins:
        ci = prev >> shift; fr = models[ci]; tot = tots[ci]
        cum = 0
        for i in range(b): cum += fr[i]
        rc.encode(cum, fr[b], tot); fr[b] += inc; tots[ci] += inc
        if tots[ci] >= limit:
            t = 0
            for i in range(256): fr[i] = (fr[i] >> 1) | 1; t += fr[i]
            tots[ci] = t
        prev = b
    return rc.finish()

def dec_o1b(data, n, frm, nctx, shift, init_scale, floor, inc, limit):
    models, tots = _build_o1b(frm, nctx, shift, init_scale, floor)
    rc = RDec(data); out = bytearray(); prev = 0
    for _ in range(n):
        ci = prev >> shift; fr = models[ci]; tot = tots[ci]
        tg = rc.get_freq(tot); c = 0; sym = 255
        for i in range(256):
            if c + fr[i] > tg: sym = i; break
            c += fr[i]
        cum = 0
        for i in range(sym): cum += fr[i]
        rc.decode(cum, fr[sym], tot); fr[sym] += inc; tots[ci] += inc
        if tots[ci] >= limit:
            t = 0
            for i in range(256): fr[i] = (fr[i] >> 1) | 1; t += fr[i]
            tots[ci] = t
        out.append(sym); prev = sym
    return bytes(out)

# ---------------------------------------------------------------------------
def main():
    names = ['small','medium','large']
    data = {n: common.get(n) for n in names}
    LIMIT = BOT - 1

    # ---- DEPLOYABLE O0 (512 B table) : init_scale=16384, inc=32 ----
    o0 = {}
    for n in names:
        ins, frm = data[n]
        freq = build_o0(frm, 16384, 1)
        enc = enc_o0(ins, freq, 32, LIMIT)
        dec = dec_o0(enc, len(ins), build_o0(frm, 16384, 1), 32, LIMIT)
        assert dec == ins, n
        o0[n] = len(enc)
    print("O0 warm-start (RAM ~640B):", o0)

    # ---- DEPLOYABLE bucketed O1, 8 ctx = 4 KiB table : shift=5 ----
    o1b = {}
    for n in names:
        ins, frm = data[n]
        enc = enc_o1b(ins, frm, 8, 5, 512, 1, 12, LIMIT)
        dec = dec_o1b(enc, len(ins), frm, 8, 5, 512, 1, 12, LIMIT)
        assert dec == ins, n
        o1b[n] = len(enc)
    print("O1 bucketed 8ctx (RAM ~4.2KiB):", o1b)

    # ---- bucketed O1, 16 ctx = 8 KiB (over budget, reference) shift=4 ----
    o1b16 = {}
    for n in names:
        ins, frm = data[n]
        enc = enc_o1b(ins, frm, 16, 4, 512, 1, 12, LIMIT)
        dec = dec_o1b(enc, len(ins), frm, 16, 4, 512, 1, 12, LIMIT)
        assert dec == ins, n
        o1b16[n] = len(enc)
    print("O1 bucketed 16ctx (RAM ~8.2KiB):", o1b16)

    # ---- full dense O1 (128 KiB, RAM-GATED reference) : init_scale=512, inc=16 ----
    o1 = {}
    for n in names:
        ins, frm = data[n]
        enc = enc_o1(ins, frm, 512, 1, 16, LIMIT)
        dec = dec_o1(enc, len(ins), frm, 512, 1, 16, LIMIT)
        assert dec == ins, n
        o1[n] = len(enc)
    print("O1 dense 256ctx (RAM ~128KiB, GATED):", o1)

if __name__ == '__main__':
    main()
