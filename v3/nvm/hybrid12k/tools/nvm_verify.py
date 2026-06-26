"""Option 2 NVM 256-pair matrix: encode each F->T, run the NVM C decoder (1 byte at a time),
check byte-exact under the emulator, collect per-pair erases/rows/programs + journal peak.
32-way parallel (serial times out). Usage: PYTHONDONTWRITEBYTECODE=1 python3 -B tools/nvm_verify.py <W> <CDEC>"""
import sys, os, subprocess, time, re, multiprocessing as mp
os.environ['PYTHONDONTWRITEBYTECODE'] = '1'
HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path[:0] = ['/ai_sw/detools-dev', HERE + '/sim', HERE + '/sim/ultrapatch']
IMG = sorted(os.path.join(HERE + '/images', d) for d in os.listdir(HERE + '/images'))
W = int(sys.argv[1]) if len(sys.argv) > 1 else 10
CDEC = sys.argv[2] if len(sys.argv) > 2 else '/tmp/opt2_dec'
NVM_RE = re.compile(r'erases=(\d+) rows=(\d+) programs=(\d+)')


def work(ij):
    import rc_v3
    rc_v3.PATHE_W = W
    i, j = ij
    blob, cfg = rc_v3.encode_v3(IMG[i], IMG[j])
    mem = '/tmp/opt2_v_%d_%d.bin' % (i, j); bl = '/tmp/opt2_v_%d_%d.blob' % (i, j); cf = '/tmp/opt2_v_%d_%d.cfg' % (i, j)
    open(mem, 'wb').write(open(IMG[i] + '/watch.bin', 'rb').read())
    open(bl, 'wb').write(blob); open(cf, 'w').write(' '.join(str(x) for x in cfg))
    r = subprocess.run([CDEC, mem, bl, cf, '1'], capture_output=True, text=True, timeout=300)
    real = open(IMG[j] + '/watch.bin', 'rb').read(); got = open(mem, 'rb').read()
    ok = (r.returncode == 0 and got == real)
    er = ro = pr = 0; span = max(os.path.getsize(IMG[i] + '/watch.bin'), len(real))
    m = NVM_RE.search(r.stderr)
    if m: er, ro, pr = int(m.group(1)), int(m.group(2)), int(m.group(3))
    floor = (span + 255) // 256
    for f in (mem, bl, cf):
        try: os.remove(f)
        except Exception: pass
    err = '' if ok else (r.stderr.strip().split(chr(10))[-1][:70] if r.stderr else 'rc=%d' % r.returncode)
    return (i, j, ok, len(blob), er, ro, pr, floor, err)


if __name__ == '__main__':
    pairs = [(i, j) for i in range(16) for j in range(16)]
    t0 = time.time()
    ctx = mp.get_context('fork')
    with ctx.Pool(min(32, (os.cpu_count() or 2) - 1)) as p:
        res = p.map(work, pairs, chunksize=1)
    ok = sum(1 for r in res if r[2]); tot = sum(r[3] for r in res)
    fails = [r for r in res if not r[2]]
    # wear: per-pair erase multiplier vs floor (skip identity pairs i==j which have floor>0 but ~0 work)
    mults = [r[4] / r[7] for r in res if r[7] > 0 and r[2]]
    tot_er = sum(r[4] for r in res if r[2]); max_er = max((r[4] for r in res if r[2]), default=0)
    worst = max(((r[4] / r[7], r) for r in res if r[7] > 0 and r[2]), default=(0, None))
    print("Option 2 NVM 256-matrix (W=%d): %d/256 byte-exact (%.0fs)" % (W, ok, time.time() - t0))
    print("  patch total=%d B" % tot)
    if mults:
        print("  wear mult vs floor: min=%.3f mean=%.3f max=%.3f  total_erases=%d  max_per_patch=%d"
              % (min(mults), sum(mults) / len(mults), max(mults), tot_er, max_er))
    if worst[1]:
        w = worst[1]
        print("  worst-wear pair %d->%d: erases=%d floor=%d (%.3fx) rows=%d" % (w[0], w[1], w[4], w[7], worst[0], w[5]))
    for f in fails[:12]:
        print("  FAIL %d->%d: %s" % (f[0], f[1], f[8]))
