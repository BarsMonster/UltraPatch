# Release Checklist

This checklist covers the encoder, decoder source, corpus, and build
provenance. Product signing, anti-rollback policy, bootloader recovery, and real
flash-driver validation remain integration responsibilities.

## Required inputs

- Use one clean commit on `main`.
- Commit the home and fixture ELFs under `test-bench/images` and
  `test-bench/fixtures`, and foreign binaries under `test-bench/foreign`.
- Keep ordered corpus membership, asset hashes, and foreign edges in
  `test-bench/corpus-inventory.tsv`.
- Keep corpus sizes and frozen wire hashes in `test-bench/wire-baseline.tsv`,
  and encoder-kernel digests in `test-bench/encoder-kernel-baseline.tsv`.
- Install the packages in [install.md](../install.md), and use the toolchain,
  archive, flag, environment, and CI-container identities recorded in
  `toolchains/release-profile.json`.

An intentional toolchain, archive, or flag change requires a reviewed profile
refresh and the full release evidence:

```sh
/usr/bin/make release-profile-json
/usr/bin/make release-profile-update
git diff -- toolchains/release-profile.json
make check-release-profile
```

Changing the container digest is a separate deliberate edit. Mirror it in
`.github/workflows/gate.yml` before running the full evidence suite.

## Verification

From the clean release checkout, run and retain the complete output:

```sh
/usr/bin/python3 scripts/release_gate.py
```

The preflight verifies an archive of the selected commit and runs build-profile
isolation, the complete release gate, sanitizers, and the required Clang leg. Do
not release unless every command succeeds and `make gate` ends with
`RESULT: ALL GATES PASS`.

Review the complete output, including the validated release profile, home and
foreign round trips and totals, the real one-face grow and revert patch sizes,
ARM memory and stack results, and NVM write-safety results. Do not ship a build
that needs deployment-only flags or relaxed baselines.

The authoritative release result is the successful push workflow for the exact
release commit, inside the container digest pinned by the workflow and release
profile.

## Intentional baseline changes

For an intended wire change, run:

```sh
make golden-update
```

Review every per-pair result and the real one-face grow/revert sizes. Do not
accept a worse home pair or a one-face regression. Commit the updated
`test-bench/wire-baseline.tsv` and Makefile size pins with the implementation in
the same commit.

For an intended encoder-kernel semantic change, run
`make encoder-kernel-baseline-update`, inspect the changed digests, and commit
`test-bench/encoder-kernel-baseline.tsv` with the implementation.

## Artifacts and evidence

The source artifact is the Git commit. The device decoder artifact is the
three-file header set `src/patch_apply.h`, `src/patch_config.h`, and
`src/rc_models.h`; install them together and include only `patch_apply.h`. The
host artifact is the `ultrapatch` path printed by `make -s host-tool-path`.

Release notes must include:

- the Git commit SHA and authoritative push-CI URL/status;
- the preflight's `release_commit`, `release_source`, and `release_preflight`
  lines;
- SHA-256 hashes of `test-bench/corpus-inventory.tsv`,
  `test-bench/wire-baseline.tsv`, and `toolchains/release-profile.json`;
- the complete gate output, including `release_profile` and the real one-face
  grow/revert metrics;
- the project and vendored-dependency license statement.

Include [the device integration contract](device-integration.md) in the handoff.
