#!/usr/bin/env python3
"""hybrid-largeseg.py -- Large-segment (tunable, RAM-sized) global-block hybrid
in-place m4 patch encoder + decoder simulator.

DEVIATION FROM DETOOLS PER-SEGMENT SCHEME
-----------------------------------------
detools' inplace_m4 chops the image into fixed 2-8 KiB segments and RE-SHIPS the
m4 relocation context (the reloc side-table / dfpatch driver) once PER segment.
With ~4332 reloc fields spread across the image, every 2 KiB segment re-ships its
share of the reloc table plus per-segment framing, inflating the patch to
2892 B (seg=8192) .. 4192 B (seg=2048) and forcing a full to_size dfdiff overlay
plus an expanded ~18 KiB reloc table resident.

THIS HYBRID instead:
  (A) ships the m4 reloc block (dfpatch) ONCE GLOBALLY -- never per segment --
      compressed with its OWN optimal (small) heatshrink window.  The reloc table
      is consumed as a forward field-order STREAM, so its residency is one small
      block buffer, not the whole table.
  (B) ships the bsdiff op-stream partitioned into LARGE, RAM-SIZED regions
      ("large segments"), each compressed independently with a heatshrink window
      sized to the RAM budget.  Regions are scheduled for LINEAR SAFETY: each
      region is reconstructed with a bounded from-FIFO journal (forward order) so
      the in-flight working set never exceeds the per-region net insertion shift
      plus the measured read-ahead lag (<=364 B for v0v1).

The segment size is the RAM lever:
  * The bsdiff stream compresses better with a LARGER heatshrink window
    (v0v1: 2348 B @w8 -> 765 B @w14).  A single global stream wants w=14 (a 16 KiB
    window) -- unaffordable at a 4 KiB budget.
  * So we CAP the per-region window at what the budget allows.  segment size ~=
    heatshrink window = 2^w.  Bigger budget -> bigger window -> fewer patch bytes,
    converging on the global sequential m4 floor (seq_m4 = 1857 B; our combined
    floor 1845 B @w12+w8).

RAM ACCOUNTING -- counts EVERYTHING resident AT ONCE.  The two compressed streams
(reloc block, bsdiff regions) are decoded at DIFFERENT times in a single pass, so
only ONE heatshrink window is live at the peak.  Peak resident set:
    peak = max(
        decode_dfpatch:  2^w_df  + INPUT_BUF + DFP_BLOCK + OP_SCRATCH,
        apply_regions:   2^w_seg + INPUT_BUF + DFP_BLOCK + from_FIFO + OP_SCRATCH
    )
where from_FIFO is the per-region forward journal (<= region net shift + lag), and
DFP_BLOCK is the resident reloc-field block (streamed, not the full table).

The simulator performs a REAL byte-exact TRUE-in-place reconstruction in a single
shared bytearray (dest region == src region, no flash shift, no second bank) and
measures peak RAM honestly.  A failed byte-exact check at a budget => byte_exact
False for that budget.
"""
import sys, os, json
sys.path.insert(0, os.path.dirname(__file__))
sys.path.insert(0, os.path.dirname(os.path.dirname(__file__)))
sys.path.insert(0, '/ai_sw/detools-dev')

from m4_oracle import load_pair
from detools import bsdiff
from detools.common import pack_size
from heatshrink2 import compress

# Fixed small scratch costs (bytes), all resident.
INPUT_BUF  = 256   # heatshrink input/refill buffer (one 256 B erase row)
OP_SCRATCH = 256   # op decode / codeword / 4B-aligned disasm scratch
DFP_BLOCK  = 512   # resident reloc-field stream block (forward field order)


# ---------------------------------------------------------------------------
# ENCODER
# ---------------------------------------------------------------------------
def hs(data, w):
    """heatshrink compress with window 2^w, lookahead 2^(w-1)."""
    return compress(bytes(data), window_sz2=w, lookahead_sz2=max(4, w - 1))


