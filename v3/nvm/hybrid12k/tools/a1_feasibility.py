"""A1 feasibility measurement (steps 1-4): can ldr relocation POSITIONS be derived on-device
(like bl) instead of shipped, under {<=12KiB SRAM, <=1 write/page}?

For each of the 256 corpus pairs (32-way parallel) we measure, ENCODER-SIDE ONLY (no decoder
change, no wire change):
  1. ex cost split by sub-class {data, code, ldr}  -> ldr share = the derivable ceiling.
  2. local-ldr derive quality: a FAITHFUL stateful ldr instruction->target scan (the on-device
     LitWin would run this) vs detools' true captured ldr fields, restricted to reachable pure-copy
     field starts -> false-positive (needs suppression) / false-negative (needs position fallback).
  3. window cost: LitWin reach = max(target-instruction); peak concurrent open [ins,target]
     intervals = LitWin live-bit occupancy; reorder-buffer span (produce in from-addr order, consume
     in apply/write order) = the bounded reorder buffer agent10 used.
  4. net ledger: gE bytes for ex offsets ALL vs (ex minus ldr) = gross ldr-position saving; minus the
     suppression/0-delta cost implied by step 2.

Usage: PYTHONDONTWRITEBYTECODE=1 python3 -B tools/a1_feasibility.py [W]
"""
import sys, os, time, multiprocessing as mp
os.environ['PYTHONDONTWRITEBYTECODE'] = '1'
HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path[:0] = ['/ai_sw/detools-dev', HERE + '/sim', HERE + '/sim/ultrapatch']
IMG = sorted(os.path.join(HERE + '/images', d) for d in os.listdir(HERE + '/images'))
W = int(sys.argv[1]) if len(sys.argv) > 1 else 10


def _setup(F, T, opt):
    """Mirror encode_v3's front half: return (frm, ops, fielddelta, cfg7, FWD, from_size, to_size)."""
    import rc_hybrid, subprocess
    from rc_hybrid import _field_deltas
    from rc_v3 import Op, DET
    from m4_oracle import _read_buffers
    fbin = F + '/watch.bin'; tbin = T + '/watch.bin'
    p = '/tmp/a1_%d.patch' % os.getpid()
    try:
        subprocess.run([DET, 'create_patch', '-t', 'sequential', '-a', 'bsdiff',
                        '--data-format', 'arm-cortex-m4', '--from-elf-file', F + '/watch.elf',
                        '--to-elf-file', T + '/watch.elf', '-c', 'none', fbin, tbin, p],
                       check=True, capture_output=True)
        d = _read_buffers(fbin, p)
    finally:
        if os.path.exists(p): os.remove(p)
    ops = [Op(o.diff_len, o.diff, o.extra, o.adj) for o in d['ops']]
    frm = d['from_image']
    from_size = len(frm); to_size = sum(o.diff_len + len(o.extra) for o in ops)
    fielddelta, cfg7 = _field_deltas(frm, d['dfpatch'], opt)
    FWD = to_size <= from_size
    return frm, ops, fielddelta, cfg7, FWD, from_size, to_size


def ldr_target_scan(frm, cfg7):
    """FAITHFUL port of detools.arm_cortex_m4.disassemble, ldr path only (Cortex-M0+: no ldr.w/bw
    membership). Returns list of (ins, target) for every ldr literal load, in instruction order, with
    the data-region skip and the literal-pool (already-seen target) skip — exactly what an on-device
    forward LitWin scan reproduces."""
    dp, fdo, fdb, fde, cp, fcb, fce = cfg7
    do_b, do_e = fdo, fdo + (fde - fdb)        # data_offset region (see m4reloc._disasm)
    n = len(frm); addr = 0; seen = set(); pairs = []
    while addr < n:
        if do_b <= addr < do_e:
            addr += 4; continue                # disassemble_data reads a 4-byte word
        if addr in seen:
            addr += 4; continue                # literal-pool word (ldr target) -> skip 4
        if addr + 2 > n: break
        up = frm[addr] | (frm[addr + 1] << 8); ins = addr; addr += 2
        if (up & 0xf800) == 0xf000:            # bl / b.w : 32-bit, consume lo
            if addr + 2 > n: continue
            addr += 2
        elif (up & 0xf800) == 0x4800:          # ldr (literal), 16-bit
            imm8 = 4 * (up & 0xff) + 4
            t = (ins - 2 if (ins % 4) == 2 else ins) + imm8
            if t + 4 <= n and t not in seen:
                seen.add(t); pairs.append((ins, t))
        elif up == 0xf8df:                     # ldr.w (won't occur on M0+ but consume lo if matched)
            if addr + 2 > n: continue
            addr += 2
        elif (up & 0xfff0) in (0xfbb0, 0xfb90, 0xf8d0, 0xf850): addr += 2
        elif (up & 0xffe0) == 0xfa00: addr += 2
        elif (up & 0xffc0) == 0xe900: addr += 2
    return pairs


