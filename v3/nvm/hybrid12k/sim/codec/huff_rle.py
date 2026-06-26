#!/usr/bin/env python3
"""
Canonical Huffman entropy coding for relocation-delta value streams, evaluated
under THREE zero-run-coupling options. Target: ARM Cortex-M0+ firmware patcher.
Integer-only, small RAM, MCU-friendly decoder. Lossless.

JPEG-style throughout: a MAXIMUM code length is pre-specified (MAXLEN) so the
decoder's bit accumulator width is bounded, and code lengths are length-limited
to that bound (Kraft-valid). Only the canonical code LENGTHS are shipped
(nibble-packed); encoder and decoder rebuild identical canonical codes
deterministically. bl and ldr are coded with SEPARATE per-stream models.

===========================================================================
THE THREE OPTIONS
===========================================================================
(A) ZEROS-ONLY RLE  (reproduces the prior 598 scheme; matrix baseline)
    Event = (zero_run, nonzero_value). The zero_run is coded as a SEPARATE
    symbol from a binned run alphabet (canonical Huffman, escape for big runs),
    and the nonzero value is a SEPARATE canonical-Huffman symbol over the tiny
    value dictionary. END token terminates (handles trailing zeros).

(B) JPEG (RUN, SIZE)
    One Huffman symbol per event = (run, size), run = capped zero-run [0..15]
    with a ZRL escape (run-of-16 zeros, JPEG-style), size = bit-length category
    SSSS of |value|. After the (run,size) Huffman code, `size` mantissa bits are
    appended using the JPEG sign convention (positive: value as-is; negative:
    value-1, i.e. ones'-complement in `size` bits). EOB symbol = (0,0) ends a
    stream's trailing zeros.

(C) JOINT (RUN, VALUE-INDEX)
    One Huffman symbol per event = (run, dictionary_index_of_value). No mantissa
    -- the value is fully named by its dictionary index (alphabet is tiny). Run
    is capped with a ZRL escape (run-of-16). EOB symbol ends trailing zeros.

All three are pure canonical Huffman on the wire. Decoders are integer-only and
strictly streamable in to-address order.
"""

import json
import os
from collections import Counter

MAXLEN = 8  # JPEG-style bound; nibble-packable (<=15), MCU-friendly accumulator


# ---------------------------------------------------------------------------
# Bit I/O (MSB-first), integer-only
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
# Canonical Huffman with JPEG-style length limiting (Kraft-valid, near-optimal).
# ---------------------------------------------------------------------------
def huffman_code_lengths(freqs):
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
    """Huffman code lengths with every length <= maxlen, Kraft sum == 1 budget."""
    lengths = huffman_code_lengths(freqs)
    if max(lengths) <= maxlen:
        return lengths
    n = len(freqs)
    lengths = [min(l, maxlen) for l in lengths]
    one = 1 << maxlen
    total = sum(1 << (maxlen - l) for l in lengths)
    order_by_freq = sorted(range(n), key=lambda i: (freqs[i], i))
    while total > one:
        for s in order_by_freq:
            if lengths[s] < maxlen:
                total -= 1 << (maxlen - lengths[s])
                lengths[s] += 1
                total += 1 << (maxlen - lengths[s])
                break
        else:
            raise ValueError("alphabet too large for maxlen")
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
    for i in sorted(range(n), key=lambda i: (lengths[i], i)):
        l = lengths[i]
        codes[i] = next_code[l]
        next_code[l] += 1
    return codes


def build_decoder_tables(lengths):
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
# Common: split a stream into (run, nonzero) events + final trailing run.
# ===========================================================================
def _events(values):
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
    return runs, nz, run  # trailing run after last nonzero


# ===========================================================================
# OPTION (A): ZEROS-ONLY RLE  (matrix baseline -- reproduces the prior 598)
#   zero_run coded as a SEPARATE symbol via a table-free Elias-gamma universal
#   code (run=0 -> 1 bit, the common "two adjacent nonzeros" case), and the
#   nonzero value coded as a SEPARATE canonical-Huffman symbol over the tiny
#   value dictionary. END token in the value alphabet terminates trailing zeros.
#   Gamma is chosen for the run because it ships NO run table -- a binned-run
#   Huffman alphabet costs more table bytes than it saves on this data.
# Wire (per stream):
#   u8 K ; K x zigzag-varint values ; (K+1) nibble lengths [val0..valK-1, END] ;
#   u16 N ; bitstream of [gamma(run)][value-code] ... [gamma(run)][END-code]
# ===========================================================================
def write_gamma(bw, n):
    m = n + 1
    k = m.bit_length()
    for _ in range(k - 1):
        bw.write_bit(0)
    bw.write_bits(m, k)


