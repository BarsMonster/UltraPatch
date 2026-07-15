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

## How UltraPatch works

UltraPatch moves expensive analysis to the host so the device can apply a patch
in place with bounded memory. The encoder tries several source/target match
plans and chooses a monotonic write direction. Growing images normally apply
from high addresses to low addresses; shrinking images apply from low to high.
This keeps the source of ordinary shifted copies ahead of the overwrite
frontier, where the original bytes are still intact.

The decoder holds two 256-byte output pages in RAM. A resident page contains
the new target bytes, while its physical flash page is not programmed yet and
still contains the old source bytes. Consequently the current page and the
previous page in write order provide both views at once: output matches read
the new bytes from RAM, while source matches can still read the old bytes from
flash. Older pages are programmed once, in order, when their buffer slot is
reused.

The host identifies the remaining source matches that would read an old byte
after its flash page has been programmed. It splits only those unsafe runs out
of the source copy and transports their exact target bytes instead. These bytes
are not necessarily stored literally in the patch: they join the common
content stream and can use 1 KiB LZ backreferences, matches against already
produced output, and adaptive entropy coding. The encoder derives
relocation-aware source deltas from the selected edit operations; together with
the choice among competing plans and the two-page delayed-write window, this
leaves few bytes that need special treatment, so the decoder does not need a
general old-byte journal.

Every generated patch is applied by the production decoder on the host before
it is accepted. On the device, source CRC is checked before the first write,
each changed flash page is erased and programmed at most once, and target CRC
is checked after the final buffered pages are committed.

## Install

On Debian or Ubuntu, install the host compiler, Cortex-M0+ toolchain, and build helpers:

```sh
apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
  binutils-arm-none-eabi \
  gcc \
  gcc-arm-none-eabi \
  git \
  libc6-dev \
  libnewlib-dev \
  make \
  python3
```

Build from the repository root with `make`. The default host executable is
`.build/ultrapatch`; use `make -s host-tool-path` to obtain the exact path selected by the current
Make arguments. Do not assume a root-level `./ultrapatch`. Give parallel compiler or measurement
runs distinct `BUILD_DIR` values.

## CLI

```sh
tool=$(make -s host-tool-path)
"$tool" [--encode] <from_image> <to_image> <patch>
"$tool" --decode <image> <patch>
"$tool" --help
```

The encoder accepts raw firmware image files, not directories, and treats every
input byte as part of the image. It neither parses ELF files nor discovers
same-basename sidecars. The release corpus is a frozen set of committed raw
binaries under `test-bench/`; the gate reads those files directly rather than
generating test inputs during the build. This input simplification is host-only;
it does not change the decoder interface or patch wire format.

## Device decoder

This section is the authoritative device integration contract.

### Artifact and ownership

Install `patch_apply.h`, `patch_config.h`, and `rc_models.h` in one include
directory. Integration code includes only `patch_apply.h`; it uses normal local
includes for the other two files and is not an amalgamation.

Include the decoder from one update translation unit to avoid duplicate
header-local code. Allocate one integrator-owned `PatchApply` for each run. The
decoder has no heap, global static state, coroutine, or private stack; its state
lives in that object and its calls use the caller's stack. Do not run two
decodes concurrently against one flash image.

`patch_config.h` fixes the shared encoder/decoder wire, page, and resource
constants, including the unsigned 8-bit `PATCH_WIRE_VERSION`. Its nonzero value
domain-separates incompatible revisions at the pre-write source CRC check.
Production builds must not override these constants. `PATCH_IMAGE_BASE` and
`PATCH_IMAGE_CAPACITY` are the decoder-only deployment-geometry exceptions.
`HAND_ROLLED_MEMMOVE` and `CRC32_DECODE(start,end)` are decoder-only, non-wire
configuration hooks described below. The installed wire mode targets
Cortex-M0/ARMv6-M. `CORTEX_M4` is a reserved wire-selection macro and defining
it is rejected; compiling the same C for another CPU does not select a
different wire.

### Decoder configuration

The default decoder computes reflected IEEE CRC-32 with a tableless internal
routine. A platform CRC library or hardware peripheral may replace it by
defining `CRC32_DECODE(start,end)` before including any UltraPatch header.
`start` and `end` are image-relative offsets describing `[start,end)`; the
result must match zlib's reflected IEEE CRC-32 (polynomial `0xedb88320`, initial
and final XOR `0xffffffff`) and must observe flash writes completed during the
apply. For example:

