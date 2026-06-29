# Ultrapatcher

Final A1 firmware patcher for the Sensor Watch target.

Production code lives at the repository root:

- `patch_apply/patch_apply.h`: reusable header-only streaming in-place decoder.
- `patch_apply/demo_patch.c`: host demo/gate wrapper used by `hy_dec`, including
  the host NVM emulator.
- `patch_generate/patch_generate.c`: host-side C encoder.
- `patch_generate/libdivsufsort/`: vendored C suffix sorter used by the encoder.
- `common/rc_models.h`: shared wire-model constants and packed model helpers.

Build and smoke-test:

```sh
make
make check
```

The local binary corpora live outside Git under `test-bench/images` and
`test-bench/fixtures`. The root `Makefile` uses those paths by default; override
them with `IMAGES=...` and `FIXTURES=...` when running checks elsewhere.
