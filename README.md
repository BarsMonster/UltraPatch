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

CLI:

```sh
./ultrapatch [--encode] <from_image> <to_image> <patch>
./ultrapatch --decode <image> <patch>
./ultrapatch --help
```

`ultrapatch` is intentionally a unified host tool: `--encode` is the default
mode, and `--decode` applies a patch through the same reference decoder backend
used for encoder self-verification. The device integration contract remains
header-only and does not require the host CLI. The encoder takes image file
paths, not image directories; if a same-basename `.elf` sidecar exists next to
an image, it is used for range extraction. Running `ultrapatch` without
arguments prints usage and exits nonzero; `--help` and `-h` print the same usage
text and exit successfully.

Full release gate:

```sh
make gate
```

For release notes and artifact provenance, use `docs/release-checklist.md`.

The binary corpora used by the release gate are tracked under `test-bench/`.
`test-bench/images` and `test-bench/fixtures` provide the 16 home matrix images
and the two one-face fixtures; `test-bench/foreign` provides the second
Cortex-M0+ lineage. The root `Makefile` uses those paths by default; override
them with `IMAGES=...`, `FIXTURES=...`, `FOREIGN=...`, and matching manifest
variables when running checks elsewhere. The expected contents are pinned by
`test-bench/corpus.sha256` and `test-bench/foreign.sha256`; the `check-assets`
leg of `make gate` runs concurrently with the matrix, so a stale or partial
corpus still fails the gate deterministically.
`make gate` also runs `make check-decoder-contract` (source-header-set and
generated-single-header/no-globals/no-heap
decoder API contract), `make check-malformed` (a deterministic reject-regression
suite for malformed envelopes, truncations, corrupt bodies, and wrong-base
application), `make check-edge` (synthetic edge-input pairs: empty/tiny/equal/
random/text/page-boundary images), `make check-golden` (pinned sha256 of eight
representative blobs — any wire drift fails the gate), `make check-degrade`,
the ARM size/divide/stack gates, the full 256-pair home corpus matrix, and the
34 foreign pair-directions — all legs run concurrently, ~34 s wall on the
reference machine.
qemu-based decode validation was removed permanently (owner decision,
2026-07-03): too slow for its marginal value — a one-time 260-pair qemu-arm
study found zero host-vs-ARM divergence, and the ARM cross-build gate still
compiles the real Thumb-1 decoder every cycle.

Create a deterministic standalone corpus bundle, if needed, with:

```sh
scripts/pack_corpus.sh artifacts/a1-corpus.tar.gz
```

CI verifies the tracked corpus via the `check-assets` leg of the same
`make gate` command.

Device integration contract:

- Include either generated `artifacts/patch_apply_single.h`, or
  `src/patch_apply.h` with its two support headers beside it on the include
  path, in one update module. Allocate a caller-owned `PatchApply` state object.
- Provide exactly two flash primitives: `flash_read(uint32_t)` and
  `flash_write(uint32_t, uint8_t)`.
- Authenticate the update, then run the WHOLE blob through
  `patch_apply_run(&state, callback, ctx)` — the callback serves blob bytes (it may
  block internally) and the return is the verdict. The decoder parses the
  envelope and verifies both CRC gates itself; there is no coroutine/fiber.
- Do not run concurrent decodes against the same flash image; see
  `docs/device-integration.md` before wiring it into a bootloader.

## License

Ultrapatcher is MIT licensed, except where a vendored dependency states
otherwise.

Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>.

Vendored files under `vendor/libdivsufsort/` keep their upstream license
notices. `src/enc_bsdiff.c` includes an attribution note for the detools
Python implementation that informed the ARM Cortex-M scanner reimplementation. See
`THIRD_PARTY_NOTICES.md`.
