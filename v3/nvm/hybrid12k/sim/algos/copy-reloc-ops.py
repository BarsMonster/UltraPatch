#!/usr/bin/env python3
"""copy-reloc-ops.py -- purpose-built opcode-stream in-place firmware patcher.

DEVIATION FROM DETOOLS: detools ships a per-output-byte bsdiff op-stream
(diff_len + diff bytes + extra_len + extra + adjustment) PLUS the m4 dfpatch
reloc side-table, then (in its in-place mode) chops the image into 8KiB segments
and re-ships reloc context per segment -> 2892-4192 B.

We instead ship ONE global patch with a tiny purpose-built CONTROL stream of
three opcode families tailored to the MEASURED access pattern (linear_fraction
== 1.0, working-set ~= net insertion shift):

    COPY(len)        : to[tp:tp+len] = fromzero[fp:fp+len] + dfdiff[tp:tp+len]
                       advances both fp and tp (the long monotone copy runs;
                       this single op covers BOTH pure-copy bytes AND reloc-only
                       bytes, because dfdiff supplies the re-encoded reloc bytes
                       and fromzero supplies the copied bytes).
    LITERAL(gap,byte): a sparse additive patch byte at tp = prev+gap+1
                       (the 0.8% genuinely-changed code bytes); applied on top of
                       the COPY value:  to[tp] += byte.
    INSERT(len,bytes): brand-new content (extra): to[tp:tp+len] = bytes + dfdiff.
    ADJUST(delta)    : reposition the from-pointer fp by a signed delta
                       (handles the small reorder spans / deletions).

The global m4 ADDRESS-BLOCKS are detools' dfpatch reloc side-table, REUSED
verbatim (it is the irreducible relocation content: re-encoded Thumb bl/b.w/
ldr/ldr.w + data/code pointer fields). It drives two derived overlays:
    * dfdiff  : to_size forward overlay, nonzero only inside to-reloc fields
    * fromzero: from-image with from-reloc fields zeroed
Both are deterministic functions of (from-image, dfpatch); on device they are
produced by disassembling the from-image and applying the dfpatch block deltas
in ascending from-address order -- i.e. they STREAM forward, matching our
forward COPY frontier, so neither needs to be fully materialized.

Wire format (two heatshrink-compressed components):
    [ctrl_stream]   COPY/LITERAL/INSERT/ADJUST opcodes (see encode_control)
    [dfpatch]       detools m4 reloc side-table (unchanged)

RAM ACCOUNTING (everything resident at once, honest):
    heatshrink decode window           = 2^w  bytes (one window, reused per
                                         component since components decode
                                         sequentially, not concurrently)
    input/codeword buffer              = 256  bytes
    from-FIFO journal (forward order)  = net-insertion-shift bytes (the scratch
                                         ring for read/write hazards)
    dfpatch residency / stream buffer  : forward -> streamed in from-addr order,
                                         small block buffer; backward -> resident
    dfdiff field scratch + op scratch  = small constant

We benchmark BOTH orderings and pick, per RAM budget, the smallest byte-exact
patch that fits.

Run:  python3 algos/copy-reloc-ops.py
Writes results/copy-reloc-ops.json with MEASURED patch_bytes + peak_ram.
"""
import sys
import os
import json
import struct

_HERE = os.path.dirname(os.path.abspath(__file__))
_SIM = os.path.dirname(_HERE)
sys.path.insert(0, _SIM)
sys.path.insert(0, '/ai_sw/detools-dev')

from m4_oracle import load_pair
from detools.common import pack_size, unpack_size
from detools import bsdiff
from heatshrink2 import compress, decompress

# --- fixed-size scratch buffers counted in the RAM budget -------------------
INPUT_BUF = 256          # erase-row / codeword input staging
OP_SCRATCH = 128         # opcode decode registers + dfdiff field scratch
DFPATCH_STREAM_BUF = 512  # forward-order: resident block window of dfpatch