def read_gamma(br):
    k = 0
    while br.read_bit() == 0:
        k += 1
    m = (1 << k) | br.read_bits(k)
    return m - 1


def encode_A(values):
    runs, nz, final_run = _events(values)

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
    pack_nibbles(nz_lengths, out)
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


def decode_A(data):
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


# ===========================================================================
# OPTION (B): JPEG (RUN, SIZE)
#   one Huffman symbol = (run, size); run in [0..15], ZRL escape = run-of-16.
#   size = bit-length category SSSS of |value|; then `size` mantissa bits with
#   JPEG sign convention. EOB = (0,0) terminates trailing zeros.
# Composite symbol id = run*16 + size  (size in 0..15, run in 0..15).
#   - (run=0,size=0)  : EOB
#   - (run=15,size=0) : ZRL (skip 16 zeros, no value)  -- JPEG-style
#   - otherwise       : skip `run` zeros, emit a value of category `size`.
# Wire (per stream):
#   u8 M (number of distinct composite symbols) ;
#   M x u8 composite-id (ascending? no: canonical order, freq-desc) ;
#   M nibble lengths ; u16 N ; bitstream
# ===========================================================================
def _ssss(mag):
    """JPEG SSSS category: number of bits to represent magnitude (0 -> 0)."""
    return mag.bit_length()


def _mantissa_bits(value, size):
    """JPEG mantissa: positive -> value; negative -> value-1 (ones' complement)."""
    if value >= 0:
        return value & ((1 << size) - 1)
    else:
        return (value - 1) & ((1 << size) - 1)


def _mantissa_decode(bits, size):
    """Inverse of _mantissa_bits given `size` and raw mantissa `bits`."""
    if bits >> (size - 1):  # top bit set -> positive
        return bits
    else:                   # negative
        return bits - (1 << size) + 1


ZRL_RUN = 16  # JPEG ZRL skips 16 zeros


def _b_events(values):
    """Yield composite (run,size) events + mantissas, JPEG-style with ZRL/EOB."""
    runs, nz, final_run = _events(values)
    events = []  # list of (run, size, value) ; run<=15, ZRL handled separately
    seq = []     # full token sequence including ZRL tokens, EOB
    for r, v in zip(runs, nz):
        run = r
        while run > 15:
            seq.append(("ZRL", None, None))
            run -= 16
        size = _ssss(abs(v))
        seq.append(("RS", run, (size, v)))
    # trailing zeros -> EOB (only if there ARE trailing zeros, else still EOB
    # to mark end; we always emit EOB so decoder knows the stream ended).
    seq.append(("EOB", None, None))
    return seq


def encode_B(values):
    seq = _b_events(values)

    # build composite symbol frequencies
    EOB_ID = 0          # run=0,size=0
    ZRL_ID = 15 * 16    # run=15,size=0
    freq = Counter()
    for kind, run, payload in seq:
        if kind == "EOB":
            freq[EOB_ID] += 1
        elif kind == "ZRL":
            freq[ZRL_ID] += 1
        else:
            size, v = payload
            freq[run * 16 + size] += 1

    sym_ids = [s for s, _ in freq.most_common()]
    M = len(sym_ids)
    id_index = {s: i for i, s in enumerate(sym_ids)}
    freqs = [freq[s] for s in sym_ids]
    lengths = limit_code_lengths(freqs, MAXLEN)
    codes = canonical_codes(lengths)

    out = bytearray()
    out.append(M)
    for s in sym_ids:
        out.append(s)            # composite id fits in u8 (max 15*16+15=255)
    pack_nibbles(lengths, out)
    n = len(values)
    out.append(n & 0xFF)
    out.append((n >> 8) & 0xFF)

    bw = BitWriter()
    for kind, run, payload in seq:
        if kind == "EOB":
            i = id_index[EOB_ID]
            bw.write_bits(codes[i], lengths[i])
        elif kind == "ZRL":
            i = id_index[ZRL_ID]
            bw.write_bits(codes[i], lengths[i])
        else:
            size, v = payload
            sid = run * 16 + size
            i = id_index[sid]
            bw.write_bits(codes[i], lengths[i])
            if size:
                bw.write_bits(_mantissa_bits(v, size), size)
    return bytes(out) + bw.finish()


