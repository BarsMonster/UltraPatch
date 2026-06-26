#!/usr/bin/env python3
"""m4_inplace_sim.py -- TRUE in-place simulator + access-pattern instrumentation
for the detools arm-cortex-m4 reconstruction.

TRUE in-place model (the hard constraint):
  * One bytearray `mem`, length == from_size == memory region. NO headroom.
  * mem starts as the from-image. The to-image must overwrite the SAME bytes.
  * dest region == src region; overlap ~100% (here from/to differ in length by
    +360 / +3856 bytes, but both live in the same flash app region; we model
    mem of size max(from,to) and require to[0:to_size] to be correct, while the
    from-image initially occupies [0:from_size]).

What the canonical detools m4 reconstruction needs, per output byte i:
    to[i] = patch[i] + fromzero[fp(i)] + dfdiff[i]
  - fromzero[fp(i)]: a from-image byte (with reloc fields zeroed). In TRUE
    in-place this byte lives in `mem` at position fp(i) -- which may already
    have been overwritten by an earlier to-write.
  - dfdiff[i]: nonzero only inside to-image reloc fields; it is the re-encoded
    to-address overlay. It is DERIVED from disassembling the to-image, but in
    detools it is precomputed host-side and shipped (it is part of what bsdiff
    must reproduce; here we treat dfdiff as a needed side input whose bytes are
    not in `mem`).

INSTRUMENTATION -- for a given write order (forward / backward over ops, or any
linearization) we record, for every output byte:
    * to_pos              (write frontier position)
    * fp                  (from read position into the shared buffer)
    * read-ahead lag      = fp - write_frontier_max  (how far the needed source
                            byte is *ahead* of everything written so far)
    * conflict            = is the source byte fp already overwritten?
We then compute:
    * read-ahead lag distribution (forward and backward op order)
    * minimum in-flight journal: the set of source bytes that must be saved
      (because they will be overwritten before they are read) -- byte-exact.
    * working-set percentiles.
"""
import sys
import os
sys.path.insert(0, os.path.dirname(__file__))
from m4_oracle import load_pair
from detools import bsdiff
import statistics


def build_access_trace(orc):
    """Produce the per-output-byte access trace in the NATURAL (forward,
    monotone-to_pos) reconstruction order. Each entry:
        (to_pos, kind, fp_or_None)
    kind in {'diff','extra'}. For diff bytes a from-byte at fp is read; for
    extra bytes no from-byte is read (but dfdiff[to_pos] may apply).
    Also returns the list of (op_index, to_start, to_end, fp_start, fp_end, adj).
    """
    trace = []          # one entry per output byte
    op_spans = []
    fp = 0
    to_pos = 0
    for k, op in enumerate(orc.ops):
        to_start = to_pos
        fp_start = fp
        for _ in range(op.diff_len):
            trace.append((to_pos, 'diff', fp))
            fp += 1
            to_pos += 1
        el = len(op.extra)
        for _ in range(el):
            trace.append((to_pos, 'extra', None))
            to_pos += 1
        op_spans.append((k, to_start, to_pos, fp_start, fp, op.adj))
        fp += op.adj
    return trace, op_spans


def analyze_forward(orc):
    """Forward write order: write output bytes 0,1,2,... in order.
    For TRUE in-place we read fromzero[fp] from the same buffer. A read at fp
    sees the ORIGINAL from-byte iff position fp has not yet been written.
    Position p is written when to_pos == p (frontier passes it).

    Because to_pos increases by 1 each output byte and is monotone, position p
    is overwritten exactly at output-step p. A diff byte at output position
    to_pos reads from fp. The read is SAFE iff fp >= to_pos (source not yet
    overwritten) OR the source byte was journaled.

    read-ahead lag = fp - to_pos  (positive => reads ahead of frontier, safe
    for free; negative => reads behind frontier => that byte is already
    overwritten => MUST have been journaled).
    """
    trace, op_spans = build_access_trace(orc)
    lags = []                 # fp - to_pos for diff bytes
    behind = []               # (to_pos, fp) where fp < to_pos (overwritten)
    for (to_pos, kind, fp) in trace:
        if kind != 'diff':
            continue
        lag = fp - to_pos
        lags.append(lag)
        if fp < to_pos:
            behind.append((to_pos, fp))
    return trace, op_spans, lags, behind


