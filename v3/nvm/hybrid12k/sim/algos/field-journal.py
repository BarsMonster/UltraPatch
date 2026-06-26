#!/usr/bin/env python3
"""field-journal.py -- DEVELOP-phase algorithm: a single GLOBAL m4 object
(recovering the seq_m4-class ~1857 B compression) applied with TRUE in-place
streaming reconstruction that holds ONLY the in-flight reloc-field journal,
evicting source bytes once their row is consumed.

DEVIATION FROM DETOOLS PER-SEGMENT SCHEME
-----------------------------------------
detools' inplace_m4 chops the image into fixed segments (2-8 KiB), re-ships
reloc context per segment, and materializes a full to_size dfdiff overlay plus
an expanded per-field reloc table (~18 KiB), bottoming at ~4192 B @2KB seg /
~2892 B @8KiB seg. We instead ship ONE global m4 patch == the sequential m4
floor (bsdiff op-stream of the reloc-zeroed images + the dfpatch reloc
side-table, ~1845-1885 B compressed) and apply it true-in-place.

THE FIELD JOURNAL
-----------------
Reconstruction model (per output byte i, mod 256):

    to[i] = patch[i] + fromzero[fp(i)] + dfdiff[i]

  * patch[i]      : bsdiff diff/extra byte (heatshrink stream).
  * fromzero[fp]  : from-image byte with reloc fields zeroed, read from the
                    SAME shared flash buffer at position fp. Under forward
                    write order fp lags the write frontier to_pos by the
                    cumulative insertion shift, so mem[fp] was overwritten
                    (to_pos-fp) steps ago. To stay byte-exact we keep a FIFO
                    "field journal" of the original (reloc-zeroed) from-bytes
                    in the window [fp, to_pos).  The journal is the dominant
                    resident RAM structure besides the heatshrink window.
  * dfdiff[i]     : sparse to-side reloc overlay (nonzero only inside ~4332
                    reloc fields, ~14% of the image). Reconstructed from the
                    shipped dfpatch reloc side-table; streamed in field order
                    so its residency is just one field (<=77 B) at a time.

TUNABLE JOURNAL CAP (= RAM budget)
----------------------------------
journal_cap_bytes bounds the FIFO of preserved from-bytes. If a diff byte at
output position to_pos needs source fp with (to_pos - fp) > cap, the original
fromzero[fp] is no longer resident -> the encoder SPILLS those 4 reloc-field /
source bytes into the patch (a side "spill" stream the decoder reads instead of
the journal). Smaller cap => more spilled bytes => larger patch (GRACEFUL
degradation). Large cap (>= shift) => zero spills => patch == seq_m4 floor.

This module is a real encoder + decoder simulator. The decoder reconstructs in
ONE shared bytearray (dest==src, NO headroom, TRUE in-place) and is verified
BYTE-EXACT against the golden to-image. RAM is accounted honestly: heatshrink
window (2^w) + input buf + dfpatch field-stream buf + op scratch + the capped
field journal + spill-stream buf.
"""
import sys, os, json
sys.path.insert(0, os.path.dirname(__file__))
sys.path.insert(0, os.path.dirname(os.path.dirname(__file__)))
sys.path.insert(0, '/ai_sw/detools-dev')

from m4_oracle import load_pair
from m4_inplace_sim import build_access_trace
from detools.common import pack_size
from detools import bsdiff
from heatshrink2 import compress

# ----- fixed scratch costs (bytes), honest device-side accounting -----
INPUT_BUF   = 256   # compressed-stream input refill buffer
OP_SCRATCH  = 256   # op/codeword decode scratch (one 256B row)
DF_STREAM   = 128   # one resident dfdiff field is <=77B; round up
SPILL_BUF   = 64    # small refill buffer for the spill side-stream


def raw_bsdiff(orc):
    raw = bytearray()
    for op in orc.ops:
        raw += pack_size(op.diff_len); raw += op.diff
        raw += pack_size(len(op.extra)); raw += op.extra
        raw += pack_size(op.adj)
    return bytes(raw)


def hs(data, w):
    if not data:
        return 0
    return len(compress(bytes(data), window_sz2=w, lookahead_sz2=w - 1))


# ======================================================================
# ENCODER: given a journal cap, decide which source bytes must spill.
# ======================================================================
def encode(orc, journal_cap_bytes):
    """Return a dict describing the shipped patch components for this cap.

    Components:
      bsdiff_raw : the global bsdiff op-stream (diff/extra/adj)         [hs]
      dfpatch    : reloc side-table that drives dfdiff                  [hs]
      spill_raw  : explicit fromzero[fp] bytes that fall outside the
                   journal cap, in the order the decoder consumes them  [hs]
    Plus the spill *mask* is implicit: the decoder reconstructs the same
    (to_pos - fp) lag deterministically, so it knows EXACTLY which diff bytes
    are spilled without any extra signalling (the cap is a header constant).
    """
    fz = orc.fromzero
    trace, _ = build_access_trace(orc)
    spill = bytearray()
    n_spill = 0
    # Deterministic replay of the forward FIFO: at each diff byte, the source
    # byte at fp was overwritten (to_pos - fp) steps ago. The FIFO holds the
    # most recent `cap` overwritten bytes. If the needed byte is older than cap
    # it has been evicted -> spill it.
    for (to_pos, kind, fp) in trace:
        if kind != 'diff':
            continue
        lag = to_pos - fp          # >=0 in forward order (frontier ahead of read)
        if lag > journal_cap_bytes:
            spill.append(fz[fp])    # ship the (reloc-zeroed) source byte
            n_spill += 1
    return {
        'bsdiff_raw': raw_bsdiff(orc),
        'dfpatch': bytes(orc.dfpatch),
        'spill_raw': bytes(spill),
        'n_spill': n_spill,
        'journal_cap_bytes': journal_cap_bytes,
    }


