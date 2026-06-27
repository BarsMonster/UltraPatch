# A1 Production Result

Final implementation: C encoder and C decoder only.

- Encoder: `c/rc_v3_enc.c` (`c/hy_enc`)
- Decoder: `c/rc_v3.c` (`c/hy_dec` host harness, `RC_V3_ARM` device build)
- NVM emulator for host verification: `c/flash_nvm.c`

The patch stream is a single range-coded A1 blob. There is no side table and no
secondary reference implementation in this tree.

## Requirements

1. At most one erase/program pass per 256 B flash row.
2. Pure in-place apply, no scratch flash region.
3. Decode SRAM at or below 12 KiB.
4. No power-fail rollback; the host can recover by full reflash.

## Measured Gates

| gate | result |
|---|---|
| C encoder + C decoder, 16x16 image matrix | 256/256 byte-exact |
| NVM row write amplification | 0 amplified rows, max 1 erase/row |
| Sequential row frontier | 0 inversions |
| ARM object at `SA_W=10` | text 5,999 B, data 0 B, bss 12,000 B (<= 12 KiB cap, 288 B margin) |
| ARM divide check | 0 hardware divide instructions; 1 soft-divide call in init |
| Coroutine stack high-water | 504 B of 576 B (72 B cushion; canary-guarded) |

Patch-size metrics:

- W=10 full 16x16 corpus total: **4,692,289 B**.
- W=10 non-self corpus total: **4,691,717 B**.
- Real one-face 360-byte firmware update:
  - `v0_base -> v1_one_face`: **882 B**
  - `v1_one_face -> v0_base`: **594 B**

## Architecture

A1 is no-bake: the decoder never rewrites source rows before the output frontier.
`[A]` copy reads raw source bytes, and corrections are applied at the monotonic
output frontier. Relocation field positions are derived instead of shipped:

- `bl` positions are derived from the local Thumb halfword pattern.
- `ldr` positions are derived per op: a copied 4-byte field is an `ldr` target
  only when an `ldr` literal instruction in the same op's copy range targets it.
- Delta values are pulled inline from the single range stream using adaptive MTF
  dictionaries and repeat/hit models.

Entropy coding: content literals use five bit-trees selected by the previous
literal byte's range (`LIT0_SEL`: the top three quartiles keep the `>>6` split
and the literal-dense bottom quartile is subdivided into two octiles), each
parity-seeded from the from-image histogram;
per-op geometry (diff/extra length, source skip) and the preserve/correction
counts and gaps use dedicated adaptive Golomb models rather than a fixed raw code.
Match distances carry an adaptive `rep0` reuse flag: when a match repeats the
immediately-previous match distance the value is omitted and the decoder reuses
the last distance (the flag's prior is biased toward "fresh" so it stays nearly
free on small patches).
The host encoder's LZ parse runs a price-feedback loop: it re-parses against bit
prices measured from the real adaptive models and keeps the result only when its
exact modeled cost drops. These are encoder/decoder-symmetric or encoder-only and
leave the no-bake apply, NVM write discipline, and divide-free property unchanged.

Output is staged through a 256 B row write-back cache. Rows whose final bytes
match the existing flash row are not erased or programmed. The preserve journal
keeps pristine source bytes that would otherwise be lost after an in-place
overwrite.

## Build And Check

```sh
cd /ai_sw/v3/nvm/hybrid12k/c
make
make check
make check-arm
make check-corpus
```

`make check` performs a C-only real-fixture smoke test in both directions and
prints the real one-face blob sizes. Expected blob sizes are `882` and `594`
bytes.

`make check-arm` verifies the Cortex-M0+ object resource gate and divide policy.
`make check-corpus` runs the local 16x16 image matrix and prints corpus totals
plus the real one-face update/revert sizes.

Set `A1_ENC_STATS=1` when running `hy_enc` to print one encoder study line with
op, literal, correction, preserve, and derived-field counts for that patch.

Manual one-direction check:

```sh
./hy_enc ../fixtures/v0_base ../fixtures/v1_one_face /tmp/grow.blob 10
cp ../fixtures/v0_base/watch.bin /tmp/mem.bin
./hy_dec /tmp/mem.bin /tmp/grow.blob 1
cmp /tmp/mem.bin ../fixtures/v1_one_face/watch.bin
```

ARM object check:

```sh
arm-none-eabi-gcc -mcpu=cortex-m0plus -mthumb -Os -DRC_V3_ARM -I c -c c/rc_v3.c -o /tmp/rc_v3_arm.o
arm-none-eabi-size /tmp/rc_v3_arm.o
```

The encoder `W` argument must match decoder `SA_W`. The production default is
`W=10` / `SA_W=10`. `W=11` saves bytes but leaves only a narrow SRAM margin, so
keep W=10 unless the deployment explicitly accepts that trade.

## Caps

The decoder rejects over-cap inputs rather than silently producing wrong output.
Current production caps and measured peaks:

- Per-op correction entries: `OPC_CAP=80`, measured peak 68.
- BL delta dictionary: `DR_KCAP_BL=208`, measured peak 180.
- EX/LDR delta dictionary: `DR_KCAP_EX=128`, measured peak 106.
- Preserve journal: `JSLOTS=904`, measured peak 903.

Raising caps is a wire-compatible decoder resource change only when the encoder
uses the same build-time limits.