def min_journal_forward(orc):
    """Minimum in-flight RAM (bytes) to hold from-field/source values that are
    read AFTER they are overwritten, under forward write order, byte-exact.

    A source byte at position p is needed at output-step s(p) (when some diff
    byte reads fp==p) and is destroyed at output-step p (when frontier writes
    p). If it is read after it is destroyed (s(p) > p for the LAST such read)
    we must journal it from before step p until its last read.

    We compute the peak simultaneous count of 'live journaled bytes': bytes
    already overwritten by the frontier but not yet consumed by their reads.
    Byte-exact: every needed source byte is preserved.
    """
    trace, op_spans = build_access_trace(orc)
    # For each diff output byte, it needs source position fp at step idx.
    # Source position p is overwritten at the output-step where to_pos==p.
    # We process output steps in order; maintain a journal of bytes we saved
    # because they'll be needed later but are about to be (or were) overwritten.
    #
    # Simplest exact accounting: a read of source p at step t is "behind" iff
    # p < t (p already overwritten). For correctness, p must have been saved at
    # the moment the frontier wrote it (step p) and kept until step t. The peak
    # number of such saved-but-not-yet-consumed bytes = min journal.
    #
    # Build, for each source position p that is read behind the frontier, the
    # interval [p (when overwritten), t_last (last read)]. Min RAM = max overlap.
    reads_of = {}   # source pos p -> list of read steps t
    step = 0
    for (to_pos, kind, fp) in trace:
        if kind == 'diff':
            reads_of.setdefault(fp, []).append(to_pos)
        step += 1
    intervals = []
    for p, ts in reads_of.items():
        last = max(ts)
        # only behind reads (read after overwrite) force journaling
        if last > p:
            intervals.append((p, last))   # save from step p .. read at last
    # peak overlap via sweep
    events = []
    for (s, e) in intervals:
        events.append((s, +1))
        events.append((e + 1, -1))
    events.sort()
    cur = peak = 0
    for _, d in events:
        cur += d
        if cur > peak:
            peak = cur
    return peak, len(intervals), intervals


def analyze_backward(orc):
    """Backward write order: write output bytes to_size-1 .. 0.
    Now position p is overwritten at output-step (to_size-1 - p) i.e. the
    frontier descends. A diff byte at output position to_pos reads source fp.
    Under backward order, source p is overwritten when the descending frontier
    reaches p, i.e. AFTER all output positions > p are written.

    read-ahead lag (backward) measures how far the needed source byte is
    *behind* (lower than) the descending frontier => already overwritten.
    Behind/overwritten condition under backward write: fp > to_pos means source
    is at a higher position than current output, which under descending order
    is ALREADY written => conflict. So conflicts are reads with fp > to_pos.
    """
    trace, op_spans = build_access_trace(orc)
    lags = []
    behind = []
    for (to_pos, kind, fp) in trace:
        if kind != 'diff':
            continue
        # descending frontier: positions > to_pos already overwritten
        lag = to_pos - fp   # positive => source below frontier (safe), neg => above (overwritten)
        lags.append(lag)
        if fp > to_pos:
            behind.append((to_pos, fp))
    return trace, op_spans, lags, behind


def min_journal_backward(orc):
    trace, op_spans = build_access_trace(orc)
    reads_of = {}
    for (to_pos, kind, fp) in trace:
        if kind == 'diff':
            reads_of.setdefault(fp, []).append(to_pos)
    # Under backward write, source p overwritten at virtual-step (to_size-1-p).
    # read of p happens at to_pos t -> virtual-step (to_size-1-t).
    # p destroyed at vstep (N-1-p); read at vstep (N-1-t). Behind iff read
    # vstep > destroy vstep => (N-1-t) > (N-1-p) => p > t => fp > to_pos.
    N = orc.to_size
    intervals = []
    for p, ts in reads_of.items():
        # last (largest) virtual read step = smallest t
        first_read_t = min(ts)
        destroy_v = N - 1 - p
        last_read_v = N - 1 - first_read_t
        if last_read_v > destroy_v:   # read after overwrite
            intervals.append((destroy_v, last_read_v))
    events = []
    for (s, e) in intervals:
        events.append((s, +1))
        events.append((e + 1, -1))
    events.sort()
    cur = peak = 0
    for _, d in events:
        cur += d
        if cur > peak:
            peak = cur
    return peak, len(intervals), intervals


def working_set(orc, window_radius=None):
    """Working set per output byte = distinct source positions touched within a
    sliding causal window. We report the *in-flight* distance between the from
    read pointer and the write frontier as the natural working set, and also
    the count of distinct journaled bytes alive (from min_journal_forward)."""
    # The relevant working set for in-place is the set of source bytes that are
    # simultaneously "in flight": overwritten-but-not-yet-read OR
    # read-ahead-but-frontier-not-arrived. We compute, at each output step, the
    # number of source positions p such that p has been passed by the frontier
    # (overwritten) but still has a pending future read.
    trace, op_spans = build_access_trace(orc)
    reads_of = {}
    for (to_pos, kind, fp) in trace:
        if kind == 'diff':
            reads_of.setdefault(fp, []).append(to_pos)
    # remaining reads countdown
    import collections
    pending = {p: sorted(ts) for p, ts in reads_of.items()}
    # at output step t (frontier at t, just overwrote position t-? ) count
    # source positions p<=t (overwritten) with a read at step > t.
    # do a sweep over steps
    # event: at step p, position p becomes overwritten; check if it has a future read.
    last_read = {p: max(ts) for p, ts in reads_of.items()}
    # number alive at step t = |{p : p <= t and last_read[p] > t}|
    sizes = []
    # efficient sweep
    add_at = collections.defaultdict(list)  # step p -> becomes overwritten
    for p in reads_of:
        add_at[p].append(p)
    alive = []  # heap of last_read for currently-overwritten-with-future-read
    import heapq
    overwritten_future = []  # min-heap of last_read times
    N = orc.to_size
    for t in range(N):
        # frontier overwrites position t
        if t in last_read and last_read[t] > t:
            heapq.heappush(overwritten_future, last_read[t])
        # pop those whose last read <= t (consumed)
        while overwritten_future and overwritten_future[0] <= t:
            heapq.heappop(overwritten_future)
        sizes.append(len(overwritten_future))
    return sizes


