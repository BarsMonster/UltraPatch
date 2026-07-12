# Ultrapatcher

Final A1 firmware patcher for the Sensor Watch target.

Production code lives under `src/`, with third-party code under `vendor/`:

- `src/patch_apply.h`: reusable header-only streaming in-place decoder entry point;
  ship it beside `src/rc_models.h` and `src/patch_config.h`. Integration code
  includes only `patch_apply.h`; that entrypoint uses normal local includes and
  is not a self-contained amalgamation.
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

To integrate the decoder, install `patch_apply.h`, `rc_models.h`, and
`patch_config.h` in the same include directory, then include only:

```c
#include "patch_apply.h"
```

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

Full development gate:

```sh
make check-build-profile
make gate
```

The matching local release preflight additionally binds those checks,
sanitizers, and the required Clang leg to a fresh archive of one clean `main`
commit:

```sh
/usr/bin/python3 scripts/release_gate.py
```

The complete release workflow, required evidence, profile refresh procedure,
and artifact provenance are authoritative in
[`docs/release-checklist.md`](docs/release-checklist.md).

The tracked corpus topology and hashes are described in
[`test-bench/README.md`](test-bench/README.md). `make gate` is authoritative for
the release-profile validation and complete concurrent correctness suite;
`make check-build-profile` separately validates build isolation.

Device integration contract:

- Install `patch_apply.h`, `rc_models.h`, and `patch_config.h` together, include
  only `patch_apply.h` from one update module, and allocate a caller-owned
  `PatchApply` state object.
- Define `PATCH_IMAGE_BASE` as the aligned absolute device address of the
  patchable image (`0` in repository host tests), and define the page-aligned
  `PATCH_IMAGE_CAPACITY` as the complete physical patch partition size from that
  base.
- Provide `flash_read(uint32_t)` and
  `flash_write_page(uint32_t, const uint8_t[OUTROW])`; both receive absolute
  addresses and a page write erases and fully programs one page.
- Authenticate the update, then run the whole blob through
  `patch_apply_run(&state, callback, ctx)` — the callback serves blob bytes (it may
  block internally), returning `PATCH_PULL_BYTE` for a byte or `PATCH_PULL_END`
  to stop. Treat `PATCH_APPLY_DONE` as success and every other result as failure.
- Do not decode concurrently against one flash image. Read
  [`docs/device-integration.md`](docs/device-integration.md) for the complete
  flash, memory, wire-configuration, authentication, and recovery contract
  before bootloader integration.

## License

Ultrapatcher is MIT licensed, except where a vendored dependency states
otherwise.

Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>.

Vendored files under `vendor/libdivsufsort/` keep their upstream license
notices. `src/enc_bsdiff.c` includes an attribution note for the detools
Python implementation that informed the ARM Cortex-M scanner reimplementation. See
`THIRD_PARTY_NOTICES.md`.