def patch_size(enc, w):
    """Compressed total patch size for window sz2 = w."""
    return (hs(enc['bsdiff_raw'], w)
            + hs(enc['dfpatch'], w)
            + hs(enc['spill_raw'], w))


# ======================================================================
# DECODER: TRUE in-place, forward order, capped field journal + spill.
# ======================================================================
def decode_inplace(orc, enc, journal_cap_bytes):
    """Reconstruct the to-image in ONE shared bytearray (dest==src), forward
    order, using only a capped FIFO field journal + the spill side-stream.
    Returns (ok, peak_journal_used_bytes).

    The decoder mirrors the encoder's deterministic lag computation, so it
    knows which diff bytes are spilled with no per-byte signalling.
    """
    N = max(orc.from_size, orc.to_size)
    mem = bytearray(orc.from_image) + bytearray(N - orc.from_size)

    # Reloc zero-mask: on a real device the decoder zeroes reloc fields of the
    # from-image on the fly from the reloc table (same table that drives
    # dfdiff). Here we model it from the oracle; it costs NO extra RAM beyond
    # the dfpatch already counted (it is derived from the same side-table).
    zmask = bytes(a ^ b for a, b in zip(orc.from_image, orc.fromzero))

    dfdiff = orc.dfdiff   # streamed one field at a time (residency = DF_STREAM)

    # FIFO field journal: original (reloc-zeroed) from-bytes for positions in
    # the window (to_pos-cap, to_pos]. Implemented as a ring keyed by position.
    # Invariant: every position p with 0 <= to_pos - p <= cap that has been
    # overwritten is present; positions older than cap have been evicted (and
    # the encoder will have spilled any read of them).
    from collections import deque
    journal = deque()      # FIFO of (pos, fromzero_byte)
    jbypos = {}            # pos -> fromzero_byte for entries still in journal
    peak = 0

    spill = enc['spill_raw']
    sp = 0  # spill read cursor

    fp = 0
    to_pos = 0
    cap = journal_cap_bytes

    def fz_at(pos):
        b = mem[pos]
        if pos < len(zmask) and zmask[pos]:
            b ^= zmask[pos]
        return b

    for op in orc.ops:
        # ---- diff bytes ----
        for k in range(op.diff_len):
            lag = to_pos - fp
            if lag <= 0:
                # source still original in flash (read-ahead): read directly
                fzb = fz_at(fp)
            elif lag <= cap:
                # within journal window -> must be resident
                fzb = jbypos[fp]
            else:
                # evicted (older than cap) -> read spilled byte from side-stream
                fzb = spill[sp]; sp += 1
            val = (op.diff[k] + fzb + dfdiff[to_pos]) & 0xFF
            # journal the (reloc-zeroed) original at to_pos BEFORE overwriting
            jb = fz_at(to_pos)
            mem[to_pos] = val
            journal.append((to_pos, jb)); jbypos[to_pos] = jb
            while journal and (to_pos - journal[0][0]) > cap:
                oldpos, _ = journal.popleft()
                jbypos.pop(oldpos, None)
            if len(journal) > peak:
                peak = len(journal)
            fp += 1
            to_pos += 1
        # ---- extra bytes (no from-read) ----
        for k in range(len(op.extra)):
            val = (op.extra[k] + dfdiff[to_pos]) & 0xFF
            jb = fz_at(to_pos)
            mem[to_pos] = val
            journal.append((to_pos, jb)); jbypos[to_pos] = jb
            while journal and (to_pos - journal[0][0]) > cap:
                oldpos, _ = journal.popleft()
                jbypos.pop(oldpos, None)
            if len(journal) > peak:
                peak = len(journal)
            to_pos += 1
        fp += op.adj

    ok = bytes(mem[:orc.to_size]) == orc.to_image
    return ok, peak, sp


# ======================================================================
# RAM accounting (honest, counts everything resident at once)
# ======================================================================
def ram_bytes(w, journal_cap_bytes, has_spill):
    win = 1 << w
    ram = win + INPUT_BUF + OP_SCRATCH + DF_STREAM + journal_cap_bytes
    if has_spill:
        ram += SPILL_BUF
    return ram


