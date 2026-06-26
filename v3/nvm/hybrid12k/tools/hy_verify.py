"""HYBRID NVM 256-pair matrix: encode each F->T with rc_hybrid, run the NVM C decoder (1 byte at
a time), assert byte-exact under the emulator AND nvm_rows_amplified==0 every pair; collect blob
sizes + wear. 32-way parallel. Usage: PYTHONDONTWRITEBYTECODE=1 python3 -B tools/hy_verify.py <W> <CDEC>"""
import sys, os, subprocess, time, re, multiprocessing as mp
os.environ['PYTHONDONTWRITEBYTECODE'] = '1'
HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path[:0] = ['/ai_sw/detools-dev', HERE + '/sim', HERE + '/sim/ultrapatch']
IMG = sorted(os.path.join(HERE + '/images', d) for d in os.listdir(HERE + '/images'))
W = int(sys.argv[1]) if len(sys.argv) > 1 else 10
CDEC = sys.argv[2] if len(sys.argv) > 2 else '/tmp/hy_dec'
RUN_TAG = str(os.getpid())
NVM_RE = re.compile(r'erases=(\d+) rows=(\d+) programs=(\d+)')
AMP_RE = re.compile(r'amplified=(\d+) maxrowerase=(\d+) inversions=(\d+)')


def work(ij):
    import rc_hybrid
    rc_hybrid.PATHE_W = W
    i, j = ij
    blob, cfg = rc_hybrid.encode_v3(IMG[i], IMG[j])
    base = '/tmp/hy_v_%s_%d_%d_%d' % (RUN_TAG, os.getpid(), i, j)
    mem = base + '.bin'; bl = base + '.blob'; cf = base + '.cfg'
    open(mem, 'wb').write(open(IMG[i] + '/watch.bin', 'rb').read())
    open(bl, 'wb').write(blob); open(cf, 'w').write(' '.join(str(x) for x in cfg))
    r = subprocess.run([CDEC, mem, bl, cf, '1'], capture_output=True, text=True, timeout=300)
    real = open(IMG[j] + '/watch.bin', 'rb').read(); got = open(mem, 'rb').read()
    ce = (got == real)
    er = ro = pr = amp = mre = inv = -1; span = max(os.path.getsize(IMG[i] + '/watch.bin'), len(real))
    m = NVM_RE.search(r.stderr)
    if m: er, ro, pr = int(m.group(1)), int(m.group(2)), int(m.group(3))
    a = AMP_RE.search(r.stderr)
    if a: amp, mre, inv = int(a.group(1)), int(a.group(2)), int(a.group(3))
    floor = (span + 255) // 256
    metrics_ok = (m is not None and a is not None)
    ok = (r.returncode == 0 and ce and metrics_ok and amp == 0 and mre <= 1 and inv == 0)   # inv==0: req#2 sequential page writes
    for f in (mem, bl, cf):
        try: os.remove(f)
        except Exception: pass
    if ok:
        err = ''
    elif r.returncode == 0 and ce and not metrics_ok:
        err = 'missing NVM metrics'
    elif ce and amp > 0:
        err = 'amp=%d' % amp
    elif ce and inv > 0:
        err = 'inversions=%d' % inv
    else:
        err = r.stderr.strip().split(chr(10))[-1][:70] if r.stderr else 'rc=%d' % r.returncode
    return (i, j, ok, len(blob), er, ro, pr, floor, amp, mre, inv, err)


if __name__ == '__main__':
    pairs = [(i, j) for i in range(16) for j in range(16)]
    t0 = time.time()
    with mp.get_context('fork').Pool(min(32, (os.cpu_count() or 2) - 1)) as p:
        res = p.map(work, pairs, chunksize=1)
    ok = sum(1 for r in res if r[2]); tot = sum(r[3] for r in res)
    fails = [r for r in res if not r[2]]
    amp_bad = sum(1 for r in res if r[8] > 0); mre_max = max(r[9] for r in res)
    inv_bad = sum(1 for r in res if r[10] > 0)
    mults = [r[4] / r[7] for r in res if r[7] > 0 and r[2]]
    best = 4934646; v2 = 5103706; shipped = 5178810; pathF = 5025418
    print("HYBRID NVM 256-matrix (W=%d): %d/256 ok (byte-exact AND rows_amplified=0 AND frontier_inversions=0) (%.0fs)" % (W, ok, time.time() - t0))
    print("  patch total=%d B  dVS_best=%+.3f%%  dVS_pathF=%+.3f%%  dVS_v2=%+.3f%%  dVS_shipped(+6%%)=%+.3f%%"
          % (tot, 100 * (tot - best) / best, 100 * (tot - pathF) / pathF, 100 * (tot - v2) / v2, 100 * (tot - shipped) / shipped))
    print("  rows_amplified>0 pairs=%d  max_row_erases(any pair)=%d  frontier_inversions>0 pairs=%d" % (amp_bad, mre_max, inv_bad))
    if mults:
        print("  wear mult vs floor: min=%.3f mean=%.3f max=%.3f" % (min(mults), sum(mults) / len(mults), max(mults)))
    for f in fails[:16]:
        print("  FAIL %d->%d: %s" % (f[0], f[1], f[11]))
    budget = {10: 4898705}.get(W)
    budget_bad = budget is not None and tot > budget
    if ok != len(pairs) or amp_bad != 0 or mre_max > 1 or inv_bad != 0 or budget_bad:
        if budget_bad:
            print("  FAIL patch total budget: %d > %d" % (tot, budget))
        sys.exit(1)