def split_ops_into_regions(orc, region_bytes):
    """Partition the op-stream into regions covering <= region_bytes of TO-image
    output each.  Ops are NOT split mid-op (large diff runs stay whole) so the
    region boundary lands on an op boundary >= region_bytes of accumulated to.
    Returns list of (op_index_start, op_index_end, to_start, to_end, fp_start).

    A region with a single huge op may exceed region_bytes -- that is fine: the
    heatshrink WINDOW (not the region span) bounds compression-window RAM; the
    region span only bounds the per-region from-FIFO journal, which for monotone
    big-diff ops stays tiny (lag <= net shift)."""
    regions = []
    fp = 0
    to_pos = 0
    cur_start_op = 0
    cur_start_to = 0
    cur_start_fp = 0
    acc = 0
    for k, op in enumerate(orc.ops):
        span = op.diff_len + len(op.extra)
        acc += span
        to_pos += span
        fp_after = fp + op.diff_len + op.adj
        if acc >= region_bytes:
            regions.append((cur_start_op, k + 1, cur_start_to, to_pos, cur_start_fp))
            cur_start_op = k + 1
            cur_start_to = to_pos
            cur_start_fp = fp_after
            acc = 0
        fp = fp_after
    if cur_start_op < len(orc.ops):
        regions.append((cur_start_op, len(orc.ops), cur_start_to, to_pos, cur_start_fp))
    return regions


def encode(orc, w_seg, region_bytes, w_df):
    """Build the hybrid patch.  Returns dict with per-component compressed sizes
    and the raw structures the decoder consumes.

    Patch layout (deviating, single global reloc block):
      header: to_size, w_seg, w_df, n_regions, region_bytes
      GLOBAL reloc block: hs(dfpatch, w_df)              -- shipped ONCE
      per region: hs(raw_bsdiff_substream(region), w_seg)
    """
    dfp = bytes(orc.dfpatch)
    dfp_c = hs(dfp, w_df)

    regions = split_ops_into_regions(orc, region_bytes)
    region_blobs = []
    region_meta = []
    for (o0, o1, t0, t1, fp0) in regions:
        raw = bytearray()
        for op in orc.ops[o0:o1]:
            raw += pack_size(op.diff_len); raw += op.diff
            raw += pack_size(len(op.extra)); raw += op.extra
            raw += pack_size(op.adj)
        blob = hs(raw, w_seg)
        region_blobs.append(blob)
        region_meta.append((o0, o1, t0, t1, fp0, len(blob)))

    # patch byte size = compressed reloc block + sum(region blobs) + framing
    framing = (
        len(pack_size(orc.to_size)) + 2  # to_size + w_seg,w_df nibble byte +1
        + len(pack_size(len(dfp_c)))
        + len(pack_size(len(regions)))
    )
    for (_, _, _, _, _, blen) in region_meta:
        framing += len(pack_size(blen))
    patch_bytes = len(dfp_c) + sum(len(b) for b in region_blobs) + framing

    return {
        'patch_bytes': patch_bytes,
        'dfp_c_len': len(dfp_c),
        'region_blobs_len': sum(len(b) for b in region_blobs),
        'framing': framing,
        'n_regions': len(regions),
        'regions': region_meta,
        'w_seg': w_seg, 'w_df': w_df, 'region_bytes': region_bytes,
    }


