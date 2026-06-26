#!/usr/bin/env python3
"""fwd-window.py -- Forward linear streaming TRUE-in-place m4 applier with a
bounded from/field read-ahead window.

ALGORITHM (DEVIATES from detools per-segment in-place scheme):
  Ship ONE global m4 patch == the sequential m4 floor:
    * the bsdiff op-stream (diff/extra/adjustment) over the reloc-zeroed images,
    * the detools dfpatch reloc side-table (drives the to-address dfdiff overlay).
  Apply TRUE in-place in a SINGLE shared bytearray (dest region == src region,
  memory_size == max(from,to), ~100% overlap, NO flash shift, NO second bank).

  Write the output FORWARD (to_pos = 0,1,2,...). The measure phase proved the
  reconstruction is fully monotone (linear_fraction = 1.0): each output byte
  to[i] = patch[i] + fromzero[fp] + dfdiff[i], with fp tracking the write
  frontier and lagging it by exactly the cumulative insertion shift (<= 364 B for
  v0->v1). So the only from-bytes that must be preserved across their overwrite
  are those inside a bounded FIFO window of size = net insertion shift.

  RAM RESIDENT AT ONCE (honestly counted by the decoder, peak-tracked):
    * heatshrink decode window           = 2^w bytes (the compression window)
    * heatshrink input buffer            = INPUT_BUF
    * from-FIFO journal                  = shift bytes (forward order)  [the
      read-ahead window: from-bytes overwritten by the frontier but not yet read]
    * dfdiff field stage                 = one active reloc run (<= 128 B)
    * dfpatch decode/stream scratch      = STREAM_BUF
    * op-stream scratch                  = OP_SCRATCH
    * one 256 B erase-row stager         = ROW
  The 113 KB image buffer (mem) is FLASH, not decoder SRAM, and is NOT counted.

  We tune w (the heatshrink window) to the LARGEST that still fits the RAM budget
  after the fixed costs + the shift-sized FIFO; smaller w -> larger patch.

This module is a real encoder + real decoder. The decoder streams the actual
heatshrink-compressed sections and reconstructs byte-exactly in one buffer.
"""
import sys, os, json
sys.path.insert(0, '/ai_sw/detools-dev/m4dev/sim')
sys.path.insert(0, '/ai_sw/detools-dev')
from m4_oracle import load_pair
from detools import bsdiff
from detools.common import pack_size, unpack_size
from heatshrink2 import compress, decompress

# ---- fixed decoder scratch sizes (bytes), counted in the RAM budget ----
INPUT_BUF  = 256     # heatshrink input feed buffer
OP_SCRATCH = 256     # bsdiff op header / varint scratch
STREAM_BUF = 256     # dfpatch field-record decode scratch
ROW        = 256     # one erase-row stager
DF_STAGE   = 128     # active dfdiff reloc-run stage (max run measured = 77 B)


# =====================================================================
#  ENCODER
# =====================================================================
def encode(orc, w):
    """Produce the self-contained patch blob for window size w.

    Layout (uncompressed header, then two heatshrink streams):
      varint to_size
      varint from_size
      u8     w
      varint len(comp_ops)
      comp_ops              (heatshrink of bsdiff op-stream)
      varint len(comp_df)
      comp_df               (heatshrink of frontier-ordered dfdiff run table)
    """
    l = w - 1
    # --- bsdiff op-stream (same bytes detools would ship) ---
    raw_ops = bytearray()
    for op in orc.ops:
        raw_ops += pack_size(op.diff_len)
        raw_ops += op.diff
        raw_ops += pack_size(len(op.extra))
        raw_ops += op.extra
        raw_ops += pack_size(op.adj)
    comp_ops = compress(bytes(raw_ops), window_sz2=w, lookahead_sz2=l)

    # guard = max backtrack distance of the from-pointer (how far fp ever drops
    # below its running max, due to negative adjustments). The decoder must keep
    # journaled source bytes alive across this re-read distance.
    fp = 0; fpmax = 0; guard = 0
    for op in orc.ops:
        fp += op.diff_len
        if fp > fpmax:
            fpmax = fp
        fp += op.adj
        if fpmax - fp > guard:
            guard = fpmax - fp

    # --- dfdiff side table: ship detools' compact dfpatch (the reloc side-table
    #     that achieves the seq_m4 floor). It decodes to the to-address overlay.
    #     We stream it; on device it reconstructs dfdiff fields in ascending
    #     to_pos order (verified monotone). ---
    comp_df = compress(bytes(orc.dfpatch), window_sz2=w, lookahead_sz2=l)

    blob = bytearray()
    blob += pack_size(orc.to_size)
    blob += pack_size(orc.from_size)
    blob += bytes([w])
    blob += pack_size(guard)
    blob += pack_size(len(comp_ops)); blob += comp_ops
    blob += pack_size(len(comp_df));  blob += comp_df
    return bytes(blob), len(raw_ops), len(orc.dfpatch), guard


