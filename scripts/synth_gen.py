# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

# Single source of truth for every synthetic wire-surface fixture: one fixed-seed LCG, the full
# mode vocabulary, and the two NAMED golden pin pairs. check_edge.sh, check_degrade.sh and
# check_golden.sh all call this. The golden pins ARE check_degrade cases (a) and (c) (same PINS
# entry), so fixture==pin holds by construction — no copy to drift. No committed binaries.
#
# Subcommands (path args first so the shell wrappers stay one-liners):
#   img  <path> <size> <mode> [args...]     size-based single image (check_edge)
#       rand <seed> | const <byte> | text | mutate <src> <seed> <permille> | insert <src> <n> <seed>
#   role <path> <from|to> <mode> [args...]  role-derived single image (check_degrade); a pair is
#                                           reproducible from its parameters alone
#       rand <n> <seed> | swap <H> <seed> | rshift <n> <seed> <a> <b> <k> | highswap <lo> <M> <seed>
#   pin  <path> <from|to> <name>            one role of a NAMED golden pin
#   pins <outdir> [name...]                 named pins -> <outdir>/<name>_{from,to}/watch.bin
import os, sys

def lcg(seed):
    s = seed & 0xffffffff
    while True:
        s = (s * 1664525 + 1013904223) & 0xffffffff
        yield (s >> 16) & 0xff

def rnd(n, seed):
    r = lcg(seed); return bytes(next(r) for _ in range(n))

def img(size, mode, a):  # size-based (check_edge): fixed-size images and derivations of a source
    if mode == "rand":
        return rnd(size, int(a[0]))
    if mode == "const":
        return bytes([int(a[0], 0)]) * size
    if mode == "text":
        line = b"The quick brown fox jumps over the lazy dog %d.\n"
        return b"".join(line % i for i in range(size))[:size]
    if mode == "mutate":
        src = bytearray(open(a[0], "rb").read())
        r = lcg(int(a[1])); permille = int(a[2])
        for i in range(len(src)):
            if (next(r) * 4) % 1000 < permille:
                src[i] ^= next(r) or 1
        return bytes(src[:size]) if size else bytes(src)
    if mode == "insert":
        src = open(a[0], "rb").read()
        r = lcg(int(a[2])); pre = bytes(next(r) for _ in range(int(a[1])))
        return (pre + src)[:size] if size else pre + src
    sys.exit("bad mode " + mode)

def role(r, mode, a):  # role-derived (check_degrade): from/to share a seed so a pair is reproducible
    if mode == "rand":
        return rnd(int(a[0]), int(a[1]))
    if mode == "swap":                                  # from=base(2H); to=second-half ++ first-half
        H, seed = int(a[0]), int(a[1]); base = rnd(2 * H, seed)
        return base if r == "from" else base[H:] + base[:H]
    if mode == "rshift":                                # region [p,q) shifted RIGHT by k
        n, seed, p, q, k = (int(x) for x in a[:5]); base = rnd(n, seed)
        if r == "from":
            data = base
        else:
            ins = rnd(k, seed ^ 0x5a5a5a5a)
            data = base[:p] + ins + base[p:q] + base[q + k:]
        assert len(data) == n, (len(data), n)
        return data
    if mode == "highswap":                              # identity below lo, top-M two halves swapped
        lo, M, seed = (int(x) for x in a[:3]); base = rnd(lo + M, seed); H = M // 2
        data = base if r == "from" else base[:lo] + base[lo + H:lo + M] + base[lo:lo + H]
        assert len(data) == lo + M
        return data
    sys.exit("bad mode " + mode)

# The two golden synthetic wire-surface pins. Each IS a check_degrade case, so the golden manifest
# and the degrade gate cannot diverge: (a) journal-BUDGET degradation, (c) unnatural apply direction.
PINS = {
    "synth_journal_degrade": ("swap", ["2048", "88"]),                             # case (a)
    "synth_unnatural_dir":   ("rshift", ["4096", "444", "256", "3400", "600"]),    # case (c)
}

def main():
    cmd = sys.argv[1]
    if cmd == "img":
        open(sys.argv[2], "wb").write(img(int(sys.argv[3]), sys.argv[4], sys.argv[5:]))
    elif cmd == "role":
        open(sys.argv[2], "wb").write(role(sys.argv[3], sys.argv[4], sys.argv[5:]))
    elif cmd == "pin":
        mode, a = PINS[sys.argv[4]]
        open(sys.argv[2], "wb").write(role(sys.argv[3], mode, a))
    elif cmd == "pins":
        outdir = sys.argv[2]
        for name in (sys.argv[3:] or list(PINS)):
            mode, a = PINS[name]
            for r in ("from", "to"):
                d = os.path.join(outdir, name + "_" + r); os.makedirs(d, exist_ok=True)
                open(os.path.join(d, "watch.bin"), "wb").write(role(r, mode, a))
    else:
        sys.exit("bad cmd " + cmd)

main()
