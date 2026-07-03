#!/usr/bin/env python3
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT
"""
Deterministic adversarial campaign driver for the A1 host ENCODER (hy_enc).

The encoder self-verifies every emitted blob on the reference decoder
(src/patch_selfcheck.c), so the contract for each generated (from,to) pair is:

  * hy_enc exits 0  -> a blob was written; the SANITIZED build proves it did so
    without a memory/UB error, and we additionally re-apply it with hy_dec and
    require a byte-exact round-trip to `to` (a mismatch is a CRITICAL finding:
    self-check should already guarantee it).
  * hy_enc exits nonzero with a plain die() message (no sanitizer text) -> a
    clean refusal. That is DESIGNED behavior, logged, not a finding.
  * sanitizer output (ASan/UBSan/LSan), a timeout (hang), or a crash with no
    clean refusal -> a FINDING.

Every case is fully determined by (class, index, master seed), so any finding
reproduces: rerun with --only <case-name> (inputs are also dumped under the
findings dir). Intended to run against a hy_enc built with
-fsanitize=address,undefined (Makefile target hy_enc_asan).

Usage:
  scripts/fuzz_encoder.py --enc ./hy_enc_asan --dec ./hy_dec [options]
    --seed N          master seed (default 1)
    --timeout SECS    per-encode wall clock (default 30); exceeding = HANG finding
    --smoke           run only the fast ~30 s smoke subset (for `make check-encfuzz`)
    --only NAME       run only the case whose name contains NAME (repro)
    --base-elf PATH   real ELF used as the mutation base for the ELF class
    --keep            keep the scratch dir on success too
    --list            print the case list and exit
Exit status: 0 if no findings, 1 if any finding, 2 on setup error.
"""
import argparse, os, shutil, struct, subprocess, sys, tempfile, time

# ------------------------------------------------------------------ determinism
class LCG:
    """Portable LCG byte source (Numerical-Recipes constants). Fully reproducible
    across Python versions, unlike random.Random's internals."""
    def __init__(self, seed):
        self.s = (seed ^ 0x2545F491) & 0xffffffff
    def u32(self):
        self.s = (self.s * 1664525 + 1013904223) & 0xffffffff
        return self.s
    def byte(self):
        return (self.u32() >> 16) & 0xff
    def bytes(self, n):
        return bytes(self.byte() for _ in range(n))
    def below(self, n):
        return self.u32() % n if n else 0

# --------------------------------------------------------------- byte generators
def const_bytes(n, b):
    return bytes([b]) * n

def alt_bytes(n, b0, b1):
    return bytes([b0 if (i & 1) == 0 else b1 for i in range(n)])

def rand_bytes(n, seed):
    return LCG(seed).bytes(n)

def bl_halfwords(imm, seed_lo=0):
    """Encode one BL (up,lo) pair as 4 LE bytes for the given 24-bit-ish immediate.
    Mirrors arm_cortex_m4 pack_bl closely enough to look like a real BL to the
    scanner: up = 0xF000|..., lo = 0xD000|..."""
    imm32 = imm & 0xffffff
    s = (imm32 >> 23) & 1
    i1 = (imm32 >> 22) & 1
    i2 = (imm32 >> 21) & 1
    j1 = 1 - (i1 ^ s)
    j2 = 1 - (i2 ^ s)
    imm10 = (imm32 >> 11) & 0x3ff
    imm11 = imm32 & 0x7ff
    up = 0xF000 | (s << 10) | imm10
    lo = 0xD000 | (j1 << 13) | (j2 << 11) | imm11
    return struct.pack("<HH", up, lo)

def ldr_lit_halfword(imm8):
    """LDR (literal): up & 0xf800 == 0x4800, low byte is the word immediate."""
    return struct.pack("<H", 0x4800 | (imm8 & 0xff))

