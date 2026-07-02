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
direction: `hy_enc from to blob 10` (self-verification included), then
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

## Permanent gate asset — decision: NO

The foreign binaries stay out of the repo: they are third-party build
artifacts (weight + provenance noise), a network-fetching gate would be flaky,
and the wire is frozen — the in-family gates plus encoder self-verification
already prevent regressions the foreign set could catch. This document plus
the S3 path above make the study reproducible on demand.