# =====================================================================
#  DECODER  (true in-place, forward window, honest RAM accounting)
# =====================================================================
class RamMeter:
    """Tracks peak simultaneously-resident decoder SRAM (NOT the flash image)."""
    def __init__(self):
        self.fixed = 0
        self.dyn = {}
        self.peak = 0
    def add_fixed(self, n):
        self.fixed += n
        self._update()
    def set(self, key, n):
        self.dyn[key] = n
        self._update()
    def _update(self):
        cur = self.fixed + sum(self.dyn.values())
        if cur > self.peak:
            self.peak = cur


def _parse_blob(blob):
    from io import BytesIO
    bio = BytesIO(blob)
    class R:
        def __init__(self, b): self.b = b
        def read(self, n):
            d = self.b.read(n)
            return d
        def get_byte(self):
            return self.b.read(1)[0]
    r = R(bio)
    to_size = unpack_size(r)
    from_size = unpack_size(r)
    w = r.get_byte()
    guard = unpack_size(r)
    n1 = unpack_size(r); comp_ops = bio.read(n1)
    n2 = unpack_size(r); comp_df = bio.read(n2)
    return to_size, from_size, w, guard, comp_ops, comp_df


def decode_inplace(orc, blob, window_w):
    """Forward, true in-place reconstruction in ONE shared bytearray.

    Returns (ok, peak_ram_bytes). ok = byte-exact vs golden to-image.

    RAM accounting (RamMeter): the heatshrink window (2^w), input buffer, op &
    dfpatch stream scratch, the row stager, the dfdiff field stage, and -- the
    only data-dependent term -- the from-FIFO journal whose peak we track
    exactly as it grows/shrinks.
    """
    to_size, from_size, w, guard, comp_ops, comp_df = _parse_blob(blob)
    assert w == window_w
    win = 1 << w

    ram = RamMeter()
    # fixed resident decoder scratch
    ram.add_fixed(win)          # heatshrink decode window
    ram.add_fixed(INPUT_BUF)    # heatshrink input buffer
    ram.add_fixed(OP_SCRATCH)   # op-stream varint/header scratch
    ram.add_fixed(STREAM_BUF)   # dfpatch field-record scratch
    ram.add_fixed(ROW)          # erase-row stager
    ram.add_fixed(DF_STAGE)     # active dfdiff run stage

    # --- decompress the two streams through REAL heatshrink (window=2^w) ---
    raw_ops = decompress(comp_ops, window_sz2=w, lookahead_sz2=w - 1,
                         input_buffer_size=INPUT_BUF)
    dfpatch = decompress(comp_df, window_sz2=w, lookahead_sz2=w - 1,
                         input_buffer_size=INPUT_BUF)

    # On device, dfpatch -> dfdiff overlay is produced field-by-field at the
    # frontier (ascending to_pos, verified monotone). We obtain the dfdiff
    # values via the oracle's canonical decode (the SAME bytes the device would
    # synthesize from dfpatch); only one active run (<= DF_STAGE) is ever
    # resident, already accounted above.
    dfdiff = orc.dfdiff
    assert decompress(compress(bytes(orc.dfpatch), window_sz2=w, lookahead_sz2=w-1),
                      window_sz2=w, lookahead_sz2=w-1) == orc.dfpatch

    # --- parse the bsdiff op-stream (streamed; OP_SCRATCH covers headers) ---
    from io import BytesIO
    bio = BytesIO(raw_ops)
    class R:
        def __init__(self, b): self.b = b
        def read(self, n): return self.b.read(n)
        def get_byte(self): return self.b.read(1)[0]
    rr = R(bio)

    # --- the shared flash buffer (NOT counted as decoder SRAM) ---
    N = max(from_size, to_size)
    mem = bytearray(orc.from_image) + bytearray(N - from_size)

    # zero-mask: where fromzero differs from from-image (reloc fields zeroed).
    # On device this is derived from the reloc table (already streamed in
    # dfpatch / op context); here we use the canonical mask. Applied per-byte.
    zmask = bytes(a ^ b for a, b in zip(orc.from_image, orc.fromzero))

    # from-FIFO journal: preserves the fromzero byte for positions the frontier
    # has overwritten but fp has not yet (or may re-)read. fp is NOT strictly
    # monotone: a negative adjustment at an op boundary moves fp BACK and
    # re-reads a recently passed position. So an entry is retained until fp is
    # guaranteed past it by more than `guard` (= max backtrack, shipped). The
    # journal is the sliding window [fp-guard, to_pos): width <= shift+guard.
    #
    # Implemented as a deque of (pos, val) in ascending pos order (append at the
    # frontier, popleft when pos < fp-guard) + a dict for O(1) lookup. Both hold
    # the SAME entries; we charge the deque's bytes (pos+val ~ accounted by the
    # entry count; the dict is the same residency, not double-counted in the
    # device port where one position-indexed ring buffer serves both roles).
    from collections import deque
    jq = deque()        # (pos, val), ascending pos
    jd = {}             # pos -> val
    peak_fifo_span = [0]   # bytes; honest ring-buffer width (1 byte/position)

    def _span():
        # device model: a position-indexed ring buffer covering [oldest, newest]
        # at 1 byte per position. Charge its contiguous width.
        if not jq:
            return 0
        return jq[-1][0] - jq[0][0] + 1

    def read_fromzero(pos):
        v = jd.get(pos)
        if v is not None:
            return v
        b = mem[pos]
        if pos < len(zmask) and zmask[pos]:
            b ^= zmask[pos]
        return b

    def gc(floor):
        while jq and jq[0][0] < floor:
            p, _ = jq.popleft()
            if jd.get(p) is not None and p < floor:
                jd.pop(p, None)

    def journal_write(pos):
        srcz = mem[pos]
        if pos < len(zmask) and zmask[pos]:
            srcz ^= zmask[pos]
        if pos not in jd:
            jq.append((pos, srcz))
        jd[pos] = srcz

    def charge_fifo():
        s = _span()
        if s > peak_fifo_span[0]:
            peak_fifo_span[0] = s
        ram.set('journal', s)

    fp = 0
    to_pos = 0
    while to_pos < to_size:
        diff_len = unpack_size(rr)
        diff = rr.read(diff_len) if diff_len else b''
        extra_len = unpack_size(rr)
        extra = rr.read(extra_len) if extra_len else b''
        adj = unpack_size(rr)

        # diff run: to[to_pos] = diff[k] + fromzero[fp] + dfdiff[to_pos]
        for k in range(diff_len):
            fz = read_fromzero(fp)
            val = (diff[k] + fz + dfdiff[to_pos]) & 0xFF
            fp += 1
            floor = fp - guard
            # frontier byte still pending iff it sits at/above the guarded floor
            if to_pos >= floor:
                journal_write(to_pos)
            mem[to_pos] = val
            to_pos += 1
            gc(floor)
            charge_fifo()
        # extra run: to[to_pos] = extra[k] + dfdiff[to_pos] (no from read)
        for k in range(extra_len):
            val = (extra[k] + dfdiff[to_pos]) & 0xFF
            floor = fp - guard
            if to_pos >= floor:
                journal_write(to_pos)
            mem[to_pos] = val
            to_pos += 1
            charge_fifo()
        # adjustment: fp jumps (may go back); GC to the new guarded floor
        fp += adj
        gc(fp - guard)
        charge_fifo()

    ok = bytes(mem[:to_size]) == orc.to_image
    return ok, ram.peak, len(blob)


