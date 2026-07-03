# Decoder Fuzz Campaign + Branch-Coverage Audit (2026-07-02)

> **Superseded in part (2026-07-03):** moving `CRC32(to)` into the envelope header
> deleted the trailer-withhold ring and the `trailer_ok` short-ring guard, and shifted
> every `src/patch_apply.h` line number cited below. The campaign result (0 findings) and
> the branch-classification reasoning still hold; the specific `trailer_ok`/ring entries and
> line pins predate that change. Re-run `make fuzz` for a current campaign (the harness now
> force-fixes only `CRC32(from)` and cross-checks the header `CRC32(to)` field at bytes 4..7).

Run against the post-robustness-sweep decoder (single pull-mode core, mandatory
`CORTEX_M0`, wire frozen at golden). The libFuzzer+ASan+UBSan harness
(`fuzz/fuzz_apply.c`) executes whole blobs against an in-memory flash emulator,
force-fixes `CRC32(from)` so mutations reach the body, and independently
cross-checks `CRC32(to)` on every DONE (no silent-wrong accept).

## Campaign

- 14 parallel jobs x 1 h, ~2.24M executions this campaign (~5M+ lifetime
  including the earlier 3M-exec run): **0 crashes, 0 leaks, 0 timeouts, 0
  sanitizer reports, 0 silent-wrong accepts.** Corpus grew 565 -> ~10,300
  units (persisted in `fuzz-corpus/`, deliberately untracked).
- Harness improvement landed: the `size < 5` minimum was removed so sub-header
  blobs (0–4 bytes, EOF during the envelope reads) are fuzzed too.

## Coverage (llvm-cov over the final corpus + the check/malformed/edge suites)

`src/patch_apply.h`: **100% functions, 99.8% lines, 98.3% regions, 94.5%
branches** from the fuzz corpus alone; the deterministic suites additionally
cover what the harness is structurally blind to.

Every unreached branch was classified by hand; none is dead product logic:

| Class | Sites | Assessment |
|---|---|---|
| Corrupt-stream overflow/UB guards needing ~2^31-magnitude coded values (`s_bv` shift cap, `bb_unzz` 0xFFFFFFFF, `sa_apply_op` dl/el/fp/gap overflow checks) | 197, 609, 873, 886, 891, 898, 905, 919 | Defensive by design; make hostile streams *defined*, not reachable-cheap. Keep. |
| Unreachable-by-construction invariant guards (out_read/out_write span gates, `ldr_targets` window bound) | 468, 474, 641 | Belt-and-braces on internal invariants; correct to keep in a frozen safety artifact. |
| Fuzzer-blind by harness design, covered by deterministic suites | `CRC32(from)` mismatch (982; harness force-fixes it), post-op error exit (1054) | Covered by `make check` / `check-malformed`. |
| Data-dependent clamps never hit on real images | `lit_tree_from_hist` probability clamp (1070) | Correctness clamp; keep. |

CBMC bounded proofs (stretch goal): ABANDONED — permanent decision (2026-07-03).
An attempt with per-kernel harnesses (journal insert/search, bit-tree accessors,
envelope readers, Golomb/varint readers) was intractable in practice: without
hand-pinned per-proof `--unwind` bounds CBMC saturation-unwinds the adaptive-unary
loops (corrupt-stream cap `RC_RICE_UNARY_MAX` = 2^20) and bit-blasts symbolic
range-coder state into a SAT instance that consumed >64 GB and repeatedly took
the host down. Taming it (nondet-bit abstractions, cgroup-guarded runners,
per-proof unwind tables) is real ongoing maintenance for kernels that are
already covered by the 5M+-exec sanitized fuzz campaign, the model-level
encoder/decoder differential tests (`make check-models`), and the deterministic
suites. Cost/benefit negative — do not retry. Any future formal-methods attempt
must run under a hard memory cap + timeout from the first invocation.

Reproduce: `make fuzz` (smoke) or
`./fuzz_apply -jobs=$(nproc) -max_total_time=3600 fuzz-corpus`; coverage via a
clang `-fprofile-instr-generate -fcoverage-mapping` build of `hy_dec` +
`fuzz_apply`, replaying `fuzz-corpus` and the check suites, merged with
`llvm-profdata` and reported with `llvm-cov`.
