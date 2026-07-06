#!/usr/bin/env python3
"""
make_opc_pair.py  --  deterministic generator of a from/to Cortex-M0+ firmware pair that
forces the A1 encoder's OPC_CAP op-split fixpoint (opc_splits_sweep>=1) in the plan SWEEP.

Usage:  python3 make_opc_pair.py OUTDIR
        -> writes OUTDIR/from/{watch.bin,watch.elf} and OUTDIR/to/{watch.bin,watch.elf}

Then:   A1_DEGRADE_STATS=1 ./ultrapatch OUTDIR/from/watch.bin OUTDIR/to/watch.bin blob
                                                                    # prints opc_splits_sweep>=1
        cp OUTDIR/from/watch.bin mem.bin
        ./ultrapatch --decode --byte-mode mem.bin blob              # rc 0, mem == to/watch.bin

Mechanism (why it works):
  - The image is a big BL-dense `dispatch` (988 calls) over 247 leaf functions, each doing K=8
    dense LDR-literal loads of distinct globals.  This yields thousands of BL + LDR-literal
    relocation fields concentrated in a few large contiguous code regions.
  - `to` recompiles with (a) 38% of leaf BODIES perturbed (same call structure), (b) the dispatch
    call order permuted (callperm) so BL immediates churn densely, and (c) every global value
    scattered (gscatter) so the literal pool churns.  This dense field churn defeats plain bsdiff
    anchoring, so the encoder's masked plan variant (mask BL immediates, PLANS variant 2) copies
    long spans through the recompiled code.
  - Masking forces long copies through mixed-size (2- and 4-byte Thumb) recompiled code.  Where a
    masked copy maps a from-field onto a to-position that is not a valid field, merge_op_field_deltas
    cannot supply an exact delta, so the residual becomes a correction.  These corrections
    concentrate enough that at least one op exceeds the OPC_CAP of 80, so plan_encode splits it to a
    fixpoint -> opc_splits_sweep>=1.  The masked variant does not win here (a cleaner variant ships,
    so opc_splits=0 in the shipped plan), but the sweep exercises the split machinery, which is what
    check_degrade.sh case (b) asserts.  Every emitted blob round-trips.

Requires arm-none-eabi-gcc (tested: 14.2.1).  gcc output is deterministic, so the pair is fixed.
"""
import os, sys, subprocess

CC = "arm-none-eabi-gcc"
# --- the proven configuration (do not change: chosen so the masked plan wins AND splits) ---
NFNS, NCALLS, SEED, K = 247, 988, 37, 8
CHURN, CALLPERM, GSCATTER = 36, 16, 1

LINK = ("ENTRY(_start)\nSECTIONS { . = 0x0; "
        ".text : { *(.text*) *(.rodata*) } .data : { *(.data*) } "
        ".bss : { *(.bss*) *(COMMON) } "
        "/DISCARD/ : { *(.ARM.*) *(.comment) *(.note*) } }\n")

def lcg(x): return (x * 1103515245 + 12345) & 0x7fffffff

def source(is_to):
    churn    = CHURN    if is_to else 0
    callperm = CALLPERM if is_to else 0
    gscatter = GSCATTER if is_to else 0
    L, nglob = [], max(NFNS, K + 4)
    for i in range(nglob):
        base = (i * 2654435761) & 0xffffffff
        extra = (gscatter * ((i * 40503 + 7) & 0xffff)) & 0xffffffff if gscatter else 0
        L.append(f"volatile unsigned G{i} = {(base + extra) & 0xffffffff}u;")
    L.append("volatile unsigned g_sink;")
    r = SEED
    for i in range(NFNS):
        r = lcg(r); churned = (r % 100) < churn
        gi = i % nglob
        stmts = [" ".join(f"g_sink+=G{(gi + j * 7) % nglob};" for j in range(K))]
        if churned:
            r = lcg(r); k = r % 7
            stmts.append(f"g_sink^=(g_sink<<{1 + (k % 5)})+G{(gi + 3) % nglob};")
        L.append(f"__attribute__((noinline)) static void f{i}(void){{ {' '.join(stmts)} }}")
    L.append("__attribute__((noinline)) void dispatch(int n){")
    L.append("  if(n<0) return;")
    order = ([(c * callperm + (c % 5)) % NFNS for c in range(NCALLS)] if callperm
             else [c % NFNS for c in range(NCALLS)])
    for c in order:
        L.append(f"  f{c % NFNS}();")
    L.append("}")
    L.append("void _start(void){ dispatch(1); for(;;){} }")
    return "\n".join(L) + "\n"

def build(outdir, is_to):
    d = os.path.join(outdir, "to" if is_to else "from")
    os.makedirs(d, exist_ok=True)
    cpath, lpath = os.path.join(d, "fw.c"), os.path.join(d, "link.ld")
    elf, binp = os.path.join(d, "watch.elf"), os.path.join(d, "watch.bin")
    open(cpath, "w").write(source(is_to)); open(lpath, "w").write(LINK)
    subprocess.run([CC, "-mcpu=cortex-m0plus", "-mthumb", "-Os", "-ffunction-sections",
                    "-fdata-sections", "-nostdlib", "-nostartfiles", "-fno-builtin",
                    "-Wl,--gc-sections", "-T", lpath, cpath, "-o", elf], check=True)
    subprocess.run(["arm-none-eabi-objcopy", "-O", "binary", elf, binp], check=True)
    return os.path.getsize(binp)

if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "opc_pair"
    fb = build(out, False); tb = build(out, True)
    print(f"wrote {out}/from (bin={fb}) and {out}/to (bin={tb})")
