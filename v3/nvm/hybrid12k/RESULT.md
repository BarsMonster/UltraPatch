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
| ARM object at `SA_W=10` | text 5,772 B, data 0 B, bss 11,024 B |
| ARM divide check | 0 hardware divide instructions; 2 soft-divide calls in init |
| Coroutine stack high-water | 504 B of 512 B |

Patch-size metrics:

- W=10 full 16x16 corpus total: **4,866,646 B**.
- W=10 non-self corpus total: **4,865,962 B**.
- Real one-face 360-byte firmware update:
  - `v0_base -> v1_one_face`: **901 B**
  - `v1_one_face -> v0_base`: **615 B**

## Architecture

A1 is no-bake: the decoder never rewrites source rows before the output frontier.
`[A]` copy reads raw source bytes, and corrections are applied at the monotonic
output frontier. Relocation field positions are derived instead of shipped:

- `bl` positions are derived from the local Thumb halfword pattern.
- `ldr` positions are derived per op: a copied 4-byte field is an `ldr` target
  only when an `ldr` literal instruction in the same op's copy range targets it.
- Delta values are pulled inline from the single range stream using adaptive MTF
  dictionaries and repeat/hit models.

Output is staged through a 256 B row write-back cache. The preserve journal keeps
pristine source bytes that would otherwise be lost after an in-place overwrite.

## Build And Check

```sh
cd /ai_sw/v3/nvm/hybrid12k/c
make
make check
```

`make check` performs a C-only real-fixture smoke test in both directions and
prints the real one-face blob sizes. Expected blob sizes are `901` and `615`
bytes.

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
