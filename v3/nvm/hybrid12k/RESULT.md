# ultrapatch v3-on-flash (A1) — PRODUCTION RESULT

The selected production decoder. Streaming, in-place, real-NVM firmware patcher (SAML22,
Cortex-M0+): a single divide-free range stream is decoded byte by byte and the new image is
reconstructed directly in flash, honoring page/row erase semantics with no write amplification.

## Requirements
1. **≤ 1 write per flash page/row** (no write amplification) — every row is erased+programmed 0 or 1
   times, never more (`nvm_rows_amplified` MUST be 0). The hard gate.
2. **In-place, no scratch flash** — no second bank or staging region.
3. **≤ 12 KiB SRAM** during decode (device has 32 KiB; updates run in a dedicated updater mode).
4. **No power-fail atomicity** — on power loss the host does a full reflash, so the decoder needs no
   rollback and may freely destroy source as it goes (this is what lets it overwrite source in-place).

Requirements 1 + 2 are why the decoder does **no baking / no source rewrites**: physically baking
de-relocated values into source rows that the in-place apply later overwrites would write those rows
≥ 2 times. Instead, relocations are applied at the monotonic output frontier (below).

## Gates — all PASS (measured under `c/flash_nvm.c` + `arm-none-eabi`, full 256-pair matrix)

| gate | result |
|---|---|
| byte-exact 256/256, fed 1 byte at a time, under the NVM emulator | **PASS** |
| ≤ 1 write/page — `rows_amplified = 0`, max 1 erase/row | **PASS** (wear mean 0.763× the span/256 floor) |
| sequential page writes — `frontier_inversions = 0` (rows finalized in one monotonic direction) | **PASS** (now an explicit emulator gate, not an inference) |
| in-place, no scratch flash | **PASS** |
| **≤ 12 KiB SRAM** (ARM `arm-none-eabi-size`, `.bss`) | **PASS — 11,040 B** (1,248 B margin; `.data = 0`) |
| divide-free hot path (`tools/check_divfree.sh`) | **PASS** (2 `__aeabi_uidiv`, both once-per-decode in the init path; 0 HW udiv/sdiv) |
| crash-safe (plain build, 300 corrupt patches) | **PASS** — 0 crash/hang (300 clean-reject); a reject now reports a reason (resource-cap vs corrupt) and an 8-byte coroutine-stack canary turns any overflow into a clean reject |

## Patch size
Corpus total **4,902,207 B**: **−0.657 % vs the prior best-compression reference** (4,934,646),
**−3.948 % vs v2**, and **−2.452 % vs the byte-addressable byte model** (5,025,418 — a NVM-invalid
reference: it assumes byte-writable flash). The flash-compliant patch is now smaller than all tracked
references. The latest A1 follow-up wins are pauseable LZSS tokens across op/delta interleave points
(no decoder SRAM cost), a `UG_CTX=7`/one-shot-model-slot SRAM retune, and small state-layout
packing. The final packing pass is wire-compatible and saves **128 B `.bss`** with no patch-size
change.

Real one-face firmware update (v0_base 113,124 ↔ v1_one_face 113,484, +360 B), decoded under the
emulator, `rows_amplified=0`: **grow = 917 B, revert = 627 B** (byte model: 933 / 647).

## Architecture
No baking, no source writes. The `[A]` copy reads raw `from[fp]`; reconstruction is corrected at the
monotonic output frontier (Path-style `[C]`). Relocation fields are de-relocated in apply order:
- **`bl`** positions are derived on-device by a local halfword pattern match (self-framing). A
  BL-looking field whose 4 bytes are not a pure copy is an implicit suppressed-BL normal copy, so
  no suppressed-BL positions are shipped or stored.
- **`ldr`** positions are **derived on-device per op** (A1): a field at `fpk` is `ldr` iff an `ldr`
  literal instruction in the *same op's* copy range targets it (`(a&~3)+4*(up&0xff)+4 == fpk`),
  reading pristine source. FWD reads instructions from a 2048 B ring of the walk's own pristine
  reads; grow reads via the journal (its lower instruction-window bytes are preserved copy-source).
  Encoder and decoder run the identical per-op predicate, so positions are never shipped; cross-op
  pairs are absorbed by `[C]`. (See `A1_FEASIBILITY.md`.)
- Per-field **delta values** are pulled inline from the single range stream at detection (adaptive
  MTF dict + an **order-1 repeat bit** + a dict-hit bit; the MTF index encodes `j−1` since index 0 is
  unreachable), so no resident delta store.
- Output via a monotonic **row write-back cache** (assemble each 256 B row in RAM, commit once =
  1 erase+program ⇒ `rows_amplified=0`). The never-evict journal covers raw source
  read-after-overwrite (peak 903 slots).

## Build / verify
```
cc -O2 -DRC_V3_MAIN -DRC_V3_NVM -I c -o dec c/rc_v3.c c/flash_nvm.c   # arm_cortex_m4.c NOT needed (no-bake)
PYTHONDONTWRITEBYTECODE=1 python3 -B tools/hy_verify.py 10 dec        # 256/256 + amp=0 + inversions=0
arm-none-eabi-gcc -mcpu=cortex-m0plus -mthumb -Os -DRC_V3_ARM -I c -c c/rc_v3.c && arm-none-eabi-size rc_v3.o
sh tools/check_divfree.sh                                            # req#5: hot path divide-free
PYTHONDONTWRITEBYTECODE=1 python3 -B tools/fixture_gate.py dec        # real-firmware generalization (6/6)
PYTHONDONTWRITEBYTECODE=1 python3 -B tools/fuzz_gate.py dec 300       # 300 corrupt patches -> 0 crash/hang
```
Golden encoder/decoder: `sim/ultrapatch/rc_hybrid.py` (the C mirrors it bit-for-bit). The encoder's
`rc_hybrid.PATHE_W` MUST equal the decoder's `SA_W` (default 10).