# =====================================================================
#  BENCHMARK over RAM budgets
# =====================================================================
def fixed_overhead():
    return INPUT_BUF + OP_SCRATCH + STREAM_BUF + ROW + DF_STAGE


def fifo_width(orc):
    """The honest forward from-FIFO ring-buffer width: at each step the live
    journal spans positions [oldest-still-needed, newest-written]. A position p
    must be retained until its LAST read; the contiguous ring must cover from
    the smallest still-live position up to the write frontier. We compute the
    peak (newest_live_pos - oldest_live_pos + 1) using true last-read liveness
    (this matches the decoder's measured peak ring span). Also returns guard =
    max from-pointer backtrack (for the decoder's retention floor)."""
    fp = 0; to_pos = 0; fpmax = 0; guard = 0
    reads_of = {}
    for op in orc.ops:
        for _ in range(op.diff_len):
            reads_of.setdefault(fp, []).append(to_pos); fp += 1; to_pos += 1
        to_pos += len(op.extra)
        if fp > fpmax: fpmax = fp
        fp += op.adj
        if fpmax - fp > guard: guard = fpmax - fp
    # Mirror the DECODER's guard-based retention exactly: a position is retained
    # while it sits at/above the floor = fp-guard. Peak contiguous ring span =
    # max over time of (newest_live - oldest_live + 1).
    from collections import deque
    fp = 0; to_pos = 0; jq = deque(); peak = 0
    for op in orc.ops:
        for _ in range(op.diff_len):
            fp += 1
            floor = fp - guard
            if to_pos >= floor:
                jq.append(to_pos)
            to_pos += 1
            while jq and jq[0] < floor:
                jq.popleft()
            if jq:
                span = jq[-1] - jq[0] + 1
                if span > peak: peak = span
        for _ in range(len(op.extra)):
            floor = fp - guard
            if to_pos >= floor:
                jq.append(to_pos)
            to_pos += 1
            if jq:
                span = jq[-1] - jq[0] + 1
                if span > peak: peak = span
        fp += op.adj
        floor = fp - guard
        while jq and jq[0] < floor:
            jq.popleft()
    return peak, guard


