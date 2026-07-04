# Foreign-Firmware Validation Study (2026-07-02)

Every prior A1 validation ran on one firmware lineage (Sensor Watch, SAML22,
Cortex-M0+). This study exercises the encoder + decoder on a second, unrelated
Cortex-M0+ lineage to convert the "degrades gracefully on foreign firmware"
design claim into evidence, and to identify which decoder resource cap binds
first outside the home corpus.

## Method

Subject: CircuitPython for `feather_m0_express` (ATSAMD21, Cortex-M0+,
Thumb-1), 18 official release binaries — raw `.bin` for 2.2.0–3.0.3 and
UF2-unpacked (app base 0x2000) for 10.0.x–10.2.1, fetched from
`https://adafruit-circuit-python.s3.amazonaws.com/bin/feather_m0_express/en_US/`
(MIT-licensed builds; images are 180–251 KB, versus the 113 KB home corpus).

16 sequential upgrade pairs plus one cross-major pair (3.0.3 -> 10.0.0), each
encoded and applied in BOTH directions (34 pair-directions total). Per
direction: `hy_enc from to blob` (self-verification included), then
`hy_dec` apply on the host NVM emulator + byte-exact `cmp`, with the NVM
write-safety gate active.

## Results

- 14/34 round-trip byte-exactly with clean NVM safety (0 amplified rows, 0
  frontier inversions). Blobs: 46 B–3.9 KB on ~250 KB images. Patch-level
  releases that only touch the version string cost ~50 B; real patch-level
  churn (3.0.0<->3.0.1) costs ~3.9 KB. Journal peak observed: 180 of 904 slots.
- 20/34 are refused BY THE ENCODER (`no feasible plan: every config exceeds a
  decoder resource cap`). No crash, no hang, no wrong output, no decoder-side
  reject anywhere in the study — a refused pair never produces a blob at all.
- 0 device-side failures: the decoder never saw an infeasible patch because
  hy_enc's cap-feasibility mirror + self-verification front-run it.

## Which cap binds

Per-plan-variant metrics on the refused pairs split them into two classes:

1. **Whole-image churn** (e.g. 10.0.0->10.0.1, 3.0.1->3.0.2): a full relink
   shifted essentially every address; the op plan needs 103K–204K never-evict
   journal slots (`JSLOTS` is 904) under EVERY bsdiff alignment. This class is
   fundamentally outside the no-bake in-place design at a 12 KiB SRAM budget —
   at ~600 KB of journal there is no cap raise to discuss. Refusal is the
   designed, correct outcome; ship such updates as full images.
2. **Moderate churn** (2.3.0->2.3.1): needs ~1,288 slots, modestly over the
   904 cap. Raising `JSLOTS` to ~1,344 (+1,320 B `.bss`, margin 2,000 -> 680
   under the 12 KiB cap) would serve this class. NOT DONE — the home product
   peaks at 478 slots, and per policy caps are not resized without a product
   requirement. Recorded here as the known knob if cross-family support ever
   becomes a requirement.

Direction asymmetry is real and expected: 2.3.1->2.3.0 round-trips at 200 B
while 2.3.0->2.3.1 is refused — the from/to roles determine the read-after-
overwrite (journal) structure.

## Fix landed from this study

`plan_encode` used to `die()` when the legacy plan config 0 was infeasible,
never trying configs 1–5 (whose different bsdiff alignments have different
preserve/correction budgets). Config 0 is always feasible on the home corpus,
so this could only fire on foreign firmware. The sweep now soft-skips ANY
infeasible config and fails, with a clear message, only when every config
exceeds a cap. Wire-neutral (golden unchanged).

## Addendum: graceful degradation landed (same day)

Owner decision after the initial results: refusal is not acceptable for
serviceable pairs — degrade compression instead. Two encoder-side degradations
landed (wire format and decoder logic unchanged; home-corpus golden byte-identical):