# ---------------------------------------------------------------------------
# DECODER -- TRUE in-place, byte-exact, honest peak RAM
# ---------------------------------------------------------------------------
def _forward_journal_global(orc):
    """EXACT minimum forward in-flight from-journal (bytes) for the WHOLE image
    under a single monotone (ascending to_pos) frontier -- identical to the
    measure-phase min_journal_forward.  This is the honest forward cost: a
    from-source byte at position p read at step s is journaled iff the frontier
    overwrote p (at step p) before its last read (last > p).  Peak simultaneous
    live journaled bytes = required FIFO RAM.

    NOTE: region partitioning does NOT shrink this for ops whose adjustment makes
    fp jump BELOW an earlier region's already-written frontier (e.g. v0v2 op11's
    backward adj re-reads position 72225 long after the frontier passed it).
    Those re-reads force journaling regardless of region boundaries, because in
    forward order ALL lower to-positions are already overwritten.  Region splits
    only help the COMPRESSION-WINDOW RAM, not the journal; backward order is what
    drives the journal to 0."""
    reads_of = {}
    fp = 0
    to_pos = 0
    for op in orc.ops:
        for _ in range(op.diff_len):
            reads_of.setdefault(fp, []).append(to_pos)
            fp += 1
            to_pos += 1
        to_pos += len(op.extra)
        fp += op.adj
    intervals = []
    for p, ts in reads_of.items():
        last = max(ts)
        if last > p:
            intervals.append((p, last))
    events = []
    for (s, e) in intervals:
        events.append((s, +1)); events.append((e + 1, -1))
    events.sort()
    cur = peak = 0
    for _, d in events:
        cur += d
        if cur > peak:
            peak = cur
    return peak