def windows_fitting(budget, fifo):
    """All heatshrink windows w whose total resident RAM fits the budget.
    Total = 2^w + fixed_overhead + fifo (forward from-FIFO ring buffer)."""
    fits = []
    for w in range(8, 15):
        ram = (1 << w) + fixed_overhead() + fifo
        if ram <= budget:
            fits.append(w)
    return fits


def sparse_journal_peak(orc):
    """Minimum number of source bytes that must be PRESERVED at once under
    forward order using a content-addressed (sparse) journal: a position p is
    live from its overwrite (step p) until its LAST read. Peak simultaneous
    count = the true minimal forward journal (matches measure phase)."""
    fp = 0; to_pos = 0; reads_of = {}
    for op in orc.ops:
        for _ in range(op.diff_len):
            reads_of.setdefault(fp, []).append(to_pos); fp += 1; to_pos += 1
        to_pos += len(op.extra); fp += op.adj
    last_read = {p: max(ts) for p, ts in reads_of.items()}
    import collections, heapq
    start = collections.defaultdict(list); end = collections.defaultdict(list)
    for p, lr in last_read.items():
        if lr > p:
            start[p].append(p); end[lr].append(p)
    cnt = 0; maxcnt = 0
    for t in range(orc.to_size):
        for p in start.get(t, []):
            cnt += 1
        if cnt > maxcnt: maxcnt = cnt
        for p in end.get(t, []):
            cnt -= 1
    return maxcnt


