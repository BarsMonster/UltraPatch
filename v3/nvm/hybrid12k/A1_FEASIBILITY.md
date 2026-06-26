# A1 — on-device `ldr`-position derive (design + measurements)

**Goal.** Under the hard requirements {≤ 12 KiB SRAM, ≤ 1 write/page, in-place, no scratch}, stop
*shipping* `ldr` relocation field positions and instead *derive* them on-device (as `bl` already is),
removing the dominant wire cost. Verified end-to-end (golden + C); the result is the production
decoder (`RESULT.md`).

## Feasibility (encoder-side, 240 pairs)
On a Cortex-M0+ image, every shipped `ex` relocation position is an **`ldr` literal load** (`data`/
`code` pointer fields = 0 on this corpus), so the derivable share is **100 %**. A faithful local
`ldr` instruction→target scan over the from-image predicts **every** real `ldr` field (100 % recall,
0 false-negatives); ~8 % of predictions are non-relocating literal loads (false-positives), which are
de-relocated by a cheap 0-delta (identity) and so are correctness-safe. Gross saving from not
shipping `ldr` positions ≈ **176 KB ≈ 3.3 %** of the patch. (`tools/a1_feasibility.py`.)

## Selected predicate — SAME-OP causal (`_op_ldr_set`)
A field at `fpk` is `ldr` iff an `ldr` literal instruction **in the same op's copy range**
`[fp0, fp0+dl)` targets it: `(a & ~3) + 4*(up & 0xff) + 4 == fpk`, with the target also in-range and
the 4 bytes a pure copy. The encoder (over the from-image) and the decoder (over the op's source)
compute this **identically per op**, so positions are never on the wire; cross-op instruction→target
pairs are simply not derived and are absorbed by the existing `[C]` corrections — so encoder and
decoder agree by construction.

The decoder can read the op's source **pristine**: at op start, committed output never overlaps the
current op's `[fp0,fp0+dl)` (FWD `tp0 ≤ fp0` ⇒ prior output is below; grow prior output ≥ `tp0+nw ≥
fp0+dl` ⇒ above; the row buffer is uncommitted). This is the contract; the C realizes it with bounded
RAM, reading in the safe direction per op:
- **FWD (shrink):** the back-scan reads instruction bytes from a **2048 B ring** `g_psrc` populated by
  the walk's own pristine `hy_src` reads (ascending). The ring is 2× the 1024 B reach so `fpk-1024`
  never aliases `fpk`.
- **grow:** the back-scan reads via **journal-aware `hy_src`** — the instruction-window bytes the
  descending output frontier has already clobbered are preserved copy-source, so the journal returns
  them pristine (raw flash would be wrong).

No resident target store; the per-field delta values are pulled inline from the single range stream.

## Verified results (golden + C, full 256-pair NVM matrix)

| | byte-exact | `rows_amplified` | ARM `.bss` | corpus patch |
|---|---|---|---|---|
| **A1 (production)** | **256/256** | **0** (max 1 erase/row) | **11,168 B ≤ 12288** | **4,902,207 B = −2.452 % vs byte model** |

- Real +1-face update (113,124 ↔ 113,484, +360 B): **grow 917 / revert 627 B** (byte model 933/647).
- Crash-safe: plain-build fuzz over 300 corrupt patches → 0 crash/hang.
- The flash-compliant patch is **smaller than the byte-addressable byte model**, i.e. on-device flash
  correctness (no write amplification) now costs ~nothing in patch size.

## Reproduce
```
cc -O2 -DRC_V3_MAIN -DRC_V3_NVM -I c -o dec c/rc_v3.c c/flash_nvm.c
python3 -B tools/a1_golden_rt.py 10        # golden round-trip + size
python3 -B tools/hy_verify.py 10 dec       # C under NVM emulator: 256/256 + amp=0
python3 -B tools/a1_feasibility.py 10      # encoder-side derivability measurement
```