1. **Journal budget** (`JSLOTS` raised 904 -> 1024, +360 B `.bss`): when the
   ideal plan needs more preserves than the budget, the encoder protects the
   first-budget preserves in apply order and converts every read of a later
   overwritten position into plain extra bytes (op split, source skipped via
   `adj`). Every remaining overwritten read still goes through the journal, so
   the wire stays independent of the deployment's NVM row size.
2. **Per-op corrections** (`OPC_CAP` 80): an op needing more corrections is
   split at its median-correction offset, iterated to a fixpoint (correction
   sets shift with op boundaries because field detection is per-op).

Result: **34/34 pair-directions round-trip** (was 14/34), zero refusals, NVM
safety clean, journal exactly at the 1024 budget on degraded pairs. The
moderate-churn showcase 2.3.0->2.3.1 (previously refused at 1,288 needed
slots) ships at 408 B. Whole-relink pairs ship working 23-175 KB blobs on
~250 KB images — degraded, as intended, but never refused; the cross-major
3.0.3->10.0.0 (effectively unrelated programs) ships 175 KB. A pair can now be
refused only if every plan variant still exceeds a cap after both
degradations (none observed in this study).

## Addendum 2: direction choice + row-window oracle landed (2026-07-03)

Two wire-affecting features landed after prototype measurement, closing most of
the remaining degradation cost:

1. **Apply direction is an encoder choice.** The direction-from-size rule is
   optimal for the home corpus (measured: no home pair prefers the flip) but
   catastrophic for equal-size images with internal insertions. The encoder now
   sweeps both directions and signals the unnatural one via an overlong
   size-delta uLEB (+1 byte, only when flipping wins). Insertion-shift pairs
   collapsed: 10.0.0->10.0.1 145,057 -> 1,722 B.
2. **Row-window oracle, OUTROW_DEPTH=2.** The decoder keeps its last two
   output rows uncommitted; their OLD flash content is still physically
   readable, so the encoder treats reads within that window as journal-free
   pristine reads (no [P] events, no conversion). Mixed-small-lag pairs
   collapsed: 10.0.1<->10.0.2 75-76 KB -> 2.2-2.4 KB; 3.0.1<->3.0.2 64-65 KB ->
   ~6.2 KB. Costs +256 B decoder .bss and makes OUTROW x OUTROW_DEPTH an
   encoding-affecting build contract (monotone-compatible toward larger
   decoder windows; see device-integration.md).

Home corpus impact of the combined landing: 36 pairs better / 1 worse (+1 B) /
219 equal, full total 4,199,788 -> 4,199,637 (-151 B), one-face unchanged
589/305 — the window's [P]-event elimination outweighs the direction plumbing.
All 34 foreign pair-directions still round-trip; the residual heavily-degraded
pairs (2.2.x, cross-major) are genuine bidirectional whole-image churn, out of
reach of any direction or window (measured lags in the tens of KB).
Depth benchmark for the record: D=1 -14.5% / D=2 -20.5% / D=4 -20.9% / D=8
-21.2% on degraded-blob bytes; D=8 exceeds the 12 KiB SRAM cap; D=2 is the
knee and was chosen.

### Final verified D=1 vs D=2 (both with the landed v2 encoding, same baseline)

| metric                          | baseline  | v2 D=1            | v2 D=2 (landed)   |
|---------------------------------|-----------|-------------------|-------------------|
| home corpus total               | 4,199,788 | 4,199,732 (-56)   | 4,199,637 (-151)  |
| home split better/worse/equal   | —         | 14 / 1 / 241      | 36 / 1 / 219      |
| one-face grow/revert            | 589/305   | 589/305           | 589/305           |
| home journal peak / total slots | 478 / 5,012 | 478 / 4,593     | 453 / 4,023       |
| foreign total (34 pair-dirs)    | 1,744,004 (degradation-only) | 1,327,495 | 1,234,665 |
| ARM .text / .bss                | 6184 / 10648 | 6244 / 10648   | 6324 / 10904      |