```c
#include <stdint.h>
uint32_t platform_image_crc32(uint32_t start, uint32_t end);
#define CRC32_DECODE(start,end) platform_image_crc32((start),(end))
#include "patch_apply.h"
```

The decoder uses the C library's `memmove` by default for both overlapping
shifts and non-overlapping model copies, avoiding a second `memcpy` primitive.
Leave `HAND_ROLLED_MEMMOVE` undefined when the final firmware already links
`memmove`. Otherwise, define it before including `patch_apply.h` to select the
smaller private backward-copy loop. This changes code generation only, not the
wire or RAM layout; compare final image size and update latency with the
intended compiler and firmware.

### Partition and flash contract

Define `PATCH_IMAGE_BASE` as the `OUTROW`-aligned absolute address of the
patchable image and `PATCH_IMAGE_CAPACITY` as the nonzero physical capacity from
that base, also aligned to `OUTROW`. The complete range must fit in `uint32_t`.
The decoder rejects a page-rounded logical image span larger than this partition
before any image-flash scan or write.

Provide these functions:

```c
uint8_t flash_read(uint32_t absolute_addr);
void flash_write_page(uint32_t absolute_page_addr,
                      const uint8_t page[OUTROW]);
```

Both arguments are absolute addresses. `flash_read` returns the current byte
and must immediately observe prior writes. `flash_write_page` synchronously
erases and fully programs one aligned `OUTROW` page from the complete supplied
buffer. The decoder preloads the whole final page, including bytes beyond the
logical image end. If the hardware erase unit is larger than `OUTROW`, the
driver must preserve bytes outside the supplied page.

The write function returns no status. Verify or retry hardware writes inside
the driver while the failure is actionable. A valid patch writes each output
page at most once; once a page has been changed, the decoder cannot reconstruct
the original image.

### Applying a patch

The decoder owns envelope parsing, direction, sizes, and both CRC checks. The
integrator supplies the complete authenticated blob through a `PatchPull`
callback:

```c
int next_byte(void *ctx, uint8_t *out);

PatchApply state; /* Caller-owned; use reserved storage if the stack is tight. */
PatchApplyResult result = patch_apply_run(&state, next_byte, &my_ctx);
if (result != PATCH_APPLY_DONE) {
    /* Inspect patch_apply_reject(), then recover by externally reflashing. */
}
```

Return `PATCH_PULL_BYTE` after storing one byte in `*out`. Return
`PATCH_PULL_END` to abort or report a truncated source; negative and other
values also abort. The callback may block internally. The compressed body has
a counted length, so success does not require transport EOF after its last byte.
Trailing bytes are not consumed by the decoder and belong to the authenticated
outer framing or session.

`patch_apply_run` is synchronous and returns `PATCH_APPLY_DONE` only after the
old-image CRC, apply, and final-image CRC succeed. Every other result is
`PATCH_APPLY_ERROR`. Returning `PATCH_PULL_END` causes a bounded error path, but
a patch cannot be resumed.

The call blocks through both full-image CRC scans and the apply, with no internal
watchdog hook. Service the watchdog inside `flash_read`, `flash_write_page`, the
byte callback, and any platform `CRC32_DECODE` implementation.

CRC32 detects accidental corruption; it does not authenticate an update.
Authenticate the complete manifest and blob, and enforce target and anti-rollback
policy, before calling the decoder. The old-image CRC is checked before the
first write. The final-image CRC is necessarily checked after apply, when flash
may already have been changed.

### Memory budget

The repository release gate has an absolute 12,288-byte `.bss` ceiling and
ratchets the reference static-wrapper flash/state plus the worst supported call
graph. It measures both library and hand-rolled copy modes and reports the
current values in `make gate` output.

Flash remains the image backing store. `PatchApply` stages at most
`OUTROW_DEPTH` dirty output pages in RAM and never holds a full-image buffer.

The stack limit includes the repository integration wrapper but excludes
`PatchApply` storage, the integrator's flash functions, pull callback, platform
CRC override, and bounded toolchain leaves. Place the state object in reserved
static/noinit storage if necessary, then add those external frames, interrupt
nesting, and RTOS frames to the product budget. A different wrapper or
toolchain must be measured in the final firmware build.

