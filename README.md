# UltraPatch

UltraPatch creates compact, in-place firmware patches for Sensor Watch. It
provides:

- `ultrapatch`, a host CLI that creates patches and can apply them for testing.
- A header-only device decoder rooted at `src/patch_apply.h`.

The host does the expensive analysis. The device decoder uses bounded RAM, no
heap, and no internal global state.

## Build and use

On Debian or Ubuntu, install the build dependencies:

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

Build the CLI and ask Make for its path:

```sh
make
tool=$(make -s host-tool-path)
```

The default path is `.build/ultrapatch`, but build arguments can change it.
Do not assume a root-level `./ultrapatch`. Use a different `BUILD_DIR` for
each parallel build or measurement run.

```sh
"$tool" [--encode] <old.bin> <new.bin> <patch>
"$tool" --decode <image.bin> <patch>
"$tool" --help
```

`--encode` is optional because encoding is the default. `--decode` applies
the patch to `image.bin` in place.

For example:

```sh
"$tool" old.bin new.bin update.patch
cp old.bin test.bin
"$tool" --decode test.bin update.patch
cmp test.bin new.bin
```

Encoder inputs are raw firmware binaries. Every byte belongs to the image;
UltraPatch does not parse ELF files or look for sidecar files.

## Device integration

### Integration steps

1. Put `patch_apply.h`, `patch_config.h`, and `rc_models.h` in one include
   directory. Include `patch_apply.h` from one update translation unit.
2. Define the patchable image range before including the header:

   ```c
   #define PATCH_IMAGE_BASE PLATFORM_APP_BASE
   #define PATCH_IMAGE_CAPACITY PLATFORM_APP_CAPACITY
   #include "patch_apply.h"
   ```

3. Provide the flash functions:

   ```c
   uint8_t flash_read(uint32_t absolute_addr);
   void flash_write_page(uint32_t absolute_page_addr,
                         const uint8_t page[OUTROW]);
   ```

4. Provide a byte callback, allocate the decoder state, and run the patch:

   ```c
   int next_byte(void *ctx, uint8_t *out);

   PatchApply state;
   PatchApplyResult result = patch_apply_run(&state, next_byte, &my_ctx);
   if (result != PATCH_APPLY_DONE) {
       /* Inspect patch_apply_reject(), then recover with a full reflash. */
   }
   ```

`PatchApply` is caller-owned. Use a separate object for each run, and do not
run two decoders concurrently against the same flash image.

### Flash contract

`PATCH_IMAGE_BASE` is the absolute start of the patchable image.
`PATCH_IMAGE_CAPACITY` is the physical capacity from that address. Both must
be aligned to `OUTROW`, the capacity must be nonzero, and the complete range
must fit in `uint32_t`. The decoder rejects an oversized image before scanning
or writing image flash; the check includes the final partial page.

`flash_read` and `flash_write_page` receive absolute addresses.
`flash_read` must immediately observe completed writes.
`flash_write_page` must synchronously erase and program one aligned
`OUTROW` page from the complete supplied buffer.

The decoder preserves bytes beyond the logical image end when it prepares the
last page. If the hardware erase unit is larger than `OUTROW`, the driver must
also preserve bytes outside the supplied page.

`flash_write_page` has no return value. Verify or retry the hardware operation
inside the driver. A valid patch writes each changed output page at most once;
after a write failure, the decoder cannot reconstruct the original image.

### Optional hooks

The default CRC implementation is a tableless reflected IEEE CRC-32. A platform
library or hardware peripheral can replace it:

```c
#include <stdint.h>
uint32_t platform_image_crc32(uint32_t start, uint32_t size);
#define CRC32_DECODE(start,size) platform_image_crc32((start),(size))
#include "patch_apply.h"
```

`start` is already an absolute device address, including
`PATCH_IMAGE_BASE`; do not add the base again. `size` is the number of bytes
to checksum. The result must match zlib CRC-32: reversed polynomial
`0xedb88320`, initial value `0xffffffff`, and final XOR `0xffffffff`. It
must see flash writes completed during the apply.

The decoder uses the C library's `memmove` by default. If the final firmware
does not already link it, define `HAND_ROLLED_MEMMOVE` before including
`patch_apply.h` to use the decoder's private backward-copy loop. This changes
code generation, not the patch format or RAM layout.

Do not override other constants in `patch_config.h`. The encoder and decoder
must use matching patch-format definitions. The device implementation targets
Cortex-M0/ARMv6-M.

### Patch input and recovery

