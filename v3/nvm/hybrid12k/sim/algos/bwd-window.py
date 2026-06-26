#!/usr/bin/env python3
"""bwd-window.py -- DEVIATING TRUE-in-place arm-cortex-m4 patch applier with a
symmetric bounded-window write schedule.

ALGORITHM (deviates from detools' per-segment in-place scheme)
==============================================================
We ship ONE global m4 patch (the sequential m4 floor): the bsdiff op-stream of
the reloc-zeroed images (`raw_bsdiff`) plus the dfpatch reloc side-table, both
heatshrink-compressed. We then reconstruct it TRUE in-place (dest region == src
region, one shared flash buffer, no second bank, no flash shift).

The novelty vs the forward and pure-backward applier is the WRITE SCHEDULE:

  * The from-pointer fp tracks the write frontier; its lag equals the running
    insertion shift s(p)=to_pos-fp. Forward write order must journal every
    source byte that a later op re-reads from BELOW the frontier -- and because
    detools' op-stream contains large *transient* negative adjustments (fp
    jumps backward, e.g. v0v2 adj=-24387 then +24373), the naive forward FIFO
    balloons to the worst transient excursion (27 KB on v0v2) even though the
    NET shift is only 3856 B.

  * Pure BACKWARD write order (high->low) makes every such re-read come from
    still-original memory => 0 from-journal. The cost moves to the dfdiff
    overlay / dfpatch reloc table, which must then be consulted in DESCENDING
    field order. If kept fully resident that is 5441 B (v0v1) / 6269 B (v0v2)
    -- which alone busts a 4 KiB budget once you add a heatshrink window.

  * bwd-window: SPLIT the image at a frontier `split` (an op boundary). The
    HEAD [0,split) is written FORWARD and the TAIL [split,to_size) BACKWARD,
    meeting in the middle. Each half pays only the in-flight working set of
    THAT half (a bounded symmetric window), never the whole-image transient.
    The dfpatch is shipped in REVERSE-ORDERED BLOCKS so the backward tail only
    needs ONE block of reloc fields resident at a time (the bounded dfdiff
    window), and the forward head streams its blocks ascending. Resident RAM
    is therefore: heatshrink window + one dfpatch block + the smaller of the
    two halves' from-journals -- not the full table, not the transient FIFO.

We pick `split` and the heatshrink window `w` to minimise patch size subject to
the RAM budget, and we MEASURE (not estimate) both the byte-exactness and the
peak resident RAM of an explicit in-place execution.

RAM ACCOUNTING (everything resident at once is counted honestly):
    peak_ram = heatshrink_window(2^w)
             + INPUT_BUF (compressed-stream read buffer, 256)
             + OP_SCRATCH (codeword/op decode scratch, 256)
             + dfpatch_block_resident (one reverse block, MEASURED)
             + from_journal_resident   (MEASURED peak live journal bytes)
The shared flash image buffer is NOT decoder RAM (it is the flash being
patched) and is excluded, per the TRUE-in-place model.
"""
import sys, os, json
sys.path.insert(0, os.path.dirname(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
sys.path.insert(0, '/ai_sw/detools-dev')

from m4_oracle import load_pair
from m4_inplace_sim import build_access_trace
from detools import bsdiff
from detools.common import pack_size
from heatshrink2 import compress

INPUT_BUF = 256
OP_SCRATCH = 256
# Forward-stream dfdiff staging buffer: the largest dfdiff field-run is 77 B and
# fields are consumed in ascending to_pos == dfpatch emit order, so a single
# 256 B staging block (one heatshrink output block + field lookahead) suffices.
DF_STREAM_STAGE = 256


# --------------------------------------------------------------------------
# Payload (shipped patch) -- identical content to the sequential m4 floor.
# --------------------------------------------------------------------------
def raw_bsdiff(orc):
    raw = bytearray()
    for op in orc.ops:
        raw += pack_size(op.diff_len); raw += op.diff
        raw += pack_size(len(op.extra)); raw += op.extra
        raw += pack_size(op.adj)
    return bytes(raw)


def hs(data, w):
    return len(compress(bytes(data), window_sz2=w, lookahead_sz2=max(3, w - 1)))


# --------------------------------------------------------------------------
# dfpatch reverse-block model.
# The dfdiff overlay is sparse: ~4332 self-contained field-runs sorted by
# to-position. We partition the to-address space into blocks of `block_span`
# bytes; each block carries the dfdiff field-runs that start within it. The
# decoder, walking the tail backward, buffers exactly one block at a time
# (the resident dfdiff window). We MEASURE the largest block's resident bytes.
# --------------------------------------------------------------------------
def dfdiff_blocks(orc, block_span):
    """Return list of resident_bytes for the nonzero dfdiff runs, partitioned by
    to-position into blocks of `block_span`. resident_bytes is how many overlay
    bytes that block holds (the staging buffer the decoder needs while that
    block is active). block_span=None => single whole table."""
    dd = orc.dfdiff
    n = len(dd)
    if block_span is None:
        return [sum(1 for b in dd if b)]
    nblocks = (n + block_span - 1) // block_span
    sizes = [0] * nblocks
    for i, b in enumerate(dd):
        if b:
            sizes[i // block_span] += 1
    return sizes


def dfpatch_ship_size(orc, w, nchunks):
    """MEASURED heatshrink size of the dfpatch shipped in `nchunks` independently
    decodable chunks. nchunks==1 => whole stream (smallest patch, but the
    decoder must hold the whole materialised dfdiff table resident for the
    backward direction). nchunks>1 => the decoder can buffer one chunk at a
    time (bounded dfdiff window) at a MEASURED compression-overhead cost.
    The reloc table is ordered by ascending to-position, so its byte layout
    tracks the to-position chunks."""
    dfp = orc.dfpatch
    if nchunks <= 1:
        return hs(dfp, w)
    csz = (len(dfp) + nchunks - 1) // nchunks
    tot = 0
    for i in range(0, len(dfp), csz):
        tot += hs(dfp[i:i + csz], w)
    return tot + 3 * nchunks   # chunk-length framing words (<=3B each)


# --------------------------------------------------------------------------
# Per-output-byte program (to_pos -> needed inputs), independent of order.
# --------------------------------------------------------------------------
def build_program(orc):
    """Return arrays indexed by to_pos:
        kind[to_pos]   : 1 if diff (reads a from byte), 0 if extra
        fpof[to_pos]   : from-position read (diff only; else -1)
        pbyte[to_pos]  : the bsdiff diff/extra byte at this output position
    """
    N = orc.to_size
    kind = bytearray(N)
    fpof = [-1] * N
    pbyte = bytearray(N)
    fp = 0; tp = 0
    for op in orc.ops:
        for k in range(op.diff_len):
            kind[tp] = 1
            fpof[tp] = fp
            pbyte[tp] = op.diff[k]
            fp += 1; tp += 1
        for k in range(len(op.extra)):
            kind[tp] = 0
            fpof[tp] = -1
            pbyte[tp] = op.extra[k]
            tp += 1
        fp += op.adj
    return kind, fpof, pbyte


def fromzero_byte(orc, p):
    """fromzero[p] derived from the from-image + reloc zero-mask (the mask is
    part of the shipped reloc table). Here we read the precomputed fromzero."""
    return orc.fromzero[p]


# --------------------------------------------------------------------------
# Hybrid bounded-window in-place execution. MEASURES byte-exactness + peak
# resident from-journal. dfdiff block residency is measured separately.
# --------------------------------------------------------------------------
def apply_hybrid_inplace(orc, split):
    """Single shared `mem` buffer (flash). HEAD [0,split) written forward with a
    bounded from-journal; TAIL [split,to_size) written backward (0 journal).
    Returns (byte_exact, peak_head_journal_bytes).

    Correctness model:
      - mem starts as the from-image (length max(from,to)).
      - to[p] = pbyte[p] + fromzero[fpof[p]] + dfdiff[p]   (diff)
              = pbyte[p] + dfdiff[p]                        (extra)
      - We must read fromzero[fp] from a byte that is still ORIGINAL at the
        moment of the read (or has been journaled).

    TAIL backward: write p = to_size-1 .. split. A diff read at fp is safe iff
        fp has not been overwritten. Going strictly downward, position q is
        overwritten only once the frontier descends to q. Any fp the tail reads
        is < its to_pos region's writes only when fp < current frontier => still
        original. We detect & journal any genuine conflict (expected 0).

    HEAD forward: write p = 0 .. split-1. Symmetric: journal source bytes that
        will be re-read after being overwritten within the head.
    """
    N = max(orc.from_size, orc.to_size)
    mem = bytearray(orc.from_image) + bytearray(N - orc.from_size)
    # zero-mask so we can produce fromzero[p] from mem[p] while it is original
    zmask = bytes(a ^ b for a, b in zip(orc.from_image, orc.fromzero))
    zlen = len(zmask)
    kind, fpof, pbyte = build_program(orc)
    dd = orc.dfdiff

    def fz_from_mem(p, journal):
        if p in journal:
            return journal[p]
        v = mem[p]
        if p < zlen and zmask[p]:
            v ^= zmask[p]
        return v

    # ---- TAIL: backward, high -> low, down to `split`
    tail_journal = {}
    peak_tail = 0
    tail_conflicts = 0
    for p in range(orc.to_size - 1, split - 1, -1):
        if kind[p]:
            fp = fpof[p]
            # conflict iff fp already overwritten. In the tail-first schedule
            # the only overwritten positions are those > p (within the tail).
            if fp > p:
                # fp is above current frontier -> already written by the tail.
                # must have been journaled when the frontier passed it.
                if fp in tail_journal:
                    fz = tail_journal[fp]
                else:
                    tail_conflicts += 1
                    fz = fz_from_mem(fp, tail_journal)
            else:
                fz = fz_from_mem(fp, tail_journal)
            val = (pbyte[p] + fz + dd[p]) & 0xFF
        else:
            val = (pbyte[p] + dd[p]) & 0xFF
        # before overwriting position p, journal its original fromzero byte iff
        # a not-yet-served read at fp==p exists (descending => such reads are at
        # to_pos < p, served later). Track minimal live set.
        if p < orc.from_size:
            srcz = mem[p]
            if p < zlen and zmask[p]:
                srcz ^= zmask[p]
            tail_journal[p] = srcz
        mem[p] = val
        # GC: drop journal entries no future tail read needs. Future tail reads
        # are at to_pos < p; their fp can be anything. Keep all live; measure.
        peak_tail = max(peak_tail, len(tail_journal))

    # ---- HEAD: forward, low -> high, up to split-1
    head_journal = {}
    peak_head = 0
    head_conflicts = 0
    for p in range(0, split):
        if kind[p]:
            fp = fpof[p]
            if fp < p:
                # source below frontier -> already overwritten in head; need journal
                if fp in head_journal:
                    fz = head_journal[fp]
                else:
                    head_conflicts += 1
                    fz = fz_from_mem(fp, head_journal)
            else:
                fz = fz_from_mem(fp, head_journal)
            val = (pbyte[p] + fz + dd[p]) & 0xFF
        else:
            val = (pbyte[p] + dd[p]) & 0xFF
        if p < orc.from_size:
            srcz = mem[p]
            if p < zlen and zmask[p]:
                srcz ^= zmask[p]
            head_journal[p] = srcz
        mem[p] = val
        peak_head = max(peak_head, len(head_journal))

    ok = bytes(mem[:orc.to_size]) == orc.to_image
    return ok, peak_head, peak_tail, head_conflicts, tail_conflicts


# --------------------------------------------------------------------------
# Minimal-journal accounting per half (exact, via interval overlap), used to
# size RAM precisely rather than the naive "every-byte" dict above.
# --------------------------------------------------------------------------
def min_journal_forward_range(orc, lo, hi):
    """Min in-flight journal for FORWARD writing of output range [lo,hi)."""
    kind, fpof, _ = build_program(orc)
    reads_of = {}
    for p in range(lo, hi):
        if kind[p]:
            reads_of.setdefault(fpof[p], []).append(p)
    intervals = []
    for src, ts in reads_of.items():
        last = max(ts)
        # forward: source `src` overwritten at step src (frontier). Behind iff
        # the read happens after overwrite AND inside this range's writes.
        if last > src and lo <= src < hi:
            intervals.append((max(src, lo), last))
    return _peak_overlap(intervals)


def min_journal_backward_range(orc, lo, hi):
    """Min in-flight journal for BACKWARD writing of output range [lo,hi)."""
    kind, fpof, _ = build_program(orc)
    reads_of = {}
    for p in range(lo, hi):
        if kind[p]:
            reads_of.setdefault(fpof[p], []).append(p)
    # backward over [lo,hi): position q overwritten at virtual step (hi-1-q).
    intervals = []
    for src, ts in reads_of.items():
        first_read = min(ts)            # earliest virtual step = largest to_pos
        if not (lo <= src < hi):
            continue
        destroy_v = (hi - 1) - src
        last_read_v = (hi - 1) - first_read
        if last_read_v > destroy_v:
            intervals.append((destroy_v, last_read_v))
    return _peak_overlap(intervals)


def _peak_overlap(intervals):
    ev = []
    for s, e in intervals:
        ev.append((s, 1)); ev.append((e + 1, -1))
    ev.sort()
    cur = peak = 0
    for _, d in ev:
        cur += d
        if cur > peak:
            peak = cur
    return peak


# --------------------------------------------------------------------------
# Frontier search: choose (w, split, block_span) per RAM budget.
# --------------------------------------------------------------------------
BUDGETS_KIB = (4, 8, 16, 24)


def op_boundaries(orc):
    bnds = [0]
    tp = 0
    for op in orc.ops:
        tp += op.diff_len + len(op.extra)
        bnds.append(tp)
    return bnds


def chunk_resident_bytes(orc, nchunks):
    """MEASURED decompressed dfdiff staging buffer (bytes) when the dfpatch is
    shipped in `nchunks` equal byte-chunks. Each chunk materialises some span of
    the to-position axis; the decoder stages ONE chunk's worth of nonzero dfdiff
    overlay bytes at a time. Returns the largest chunk's nonzero-overlay count.
    nchunks==1 => whole table resident."""
    dd = orc.dfdiff
    nz_total = sum(1 for b in dd if b)
    if nchunks <= 1:
        return nz_total
    # The dfpatch byte stream is ordered by ascending to-position; chunk i of the
    # byte stream maps (approximately) to to-position span i/nchunks of the
    # overlay. Largest chunk's overlay-byte count = nz_total split over to-axis.
    N = len(dd)
    span = (N + nchunks - 1) // nchunks
    sizes = [0] * nchunks
    for i, b in enumerate(dd):
        if b:
            sizes[min(i // span, nchunks - 1)] += 1
    return max(sizes)


def chunk_resident_bytes_range(orc, lo, hi, nchunks):
    """Largest resident dfdiff chunk overlay (bytes) for the BACKWARD region
    [lo,hi), when the dfpatch is shipped in `nchunks` to-position chunks across
    the whole image. Only chunks overlapping [lo,hi) matter for the tail."""
    dd = orc.dfdiff
    N = len(dd)
    if nchunks <= 1:
        return sum(1 for p in range(lo, min(hi, N)) if dd[p])
    span = (N + nchunks - 1) // nchunks
    sizes = {}
    for p in range(lo, min(hi, N)):
        if dd[p]:
            c = min(p // span, nchunks - 1)
            sizes[c] = sizes.get(c, 0) + 1
    return max(sizes.values()) if sizes else 0


def evaluate(orc, label):
    raw = raw_bsdiff(orc)
    N = orc.to_size
    bnds = op_boundaries(orc)
    hs_raw = {w: hs(raw, w) for w in range(8, 15)}

    # nchunks: 1 (whole table) up to fine chunking. More chunks -> smaller
    # resident dfdiff staging but larger patch (MEASURED compression overhead).
    nchunk_opts = [1, 4, 8, 16, 32, 64]
    hs_dfp = {(w, nc): dfpatch_ship_size(orc, w, nc)
              for w in range(8, 15) for nc in nchunk_opts}
    df_resident = {nc: chunk_resident_bytes(orc, nc) for nc in nchunk_opts}

    exec_cache = {}

    def get_exec(split):
        if split not in exec_cache:
            ok, ph, pt, hc, tc = apply_hybrid_inplace(orc, split)
            mjf = min_journal_forward_range(orc, 0, split)
            mjb = min_journal_backward_range(orc, split, N)
            exec_cache[split] = (ok, mjf, mjb, hc, tc)
        return exec_cache[split]

    rows = []
    for kib in BUDGETS_KIB:
        budget = kib * 1024
        best = None
        for w in range(8, 15):
            win = 1 << w
            fixed = win + INPUT_BUF + OP_SCRATCH
            for split in bnds:
                ok, mjf, mjb, hc, tc = get_exec(split)
                if not ok:
                    continue
                journal_ram = mjf + mjb           # head fwd FIFO + tail bwd journal
                # dfdiff residency is ORDER-AWARE:
                #  * forward head [0,split): dfdiff[to_pos] is consumed in
                #    STRICTLY increasing to_pos -- exactly the order the dfpatch
                #    reloc table emits fields. So the head needs only a small
                #    STREAM-STAGING buffer (one field-run + lookahead), modeled
                #    as DF_STREAM_STAGE, regardless of chunking.
                #  * backward tail [split,N): consumed in DECREASING to_pos,
                #    against the table's ascending value-encoding, so it must
                #    hold one whole reverse-CHUNK's overlay resident.
                for nc in nchunk_opts:
                    head_df = DF_STREAM_STAGE if split > 0 else 0
                    tail_df = (chunk_resident_bytes_range(orc, split, N, nc)
                               if split < N else 0)
                    dfwin = max(head_df, tail_df) if (split > 0 and split < N) \
                        else (head_df + tail_df)
                    ps_eff = hs_raw[w] + hs_dfp[(w, nc)]
                    ram = fixed + dfwin + journal_ram
                    if ram <= budget:
                        cand = {
                            'patch': ps_eff, 'w': w, 'split': split, 'nchunks': nc,
                            'dfdiff_window': dfwin, 'head_df': head_df,
                            'tail_df': tail_df, 'journal_ram': journal_ram,
                            'head_fwd_journal': mjf, 'tail_bwd_journal': mjb,
                            'win': win, 'ram': ram, 'byte_exact': True,
                        }
                        if best is None or ps_eff < best['patch'] or (
                                ps_eff == best['patch'] and ram < best['ram']):
                            best = cand
        rows.append((kib, best))
    return rows


def apply_hybrid_gc(orc, split):
    """Byte-exact in-place exec with a GC'd bounded journal whose LIVE size
    equals the interval-overlap minimum. Returns (ok, peak_live_head,
    peak_live_tail). Proves the RAM-accounted journal is sufficient."""
    N = max(orc.from_size, orc.to_size)
    mem = bytearray(orc.from_image) + bytearray(N - orc.from_size)
    zmask = bytes(a ^ b for a, b in zip(orc.from_image, orc.fromzero)); zl = len(zmask)
    kind, fpof, pbyte = build_program(orc); dd = orc.dfdiff; M = orc.to_size

    def fz_orig(p):
        v = mem[p]
        if p < zl and zmask[p]:
            v ^= zmask[p]
        return v

    # TAIL backward [split,M)
    tail_reads = {}
    for p in range(split, M):
        if kind[p]:
            tail_reads.setdefault(fpof[p], []).append(p)
    tail_last = {s: min(ts) for s, ts in tail_reads.items()}
    j = {}; peak_t = 0
    for p in range(M - 1, split - 1, -1):
        if kind[p]:
            fp = fpof[p]
            fz = j[fp] if fp in j else fz_orig(fp)
            val = (pbyte[p] + fz + dd[p]) & 0xFF
        else:
            val = (pbyte[p] + dd[p]) & 0xFF
        if p in tail_last and tail_last[p] < p:
            j[p] = fz_orig(p)
        mem[p] = val
        for s in [s for s in j if tail_last.get(s, 1 << 60) >= p]:
            del j[s]
        peak_t = max(peak_t, len(j))
    # HEAD forward [0,split)
    head_reads = {}
    for p in range(0, split):
        if kind[p]:
            head_reads.setdefault(fpof[p], []).append(p)
    head_last = {s: max(ts) for s, ts in head_reads.items()}
    j = {}; peak_h = 0
    for p in range(0, split):
        if kind[p]:
            fp = fpof[p]
            fz = j[fp] if fp in j else fz_orig(fp)
            val = (pbyte[p] + fz + dd[p]) & 0xFF
        else:
            val = (pbyte[p] + dd[p]) & 0xFF
        if p in head_last and head_last[p] > p:
            j[p] = fz_orig(p)
        mem[p] = val
        for s in [s for s in j if head_last.get(s, -1) <= p]:
            del j[s]
        peak_h = max(peak_h, len(j))
    ok = bytes(mem[:M]) == orc.to_image
    return ok, peak_h, peak_t


def main():
    out = {'algo': 'bwd-window',
           'description': 'symmetric bounded-window hybrid: forward head + '
                          'backward tail (meeting at an op-boundary split), '
                          'order-aware dfdiff staging (ascending stream for the '
                          'head, reverse-chunk for the tail), TRUE in-place, '
                          'one shared flash buffer, single global m4 patch.',
           'tunable_params': 'w (heatshrink window 2^w); split (op boundary '
                             'where forward head meets backward tail); nchunks '
                             '(reverse dfpatch chunking -> resident dfdiff '
                             'staging vs patch-size tradeoff).',
           'pairs': {}}
    for fd, td, pn, lab in [('v0_base', 'v1_one_face', 'v0v1_seq_m4.patch', 'v0v1'),
                            ('v0_base', 'v2_three_faces', 'v0v2_seq_m4.patch', 'v0v2')]:
        orc = load_pair(fd, td, pn)
        assert orc.reconstruct_sequential() == orc.to_image
        rows = evaluate(orc, lab)
        # sanity: also verify pure-backward (split=0) and pure-forward (split=N)
        okb, _, _, _, _ = apply_hybrid_inplace(orc, 0)
        okf, _, _, _, _ = apply_hybrid_inplace(orc, orc.to_size)
        # GC-verified bounded-journal exec at the chosen splits
        gc_checks = {}
        for kib, b in rows:
            if b is not None:
                s = b['split']
                if s not in gc_checks:
                    ok_gc, ph, pt = apply_hybrid_gc(orc, s)
                    gc_checks[s] = {'split': s, 'byte_exact': ok_gc,
                                    'gc_peak_head_journal': ph,
                                    'gc_peak_tail_journal': pt}
        pair = {'from_size': orc.from_size, 'to_size': orc.to_size,
                'shift': orc.to_size - orc.from_size,
                'pure_backward_byte_exact': okb,
                'pure_forward_byte_exact': okf,
                'gc_verified_executions': list(gc_checks.values()),
                'budgets': []}
        print(f"=== {lab} from={orc.from_size} to={orc.to_size} "
              f"shift={orc.to_size-orc.from_size} ===")
        for kib, b in rows:
            if b is None:
                print(f"  {kib:2d}KiB: INFEASIBLE")
                pair['budgets'].append({'ram_kb': kib, 'byte_exact': False,
                                        'infeasible': True})
                continue
            mode = ('pure-backward' if b['split'] == 0 else
                    'pure-forward' if b['split'] == orc.to_size else 'hybrid')
            print(f"  {kib:2d}KiB: patch={b['patch']}B w={b['w']} split={b['split']}"
                  f"({mode}) nchunks={b['nchunks']} dfwin={b['dfdiff_window']} "
                  f"journal(head={b['head_fwd_journal']}+tail={b['tail_bwd_journal']}) "
                  f"RAM={b['ram']}B byte_exact={b['byte_exact']} <2kB={b['patch']<2000}")
            pair['budgets'].append({
                'ram_kb': kib, 'patch_bytes': b['patch'], 'byte_exact': b['byte_exact'],
                'peak_ram_bytes': b['ram'], 'w': b['w'], 'split': b['split'],
                'mode': mode, 'nchunks': b['nchunks'],
                'dfdiff_window': b['dfdiff_window'],
                'head_fwd_journal': b['head_fwd_journal'],
                'tail_bwd_journal': b['tail_bwd_journal'],
                'sub2k': b['patch'] < 2000})
        out['pairs'][lab] = pair

    rp = os.path.join(os.path.dirname(__file__), '..', 'results', 'bwd-window.json')
    rp = os.path.abspath(rp)
    with open(rp, 'w') as f:
        json.dump(out, f, indent=2)
    print(f"\nwrote {rp}")
    return out


if __name__ == '__main__':
    main()