### Failure and recovery

`PATCH_APPLY_DONE` means the target image is present and both CRCs were verified.
After `PATCH_APPLY_ERROR`, the image may be unchanged, partially written, or
fully written but unverified; perform a full external reflash. Do not retry the
patch.

The decoder has no persistent progress marker, rollback, checkpoint, or resume
protocol. A reset or power loss during apply also requires a full external
reflash.

## Verification and release

This section is the authoritative release procedure for the host encoder,
header-only decoder, measured corpus, and device footprint. Product signing,
anti-rollback policy, bootloader recovery, and real flash-driver validation
remain integration responsibilities.

### Required inputs

- Use a clean `main` checkout at the exact release commit.
- Keep the frozen static `watch.bin` corpus under `test-bench/images`,
  `test-bench/fixtures`, and `test-bench/foreign` intact. The gate reads all 36
  binaries directly; it does not derive or materialize corpus inputs during the
  release build.
- Install the packages listed under [Install](#install). Exact compiler and
  system-library identities are not release criteria; the measured size and
  memory outcomes are.

### Verification

Run and retain the complete output:

```sh
make gate
```

Do not release unless it succeeds. Review all reported outcomes: 290/290
self-verifying corpus encodes, the complete corpus total, the real one-face grow
and revert sizes, reference-static-wrapper ARM flash/state, and worst supported
decoder stack.

Every successful encoder call has already applied the emitted patch through the
production decoder, required the exact target and complete blob consumption,
and enforced NVM write safety. The corpus gate records a size only after that
self-verification succeeds.

### Artifacts and evidence

The source artifact is the Git commit. The device decoder artifact is the
three-file header set `src/patch_apply.h`, `src/patch_config.h`, and
`src/rc_models.h`; install them together and include only `patch_apply.h`. The
host artifact is the path printed by `make -s host-tool-path`.

Release notes must include the commit SHA and complete `make gate` output,
including the real one-face grow/revert metrics. Include this README in the
handoff.

## License

UltraPatch-authored source and documentation are licensed under the repository's
`LICENSE`.

Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>.

### libdivsufsort

`vendor/libdivsufsort/` is vendored from libdivsufsort by Yuta Mori and retains
its upstream MIT-style license headers in each vendored source file.

### CircuitPython release corpus

The committed binaries under `test-bench/foreign/` are derived from official
Adafruit CircuitPython releases for the `feather_m0_express` board. They are
test data, not UltraPatch-authored firmware.

The raw-binary releases are `2.2.0`, `2.2.1`, `2.2.2`, `2.2.3`, `2.2.4`,
`2.3.0`, `2.3.1`, `3.0.0`, `3.0.1`, `3.0.2`, and `3.0.3`. The UF2-derived
releases are `10.0.0`, `10.0.1`, `10.0.2`, `10.0.3`, `10.1.1`, `10.1.2`, and
`10.1.3`; they were unpacked at application base `0x2000` to match the raw-bin
layout. The exact committed files and their Git history are the frozen corpus
provenance record.

Official artifacts came from the Adafruit CircuitPython release listings for
[older raw binaries](https://adafruit-circuit-python.s3.amazonaws.com/index.html?prefix=bin/feather_m0_express/en_US/OLD/)
and [current UF2 releases](https://adafruit-circuit-python.s3.amazonaws.com/index.html?prefix=bin/feather_m0_express/en_US/).
Corresponding source and per-file notices are available from the release tags
and history in the [Adafruit CircuitPython repository](https://github.com/adafruit/circuitpython).
The available 2.x/3.x release tags carry this root notice:

> Copyright (c) 2013, 2014 Damien P. George

The available 10.x release tags carry this updated root notice:

> Copyright (c) 2013-2025 Damien P. George

Both grant the following MIT license:

> Permission is hereby granted, free of charge, to any person obtaining a copy
> of this software and associated documentation files (the "Software"), to deal
> in the Software without restriction, including without limitation the rights
> to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
> copies of the Software, and to permit persons to whom the Software is
> furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in
> all copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
> IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
> FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
> AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
> LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
> OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
> THE SOFTWARE.

Individual CircuitPython source files and bundled components may name
additional copyright holders or licenses; the corresponding source headers and
license files in the upstream release history are authoritative for those
components.