def _ge_bytes(per_op_offsets):
    """Encode per-op ex offset lists exactly like _encode_A_hybrid.emit_geom_pc's gE path and return
    the flushed byte count (one shared adaptive UGolomb across ops, |gap| coding)."""
    from rc_codec import UGolomb, BinEnc
    rc_ = BinEnc(); gE = UGolomb('g')
    for offs in per_op_offsets:
        gE.encode(rc_, len(offs)); pe = None
        for w in offs:
            gE.encode(rc_, abs(w - pe) if pe is not None else w); pe = w
    return len(rc_.flush_opt())


def work(ij):
    from rc_hybrid import _scan_op_fields
    from rc_v3 import DEFAULT_OPT, PATHE_W
    i, j = ij
    if i == j: return None
    opt = dict(DEFAULT_OPT); opt['W'] = W
    try:
        frm, ops, fielddelta, cfg7, FWD, from_size, to_size = _setup(IMG[i], IMG[j], opt)
    except Exception as e:
        return ('ERR', i, j, repr(e)[:80])

    # per-op apply meta (apply/write order); collect ex offsets split by kind + reachable pure starts
    meta = []; tp = fp = 0
    for o in ops:
        meta.append((tp, fp, o.diff_len, o.adj, o.diff))
        tp += o.diff_len + len(o.extra); fp += o.diff_len + o.adj
    order = range(len(meta)) if FWD else range(len(meta) - 1, -1, -1)

    ex_by_kind = {'data': 0, 'code': 0, 'ldr': 0}
    exo_all = []      # per-op ex offset lists (all kinds)
    exo_noldr = []    # per-op ex offset lists (ldr dropped -> derived)
    real_ldr = set()  # from-addresses of true ldr fields (reachable)
    ex_ldr_apply_order = []   # true ldr field from-addrs in apply/write order (for reorder span)
    reachable = set()         # from-addrs that are 4-byte pure-copy field starts (derive-reachable)

    for opi in order:
        (tp0, fp0, dl, adj, diff) = meta[opi]
        a_all = []; a_no = []
        for ev in _scan_op_fields(frm, fp0, dl, from_size, fielddelta, diff, desc=not FWD):
            if ev[0] != 'ex': continue
            k = ev[1]; fpk = ev[2]; nm = fielddelta[fpk][0]
            ex_by_kind[nm] = ex_by_kind.get(nm, 0) + 1
            a_all.append(k)
            if nm == 'ldr':
                real_ldr.add(fpk); ex_ldr_apply_order.append(fpk)
            else:
                a_no.append(k)
        exo_all.append(a_all); exo_noldr.append(a_no)
        # reachable pure 4-byte-aligned-ish copy starts in this op's diff range
        D = diff
        for k in range(0, dl - 3):
            if D[k] == 0 and D[k + 1] == 0 and D[k + 2] == 0 and D[k + 3] == 0:
                reachable.add(fp0 + k)

    # --- step 2: local-ldr derive quality (faithful scan, restricted to reachable starts) ---
    pairs = ldr_target_scan(frm, cfg7)
    pred_all = set(t for _, t in pairs)
    pred = pred_all & reachable             # only reachable predictions can fire during apply
    truep = pred & real_ldr
    falsep = pred - real_ldr                # de-reloc would fire wrongly -> needs suppression
    falsen = real_ldr - pred_all            # real field the scan never predicts -> ship its position

    # --- step 3: window cost ---
    reach = max((t - ins for ins, t in pairs), default=0)
    # LitWin live occupancy: sweep addresses; a bit is live over [ins, target]
    evs = []
    for ins, t in pairs:
        evs.append((ins, +1)); evs.append((t, -1))
    evs.sort()
    cur = peak_lit = 0
    for _, d in evs:
        cur += d
        if cur > peak_lit: peak_lit = cur
    # reorder-buffer span: produce ldr targets in from-address (instruction) order, consume them in
    # apply/write order. Peak |produced-but-not-consumed| among the REAL ldr fields.
    prod_order = {t: idx for idx, (ins, t) in enumerate(sorted(pairs, key=lambda x: x[1]))}  # by target addr asc
    consume_seq = [prod_order.get(fpk) for fpk in ex_ldr_apply_order if fpk in prod_order]
    reorder_span = 0
    if consume_seq:
        consumed = set(); maxprod = -1
        for c in consume_seq:
            consumed.add(c); maxprod = max(maxprod, c)
            # outstanding = produced (index<=maxprod) not yet consumed
            reorder_span = max(reorder_span, (maxprod + 1) - len(consumed))

    # --- step 4: net ledger (gE bytes) ---
    ge_all = _ge_bytes(exo_all); ge_noldr = _ge_bytes(exo_noldr)
    saving = ge_all - ge_noldr              # gross bytes saved by NOT shipping ldr positions

    return ('OK', i, j, ex_by_kind['data'], ex_by_kind['code'], ex_by_kind['ldr'],
            len(real_ldr), len(pred), len(truep), len(falsep), len(falsen),
            reach, peak_lit, reorder_span, ge_all, ge_noldr, saving)