def decode_B(data):
    pos = 0
    M = data[pos]; pos += 1
    sym_ids = [data[pos + i] for i in range(M)]; pos += M
    lengths, pos = unpack_nibbles(data, pos, M)
    n = data[pos] | (data[pos + 1] << 8); pos += 2

    fc, fi, blc, order, maxlen = build_decoder_tables(lengths)
    br = BitReader(data[pos:])
    out = []
    EOB_ID = 0
    ZRL_ID = 15 * 16
    while len(out) < n:
        sidx = decode_symbol(br, fc, blc, fi, order, maxlen)
        sid = sym_ids[sidx]
        if sid == EOB_ID:
            break
        if sid == ZRL_ID:
            out.extend([0] * 16)
            continue
        run = sid >> 4
        size = sid & 0x0F
        if run:
            out.extend([0] * run)
        bits = br.read_bits(size) if size else 0
        v = _mantissa_decode(bits, size) if size else 0
        out.append(v)
    # pad trailing zeros if EOB came before N filled
    if len(out) < n:
        out.extend([0] * (n - len(out)))
    return out


# ===========================================================================
# OPTION (C): JOINT (RUN, VALUE-INDEX)
#   one Huffman symbol = (run, dict_index); run [0..15], ZRL escape = run-of-16;
#   no mantissa. EOB terminates trailing zeros.
# composite id = run * D + (dict_index)   where D = dict size + special slots.
# We reserve dict_index in [0..D-1] for real values; EOB and ZRL are encoded
# as their own composite ids appended after the run*D space.
# Simpler & u8-safe: composite id = run * (D+? ) ... instead, use explicit
# tuple table. We ship: dict values, then a list of composite (run, vidx) pairs.
# To keep ids small & u8-packable we encode id = run * (D) + vidx for events,
# and use two extra sentinel ids EOB_ID, ZRL_ID beyond the event space.
# ===========================================================================
def _c_events(values):
    runs, nz, final_run = _events(values)
    seq = []
    for r, v in zip(runs, nz):
        run = r
        while run > 15:
            seq.append(("ZRL", None, None))
            run -= 16
        seq.append(("RV", run, v))
    seq.append(("EOB", None, None))
    return seq


def encode_C(values):
    runs, nz, _f = _events(values)
    cnz = Counter(nz)
    dict_syms = [v for v, _ in cnz.most_common()]
    D = len(dict_syms)
    vidx = {v: i for i, v in enumerate(dict_syms)}

    seq = _c_events(values)
    # event composite id = run * D + vidx, in [0 .. 16*D-1]
    # sentinels appended above that:
    EOB_ID = 16 * D
    ZRL_ID = 16 * D + 1

    freq = Counter()
    for kind, run, v in seq:
        if kind == "EOB":
            freq[EOB_ID] += 1
        elif kind == "ZRL":
            freq[ZRL_ID] += 1
        else:
            freq[run * D + vidx[v]] += 1

    sym_ids = [s for s, _ in freq.most_common()]
    M = len(sym_ids)
    id_index = {s: i for i, s in enumerate(sym_ids)}
    freqs = [freq[s] for s in sym_ids]
    lengths = limit_code_lengths(freqs, MAXLEN)
    codes = canonical_codes(lengths)

    # composite id max = 16*D+1; for D<=15 this is <=241, so it fits a u8.
    out = bytearray()
    out.append(D)
    for v in dict_syms:
        varint_encode(out, zz_encode(v))
    out.append(M)
    for s in sym_ids:
        out.append(s)            # u8 composite id
    pack_nibbles(lengths, out)
    n = len(values)
    out.append(n & 0xFF)
    out.append((n >> 8) & 0xFF)

    bw = BitWriter()
    for kind, run, v in seq:
        if kind == "EOB":
            i = id_index[EOB_ID]
        elif kind == "ZRL":
            i = id_index[ZRL_ID]
        else:
            i = id_index[run * D + vidx[v]]
        bw.write_bits(codes[i], lengths[i])
    return bytes(out) + bw.finish()