# ============================================================================
# ENCODER
# ============================================================================
def encode_control(orc):
    """Emit the purpose-built control opcode stream for the oracle's op list.

    Layout (all integers via detools varints; signed via pack_size/zigzag):
        n_ops
        per op:
            COPY  : copy_len                       (unsigned)
            LITS  : n_lit                          (unsigned)
                    n_lit * ( gap , byte )         (gap unsigned, byte raw)
            INS   : extra_len                      (unsigned)
                    extra_len raw bytes
            ADJ   : adjustment                     (signed via pack_size)
    The decoder reconstructs to[] = COPY(fromzero+dfdiff) then += LITERAL,
    then INSERT(extra+dfdiff), then moves fp by ADJ.
    """
    out = bytearray()
    out += pack_size(len(orc.ops))
    for op in orc.ops:
        out += pack_size(op.diff_len)
        lits = [(k, b) for k, b in enumerate(op.diff) if b]
        out += pack_size(len(lits))
        prev = 0
        for k, b in lits:
            out += pack_size(k - prev)
            out.append(b)
            prev = k
        out += pack_size(len(op.extra))
        out += op.extra
        out += pack_size(op.adj)
    return bytes(out)


def hs(data, w):
    return compress(bytes(data), window_sz2=w, lookahead_sz2=max(3, w - 1))


def encode_patch(orc, w):
    """Return (ctrl_compressed, dfpatch_compressed, raw_ctrl_len)."""
    ctrl = encode_control(orc)
    cz = hs(ctrl, w)
    dz = hs(orc.dfpatch, w)
    return cz, dz, len(ctrl)


# ============================================================================
# DECODER -- parses the control stream back into op tuples
# ============================================================================
class _Reader:
    def __init__(self, data):
        self.d = data
        self.i = 0

    def u(self):
        # unsigned varint compatible with pack_size/unpack_size
        val = self.d[self.i]
        self.i += 1
        if (val & 0x80) == 0:
            # tiny path not used by detools pack_size; fall through to full
            pass
        # use detools unpack_size via a tiny stream shim
        return None  # replaced below


def decode_control(ctrl_compressed, w):
    """Decompress + parse the control stream into a list of decoded ops:
        [(copy_len, [(abs_tp_offset_within_op, byte)...], extra_bytes, adj)]
    Returns (ops, decode_window_bytes_used)."""
    raw = decompress(bytes(ctrl_compressed), window_sz2=w, lookahead_sz2=max(3, w - 1))
    # parse with detools varint helpers via an index closure
    pos = [0]

    def rdu():
        # mimic unpack_size over a bytes object
        byte = raw[pos[0]]
        pos[0] += 1
        is_signed = (byte & 0x40)
        value = (byte & 0x3f)
        offset = 6
        while byte & 0x80:
            byte = raw[pos[0]]
            pos[0] += 1
            value |= ((byte & 0x7f) << offset)
            offset += 7
        if is_signed:
            value *= -1
        return value

    n_ops = rdu()
    ops = []
    for _ in range(n_ops):
        copy_len = rdu()
        n_lit = rdu()
        lits = []
        prev = 0
        for _ in range(n_lit):
            gap = rdu()
            k = prev + gap
            byte = raw[pos[0]]
            pos[0] += 1
            lits.append((k, byte))
            prev = k
        extra_len = rdu()
        extra = raw[pos[0]:pos[0] + extra_len]
        pos[0] += extra_len
        adj = rdu()
        ops.append((copy_len, lits, bytes(extra), adj))
    return ops, (1 << w)


# ============================================================================
# DERIVED OVERLAYS (dfdiff + fromzero) -- in the SIM we obtain them from the
# oracle (which builds them via detools create_readers). On device they are
# produced by disassembling the from-image + applying dfpatch blocks; the SIM's
# RAM model below charges only the bounded resident structures, not these full
# arrays (they stream).
# ============================================================================


# ============================================================================
# TRUE IN-PLACE APPLY -- forward and backward, single shared buffer, byte-exact
# ============================================================================
def _last_read_of_source(ops):
    """Return dict source-position -> last output-step at which it is read as a
    fromzero source (for correct journal retention). Output-step == tp."""
    last_read = {}
    fp = 0
    tp = 0
    for (copy_len, lits, extra, adj) in ops:
        for k in range(copy_len):
            last_read[fp] = tp     # later assignments overwrite => keeps max tp
            fp += 1
            tp += 1
        tp += len(extra)
        fp += adj
    return last_read