if __name__ == '__main__':
    pairs = [(i, j) for i in range(16) for j in range(16)]
    t0 = time.time()
    with mp.get_context('fork').Pool(min(32, (os.cpu_count() or 2) - 1)) as p:
        res = [r for r in p.map(work, pairs, chunksize=1) if r is not None]
    errs = [r for r in res if r[0] == 'ERR']
    ok = [r for r in res if r[0] == 'OK']
    print("A1 feasibility (W=%d): %d pairs measured, %d errors (%.0fs)" % (W, len(ok), len(errs), time.time() - t0))
    for e in errs[:8]: print("  ERR %d->%d: %s" % (e[1], e[2], e[3]))
    if not ok: sys.exit(1)

    def col(idx): return [r[idx] for r in ok]
    data, code, ldr = sum(col(3)), sum(col(4)), sum(col(5))
    ex_tot = data + code + ldr
    rl, pr, tp, fp_, fn = sum(col(6)), sum(col(7)), sum(col(8)), sum(col(9)), sum(col(10))
    ge_all, ge_no, sav = sum(col(14)), sum(col(15)), sum(col(16))
    reach_mx = max(col(11)); lit_mx = max(col(12)); reorder_mx = max(col(13))

    print("\n== step 1: ex field split (corpus totals) ==")
    print("  ex fields: %d  (data=%d %.1f%%, code=%d %.1f%%, ldr=%d %.1f%%)"
          % (ex_tot, data, 100*data/ex_tot, code, 100*code/ex_tot, ldr, 100*ldr/ex_tot))
    print("  => ldr = %.1f%% of ex positions (the derivable ceiling)" % (100*ldr/ex_tot))

    print("\n== step 2: local-ldr derive quality (reachable pure-copy field starts) ==")
    print("  real ldr fields (reachable): %d" % rl)
    print("  predicted (faithful scan, reachable): %d  true-pos=%d  false-pos=%d  false-neg=%d" % (pr, tp, fp_, fn))
    if rl: print("  recall=%.3f%% (1-false-neg/real)   false-pos rate=%.3f%% of predictions"
                 % (100*(rl-fn)/rl, 100*fp_/pr if pr else 0))
    print("  suppress-exceptions needed (false-pos): %d   position-fallbacks (false-neg): %d" % (fp_, fn))

    print("\n== step 3: window cost (vs agent10: LitWin 2KB / reorder 1100, max disorder 956) ==")
    print("  LitWin reach (max target-instruction)= %d B   peak live bits= %d   reorder span= %d" % (reach_mx, lit_mx, reorder_mx))

    print("\n== step 4: net ledger (gE ex-offset bytes) ==")
    print("  gE bytes ALL ex offsets = %d   minus-ldr = %d   GROSS ldr-position saving = %d B (%.1f KB)"
          % (ge_all, ge_no, sav, sav/1024.0))
    # cost estimates: option alpha (suppress false-pos like sbl ~ shipped offset, est ~1.5 B/entry incl count)
    #                 option beta  (ship a cheap 0/rep delta for every predicted target ~ <=2 bits)
    alpha = fp_ * 1.5 + fn * 1.5
    beta_bits = (pr - tp) * 2 + fn * 12       # 0-deltas for non-field predictions ~2b; false-neg still ship pos
    print("  est suppression cost  alpha (ship false-pos+false-neg offsets ~1.5 B ea) = %.0f B" % alpha)
    print("  est suppression cost  beta  (0-delta per non-field prediction ~2 bit + fn pos) = %.0f B" % (beta_bits/8.0))
    print("  => NET ldr-derive saving  alpha ~ %.0f B (%.2f%% of patch)   beta ~ %.0f B (%.2f%% of patch)"
          % (sav-alpha, 100*(sav-alpha)/5163834, sav-beta_bits/8.0, 100*(sav-beta_bits/8.0)/5163834))
    print("\n  (patch baseline 5,163,834 B = +2.75%% vs Path F; full ldr-position elimination would cut the")
    print("   gross saving above off the +2.75%% gap, less the suppression cost.)")
