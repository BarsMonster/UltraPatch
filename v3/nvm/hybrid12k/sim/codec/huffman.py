"""
Canonical-Huffman codec for relocation-delta value streams.

Target: ARM Cortex-M0+ firmware patcher. Integer-only, small RAM, MCU-friendly
decoder. Lossless. JPEG-style: a maximum code length is pre-specified to bound
the decoder's bit width, and code lengths are length-limited to that bound. Only
the canonical code LENGTHS are shipped (nibble-packed); the decoder rebuilds the
codes deterministically. bl and ldr are coded with SEPARATE models.

==============================================================================
TWO VARIANTS
==============================================================================
(a) PLAIN order-0 Huffman, zero-as-symbol.
    The full alphabet (including the value 0) is Huffman-coded directly. One
    canonical code per distinct value; 0 gets the shortest code (~78% of the
    stream). No run-coding.

(b) ZERO-RLE + Huffman of nonzeros.
    Same event model as the sibling RLE prototype, but the zero-run LENGTH is
    *also* Huffman-coded (binned) instead of Elias-gamma, and the nonzero
    symbol + END share a second canonical alphabet. This tests whether
    explicit run-coding beats letting Huffman spend ~0.3 bits per zero.

Both variants are pure canonical Huffman on the wire, decoder is integer-only.

==============================================================================
WIRE FORMAT
==============================================================================
Variant (a), per stream (little-endian):
  u8    K              = alphabet size (number of distinct values, incl 0)
  K x   zigzag-varint  = the VALUES in canonical-alphabet order (freq-desc)
  K     nibble-packed code lengths (<= MAXLEN, fits a nibble for MAXLEN<=15)
  u16   N              = number of values in the stream
  []    bitstream      = N canonical codes, MSB-first, byte-padded

Variant (b), per stream (little-endian):
  u8    K              = number of nonzero symbols
  K x   zigzag-varint  = the nonzero VALUES, canonical order (freq-desc)
  u8    R              = size of the run-length alphabet
  K+1   nibble-packed code lengths for [sym0..symK-1, END]
  R     nibble-packed code lengths for run bins
  u16   N              = number of values in the stream
  []    bitstream      = events: [run-code][sym-code], terminated by END,
                          MSB-first, byte-padded
The run alphabet is a direct map 0..R-1 (run length == bin index); a run >= R-1
is escaped (see code). For this dataset runs fit a small direct alphabet.
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

    def write_bits(self, value, count):
        for i in range(count - 1, -1, -1):
            self.cur = (self.cur << 1) | ((value >> i) & 1)
            self.nbits += 1
            if self.nbits == 8:
                self.buf.append(self.cur)
                self.cur = 0
                self.nbits = 0

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


# ---------------------------------------------------------------------------
# zigzag signed varint (byte-level) for shipped dictionary values
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
# Canonical Huffman with JPEG-style length limiting.
# ---------------------------------------------------------------------------
def huffman_code_lengths(freqs):
    """Plain Huffman code lengths. Single symbol -> length 1."""
    import heapq
    n = len(freqs)
    if n == 1:
        return [1]
    lengths = [0] * n
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


def limit_code_lengths(freqs, maxlen):
    """Return Huffman code lengths with every length <= maxlen.

    Uses the standard package-merge-free approach: compute plain Huffman, then
    if any length exceeds maxlen, apply the classic length-limiting fixup that
    keeps the Kraft sum <= 1 and re-derives a valid canonical length set.
    For small alphabets this fixup is exact/near-optimal; we verify Kraft after.
    """
    lengths = huffman_code_lengths(freqs)
    if max(lengths) <= maxlen:
        return lengths

    n = len(freqs)
    # Clamp overlong lengths to maxlen. Shortening codes can only make the
    # Kraft sum LARGER, so afterwards the codeset may overflow the 2^-maxlen
    # budget and we must lengthen the cheapest symbols back until it fits.
    lengths = [min(l, maxlen) for l in lengths]
    one = 1 << maxlen                       # Kraft budget, in units of 2^-maxlen
    total = sum(1 << (maxlen - l) for l in lengths)
    # Lengthen the lowest-frequency symbols until the codeset is valid (<= one).
    order_by_freq = sorted(range(n), key=lambda i: (freqs[i], i))
    while total > one:
        for s in order_by_freq:
            if lengths[s] < maxlen:
                total -= 1 << (maxlen - lengths[s])
                lengths[s] += 1
                total += 1 << (maxlen - lengths[s])
                break
        else:
            # everything at maxlen but still over budget: impossible iff
            # n <= 2^maxlen, which holds for our tiny alphabets.
            raise ValueError("alphabet too large for maxlen")
    # We may now be UNDER budget (total < one) wasting code space; tighten by
    # shortening high-freq symbols where it stays valid (keeps it canonical &
    # prefix-free). This reclaims bits and lowers stream size.
    changed = True
    while changed:
        changed = False
        for s in sorted(range(n), key=lambda i: (-freqs[i], i)):
            if lengths[s] > 1:
                new_total = total - (1 << (maxlen - lengths[s])) \
                    + (1 << (maxlen - (lengths[s] - 1)))
                if new_total <= one:
                    total = new_total
                    lengths[s] -= 1
                    changed = True
    return lengths


def canonical_codes(lengths):
    """Standard canonical assignment in (length, symbol-index) order."""
    n = len(lengths)
    maxlen = max(lengths)
    bl_count = [0] * (maxlen + 1)
    for l in lengths:
        bl_count[l] += 1
    next_code = [0] * (maxlen + 2)
    code = 0
    for bits in range(1, maxlen + 1):
        code = (code + bl_count[bits - 1]) << 1
        next_code[bits] = code
    codes = [0] * n
    # canonical order = sort by (length, index)
    for i in sorted(range(n), key=lambda i: (lengths[i], i)):
        l = lengths[i]
        codes[i] = next_code[l]
        next_code[l] += 1
    return codes


def build_decoder_tables(lengths):
    """Integer-only canonical decode tables: per-length first-code/index."""
    n = len(lengths)
    maxlen = max(lengths)
    bl_count = [0] * (maxlen + 1)
    for l in lengths:
        bl_count[l] += 1
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


def decode_symbol(br, first_code, bl_count, first_index, order, maxlen):
    code = 0
    length = 0
    while True:
        code = (code << 1) | br.read_bit()
        length += 1
        cnt = bl_count[length]
        if cnt and code < first_code[length] + cnt:
            return order[first_index[length] + (code - first_code[length])]
        if length > maxlen:
            raise ValueError("invalid huffman stream")


# ---------------------------------------------------------------------------
# Nibble-pack helpers for shipped code-length arrays
# ---------------------------------------------------------------------------
def pack_nibbles(lengths, out):
    bi = 0
    while bi < len(lengths):
        hi = lengths[bi]
        lo = lengths[bi + 1] if bi + 1 < len(lengths) else 0
        out.append((hi << 4) | lo)
        bi += 2


def unpack_nibbles(data, pos, count):
    lengths = []
    nbytes = (count + 1) // 2
    for b in range(nbytes):
        byte = data[pos + b]
        lengths.append(byte >> 4)
        if len(lengths) < count:
            lengths.append(byte & 0x0F)
    return lengths[:count], pos + nbytes


# ===========================================================================
# VARIANT (a): plain order-0 Huffman, zero is a symbol
# ===========================================================================
MAXLEN = 8  # JPEG-style bound; nibble-packable, MCU-friendly bit width


def encode_plain(values):
    c = Counter(values)
    syms = [v for v, _ in c.most_common()]      # freq-desc
    K = len(syms)
    if K == 0:
        # empty stream: K=0, N=0, no table, no bitstream
        return bytes([0, 0, 0])
    sym_index = {v: i for i, v in enumerate(syms)}
    freqs = [c[s] for s in syms]
    lengths = limit_code_lengths(freqs, MAXLEN)
    codes = canonical_codes(lengths)

    out = bytearray()
    out.append(K)
    for v in syms:
        varint_encode(out, zz_encode(v))
    pack_nibbles(lengths, out)
    n = len(values)
    out.append(n & 0xFF)
    out.append((n >> 8) & 0xFF)

    if K == 1:
        return bytes(out)  # value implied by single symbol + N

    bw = BitWriter()
    for v in values:
        i = sym_index[v]
        bw.write_bits(codes[i], lengths[i])
    return bytes(out) + bw.finish()


def decode_plain(data):
    pos = 0
    K = data[pos]; pos += 1
    if K == 0:
        return []
    syms = []
    for _ in range(K):
        z, pos = varint_decode(data, pos)
        syms.append(zz_decode(z))
    lengths, pos = unpack_nibbles(data, pos, K)
    n = data[pos] | (data[pos + 1] << 8); pos += 2
    if K == 1:
        # single symbol: bitstream is N zero-length codes; just replicate
        return [syms[0]] * n

    fc, fi, blc, order, maxlen = build_decoder_tables(lengths)
    br = BitReader(data[pos:])
    out = []
    for _ in range(n):
        sym = decode_symbol(br, fc, blc, fi, order, maxlen)
        out.append(syms[sym])
    return out


# ===========================================================================
# VARIANT (b): zero-RLE + Huffman of nonzeros AND run lengths
# ===========================================================================
def _runs_and_syms(values):
    """Split stream into (run, nonzero) events plus a final run + END."""
    runs = []
    nz = []
    run = 0
    for v in values:
        if v == 0:
            run += 1
        else:
            runs.append(run)
            nz.append(v)
            run = 0
    final_run = run
    return runs, nz, final_run


def encode_rle_huff(values):
    runs, nz, final_run = _runs_and_syms(values)

    # nonzero + END alphabet
    cnz = Counter(nz)
    nz_syms = [v for v, _ in cnz.most_common()]
    K = len(nz_syms)
    nz_index = {v: i for i, v in enumerate(nz_syms)}
    END = K
    nz_freqs = [cnz[s] for s in nz_syms] + [1]
    nz_lengths = limit_code_lengths(nz_freqs, MAXLEN)
    nz_codes = canonical_codes(nz_lengths)

    # run-length alphabet: direct 0..R-1, with an ESCAPE = R-1 for big runs.
    # Pick R so that runs we see directly fit; escape carries a varint after.
    all_runs = runs + [final_run]
    max_run = max(all_runs) if all_runs else 0
    # choose R: cap the direct alphabet; runs >= R-1 use escape bin (R-1)
    R = min(max_run + 2, 16)  # keep <=16 so a length fits a nibble bound below
    ESC = R - 1
    crun = Counter(min(r, ESC) for r in all_runs)
    run_freqs = [crun.get(i, 0) for i in range(R)]
    # ensure escape symbol present in alphabet even if unused
    run_lengths = limit_code_lengths([max(f, 1) for f in run_freqs], MAXLEN)
    run_codes = canonical_codes(run_lengths)

    out = bytearray()
    out.append(K)
    for v in nz_syms:
        varint_encode(out, zz_encode(v))
    out.append(R)
    pack_nibbles(nz_lengths, out)          # K+1 entries
    pack_nibbles(run_lengths, out)         # R entries
    n = len(values)
    out.append(n & 0xFF)
    out.append((n >> 8) & 0xFF)

    bw = BitWriter()

    def emit_run(r):
        if r >= ESC:
            bw.write_bits(run_codes[ESC], run_lengths[ESC])
            # raw varint-ish: write run in 4-bit chunks, continuation bit
            x = r
            while True:
                chunk = x & 0x7
                x >>= 3
                bw.write_bits((1 if x else 0), 1)
                bw.write_bits(chunk, 3)
                if not x:
                    break
        else:
            bw.write_bits(run_codes[r], run_lengths[r])

    for r, v in zip(runs, nz):
        emit_run(r)
        i = nz_index[v]
        bw.write_bits(nz_codes[i], nz_lengths[i])
    emit_run(final_run)
    bw.write_bits(nz_codes[END], nz_lengths[END])
    return bytes(out) + bw.finish()


def decode_rle_huff(data):
    pos = 0
    K = data[pos]; pos += 1
    nz_syms = []
    for _ in range(K):
        z, pos = varint_decode(data, pos)
        nz_syms.append(zz_decode(z))
    R = data[pos]; pos += 1
    ESC = R - 1
    nz_lengths, pos = unpack_nibbles(data, pos, K + 1)
    run_lengths, pos = unpack_nibbles(data, pos, R)
    n = data[pos] | (data[pos + 1] << 8); pos += 2

    nfc, nfi, nblc, norder, nmax = build_decoder_tables(nz_lengths)
    rfc, rfi, rblc, rorder, rmax = build_decoder_tables(run_lengths)
    END = K

    br = BitReader(data[pos:])
    out = []

    def read_run():
        rb = decode_symbol(br, rfc, rblc, rfi, rorder, rmax)
        if rb == ESC:
            x = 0
            shift = 0
            while True:
                cont = br.read_bit()
                chunk = (br.read_bit() << 2) | (br.read_bit() << 1) | br.read_bit()
                x |= chunk << shift
                shift += 3
                if not cont:
                    break
            return x
        return rb

    while len(out) < n:
        run = read_run()
        if run:
            out.extend([0] * run)
        sym = decode_symbol(br, nfc, nblc, nfi, norder, nmax)
        if sym == END:
            break
        out.append(nz_syms[sym])
    return out


# ===========================================================================
# VARIANT (b2): zero-RLE with Elias-gamma runs + canonical Huffman nonzeros
# (Huffman where it helps - the nonzero dictionary index - and a table-free
#  universal code for run lengths, avoiding the run-table overhead of (b).)
# ===========================================================================
def write_gamma(bw, n):
    m = n + 1
    k = m.bit_length()
    for _ in range(k - 1):
        bw.write_bits(0, 1)
    bw.write_bits(m, k)


def read_gamma(br):
    k = 0
    while br.read_bit() == 0:
        k += 1
    m = (1 << k) | (br_read_bits(br, k))
    return m - 1


def br_read_bits(br, count):
    v = 0
    for _ in range(count):
        v = (v << 1) | br.read_bit()
    return v


def encode_rle_gamma(values):
    runs, nz, final_run = _runs_and_syms(values)
    cnz = Counter(nz)
    nz_syms = [v for v, _ in cnz.most_common()]
    K = len(nz_syms)
    nz_index = {v: i for i, v in enumerate(nz_syms)}
    END = K
    nz_freqs = [cnz[s] for s in nz_syms] + [1]
    nz_lengths = limit_code_lengths(nz_freqs, MAXLEN)
    nz_codes = canonical_codes(nz_lengths)

    out = bytearray()
    out.append(K)
    for v in nz_syms:
        varint_encode(out, zz_encode(v))
    pack_nibbles(nz_lengths, out)           # K+1 entries
    n = len(values)
    out.append(n & 0xFF)
    out.append((n >> 8) & 0xFF)

    bw = BitWriter()
    for r, v in zip(runs, nz):
        write_gamma(bw, r)
        i = nz_index[v]
        bw.write_bits(nz_codes[i], nz_lengths[i])
    write_gamma(bw, final_run)
    bw.write_bits(nz_codes[END], nz_lengths[END])
    return bytes(out) + bw.finish()


def decode_rle_gamma(data):
    pos = 0
    K = data[pos]; pos += 1
    nz_syms = []
    for _ in range(K):
        z, pos = varint_decode(data, pos)
        nz_syms.append(zz_decode(z))
    nz_lengths, pos = unpack_nibbles(data, pos, K + 1)
    n = data[pos] | (data[pos + 1] << 8); pos += 2

    nfc, nfi, nblc, norder, nmax = build_decoder_tables(nz_lengths)
    END = K
    br = BitReader(data[pos:])
    out = []
    while len(out) < n:
        run = read_gamma(br)
        if run:
            out.extend([0] * run)
        sym = decode_symbol(br, nfc, nblc, nfi, norder, nmax)
        if sym == END:
            break
        out.append(nz_syms[sym])
    return out


def rle_gamma_model_size(values):
    runs, nz, final_run = _runs_and_syms(values)
    cnz = Counter(nz)
    K = len(cnz)
    m = 1                                    # u8 K
    tmp = bytearray()
    for v in cnz:
        varint_encode(tmp, zz_encode(v))
    m += len(tmp)                            # nonzero values
    m += (K + 1 + 1) // 2                     # nz code lengths (K+1)
    m += 2                                    # u16 N
    return m


# ---------------------------------------------------------------------------
# Model-size breakdown helpers (for reporting)
# ---------------------------------------------------------------------------
def plain_model_size(values):
    c = Counter(values)
    K = len(c)
    m = 1                                   # u8 K
    tmp = bytearray()
    for v in c:
        varint_encode(tmp, zz_encode(v))
    m += len(tmp)                           # values
    m += (K + 1) // 2                       # nibble code lengths
    m += 2                                  # u16 N
    return m


def rle_model_size(values):
    runs, nz, final_run = _runs_and_syms(values)
    cnz = Counter(nz)
    K = len(cnz)
    all_runs = runs + [final_run]
    max_run = max(all_runs) if all_runs else 0
    R = min(max_run + 2, 16)
    m = 1                                    # u8 K
    tmp = bytearray()
    for v in cnz:
        varint_encode(tmp, zz_encode(v))
    m += len(tmp)                            # nonzero values
    m += 1                                   # u8 R
    m += (K + 1 + 1) // 2                     # nz code lengths (K+1)
    m += (R + 1) // 2                         # run code lengths
    m += 2                                    # u16 N
    return m


# ---------------------------------------------------------------------------
# Bench / verify
# ---------------------------------------------------------------------------
def main():
    here = os.path.dirname(os.path.abspath(__file__))
    d = json.load(open(os.path.join(here, "..", "reloc_values.json")))
    bl, ldr = d["bl"], d["ldr"]
    HEADERS = 47

    print("=" * 70)
    print("CANONICAL HUFFMAN CODEC  (separate models, MAXLEN=%d)" % MAXLEN)
    print("=" * 70)

    # ---- round-trip both variants on bl, ldr, combined ----
    allok = True
    results = {}
    for label, enc, dec in [
        ("(a) plain order-0 Huffman (zero as symbol)", encode_plain, decode_plain),
        ("(b) zero-RLE(Huffman runs) + Huffman nonzeros", encode_rle_huff, decode_rle_huff),
        ("(b2) zero-RLE(Elias-gamma runs) + Huffman nonzeros", encode_rle_gamma, decode_rle_gamma),
    ]:
        bl_e = enc(bl)
        ldr_e = enc(ldr)
        bl_ok = dec(bl_e) == bl
        ldr_ok = dec(ldr_e) == ldr
        comb_ok = (dec(bl_e) + dec(ldr_e)) == (bl + ldr)
        allok = allok and bl_ok and ldr_ok and comb_ok
        results[label] = (bl_e, ldr_e, bl_ok, ldr_ok, comb_ok)
        print("\n--- %s ---" % label)
        print("  round-trip  bl=%s  ldr=%s  combined=%s"
              % ("PASS" if bl_ok else "FAIL",
                 "PASS" if ldr_ok else "FAIL",
                 "PASS" if comb_ok else "FAIL"))

    # ---- edge cases ----
    print("\n--- edge cases (variant a & b) ---")
    edges = {
        "empty": [],
        "all-zero(50)": [0] * 50,
        "single-nonzero": [-168],
        "no-zeros": [-168, -336, 168, -360, -8],
        "single-value-repeat": [-336] * 17,
        "alternating": [0, -168, 0, -168, 0, -168],
        "trailing-zeros": [-168, 0, 0, 0],
        "leading-zeros": [0, 0, 0, -168],
    }
    for name, v in edges.items():
        oa = decode_plain(encode_plain(v)) == v
        ob = decode_rle_huff(encode_rle_huff(v)) == v
        ob2 = decode_rle_gamma(encode_rle_gamma(v)) == v
        allok = allok and oa and ob and ob2
        print("  %-22s a=%s b=%s b2=%s"
              % (name, "PASS" if oa else "FAIL",
                 "PASS" if ob else "FAIL", "PASS" if ob2 else "FAIL"))

    # ---- measured byte breakdown ----
    def report(label, enc, model_fn):
        bl_e = enc(bl)
        ldr_e = enc(ldr)
        bl_m = model_fn(bl)
        ldr_m = model_fn(ldr)
        bl_bits = len(bl_e) - bl_m
        ldr_bits = len(ldr_e) - ldr_m
        val_sum = len(bl_e) + len(ldr_e)
        total = val_sum + HEADERS
        print("\n=== %s ===" % label)
        print("  bl  : %4d B  (tables %2d B, bitstream %4d B)" % (len(bl_e), bl_m, bl_bits))
        print("  ldr : %4d B  (tables %2d B, bitstream %4d B)" % (len(ldr_e), ldr_m, ldr_bits))
        print("  shipped code-length tables total : %d B" % (bl_m + ldr_m))
        print("  value-stream sum (bl+ldr)        : %4d B" % val_sum)
        print("  + dfpatch headers                : %4d B" % HEADERS)
        print("  TOTAL                            : %4d B" % total)
        print("    beats 833 (full heatshrink)?   %s (margin %+d B)"
              % ("YES" if total < 833 else "NO", 833 - total))
        print("    beats 598 (RLE sibling)?       %s (margin %+d B)"
              % ("YES" if total < 598 else "NO", 598 - total))
        print("    beats 784 (heatshrink values)? %s (vs val-sum %d)"
              % ("YES" if val_sum < 784 else "NO", val_sum))
        print("    beats 590 (separate floor)?    %s (vs val-sum %d)"
              % ("YES" if val_sum < 590 else "NO", val_sum))
        return total, val_sum

    ta, va = report("VARIANT (a) plain order-0 Huffman (zero as symbol)",
                     encode_plain, plain_model_size)
    tb, vb = report("VARIANT (b) zero-RLE (Huffman runs) + Huffman nonzeros",
                     encode_rle_huff, rle_model_size)
    tb2, vb2 = report("VARIANT (b2) zero-RLE (Elias-gamma runs) + Huffman nonzeros",
                      encode_rle_gamma, rle_gamma_model_size)

    print("\n" + "=" * 70)
    print("SUMMARY")
    print("=" * 70)
    print("  variant (a)  plain Huffman          TOTAL %d B  (value-sum %d)" % (ta, va))
    print("  variant (b)  RLE(Huffman runs)      TOTAL %d B  (value-sum %d)" % (tb, vb))
    print("  variant (b2) RLE(gamma runs)+Huff   TOTAL %d B  (value-sum %d)" % (tb2, vb2))
    print("  baselines: heatshrink-full 833, RLE-sibling 598,")
    print("             heatshrink-values 784, floor-sep 590, floor-comb 720")
    print("  lossless (all round-trips + edges): %s" % ("YES" if allok else "NO"))


if __name__ == "__main__":
    main()