def apply_forward(orc, ops):
    """Forward write order in ONE shared bytearray (dest==src, no headroom
    beyond max(from,to)). Keeps a bounded journal of fromzero source bytes that
    are overwritten by the frontier BEFORE they are read. A journaled byte is
    retained until its LAST read (computed from the plan), so non-monotone fp
    jumps (negative adjustments) are handled correctly. Returns
    (byte_exact, peak_journal_bytes)."""
    N = max(orc.from_size, orc.to_size)
    mem = bytearray(orc.from_image) + bytearray(N - orc.from_size)
    zmask = bytes(a ^ b for a, b in zip(orc.from_image, orc.fromzero))
    dfdiff = orc.dfdiff
    last_read = _last_read_of_source(ops)
    journal = {}          # source-pos -> saved fromzero byte (alive until last read)
    peak = 0
    fp = 0
    tp = 0

    def fromzero_at(p):
        if p in journal:
            return journal[p]
        b = mem[p]
        if p < len(zmask) and zmask[p]:
            b ^= zmask[p]
        return b

    for (copy_len, lits, extra, adj) in ops:
        litmap = dict(lits)
        for k in range(copy_len):
            fz = fromzero_at(fp)
            v = (fz + dfdiff[tp]) & 0xFF
            if k in litmap:
                v = (v + litmap[k]) & 0xFF
            # Before overwriting position tp: if its (zeroed) from-value is read
            # AFTER now (last_read[tp] > tp), save it; otherwise it is dead.
            if last_read.get(tp, -1) > tp:
                srcz = mem[tp]
                if tp < len(zmask) and zmask[tp]:
                    srcz ^= zmask[tp]
                journal[tp] = srcz
            mem[tp] = v
            # the source we just consumed (fp) is dead if this was its last read
            if last_read.get(fp, -1) <= tp and fp in journal:
                del journal[fp]
            if len(journal) > peak:
                peak = len(journal)
            fp += 1
            tp += 1
        for k in range(len(extra)):
            # extra bytes never reads a from-source; still preserve a from-value
            # at tp if a later op reads it.
            if last_read.get(tp, -1) > tp:
                srcz = mem[tp]
                if tp < len(zmask) and zmask[tp]:
                    srcz ^= zmask[tp]
                journal[tp] = srcz
            v = (extra[k] + dfdiff[tp]) & 0xFF
            mem[tp] = v
            tp += 1
            if len(journal) > peak:
                peak = len(journal)
        fp += adj
    ok = bytes(mem[:orc.to_size]) == orc.to_image
    return ok, peak


def apply_backward(orc, ops):
    """Backward write order in ONE shared bytearray. Frontier descends
    to_size-1..0; sources are read from still-original lower memory -> 0 journal.
    dfpatch must be fully resident (descending field access). Returns
    (byte_exact, conflicts)."""
    N = max(orc.from_size, orc.to_size)
    mem = bytearray(orc.from_image) + bytearray(N - orc.from_size)
    zmask = bytes(a ^ b for a, b in zip(orc.from_image, orc.fromzero))
    dfdiff = orc.dfdiff
    # Build the per-output-byte plan: (tp, kind, fp, addbyte) in forward order.
    plan = []
    fp = 0
    tp = 0
    for (copy_len, lits, extra, adj) in ops:
        litmap = dict(lits)
        for k in range(copy_len):
            plan.append((tp, 'copy', fp, litmap.get(k, 0)))
            fp += 1
            tp += 1
        for k in range(len(extra)):
            plan.append((tp, 'ins', None, extra[k]))
            tp += 1
        fp += adj
    conflicts = 0
    for (tp, kind, fp, addbyte) in reversed(plan):
        if kind == 'copy':
            if fp > tp:
                conflicts += 1
            b = mem[fp]
            if fp < len(zmask) and zmask[fp]:
                b ^= zmask[fp]
            v = (b + dfdiff[tp] + addbyte) & 0xFF
        else:
            v = (addbyte + dfdiff[tp]) & 0xFF
        mem[tp] = v
    ok = bytes(mem[:orc.to_size]) == orc.to_image
    return ok, conflicts


# ============================================================================
# FRONTIER: size vs RAM, MEASURED (decode the real compressed stream, apply
# true-in-place, verify byte-exact, count all resident RAM).
# ============================================================================
def peak_ram_forward(orc, w, fifo_peak):
    """Honest concurrent accounting. During apply, the reconstruction consumes
    BOTH streams concurrently (the control opcodes drive the frontier; the
    dfpatch-derived dfdiff/fromzero overlays supply reloc/source bytes). So we
    charge TWO live heatshrink decode windows (control + dfpatch) plus the
    input codeword buffer, opcode/dfdiff scratch, the bounded dfpatch block-
    cursor stage, and the from-FIFO journal (the scratch ring for hazards)."""
    window = 1 << w
    return (2 * window + INPUT_BUF + OP_SCRATCH + DFPATCH_STREAM_BUF + fifo_peak)


