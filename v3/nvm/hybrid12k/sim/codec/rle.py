"""
RLE-based codec for relocation-delta value streams.

Target: ARM Cortex-M0+ firmware patcher. Integer-only, small RAM, MCU-friendly
decoder. Lossless.

==============================================================================
SCHEME (zero-run RLE + dictionary + canonical Huffman)
==============================================================================
The streams are dominated by zeros (78% combined). Each stream (bl, ldr) is
coded SEPARATELY with its own small fixed model, exploiting two facts:

  1. Most values are 0  -> code the LENGTHS of zero-runs, not the zeros.
  2. The nonzero values come from a tiny per-stream dictionary
     (bl: 3 nonzero symbols, ldr: 11) and one symbol dominates each stream
     (-168 in bl, -336 in ldr) -> code the dictionary INDEX with a variable
     length prefix code so the common symbol costs ~1 bit.

A stream is modelled as a sequence of EVENTS, each:

      [ zero-run-length ]  [ nonzero symbol ]

meaning "skip N zeros, then emit this nonzero". A special END token marks the
final event (so a trailing zero-run is just "[run][END]").

  * Zero-run-length: Elias-gamma over (run+1). This is the win - run=0 (two
    adjacent nonzeros, very common here) costs a single bit, run=1 costs 3
    bits, etc. Decode is integer-only: count leading zero bits k, then read k
    more bits. No tables, no division.

  * Nonzero symbol AND the END token share one canonical Huffman alphabet of
    size (num_nonzero_syms + 1). Canonical Huffman is MCU-friendly: only the
    per-symbol CODE LENGTHS are shipped; both encoder and decoder rebuild the
    identical canonical codes deterministically. Decode uses the standard
    integer-only first-code/base-index method (a few comparisons per symbol).

==============================================================================
WIRE FORMAT  (per stream, byte order little-endian)
==============================================================================
  u8    K              = number of nonzero symbols (dictionary size)
  K x   zigzag-varint  = the nonzero symbol VALUES, in canonical-alphabet order
                         (index 0..K-1; index K == END, has no value)
  (K+1) nibble-packed code lengths, one per alphabet symbol (0..15 fits;
                         here max length is 8). Order: sym0..symK-1, then END.
  u16   N              = total number of values in the stream
  []    bitstream      = events, MSB-first, zero-padded to a byte boundary

The dictionary VALUES double as the model. They are shipped freq-desc so the
canonical Huffman assignment (shorter codes to earlier/ more frequent symbols)
is implied by the code-length array.
"""

import json
import os
from collections import Counter


# ---------------------------------------------------------------------------
# Bit I/O  (MSB-first), integer-only
# ---------------------------------------------------------------------------
class BitWriter:
    def __init__(self):
        self.buf = bytearray()
        self.cur = 0
        self.nbits = 0

    def write_bit(self, b):
        self.cur = (self.cur << 1) | (b & 1)
        self.nbits += 1
        if self.nbits == 8:
            self.buf.append(self.cur)
            self.cur = 0
            self.nbits = 0

    def write_bits(self, value, count):
        for i in range(count - 1, -1, -1):
            self.write_bit((value >> i) & 1)

    def finish(self):
        if self.nbits:
            self.cur <<= (8 - self.nbits)
            self.buf.append(self.cur)
            self.cur = 0
            self.nbits = 0
        return bytes(self.buf)


class BitReader:
    def __init__(self, data):
        self.data = data
        self.pos = 0

    def read_bit(self):
        byte = self.data[self.pos >> 3]
        bit = (byte >> (7 - (self.pos & 7))) & 1
        self.pos += 1
        return bit

    def read_bits(self, count):
        v = 0
        for _ in range(count):
            v = (v << 1) | self.read_bit()
        return v


# ---------------------------------------------------------------------------
# Elias-gamma over (n+1) for zero-run lengths.  n >= 0.
#   encode: write (bitlen-1) leading zeros, then the bitlen-bit value m=n+1.
#   decode: count leading zeros k, read k more bits, m = (1<<k)|extra, n=m-1.
# ---------------------------------------------------------------------------
def write_gamma(bw, n):
    m = n + 1
    k = m.bit_length()          # number of bits in m
    for _ in range(k - 1):      # k-1 leading zeros
        bw.write_bit(0)
    bw.write_bits(m, k)         # m (its top bit is the unary terminator '1')


def read_gamma(br):
    k = 0
    while br.read_bit() == 0:
        k += 1
    # we have consumed the leading '1'; read k more bits
    m = (1 << k) | br.read_bits(k)
    return m - 1