def pct(vals, p):
    if not vals:
        return 0
    s = sorted(vals)
    k = int(round((p / 100.0) * (len(s) - 1)))
    return s[k]


def run(orc, label):
    # verify model
    assert orc.reconstruct_sequential() == orc.to_image
    trace, op_spans, lags_f, behind_f = analyze_forward(orc)
    _, _, lags_b, behind_b = analyze_backward(orc)
    jf_peak, jf_n, jf_iv = min_journal_forward(orc)
    jb_peak, jb_n, jb_iv = min_journal_backward(orc)
    ws = working_set(orc)
    n_diff = sum(1 for _ in trace if _[1] == 'diff')
    n_extra = sum(1 for _ in trace if _[1] == 'extra')

    # read-ahead lag: max distance a needed from-byte is ahead of write frontier
    max_read_ahead_f = max(lags_f) if lags_f else 0
    max_read_ahead_b = max(lags_b) if lags_b else 0

    # linear fraction: fraction of diff bytes whose source is read in the same
    # monotone direction as the frontier without conflict, under best order.
    # forward conflicts:
    conf_f = len(behind_f)
    conf_b = len(behind_b)
    best_conf = min(conf_f, conf_b)
    linear_fraction = 1.0 - (best_conf / n_diff) if n_diff else 1.0

    res = {
        'label': label,
        'from_size': orc.from_size,
        'to_size': orc.to_size,
        'n_ops': len(orc.ops),
        'n_diff_bytes': n_diff,
        'n_extra_bytes': n_extra,
        'reloc_from': len(orc.reloc_from),
        'reloc_to': len(orc.reloc_to),
        'dfpatch_size': orc.dfpatch_size,
        'forward': {
            'lag_min': min(lags_f) if lags_f else 0,
            'lag_p50': pct(lags_f, 50),
            'lag_p99': pct(lags_f, 99),
            'lag_max': max_read_ahead_f,
            'conflicts_behind': conf_f,
            'journal_peak_bytes': jf_peak,
            'journal_intervals': jf_n,
        },
        'backward': {
            'lag_min': min(lags_b) if lags_b else 0,
            'lag_p50': pct(lags_b, 50),
            'lag_p99': pct(lags_b, 99),
            'lag_max': max_read_ahead_b,
            'conflicts_behind': conf_b,
            'journal_peak_bytes': jb_peak,
            'journal_intervals': jb_n,
        },
        'working_set_p50': pct(ws, 50),
        'working_set_p99': pct(ws, 99),
        'working_set_max': max(ws) if ws else 0,
        'linear_fraction': linear_fraction,
    }
    return res


if __name__ == '__main__':
    import json
    out = {}
    for fd, td, pn, label in [
        ('v0_base', 'v1_one_face', 'v0v1_seq_m4.patch', 'v0v1'),
        ('v0_base', 'v2_three_faces', 'v0v2_seq_m4.patch', 'v0v2'),
    ]:
        orc = load_pair(fd, td, pn)
        r = run(orc, label)
        out[label] = r
        print(f"\n=== {label} : from={r['from_size']} to={r['to_size']} ops={r['n_ops']} "
              f"diff={r['n_diff_bytes']} extra={r['n_extra_bytes']} ===")
        print(f"  reloc_from={r['reloc_from']} reloc_to={r['reloc_to']} dfpatch={r['dfpatch_size']}")
        print(f"  FORWARD : lag[min={r['forward']['lag_min']} p50={r['forward']['lag_p50']} "
              f"p99={r['forward']['lag_p99']} max={r['forward']['lag_max']}] "
              f"conflicts_behind={r['forward']['conflicts_behind']} "
              f"journal_peak={r['forward']['journal_peak_bytes']}B")
        print(f"  BACKWARD: lag[min={r['backward']['lag_min']} p50={r['backward']['lag_p50']} "
              f"p99={r['backward']['lag_p99']} max={r['backward']['lag_max']}] "
              f"conflicts_behind={r['backward']['conflicts_behind']} "
              f"journal_peak={r['backward']['journal_peak_bytes']}B")
        print(f"  working_set: p50={r['working_set_p50']} p99={r['working_set_p99']} "
              f"max={r['working_set_max']}  linear_fraction={r['linear_fraction']:.5f}")
    with open(os.path.join(os.path.dirname(__file__), 'artifacts', 'access_metrics.json'), 'w') as f:
        json.dump(out, f, indent=2)
    print("\nwrote artifacts/access_metrics.json")
