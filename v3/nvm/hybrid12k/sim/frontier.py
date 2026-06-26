#!/usr/bin/env python3
"""frontier.py -- achieved-patch-size vs SRAM-budget frontier for a DEVIATING
single-global-patch TRUE-in-place arm-cortex-m4 applier.

DEVIATION from detools' per-segment in-place scheme: detools chops the image into
fixed segments (8 KiB) and re-ships reloc context per segment, inflating the
in-place m4 patch to ~2892-4192 B and materializing a full to_size overlay +
expanded reloc table (~18 KiB). We instead ship ONE global m4 patch == the
sequential m4 floor (the bsdiff op-stream of the reloc-zeroed images + the dfpatch
reloc side-table) and apply it true-in-place. The access trace (m4_inplace_sim.py)
proves this is safe with a tiny working set:
  * backward write order  -> 0 from-journal, dfpatch resident.
  * forward  write order  -> from-FIFO journal == net insertion shift, dfpatch streamed.

RAM accounting counts EVERYTHING resident at once:
  heatshrink window (2^w) + 256 B input buf + dfpatch table/stream buf
  + from-journal/FIFO + op/codeword scratch.
"""
import sys, os
sys.path.insert(0, os.path.dirname(__file__)); sys.path.insert(0, '/ai_sw/detools-dev')
from m4_oracle import load_pair
from m4_inplace_sim import min_journal_forward
from heatshrink2 import compress
from detools.common import pack_size

OP_SCRATCH = 256
INPUT_BUF = 256
STREAM_BUF = 512   # forward-order dfpatch block-decode buffer


def hs(data, w, l):
    return len(compress(bytes(data), window_sz2=w, lookahead_sz2=l))


def raw_bsdiff(orc):
    raw = bytearray()
    for op in orc.ops:
        raw += pack_size(op.diff_len); raw += op.diff
        raw += pack_size(len(op.extra)); raw += op.extra
        raw += pack_size(op.adj)
    return bytes(raw)


def frontier(orc):
    raw = raw_bsdiff(orc)
    dfp = orc.dfpatch
    shift = orc.to_size - orc.from_size
    jf_peak, _, _ = min_journal_forward(orc)        # minimum forward journal (byte-exact)
    rows = []
    for budget_kib in (4, 8, 16, 24):
        budget = budget_kib * 1024
        best = None
        for w in range(8, 15):
            l = w - 1
            win = 1 << w
            ps = hs(raw, w, l) + hs(dfp, w, l)
            # backward: dfpatch fully resident, 0 journal
            ram_back = win + INPUT_BUF + len(dfp) + OP_SCRATCH
            # forward: dfpatch streamed ascending + from-FIFO (min journal)
            ram_fwd = win + INPUT_BUF + STREAM_BUF + OP_SCRATCH + jf_peak
            for mode, ram in (('back', ram_back), ('fwd', ram_fwd)):
                if ram <= budget and (best is None or ps < best['patch']):
                    best = {'patch': ps, 'w': w, 'order': mode, 'ram': ram}
        rows.append((budget_kib, best))
    return rows, shift, jf_peak


if __name__ == '__main__':
    import json
    out = {}
    for fd, td, pn, lab in [('v0_base', 'v1_one_face', 'v0v1_seq_m4.patch', 'v0v1'),
                            ('v0_base', 'v2_three_faces', 'v0v2_seq_m4.patch', 'v0v2')]:
        orc = load_pair(fd, td, pn)
        rows, shift, jf = frontier(orc)
        out[lab] = {'from': orc.from_size, 'to': orc.to_size, 'shift': shift,
                    'min_fwd_journal': jf, 'frontier': []}
        print(f"=== {lab} from={orc.from_size} to={orc.to_size} shift={shift} "
              f"min_fwd_journal={jf}B ===")
        for kib, b in rows:
            if b:
                print(f"  {kib:2d}KiB: patch={b['patch']}B order={b['order']} "
                      f"w={b['w']} RAM={b['ram']}B(<{kib}KiB) <2kB={b['patch']<2000}")
                out[lab]['frontier'].append(
                    {'ram_kib': kib, 'patch': b['patch'], 'order': b['order'],
                     'w': b['w'], 'ram_bytes': b['ram'], 'sub2k': b['patch'] < 2000})
            else:
                print(f"  {kib:2d}KiB: INFEASIBLE")
                out[lab]['frontier'].append({'ram_kib': kib, 'infeasible': True})
    with open(os.path.join(os.path.dirname(__file__), 'artifacts', 'frontier.json'), 'w') as f:
        json.dump(out, f, indent=2)
    print("wrote artifacts/frontier.json")