def thumb_stream(n, seed, dense=True, misalign=False, straddle=False):
    """Build a Thumb-like byte stream densely populated with BL and LDR-literal
    patterns. `misalign` inserts an odd padding byte so 4-byte fields land at
    odd offsets; `straddle` truncates the final field mid-encoding at EOF."""
    r = LCG(seed)
    out = bytearray()
    if misalign:
        out.append(r.byte())
    while len(out) < n:
        pick = r.below(4)
        if pick == 0 or (dense and pick != 3):
            out += bl_halfwords(r.u32(), r.u32())
        elif pick == 1:
            out += ldr_lit_halfword(r.byte())
            out += struct.pack("<H", r.u32() & 0xffff)  # filler halfword
        elif pick == 2:
            out += struct.pack("<H", 0xf8df)            # ldr.w opcode
            out += struct.pack("<H", r.u32() & 0xfff)   # imm12
        else:
            out += struct.pack("<H", r.u32() & 0xffff)
    out = out[:n]
    if straddle and n >= 1:
        # leave a partial BL/LDR opcode as the very last byte(s)
        tail = r.below(3)
        if tail == 0:
            out[-1] = 0xf0            # BL first-halfword high byte at EOF
        elif tail == 1 and n >= 2:
            out[-2] = 0xf0; out[-1] = 0x00   # full BL up-half, lo-half missing
        else:
            out[-1] = 0x48            # LDR-literal opcode high byte at EOF
    return bytes(out)

def thumb_perturbed(src, seed, permille):
    """Churn BL immediates in a Thumb stream (correction-storm approximation):
    walk 4-byte-aligned, rewrite ~permille/1000 of the BL fields with a new imm."""
    r = LCG(seed)
    b = bytearray(src)
    i = 0
    while i + 4 <= len(b):
        up = b[i] | (b[i + 1] << 8)
        lo = b[i + 2] | (b[i + 3] << 8)
        if (up & 0xf800) == 0xf000 and (lo & 0xd000) == 0xd000:
            if r.below(1000) < permille:
                b[i:i + 4] = bl_halfwords(r.u32(), r.u32())
            i += 4
        else:
            i += 2
    return bytes(b)

# --------------------------------------------------------------- ELF mutations
def elf_mut(base, kind, seed):
    """Return an adversarially mutated copy of a real ELF32-LE image."""
    b = bytearray(base)
    r = LCG(seed)
    n = len(b)
    if kind == "trunc_header":
        return bytes(b[:r.below(52)])                     # < 52 -> die at gate
    if kind == "trunc_random":
        return bytes(b[:1 + r.below(max(1, n))])          # arbitrary truncation
    if kind == "bad_class":
        if n > 5: b[4] = r.byte() | 0x80                  # EI_CLASS != 1
        return bytes(b)
    if kind == "bad_endian":
        if n > 6: b[5] = 2                                 # EI_DATA = big-endian
        return bytes(b)
    if kind == "shoff_past_eof":
        struct.pack_into("<I", b, 32, n + 0x1000)          # e_shoff past EOF
        return bytes(b)
    if kind == "shnum_huge":
        struct.pack_into("<H", b, 48, 0xffff)              # e_shnum enormous
        return bytes(b)
    if kind == "shentsize_small":
        struct.pack_into("<H", b, 46, r.below(40))         # shentsize < 40
        return bytes(b)
    if kind == "shnum_zero":
        struct.pack_into("<H", b, 48, 0)                   # e_shnum = 0
        return bytes(b)
    if kind == "corrupt_shdrs":
        # blow up section offsets/sizes to point past EOF / be huge
        shoff = struct.unpack_from("<I", b, 32)[0]
        shent = struct.unpack_from("<H", b, 46)[0]
        shnum = struct.unpack_from("<H", b, 48)[0]
        if shent >= 40 and shoff and shoff + shent * shnum <= n:
            for i in range(min(shnum, 200)):
                p = shoff + i * shent
                if r.below(2):
                    struct.pack_into("<I", b, p + 16, (n + r.u32()) & 0xffffffff)  # sh_offset
                if r.below(2):
                    struct.pack_into("<I", b, p + 20, r.u32())                     # sh_size huge
        return bytes(b)
    if kind == "corrupt_symtab":
        # find SHT_SYMTAB sections and scribble sym st_value / st_shndx
        shoff = struct.unpack_from("<I", b, 32)[0]
        shent = struct.unpack_from("<H", b, 46)[0]
        shnum = struct.unpack_from("<H", b, 48)[0]
        if shent >= 40 and shoff and shoff + shent * shnum <= n:
            for i in range(shnum):
                p = shoff + i * shent
                st = struct.unpack_from("<I", b, p + 4)[0]
                off = struct.unpack_from("<I", b, p + 16)[0]
                sz = struct.unpack_from("<I", b, p + 20)[0]
                es = struct.unpack_from("<I", b, p + 36)[0]
                if st == 2 and es >= 16 and off + sz <= n:
                    k = 0
                    while k + es <= sz and k < 4096:
                        if r.below(3) == 0:
                            struct.pack_into("<I", b, off + k + 4, r.u32())        # st_value
                        if r.below(3) == 0:
                            struct.pack_into("<H", b, off + k + 6, r.u32() & 0xffff)  # st_shndx
                        k += es
        return bytes(b)
    if kind == "flip_bytes":
        for _ in range(1 + r.below(64)):
            if n: b[r.below(n)] = r.byte()
        return bytes(b)
    raise ValueError(kind)

