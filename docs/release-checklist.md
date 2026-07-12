# Release Checklist

This checklist covers the in-repository patcher release scope: encoder,
decoder source, host verification, corpus reproducibility, and release
provenance. Product OTA signing, bootloader integration, power-fail recovery, and
hardware flash validation are external integration work.

## Required Inputs

- Release commit on `main`.
- Clean working tree.
- Home and fixture ELFs committed under `test-bench/images` and
  `test-bench/fixtures`; foreign binaries committed under `test-bench/foreign`.
- Ordered membership, asset hashes, and the 17 undirected foreign edges in
  `test-bench/corpus-inventory.tsv`.
- Home sizes, all corpus wire hashes, and the four additional golden wires in
  `test-bench/wire-baseline.tsv`, validated against the inventory by
  `make check-release-inventory`.
- The five compiler-independent encoder kernel result digests in
  `test-bench/encoder-kernel-baseline.tsv`.
- The selected host/Arm GCC drivers and named subtools (`cc1`, `collect2`,
  assembler, linker), required Clang and named binutils, `libc.a`/`libgcc.a`
  content hashes, compiler environment policy, effective compile/link flags,
  and authoritative CI container digest recorded in
  `toolchains/release-profile.json`, with packages installed as documented in
  `install.md`.

For an intentional tool/archive/flag update, refresh and review the complete
schema-3 lock:

```sh
/usr/bin/make release-profile-json
/usr/bin/make release-profile-update
git diff -- toolchains/release-profile.json
make check-release-profile
```

The atomic updater takes the exclusive release-input lock, rejects runtime
Make/tool/flag overrides, validates and preserves the existing immutable
container wrapper, preserves the file mode,
and makes an identical second update a true no-op. Change the container digest
only by an explicit reviewed edit, mirror it in `.github/workflows/gate.yml`,
then refresh and run the full evidence suite. Candidate inspection and update
both use the canonical `/usr/bin:/bin` path, C locale, isolated system Python,
and absolute launchers, and reject Make launch controls before any recipe runs.

## Gate

Run the matching local preflight from a clean checkout on `main`:

```sh
/usr/bin/python3 scripts/release_gate.py
```

The driver holds the release-input lock, requires a clean `main` checkout (and a
matching `GITHUB_SHA` when provided), captures that commit, and runs from a
fresh temporary `git archive` so ignored `.build` state and working-tree bytes
cannot enter verification. Its child commands receive a small fixed
environment. It streams the build-isolation, `make gate`, ASan/UBSan, and
required Clang commands, requires each command's explicit success evidence,
then rechecks the original branch, commit, and working tree. Every public Make
target retains its hard 80-second cap. Record the complete output.

This local run verifies that its selected tools and archives match the lock; it
does not attest its runtime OCI container. The authoritative release result is
the successful push workflow for the exact `github.sha`, executed inside the
container digest pinned in both the workflow and the full release lock.

`make gate` remains the development correctness gate and independently validates the
release descriptor before forking its verification legs. It must report:

- `release_profile`: the validated profile identifier from
  `toolchains/release-profile.json`
- `release inventory`: all committed asset and wire baselines agree
- `corpus assets` and `foreign assets`: verified through
  `test-bench/corpus-inventory.tsv`
- `malformed rejects`: nonzero deterministic reject count
- `edge inputs`: all synthetic edge cases round-tripped or cleanly refused
- `golden wire`: OK against the G rows in `test-bench/wire-baseline.tsv` (the
  C/F rows are checked by the corpus matrix; unexplained drift blocks release)
- `model contract`: OK for shared model/default-cap invariants
- `wire config override`: OK for one nondefault same-name/same-value override
  across encoder plus host and Cortex-M0+ ARM decoder compile paths
- ARM object/linked footprint and both static/generic integration stack bounds
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

The driver also runs `make check-clang`, which uses the descriptor-pinned `CLANG`
command for a warning-clean build, proves that its encoder emits the frozen wire,
and emits `clang_contract=OK`; it also runs `make check-decoder-sanitize` and
`make check-encoder-sanitize`.

Do not ship from a build that requires deployment-only CFLAGS or relaxed baseline
thresholds.

For an intentional wire change, run `make golden-update`. It measures a private
candidate, rejects any worse home pair or one-face regression, and updates
`test-bench/wire-baseline.tsv` plus the four Makefile size pins. Commit those two
files together. Publication uses ordinary same-directory replacements; if it is
interrupted between them, restore the pair and rerun:

```sh
git restore -- Makefile test-bench/wire-baseline.tsv
make golden-update
```

For a deliberate semantic change to an encoder kernel, run
`make encoder-kernel-baseline-update`, inspect the changed digests, and commit
`test-bench/encoder-kernel-baseline.tsv` with the implementation. The updater is
explicit and never runs from normal builds, `make gate`, or `make golden-update`.

The host CLI's successful output-publication contract is the native host OS
behavior of its same-directory temporary file and `rename()`. A separate
cross-platform transaction or filesystem durability layer is not a release
requirement; reported I/O failures must still return nonzero and preserve an
existing destination. This policy applies only to host files, not device flash.

Verify the release uses the committed `patch_config.h`; it is the sole production
definition of the encoder/decoder wire and resource constants. `PATCH_IMAGE_BASE`
and `PATCH_IMAGE_CAPACITY` remain decoder-only integration configuration.

## Artifacts

The release source artifact is the Git commit. The device decoder artifact is
the three-file header set rooted at `src/patch_apply.h`, with `src/rc_models.h`
and `src/patch_config.h` installed beside it. Integration code includes only
`patch_apply.h`; it is not a self-contained amalgamation. The host tool is the
unified `ultrapatch` CLI at the path printed by `make host-tool-path`; normal
builds place it at `.build/<profile-id>/ultrapatch`. It is built from
`src/patch_generate.c`, the `src/enc_*.c` subsystem modules,
`src/patch_host_backend.c`, and the vendored `vendor/libdivsufsort/` sources;
encode is its default mode, while `--decode` is the host reference/debug mode.

For traceability, release notes should include:

- Git commit SHA.
- The `release_commit`, `release_source`, and `release_preflight` lines from the
  matching local driver, plus the URL/status of the authoritative push CI job.
- `sha256sum test-bench/corpus-inventory.tsv`.
- `sha256sum test-bench/wire-baseline.tsv`.
- `sha256sum toolchains/release-profile.json`.
- The `release_profile` line from `make gate`.
- `make gate` output.
- Selected driver/subtool/binutil identities and executable hashes, named runtime
  archive hashes, compiler environment policy, effective compile/link flags, and
  configured CI container digest validated against
  `toolchains/release-profile.json` by `make gate` and push CI. This scope is not
  a recursive dynamic-library closure; the behavioral gates remain required.
- License statement: project MIT except vendored libdivsufsort, which retains
  its upstream notice; `src/enc_bsdiff.c` includes an attribution note for
  the detools Python implementation that informed the ARM Cortex-M scanner
  reimplementation.

## Scope Boundary

Before handing off to product integration, confirm that `docs/device-integration.md`
is included with the release. It defines the decoder call contract and explicitly
places patch authentication, anti-rollback policy, bootloader recovery, and real
flash-driver validation outside this source release.