The byte callback returns `PATCH_PULL_BYTE` after writing one byte to `*out`.
Return `PATCH_PULL_END` for end of input or truncation. Any other return value
also aborts the patch. The callback may block.

The patch body has a recorded length. The decoder stops after that body and
does not consume trailing bytes. The caller can use them for outer framing.

`patch_apply_run` is synchronous. It checks the source CRC before the first
write, applies the patch, commits buffered pages, and checks the target CRC.
It returns `PATCH_APPLY_DONE` only when all steps succeed.

CRC detects accidental corruption but does not authenticate an update.
Authenticate the complete manifest and patch, and enforce target and
anti-rollback policy, before calling the decoder. The call can spend a long
time in full-image CRC scans and patch application, so service the watchdog
inside the flash functions, byte callback, and platform CRC implementation.

After `PATCH_APPLY_ERROR`, the image may be unchanged, partially written, or
fully written but unverified. Recover with a full external reflash; do not retry
the patch. A reset or power loss during application has the same recovery
requirement. The decoder has no resume, rollback, or persistent progress
protocol.

### Memory

The decoder uses no heap or internal global state. `PatchApply` contains its
working state and at most two dirty output pages; it never contains a full
firmware image.

`make check-footprint` reports reference flash, state, and stack use for both
copy modes. The project has a hard 12,288-byte `.bss` limit. The reported
stack excludes `PatchApply` storage, platform flash and CRC functions, the byte
callback, interrupts, and RTOS frames. Include those costs when measuring the
final firmware. If stack space is tight, place `PatchApply` in caller-owned
static or no-init storage.

## Design summary

The encoder searches several source/target match plans and chooses a monotonic
write direction. Growing images normally apply from high addresses to low
addresses; shrinking images apply from low to high. This keeps most source
bytes ahead of the overwrite point.

The decoder buffers two 256-byte output pages. New bytes remain in RAM while
the corresponding flash pages still contain the old bytes, allowing output
matches to read RAM and source matches to read flash. Pages are programmed once
when their buffer slot is reused.

The host identifies source copies that would read overwritten data and sends
the required target bytes through the normal compressed content stream. The
device therefore needs neither a full-image buffer nor an old-byte journal.

## Verification and release

Run the release gate from the commit that will be released:

```sh
make gate
```

Do not release if it fails. Retain the commit SHA and complete output.

The gate checks:

- All 290 patch directions from the frozen 36-image corpus under
  `test-bench/images`, `test-bench/fixtures`, and `test-bench/foreign`.
- Both directions of the real one-face update, reported separately from the
  corpus total.
- Encoder self-application with the production decoder, exact target output,
  complete patch consumption, and safe NVM writes.
- Host `--decode` behavior, including in-place application and rejection of a
  truncated patch without changing the input file.
- Decoder flash, state, and stack limits in both copy modes.

The gate reads the committed `watch.bin` files in place. Replacing a fixture is
an explicit corpus and size-ratchet change, not part of a release build.

## License and third-party notices

UltraPatch-authored source and documentation are licensed under
[LICENSE](LICENSE).

Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>.

### libdivsufsort

`vendor/libdivsufsort/` is vendored from libdivsufsort by Yuta Mori and retains
its upstream MIT-style license headers in each vendored source file.

### CircuitPython test corpus

The committed binaries under `test-bench/foreign/` are derived from official
Adafruit CircuitPython releases for the `feather_m0_express` board. They are
test data, not UltraPatch-authored firmware.

The raw releases are `2.2.0`, `2.2.1`, `2.2.2`, `2.2.3`, `2.2.4`,
`2.3.0`, `2.3.1`, `3.0.0`, `3.0.1`, `3.0.2`, and `3.0.3`. The UF2
releases are `10.0.0`, `10.0.1`, `10.0.2`, `10.0.3`, `10.1.1`,
`10.1.2`, and `10.1.3`; they were unpacked at application base `0x2000` to
match the raw image layout. The committed files and their Git history are the
frozen corpus provenance record.

Official artifacts came from the Adafruit CircuitPython listings for
[older raw binaries](https://adafruit-circuit-python.s3.amazonaws.com/index.html?prefix=bin/feather_m0_express/en_US/OLD/)
and [current UF2 releases](https://adafruit-circuit-python.s3.amazonaws.com/index.html?prefix=bin/feather_m0_express/en_US/).
Corresponding source and per-file notices are available from the release tags
and history in the
[Adafruit CircuitPython repository](https://github.com/adafruit/circuitpython).
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
