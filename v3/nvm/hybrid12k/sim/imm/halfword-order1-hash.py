"""Halfword order-1 with a BOUNDED, from-image-derived context (<=4 KiB table).

Idea (what actually moves the needle on ARMv6-M Thumb):
  - Each insert halfword = (lo, hi).  The strongest cheap predictors are:
        lo  is predicted by the CURRENT high byte  (intra-instruction structure)
        hi  is predicted by the PREVIOUS high byte (instruction-class transition)
    A naive hash of the full previous halfword DESTROYS this structure (the high
    byte is an opcode-class field; hashing scrambles it) -- measured: hashing to
    a few buckets gives ~no gain over order-0.  So instead of hashing, we map the
    256 possible high-byte context values into K CLASSES via a tiny k-means over
    the from-image (cosine on the conditional symbol histograms).  The class map
    is DERIVED from the from-image by both sides (one flash scan) -> NOT shipped.

Coding:
  - Integer-only binary range coder, 12-bit probabilities (no floats anywhere).
  - Each byte is coded as two 4-bit nibbles via bit-trees:
        high nibble : bit-tree keyed on the CLASS of the context byte   (K classes)
        low  nibble : bit-tree keyed on the high nibble (16 shared trees)
    Counts (c0,c1) per node are uint8, seeded by one from-image scan and bumped
    online identically on both sides; on overflow both halves are halved.

RAM (decoder peak, HONEST):
  prob table  = (2*K class high-nibble trees + 16 SHARED low-nibble trees)
                * 16 nodes * 2 counts * 1 byte(uint8)
              = (2*K + 16) * 32 bytes.   K=40 -> 96 trees -> 3072 bytes.
    (the low-nibble trees are shared across L and H: the low nibble of an opcode
     byte carries far less class structure than the high nibble, so a single set
     of 16 low-nibble trees keyed only on the byte's own high nibble loses almost
     nothing vs. two separate sets, and saves 512 B.)
  class maps  = 2 * 256 bytes = 512 bytes (Lmap[curHi]->class, Hmap[prevHi]->class)
                (built from the from-image scan, held during decode)
  range-coder + loop state: < 64 bytes.
  TOTAL peak  ~= 3072 + 512 + 64 = 3648 bytes  (K=40, under the 4 KiB budget).

Model-build cost & the HONEST MCU caveat:
  To DERIVE the class maps on-device (shipped_table_bytes = 0) the decoder must do
  ONE from-image pass to gather the two 256x256 conditional histograms, then a
  small fixed-iteration k-means (K<=40, 10 iters) on 256 vectors, then a second
  from-image pass to seed the counts.  The two histograms are TRANSIENT but cost
  ~512 KB of build RAM -- that does NOT fit a 32 KiB-SRAM M0+.  So the realistic
  MCU deployment SHIPS the two 256-byte class maps (512 B total, allowed by the
  spec: "a 256-entry-class table built from the from-image is fine").  With the
  maps shipped, on-device build is a single streaming from-image seeding pass with
  NO large histogram, and decode-time peak stays 3648 B.  Both paths produce
  byte-identical output (the maps are the only thing k-means computes).
    shipped_table_bytes = 512  (deployable path)  /  0  (if decoder runs k-means,
    not MCU-feasible due to 512 KB build histograms).
  Decode-time peak RAM is 3648 B either way -- WITHIN the 4 KiB budget.
"""
import sys, os, math, array
sys.path[:0] = [os.path.dirname(os.path.abspath(__file__))]
import common

# ------------------------------------------------------------------ params
PBITS = 12
PMAX = 1 << PBITS
K = 40                 # context classes; (2*K+16) trees * 32 B = 3072 B at K=40
NB = 16                # bit-tree node slots for a 4-bit nibble (1..15 used)
CMAX = 252             # uint8 count renorm ceiling (c0+c1 stays < 256)
KMEANS_ITERS = 10

# ------------------------------------------------------------------ class map (from-image derived)
def build_maps(frm):
    """One from-image pass -> conditional histograms -> k-means class maps.
    Returns (Lmap, Hmap): 256-entry byte->class lookups, identical on both sides."""
    Lh = [array.array('I', [0] * 256) for _ in range(256)]   # P(lo | curHi)
    Hh = [array.array('I', [0] * 256) for _ in range(256)]   # P(hi | prevHi)
    prev = 0
    n = len(frm) & ~1
    for i in range(0, n, 2):
        lo = frm[i]; hi = frm[i + 1]; ph = (prev >> 8) & 0xFF
        Lh[hi][lo] += 1
        Hh[ph][hi] += 1
        prev = lo | (hi << 8)

    def kmeans(hist):
        tot = [sum(h) for h in hist]
        items = [i for i in range(256) if tot[i] > 0]
        assign = [0] * 256
        if not items:
            return assign
        order = sorted(items, key=lambda i: -tot[i])
        cent = [list(hist[order[j % len(order)]]) for j in range(K)]
        for _ in range(KMEANS_ITERS):
            cnorm = [math.sqrt(sum(x * x for x in c)) + 1e-9 for c in cent]
            for i in items:
                h = hist[i]; best = -1.0; bj = 0
                nz = [s for s in range(256) if h[s]]
                for j in range(K):
                    cj = cent[j]
                    d = 0
                    for s in nz:
                        d += h[s] * cj[s]
                    d /= cnorm[j]
                    if d > best:
                        best = d; bj = j
                assign[i] = bj
            newc = [[0] * 256 for _ in range(K)]
            cnt = [0] * K
            for i in items:
                j = assign[i]; cnt[j] += 1; h = hist[i]
                for s in range(256):
                    if h[s]:
                        newc[j][s] += h[s]
            for j in range(K):
                if cnt[j]:
                    cent[j] = newc[j]
        return assign

    return kmeans(Lh), kmeans(Hh)