def elf_handbuilt(kind):
    """Minimal hand-built pathological ELF32-LE headers (no valid section data)."""
    hdr = bytearray(64)
    hdr[0:4] = b"\x7fELF"
    hdr[4] = 1   # ELFCLASS32
    hdr[5] = 1   # ELFDATA2LSB
    hdr[6] = 1
    struct.pack_into("<H", hdr, 16, 2)     # e_type ET_EXEC
    struct.pack_into("<H", hdr, 18, 40)    # e_machine EM_ARM
    struct.pack_into("<I", hdr, 20, 1)     # e_version
    if kind == "min_valid_shell":
        # header claims 1 section of size 40 at offset 64, but file ends -> off+ent>eof
        struct.pack_into("<I", hdr, 32, 64)
        struct.pack_into("<H", hdr, 46, 40)
        struct.pack_into("<H", hdr, 48, 1)
        return bytes(hdr)
    if kind == "shoff_overflow":
        struct.pack_into("<I", hdr, 32, 0xfffffff0)
        struct.pack_into("<H", hdr, 46, 40)
        struct.pack_into("<H", hdr, 48, 0xffff)
        return bytes(hdr)
    if kind == "all_zero_after_ident":
        return bytes(hdr)      # e_shoff/shnum = 0 -> die "bad ELF sections"
    return bytes(hdr)

def dhash(s):
    """Deterministic small hash (Python's hash() is per-process randomized)."""
    h = 2166136261
    for ch in s.encode():
        h = ((h ^ ch) * 16777619) & 0xffffffff
    return h

# --------------------------------------------------------------- case catalogue
# SMALL sizes are safe to run under the ASan build (10-15x slower); boundary OOB is
# size-independent so they carry the memory-safety coverage. LARGE sizes (64K..300K)
# are tagged big=True: the encoder's match-finding is super-linear on low-entropy data,
# so run those under the PLAIN self-checking encoder (--large) for scale/round-trip.
SIZES = [0, 1, 3, 4096, 16384]
BIG = 300000
DEGEN = 1024

