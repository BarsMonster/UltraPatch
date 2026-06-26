# v3 NVM — updated requirements and what they force

## New requirements (this round)
1. **No power-fail recovery** — on power fail, the host does a **full reflash**. ⇒ the decoder
   needs **no atomicity / no rollback**; it may freely destroy source as it goes.
2. **No write amplification** — **every flash page/row is erased+programmed 0 or 1 times, never
   more.** (Hard gate; emulator now reports `nvm_rows_amplified` which MUST be 0.)
3. **No scratch pads in flash** — pure in-place, no extra flash region (already assumed).

## Impact: the previous winner and baking are now INVALID
- The **row write-back cache** (Options 2 & 4) committed some rows **twice** (bake touches a reloc
  row, apply later overwrites it) → mean wear **1.25–1.66×**, max 2.33×. That now **violates
  requirement 2** (any row written twice fails).
- **Physical baking is fundamentally incompatible with ≤1 write/page:** baking writes
  de-relocated values into source rows; apply then overwrites those same in-place rows → **≥2
  writes** for every reloc-bearing row. There is **no way to physically bake and write each page
  once** in a single in-place region.
- **Logical bake on read** (Option 1) was already infeasible (reloc map is image-scale, 78–145 KB).

So neither persistence strategy for baking survives. **Baking must be removed.**

## What's forced: apply relocations at the OUTPUT frontier (to-order), no baking
The only architecture that satisfies {≤1 write/page, in-place, no scratch, ≤8 KiB}:
- **Drop the `[B]` from-order bake entirely.** No disassembler on the decoder, no source writes.
- The `[A]` copy reads **raw** `from[fp]` (non-monotonic `fp` is fine — raw reads need no delta).
- **Relocations are applied at the to-position**, in apply/to-order, as the monotonic output
  frontier reaches them — exactly Path F's `[C]` corrections mechanism, **extended from the 455
  exceptions to ALL ~2,900–9,768 relocation fields per patch**. Consumed O(window) at the frontier.
- **Output via a row cache** (assemble each 256 B row in RAM, commit once = 1 erase+program) ⇒
  **1 write/row, `nvm_rows_amplified=0`**. Journal stays (covers raw source read-after-overwrite).
- RAM: **no bake disassembler** (the `LitWin`/bstream state, ~2 KB, disappears) + output row
  buffer (+256 B) + journal + `[A]` + models → likely **fits, possibly smaller than Path F**.

## The open question = patch-size cost (must measure)
Removing baking means the decoder can no longer **derive** relocation positions (the disassembler
did that for free). They must be **shipped in to-order**. The project previously measured a naive
to-order `[B]` (absolute `der`-gaps) at **+6.09%** ("Step C", rejected for size). BUT Path F's
`[C]` uses **op-local offsets** (cheap, like the per-op `[P]`/`[C]` that cost ~1% for 1,358
entries), which may be **far cheaper** than absolute gaps. The real cost for 9,768 entries with
op-local encoding is unknown — **it could be anywhere from ~+0.5% to +6%**, and it is the number
that decides whether the new requirements are nearly free or a real size hit.

## The decisive experiment
Build the **no-bake, to-order-reloc** decoder (drop `[B]`/bake; route all relocations through an
extended `[C]` in to-order op-local form; add the output row cache) and measure, under the shared
NVM emulator:
1. **byte-exact 256/256** in-place, byte-by-byte, **AND `nvm_rows_amplified=0`** (≤1 write/page).
2. **≤8192 B SRAM** (ARM) — expect ≤ Path F (no disassembler).
3. **patch-size Δ** vs Path F's +1.84% (the cost of shipping reloc positions to-order).
4. wear = floor (1 erase/output row); 0 extra flash; divide-free.

If the size cost is small, this is the clean on-device solution. If it's ~+6%, that's the
measured price of the no-write-amplification constraint, and we decide knowingly.
