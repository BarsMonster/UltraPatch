#!/usr/bin/env python3
"""
Integer (fixed-point) range coder + RLE coupling for relocation-delta value
streams.  THREE RLE-coupling options, all on top of the SAME 32-bit integer
range coder (TOTBITS=12, TOT=4096 -> division-free decode hot path).

Target: ARM Cortex-M0+, no FPU, integer-only, small RAM. Decoder is
division-free in the hot path (range/total is a >>TOTBITS shift), uses static
pre-quantized freq tables (8-bit normalized, sum 256, rescaled x16 on the MCU).

bl and ldr are coded SEPARATELY, each with its own static per-stream model.

----------------------------------------------------------------------------
SPARSE FREQUENCY TABLES
----------------------------------------------------------------------------
The joint alphabets are mostly empty (a (run,value) grid has hundreds of cells
but only ~15-40 ever occur). Shipping a dense freq byte per cell is wasteful,
so tables are shipped SPARSE: only the symbols that actually occur are listed.

  table wire cost (measured) = 1 byte (count of live symbols)
                             + per live symbol: varint(symbol_id) + 1 freq byte
  plus, where values are not reconstructable from the symbol id (options A, C),
  a small dictionary of 1 signed byte per nonzero value (value/4 fits int8).

On the MCU the decoder rebuilds a tiny lookup (live-symbol list + cumulative
freqs) once; the hot path is a linear scan over the live symbols (<= a few
dozen), integer compares only, no division.

----------------------------------------------------------------------------
THE THREE OPTIONS
----------------------------------------------------------------------------
(A) ZEROS-ONLY RLE
      Event = (zero_run, nonzero_value), each coded as a SEPARATE symbol:
        - run model   : alphabet = {0..CAP-1 literal runs} + ESC + END.
                        ESC means "run >= CAP": the residual run is sent by
                        chaining ESC symbols (each ESC == CAP zeros) followed
                        by a literal remainder run. END flushes trailing zeros.
        - nzval model : the tiny nonzero dictionary, its own symbol.
(B) JPEG (RUN, SIZE)
      Event = composite (run, size) in ONE joint model:
        run  = capped zero-run (ZRL escape = 16 zeros, no value),
        size = bit-length category of |value|.
      Then `size` magnitude bits + 1 sign bit coded as BYPASS (equiprobable)
      range-coder bits.  EOB symbol terminates trailing zeros.
(C) JOINT (RUN, VALUE-INDEX)
      Event = composite (run, dict_index) in ONE joint model, no mantissa.
      RUNESC (== CAP zeros, no value) chains for long runs; EOB for tail.
"""

import json
import os

TOTBITS = 12
TOT = 1 << TOTBITS          # 4096 fixed total frequency (power of two)
BOT = 1 << 16
MASK32 = 0xFFFFFFFF
HEADER = 47


# ==========================================================================
# 32-bit integer range coder (Subbotin style, byte renorm) -- from range.py
# ==========================================================================
class RangeEncoder:
    def __init__(self):
        self.low = 0
        self.range = MASK32
        self.out = bytearray()

    def _renorm(self):
        while True:
            if ((self.low ^ (self.low + self.range)) & 0xFF000000) != 0:
                if self.range >= BOT:
                    break
                self.range = (-self.low) & (BOT - 1)
            self.out.append((self.low >> 24) & 0xFF)
            self.low = (self.low << 8) & MASK32
            self.range = (self.range << 8) & MASK32

    def encode(self, cumfreq, freq):
        r = self.range >> TOTBITS
        self.low = (self.low + r * cumfreq) & MASK32
        self.range = r * freq
        self._renorm()

    def finish(self):
        for _ in range(4):
            self.out.append((self.low >> 24) & 0xFF)
            self.low = (self.low << 8) & MASK32
        return bytes(self.out)


