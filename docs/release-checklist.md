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
- Canonical ordered membership and the 17 undirected foreign edges committed in
  `test-bench/release-inventory.tsv`.
- `test-bench/corpus.sha256` and `test-bench/foreign.sha256` committed with the
  release, with the size/wire/golden baselines validated against the same
  inventory by `make check-release-inventory`.
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
make check-release-profile check-release-gate-contract
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
/usr/bin/python3 -I -S scripts/release_gate.py
```

The driver rejects inherited Make/Python launch controls, global/system Git
configuration, non-normal index flags, and a mismatched `GITHUB_SHA`; `-I -S`
also prevents environment or site-package import injection before those checks. It holds
the release-input lock, captures one clean `main` commit, and runs from a fresh
temporary `git archive` export so ignored `.build` state and working-tree bytes
cannot enter the verification. It streams and captures the build-isolation,
`make gate`, ASan/UBSan, and required Clang commands, requiring each command's
explicit success evidence, and rechecks the original branch, commit, index, and
working tree after every command. Every public Make target retains its hard
80-second cap. Record the complete output.

This local run verifies that its selected tools and archives match the lock; it
does not attest its runtime OCI container. The authoritative release result is
the successful push workflow for the exact `github.sha`, executed inside the
container digest pinned in both the workflow and the full release lock.

`make gate` remains the development correctness gate and independently validates the
release descriptor before forking its verification legs. It must report:

- `release_profile`: the validated profile identifier from
  `toolchains/release-profile.json`
- `release inventory`: all committed asset and wire baselines agree
- `corpus assets`: verified through `test-bench/corpus.sha256`
- `foreign assets`: verified through `test-bench/foreign.sha256`
- `malformed rejects`: nonzero deterministic reject count
- `edge inputs`: all synthetic edge cases round-tripped or cleanly refused
- `golden wire`: OK against the committed `test-bench/golden.sha256` (the wire
  freeze — an unexplained mismatch blocks release)
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
and emits `clang_contract=OK`; it also runs `make check-decoder-sanitize`.

Do not ship from a build that requires deployment-only CFLAGS or relaxed baseline
thresholds.

The host CLI's successful output-publication contract is the native host OS
behavior of its same-directory temporary file and `rename()`. A separate
cross-platform transaction or filesystem durability layer is not a release
requirement; reported I/O failures must still return nonzero and preserve an
existing destination. This policy applies only to host files, not device flash.

Record the release `WIRE_CONFIG_FLAGS` value. The encoder and decoder **MUST**
use the exact same wire macro names with the exact same values; the target
family, `WINDOW_LOG`, `JSLOTS`, `OPC_CAP`, `OUTROW`, `OUTROW_DEPTH`,
`DR_KCAP_BL`, `DR_KCAP_EX`, and any wire-model override belong in that shared
value. `PATCH_IMAGE_BASE` and `PATCH_IMAGE_CAPACITY` are decoder-only integration
configuration and must stay separate from the wire flags.

## Corpus Bundle

To publish a standalone copy of the exact binary gate assets:

```sh
scripts/pack_corpus.sh artifacts/a1-corpus.tar.gz
```

Publish both `artifacts/a1-corpus.tar.gz` and
`artifacts/a1-corpus.tar.gz.sha256` with the release. The archive is
deterministic for the canonical release inventory and contains only its verified
asset paths plus the asset, size, wire, and golden manifests. The packer verifies
the complete staged archive before same-directory publication; the checksum
detects an interruption between archive and checksum replacement. Run
`make check-pack-corpus` to exercise deterministic output and injected
generation/publication failures.

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
- `sha256sum test-bench/corpus.sha256`.
- `sha256sum test-bench/foreign.sha256`.
- `sha256sum test-bench/release-inventory.tsv`.
- `sha256sum artifacts/a1-corpus.tar.gz` when a corpus bundle is published.
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