def peak_ram_backward(orc, w):
    """Backward write order: 0 from-journal, but the dfpatch must be fully
    resident (descending field access into the reloc table). One control-decode
    window is live concurrently. dfdiff is derived from the resident dfpatch +
    from-image disassembly on demand."""
    window = 1 << w
    return (window + INPUT_BUF + OP_SCRATCH + orc.dfpatch_size)


def run_pair(orc, label):
    # sanity: model reconstructs
    assert orc.reconstruct_sequential() == orc.to_image
    budgets = [4, 8, 16, 24]
    results = []
    detail = {}
    # candidate window sizes
    ws = list(range(8, 15))
    # Precompute per-w: compressed sizes + byte-exact in-place verification.
    cand = []  # (w, patch_bytes, fwd_ok, fifo_peak, back_ok)
    for w in ws:
        cz, dz, raw_ctrl = encode_patch(orc, w)
        patch_bytes = len(cz) + len(dz)
        ops, _ = decode_control(cz, w)
        fwd_ok, fifo_peak = apply_forward(orc, ops)
        back_ok, conflicts = apply_backward(orc, ops)
        cand.append({
            'w': w, 'patch': patch_bytes, 'ctrl': len(cz), 'dfp': len(dz),
            'raw_ctrl': raw_ctrl, 'fwd_ok': fwd_ok, 'fifo': fifo_peak,
            'back_ok': back_ok, 'conflicts': conflicts,
        })
    detail['candidates'] = cand

    for kib in budgets:
        budget = kib * 1024
        best = None
        for c in cand:
            w = c['w']
            # forward option
            if c['fwd_ok']:
                ram = peak_ram_forward(orc, w, c['fifo'])
                if ram <= budget:
                    if best is None or c['patch'] < best['patch_bytes']:
                        best = {'patch_bytes': c['patch'], 'order': 'fwd',
                                'w': w, 'peak_ram': ram, 'byte_exact': True}
            # backward option
            if c['back_ok'] and c['conflicts'] == 0:
                ram = peak_ram_backward(orc, w)
                if ram <= budget:
                    if best is None or c['patch'] < best['patch_bytes']:
                        best = {'patch_bytes': c['patch'], 'order': 'back',
                                'w': w, 'peak_ram': ram, 'byte_exact': True}
        if best is None:
            results.append({'ram_kb': kib, 'patch_bytes': -1,
                            'byte_exact': False, 'peak_ram_bytes': -1})
        else:
            results.append({'ram_kb': kib, 'patch_bytes': best['patch_bytes'],
                            'byte_exact': True,
                            'peak_ram_bytes': best['peak_ram'],
                            'order': best['order'], 'w': best['w']})
    return results, detail


if __name__ == '__main__':
    out = {}
    for fd, td, pn, lab in [('v0_base', 'v1_one_face', 'v0v1_seq_m4.patch', 'v0v1_one_face'),
                            ('v0_base', 'v2_three_faces', 'v0v2_seq_m4.patch', 'v0v2_three_faces')]:
        orc = load_pair(fd, td, pn)
        res, detail = run_pair(orc, lab)
        out[lab] = {
            'from_size': orc.from_size, 'to_size': orc.to_size,
            'shift': orc.to_size - orc.from_size,
            'dfpatch_size': orc.dfpatch_size,
            'results': res,
            'detail': detail,
        }
        print(f"=== {lab}: from={orc.from_size} to={orc.to_size} shift={orc.to_size-orc.from_size} ===")
        for c in detail['candidates']:
            print(f"  w={c['w']:2d} patch={c['patch']:5d}B (ctrl={c['ctrl']} dfp={c['dfp']}) "
                  f"fwd_ok={c['fwd_ok']} fifo={c['fifo']}B back_ok={c['back_ok']} confl={c['conflicts']}")
        for r in res:
            if r['byte_exact']:
                print(f"  {r['ram_kb']:2d}KiB: patch={r['patch_bytes']}B order={r['order']} "
                      f"w={r['w']} peak_ram={r['peak_ram_bytes']}B <2kB={r['patch_bytes']<2000}")
            else:
                print(f"  {r['ram_kb']:2d}KiB: INFEASIBLE (byte_exact=False)")
    resdir = os.path.join(_SIM, 'results')
    os.makedirs(resdir, exist_ok=True)
    with open(os.path.join(resdir, 'copy-reloc-ops.json'), 'w') as f:
        json.dump(out, f, indent=2)
    print("wrote results/copy-reloc-ops.json")