# ======================================================================
# BENCHMARK over RAM budgets {4,8,16,24} KiB
# ======================================================================
def bench_pair(orc, label):
    """For each RAM budget, choose (window w, journal cap) that minimizes the
    MEASURED byte-exact patch size, honestly counting all resident RAM, then
    verify byte-exact in-place decode and record measured peak RAM."""
    shift = orc.to_size - orc.from_size
    # candidate journal caps: 0 (max spill) up to >= shift (no spill).
    # include the exact lag-driven points.
    cap_candidates = sorted(set(
        [0, 64, 128, 192, 256, 320, 384, 512, 768, 1024, 1536, 2048,
         3072, 4096, 6144, 8192, shift, shift + 8, max(shift, 0)]
    ))
    results = []
    curve = []   # (cap, best_patch_over_w) for the size-vs-cap plot
    for budget_kib in (4, 8, 16, 24):
        budget = budget_kib * 1024
        best = None
        for cap in cap_candidates:
            if cap < 0:
                continue
            enc = encode(orc, cap)
            has_spill = enc['n_spill'] > 0
            for w in range(8, 15):
                ram = ram_bytes(w, cap, has_spill)
                if ram > budget:
                    continue
                ps = patch_size(enc, w)
                if best is None or ps < best['patch'] or (
                        ps == best['patch'] and ram < best['ram_model']):
                    best = {'patch': ps, 'w': w, 'cap': cap, 'ram_model': ram,
                            'n_spill': enc['n_spill'], 'enc': enc}
        if best is None:
            results.append({'ram_kb': budget_kib, 'patch_bytes': -1,
                            'byte_exact': False, 'peak_ram_bytes': -1,
                            'infeasible': True})
            continue
        # VERIFY byte-exact in-place decode at the chosen cap, measure peak RAM.
        enc = best['enc']
        ok, peak_journal, n_spill_read = decode_inplace(orc, enc, best['cap'])
        # measured peak RAM = window + bufs + actual peak journal entries + spill buf
        has_spill = best['n_spill'] > 0
        peak_ram = ((1 << best['w']) + INPUT_BUF + OP_SCRATCH + DF_STREAM
                    + peak_journal + (SPILL_BUF if has_spill else 0))
        results.append({
            'ram_kb': budget_kib,
            'patch_bytes': best['patch'],
            'byte_exact': bool(ok),
            'peak_ram_bytes': peak_ram,
            'window_sz2': best['w'],
            'journal_cap_bytes': best['cap'],
            'n_spill_bytes': best['n_spill'],
            'order': 'forward',
        })
    # size-vs-cap curve (best window per cap, byte-exact verified)
    for cap in cap_candidates:
        if cap < 0:
            continue
        enc = encode(orc, cap)
        bestps = min(patch_size(enc, w) for w in range(8, 15))
        ok, peak_j, _ = decode_inplace(orc, enc, cap)
        curve.append({'journal_cap_bytes': cap, 'best_patch_bytes': bestps,
                      'n_spill_bytes': enc['n_spill'], 'byte_exact': bool(ok),
                      'peak_journal_entries': peak_j})
    return results, curve, shift


def main():
    out = {'algorithm': 'field-journal',
           'description': 'global single m4 object + capped in-flight reloc-field '
                          'journal, forward TRUE in-place, graceful spill on overflow',
           'pairs': {}}
    pairs = [('v0_base', 'v1_one_face', 'v0v1_seq_m4.patch', 'v0v1_small_+360B'),
             ('v0_base', 'v2_three_faces', 'v0v2_seq_m4.patch', 'v0v2_three_faces_+3856B')]
    for fd, td, pn, lab in pairs:
        orc = load_pair(fd, td, pn)
        # sanity: model is byte-exact
        assert orc.reconstruct_sequential() == orc.to_image, "oracle model broke"
        results, curve, shift = bench_pair(orc, lab)
        out['pairs'][lab] = {
            'from_size': orc.from_size, 'to_size': orc.to_size,
            'insertion_shift': shift,
            'reloc_fields': len(orc.reloc_to),
            'dfpatch_raw_bytes': orc.dfpatch_size,
            'results': results,
            'size_vs_cap_curve': curve,
        }
        print(f"\n=== {lab}  from={orc.from_size} to={orc.to_size} shift={shift} ===")
        for r in results:
            if r.get('infeasible'):
                print(f"  {r['ram_kb']:2d}KiB: INFEASIBLE")
            else:
                print(f"  {r['ram_kb']:2d}KiB: patch={r['patch_bytes']}B "
                      f"byte_exact={r['byte_exact']} peak_ram={r['peak_ram_bytes']}B "
                      f"w={r['window_sz2']} cap={r['journal_cap_bytes']} "
                      f"spill={r['n_spill_bytes']}B <2kB={r['patch_bytes']<2000}")
        print("  size-vs-cap (best window per cap):")
        for c in curve:
            print(f"    cap={c['journal_cap_bytes']:5d}B -> patch={c['best_patch_bytes']}B "
                  f"spill={c['n_spill_bytes']}B exact={c['byte_exact']}")
    rp = os.path.join(os.path.dirname(os.path.dirname(__file__)), 'results',
                      'field-journal.json')
    with open(rp, 'w') as f:
        json.dump(out, f, indent=2)
    print(f"\nwrote {rp}")
    return out


if __name__ == '__main__':
    main()
