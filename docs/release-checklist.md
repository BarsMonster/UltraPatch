# Release Checklist

This checklist covers the in-repository A1 patcher release scope: encoder,
decoder source, host verification, corpus reproducibility, and release
provenance. Product OTA signing, bootloader integration, power-fail recovery, and
hardware flash validation are external integration work.

## Required Inputs

- Release commit on `main`.
- Clean working tree.
- Pinned corpus assets committed under `test-bench/images`,
  `test-bench/fixtures`, and `test-bench/foreign`.
- `test-bench/corpus.sha256` and `test-bench/foreign.sha256` committed with the
  release.
- ARM GCC/binutils available as documented in `install.md`.

## Gate

Run:

```sh
make gate
```

Record the complete output in the release notes. The gate must report:

- `corpus assets`: verified through `test-bench/corpus.sha256`
- `foreign assets`: verified through `test-bench/foreign.sha256`
- `malformed rejects`: nonzero deterministic reject count
- `edge inputs`: all synthetic edge cases round-tripped or cleanly refused
- `golden wire`: OK against the committed `test-bench/golden.sha256` (the wire
  freeze — an unexplained mismatch blocks release)
- `model contract`: OK for shared model/default-cap invariants
- `wire config override`: OK for one nondefault same-name/same-value override
  across encoder plus source, generated, and Cortex-M0+ ARM decoder compile paths
- ARM integration shape and `.text/.data/.bss`
- ARM soft-divide count
- `matrix round-trips`: `256/256`
- corpus `full_total`
- foreign `full_total`
- real one-face grow/revert patch sizes
- NVM page amplification, max erases-per-page, frontier inversions, unaligned or
  out-of-range page calls, and final-page canary corruption
- journal peak slots
- final `RESULT: ALL GATES PASS`

Also run before release (not part of `make gate`):

- A clang leg (`make CC=clang -B all && make check check-malformed check-golden`)
  — CI runs this on every push; the golden check proves the clang-built encoder
  emits byte-identical blobs.
- `make decoder-header` if publishing a one-file device decoder artifact.

Do not ship from a build that requires deployment-only CFLAGS or relaxed baseline
thresholds.

Record the release `WIRE_CONFIG_FLAGS` value. The encoder and decoder **MUST**
use the exact same wire macro names with the exact same values; the target
family, `WINDOW_LOG`, `JSLOTS`, `OPC_CAP`, `OUTROW`, `OUTROW_DEPTH`,
`DR_KCAP_BL`, `DR_KCAP_EX`, and any wire-model override belong in that shared
value. `PATCH_IMAGE_BASE` is decoder-only integration configuration and must stay
separate from the wire flags.

## Corpus Bundle

To publish a standalone copy of the exact binary gate assets:

```sh
scripts/pack_corpus.sh artifacts/a1-corpus.tar.gz
```

Publish both `artifacts/a1-corpus.tar.gz` and
`artifacts/a1-corpus.tar.gz.sha256` with the release. The archive is
deterministic for a fixed `test-bench/corpus.sha256` manifest.

## Artifacts

The release source artifact is the Git commit. The device decoder artifact is
either the generated single header from `make decoder-header` or the decoder
source header set rooted at `src/patch_apply.h`. The host tool is the unified
`ultrapatch` CLI built from `src/patch_generate.c`, the `src/enc_*.c` subsystem
modules, `src/patch_host_backend.c`, and the vendored
`vendor/libdivsufsort/` sources; encode is its default mode, while `--decode`
is the host reference/debug mode.

For traceability, release notes should include:

- Git commit SHA.
- `sha256sum test-bench/corpus.sha256`.
- `sha256sum test-bench/foreign.sha256`.
- `sha256sum artifacts/patch_apply_single.h` when a one-file decoder header is
  published.
- `sha256sum artifacts/a1-corpus.tar.gz` when a corpus bundle is published.
- `make gate` output.
- Toolchain package/version used for `check-arm`.
- License statement: project MIT except vendored libdivsufsort, which retains
  its upstream notice; `src/enc_bsdiff.c` includes an attribution note for
  the detools Python implementation that informed the ARM Cortex-M scanner
  reimplementation.

## Scope Boundary

Before handing off to product integration, confirm that `docs/device-integration.md`
is included with the release. It defines the decoder call contract and explicitly
places patch authentication, anti-rollback policy, bootloader recovery, and real
flash-driver validation outside this source release.