Why D=2 is the knee and not an artifact: D=1's window is SAME-ROW only — a
lag-δ read is covered just when `(t mod ROW) >= δ`, so it is partial even for
tiny lags and zero past 256 B. D=2 is the smallest depth with an UNCONDITIONAL
previous-row guarantee: every sub-row lag is fully covered regardless of row
phase; each further depth merely extends a partial band by another 256 B.
Home journal lags are function-scale (>=256 B, from code displaced by whole
inserted/removed functions between distant releases — the measured peak pair
sheds nothing at D=1 and 25 slots at D=2), while the foreign small-lag mass is
relinker micro-drift (<256 B) that D=2 covers completely. A deployment whose
firmware drifts in the 512 B–2 KB band can retune with `-DOUTROW_DEPTH=4` +
`-DA1_ROW_DEPTH=4` on its own blobs; compatibility toward deeper decoders is
monotone, and the degradation logger's lag histogram re-derives the right
depth for any new corpus.

## Permanent gate asset — decision: YES (2026-07-04, supersedes the prior NO)

The foreign set is now a tracked, permanently-gated corpus. Owner decision
reversing the earlier "keep it out of the repo" call: a second lineage that the
encoder was never tuned on is worth a standing regression guard, and the cost
turned out to be small enough to fold into the existing budget.

**Pinned set (18 images, 34 pair-directions).** Two contiguous release families
joined by one cross-major jump, under `test-bench/foreign/<ver>/watch.bin`:

- old family, raw `.bin` (app already at 0x2000): `2.2.0 2.2.1 2.2.2 2.2.3
  2.2.4 2.3.0 2.3.1 3.0.0 3.0.1 3.0.2 3.0.3`
- new family, UF2 unpacked to app base 0x2000: `10.0.0 10.0.1 10.0.2 10.0.3
  10.1.1 10.1.2 10.1.3`

17 adjacent pairs (the `3.0.3 -> 10.0.0` boundary is the cross-major one), each
applied in both directions = 34. The images are committed and final; their bytes
are pinned by `test-bench/foreign.sha256` (verified by `make check-assets`). The
new-family images were UF2-unpacked to app base 0x2000 to match the raw-`.bin`
layout of the old family; the S3 source path above plus the `## Method` recipe
document their origin. The new family stops at 10.1.3 (not 10.2.1) to land exactly
18 images; extend both lists and re-pin the manifest + `BASE_FOREIGN_TOTAL` to
grow the set.

**Gate integration (cost kept near-zero).** The 34 foreign pair-directions run
in the SAME parallel pool as the 256-pair home matrix (`check_corpus.sh`), not a
second leg. The single slowest job in the whole gate is the foreign cross-major
pair (`3.0.3 <-> 10.0.0`, effectively unrelated programs, ~13 s, 150-175 KB
blob); it is emitted FIRST so it overlaps every other pair (longest-processing-
time-first). Net effect on `make gate`: ~23 s -> ~29 s wall on the 32-core
reference machine, comfortably under the 60 s budget, for ~154 CPU-s of added
work. Reported metrics: `foreign round-trips` (must be 34/34) and
`foreign full_total`, which ratchets against `BASE_FOREIGN_TOTAL` exactly like
`BASE_FULL_TOTAL`. NVM write-safety maxima (amplified / erases-per-row /
frontier inversions) cover the foreign pairs too — all clean, journal peak 768
(the degradation budget), which is the gate's overall journal-peak driver.

Current measurement with the landed encoder: 34/34 round-trip byte-exact,
`foreign full_total = 1,373,186` B (this is the pinned `BASE_FOREIGN_TOTAL`; it
differs from the historical 1,234,665 above because the encoder has moved on
since 2026-07-03). Re-pin on any intentional wire change, same as the home
baseline.
