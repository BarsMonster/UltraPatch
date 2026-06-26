"""Order-1 byte model for INSERT (new Thumb code) literals.

Context = previous byte (optionally quantized/hashed to fewer buckets to bound RAM).
Per-context symbol frequencies are DERIVED FROM THE FROM-IMAGE in a single pass
(no shipped table; encoder and decoder both scan the shared from-image identically).

Coder: integer-only 32-bit range coder (carryless, Subbotin-style).

RAM accounting (decoder, this stage):
  Model = NBUCKET contexts, each a cumulative-frequency table of 257 entries.
  We hold per-context freq counts as uint16 -> NBUCKET * 256 * 2 bytes,
  plus per-context total (NBUCKET * 2). On the fly we compute cum on demand by
  a small running sum (no stored 257-cum array needed) using a Fenwick-free
  linear scan -> cheap on M0+, O(256) per symbol but no extra RAM.

We sweep NBUCKET in {256, 64, 32, 16, 8} to show the size/RAM tradeoff.
"""
import sys, os
sys.path[:0] = ['/ai_sw/detools-dev/m4dev/sim/imm']
import common

# ---------------- bucket function ----------------
# Quantize previous byte to a bucket. For NBUCKET==256 it is identity (full o1).
# For fewer, we use top bits (high bits of a byte carry the most structure for
# Thumb opcodes, where the high byte of a halfword selects the opcode class).
def make_bucketer(nbucket):
    if nbucket == 256:
        return lambda b: b
    shift = 0
    n = nbucket
    while (1 << shift) < n:
        shift += 1
    # nbucket is power of two; take top `shift` bits of the byte
    rsh = 8 - shift
    return lambda b: b >> rsh

# ---------------- model from from-image ----------------
def build_model(frm, nbucket, byte_freq=False):
    """byte_freq=True -> rescale each context so every freq fits in a uint8
    (1..255), shrinking decoder RAM to nbucket*256 bytes."""
    bk = make_bucketer(nbucket)
    # freq[ctx][sym]; use list of lists of ints
    freq = [[0] * 256 for _ in range(nbucket)]
    prev = 0  # initial context (start-of-stream) = 0
    for b in frm:
        c = bk(prev)
        freq[c][b] += 1
        prev = b
    if byte_freq:
        # rescale so max entry <= 255, each >=1. total <= 256*255 < 2^16.
        for c in range(nbucket):
            row = freq[c]
            mx = max(row)
            if mx == 0:
                for i in range(256):
                    row[i] = 1
                continue
            for i in range(256):
                # map count -> 1..255
                v = (row[i] * 254) // mx + 1
                row[i] = v
        return freq, bk
    # Laplace smoothing + scale so totals fit in range-coder limit.
    # Range coder needs total <= 2^16. Scale each context to <= 2^14 to be safe.
    TOTAL_BITS = 14
    TMAX = 1 << TOTAL_BITS
    for c in range(nbucket):
        row = freq[c]
        # add-1 smoothing for full alphabet coverage (lossless: every sym codable)
        s = 0
        for i in range(256):
            row[i] += 1
            s += row[i]
        if s > TMAX:
            # rescale down to TMAX preserving >=1
            scale = s
            ns = 0
            for i in range(256):
                v = (row[i] * (TMAX - 256)) // scale + 1
                row[i] = v
                ns += v
            # ns <= TMAX guaranteed since each term <= original share
        # else keep
    return freq, bk

def cum_of(row, sym):
    """Return (cumlo, f, total) for symbol via linear scan (no extra RAM)."""
    lo = 0
    for i in range(sym):
        lo += row[i]
    f = row[sym]
    total = lo + f
    for i in range(sym + 1, 256):
        total += row[i]
    return lo, f, total

def total_of(row):
    return sum(row)

def find_sym(row, target):
    """Given cum target in [0,total), find sym and (cumlo,f)."""
    lo = 0
    for i in range(256):
        f = row[i]
        if lo + f > target:
            return i, lo, f
        lo += f
    # shouldn't happen
    raise RuntimeError("decode underflow")

# ---------------- 32-bit range coder (Subbotin carryless) ----------------
TOP = 1 << 24
BOT = 1 << 16
MASK = 0xFFFFFFFF