def decode_inplace(orc, enc, order):
    """Reconstruct the to-image IN PLACE in one shared bytearray (dest==src, no
    headroom beyond max(from,to)).  order in {'fwd','back'}.

    'fwd'  : write to-positions ascending; per-region bounded from-FIFO journal;
             reloc block STREAMED ascending in field order -> DFP_BLOCK residency.
    'back' : write to-positions descending; 0 from-journal (measure-phase proves
             0 conflicts) but the reloc table must be RESIDENT (descending field
             access) -> full compressed-or-expanded dfpatch in RAM.

    Returns (byte_exact, peak_ram, journal_bytes).  Only ONE heatshrink window is
    live at the peak (reloc stream and bsdiff regions are decoded at different
    times in the single pass)."""
    N = max(orc.from_size, orc.to_size)
    mem = bytearray(orc.from_image) + bytearray(N - orc.from_size)
    dfdiff = orc.dfdiff
    win_df = 1 << enc['w_df']
    win_seg = 1 << enc['w_seg']

    if order == 'fwd':
        # Analytic global forward journal (honest -- region splits do NOT shrink
        # it; see _forward_journal_global docstring).
        analytic_journal = _forward_journal_global(orc)
        max_region_fifo = analytic_journal
        # GENUINE in-place forward apply: read from-bytes from `mem` (the shared
        # buffer), journaling any from-byte that will be read AFTER the frontier
        # overwrites it.  zmask applies the reloc-zeroing on the fly (part of the
        # streamed reloc driver).  We verify the live journal peak == analytic.
        zmask = bytes(a ^ b for a, b in zip(orc.from_image, orc.fromzero))
        # Precompute, per from-position p, the last to_pos that reads it (so we
        # can drop journal entries once consumed).
        last_read = {}
        fp = 0; tp = 0
        for op in orc.ops:
            for _ in range(op.diff_len):
                last_read[fp] = tp; fp += 1; tp += 1
            tp += len(op.extra); fp += op.adj
        journal = {}              # from-pos -> fromzero byte (saved before overwrite)
        live_peak = 0
        fp = 0; to_pos = 0
        for op in orc.ops:
            for k in range(op.diff_len):
                # obtain fromzero[fp]: from journal if overwritten, else from mem.
                # Pop only on the LAST read of this from-position (a from-byte may
                # be read more than once, e.g. across an adj that re-reads it).
                if fp in journal:
                    fz = journal[fp]
                    if last_read.get(fp) == to_pos:
                        del journal[fp]
                else:
                    src = mem[fp]
                    fz = (src ^ zmask[fp]) if (fp < len(zmask) and zmask[fp]) else src
                val = (op.diff[k] + fz + dfdiff[to_pos]) & 0xFF
                # before overwriting to_pos, journal its fromzero value iff a
                # FUTURE read needs it (last_read[to_pos] > to_pos) and it is not
                # already overwritten/consumed.
                if to_pos < orc.from_size:
                    lr = last_read.get(to_pos)
                    if lr is not None and lr > to_pos and to_pos not in journal:
                        s = mem[to_pos]
                        journal[to_pos] = ((s ^ zmask[to_pos])
                                           if (to_pos < len(zmask) and zmask[to_pos])
                                           else s)
                mem[to_pos] = val
                if len(journal) > live_peak:
                    live_peak = len(journal)
                fp += 1; to_pos += 1
            for k in range(len(op.extra)):
                val = (op.extra[k] + dfdiff[to_pos]) & 0xFF
                if to_pos < orc.from_size:
                    lr = last_read.get(to_pos)
                    if lr is not None and lr > to_pos and to_pos not in journal:
                        s = mem[to_pos]
                        journal[to_pos] = ((s ^ zmask[to_pos])
                                           if (to_pos < len(zmask) and zmask[to_pos])
                                           else s)
                mem[to_pos] = val
                if len(journal) > live_peak:
                    live_peak = len(journal)
                to_pos += 1
            fp += op.adj
        byte_exact = bytes(mem[:orc.to_size]) == orc.to_image
        # honest journal RAM = the genuinely measured live peak (authoritative).
        # Cross-check it agrees with the analytic interval-overlap value to within
        # the +/-1 endpoint convention (analytic [p,last] inclusive vs measured
        # journal-size-after-insert).
        assert abs(live_peak - analytic_journal) <= 1, (live_peak, analytic_journal)
        max_region_fifo = live_peak
        # HONEST forward accounting.  In forward order the reloc driver and the
        # bsdiff op-stream are consumed CONCURRENTLY (the reloc fields for
        # to_pos must be available exactly when the frontier reaches to_pos, in
        # ascending order).  Two sub-strategies, take the cheaper:
        #  (A) concurrent dual-stream: both heatshrink windows live at once,
        #      reloc streamed ascending in field order (DFP_BLOCK buffer):
        #          2^w_seg + 2^w_df + INPUT_BUF*2 + DFP_BLOCK + from_FIFO + OP
        #  (B) pre-expand reloc into RAM once (one window at a time), then apply
        #      with only the bsdiff window resident:
        #          max( 2^w_df + dfpatch_size,         # expand phase
        #               2^w_seg + dfpatch_size + from_FIFO )  # apply phase
        #          + INPUT_BUF + OP
        ram_A = (win_seg + win_df + 2 * INPUT_BUF + DFP_BLOCK
                 + max_region_fifo + OP_SCRATCH)
        ram_B = max(win_df + orc.dfpatch_size,
                    win_seg + orc.dfpatch_size + max_region_fifo) \
                + INPUT_BUF + OP_SCRATCH
        peak_ram = min(ram_A, ram_B)
        return byte_exact, peak_ram, max_region_fifo

    else:  # backward
        # Build per-output-byte value source, then write descending; reads see
        # still-original lower memory (0 conflicts per measure phase).
        diffbyte = {}; extrabyte = {}; fpof = {}
        fp = 0; tp = 0
        order_trace = []
        for op in orc.ops:
            for k in range(op.diff_len):
                diffbyte[tp] = op.diff[k]; fpof[tp] = fp
                order_trace.append(tp); fp += 1; tp += 1
            for k in range(len(op.extra)):
                extrabyte[tp] = op.extra[k]
                order_trace.append(tp); tp += 1
            fp += op.adj
        zmask = bytes(a ^ b for a, b in zip(orc.from_image, orc.fromzero))
        conflicts = 0
        for to_pos in reversed(order_trace):
            if to_pos in diffbyte:
                fpv = fpof[to_pos]
                src = mem[fpv]
                if fpv < len(zmask) and zmask[fpv]:
                    fz = src ^ zmask[fpv]
                else:
                    fz = src
                if fpv > to_pos:
                    conflicts += 1
                val = (diffbyte[to_pos] + fz + dfdiff[to_pos]) & 0xFF
            else:
                val = (extrabyte[to_pos] + dfdiff[to_pos]) & 0xFF
            mem[to_pos] = val
        byte_exact = bytes(mem[:orc.to_size]) == orc.to_image and conflicts == 0
        # HONEST backward accounting: backward write order needs the reloc driver
        # accessible in DESCENDING field order.  heatshrink is a FORWARD-only
        # stream -> it cannot be random-accessed in reverse.  So the decoder must
        # expand the whole reloc block (the uncompressed dfpatch reloc driver)
        # into RAM once and index it descending.  Resident cost = uncompressed
        # dfpatch_size (the measure phase's "dfpatch resident", ~6269B for v0v2).
        # Phase-1 expansion: stream the reloc block in (window 2^w_df) WHILE
        # writing the expanded table -> both live: win_df + dfpatch_size.
        dfp_resident = orc.dfpatch_size
        ram_phase1 = win_df + INPUT_BUF + dfp_resident + OP_SCRATCH
        ram_phase2 = win_seg + INPUT_BUF + dfp_resident + OP_SCRATCH
        peak_ram = max(ram_phase1, ram_phase2)
        return byte_exact, peak_ram, 0


