# Effective Compression vs. Small-Footprint Compressors

This measures ultrapatch's **inherent compression** — what it achieves as a pure
compressor, with all delta benefit removed — and places it against well-known
compact compressors, ranked by ratio and annotated with the memory each needs to
**decompress**. Decode memory is the axis that matters for the Cortex-M0+ target
(12 KiB SRAM hard cap), so it shares the table with the ratio.

## Method

- **Corpus:** the 16 native/home images (`test-bench/images/img_*/watch.bin`),
  99,740–215,812 B each, 2,555,032 B total.
- **"From-0" encode:** each image is patched from a **0-size source**, so no
  `copy` op can draw from a prior image — the whole target is encoded from
  nothing. This is ultrapatch's *worst case* and isolates the residual
  "new content" cost; it makes the delta tool directly comparable to a plain
  compressor. Verified byte-exact round-trip (empty → target).
- **Per-file, no cross-file dictionary** for every tool (the fair analog of
  per-image blobs). `xz` uses `--format=raw` to strip its container; ultrapatch
  blobs carry ~12–16 B of envelope (2×CRC32 + sizes), negligible at ~100 KB/img.
- **Tool versions:** gzip 1.14, bzip2 1.0.8, xz (XZ Utils) 5.8.3,
  zstd 1.5.7, heatshrink 0.4.1. ultrapatch built at `WINDOW_LOG` ∈ {10,12,14}.

## Unified table (native/home, from-0, original = 2,555,032 B)

Ranked best→worst by ratio. "Decode RAM" is approximate and lists the dominant
term (sliding window / dictionary), which is the tunable, comparable quantity.

| Compressor / setting        | Decode RAM (approx)        | Compressed (B) | % of orig | Ratio  |
| --------------------------- | -------------------------- | -------------: | --------: | -----: |
| xz -9e (LZMA2)              | 64 MiB dict                |      1,614,600 |   63.19 % | 1.582× |
| ultrapatch `WINDOW_LOG=14`  | ~26 KiB .bss ‡ (projected) |      1,630,081 |   63.80 % | 1.567× |
| ultrapatch `WINDOW_LOG=12`  | ~13 KiB .bss ‡ (projected) |      1,678,149 |   65.68 % | 1.523× |
| xz raw, dict=4 KiB (LZMA2)  | ~20 KiB (4 KiB dict+state) |      1,695,218 |   66.35 % | 1.507× |
| **ultrapatch `WINDOW_LOG=10` (default)** | **~10 KiB .bss ‡ (measured)** | **1,708,386** | **66.86 %** | **1.496×** |
| zstd -19 (default)          | ~window≤file + DCtx †      |      1,756,463 |   68.75 % | 1.455× |
| gzip -9 (DEFLATE)           | ~32 KiB window (+tables)   |      1,814,589 |   71.02 % | 1.408× |
| bzip2 -9 (BWT)              | ~2.5 MiB (block)           |      1,860,584 |   72.82 % | 1.373× |
| zstd -19, wlog=12           | 4 KiB window + DCtx †      |      1,871,310 |   73.24 % | 1.365× |
| zstd -19, wlog=10           | 1 KiB window + DCtx †      |      2,017,834 |   78.97 % | 1.266× |
| heatshrink -w13 -l12 (best) | ~8 KiB (window)            |      2,365,859 |   92.60 % | 1.080× |
| heatshrink -w10 -l9 (matched)| ~1 KiB (window)           |      2,377,487 |   93.05 % | 1.075× |
| heatshrink -w8 -l7          | ~256 B (window)            |      2,431,603 |   95.17 % | 1.051× |

‡ ultrapatch figures are total `.bss`, no heap, no divide. `WINDOW_LOG=10` is
measured by `make check-arm` (10,300 B); 12/14 are projected (+ring = +3/+15 KiB)
and **exceed the 12 KiB SRAM gate**, so they are measurement-only, not shippable.

† zstd's reference decoder allocates a fixed `DCtx` workspace of tens of KiB
regardless of window, so it is not a small-footprint *decoder* even at `wlog=10`.

## Findings

**At equal memory, ultrapatch wins decisively.**
- **Same 1 KiB window vs zstd:** 1,708,386 vs 2,017,834 — ultrapatch is **15.3 %
  smaller**. (LZMA2 cannot reach 1 KiB; its dict floor is 4 KiB.)