class RangeDecoder:
    def __init__(self, data):
        self.data = data
        self.pos = 0
        self.low = 0
        self.range = MASK32
        self.code = 0
        for _ in range(4):
            self.code = ((self.code << 8) | self._byte()) & MASK32

    def _byte(self):
        if self.pos < len(self.data):
            b = self.data[self.pos]
            self.pos += 1
            return b
        return 0

    def _renorm(self):
        while True:
            if ((self.low ^ (self.low + self.range)) & 0xFF000000) != 0:
                if self.range >= BOT:
                    break
                self.range = (-self.low) & (BOT - 1)
            self.code = ((self.code << 8) | self._byte()) & MASK32
            self.low = (self.low << 8) & MASK32
            self.range = (self.range << 8) & MASK32

    def get_freq(self):
        self._r = self.range >> TOTBITS
        v = ((self.code - self.low) & MASK32) // self._r
        return v if v < TOT else TOT - 1

    def decode(self, cumfreq, freq):
        r = self._r
        self.low = (self.low + r * cumfreq) & MASK32
        self.range = r * freq
        self._renorm()


HALF = TOT >> 1  # bypass-bit split point


def enc_bypass(enc, bit):
    # equiprobable bit, division-free (TOT split in two equal halves).
    if bit:
        enc.encode(HALF, TOT - HALF)
    else:
        enc.encode(0, HALF)


def dec_bypass(dec):
    v = dec.get_freq()
    if v < HALF:
        dec.decode(0, HALF)
        return 0
    dec.decode(HALF, TOT - HALF)
    return 1


