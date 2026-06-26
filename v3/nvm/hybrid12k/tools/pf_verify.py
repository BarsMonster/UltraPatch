"""Path F C byte-by-byte 256/256 matrix. The C decoder is fed 1 byte at a time (arg5 present).
Usage: PYTHONDONTWRITEBYTECODE=1 python3 -B tools/pf_verify.py <W> <CDEC_bin>
Requires the encoder W (rc_v3.PATHE_W) to match the C decoder's SA_W (build with -DSA_W=W)."""
import sys, os, subprocess, time, multiprocessing as mp
os.environ['PYTHONDONTWRITEBYTECODE'] = '1'
HERE = '/ai_sw/v3/super/pathF'
sys.path[:0] = ['/ai_sw/detools-dev', HERE + '/sim', HERE + '/sim/ultrapatch']
IMG = sorted(os.path.join(HERE + '/images', d) for d in os.listdir(HERE + '/images'))
W = int(sys.argv[1]) if len(sys.argv) > 1 else 10
CDEC = sys.argv[2] if len(sys.argv) > 2 else '/tmp/pathF_v3'


def work(ij):
    import rc_v3
    rc_v3.PATHE_W = W
    i, j = ij
    blob, cfg = rc_v3.encode_v3(IMG[i], IMG[j])
    mem = '/tmp/pfv_%d_%d.bin' % (i, j); bl = '/tmp/pfv_%d_%d.blob' % (i, j); cf = '/tmp/pfv_%d_%d.cfg' % (i, j)
    open(mem, 'wb').write(open(IMG[i] + '/watch.bin', 'rb').read())
    open(bl, 'wb').write(blob); open(cf, 'w').write(' '.join(str(x) for x in cfg))
    r = subprocess.run([CDEC, mem, bl, cf, '1'], capture_output=True, text=True, timeout=120)
    real = open(IMG[j] + '/watch.bin', 'rb').read(); got = open(mem, 'rb').read()
    ok = (r.returncode == 0 and got == real)
    jp = 0; streamed = 'streaming OK' in r.stderr
    for t in r.stderr.split():
        if t.startswith('journal_used='):
            try: jp = int(t.split('=')[1])
            except Exception: pass
    for f in (mem, bl, cf): os.remove(f)
    return (i, j, ok, jp, len(blob), streamed, '' if ok else (r.stderr.strip().split(chr(10))[-1][:70] if r.stderr else 'rc=%d' % r.returncode))


if __name__ == '__main__':
    pairs = [(i, j) for i in range(16) for j in range(16)]
    t0 = time.time()
    ctx = mp.get_context('fork')
    with ctx.Pool(min(32, (os.cpu_count() or 2) - 1)) as p:
        res = p.map(work, pairs, chunksize=1)
    ok = sum(1 for r in res if r[2]); tot = sum(r[4] for r in res); jpk = max(r[3] for r in res)
    strm = sum(1 for r in res if r[5]); fails = [r for r in res if not r[2]]
    best = 4934646; v2 = 5103706; shipped = 5178810
    print("Path F C byte-by-byte (W=%d): %d/256 byte-exact (%.0fs)" % (W, ok, time.time() - t0))
    print("  total=%d B  dVS_best=%+.3f%%  dVS_v2=%+.3f%%  dVS_shipped=%+.3f%%  jpeak=%d  streaming_proven=%d/256"
          % (tot, 100 * (tot - best) / best, 100 * (tot - v2) / v2, 100 * (tot - shipped) / shipped, jpk, strm))
    for f in fails[:8]:
        print("  FAIL %d->%d: %s" % (f[0], f[1], f[6]))
