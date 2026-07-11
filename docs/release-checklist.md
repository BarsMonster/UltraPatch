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
- Canonical ordered membership committed in `test-bench/release-inventory.tsv`.
- `test-bench/corpus.sha256` and `test-bench/foreign.sha256` committed with the
  release, with the size/wire/golden baselines validated against the same
  inventory by `make check-release-inventory`.
- The exact default GCC, required Clang, GNU Arm GCC/binutils,
  `libc.a`/`libgcc.a` content hashes, and effective flags recorded in
  `toolchains/release-profile.json`, with packages installed as documented in
  `install.md`.

## Gate

Run:

```sh
make check-build-profile
make gate
```

`make check-build-profile` is the build-isolation regression: it must prove that
colliding compiler/configuration builds select and execute their own host tools.
The gate independently validates the release descriptor before forking its
verification legs. Record the complete output in the release notes. The gate
must report:

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
  across encoder plus source, generated, and Cortex-M0+ ARM decoder compile paths
- ARM and stack packaging parity: the source-header set and the one canonical
  generated header have identical object/linked footprints and both integration
  shapes have identical static stack results
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

- A clang leg
  (`make CC=clang -B all && make CC=clang check check-malformed check-golden`)
  — CI runs this on every push; keeping `CC=clang` on both commands selects the
  same profile-scoped host binary, and the golden check proves that encoder emits
  the frozen wire blobs.
- `make decoder-header` if explicitly refreshing the one-file device decoder
  artifact outside `make gate` (the gate refreshes the same canonical path before
  its parallel checks).

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
either the generated single header from `make decoder-header` or the decoder
source header set rooted at `src/patch_apply.h`. The host tool is the unified
`ultrapatch` CLI at the path printed by `make host-tool-path`; normal builds place
it at `.build/<profile-id>/ultrapatch`. It is built from `src/patch_generate.c`,
the `src/enc_*.c` subsystem modules, `src/patch_host_backend.c`, and the vendored
`vendor/libdivsufsort/` sources; encode is its default mode, while `--decode`
is the host reference/debug mode.

The generated form is published atomically at `artifacts/patch_apply_single.h`:
first creation uses mode `0644`, while replacement preserves an existing readable
permission mode. All release checks consume this exact file rather than private
per-test regenerations. Both packaging forms support the default internally linked
decoder and the opt-in multi-TU split (`ULTRAPATCH_IMPLEMENTATION` in exactly one
TU, `ULTRAPATCH_DECLARATIONS_ONLY` in callers); the release contract compiles,
links, and behaviorally exercises all three modes.

For traceability, release notes should include:

- Git commit SHA.
- `sha256sum test-bench/corpus.sha256`.
- `sha256sum test-bench/foreign.sha256`.
- `sha256sum test-bench/release-inventory.tsv`.
- `sha256sum artifacts/patch_apply_single.h` when a one-file decoder header is
  published.
- `sha256sum artifacts/a1-corpus.tar.gz` when a corpus bundle is published.
- `sha256sum toolchains/release-profile.json`.
- The `release_profile` line from `make gate`.
- `make gate` output.
- Exact tool identities, runtime archive hashes, and effective flags validated
  against `toolchains/release-profile.json` by `make gate`.
- License statement: project MIT except vendored libdivsufsort, which retains
  its upstream notice; `src/enc_bsdiff.c` includes an attribution note for
  the detools Python implementation that informed the ARM Cortex-M scanner
  reimplementation.

## Scope Boundary

Before handing off to product integration, confirm that `docs/device-integration.md`
is included with the release. It defines the decoder call contract and explicitly
places patch authentication, anti-rollback policy, bootloader recovery, and real
flash-driver validation outside this source release.
