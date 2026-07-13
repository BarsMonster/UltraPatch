# Release Checklist

This checklist covers the encoder, decoder source, corpus, and build
provenance. Product signing, anti-rollback policy, bootloader recovery, and real
flash-driver validation remain integration responsibilities.

## Required inputs

- Use a clean `main` checkout at the exact release commit.
- Commit the home and fixture ELFs under `test-bench/images` and
  `test-bench/fixtures`, and foreign binaries under `test-bench/foreign`.
- Keep ordered corpus membership, asset hashes, and foreign edges in
  `test-bench/corpus-inventory.tsv`.
- Keep home per-pair sizes, golden-blob sizes, and frozen hashes for every corpus
  and golden wire in `test-bench/wire-baseline.tsv`.
- Install the packages in [install.md](../install.md). The project does not pin
  exact compiler, binutils, system-library, or archive identities; the behavior,
  wire, compression, ARM-size, and stack ratchets are the release criteria.

## Verification

From the clean release checkout, run and retain the complete output:

```sh
make gate
make check-decoder-sanitize
make check-encoder-sanitize
make check-clang
```

This sequence verifies the complete release gate, sanitizers, and the required
Clang leg. Do not release unless every command succeeds and `make gate` reports
`ALL GATES PASS`.

Review the complete output, including home and foreign round trips and totals,
the real one-face grow and revert patch sizes, ARM memory and stack results, and
NVM write-safety results. Do not ship a build that needs deployment-only flags
or relaxed baselines.

The authoritative release result is the retained output from the verification
sequence above, run from the exact release commit. Package installation is
intentionally not an exact toolchain-identity contract; any resulting output
change must still pass the release ratchets.

## Intentional baseline changes

For an intended wire change, run:

```sh
make golden-update
```

Review every per-pair result and the real one-face grow/revert sizes. Do not
accept a worse home pair, a foreign-total regression, or either one-face
regression. Commit the updated `test-bench/wire-baseline.tsv` and four Makefile
size pins with the implementation in the same commit.

## Artifacts and evidence

The source artifact is the Git commit. The device decoder artifact is the
three-file header set `src/patch_apply.h`, `src/patch_config.h`, and
`src/rc_models.h`; install them together and include only `patch_apply.h`. The
host artifact is the `ultrapatch` path printed by `make -s host-tool-path`.

Release notes must include:

- the Git commit SHA;
- SHA-256 hashes of `test-bench/corpus-inventory.tsv` and
  `test-bench/wire-baseline.tsv`;
- the complete gate output, including the real one-face grow/revert metrics;
- the project and vendored-dependency license statement.

Include [the device integration contract](device-integration.md) in the handoff.
