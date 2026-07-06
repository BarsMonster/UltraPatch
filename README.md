# Ultrapatcher

Final A1 firmware patcher for the Sensor Watch target.

Production code lives under `src/`, with third-party code under `vendor/`:

- `src/patch_apply.h`: reusable header-only streaming in-place decoder entry point;
  ship it with `src/rc_models.h` and `src/patch_config.h`.
- `src/patch_apply_push_adapter.h`: optional SPSC ring adapter for event-driven
  (ISR push) producers; not part of the device decoder artifact.
- `src/patch_host_backend.c`: shared host reference-decoder backend used by
  `ultrapatch --decode` and encoder self-verification, including the host NVM
  emulator.
- `src/patch_apply_demo.c`: standalone compatibility wrapper for building a
  decode-only host harness.
- `src/patch_generate.c`: host-side `ultrapatch` CLI entry point. The encoder is
  the default mode and is built from normal internal `src/enc_*.c` modules (`enc_util`,
  `enc_elf`, `enc_bsdiff`, `enc_field`, `enc_rc`, `enc_lz`, `enc_emit`,
  `enc_plan`). Encoder complexity is deliberately host-side.
- `src/patch_selfcheck.c`: compatibility wrapper for direct builds of the
  historical selfcheck source.
- `vendor/libdivsufsort/`: vendored C suffix sorter used by the encoder.
- `src/rc_models.h`: shared wire-model constants and packed model helpers.
- `src/patch_config.h`: shared build-time configuration defaults and mirror knobs.

Build and smoke-test:

```sh
make
make check
```

CLI:

```sh
./ultrapatch [--encode] <from_image> <to_image> <patch>
./ultrapatch --decode [--byte-mode] <image> <patch>
./ultrapatch --help
```

`--encode` is the default mode. The encoder takes image file paths, not image
directories; if a same-basename `.elf` sidecar exists next to an image, it is
used for range extraction. Running `ultrapatch` without arguments prints usage
and exits nonzero; `--help` and `-h` print the same usage text and exit
successfully.

Full release gate:

```sh
make gate
```

For release notes and artifact provenance, use `docs/release-checklist.md`.

The binary corpora used by the release gate are tracked under `test-bench/images`
and `test-bench/fixtures`: 16 matrix images plus the two one-face fixtures. The
root `Makefile` uses those paths by default; override them with `IMAGES=...`,
`FIXTURES=...`, and a matching `CORPUS_MANIFEST=...` when running checks
elsewhere.
The expected corpus contents are pinned by `test-bench/corpus.sha256`; `make gate`
runs `make check-assets` before the matrix so a stale or partial corpus fails
early.
`make gate` also runs `make check-decoder-contract` (single-header/no-globals/no-heap
decoder API contract), `make check-malformed` (a deterministic reject-regression
suite for malformed envelopes, truncations, corrupt bodies, and wrong-base
application), `make check-edge` (synthetic edge-input pairs: empty/tiny/equal/
random/text/page-boundary images), `make check-golden` (pinned sha256 of eight
representative blobs — any wire drift fails the gate), `make check-degrade`,
the ARM size/divide/stack gates, and the full 256-pair corpus matrix — all legs
run concurrently, ~34 s wall on the reference machine.
qemu-based decode validation was removed permanently (owner decision,
2026-07-03): too slow for its marginal value — a one-time 260-pair qemu-arm
study found zero host-vs-ARM divergence, and the ARM cross-build gate still
compiles the real Thumb-1 decoder every cycle.

Create a deterministic standalone corpus bundle, if needed, with:

```sh
scripts/pack_corpus.sh artifacts/a1-corpus.tar.gz
```

CI verifies the tracked corpus with `make check-assets` before the same
`make gate` command runs.

Device integration contract:

- Include `src/patch_apply.h` in an update module, keep its two support headers
  beside it on the include path, and allocate a caller-owned `PatchApply` state object.
- Provide exactly two flash primitives: `flash_read(uint32_t)` and
  `flash_write(uint32_t, uint8_t)`.
- Authenticate the update, then run the WHOLE blob through
  `patch_apply_run(&state, callback, ctx)` — the callback serves blob bytes (it may
  block internally) and the return is the verdict. The decoder parses the
  envelope and verifies both CRC gates itself; there is no coroutine/fiber.
  Event-driven producers adapt via `src/patch_apply_push_adapter.h`.
- Do not run concurrent decodes against the same flash image; see
  `docs/device-integration.md` before wiring it into a bootloader.

## License

Ultrapatcher is MIT licensed, except where a vendored dependency states
otherwise.

Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>.

Vendored files under `vendor/libdivsufsort/` keep their upstream license
notices. `src/arm_cortex_m4.c` includes an attribution note for the detools
Python implementation that informed the C reimplementation. See
`THIRD_PARTY_NOTICES.md`.
