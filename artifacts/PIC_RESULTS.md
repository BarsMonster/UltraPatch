# PIC experiment: does position-independent code shrink the delta? (GCC + LLVM)

Same change as before (register `counter_face`), built baseline + modified under each
toolchain/PIC mode for the PRO board. All patches below verified to reconstruct byte-exact.
Delta encoders: zstd `--patch-from` (generic), detools bsdiff+heatshrink (generic, 32KiB-safe),
detools `arm-cortex-m4`+heatshrink (relocation-aware, 32KiB-safe).

| config | base B | mod B | raw diff % | diff runs | zstd-PF | dt-bsdiff | dt-arm-cm4 |
|--------|-------:|------:|-----------:|----------:|--------:|----------:|-----------:|
| **gcc-nopic** (ref) | 113124 | 113484 | 58.8 | 3545 | 3829 | 4134 | **3189** |
| gcc-pic (-fPIC) | 117892 | 118308 | 56.8 | 3419 | 3898 | 4122 | 3460 |
| clang-nopic (ref) | 116500 | 116868 | 65.3 | 3661 | 4222 | 4554 | 3417 |
| clang-pic (-fPIC) | 119096 | 119488 | 65.1 | 5306 | 5306 | 5098 | 3650 |
| clang-ropi (-fropi) | 117844 | 118228 | 65.2 | 3648 | 4536 | 4599 | 3389 |
| clang-ropi+rwpi | — | — | — | — | — | — | LINK FAILS |

## Headline
**PIC does not reduce the delta — it is neutral-to-worse on every metric.** The smallest patch
remains plain **gcc-nopic + relocation-aware diff = 3189 B**. Every PIC variant is larger:
gcc-pic +8.5%, clang-pic +14.5%, clang-ropi +6.3% (vs the gcc-nopic reloc-aware patch).

## Why (confirmed mechanically)
- **ROPI+RWPI doesn't even link** with the GNU toolchain: `conflicting use of R9 / SB relative
  addressing conflicts`. RWPI reserves R9 as a static base, but newlib/libgcc aren't built RWPI.
  True ROPI/RWPI needs a position-independent runtime library (armclang ships one; GNU doesn't).
- **-fPIC just relocates the churn into a GOT.** Section dumps: gcc-pic gains a `.got` (1180 B) +
  `.got.plt`; clang-pic a `.got` (240 B). The GOT is itself a table of *absolute* addresses, so it
  still changes when code moves — plus PIC adds code/data (bigger binary → more to diff).
- **ROPI (clang -fropi) is GOT-free but bloats `.text`** (0x1c614 vs 0x1b38c nopic) via PC-relative
  address computation, and still leaves data-pointer + bulk-shift churn untouched.
- **PIC cannot stop the dominant churn — the bulk address shift.** Inserting a face still pushes all
  later code to higher addresses regardless of PIC. That positional shift (not absolute addressing)
  is what makes 58–65% of bytes differ; only a copy/insert-modeling differ collapses it.
- **clang codegen is larger and churnier than gcc** here (65% vs 59% raw diff, bigger patches).

## Verdict
The experiment confirms the earlier prediction: **"use relative addressing" (PIC) is the wrong
lever for delta size on this target.** Code is already PC-relative where it matters; PIC adds
overhead and a churning GOT without removing the positional shift. The real levers remain:
1. **Stable / append-only linker layout** so addresses don't move (kills the shift; zero runtime cost).
2. **Relocation-aware diffing** (detools arm-cortex-m4) to mop up absolute pointers at patch time.
