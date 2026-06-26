"""vp-2: REAL-FIRMWARE generalization gate. Runs every ordered pair of the genuine COLOR=PRO
firmware fixtures (m4dev/fixtures: v0_base / v1_one_face / v2_three_faces — structurally independent
of the 16-image synthetic corpus) through rc_hybrid.encode_v3 -> the NVM C decoder (1 byte at a time),
and asserts byte-exact reconstruction AND rows_amplified=0 AND frontier_inversions=0 for every pair.
This closes the "the corpus is one synthetic family" coverage gap.

Usage: PYTHONDONTWRITEBYTECODE=1 python3 -B tools/fixture_gate.py <CDEC>
  where <CDEC> = a decoder built with: cc -O2 -DRC_V3_MAIN -DRC_V3_NVM -I c -o dec c/rc_v3.c c/flash_nvm.c
"""
import sys, os, subprocess, re
os.environ['PYTHONDONTWRITEBYTECODE'] = '1'
HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))   # .../hybrid12k
sys.path[:0] = ['/ai_sw/detools-dev', HERE + '/sim', HERE + '/sim/ultrapatch']

# locate m4dev/fixtures by walking up from this tree (fixtures live in the git repo, not the live copy)
def _find_fixtures():
    d = HERE
    for _ in range(8):
        cand = os.path.join(d, 'fixtures')
        if os.path.isdir(cand) and os.path.isdir(os.path.join(cand, 'v0_base')):
            return cand
        d = os.path.dirname(d)
    return None

FIX = _find_fixtures()
NVM_RE = re.compile(r'amplified=(\d+) maxrowerase=(\d+) inversions=(\d+)')


def run(cdec, F, T):
    import rc_hybrid
    rc_hybrid.PATHE_W = 10
    Fd, Td = os.path.join(FIX, F), os.path.join(FIX, T)
    blob, cfg = rc_hybrid.encode_v3(Fd, Td)
    mem = '/tmp/fx_%s_%s.bin' % (F, T); bl = '/tmp/fx_%s_%s.blob' % (F, T); cf = '/tmp/fx_%s_%s.cfg' % (F, T)
    open(mem, 'wb').write(open(Fd + '/watch.bin', 'rb').read())
    open(bl, 'wb').write(blob); open(cf, 'w').write(' '.join(str(x) for x in cfg))
    r = subprocess.run([cdec, mem, bl, cf, '1'], capture_output=True, text=True, timeout=300)
    real = open(Td + '/watch.bin', 'rb').read(); got = open(mem, 'rb').read()
    amp = mre = inv = -1
    m = NVM_RE.search(r.stderr)
    if m: amp, mre, inv = int(m.group(1)), int(m.group(2)), int(m.group(3))
    ok = (r.returncode == 0 and got == real and amp == 0 and inv == 0)
    for f in (mem, bl, cf):
        try: os.remove(f)
        except Exception: pass
    return ok, len(blob), amp, mre, inv, (got == real), r.returncode


if __name__ == '__main__':
    cdec = sys.argv[1] if len(sys.argv) > 1 else '/tmp/hy_dec'
    if not FIX:
        print("FIXTURE GATE: SKIP — m4dev/fixtures not found (only present in the git repo tree)"); sys.exit(0)
    names = ['v0_base', 'v1_one_face', 'v2_three_faces']
    pairs = [(F, T) for F in names for T in names if F != T]
    npass = 0
    print("vp-2 REAL-FIRMWARE fixture gate (%s):" % FIX)
    for (F, T) in pairs:
        ok, n, amp, mre, inv, ce, rc = run(cdec, F, T)
        npass += ok
        print("  %-14s -> %-14s : %s  blob=%-6d amp=%d maxrow=%d inversions=%d%s"
              % (F, T, 'OK' if ok else 'FAIL', n, amp, mre, inv,
                 '' if ok else '  (byte_exact=%s rc=%d)' % (ce, rc)))
    print("RESULT: %s (%d/%d pairs byte-exact + amp=0 + inversions=0)"
          % ('PASS' if npass == len(pairs) else 'FAIL', npass, len(pairs)))
    sys.exit(0 if npass == len(pairs) else 1)
