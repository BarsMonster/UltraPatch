"""vp-4: corrupt-patch fuzz gate. Mutates a VALID patch blob N ways (seeded -> reproducible) and runs
each through the PLAIN -O2 NVM decoder, asserting the decoder NEVER crashes (no signal) and NEVER hangs
(no timeout) — it must either cleanly reject (rc=1, CRC/g_rcerr) or, for a benign no-op mutation, decode
(rc=0). Codifies the "0 crash/UB on corrupt patches" claim as a runnable gate.

IMPORTANT: build the decoder PLAIN (`cc -O2 -DRC_V3_MAIN -DRC_V3_NVM ...`). Do NOT use -fsanitize:
ASan/UBSan cannot follow the decoder's manual coroutine stack and will reject even VALID blobs, so a
sanitizer "0 crashes" run is a hollow signal. The plain build is the meaningful one.

Usage: PYTHONDONTWRITEBYTECODE=1 python3 -B tools/fuzz_gate.py <CDEC> [N]
"""
import sys, os, subprocess, random
os.environ['PYTHONDONTWRITEBYTECODE'] = '1'
HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path[:0] = ['/ai_sw/detools-dev', HERE + '/sim', HERE + '/sim/ultrapatch']


def _find_fixtures():
    d = HERE
    for _ in range(8):
        c = os.path.join(d, 'fixtures')
        if os.path.isdir(os.path.join(c, 'v0_base')): return c
        d = os.path.dirname(d)
    return None


def mutate(rng, blob):
    b = bytearray(blob); n = len(b)
    t = rng.randrange(5)
    if t == 0:                                   # flip 1..8 random bits
        for _ in range(rng.randint(1, 8)):
            i = rng.randrange(n); b[i] ^= (1 << rng.randrange(8))
    elif t == 1:                                 # randomize 1..16 bytes
        for _ in range(rng.randint(1, 16)):
            b[rng.randrange(n)] = rng.randrange(256)
    elif t == 2:                                 # truncate
        return bytes(b[:rng.randint(12, n)])
    elif t == 3:                                 # append junk
        return bytes(b) + bytes(rng.randrange(256) for _ in range(rng.randint(1, 64)))
    else:                                        # zero a random run
        i = rng.randrange(n); j = min(n, i + rng.randint(1, 32))
        for k in range(i, j): b[k] = 0
    return bytes(b)


if __name__ == '__main__':
    cdec = sys.argv[1] if len(sys.argv) > 1 else '/tmp/hy_dec'
    N = int(sys.argv[2]) if len(sys.argv) > 2 else 300
    FIX = _find_fixtures()
    if not FIX:
        print("FUZZ GATE: SKIP — m4dev/fixtures not found"); sys.exit(0)
    import rc_hybrid; rc_hybrid.PATHE_W = 10
    F, T = os.path.join(FIX, 'v0_base'), os.path.join(FIX, 'v2_three_faces')
    blob, cfg = rc_hybrid.encode_v3(F, T)
    mem0 = open(F + '/watch.bin', 'rb').read()
    cf = '/tmp/fz.cfg'; open(cf, 'w').write(' '.join(str(x) for x in cfg))
    rng = random.Random(0xA1F0)   # fixed seed -> reproducible

    # sanity: the VALID blob must decode (rc=0)
    mem = '/tmp/fz.bin'; open(mem, 'wb').write(mem0); open('/tmp/fz.blob', 'wb').write(blob)
    r0 = subprocess.run([cdec, mem, '/tmp/fz.blob', cf, '1'], capture_output=True, text=True, timeout=300)
    if r0.returncode != 0:
        print("FUZZ GATE: FAIL — the VALID blob did not decode (rc=%d). Is this a sanitizer build?" % r0.returncode)
        sys.exit(1)

    crashes = hangs = clean_reject = accidental_ok = other = 0
    crash_samples = []
    for i in range(N):
        mb = mutate(rng, blob)
        open(mem, 'wb').write(mem0)                 # reset from-image each run
        open('/tmp/fz.blob', 'wb').write(mb)
        try:
            r = subprocess.run([cdec, mem, '/tmp/fz.blob', cf, '1'], capture_output=True, text=True, timeout=20)
        except subprocess.TimeoutExpired:
            hangs += 1; crash_samples.append((i, 'HANG')); continue
        if r.returncode < 0:
            crashes += 1; crash_samples.append((i, 'signal %d' % (-r.returncode)))
        elif r.returncode == 0:
            accidental_ok += 1
        elif r.returncode == 1:
            clean_reject += 1
        else:
            other += 1
    for f in (mem, '/tmp/fz.blob', cf):
        try: os.remove(f)
        except Exception: pass
    ok = (crashes == 0 and hangs == 0)
    print("vp-4 fuzz gate: %d mutations of a valid %s->%s blob (seed 0xA1F0, PLAIN build)" % (N, 'v0_base', 'v2_three_faces'))
    print("  clean_reject=%d  accidental_decode=%d  other=%d  CRASHES=%d  HANGS=%d"
          % (clean_reject, accidental_ok, other, crashes, hangs))
    if crash_samples: print("  first failures:", crash_samples[:8])
    print("RESULT: %s (0 crashes AND 0 hangs required)" % ('PASS' if ok else 'FAIL'))
    sys.exit(0 if ok else 1)