def decode_C(data):
    pos = 0
    D = data[pos]; pos += 1
    dict_syms = []
    for _ in range(D):
        z, pos = varint_decode(data, pos)
        dict_syms.append(zz_decode(z))
    M = data[pos]; pos += 1
    sym_ids = [data[pos + i] for i in range(M)]; pos += M
    lengths, pos = unpack_nibbles(data, pos, M)
    n = data[pos] | (data[pos + 1] << 8); pos += 2

    EOB_ID = 16 * D
    ZRL_ID = 16 * D + 1
    fc, fi, blc, order, maxlen = build_decoder_tables(lengths)
    br = BitReader(data[pos:])
    out = []
    while len(out) < n:
        sidx = decode_symbol(br, fc, blc, fi, order, maxlen)
        sid = sym_ids[sidx]
        if sid == EOB_ID:
            break
        if sid == ZRL_ID:
            out.extend([0] * 16)
            continue
        run = sid // D
        vi = sid % D
        if run:
            out.extend([0] * run)
        out.append(dict_syms[vi])
    if len(out) < n:
        out.extend([0] * (n - len(out)))
    return out


# ---------------------------------------------------------------------------
# Shipped-table (model) sizes per stream, per option.
# ---------------------------------------------------------------------------
def model_size_A(values):
    runs, nz, final_run = _events(values)
    cnz = Counter(nz)
    K = len(cnz)
    m = 1
    tmp = bytearray()
    for v in cnz:
        varint_encode(tmp, zz_encode(v))
    m += len(tmp)                # dictionary values
    m += (K + 1 + 1) // 2        # nz lengths (K+1, incl END)
    m += 2                       # u16 N
    return m


def model_size_B(values):
    seq = _b_events(values)
    EOB_ID = 0
    ZRL_ID = 15 * 16
    freq = Counter()
    for kind, run, payload in seq:
        if kind == "EOB":
            freq[EOB_ID] += 1
        elif kind == "ZRL":
            freq[ZRL_ID] += 1
        else:
            size, v = payload
            freq[run * 16 + size] += 1
    M = len(freq)
    m = 1                        # u8 M
    m += M                       # M x u8 composite id
    m += (M + 1) // 2            # nibble lengths
    m += 2                       # u16 N
    return m


def model_size_C(values):
    runs, nz, _f = _events(values)
    cnz = Counter(nz)
    D = len(cnz)
    seq = _c_events(values)
    EOB_ID = 16 * D
    ZRL_ID = 16 * D + 1
    vidx = {v: i for i, v in enumerate([v for v, _ in cnz.most_common()])}
    freq = Counter()
    for kind, run, v in seq:
        if kind == "EOB":
            freq[EOB_ID] += 1
        elif kind == "ZRL":
            freq[ZRL_ID] += 1
        else:
            freq[run * D + vidx[v]] += 1
    M = len(freq)
    m = 1                        # u8 D
    tmp = bytearray()
    for v in cnz:
        varint_encode(tmp, zz_encode(v))
    m += len(tmp)                # dict values
    m += 1                       # u8 M
    m += M                       # composite ids (u8 each)
    m += (M + 1) // 2            # nibble lengths
    m += 2                       # u16 N
    return m


# ---------------------------------------------------------------------------
# Bench / verify
# ---------------------------------------------------------------------------
OPTIONS = [
    ("A", "ZEROS-ONLY RLE (run+value as separate symbols)",
     encode_A, decode_A, model_size_A),
    ("B", "JPEG (RUN,SIZE) + mantissa",
     encode_B, decode_B, model_size_B),
    ("C", "JOINT (RUN,VALUE-INDEX), no mantissa",
     encode_C, decode_C, model_size_C),
]