class Encoder:
    def __init__(self):
        self.low = 0
        self.range = MASK
        self.out = bytearray()
    def encode(self, cumlo, f, total):
        r = self.range // total
        self.low = (self.low + r * cumlo) & MASK
        self.range = r * f
        while True:
            if (self.low ^ (self.low + self.range)) < TOP:
                pass
            elif self.range < BOT:
                self.range = (-self.low) & (BOT - 1)
            else:
                break
            self.out.append((self.low >> 24) & 0xFF)
            self.low = (self.low << 8) & MASK
            self.range = (self.range << 8) & MASK
    def finish(self):
        for _ in range(4):
            self.out.append((self.low >> 24) & 0xFF)
            self.low = (self.low << 8) & MASK
        return bytes(self.out)

class Decoder:
    def __init__(self, data):
        self.data = data
        self.pos = 0
        self.low = 0
        self.range = MASK
        self.code = 0
        for _ in range(4):
            self.code = ((self.code << 8) | self._byte()) & MASK
    def _byte(self):
        if self.pos < len(self.data):
            b = self.data[self.pos]; self.pos += 1; return b
        return 0
    def getfreq(self, total):
        r = self.range // total
        v = ((self.code - self.low) & MASK) // r
        return v if v < total else total - 1
    def decode(self, cumlo, f, total):
        r = self.range // total
        self.low = (self.low + r * cumlo) & MASK
        self.range = r * f
        while True:
            if (self.low ^ (self.low + self.range)) < TOP:
                pass
            elif self.range < BOT:
                self.range = (-self.low) & (BOT - 1)
            else:
                break
            self.code = ((self.code << 8) | self._byte()) & MASK
            self.low = (self.low << 8) & MASK
            self.range = (self.range << 8) & MASK

# ---------------- encode / decode ----------------
def encode(insert, frm, nbucket=256, byte_freq=False):
    freq, bk = build_model(frm, nbucket, byte_freq)
    totals = [total_of(freq[c]) for c in range(nbucket)]
    enc = Encoder()
    prev = 0
    for b in insert:
        c = bk(prev)
        row = freq[c]
        lo, f, _ = cum_of(row, b)
        enc.encode(lo, f, totals[c])
        prev = b
    return enc.finish()

def decode(data, frm, n, nbucket=256, byte_freq=False):
    freq, bk = build_model(frm, nbucket, byte_freq)
    totals = [total_of(freq[c]) for c in range(nbucket)]
    dec = Decoder(data)
    out = bytearray()
    prev = 0
    for _ in range(n):
        c = bk(prev)
        row = freq[c]
        total = totals[c]
        target = dec.getfreq(total)
        sym, lo, f = find_sym(row, target)
        dec.decode(lo, f, total)
        out.append(sym)
        prev = sym
    return bytes(out)

# ---------------- RAM model ----------------
def decoder_ram(nbucket, byte_freq=False):
    # freq table dominates. totals computed on the fly (linear scan) -> no store,
    # but we cache totals[nbucket] as uint16. + small coder state (~64B).
    bytes_per_entry = 1 if byte_freq else 2
    return nbucket * 256 * bytes_per_entry + nbucket * 2 + 64

if __name__ == "__main__":
    names = ['small', 'medium', 'large']
    data = {n: common.get(n) for n in names}

    print("=== uint16-freq variant (RAM-heavy) ===")
    print(f"{'NB':>4} {'small':>6} {'medium':>7} {'large':>7} {'RAM_B':>7} {'lossless':>9}")
    for nb in [256, 64, 32, 16, 8]:
        sizes = {}; ok = True
        for n in names:
            ins, frm = data[n]
            enc = encode(ins, frm, nb)
            dec = decode(enc, frm, len(ins), nb)
            ok = ok and (dec == ins)
            sizes[n] = len(enc)
        print(f"{nb:>4} {sizes['small']:>6} {sizes['medium']:>7} {sizes['large']:>7} {decoder_ram(nb):>7} {str(ok):>9}")

    print()
    print("=== uint8-freq variant (RAM = NB*256 + overhead) ===")
    print(f"{'NB':>4} {'small':>6} {'medium':>7} {'large':>7} {'RAM_B':>7} {'lossless':>9}")
    for nb in [256, 64, 32, 16, 8]:
        sizes = {}; ok = True
        for n in names:
            ins, frm = data[n]
            enc = encode(ins, frm, nb, byte_freq=True)
            dec = decode(enc, frm, len(ins), nb, byte_freq=True)
            ok = ok and (dec == ins)
            sizes[n] = len(enc)
        print(f"{nb:>4} {sizes['small']:>6} {sizes['medium']:>7} {sizes['large']:>7} {decoder_ram(nb, True):>7} {str(ok):>9}")
    print()
    print("Baseline two-table parity: small=300 large=3388 ; byte_o1 floor 290/3243")