The matrix gates are meant to be CI-hard gates, not advisory scripts:
- `tools/hy_verify.py` now requires NVM metrics to be present, fails if byte-exact / amplification /
  inversion checks fail, and gates the W=10 corpus total at **4,902,207 B**.
- `tools/a1_golden_rt.py` gates W=10 at **4,901,523 B** for non-self pairs and journal peak
  **903**.
- `sim/ultrapatch/rc_hybrid.py` preflights streamed-delta dictionary caps before emitting a patch, so
  the Python encoder fails early instead of producing a blob the production decoder would reject.

## Optional build: W = 11 (larger LZSS window) — opt-in, slim SRAM margin
The default LZSS window is **W = 10** (`SA_W` / `PATHE_W`), which is what keeps `.bss ≤ 12 KiB`.
Building both sides at **W = 11** saves **−8,731 B** on the corpus (4,902,207 → **4,893,476**, still
256/256, `rows_amplified=0`, `frontier_inversions=0`) and now fits the SRAM cap after the model
reclaim, but with only **224 B margin** (`.bss = 12,064 B`). Keep W = 10 as the default unless the
deployment values those patch bytes more than SRAM headroom:
```
cc -O2 -DRC_V3_MAIN -DRC_V3_NVM -DSA_W=11 -I c -o dec c/rc_v3.c c/flash_nvm.c
PYTHONDONTWRITEBYTECODE=1 python3 -B tools/hy_verify.py 11 dec        # encoder PATHE_W=11 + decoder SA_W=11
```
Keep encoder `PATHE_W` and decoder `SA_W` identical or every pair fails (the wire is unframed).

## Known limitations / robustness notes
- **Flash layer is emulator-shaped.** `orow_commit` forces a row erase by probing
  `flash_write(addr, 0xFF)` until a bit needs setting, and programs byte-by-byte — correct under the
  emulator. On real SAML22 NVMCTRL this must become an explicit **Erase-Row** command + **64 B
  page-buffer program**. The decode logic (byte-exact, `rows_amplified=0`, `frontier_inversions=0`) is
  validated; the physical NVMCTRL driver behind `flash_read`/`flash_write` is the remaining device-port work.
- **`g_psrc` (2048 B) is a hard correctness floor, NOT reclaimable.** The FWD ldr back-scan touches a
  1028-byte span (1024 B ldr reach + the 4-byte bl lookahead that must stay pristine in the ring for
  the encoder-matching derivation), so any power-of-two ring < 2048 aliases. A 1024 B ring — even one
  re-reading the 4 deepest addresses from the journal/flash — reads a clobbered byte and is only
  *corpus-lucky* (measured to mis-read the aliasing slot ~72k times over 8 pairs while still passing
  256/256). Do not shrink it. SRAM margin is now **1,248 B** at W=10 (`.bss` 11,040 / 12,288);
  W=11 consumes most of that and leaves **224 B**.
- **Caps are corpus-peak + margin; over-cap input is REJECTED** (CRC-gated, never silent-wrong — but
  cannot apply). Peaks / caps: `OPC_CAP` 68/80, `DR_KCAP_BL` 180/208, `DR_KCAP_EX`
  106/128, and **`JSLOTS` 903/904** (the tightest cap; a journal-heavier firmware rejects with
  `reason=1`, distinguishable from a corrupt-stream `reason=2`). All caps are `-D` overridable;
  raising journal capacity normally costs ≈ 3 B/slot, though slot 904 is free from alignment. If
  `DR_KCAP_BL` / `DR_KCAP_EX` are retuned in C, pass matching caps to the Python encoder preflight
  (`DR_KCAP_BL`, `DR_KCAP_EX`) so encode-time and decode-time resource checks stay aligned.
- **Coroutine stack high-water is 504 B** of 512 (data-independent call depth); the deepest 8 B are a
  `0xC5` canary checked at decode end, so an overflow rejects rather than silently corrupts.
- `main()` still accepts a `ranges.cfg` arg for tooling compatibility; the no-bake decoder ignores it
  (nothing is stored on-device).

## Development guardrails
- Treat the Python golden and C decoder as one wire contract. Any change to `SA_W`/`PATHE_W`,
  `UG_CTX`, delta dictionary caps, literal models, or field-detection order needs both golden and C
  matrix verification.
- Do not accept a memory win unless `RC_V3_STACKMEAS` still reports ≤ 504 B high-water on representative
  grow and shrink fixtures. A static `.bss` reduction that increases the coroutine frame can trip the
  canary.
- Do not commit decoder-memory changes based only on host `size`; run the ARM object check:
  `arm-none-eabi-gcc -mcpu=cortex-m0plus -mthumb -Os -DRC_V3_ARM -I c -c c/rc_v3.c -o /tmp/rc_v3_arm.o`
  followed by `arm-none-eabi-size /tmp/rc_v3_arm.o`.
- Keep `tools/check_divfree.sh` in the verification set. The two current soft divides are confined to
  initialization; any hot-path divide is a regression on Cortex-M0+.
- Compression experiments that change BL/LDR ambiguity handling, delta value coding, or shared
  dictionaries are wire-format/model changes. Measure them as separate branches and require unchanged
  NVM gates before comparing patch totals.