# ---------------------------------------------------------------------------
# zigzag signed varint (byte-level) for the shipped dictionary values
# ---------------------------------------------------------------------------
def zz_encode(n):
    return (n << 1) if n >= 0 else (((-n) << 1) - 1)


def zz_decode(z):
    return (z >> 1) ^ -(z & 1)


def varint_encode(out, n):
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            break


def varint_decode(data, pos):
    n = 0
    shift = 0
    while True:
        b = data[pos]
        pos += 1
        n |= (b & 0x7F) << shift
        if not (b & 0x80):
            break
        shift += 7
    return n, pos


# ---------------------------------------------------------------------------
# Canonical Huffman: code-length computation, and code assignment.
# ---------------------------------------------------------------------------
def huffman_code_lengths(freqs):
    """freqs: list of weights (one per symbol). Returns list of code lengths.
    Single-symbol alphabet -> length 1."""
    import heapq
    n = len(freqs)
    if n == 1:
        return [1]
    lengths = [0] * n
    # nodes: [weight, tiebreak, members]
    heap = [[f if f > 0 else 1, i, [i]] for i, f in enumerate(freqs)]
    heapq.heapify(heap)
    ctr = n
    while len(heap) > 1:
        a = heapq.heappop(heap)
        b = heapq.heappop(heap)
        for s in a[2] + b[2]:
            lengths[s] += 1
        heapq.heappush(heap, [a[0] + b[0], ctr, a[2] + b[2]])
        ctr += 1
    return lengths


def canonical_codes(lengths):
    """Standard canonical Huffman assignment from code lengths.
    Returns (codes, lengths) where codes[i] is the integer code for symbol i.
    Symbols are assigned in order of (length, symbol-index)."""
    n = len(lengths)
    maxlen = max(lengths)
    # count codes of each length
    bl_count = [0] * (maxlen + 1)
    for l in lengths:
        bl_count[l] += 1
    # first code for each length
    next_code = [0] * (maxlen + 2)
    code = 0
    for bits in range(1, maxlen + 1):
        code = (code + bl_count[bits - 1]) << 1
        next_code[bits] = code
    codes = [0] * n
    for i in range(n):
        l = lengths[i]
        codes[i] = next_code[l]
        next_code[l] += 1
    return codes


def build_decoder_tables(lengths):
    """Build the integer-only canonical-Huffman decode tables.
    Returns (first_code, first_index, symbols_by_canonical_order, maxlen).
    Decode: accumulate bits; at length L, if code < first_code[L]+count[L]
    then symbol = sorted_syms[first_index[L] + (code - first_code[L])]."""
    n = len(lengths)
    maxlen = max(lengths)
    bl_count = [0] * (maxlen + 1)
    for l in lengths:
        bl_count[l] += 1
    # symbols sorted by (length, symbol index) = canonical order
    order = sorted(range(n), key=lambda i: (lengths[i], i))
    first_code = [0] * (maxlen + 1)
    first_index = [0] * (maxlen + 1)
    code = 0
    idx = 0
    for bits in range(1, maxlen + 1):
        first_code[bits] = code
        first_index[bits] = idx
        code = (code + bl_count[bits]) << 1
        idx += bl_count[bits]
    return first_code, first_index, bl_count, order, maxlen


# ---------------------------------------------------------------------------
# Encode / decode one stream
# ---------------------------------------------------------------------------
def encode_stream(values):
    c = Counter(v for v in values if v != 0)
    syms = [v for v, _ in c.most_common()]      # freq-desc
    K = len(syms)
    sym_index = {v: i for i, v in enumerate(syms)}
    END = K

    # Huffman over alphabet [sym0..symK-1, END]; END given weight 1.
    freqs = [c[s] for s in syms] + [1]
    lengths = huffman_code_lengths(freqs)
    codes = canonical_codes(lengths)

    # ---- header ----
    out = bytearray()
    out.append(K)
    for v in syms:
        varint_encode(out, zz_encode(v))
    # nibble-pack code lengths (alphabet size K+1)
    nibbles = lengths[:]                          # one per symbol incl END
    bi = 0
    while bi < len(nibbles):
        hi = nibbles[bi]
        lo = nibbles[bi + 1] if bi + 1 < len(nibbles) else 0
        out.append((hi << 4) | lo)
        bi += 2
    n = len(values)
    out.append(n & 0xFF)
    out.append((n >> 8) & 0xFF)

    # ---- bitstream ----
    bw = BitWriter()
    run = 0
    for v in values:
        if v == 0:
            run += 1
        else:
            write_gamma(bw, run)
            i = sym_index[v]
            bw.write_bits(codes[i], lengths[i])
            run = 0
    write_gamma(bw, run)
    bw.write_bits(codes[END], lengths[END])
    body = bw.finish()
    return bytes(out) + body


