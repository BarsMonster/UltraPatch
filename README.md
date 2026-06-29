# Ultrapatcher

Final A1 firmware patcher for the Sensor Watch target.

Production code lives at the repository root:

- `patch_apply/patch_apply.h`: reusable header-only streaming in-place decoder.
- `patch_apply/demo_patch.c`: host demo/gate wrapper used by `hy_dec`, including
  the host NVM emulator.
- `patch_generate/patch_generate.c`: host-side C encoder.
- `vendor/libdivsufsort/`: vendored C suffix sorter used by the encoder.
- `common/rc_models.h`: shared wire-model constants and packed model helpers.

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
`make gate` also runs `make check-malformed`, a deterministic reject-regression
suite for malformed envelopes, truncations, appended garbage, and wrong-base
application.

Create a deterministic standalone corpus bundle, if needed, with:

```sh
scripts/pack_corpus.sh artifacts/a1-corpus.tar.gz
```

CI verifies the tracked corpus with `make check-assets` before the same
`make gate` command runs.

Device integration contract:

- Include `patch_apply/patch_apply.h` in exactly one update module.
- Provide `flash_read(uint32_t)`, `flash_write(uint32_t, uint8_t)`, and
  `uint32_t g_image_span`.
- Parse and authenticate the patch envelope outside the decoder, then call
  `patch_apply_set`, `patch_apply_init`, `patch_apply_push`, and
  `patch_apply_finish`.
- The decoder is single-instance and non-reentrant; see
  `docs/device-integration.md` before wiring it into a bootloader.

## License

Ultrapatcher is MIT licensed.

Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>.

Vendored files under `vendor/libdivsufsort/` keep their upstream
license notices. See `THIRD_PARTY_NOTICES.md`.
