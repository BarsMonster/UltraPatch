# Ultrapatcher

Final A1 firmware patcher for the Sensor Watch target.

Production code lives under `src/`, with third-party code under `vendor/`:

- `src/patch_apply.h`: reusable header-only streaming in-place decoder entry point;
  ship it with `src/rc_models.h` and `src/patch_config.h`, or generate the
  single public decoder header with `make decoder-header`.
- `src/patch_host_backend.c`: shared host reference-decoder backend used by
  `ultrapatch --decode` and encoder self-verification, including the host NVM
  emulator.
- `src/patch_generate.c`: host-side unified `ultrapatch` CLI entry point. Encode
  is the default mode and decode is a host reference/debug mode backed by
  `src/patch_host_backend.c`; the device decoder artifact is still only the
  header set rooted at `src/patch_apply.h`. Encoder complexity is deliberately
  host-side and is built from normal internal `src/enc_*.c` modules (`enc_util`,
  `enc_elf`, `enc_bsdiff`, `enc_field`, `enc_rc`, `enc_lz`, `enc_emit`,
  `enc_plan`).
- `vendor/libdivsufsort/`: vendored C suffix sorter used by the encoder.
- `src/rc_models.h`: shared wire-model constants and packed model helpers.
- `src/patch_config.h`: shared build-time configuration defaults and mirror knobs.

Build and smoke-test:

```sh
make
make check
```

Generate the standalone public decoder header:

```sh
make decoder-header
```

By default this writes `artifacts/patch_apply_single.h`, containing
`patch_config.h`, `rc_models.h`, and `patch_apply.h` in dependency order. The
contract test compiles this artifact without the `src/` include path.

`WIRE_CONFIG_FLAGS` is the single repository build path for wire-affecting
overrides and defaults to `-DCORTEX_M0`. Encoder and decoder builds **MUST** use
the exact same macro names with the exact same values for `WINDOW_LOG`,
`JSLOTS`, `OPC_CAP`, `OUTROW`, `OUTROW_DEPTH`, `DR_KCAP_BL`, `DR_KCAP_EX`, the
target family, and any wire-model override. `PATCH_IMAGE_BASE` and
`PATCH_IMAGE_CAPACITY` are separate, decoder-only partition configuration;
repository decoder harnesses use
`DECODER_CONFIG_FLAGS='-DPATCH_IMAGE_BASE=0u -DPATCH_IMAGE_CAPACITY=67108864u'`.

CLI:

```sh
tool=$(make -s host-tool-path)
"$tool" [--encode] <from_image> <to_image> <patch>
"$tool" --decode <image> <patch>
"$tool" --help
```

`ultrapatch` is intentionally a unified host tool: `--encode` is the default
mode, and `--decode` applies a patch through the same reference decoder backend
used for encoder self-verification. The device integration contract remains
header-only and does not require the host CLI. Repository builds isolate the
tool under `.build/<profile-id>/`; `make host-tool-path` selects the compiler and
flags requested on that Make invocation. The encoder takes image file
paths, not image directories; if a same-basename `.elf` sidecar exists next to
an image, it is used for range extraction. Running `ultrapatch` without
arguments prints usage and exits nonzero; `--help` and `-h` print the same usage
text and exit successfully.

Host encode/decode outputs are published through a temporary file in the
destination directory followed by the native OS `rename()` operation. Reported
write, sync, close, or rename errors fail the command and preserve an existing
destination. The host tool intentionally relies on the platform's normal
filesystem semantics after a successful rename; UltraPatch does not add a
second portability or durability protocol around them. This is a host CLI
policy and is separate from the embedded decoder's full-reflash recovery rule.

Full release gate:

```sh
make check-build-profile
make gate
```

For release notes and artifact provenance, use `docs/release-checklist.md`.

