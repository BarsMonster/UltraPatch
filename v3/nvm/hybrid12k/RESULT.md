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
| ARM object at `SA_W=10` | text 5,003 B, data 0 B, bss 10,736 B (<= 12 KiB cap, 1,552 B margin) |
| ARM divide check | 0 hardware divide instructions; 1 soft-divide call in init |
| Coroutine stack high-water | 456 B of 576 B (120 B cushion; canary-guarded) |

Patch-size metrics:

- W=10 full 16x16 corpus total: **4,595,773 B**.
- Real one-face 360-byte firmware update:
  - `v0_base -> v1_one_face`: **873 B**
  - `v1_one_face -> v0_base`: **582 B**

## Architecture

A1 is no-bake: the decoder never rewrites source rows before the output frontier.
`[A]` copy reads raw source bytes, and corrections are applied at the monotonic
output frontier. Relocation field positions are derived instead of shipped:

- `bl` positions are derived from the local Thumb halfword pattern.
- `ldr` positions are derived per op: a copied 4-byte field is an `ldr` target
  only when an `ldr` literal instruction in the same op's copy range targets it.
- Delta values are pulled inline from the single range stream using adaptive MTF
  dictionaries and repeat/hit models.

Entropy coding: content literals use five bit-trees selected from the previous
literal byte (`LIT0_SEL`: a five-way mapping derived by minimising the
conditional entropy of the literal distribution over the firmware corpus; the
buckets are non-contiguous, grouping the dominant zero-diff byte with the low
range and isolating the most common high byte), each parity-seeded from the
from-image histogram. Per-op geometry (diff/extra length, source skip) uses
dedicated adaptive Golomb models rather than a fixed raw code; the preserve and
correction counts/gaps share one set of Golomb models (their distributions
coincide, which also frees decoder RAM). The relocation-delta MTF dictionary
indices are coded with a lean adaptive UNARY model (`IdxUnary`, five per-position
priors seeded toward the most-recent entry): the index is ~54% zero, so unary
beats the former Golomb here while dropping the per-stream gamma model state.
Match distances carry an adaptive `rep0` reuse flag, contexted order-1 on the
previous reuse outcome (reuse runs cluster): when a match repeats the
immediately-previous match distance the value is omitted and the decoder reuses
the last distance (the flag's prior is biased toward "fresh" so it stays nearly
free on small patches).
Match lengths carry a structural prior: the LZ minimum match length is 3, so the
length-1 value is always >= 2 and the first unary-prefix bit of the match-length
Golomb is always "continue"; seeding it cheap makes that bit near-free from the
first symbol (a format invariant, not a corpus fit).

