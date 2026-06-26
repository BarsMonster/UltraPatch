"""A1 golden round-trip + size: encode+decode all 256 pairs with the ldr-derive golden, assert
byte-exact, sum patch sizes, compare to the pre-A1 hybrid (5,163,834 B) and Path F (5,025,418).
Usage: PYTHONDONTWRITEBYTECODE=1 python3 -B tools/a1_golden_rt.py [W]"""
import sys, os, time, multiprocessing as mp
os.environ['PYTHONDONTWRITEBYTECODE'] = '1'
HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path[:0] = ['/ai_sw/detools-dev', HERE + '/sim', HERE + '/sim/ultrapatch']
IMG = sorted(os.path.join(HERE + '/images', d) for d in os.listdir(HERE + '/images'))
W = int(sys.argv[1]) if len(sys.argv) > 1 else 10


def work(ij):
    import rc_hybrid
    rc_hybrid.PATHE_W = W
    i, j = ij
    if i == j: return (i, j, True, 0, 0)
    try:
        blob, cfg = rc_hybrid.encode_v3(IMG[i], IMG[j])
        frm = open(IMG[i] + '/watch.bin', 'rb').read()
        res, info = rc_hybrid.decode_v3(blob, frm, cfg)
        real = open(IMG[j] + '/watch.bin', 'rb').read()
        return (i, j, res == real, len(blob), info['jpeak'])
    except Exception as e:
        return (i, j, False, -1, repr(e)[:90])


if __name__ == '__main__':
    pairs = [(i, j) for i in range(16) for j in range(16)]
    t0 = time.time()
    with mp.get_context('fork').Pool(min(32, (os.cpu_count() or 2) - 1)) as p:
        res = p.map(work, pairs, chunksize=1)
    real_pairs = [r for r in res if r[0] != r[1]]
    ok = sum(1 for r in real_pairs if r[2])
    tot = sum(r[3] for r in real_pairs if r[2] and r[3] > 0)
    fails = [r for r in real_pairs if not r[2]]
    pre_a1 = 5163834; pathF = 5025418; best = 4934646
    print("A1 golden round-trip (W=%d): %d/%d pairs byte-exact (%.0fs)" % (W, ok, len(real_pairs), time.time() - t0))
    print("  patch total = %d B" % tot)
    print("  vs pre-A1 hybrid (5,163,834) = %+.3f%% (%+d B)" % (100 * (tot - pre_a1) / pre_a1, tot - pre_a1))
    print("  vs Path F      (5,025,418) = %+.3f%%" % (100 * (tot - pathF) / pathF))
    print("  vs best        (4,934,646) = %+.3f%%" % (100 * (tot - best) / best))
    jpk = [r[4] for r in real_pairs if r[2] and r[3] > 0]
    if jpk: print("  journal peak: max=%d" % max(jpk))
    for f in fails[:12]: print("  FAIL %d->%d: %s" % (f[0], f[1], f[4] if isinstance(f[4], str) else 'mismatch'))
    budget = {10: (4881942, 903)}.get(W)
    total_bad = budget is not None and tot > budget[0]
    jpeak_bad = budget is not None and bool(jpk) and max(jpk) > budget[1]
    if ok != len(real_pairs) or fails or total_bad or jpeak_bad:
        if total_bad:
            print("  FAIL patch total budget: %d > %d" % (tot, budget[0]))
        if jpeak_bad:
            print("  FAIL journal peak budget: %d > %d" % (max(jpk), budget[1]))
        sys.exit(1)