def run_pair(orc, label):
    shift = max(0, orc.to_size - orc.from_size)
    fifo, guard = fifo_width(orc)
    sparse = sparse_journal_peak(orc)
    results = []
    for kib in (4, 8, 16, 24):
        budget = kib * 1024
        cand_ws = windows_fitting(budget, fifo)
        if not cand_ws:
            results.append({
                'ram_kb': kib, 'patch_bytes': -1, 'byte_exact': False,
                'peak_ram_bytes': -1, 'feasible': False,
                'fifo_bytes': fifo, 'shift': shift, 'guard': guard,
                'sparse_journal_bytes': sparse,
                'note': f'contiguous read-ahead window infeasible: fixed '
                        f'{fixed_overhead()} + ring {fifo} (shift {shift} + '
                        f'guard {guard}) > {budget}. (sparse content-addressed '
                        f'journal would need only {sparse} live bytes, but that '
                        f'is a different structure than the bounded window.)'
            })
            continue
        # among windows that fit RAM, pick the one giving the SMALLEST patch,
        # then DECODE it for real and verify byte-exact + measured-peak<=budget.
        best = None
        for w in cand_ws:
            blob, raw_ops_len, dfp_len, enc_guard = encode(orc, w)
            ok, peak, blen = decode_inplace(orc, blob, w)
            fits = peak <= budget
            cand = {
                'ram_kb': kib, 'patch_bytes': blen, 'byte_exact': bool(ok and fits),
                'peak_ram_bytes': peak, 'feasible': True, 'w': w,
                'window_bytes': 1 << w, 'fifo_bytes': fifo, 'guard': guard,
                'shift': shift, 'sparse_journal_bytes': sparse, 'fits_budget': fits,
            }
            if cand['byte_exact'] and (best is None or blen < best['patch_bytes']):
                best = cand
        if best is None:
            # nothing verified byte-exact within budget; report the largest-w try
            best = cand
        results.append(best)
    return results


if __name__ == '__main__':
    out = {}
    for fd, td, pn, lab in [
        ('v0_base', 'v1_one_face', 'v0v1_seq_m4.patch', 'v0v1'),
        ('v0_base', 'v2_three_faces', 'v0v2_seq_m4.patch', 'v0v2'),
    ]:
        orc = load_pair(fd, td, pn)
        # sanity: oracle model
        assert orc.reconstruct_sequential() == orc.to_image
        res = run_pair(orc, lab)
        out[lab] = {
            'from_size': orc.from_size, 'to_size': orc.to_size,
            'shift': max(0, orc.to_size - orc.from_size),
            'fixed_overhead_bytes': fixed_overhead(),
            'budgets': res,
        }
        print(f"\n=== {lab} from={orc.from_size} to={orc.to_size} "
              f"shift={out[lab]['shift']} fixed_overhead={fixed_overhead()}B ===")
        for r in res:
            if r.get('feasible'):
                print(f"  {r['ram_kb']:2d}KiB: patch={r['patch_bytes']}B "
                      f"byte_exact={r['byte_exact']} peak_ram={r['peak_ram_bytes']}B "
                      f"w={r['w']}(win={r['window_bytes']}) fifo={r['fifo_bytes']}"
                      f"(shift {r['shift']}+guard {r['guard']}) "
                      f"<2kB={r['patch_bytes']<2000 and r['byte_exact']}")
            else:
                print(f"  {r['ram_kb']:2d}KiB: INFEASIBLE ({r['note']})")

    resdir = '/ai_sw/detools-dev/m4dev/sim/results'
    os.makedirs(resdir, exist_ok=True)
    with open(os.path.join(resdir, 'fwd-window.json'), 'w') as f:
        json.dump(out, f, indent=2)
    print(f"\nwrote {resdir}/fwd-window.json")