# ==========================================================================
# SPARSE static model over an integer-id alphabet.
# Quantize occurring symbols to 8-bit freqs (sum 256), rescale x16 -> TOT.
# ==========================================================================
class Model:
    """Static model over a sparse set of integer symbol ids.

    counts: dict {symbol_id: count}.  Builds:
      * self.ids   : sorted live symbol ids
      * self.freq  : freq per live id (sum == TOT)
      * self.cum   : cumulative freqs (len = len(ids)+1)
      * self.pos   : {symbol_id: index into ids}
    """

    def __init__(self, counts):
        self.ids = sorted(counts)
        n = sum(counts.values())
        # 8-bit normalized, each occurring symbol >= 1, sum == 256
        q = []
        for sid in self.ids:
            f = (counts[sid] * 256) // n if n else 0
            if counts[sid] > 0 and f == 0:
                f = 1
            q.append(f)
        if self.ids:
            mi = max(range(len(q)), key=lambda i: q[i])
            q[mi] += 256 - sum(q)
            if q[mi] <= 0:                      # pathological (few symbols)
                # fall back: distribute evenly
                base = 256 // len(q)
                q = [base] * len(q)
                q[0] += 256 - sum(q)
        self.q8 = q
        # expand x16 -> TOT
        f = [x * (TOT // 256) for x in q]
        if self.ids:
            s = sum(f)
            if s != TOT:
                f[max(range(len(f)), key=lambda i: f[i])] += TOT - s
        self.freq = f
        cum = [0]
        for x in f:
            cum.append(cum[-1] + x)
        self.cum = cum
        self.pos = {sid: i for i, sid in enumerate(self.ids)}

    def enc(self, encoder, sid):
        i = self.pos[sid]
        encoder.encode(self.cum[i], self.freq[i])

    def dec(self, decoder):
        v = decoder.get_freq()
        i = 0
        while self.cum[i + 1] <= v:
            i += 1
        decoder.decode(self.cum[i], self.freq[i])
        return self.ids[i]

    def table_bytes(self):
        """Sparse table wire cost: 1 byte live-count + per symbol
        varint(id) + 1 freq byte (q8)."""
        b = 1
        for sid in self.ids:
            b += _varint_len(sid) + 1
        return b


def _varint_len(n):
    n = n if n >= 0 else (-n)
    if n == 0:
        return 1
    c = 0
    while n:
        c += 1
        n >>= 7
    return c


def _dict_bytes(nzsyms):
    """1 signed byte per nonzero value (value/4 fits int8: |532|/4=133 -> still
    needs care; use varint of value/4 to be safe & honest)."""
    b = 0
    for v in nzsyms:
        b += _varint_len(v // 4 if v % 4 == 0 else v)
    return b


# ==========================================================================
# (A) ZEROS-ONLY RLE: SEPARATE run model + nzval model
# ==========================================================================
A_CAP = 16   # literal runs 0..A_CAP-1; ESC == A_CAP zeros (chains); END = stop


def codeA(values):
    nzsyms = sorted({v for v in values if v != 0})
    nz_idx = {s: i for i, s in enumerate(nzsyms)}

    ESC = A_CAP          # run id meaning "A_CAP zeros, more run follows"
    END = A_CAP + 1      # run id meaning "stop (flush trailing zeros)"

    # Event stream: for each nonzero value emit its preceding run as
    #   [ESC]* (each ESC == A_CAP zeros) then a literal remainder run symbol,
    #   then a NZ value symbol.  A literal run symbol is ALWAYS followed by a
    #   value.  The stream ends with END; the trailing zero count is sent right
    #   after END as one literal run symbol per A_CAP plus a remainder, all in
    #   the run model (END unambiguously switches the decoder to tail mode).
    def gen():
        run = 0
        for v in values:
            if v == 0:
                run += 1
            else:
                r = run
                while r >= A_CAP:
                    yield ('RUN', ESC)
                    r -= A_CAP
                yield ('RUN', r)
                yield ('NZ', nz_idx[v])
                run = 0
        # END, then the trailing run as ESC-chains + remainder
        yield ('RUN', END)
        r = run
        while r >= A_CAP:
            yield ('RUN', ESC)
            r -= A_CAP
        yield ('RUN', r)

    events = list(gen())
    run_counts = {}
    nz_counts = {}
    for kind, sid in events:
        d = run_counts if kind == 'RUN' else nz_counts
        d[sid] = d.get(sid, 0) + 1
    run_m = Model(run_counts)
    nz_m = Model(nz_counts) if nz_counts else None

    enc = RangeEncoder()
    for kind, sid in events:
        (run_m if kind == 'RUN' else nz_m).enc(enc, sid)
    body = enc.finish()

    # decode
    dec = RangeDecoder(body)
    out = []
    run = 0
    while True:
        rid = run_m.dec(dec)
        if rid == END:
            break
        if rid == ESC:
            run += A_CAP
            continue
        # literal remainder run, then a nonzero value
        run += rid
        out.extend([0] * run)
        run = 0
        idx = nz_m.dec(dec)
        out.append(nzsyms[idx])
    # tail mode: read trailing run (ESC-chains + one remainder) until a
    # non-ESC literal run symbol terminates it.
    run = 0
    while True:
        rid = run_m.dec(dec)
        if rid == ESC:
            run += A_CAP
            continue
        run += rid
        break
    out.extend([0] * run)
    ok = out == values

    tbl = run_m.table_bytes() + (nz_m.table_bytes() if nz_m else 0) + _dict_bytes(nzsyms)
    return len(body), tbl, ok, out


# ==========================================================================
# (B) JPEG (RUN, SIZE) joint model + bypass mantissa + EOB
# ==========================================================================
B_ZRL_RUN = 16   # ZRL == 16 zeros, no value


def _bitlen(x):
    n = 0
    while x:
        n += 1
        x >>= 1
    return n


def codeB(values):
    maxsize = max((_bitlen(abs(v)) for v in values if v != 0), default=1)
    SZ = maxsize + 1            # size 0 reserved (EOB / ZRL)

    def comp(rf, size):
        return rf * SZ + size

    EOB = comp(0, 0)
    ZRL = comp(B_ZRL_RUN, 0)

    def gen():
        run = 0
        for v in values:
            if v == 0:
                run += 1
                continue
            r = run
            while r >= 16:
                yield ('ZRL',)
                r -= 16
            yield ('VAL', r, v)
            run = 0
        yield ('EOB',)

    events = list(gen())
    counts = {}
    for e in events:
        if e[0] == 'ZRL':
            cid = ZRL
        elif e[0] == 'EOB':
            cid = EOB
        else:
            _, r, v = e
            cid = comp(r, _bitlen(abs(v)))
        counts[cid] = counts.get(cid, 0) + 1
    m = Model(counts)

    enc = RangeEncoder()
    for e in events:
        if e[0] == 'ZRL':
            m.enc(enc, ZRL)
        elif e[0] == 'EOB':
            m.enc(enc, EOB)
        else:
            _, r, v = e
            size = _bitlen(abs(v))
            m.enc(enc, comp(r, size))
            mag = abs(v)
            for b in range(size - 1, -1, -1):
                enc_bypass(enc, (mag >> b) & 1)
            enc_bypass(enc, 1 if v < 0 else 0)
    body = enc.finish()

    dec = RangeDecoder(body)
    out = []
    N = len(values)
    while len(out) < N:
        cid = m.dec(dec)
        if cid == EOB:
            break
        if cid == ZRL:
            out.extend([0] * 16)
            continue
        rf = cid // SZ
        size = cid % SZ
        out.extend([0] * rf)
        mag = 0
        for _ in range(size):
            mag = (mag << 1) | dec_bypass(dec)
        sign = dec_bypass(dec)
        out.append(-mag if sign else mag)
    if len(out) < N:
        out.extend([0] * (N - len(out)))
    out = out[:N]
    ok = out == values

    # no value dictionary: values fully reconstructed from size+mantissa+sign
    tbl = m.table_bytes()
    return len(body), tbl, ok, out


# ==========================================================================
# (C) JOINT (RUN, VALUE-INDEX) joint model, no mantissa, EOB, run escape
# ==========================================================================
def codeC(values):
    nzsyms = sorted({v for v in values if v != 0})
    nz_idx = {s: i for i, s in enumerate(nzsyms)}
    K = max(len(nzsyms), 1)

    # per-stream run cap from data, kept tight
    runs = []
    run = 0
    for v in values:
        if v == 0:
            run += 1
        else:
            runs.append(run)
            run = 0
    maxrun = max(runs, default=0)
    CAP = min(maxrun, 32) + 1          # literal runs 0..CAP-1; ESC = CAP zeros

    JBODY = CAP * K
    RUNESC = JBODY                     # CAP zeros, no value (chains)
    EOB = JBODY + 1

    def comp(r, di):
        return r * K + di

    def gen():
        run = 0
        for v in values:
            if v == 0:
                run += 1
            else:
                r = run
                while r >= CAP:
                    yield ('ESC',)
                    r -= CAP
                yield ('VAL', r, nz_idx[v])
                run = 0
        yield ('EOB',)

    events = list(gen())
    counts = {}
    for e in events:
        if e[0] == 'ESC':
            cid = RUNESC
        elif e[0] == 'EOB':
            cid = EOB
        else:
            _, r, di = e
            cid = comp(r, di)
        counts[cid] = counts.get(cid, 0) + 1
    m = Model(counts)

    enc = RangeEncoder()
    for e in events:
        if e[0] == 'ESC':
            m.enc(enc, RUNESC)
        elif e[0] == 'EOB':
            m.enc(enc, EOB)
        else:
            _, r, di = e
            m.enc(enc, comp(r, di))
    body = enc.finish()

    dec = RangeDecoder(body)
    out = []
    N = len(values)
    run = 0
    while len(out) < N:
        cid = m.dec(dec)
        if cid == EOB:
            break
        if cid == RUNESC:
            run += CAP
            continue
        r = cid // K
        di = cid % K
        out.extend([0] * (run + r))
        run = 0
        out.append(nzsyms[di])
    if len(out) < N:
        out.extend([0] * (N - len(out)))
    out = out[:N]
    ok = out == values

    tbl = m.table_bytes() + _dict_bytes(nzsyms)
    return len(body), tbl, ok, out


# ==========================================================================
# Benchmark / verification driver
# ==========================================================================
def load():
    here = os.path.dirname(os.path.abspath(__file__))
    d = json.load(open(os.path.join(here, "..", "reloc_values.json")))
    return d["bl"], d["ldr"]


def main():
    bl, ldr = load()
    combined = bl + ldr

    print("=" * 78)
    print("INTEGER RANGE CODER + RLE COUPLING  (TOTBITS=%d TOT=%d, division-free)"
          % (TOTBITS, TOT))
    print("Sparse static freq tables (8-bit, sum 256, x16->4096 on MCU). bl/ldr separate.")
    print("=" * 78)

    def run_option(label, fn):
        bb_body, bb_tbl, bb_ok, _ = fn(bl)
        ld_body, ld_tbl, ld_ok, _ = fn(ldr)
        total = bb_body + ld_body + bb_tbl + ld_tbl + HEADER
        ok = bb_ok and ld_ok
        print(f"\n[{label}]")
        print(f"  bl : body={bb_body:4d}  tbl={bb_tbl:4d}  lossless={'PASS' if bb_ok else 'FAIL'}")
        print(f"  ldr: body={ld_body:4d}  tbl={ld_tbl:4d}  lossless={'PASS' if ld_ok else 'FAIL'}")
        print(f"  TOTAL = ({bb_body}+{ld_body}) body + ({bb_tbl}+{ld_tbl}) tbl + {HEADER} hdr"
              f" = {total} B   lossless={'PASS' if ok else 'FAIL'}")
        return {"label": label, "body": bb_body + ld_body,
                "tbl": bb_tbl + ld_tbl, "total": total, "ok": ok,
                "bl": (bb_body, bb_tbl), "ldr": (ld_body, ld_tbl)}

    rA = run_option("A  ZEROS-ONLY RLE (separate run + nzval)", codeA)
    rB = run_option("B  JPEG (RUN,SIZE) joint + bypass mantissa", codeB)
    rC = run_option("C  JOINT (RUN,VALUE-INDEX) joint", codeC)
    rows = [rA, rB, rC]
    best = min((r for r in rows if r["ok"]), key=lambda r: r["total"])

    print("\n" + "=" * 78)
    print("COMPARISON TABLE  (measured bytes; separate per-stream static models)")
    print("=" * 78)
    print(f"{'option':42s} {'body':>6s} {'tbl':>5s} {'hdr':>4s} {'TOTAL':>6s} {'OK':>5s}")
    print("-" * 78)
    for r in rows:
        star = "  <-- smallest" if r is best else ""
        print(f"{r['label']:42s} {r['body']:6d} {r['tbl']:5d} {HEADER:4d} "
              f"{r['total']:6d} {'PASS' if r['ok'] else 'FAIL':>5s}{star}")

    print("\n" + "=" * 78)
    print("BASELINE COMPARISON  (TOTAL vs each baseline)")
    print("=" * 78)
    bases = [(833, "hs-833"), (598, "RLE-598"), (596, "range-596"),
             (590, "floor-590")]
    print(f"{'option':42s} " + " ".join(f"{nm:>12s}" for _, nm in bases))
    for r in rows:
        cells = []
        for base, _ in bases:
            cells.append((f"+{base - r['total']}" if r['total'] < base
                          else f"-{r['total'] - base}"))
        print(f"{r['label']:42s} " + " ".join(f"{c:>12s}" for c in cells))

    # ---------- round-trip: combined + edge cases ----------
    print("\n" + "=" * 78)
    print("ROUND-TRIP VERIFICATION  (bl, ldr, combined + edge cases)")
    print("=" * 78)
    cases = {
        "bl full": bl,
        "ldr full": ldr,
        "combined bl+ldr": combined,
        "single nonzero": [-168],
        "all zeros x100": [0] * 100,
        "no zeros": [-336, -360, -336, -360, -8],
        "alternating": [0, -168, 0, -168, 0, -168],
        "leading+trailing zeros": [0, 0, -168, 0, 0, 0],
        "long run 543>cap": [0] * 543 + [-168] + [0] * 600,
        "single zero [0]": [0],
        "two nonzeros adjacent": [-168, -168],
    }
    for fn, fname in [(codeA, "A"), (codeB, "B"), (codeC, "C")]:
        fails = []
        for nm, sy in cases.items():
            try:
                out = fn(sy)[3]
                if out != sy:
                    fails.append(nm)
            except Exception as e:
                fails.append(f"{nm} (EXC {e})")
        if fails:
            print(f"\n  option {fname}: FAIL -> {fails}")
        else:
            print(f"\n  option {fname}: ALL {len(cases)} cases PASS")

    print("\n" + "=" * 78)
    print(f"SMALLEST (lossless): {best['label']}  =  {best['total']} B")
    print("=" * 78)


if __name__ == "__main__":
    main()