The host encoder's LZ parse runs a price-feedback loop: it re-parses against bit
prices measured from the real adaptive models and keeps the result only when the
shipped patch actually shrinks. Two refinements make the parse select on the real
wire: (1) it prices tag0 literals under the SAME order-1 previous-byte context the
wire codes them with (carrying the prevlit through the forward DP), instead of an
order-0 average; (2) the acceptance gate is the EXACT full-body flushed byte count
(the real geometry/preserve/delta streams emitted interleaved with the LZ tokens
and finalized with the same optimal flush), not a token-stream-only modeled cost.
The forward DP is also flag-history-aware: the token flag is an order-2 model
(four contexts on the previous two token kinds), so the parse carries the flag
history as DP state and prices each span/match transition under its real context
instead of a washed-out scalar average. The parse is `rep0`-aware so it
deliberately prices and exploits last-distance reuse, and it explicitly probes the
reuse-distance match the Pareto candidate set drops (extending the previous
distance for one cheap flag bit). The relocation-field detector also admits
slightly smaller delta-bearing blocks (it pays off across the corpus). The wire
omits the always-zero leading range-coder cache byte (a structural LZMA invariant),
saving one byte per patch. These parse refinements are encoder-only (the wire
format is unchanged; they only choose a cheaper legal parse), so the no-bake apply,
NVM write discipline, and divide-free property are untouched. On the decoder side
the Rice and Gamma readers now share one adaptive unary-prefix and one mantissa
helper (emitted once instead of duplicated), and the Thumb-BL de-relocation
collapses into a single fused round-trip; both are bit-exact and shrink the object.
The per-op de-relocation field reader peeks the four field bytes once (reused for
the Thumb-BL pattern test, the BL/EX pack, and the ldr-derive ring record) instead
of re-reading them two or three times, and the coroutine layer is expressed as one
shared saved-stack-pointer fiber core (host x86 and the ARM device differ only in
the swap primitive — the dead ucontext host fallback was removed since the only two
real targets are the x86-64 host and the ARM device); both are bit-exact decoder
simplifications. The bit-tree adaptation `rate` (lit0=5, lit1=4, dval=4) is a
compile-time constant per call site, so it is no longer stored per-tree (the
`BitTree`/`BTE` struct drops its `rate` byte, −16 B `.bss`); and the plaintext
header omits the `fp_end` seed on FWD/shrink/equal patches where it is redundant
with `CRC32(to)` (load-bearing only for the grow direction). The `rep0`
last-distance reuse prior is seeded at 1/4 (was 1/8): a paired min-over-pairs corpus
sweep places the optimum that does not regress the real one-face product patch at
1/4 (3/8 helps the corpus aggregate more but costs the one-face update +1/+1 B).
The host parse adds a `kd` (rice-parameter) anneal probe: `fit_k_tokens` minimises
the rice codelength of the raw distances, but the shipped cost is the adaptive
`M_gd` encode seeded at `k`, so the encoder probes `kd` outward in each direction
under the exact full-body byte gate and keeps any strict improvement (encoder-only;
the wire is unchanged). The bsdiff seed pass weights matched bytes 3:1 (was 2:1)
when growing each op's diff region over the following literal `extra` run, so cheap
near-zero diff bytes are preferred to fresh literals and the downstream split feeds
on better material; and the priced LZ DP carries two arrival variants per state (the
cheapest arrival plus the cheapest arrival holding a *different* rep distance) so a
later `rep0` reuse can be exploited — both encoder-only, the wire unchanged, and
together they cut the corpus total by ~1.0%. On the decoder side three further
bit-exact simplifications shrink the object: the per-op correction lookup is a
linear scan over the count-bounded array (the offsets are unique, so it returns the
same value the binary search did), the literal-patch cursor folds its FWD/grow
first-read and advance onto one `base`/`step` helper, and the de-relocation field
dispatch collapses its two unrolled 4-byte loops into one. Further decoder-only,
bit-exact cleanup keeps the push FIFO as the single-byte mailbox it really is,
uses the journal page-table sentinel as the journal count, stores the resident
output row by base address plus a sentinel, reuses the relocation MTF front entry
as the repeat-last value, and stores byte-tree probabilities as 12-bit packed
logical `p[1..255]` nodes. The apply state no longer duplicates the global
from/to sizes or direction flag, and the journal peak is reported from the
monotonic page-table sentinel instead of a second counter.

Output is staged through a 256 B row write-back cache. Rows whose final bytes
match the existing flash row are not erased or programmed. The preserve journal
keeps pristine source bytes that would otherwise be lost after an in-place
overwrite.

## Build And Check

```sh
cd /ai_sw/v3/nvm/hybrid12k/c
make
make gate          # builds + runs all three gates below, one consolidated summary (~20s)
```

`make gate` is the single full-gate command: it builds and runs `check`,
`check-arm`, and `check-corpus`, then prints one consolidated summary of every
tracked metric (ARM `.text`/`.data`/`.bss` + divide policy, corpus full total,
the real one-face grow/revert, matrix round-trips, NVM write-safety, and
journal peak). It exits nonzero if any gate fails and dumps the raw blocks so the
offending metric is visible. The three gates can still be run individually:

```sh
make check         # one-face smoke both directions + corrupt/truncated/CRC fuzz
make check-arm     # Cortex-M0+ object resource gate + divide policy
make check-corpus  # 16x16 image matrix (parallel) + corpus total + one-face
```

`make check` performs a C-only real-fixture smoke test in both directions and
prints the real one-face blob sizes. Expected blob sizes are `873` and `582`
bytes.

`make check-arm` verifies the Cortex-M0+ object resource gate and divide policy.
`make check-corpus` runs the local 16x16 image matrix (parallelized across cores
via `check_corpus.sh`; override with `JOBS=N`) and prints the corpus total plus the
real one-face update/revert sizes.

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
`W=10` / `SA_W=10`. With the current packed byte-tree models, an `SA_W=11` build
now fits the 12 KiB SRAM cap at text 5,003 B, data 0 B, bss 11,760 B (528 B
margin) and improves the corpus total to 4,580,558 B with the real one-face
update unchanged at 873/582 B. The 256-pair patch-size split for W=11 vs W=10 is
136 better / 15 worse / 105 equal. Production stays at W=10 to keep the larger
1,552 B SRAM margin; W=11 is a product tradeoff, not a correctness requirement.

## Caps

The decoder rejects over-cap inputs rather than silently producing wrong output.
Current production caps and measured peaks:

- Per-op correction entries: `OPC_CAP=80`, measured peak 68.
- BL delta dictionary: `DR_KCAP_BL=208`, measured peak 180.
- EX/LDR delta dictionary: `DR_KCAP_EX=128`, measured peak 106.
- Preserve journal: `JSLOTS=904`, measured peak 625.

Raising caps is a wire-compatible decoder resource change only when the encoder
uses the same build-time limits.