def build_cases(seed, base_elf):
    """Yield case dicts: {name, cls, frm(bytes), to(bytes), felf, toelf, big}."""
    cases = []
    def add(name, cls, frm, to, felf=None, toelf=None, big=False):
        cases.append(dict(name=name, cls=cls, frm=frm, to=to, felf=felf,
                          toelf=toelf, big=big))

    # ---- class R: random images, equal and unequal sizes ----
    idx = 0
    for sa in SIZES:
        for sb in SIZES:
            idx += 1
            add(f"rand_{sa}_{sb}", "random",
                rand_bytes(sa, seed + idx), rand_bytes(sb, seed + 1000 + idx))
    # a couple of large ones (big=True: run under the plain encoder via --large)
    add("rand_big_equal", "random", rand_bytes(BIG, seed + 7), rand_bytes(BIG, seed + 8), big=True)
    add("rand_big_uneq", "random", rand_bytes(BIG, seed + 9), rand_bytes(4096, seed + 10), big=True)
    # near-identical random (small diff -> bsdiff/LZ real work)
    base = rand_bytes(65536, seed + 11)
    mut = bytearray(base)
    rr = LCG(seed + 12)
    for _ in range(200):
        mut[rr.below(len(mut))] = rr.byte()
    add("rand_neardup", "random", base, bytes(mut), big=True)

    # ---- class D: degenerate content ----
    degens = {
        "zeros": const_bytes(DEGEN, 0x00),
        "ffs": const_bytes(DEGEN, 0xff),
        "repA": const_bytes(DEGEN + 1, 0x41),
        "alt": alt_bytes(DEGEN, 0xaa, 0x55),
        "alt_odd": alt_bytes(DEGEN - 1, 0x12, 0x34),
        "maxent": rand_bytes(DEGEN, seed + 20),
    }
    names = list(degens)
    for i, a in enumerate(names):
        for bb in names[i:]:
            add(f"degen_{a}_{bb}", "degenerate", degens[a], degens[bb])
    # degenerate against random and empty
    add("degen_zeros_empty", "degenerate", degens["zeros"], b"")
    add("degen_empty_ffs", "degenerate", b"", degens["ffs"])
    add("degen_zeros_rand", "degenerate", degens["zeros"], rand_bytes(DEGEN, seed + 21))

    # ---- class T: Thumb-like structured ----
    for i, (dense, mis, strad) in enumerate([
            (True, False, False), (True, True, False), (True, False, True),
            (False, False, False), (True, True, True)]):
        fa = thumb_stream(9000, seed + 30 + i, dense, mis, strad)
        tb = thumb_perturbed(fa, seed + 40 + i, 120)
        add(f"thumb_{'dense' if dense else 'sparse'}"
            f"{'_mis' if mis else ''}{'_strad' if strad else ''}",
            "thumb", fa, tb)
    # tiny straddle sizes (fields cut at EOF): 1..7 byte tails
    for sz in (1, 2, 3, 5, 6, 7, 9, 4095, 4097):
        add(f"thumb_straddle_{sz}", "thumb",
            thumb_stream(sz, seed + 60 + sz, True, False, True),
            thumb_stream(sz, seed + 80 + sz, True, True, True))
    # BL/LDR opcode as the final bytes, exact-length images
    add("thumb_bl_at_eof", "thumb", bl_halfwords(0x1234)[:3] + b"", const_bytes(4, 0x48))
    add("thumb_ldr_oob_imm", "thumb",
        ldr_lit_halfword(0xff) + const_bytes(2, 0),   # imm points far past 4-byte image
        ldr_lit_halfword(0x00) + const_bytes(2, 0))

    # ---- class G: degradation / resource stress ----
    # huge journal demand: >384 KiB span with scattered single-byte flips
    big = rand_bytes(BIG, seed + 90)
    bigm = bytearray(big)
    rg = LCG(seed + 91)
    for _ in range(4000):
        bigm[rg.below(len(bigm))] ^= (rg.byte() | 1)
    add("stress_journal_scatter", "stress", big, bytes(bigm), big=True)
    # dense correction storm via heavy Thumb churn
    cs = thumb_stream(60000, seed + 92, True, False, False)
    add("stress_corr_storm", "stress", cs, thumb_perturbed(cs, seed + 93, 600), big=True)
    # growth across page boundary
    add("stress_grow_span", "stress",
        rand_bytes(255, seed + 94), rand_bytes(257 * 512, seed + 95), big=True)
    # incompressible huge (LZ window / suffix-sort stress)
    add("stress_incompressible", "stress",
        rand_bytes(200000, seed + 96), rand_bytes(200000, seed + 97), big=True)

    # ---- class E: ELF path (mutated real ELF + hand-built pathological) ----
    if base_elf is not None:
        bin_for_elf = rand_bytes(2048, seed + 100)   # bin that won't match data seg
        elf_kinds = ["trunc_header", "trunc_random", "bad_class", "bad_endian",
                     "shoff_past_eof", "shnum_huge", "shentsize_small", "shnum_zero",
                     "corrupt_shdrs", "corrupt_symtab", "flip_bytes"]
        for k in elf_kinds:
            for j in range(3):   # a few seeds each
                felf = elf_mut(base_elf, k, seed + 200 + j + dhash(k) % 97)
                add(f"elf_{k}_{j}", "elf", bin_for_elf, rand_bytes(64, seed + 300 + j),
                    felf=felf, toelf=None)
        for k in ["min_valid_shell", "shoff_overflow", "all_zero_after_ident"]:
            add(f"elf_hb_{k}", "elf", bin_for_elf, rand_bytes(64, seed + 400),
                felf=elf_handbuilt(k), toelf=None)
        # valid ELF on BOTH sides, mutated bin: exercises full code/data windows
        add("elf_valid_both", "elf", base_bin_guess(base_elf), rand_bytes(64, seed + 401),
            felf=base_elf, toelf=None)
    return cases