The binary corpora used by the release gate are tracked under `test-bench/`.
`test-bench/images` and `test-bench/fixtures` provide the 16 home matrix images
and the two one-face fixtures; `test-bench/foreign` provides the second
Cortex-M0+ lineage. `test-bench/release-inventory.tsv` is the canonical ordered
membership; the asset, per-pair size, corpus-wire, and golden manifests are
cross-validated against it by `make check-release-inventory`. The root
`Makefile` uses those paths by default. For a non-release measurement, override
`IMAGES`, `FIXTURES`, and `FOREIGN`, set `CORPUS_INVENTORY=""` to discover the
supplied directories, and provide or disable the matching manifests. The
`check-assets` leg of `make gate` runs concurrently with the matrix, so a stale,
partial, or mutually inconsistent corpus still fails the gate deterministically.
`make check-build-profile` separately proves concurrent GCC, Clang, and
alternate-flag builds cannot collide. `make gate` validates the checked release
profile, then also runs `make check-decoder-contract` (source-header-set and
generated-single-header/no-globals/no-heap
decoder API contract), `make check-malformed` (a deterministic reject-regression
suite for malformed envelopes, truncations, corrupt bodies, and wrong-base
application), `make check-edge` (synthetic edge-input pairs: empty/tiny/equal/
random/text/page-boundary images), `make check-golden` (pinned sha256 of eight
representative blobs — any wire drift fails the gate), `make check-degrade`,
the ARM size/divide/stack gates, the full 256-pair home corpus matrix, and the
34 foreign pair-directions — all legs run concurrently, ~63 s cold / ~56 s warm on the
reference machine.
qemu-based decode validation was removed permanently (owner decision,
2026-07-03): too slow for its marginal value — a one-time 260-pair qemu-arm
study found zero host-vs-ARM divergence, and the ARM cross-build gate still
compiles the real Thumb-1 decoder every cycle.

Create a deterministic standalone corpus bundle, if needed, with:

```sh
scripts/pack_corpus.sh artifacts/a1-corpus.tar.gz
```

The packer takes its file list from the verified release manifests, includes
the inventory plus size/wire/golden baselines, validates the staged archive,
and only then publishes the archive and checksum through same-directory
renames. A failure before publication preserves both existing outputs; an
interruption between the two renames is detected by the checksum mismatch.

CI verifies the tracked corpus via the `check-assets` leg of the same
`make gate` command.

Device integration contract:

- Include either generated `artifacts/patch_apply_single.h`, or
  `src/patch_apply.h` with its two support headers beside it on the include
  path, in one update module. Allocate a caller-owned `PatchApply` state object.
- Define `PATCH_IMAGE_BASE` as the aligned absolute device address of the
  patchable image (`0` in repository host tests), and define the page-aligned
  `PATCH_IMAGE_CAPACITY` as the complete physical patch partition size from that
  base.
- Provide exactly two flash primitives: `flash_read(uint32_t)` and
  `flash_write_page(uint32_t, const uint8_t[OUTROW])`. Both receive absolute
  device addresses; one page-write call erases and fully programs the page. The
  write primitive deliberately returns `void`: a driver may verify/retry
  internally, but any unrecoverable failure after a write requires full
  external reflash; the final CRC detects integrity and does not provide
  recovery.
- Authenticate the update, then run the WHOLE blob through
  `patch_apply_run(&state, callback, ctx)` — the callback serves blob bytes (it may
  block internally), returning `PATCH_PULL_BYTE` for a byte or `PATCH_PULL_END`
  to stop. The `PatchApplyResult` return is the verdict (`PATCH_APPLY_DONE == 0`;
  errors are nonzero). The decoder parses the envelope and verifies both CRC
  gates itself; there is no coroutine/fiber.
- Do not run concurrent decodes against the same flash image; see
  `docs/device-integration.md` before wiring it into a bootloader.

The decoder rejects an envelope whose page-rounded image span exceeds
`PATCH_IMAGE_CAPACITY` before its first flash read. This is a pre-apply partition
guard, not recovery: after the first page write, any later decoder, transport,
power, or flash failure has no rollback path and requires a full external
reflash. Earlier detection of such post-write failures cannot recover the image
and is therefore not a decoder design goal.

## License

Ultrapatcher is MIT licensed, except where a vendored dependency states
otherwise.

Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>.

Vendored files under `vendor/libdivsufsort/` keep their upstream license
notices. `src/enc_bsdiff.c` includes an attribution note for the detools
Python implementation that informed the ARM Cortex-M scanner reimplementation. See
`THIRD_PARTY_NOTICES.md`.