- **Same 4 KiB window:** ultrapatch `WINDOW_LOG=12` (1,678,149) beats zstd-wlog12
  by 10.3 % **and** beats 4 KiB-dict LZMA (1,695,218) by 1.0 % — at matched tiny
  footprint it edges out LZMA itself.
- **1 KiB ultrapatch beats 32 KiB gzip -9** by 5.9 % and **beats ~8 MiB default
  zstd -19** by 2.7 %.
- Only full-fat LZMA (`xz -9e`, 64 MiB dict) beats it — by 5.5 %, at 64,000× the
  RAM.

**heatshrink shows what the range coder is worth.** heatshrink is the canonical
tiny-RAM embedded compressor, and its decoder is the smallest here (~2^w window +
a few dozen bytes of state). But it is **pure LZSS with no entropy coding**, so on
ARM firmware it barely compresses:

| 1 KiB-window LZSS codec | Ratio  | Saved  | Decode RAM        |
| ----------------------- | -----: | -----: | ----------------- |
| heatshrink -w10         | 1.075× |  6.9 % | ~1.1 KiB          |
| **ultrapatch W10**      | 1.496× | 33.1 % | ~10 KiB           |

At the identical window ultrapatch's output is **28.1 % smaller**. The only
structural difference is the LZMA-class adaptive binary **range coder** feeding on
the LZSS tokens; on high-entropy firmware the match-finder alone finds little, so
the entropy stage turns 7 % savings into 33 %. That is where ultrapatch's ~10 KiB
budget goes — the ~6 KiB of context models, not the window — and it is what lets a
1 KiB-window codec sit in LZMA's ratio class at 1/6400th of LZMA's dictionary RAM.

## `WINDOW_LOG` sensitivity (ultrapatch only)

Larger LZSS windows compress the from-0 case better, but the window is
`2^WINDOW_LOG` bytes of decoder `.bss`, and `.bss` is already 10,300 B at
`WINDOW_LOG=10` against the 12,288 B cap:

| `WINDOW_LOG` | Ring   | ≈ Decode `.bss` | vs 12 KiB cap | Compressed | Ratio  |
| -----------: | -----: | --------------: | ------------- | ---------: | -----: |
| 10 (default) | 1 KiB  | ~10.3 KiB       | fits          |  1,708,386 | 1.496× |
| 12           | 4 KiB  | ~13.4 KiB       | busts (+1 KiB)|  1,678,149 | 1.523× |
| 14           | 16 KiB | ~25.7 KiB       | 2× over       |  1,630,081 | 1.567× |

So the +1.8 % (W12) / +4.6 % (W14) it would save costs 3–15 KiB of SRAM the part
does not have — which is why `WINDOW_LOG=10` is the production default. The LZSS
ring holds *output* history, so in the real delta use-case (most bytes copied from
the source image, not the ring) the window matters even less; these from-0 numbers
are the window's best case.

## Caveat: this is the worst case for a delta tool

From-0 removes all delta benefit. In its real job — patching between firmware
versions — most target bytes come from `copy` ops against the existing image, so
shipped patches are far smaller than any full-image number here, and none of the
compared compressors can do that at all. This study isolates the compression
floor, not the product metric (see `make gate`).

## Reproduction

```sh
# ultrapatch at a given window (single shared knob moves encoder+decoder):
make -B WIRE_CONFIG_FLAGS='-DCORTEX_M0 -DWINDOW_LOG=10'
cp ultrapatch up10
: > empty.bin
for f in test-bench/images/img_*/watch.bin; do ./up10 empty.bin "$f" /tmp/b.blob; \
    stat -c%s /tmp/b.blob; done | paste -sd+ | bc

# peers (per file, minimal container):
gzip -9 -c < f            # DEFLATE, 32 KiB
bzip2 -9 -c < f           # BWT
xz -9e -c < f             # LZMA2, 64 MiB dict
xz --format=raw --lzma2=preset=9e,dict=4KiB -c < f   # small-dict LZMA
zstd -19 -c --no-check --zstd=wlog=10 < f             # window-matched zstd
heatshrink -e -w 10 -l 9 < f                          # tiny-RAM LZSS
```