# ------------------------------------------------------------------ binary range coder (carry-aware)
class Enc:
    def __init__(self):
        self.low = 0; self.rng = 0xFFFFFFFF; self.out = bytearray()
    def _carry(self):
        i = len(self.out) - 1
        while i >= 0 and self.out[i] == 0xFF:
            self.out[i] = 0; i -= 1
        if i >= 0:
            self.out[i] += 1
    def _renorm(self):
        while self.rng < (1 << 24):
            self.out.append((self.low >> 24) & 0xFF)
            self.low = (self.low << 8) & 0xFFFFFFFF
            self.rng = (self.rng << 8) & 0xFFFFFFFF
    def bit(self, p0, b):
        bound = (self.rng >> PBITS) * p0
        if b == 0:
            self.rng = bound
        else:
            nl = (self.low + bound) & 0xFFFFFFFF
            if nl < self.low:
                self._carry()
            self.low = nl
            self.rng -= bound
        self._renorm()
    def finish(self):
        for _ in range(4):
            self.out.append((self.low >> 24) & 0xFF)
            self.low = (self.low << 8) & 0xFFFFFFFF
        return bytes(self.out)

class Dec:
    def __init__(self, data):
        self.data = data; self.pos = 0; self.rng = 0xFFFFFFFF; self.code = 0
        for _ in range(4):
            self.code = ((self.code << 8) | self._byte()) & 0xFFFFFFFF
    def _byte(self):
        if self.pos < len(self.data):
            v = self.data[self.pos]; self.pos += 1; return v
        return 0
    def _renorm(self):
        while self.rng < (1 << 24):
            self.code = ((self.code << 8) | self._byte()) & 0xFFFFFFFF
            self.rng = (self.rng << 8) & 0xFFFFFFFF
    def bit(self, p0):
        bound = (self.rng >> PBITS) * p0
        if self.code < bound:
            self.rng = bound; b = 0
        else:
            self.code -= bound; self.rng -= bound; b = 1
        self._renorm()
        return b

# ------------------------------------------------------------------ count tables
# Tree layout: a flat uint8 buffer.  Trees:
#   class high-nibble trees for L : index 0 .. K-1
#   class high-nibble trees for H : index K .. 2K-1
#   SHARED low-nibble trees       : index 2K .. 2K+15   (keyed on byte's high nibble,
#                                                         shared by both L and H bytes)
# Each tree: NB nodes of (c0,c1) -> 2*NB uint8.
NTREES = 2 * K + 16
TREE_STRIDE = 2 * NB   # bytes per tree

def _alloc():
    c = bytearray([1]) * (NTREES * TREE_STRIDE)
    return c

def _tabidx_L_hi(cls): return cls
def _tabidx_H_hi(cls): return K + cls
def _tabidx_L_lo(hin): return 2 * K + hin
def _tabidx_H_lo(hin): return 2 * K + hin   # shared with L low-nibble trees

def _prob0(tab, base, node):
    a = tab[base + 2 * node]; b = tab[base + 2 * node + 1]
    p = (a * PMAX) // (a + b)
    if p < 1: p = 1
    elif p > PMAX - 1: p = PMAX - 1
    return p

def _bump(tab, base, node, bit):
    ia = base + 2 * node; ib = ia + 1
    if bit == 0:
        tab[ia] += 1
    else:
        tab[ib] += 1
    if tab[ia] + tab[ib] >= CMAX:
        tab[ia] = (tab[ia] >> 1) | 1
        tab[ib] = (tab[ib] >> 1) | 1

# ------------------------------------------------------------------ nibble coding
def _enc_nib(enc, tab, base, v):
    node = 1
    for k in range(3, -1, -1):
        b = (v >> k) & 1
        enc.bit(_prob0(tab, base, node), b)
        _bump(tab, base, node, b)
        node = (node << 1) | b

def _dec_nib(dec, tab, base):
    node = 1
    for _ in range(4):
        b = dec.bit(_prob0(tab, base, node))
        _bump(tab, base, node, b)
        node = (node << 1) | b
    return node & 0xF

