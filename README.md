# Ultrapatcher

Final A1 firmware patcher for the Sensor Watch target.

Production code lives in `v3/nvm/hybrid12k/c`:

- `rc_v3_enc.c`: host-side C encoder.
- `rc_v3.c`: streaming in-place C decoder.
- `flash_nvm.c`: host NVM emulator used by `hy_dec`.
- `libdivsufsort/`: vendored C suffix sorter used by the host encoder.

Build and smoke-test:

```sh
cd v3/nvm/hybrid12k/c
make
make check
```

The local binary corpora live outside Git under `test-bench/images` and
`test-bench/fixtures`; `v3/nvm/hybrid12k` keeps lightweight symlinks to those
directories for verification runs.