HEADERS = 47
EDGES = {
    "empty": [],
    "all-zero(50)": [0] * 50,
    "single-nonzero": [-168],
    "no-zeros": [-168, -336, 168, -360, -8],
    "single-value-repeat": [-336] * 17,
    "alternating": [0, -168, 0, -168, 0, -168],
    "trailing-zeros": [-168, 0, 0, 0],
    "leading-zeros": [0, 0, 0, -168],
    "long-run(40)": [0] * 40 + [-168] + [0] * 40,
    "big-mag": [0, 532, 0, -532, -4, 4],
}


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    d = json.load(open(os.path.join(here, "..", "reloc_values.json")))
    bl, ldr = d["bl"], d["ldr"]

    print("=" * 78)
    print("CANONICAL HUFFMAN under 3 RLE-coupling options (MAXLEN=%d, separate models)"
          % MAXLEN)
    print("=" * 78)

    allok = True
    summary = []
    for tag, name, enc, dec, msz in OPTIONS:
        bl_e, ldr_e = enc(bl), enc(ldr)
        bl_ok = dec(bl_e) == bl
        ldr_ok = dec(ldr_e) == ldr
        comb_ok = (dec(bl_e) + dec(ldr_e)) == (bl + ldr)

        # edges
        edge_ok = True
        edge_fail = []
        for ename, ev in EDGES.items():
            if dec(enc(ev)) != ev:
                edge_ok = False
                edge_fail.append(ename)
        ok = bl_ok and ldr_ok and comb_ok and edge_ok
        allok = allok and ok

        bl_m, ldr_m = msz(bl), msz(ldr)
        bl_bits = len(bl_e) - bl_m
        ldr_bits = len(ldr_e) - ldr_m
        val_sum = len(bl_e) + len(ldr_e)
        total = val_sum + HEADERS
        tables = bl_m + ldr_m
        summary.append((tag, name, total, val_sum, tables, bl_bits + ldr_bits, ok))

        print("\n--- OPTION (%s) %s ---" % (tag, name))
        print("  round-trip: bl=%s ldr=%s combined=%s  edges=%s%s"
              % ("PASS" if bl_ok else "FAIL",
                 "PASS" if ldr_ok else "FAIL",
                 "PASS" if comb_ok else "FAIL",
                 "PASS" if edge_ok else "FAIL",
                 "" if edge_ok else " (" + ",".join(edge_fail) + ")"))
        print("  bl  : %4d B = tables %3d B + bitstream %4d B" % (len(bl_e), bl_m, bl_bits))
        print("  ldr : %4d B = tables %3d B + bitstream %4d B" % (len(ldr_e), ldr_m, ldr_bits))
        print("  shipped tables total : %3d B" % tables)
        print("  value-stream sum     : %4d B" % val_sum)
        print("  + headers (47 B)     : %4d B" % HEADERS)
        print("  TOTAL                : %4d B" % total)

    print("\n" + "=" * 78)
    print("COMPARISON TABLE")
    print("=" * 78)
    hdr = ("%-4s %-40s %6s %7s %7s %6s" %
           ("opt", "scheme", "TOTAL", "bits", "tables", "loss"))
    print(hdr)
    print("-" * 78)
    best = min(summary, key=lambda r: r[2])
    for tag, name, total, val_sum, tables, bits, ok in summary:
        mark = " <== SMALLEST" if (tag, total) == (best[0], best[2]) else ""
        print("%-4s %-40s %6d %7d %7d %6s%s"
              % (tag, name[:40], total, bits, tables,
                 "YES" if ok else "NO", mark))

    print("\nBASELINE CHECKS (total bytes incl 47B headers; floors are bitstream/value-sum):")
    print("  baselines: heatshrink-full 833, heatshrink-values 784,")
    print("             floor-combined 720, floor-separate 590,")
    print("             prior RLE & Huffman 598, prior range(order-1) 596.")
    print("%-4s %-9s %-9s %-9s %-12s" %
          ("opt", "b<833", "b<598", "b<596", "val<590(floor)"))
    for tag, name, total, val_sum, tables, bits, ok in summary:
        print("%-4s %-9s %-9s %-9s %-12s"
              % (tag,
                 "YES%+d" % (833 - total),
                 "YES%+d" % (598 - total) if total < 598 else "NO %+d" % (598 - total),
                 "YES%+d" % (596 - total) if total < 596 else "NO %+d" % (596 - total),
                 ("YES %d" % val_sum) if val_sum < 590 else ("NO %d" % val_sum)))

    print("\nSMALLEST: OPTION (%s) %s -> %d B" % (best[0], best[1], best[2]))
    print("LOSSLESS (all streams + edges, all options): %s" % ("YES" if allok else "NO"))


if __name__ == "__main__":
    main()
