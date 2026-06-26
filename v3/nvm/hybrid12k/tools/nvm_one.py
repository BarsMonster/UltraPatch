"""Single-pair NVM test: encode F->T with local sim, run the NVM C decoder, report
byte-exact + NVM wear (erases/rows/programs). Usage: nvm_one.py <W> <CDEC> <i> <j>"""
import sys, os, subprocess
os.environ['PYTHONDONTWRITEBYTECODE'] = '1'
HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path[:0] = ['/ai_sw/detools-dev', HERE + '/sim', HERE + '/sim/ultrapatch']
IMG = sorted(os.path.join(HERE + '/images', d) for d in os.listdir(HERE + '/images'))
W = int(sys.argv[1]); CDEC = sys.argv[2]; i = int(sys.argv[3]); j = int(sys.argv[4])
import rc_v3
rc_v3.PATHE_W = W
blob, cfg = rc_v3.encode_v3(IMG[i], IMG[j])
mem = '/tmp/opt2_one_%d_%d.bin' % (i, j); bl = '/tmp/opt2_one_%d_%d.blob' % (i, j); cf = '/tmp/opt2_one_%d_%d.cfg' % (i, j)
open(mem, 'wb').write(open(IMG[i] + '/watch.bin', 'rb').read())
open(bl, 'wb').write(blob); open(cf, 'w').write(' '.join(str(x) for x in cfg))
r = subprocess.run([CDEC, mem, bl, cf, '1'], capture_output=True, text=True, timeout=300)
real = open(IMG[j] + '/watch.bin', 'rb').read(); got = open(mem, 'rb').read()
ok = (r.returncode == 0 and got == real)
print("pair %d->%d (from=%d to=%d): %s  blob=%d" % (i, j, len(real) if False else os.path.getsize(IMG[i]+'/watch.bin'), len(real), 'OK' if ok else 'FAIL', len(blob)))
print("STDERR:")
print(r.stderr.strip())
for f in (mem, bl, cf):
    try: os.remove(f)
    except Exception: pass
sys.exit(0 if ok else 1)
