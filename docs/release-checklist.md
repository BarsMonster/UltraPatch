# Release Checklist

This checklist covers the in-repository A1 patcher release scope: encoder,
decoder source, host verification, corpus reproducibility, and release
provenance. Product OTA signing, bootloader integration, power-fail recovery, and
hardware flash validation are external integration work.

## Required Inputs

- Release commit on `main`.
- Clean working tree.
- Pinned corpus assets committed under `test-bench/images` and
  `test-bench/fixtures`.
- `test-bench/corpus.sha256` committed with the release.
- ARM GCC/binutils available as documented in `install.md`.

## Gate

Run:

```sh
make gate
```

Record the complete output in the release notes. The gate must report:

- `corpus assets`: verified through `test-bench/corpus.sha256`
- `malformed rejects`: nonzero deterministic reject count
- ARM `.text/.data/.bss`
- ARM soft-divide count
- `matrix round-trips`: `256/256`
- corpus `full_total`
- real one-face grow/revert patch sizes
- NVM row amplification, max erases-per-row, frontier inversions
- journal peak slots
- final `RESULT: ALL GATES PASS`

Do not ship from a build that requires deployment-only CFLAGS or relaxed baseline
thresholds.

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
`src/patch_apply.h` plus `src/rc_models.h`; the host encoder is built from
`src/patch_generate.c`, `src/arm_cortex_m4.c`, and the vendored
`vendor/libdivsufsort/` sources.

For traceability, release notes should include:

- Git commit SHA.
- `sha256sum test-bench/corpus.sha256`.
- `sha256sum artifacts/a1-corpus.tar.gz` when a corpus bundle is published.
- `make gate` output.
- Toolchain package/version used for `check-arm`.
- License statement: project MIT except vendored libdivsufsort, which retains
  its upstream notice; `src/arm_cortex_m4.c` includes an attribution note for
  the detools Python implementation that informed the C reimplementation.

## Scope Boundary

Before handing off to product integration, confirm that `docs/device-integration.md`
is included with the release. It defines the decoder call contract and explicitly
places patch authentication, anti-rollback policy, bootloader recovery, and real
flash-driver validation outside this source release.