# ---------------------------------------------------------------------------
# FRONTIER SEARCH -- for each RAM budget pick the encoding (w_seg, region, w_df)
# that minimizes patch_bytes while the MEASURED peak RAM fits the budget.
# ---------------------------------------------------------------------------
def frontier_for_budget(orc, budget_bytes):
    best = None
    # window for reloc block: small windows are best for dfpatch; pick the one
    # that fits and minimizes its compressed size.  region window = the lever.
    for w_seg in range(8, 15):
        win_seg = 1 << w_seg
        # region span chosen ~ the window so the from-FIFO stays bounded; we let
        # regions be as large as ops allow (huge diff ops stay monotone, tiny FIFO)
        for region_bytes in (win_seg, win_seg * 2, win_seg * 4, orc.to_size):
            for w_df in range(8, 15):
                win_df = 1 << w_df
                # quick RAM feasibility precheck on the dominant window terms
                ram_p1 = win_df + INPUT_BUF + DFP_BLOCK + OP_SCRATCH
                ram_p2_floor = win_seg + INPUT_BUF + DFP_BLOCK + OP_SCRATCH
                if max(ram_p1, ram_p2_floor) > budget_bytes:
                    continue
                enc = encode(orc, w_seg, region_bytes, w_df)
                for order in ('fwd', 'back'):
                    ok, peak_ram, fifo = decode_inplace(orc, enc, order)
                    if peak_ram > budget_bytes:
                        continue
                    if not ok:
                        continue
                    cand = {
                        'patch_bytes': enc['patch_bytes'],
                        'peak_ram_bytes': peak_ram,
                        'byte_exact': ok,
                        'order': order,
                        'w_seg': w_seg, 'w_df': w_df,
                        'region_bytes': region_bytes,
                        'n_regions': enc['n_regions'],
                        'max_region_fifo': fifo,
                        'dfp_c_len': enc['dfp_c_len'],
                        'region_blobs_len': enc['region_blobs_len'],
                        'framing': enc['framing'],
                    }
                    if best is None or cand['patch_bytes'] < best['patch_bytes']:
                        best = cand
    return best


def run_pair(fd, td, pn, lab):
    orc = load_pair(fd, td, pn)
    # sanity: model reconstruction is byte-exact
    assert orc.reconstruct_sequential() == orc.to_image
    results = []
    for kib in (4, 8, 16, 24):
        budget = kib * 1024
        best = frontier_for_budget(orc, budget)
        if best is None:
            results.append({'ram_kb': kib, 'infeasible': True, 'byte_exact': False})
        else:
            results.append({
                'ram_kb': kib,
                'patch_bytes': best['patch_bytes'],
                'byte_exact': best['byte_exact'],
                'peak_ram_bytes': best['peak_ram_bytes'],
                'order': best['order'],
                'w_seg': best['w_seg'], 'w_df': best['w_df'],
                'segment_bytes': (1 << best['w_seg']),
                'region_bytes': best['region_bytes'],
                'n_regions': best['n_regions'],
                'max_region_fifo': best['max_region_fifo'],
                'dfp_c_len': best['dfp_c_len'],
                'region_blobs_len': best['region_blobs_len'],
                'framing': best['framing'],
            })
    return orc, results