def _train_nib(tab, base, v):
    node = 1
    for k in range(3, -1, -1):
        b = (v >> k) & 1
        _bump(tab, base, node, b)
        node = (node << 1) | b

# ------------------------------------------------------------------ seeding (from-image scan #2)
# CAUSAL ORDER per halfword: code HI first (ctx = prevHi class), then LO (ctx = curHi
# class), so the decoder always knows the context before it needs it.
def _seed(tab, frm, Lmap, Hmap):
    prev = 0
    n = len(frm) & ~1
    for i in range(0, n, 2):
        lo = frm[i]; hi = frm[i + 1]; ph = (prev >> 8) & 0xFF
        hcl = Hmap[ph]; hhin = hi >> 4; hlon = hi & 0xF
        _train_nib(tab, _tabidx_H_hi(hcl) * TREE_STRIDE, hhin)
        _train_nib(tab, _tabidx_H_lo(hhin) * TREE_STRIDE, hlon)
        lcl = Lmap[hi]; lhin = lo >> 4; llon = lo & 0xF
        _train_nib(tab, _tabidx_L_hi(lcl) * TREE_STRIDE, lhin)
        _train_nib(tab, _tabidx_L_lo(lhin) * TREE_STRIDE, llon)
        prev = lo | (hi << 8)

# ------------------------------------------------------------------ public encode/decode
def encode(insert, frm):
    Lmap, Hmap = build_maps(frm)
    tab = _alloc()
    _seed(tab, frm, Lmap, Hmap)
    enc = Enc()
    prev = 0
    n = len(insert) & ~1
    for i in range(0, n, 2):
        lo = insert[i]; hi = insert[i + 1]; ph = (prev >> 8) & 0xFF
        hcl = Hmap[ph]; hhin = hi >> 4; hlon = hi & 0xF
        _enc_nib(enc, tab, _tabidx_H_hi(hcl) * TREE_STRIDE, hhin)
        _enc_nib(enc, tab, _tabidx_H_lo(hhin) * TREE_STRIDE, hlon)
        lcl = Lmap[hi]; lhin = lo >> 4; llon = lo & 0xF
        _enc_nib(enc, tab, _tabidx_L_hi(lcl) * TREE_STRIDE, lhin)
        _enc_nib(enc, tab, _tabidx_L_lo(lhin) * TREE_STRIDE, llon)
        prev = lo | (hi << 8)
    return enc.finish()

def decode(data, frm, out_len):
    Lmap, Hmap = build_maps(frm)
    tab = _alloc()
    _seed(tab, frm, Lmap, Hmap)
    dec = Dec(data)
    out = bytearray()
    prev = 0
    for _ in range(out_len // 2):
        ph = (prev >> 8) & 0xFF
        hcl = Hmap[ph]
        hhin = _dec_nib(dec, tab, _tabidx_H_hi(hcl) * TREE_STRIDE)
        hlon = _dec_nib(dec, tab, _tabidx_H_lo(hhin) * TREE_STRIDE)
        hi = (hhin << 4) | hlon
        lcl = Lmap[hi]
        lhin = _dec_nib(dec, tab, _tabidx_L_hi(lcl) * TREE_STRIDE)
        llon = _dec_nib(dec, tab, _tabidx_L_lo(lhin) * TREE_STRIDE)
        lo = (lhin << 4) | llon
        out.append(lo); out.append(hi)
        prev = lo | (hi << 8)
    return bytes(out)

# ------------------------------------------------------------------ main
if __name__ == '__main__':
    table_bytes = NTREES * TREE_STRIDE
    maps_bytes = 2 * 256
    peak = table_bytes + maps_bytes + 64   # K=40 -> 3072 + 512 + 64 = 3648 B
    results = {}
    ok = True
    for name in ['small', 'medium', 'large']:
        ins, frm = common.get(name)
        comp = encode(ins, frm)
        back = decode(comp, frm, len(ins))
        good = back == ins
        ok = ok and good
        results[name] = len(comp)
        print(f"{name}: insert={len(ins)} compressed={len(comp)} lossless={good}")
    print(f"K={K}  trees={NTREES}  prob_table={table_bytes}B  class_maps={maps_bytes}B")
    print(f"peak decoder RAM ~= {peak}B (decode-time) ; integer_only=Y")
    print(f"shipped_table_bytes=512 (ship 256B Lmap+256B Hmap; on-MCU k-means needs "
          f"~512KB transient histograms -> not 32KiB-feasible, so ship the maps)")
    print(f"ALL LOSSLESS: {ok}")
    common.report('halfword-order1-hash', results, ram_bytes=peak,
                  integer_only=True, shipped_table_B=512,
                  notes=f"K={K} from-image kmeans class ctx + shared-low nibble bit-tree range coder; "
                        f"ship 512B class maps (build histograms too big for M0+)")
