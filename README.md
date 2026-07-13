# UltraPatch

UltraPatch creates and applies compact in-place firmware patches for the Sensor
Watch target. It ships two artifacts:

- `ultrapatch`, a host CLI whose default mode generates patches. Its `--decode`
  mode is the reference decoder used for debugging and encoder self-checks.
- A header-only device decoder rooted at `src/patch_apply.h`. Install
  `patch_apply.h`, `patch_config.h`, and `rc_models.h` together; application code
  includes only `patch_apply.h`.

The host encoder lives under `src/`, and its suffix sorter is vendored under
`vendor/libdivsufsort/`.

## Build and CLI

```sh
make
make check

tool=$(make -s host-tool-path)
"$tool" [--encode] <from_image> <to_image> <patch>
"$tool" --decode <image> <patch>
"$tool" --help
```

The encoder accepts image files, not directories. If a same-basename `.elf`
sidecar is present beside an image, the encoder uses it for range extraction.
The default host executable is `.build/ultrapatch`; always use
`make host-tool-path` to obtain its exact path instead of assuming a root-level
`./ultrapatch`. Set `BUILD_DIR` to a private directory when parallel builds or
measurements need isolation.

See [install.md](install.md) for packages and common commands.

## Device decoder

Define the patch partition through `PATCH_IMAGE_BASE` and
`PATCH_IMAGE_CAPACITY`, provide the required page-flash functions, allocate a
caller-owned `PatchApply`, and invoke `patch_apply_run()`. The decoder uses no
heap or global static state.

Read [the device integration contract](docs/device-integration.md) before
integrating it into a bootloader. It defines the flash, memory, callback,
authentication, watchdog, concurrency, and recovery requirements.

### Decoder configuration

`PATCH_IMAGE_BASE` and `PATCH_IMAGE_CAPACITY` are required and describe the
physical patch partition. Define them before including `patch_apply.h`.

The decoder uses the C library's `memmove` by default. It deliberately uses
that same primitive for non-overlapping model copies as well as overlapping
shifts, avoiding the flash cost of linking both `memcpy` and `memmove`. Leave
`HAND_ROLLED_MEMMOVE` undefined when the final firmware already links
`memmove`. If it does not, define `HAND_ROLLED_MEMMOVE` before including
`patch_apply.h` to select the decoder's smaller private backward-copy loop.
The option changes code generation only, not the wire format or RAM layout;
the private byte loop may be slower, so compare the final linked image and
update latency with the intended compiler and firmware.

## Verification and release

```sh
make gate
make check-decoder-sanitize
make check-encoder-sanitize
make check-clang
```

The authoritative release procedure and required evidence are in
[the release checklist](docs/release-checklist.md). Corpus membership and
provenance are documented in [test-bench/README.md](test-bench/README.md).

## License

UltraPatch is MIT licensed except where a vendored dependency states otherwise.
Vendored notices and the `enc_bsdiff.c` attribution are collected in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>.
