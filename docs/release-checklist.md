# Release Checklist

This checklist covers the host encoder, header-only decoder, and their measured release corpus and
device footprint. Product signing, anti-rollback policy, bootloader recovery, and real flash-driver
validation remain integration responsibilities.

## Required inputs

- Use a clean `main` checkout at the exact release commit.
- Keep the frozen static `watch.bin` corpus under `test-bench/images`, `test-bench/fixtures`, and
  `test-bench/foreign` intact. The gate reads all 36 binaries directly; it does not derive or
  materialize corpus inputs during the release build.
- Install the packages in [install.md](../install.md). Exact compiler and system-library identities
  are not release criteria; the measured size and memory outcomes are.

## Verification

Run and retain the complete output:

```sh
make gate
```

Do not release unless it succeeds. Review all reported outcomes: 290/290 self-verifying corpus
encodes, the complete corpus total, the real one-face grow and revert sizes, reference-static-wrapper
ARM flash/state, and worst supported decoder stack.

Every successful encoder call has already applied the emitted patch through the production decoder,
required the exact target and complete blob consumption, and enforced NVM write safety. The corpus
gate records a size only after that self-verification succeeds.

## Artifacts and evidence

The source artifact is the Git commit. The device decoder artifact is the three-file header set
`src/patch_apply.h`, `src/patch_config.h`, and `src/rc_models.h`; install them together and include
only `patch_apply.h`. The host artifact is the path printed by `make -s host-tool-path`.

Release notes must include the commit SHA and complete `make gate` output, including the real
one-face grow/revert metrics.

Include [the device integration contract](device-integration.md) in the handoff.
