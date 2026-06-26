# Delta-tool benchmark — Sensor Watch Pro (baseline → +counter_face)

from = baseline/watch.bin (113124 B), to = with_counter_face/watch.bin (113484 B)
Net code change ≈ 360 B new + ~3545 relocation edit-sites. Every patch below verified
to reconstruct `to` byte-exact via its own apply step.

References (no delta — full image): raw 113484 · zstd-19 77533 · xz-9e 71616

| tool / config                         | patch B | vs raw | embedded apply on SAM L22 (32 KB RAM, from-image in flash) |
|---------------------------------------|--------:|-------:|-----------------------------------------------------------|
| detools arm-cortex-m4 + lzma          | **1335**| 0.7%   | smallest, but LZMA decoder needs big dict RAM → tight/no  |
| detools hdiffpatch + lzma             | 2341    | 1.0%   | LZMA RAM caveat                                            |
| bsdiff4 (classic, bz2)                | 2381    | 1.1%   | NO — needs whole from+to in RAM (~226 KB) + bz2           |
| detools bsdiff + lzma                 | 2439    | 1.1%   | LZMA RAM caveat; diff streams, decomp doesn't             |
| detools hdiffpatch + heatshrink       | 3057    | 1.3%   | YES — tiny decompressor                                    |
| detools bsdiff + lz4                  | 3094    | 1.4%   | YES — small lz4 decoder                                    |
| **detools arm-cortex-m4 + heatshrink**| **3189**| 1.4%   | **YES — reloc-aware + KB-scale RAM, streams from flash**  |
| zstd --patch-from                     | 3829    | 1.7%   | NO — 111 KB window must be in RAM                          |
| detools bsdiff + heatshrink           | 4134    | 1.8%   | YES — tiny decompressor                                    |
| detools bsdiff + crle                 | 8613    | 7.6%   | YES — trivial RLE decoder                                  |
| detools bsdiff + none                 | 113521  | 100%   | YES (no decomp) but no point                              |

## Two different "winners"
- **Smallest on the wire:** detools `arm-cortex-m4` + lzma = 1335 B. The data-format transform
  un-relocates the shifted absolute addresses (the churn we measured), then lzma packs the
  residual. ~85× smaller than the full image; ~3× smaller than generic zstd delta.
- **Best actually-applyable on this watch:** detools `arm-cortex-m4` + **heatshrink** = 3189 B.
  detools' "sequential" patch reads the from-image directly from flash via callbacks and writes
  the to-image sequentially; heatshrink's decompressor is ~1 KB code + a small (256 B default)
  window. Fits the 32 KB-RAM SAM L22 comfortably. Relocation-aware AND embeddable.

## Key embedded caveat
Smaller patch ≠ easier to apply. The three smallest entries lean on **LZMA**, whose decoder needs
a multi-KB–tens-of-KB dictionary buffer — uncomfortable on 32 KB RAM. zstd --patch-from and
classic bsdiff4 are smaller-ish but need 111 KB / ~226 KB RAM respectively → not applyable here.
The relevant trade is **patch size vs decompressor RAM**, and heatshrink/lz4/crle are the
on-device-friendly compressors. detools is the only tool here that is both relocation-aware and
designed to apply on the MCU streaming from flash.