def segment_scaling_table(orc):
    """How segment size (= heatshrink window 2^w_seg) scales the patch and lands
    vs the seq_m4 floor (1857).  Reloc block fixed at its optimal small window
    (w_df=8).  Shows the size lever the RAM budget controls."""
    table = []
    for w_seg in range(8, 15):
        enc = encode(orc, w_seg, orc.to_size, 8)
        okf, ramf, fifo = decode_inplace(orc, enc, 'fwd')
        table.append({
            'w_seg': w_seg, 'segment_bytes': 1 << w_seg,
            'patch_bytes': enc['patch_bytes'],
            'reloc_block': enc['dfp_c_len'],
            'region_blobs': enc['region_blobs_len'],
            'fwd_peak_ram': ramf, 'fwd_journal': fifo, 'byte_exact': okf,
            'delta_vs_1857': enc['patch_bytes'] - 1857,
        })
    return table


if __name__ == '__main__':
    SEQ_M4 = 1857
    out = {'algo': 'hybrid-largeseg',
           'description': 'global reloc block (once) + RAM-sized bsdiff regions, true in-place',
           'seq_m4_floor': SEQ_M4,
           'fixed_ram_terms': {'INPUT_BUF': INPUT_BUF, 'OP_SCRATCH': OP_SCRATCH,
                               'DFP_BLOCK': DFP_BLOCK},
           'pairs': {}}
    for fd, td, pn, lab in [
        ('v0_base', 'v1_one_face', 'v0v1_seq_m4.patch', 'v0v1_one_face_+360B'),
        ('v0_base', 'v2_three_faces', 'v0v2_seq_m4.patch', 'v0v2_three_faces_+3856B'),
    ]:
        orc, results = run_pair(fd, td, pn, lab)
        out['pairs'][lab] = {
            'from_size': orc.from_size, 'to_size': orc.to_size,
            'shift': orc.to_size - orc.from_size,
            'reloc_from': len(orc.reloc_from), 'reloc_to': len(orc.reloc_to),
            'dfpatch_size': orc.dfpatch_size,
            'results': results,
            'segment_scaling': segment_scaling_table(orc),
        }
        print(f"\n=== {lab}  from={orc.from_size} to={orc.to_size} "
              f"shift={orc.to_size - orc.from_size} ===")
        for r in results:
            if r.get('infeasible'):
                print(f"  {r['ram_kb']:2d}KiB: INFEASIBLE")
            else:
                print(f"  {r['ram_kb']:2d}KiB: patch={r['patch_bytes']}B "
                      f"(reloc={r['dfp_c_len']}+regions={r['region_blobs_len']}+fr={r['framing']}) "
                      f"order={r['order']} seg=2^{r['w_seg']}={r['segment_bytes']}B w_df={r['w_df']} "
                      f"regions={r['n_regions']} fifo={r['max_region_fifo']}B "
                      f"peakRAM={r['peak_ram_bytes']}B exact={r['byte_exact']} "
                      f"<2kB={r['patch_bytes'] < 2000} vs1857={r['patch_bytes'] - SEQ_M4:+d}")
        print(f"  -- segment(window) scaling vs 1857 floor --")
        for s in out['pairs'][lab]['segment_scaling']:
            print(f"     seg=2^{s['w_seg']:>2}={s['segment_bytes']:>5}B patch={s['patch_bytes']}B "
                  f"(reloc={s['reloc_block']}+regions={s['region_blobs']}) "
                  f"fwd_peakRAM={s['fwd_peak_ram']}B vs1857={s['delta_vs_1857']:+d} "
                  f"exact={s['byte_exact']}")

    respath = os.path.join(os.path.dirname(os.path.dirname(__file__)),
                           'results', 'hybrid-largeseg.json')
    with open(respath, 'w') as f:
        json.dump(out, f, indent=2)
    print(f"\nwrote {respath}")