def decode_stream(data):
    pos = 0
    K = data[pos]; pos += 1
    syms = []
    for _ in range(K):
        z, pos = varint_decode(data, pos)
        syms.append(zz_decode(z))
    alpha = K + 1
    # read nibble-packed code lengths
    lengths = []
    nbytes = (alpha + 1) // 2
    for b in range(nbytes):
        byte = data[pos + b]
        lengths.append(byte >> 4)
        if len(lengths) < alpha:
            lengths.append(byte & 0x0F)
    lengths = lengths[:alpha]
    pos += nbytes
    n = data[pos] | (data[pos + 1] << 8); pos += 2

    first_code, first_index, bl_count, order, maxlen = build_decoder_tables(lengths)
    END = K

    br = BitReader(data[pos:])
    out = []
    while len(out) < n:
        run = read_gamma(br)
        if run:
            out.extend([0] * run)
        # decode one canonical-Huffman symbol
        code = 0
        length = 0
        while True:
            code = (code << 1) | br.read_bit()
            length += 1
            cnt = bl_count[length]
            if cnt and code < first_code[length] + cnt:
                sym = order[first_index[length] + (code - first_code[length])]
                break
        if sym == END:
            break
        out.append(syms[sym])
    return out


# ---------------------------------------------------------------------------
# Bench / verify
# ---------------------------------------------------------------------------
def main():
    here = os.path.dirname(os.path.abspath(__file__))
    d = json.load(open(os.path.join(here, "..", "reloc_values.json")))
    bl, ldr = d["bl"], d["ldr"]

    enc = {}
    allok = True
    for name, vals in [("bl", bl), ("ldr", ldr)]:
        e = encode_stream(vals)
        dec = decode_stream(e)
        ok = dec == vals
        allok = allok and ok
        enc[name] = e
        print(f"[{name:3}] values={len(vals):4d}  encoded={len(e):4d} B  round-trip {'PASS' if ok else 'FAIL'}")

    combined_in = bl + ldr
    combined_dec = decode_stream(enc["bl"]) + decode_stream(enc["ldr"])
    ok_comb = combined_dec == combined_in
    allok = allok and ok_comb
    print(f"[comb] values={len(combined_in):4d}  encoded={len(enc['bl'])+len(enc['ldr']):4d} B  "
          f"round-trip {'PASS' if ok_comb else 'FAIL'}")

    # ---- model-overhead breakdown per stream ----
    def model_size(vals):
        c = Counter(v for v in vals if v != 0)
        syms = [s for s, _ in c.most_common()]
        K = len(syms)
        m = 1                              # u8 K
        tmp = bytearray()
        for v in syms:
            varint_encode(tmp, zz_encode(v))
        m += len(tmp)                      # dictionary values
        m += (K + 1 + 1) // 2              # nibble-packed code lengths
        m += 2                             # u16 N
        return m

    bl_b, ldr_b = len(enc["bl"]), len(enc["ldr"])
    bl_m, ldr_m = model_size(bl), model_size(ldr)
    headers = 47
    val_sum = bl_b + ldr_b
    total = val_sum + headers

    print("\n=== MEASURED BYTES ===")
    print(f"  bl   coded : {bl_b:4d} B  (model {bl_m} B, bitstream {bl_b-bl_m} B)")
    print(f"  ldr  coded : {ldr_b:4d} B  (model {ldr_m} B, bitstream {ldr_b-ldr_m} B)")
    print(f"  model total: {bl_m+ldr_m:4d} B")
    print(f"  value-stream sum     : {val_sum:4d} B")
    print(f"  + dfpatch headers ~47: {headers:4d} B")
    print(f"  TOTAL                : {total:4d} B")

    print("\n=== COMPARISON ===")
    print(f"  raw varint (values)        : 5394 B")
    print(f"  heatshrink (values)        :  784 B")
    print(f"  heatshrink (full dfpatch)  :  833 B")
    print(f"  entropy floor (combined)   :  720 B")
    print(f"  entropy floor (separate)   :  590 B")
    print(f"\n  beats 833 (full)?        {'YES' if total < 833 else 'NO'}  (margin {833-total:+d} B)")
    print(f"  beats 784 (values)?      {'YES' if val_sum < 784 else 'NO'}  (margin {784-val_sum:+d} B vs value-stream sum {val_sum})")
    print(f"  beats 590 (sep floor)?   {'YES' if val_sum < 590 else 'NO'}  (value-stream sum {val_sum} vs 590)")
    print(f"  lossless?                {'YES' if allok else 'NO'}")


if __name__ == "__main__":
    main()
