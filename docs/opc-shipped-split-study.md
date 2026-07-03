# Shipped OPC_CAP Op-Split — Reproduction Recipe (2026-07-03)

`make check-degrade` asserts the OPC_CAP op-split loop deterministically at the
PLAN-SWEEP level (`opc_splits_sweep>=1` on committed corpus images:
non-winning masked variants concentrate hundreds of corrections and split).
This note records the stronger case — a split in the SHIPPED (winning) plan —
which was proven reachable but deliberately left OUT of the gate.

## Result (stock hy_enc, unmodified source)

`scripts/make_opc_pair.py` (requires `arm-none-eabi-gcc`; tested 14.2.1)
generates a from/to Cortex-M0+ pair where the winning plan is the masked
variant (mask BL + literal pools, plan idx 5) with 10 ops over the
80-correction cap (peak 143 pre-split):

```
A1_DEGRADE dir=fwd natural=0 deg_journal=1 pres_needed=5948 converted=5006
           opc_splits=10 opc_splits_sweep=10 budget=1024 opc_cap=80
blob = 7905 B, 678 ops; hy_dec round-trips byte-exactly.
```

## Why it is not gated

- **Toolchain sensitivity.** The masked plan wins the size sweep by only
  ~98 B (7892 vs 7990) against the fuzz-20 op-derived variant, and the race
  depends on gcc's exact code layout. A compiler upgrade could flip the
  winner and fail the gate spuriously. The gate's sweep-level assertion is
  toolchain-independent (committed images) with a 300%-over-cap margin.
- **No decoder coverage delta.** A split op is just two ordinary ops on the
  wire; degradation is pure encoder planning. The decoder decodes nothing new,
  and every emitted blob is self-verified by the reference decoder anyway.

## Mechanism (measured, resolves how a shipped split can exist at all)

Op-derived field deltas (variants 1/2) are tautologically exact at the op-walk
position, so block-match/op-alignment disagreements never survive into those
plans (`corr<=1`). The ONLY route to a shipped split is making the MASKED
variant the smallest feasible body: dense BL-immediate + literal-pool churn
defeats plain bsdiff anchoring, masking then forces long copies through
mixed-size (2-/4-byte Thumb) recompiled code, and masked misalignments map
from-fields onto invalid to-positions that `merge_op_field_deltas` rejects —
each becomes a correction, and they concentrate per-op. Uniform all-BL code
realigns at period 4 and stays exact: MIXED instruction sizes are required.
Small hand-crafted pairs cannot reach it (op-derived exactness gives corr=0);
real compiler output is necessary. Fire window is churn ~36–42%.
