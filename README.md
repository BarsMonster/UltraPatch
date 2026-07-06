# Ultrapatcher

Final A1 firmware patcher for the Sensor Watch target.

Production code lives under `src/`, with third-party code under `vendor/`:

- `src/patch_apply.h`: reusable self-contained header-only streaming in-place decoder.
- `src/patch_apply_push_adapter.h`: optional SPSC ring adapter for event-driven
  (ISR push) producers; not part of the device decoder artifact.
- `src/patch_apply_demo.c`: host demo/gate wrapper used by `hy_dec`,
  including the host NVM emulator.
- `src/patch_generate.c`: host-side C encoder. It is a single translation unit
  assembled as a thin umbrella: a shared prologue (typedefs + explicit `EncCtx`)
  followed by an ordered `#include` of the `src/enc_*.inc` subsystem modules
  (`enc_util`, `enc_elf`, `enc_bsdiff`, `enc_field`, `enc_rc`, `enc_lz`,
  `enc_emit`, `enc_plan`, `enc_cli`). The single-TU shape preserves whole-program
  optimization and the byte-exact wire; the modules are not standalone TUs and
  must stay in include order. `test/model_diff.c` reaches the file-static models
  by `#include`-ing `patch_generate.c` directly.
- `src/patch_selfcheck.c`: reference-decoder self-verification built into
  `hy_enc` — every emitted patch is proven to apply before it is written.
- `vendor/libdivsufsort/`: vendored C suffix sorter used by the encoder.
- `src/rc_models.h`: encoder-side wire-model constants and packed model helpers,
  mirrored inside the self-contained decoder header.
- `src/patch_config.h`: encoder-side build-time configuration defaults and mirror knobs,
  also mirrored inside the self-contained decoder header.

Build and smoke-test:

```sh
make
make check
```

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
`make check-models`, the ARM size/divide/stack gates, and the full 256-pair
corpus matrix — all legs run concurrently, ~34 s wall on the reference machine.
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

- Include only `src/patch_apply.h` in an update module and allocate a caller-owned
  `PatchApply` state object.
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