def base_bin_guess(_elf):
    # placeholder bin; replaced at runtime by the real matching bin when available
    return b"\x00" * 256

# fast (<~3 s each under ASan) representative-of-every-class subset for `make check-encfuzz`
SMOKE = {"rand_0_0", "rand_1_3", "rand_3_1", "rand_4096_4096", "degen_zeros_ffs",
         "degen_alt_alt", "degen_zeros_empty", "thumb_dense", "thumb_straddle_3",
         "thumb_ldr_oob_imm", "elf_trunc_header_0", "elf_shoff_past_eof_0",
         "elf_corrupt_symtab_0", "elf_hb_min_valid_shell"}

# --------------------------------------------------------------- runner
SAN_MARKERS = ("runtime error:", "AddressSanitizer", "UndefinedBehaviorSanitizer",
               "LeakSanitizer", "SUMMARY: ", "ERROR: ")

def is_sanitizer(text):
    return any(m in text for m in SAN_MARKERS)

def write_dir(root, name, binb, elfb):
    d = os.path.join(root, name)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "watch.bin"), "wb") as f:
        f.write(binb)
    if elfb is not None:
        with open(os.path.join(d, "watch.elf"), "wb") as f:
            f.write(elfb)
    return d

def run_case(c, enc, dec, workdir, timeout, findings_dir, keep):
    scratch = tempfile.mkdtemp(dir=workdir, prefix=c["name"] + "_")
    fdir = write_dir(scratch, "from", c["frm"], c["felf"])
    tdir = write_dir(scratch, "to", c["to"], c["toelf"])
    blob = os.path.join(scratch, "out.blob")
    env = dict(os.environ)
    env["ASAN_OPTIONS"] = "detect_leaks=0:abort_on_error=1:exitcode=99"
    env["UBSAN_OPTIONS"] = "halt_on_error=1:abort_on_error=1:print_stacktrace=1:exitcode=99"
    verdict, detail = "OK", ""
    t0 = time.time()
    try:
        p = subprocess.run([enc, fdir, tdir, blob, "10"], env=env,
                           stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                           timeout=timeout)
        err = p.stderr.decode("utf-8", "replace")
        if is_sanitizer(err):
            verdict, detail = "SANITIZER", err[-2000:]
        elif p.returncode == 0:
            # success -> require byte-exact round-trip through the real decoder
            if not os.path.exists(blob):
                verdict, detail = "NOBLOB", "exit 0 but no blob written"
            else:
                mem = os.path.join(scratch, "mem.bin")
                shutil.copyfile(os.path.join(fdir, "watch.bin"), mem)
                dp = subprocess.run([dec, mem, blob, "1"], stdout=subprocess.DEVNULL,
                                   stderr=subprocess.PIPE, timeout=timeout)
                if dp.returncode != 0:
                    verdict, detail = "ROUNDTRIP_FAIL", "hy_dec rejected a self-verified blob"
                elif open(mem, "rb").read() != c["to"]:
                    verdict, detail = "ROUNDTRIP_FAIL", "decoded image != to"
                else:
                    verdict = "SUCCESS"
        elif p.returncode < 0:
            verdict, detail = "CRASH", f"killed by signal {-p.returncode}\n{err[-1000:]}"
        else:
            verdict, detail = "REFUSAL", err.strip().split("\n")[-1][:200]
    except subprocess.TimeoutExpired:
        verdict, detail = "HANG", f"exceeded {timeout}s"
    dt = time.time() - t0
    is_finding = verdict in ("SANITIZER", "CRASH", "HANG", "ROUNDTRIP_FAIL", "NOBLOB")
    if is_finding:
        os.makedirs(findings_dir, exist_ok=True)   # created lazily: clean runs leave no residue
        keepdir = os.path.join(findings_dir, c["name"])
        shutil.copytree(scratch, keepdir, dirs_exist_ok=True)
    if not keep and not is_finding:
        shutil.rmtree(scratch, ignore_errors=True)
    return verdict, detail, dt

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--enc", default="./hy_enc_asan")
    ap.add_argument("--dec", default="./hy_dec")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--timeout", type=int, default=30)
    ap.add_argument("--smoke", action="store_true")
    ap.add_argument("--large", action="store_true",
                    help="include the big (64K..300K) cases; intended for the PLAIN "
                         "self-checking encoder (they are super-linear on low-entropy data)")
    ap.add_argument("--only", default=None)
    ap.add_argument("--base-elf", default="test-bench/images/img_15_n83/watch.elf")
    ap.add_argument("--base-bin", default="test-bench/images/img_15_n83/watch.bin")
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--list", action="store_true")
    args = ap.parse_args()

    base_elf = None
    if args.base_elf and os.path.exists(args.base_elf):
        base_elf = open(args.base_elf, "rb").read()
    cases = build_cases(args.seed, base_elf)
    # patch in the real matching bin for the elf_valid_both case
    if base_elf is not None and os.path.exists(args.base_bin):
        real_bin = open(args.base_bin, "rb").read()
        for c in cases:
            if c["name"] == "elf_valid_both":
                c["frm"] = real_bin
    if not args.large:
        cases = [c for c in cases if not c["big"]]
    if args.smoke:
        cases = [c for c in cases if c["name"] in SMOKE]
    if args.only:
        cases = [c for c in cases if args.only in c["name"]]
    if args.list:
        for c in cases:
            print(f"{c['cls']:11s} {c['name']}")
        print(f"total {len(cases)} cases")
        return 0
    if not os.path.exists(args.enc):
        sys.stderr.write(f"encoder not found: {args.enc}\n")
        return 2

    workdir = tempfile.mkdtemp(prefix="encfuzz_")
    findings_dir = os.path.join(os.getcwd(), "encfuzz-findings")  # created lazily on first finding
    counts = {}
    findings = []
    t0 = time.time()
    for i, c in enumerate(cases):
        verdict, detail, dt = run_case(c, args.enc, args.dec, workdir,
                                       args.timeout, findings_dir, args.keep)
        counts[verdict] = counts.get(verdict, 0) + 1
        tag = "!!" if verdict in ("SANITIZER", "CRASH", "HANG", "ROUNDTRIP_FAIL",
                                  "NOBLOB") else "  "
        print(f"{tag} [{i+1}/{len(cases)}] {c['name']:28s} {verdict:14s} {dt:5.1f}s"
              + (f"  {detail.splitlines()[0][:80]}" if detail else ""))
        sys.stdout.flush()
        if tag == "!!":
            findings.append((c["name"], c["cls"], verdict, detail))
    if not args.keep:
        shutil.rmtree(workdir, ignore_errors=True)
    elapsed = time.time() - t0
    print("\n==================== ENCODER FUZZ SUMMARY ====================")
    print(f"seed={args.seed} cases={len(cases)} elapsed={elapsed:.1f}s")
    for k in sorted(counts):
        print(f"  {k:14s}: {counts[k]}")
    if findings:
        print(f"\n*** {len(findings)} FINDING(S) (inputs under {findings_dir}) ***")
        for name, cls, verdict, detail in findings:
            print(f"  [{verdict}] {cls}/{name}")
            for ln in detail.splitlines()[:6]:
                print(f"      {ln}")
        return 1
    print("\nNO FINDINGS: every pair either round-tripped byte-exactly or refused cleanly.")
    return 0

if __name__ == "__main__":
    sys.exit(main())
