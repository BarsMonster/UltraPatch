# UltraPatch

UltraPatch creates compact, in-place firmware delta patches for Cortex-M0/M0+-class
devices. It was inspired by [detools](https://github.com/eerimoq/detools). It
provides:

- `ultrapatch`, a host CLI that creates patches and can apply them for testing.
- A header-only device decoder in `src/patch_apply.h`.

The host does the expensive analysis. The device decoder is an in-place
streaming patcher: it applies the patch to internal flash directly from a byte
stream, so the patch never needs to be stored in full before application.
There is no second image slot and no full-image RAM buffer; new bytes are
staged in two 256-byte RAM pages. The decoder allocates nothing and keeps no
internal global state.

A wrong or stale patch is rejected by the source CRC before the first flash write.
Applying is not power-fail-safe. A partially applied patch can only be recovered by
a full reflash. UltraPatch can also do a full reflash: use an empty (zero-length)
source image to generate a full-reflash patch (it is still compressed by ~30%).
Also, if only a few bytes changed, only the minimum number of flash pages is
erased and reprogrammed.

The decoder targets Cortex-M0/ARMv6-M and is plain C11. It never divides,
never touches floating point or 64-bit arithmetic, and holds all working state
in one caller-owned `PatchApply` (no heap, no file-scope state). UltraPatch
correctly applies any binary content beyond the M0 instruction set (the
round-trip is always byte-exact), but compression there is less spectacular:
core improvements would be needed to model M3/M4/M7 addressing modes.

UltraPatch requires approximately 5.2 KiB of flash, 6.6+0.5 KiB of SRAM (with stack).

## Host CLI & build process

On Debian or Ubuntu, install the build dependencies:

```sh
apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
  gcc git libc6-dev make python3
```

`gcc-arm-none-eabi`, `binutils-arm-none-eabi`, and `libnewlib-dev` are needed
only by `make check-footprint`, which cross-compiles the decoder to measure it.

Build the CLI and ask Make for its path:

```sh
make
tool=$(make -s host-tool-path)
```

The default path is `.build/ultrapatch`, but build arguments can change it.
Do not assume a root-level `./ultrapatch`.

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

Every encode self-applies its patch with the production decoder and will not
emit a patch that does not reproduce the exact target image. The project release
gate (`make gate`) also round-trips a 290-direction image corpus and bounds the
decoder footprint.

## How it works

The encoder finds shared content between the old and new images with
suffix-array matching (bsdiff-style, using libdivsufsort) and builds a plan of
operations: copy a run of source bytes with per-byte differences, insert new
bytes, seek in the source. The plan is applied in one monotonic direction so
that source data is consumed before it is overwritten: a growing image is
normally patched from high addresses down, a shrinking one from low addresses
up. The two-page commit delay keeps just-overwritten source readable a little
longer, and the few copies that would still read stale data are converted to
literals at encode time.

When code moves, every Cortex-M0 instruction that addresses it by offset
changes its immediate field even though nothing else changed. UltraPatch finds
BL branches and LDR literal loads on the device by scanning the instruction
stream (no positions are shipped) and reconstructs their new immediates from a
small piecewise "shift map" that describes how far each address range moved.
Values the map does not predict come from two move-to-front caches of recent
values with a coded escape, so mispredictions are cheap and never fatal.

Everything else travels as a byte stream compressed with LZSS over a 2 KiB
window of recent output: literal runs interleaved with matches, a
repeat-last-distance shortcut, and long-range "out matches" into output
written earlier in the apply.

All of it is coded in a single adaptive binary range-coder stream. Literal
bytes go through context-selected bit trees seeded from the source image's
byte histogram, so the models start warm instead of flat. Lengths use adaptive
Elias-gamma codes, distances and positions use adaptive Rice codes, and
structural decisions (span vs match, distance reuse, cache hits) use small
adaptive flag models. The decoder mirrors every model bit-exactly, which is
why the whole patch fits in one stream with no shipped tables.

## Compression comparison

Measured on the real product update from this repository's fixtures
(`v0_base` → `v1_one_face`, 113,124 → 113,484 bytes; detools 0.53.0). Sizes in
bytes:

| Tool / mode | Patch size |
|---|---|
| **UltraPatch** (in-place, 512 B page cache) | **573** (revert 290) |
| detools sequential, zstd | 2,279 |
| detools sequential, lzma | 2,439 (revert 2,096) |
| detools sequential, heatshrink | 4,134 |
| detools in-place, lzma, 512 B segments | 3,785 |
| detools in-place, heatshrink, 512 B segments | 6,974 |
| detools in-place, lzma, 64 KiB segments | 71,423 |
| detools in-place, heatshrink, 64 KiB segments | 107,795 |

Note that detools *sequential* patches are not in-place: applying them needs a
second image slot or external staging. The in-place runs used
`--memory-size 131072` with the listed `--segment-size`. On the apply side,
lzma decompression needs far more RAM than UltraPatch's 6.6 KiB total;
heatshrink is the one with comparable RAM needs.

Full image delivery (the same 113,484-byte target):

| Method | Size | % of image |
|---|---|---|
| 7z (LZMA2, `-mx=9`) | 71,524 | 63.0% |
| **UltraPatch** empty-source full-reflash patch | **74,712** | 65.8% |
| gzip -9 | 79,464 | 70.0% |
| zip -9 | 79,604 | 70.1% |

The UltraPatch row is directly flashable in place by the same 6.6 KiB-RAM
decoder. The archive rows are baselines for transport size only; on the device
they would still need a decompressor and staging.

## Device integration

The decoder is three headers in `src/`: `patch_apply.h`, `patch_config.h`, and
`rc_models.h`.

The decoder cannot execute from the flash range it is patching. It erases
and reprograms pages inside `PATCH_IMAGE_BASE .. PATCH_IMAGE_BASE +
PATCH_IMAGE_CAPACITY` while running, so the updater code, the flash driver, and
the active vector table must all live outside that range. Either extend the
bootloader and link UltraPatch into it, or copy the updater to SRAM and execute
it from there. The SRAM route is practical on microcontrollers with at least
16 KiB of SRAM, given the decoder's roughly 12.5 KiB required for code, state,
and stack.

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
run two decoders concurrently against the same flash image. It can live in
static, no-init, or reused bootloader work RAM: `patch_apply_run` zeroes the
object itself, so no-init placement needs no initialization code.

### Feeding the patch stream

The decoder pulls its input: `patch_apply_run` invokes the byte callback
synchronously, on the caller's stack, whenever it needs the next patch byte.
The callback returns `PATCH_PULL_BYTE` after writing one byte to `*out`, or
`PATCH_PULL_END` for end of input or truncation. Any other return value also
aborts the patch. The callback may block. The patch body has a recorded length:
the decoder stops after that body and does not consume trailing bytes, so outer
framing such as signatures or manifests can follow the patch in the same
stream.

Pull mode: the callback fetches the next byte itself. This fits transports
that already buffer and flow-control in the driver or protocol (a TCP socket,
USB with NAK, an RTS/CTS UART driver), and reads from staged local storage the
same way:

```c
int next_byte_stream_pull(void *ctx, uint8_t *out) {
    (void)ctx;
    int c = transport_read_byte_blocking();  /* service the watchdog inside */
    if (c < 0) return PATCH_PULL_END;        /* link closed or timed out */
    *out = (uint8_t)c;
    return PATCH_PULL_BYTE;
}
```

Push mode: the transport delivers bytes asynchronously (interrupt or DMA)
into a ring buffer; the callback drains the ring and sleeps while it is empty:

```c
int next_byte_ring_pull(void *ctx, uint8_t *out) {
    Ring *r = ctx;
    while (ring_empty(r)) {          /* transport ISR pushes into the ring */
        if (r->aborted) return PATCH_PULL_END;
        watchdog_feed();
        wait_for_interrupt();
    }
    *out = ring_pop(r);
    return PATCH_PULL_BYTE;
}
```

Size the ring for the slowest stretch of decoding rather than for the average
byte rate. Most patch bytes decode in microseconds, but a byte that completes
an output page triggers a synchronous page erase and program, and the header
bytes trigger the full-image source CRC scan, which is the longest stall by
far. With the default bit-at-a-time CRC that scan takes on the order of
seconds for a 100 KiB image; a hardware `CRC32_DECODE` shortens it. While the
decoder is stalled, incoming bytes accumulate in the ring. With link-level
flow control a small ring is enough: assert flow control while the ring is
more than half full. Without flow control, the ring must absorb the byte rate
times the longest stall.

The `CRC32_READY` hook (see Optional hooks) removes the scan stall from the
ring budget entirely. The sender always transmits the first 27 bytes of the
patch (27 covers the envelope header of any valid patch; send the whole patch
if it is smaller), waits for the device to report that `CRC32_READY` fired,
and only then streams the remainder. The ring then only has to absorb
page-commit bursts, so a 64-128 byte ring is typically enough.

### Flash

`PATCH_IMAGE_BASE` is the absolute start of the patchable image.
`PATCH_IMAGE_CAPACITY` is the physical capacity from that address. Both must
be aligned to `OUTROW`, the capacity must be nonzero, and the complete range
must fit in `uint32_t`. The decoder rejects an oversized image before scanning
or writing image flash; the check includes the final partial page.

`OUTROW` (256 bytes by default; retargetable, see Parameters) is the erase-page
size: the decoder changes flash only in whole, aligned erase pages. `flash_read`
and `flash_write_page` receive absolute addresses. `flash_read` must immediately
observe completed writes. `flash_write_page` must synchronously erase and program
one aligned `OUTROW` page from the complete supplied buffer. If the hardware program
page is smaller than the erase page, erase once and program the 256 bytes in as many write
operations as the part requires.

The decoder preserves bytes beyond the logical image end when it prepares the
last page. If the hardware erase unit is larger than `OUTROW`, the driver must
also preserve bytes outside the supplied page. That configuration is highly
discouraged: every page write then erases and reprograms the whole larger
unit, which multiplies flash write amplification. `OUTROW` is very strongly
advised to match the hardware erase-page size (see Parameters for
retargeting).

`flash_write_page` has no return value. Verify or retry the hardware operation
inside the driver. A valid patch writes each changed output page at most once;
after a write failure, the decoder cannot reconstruct the original image. If
the driver detects an unrecoverable write failure, feel free to report a fatal
error and reset the device: the image needs a full reflash either way.

It is recommended not to update the bootloader with UltraPatch (or at all):
flash retention reduces with write cycles, and the bootloader is the one
component that must stay intact to recover the device.

### Optional hooks

The default CRC implementation is a tableless reflected IEEE CRC-32, which is very
compact but might be a bit slow on a microcontroller. A hardware CRC peripheral
(available on many microcontrollers) or a table-driven library implementation
can be 10+x faster and save some battery:

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

The decoder uses the C library's `memmove` by default, which is free when the
firmware already links it. If it does not, define `HAND_ROLLED_MEMMOVE` before
including `patch_apply.h` to use the decoder's private backward-copy loop
instead of pulling the library implementation into flash, which saves code
size when `memmove` would be linked only for the decoder. Only code generation
changes; the patch format and RAM layout stay the same.

`CRC32_READY()` is an optional notification hook for streaming senders. When
defined before including the headers, the decoder invokes it exactly once per
apply, after the source image is fully validated (source CRC match) and the
pre-body flash scans are complete, and immediately before the first
compressed-body byte is pulled. It is not called when validation fails:

```c
#define CRC32_READY() transport_signal_ready()
#include "patch_apply.h"
```

Define `NO_GNU_EXTENSIONS` to force the plain-C11 fallbacks for compilers that
define `__GNUC__` without full attribute support. The decoder behaves
identically either way.

## Recovery and security

`patch_apply_run` is synchronous. It checks the source CRC before the first
write, applies the patch, commits buffered pages, and checks the target CRC.
It returns `PATCH_APPLY_DONE` only when all steps succeed. Anything wrong with
the header or the source image (a mismatched wire revision, a truncated
header, implausible sizes, an image over capacity, a wrong or dirty source) is
rejected before the first flash write.

CRC detects accidental corruption but does not authenticate an update.
Authenticate the patch if needed, and enforce target and anti-rollback policy,
before calling the decoder. The call can spend a long time in full-image CRC
scans and patch application, so service the watchdog inside the flash
functions, byte callback, and platform CRC implementation.

After `PATCH_APPLY_ERROR`, the image may be unchanged, partially written, or
fully written but unverified. Recover with a full external reflash; do not retry
the patch (it will likely be rejected due to mismatch of original image CRC).
A reset or power loss during application will also require full reflash.

## Reference

### Parameters

| Define | Role | Notes |
|---|---|---|
| `PATCH_IMAGE_BASE` | required, before include | absolute start of the patchable image; `OUTROW`-aligned (compile-time asserted) |
| `PATCH_IMAGE_CAPACITY` | required, before include | physical capacity from that address; a nonzero multiple of `OUTROW`; base + capacity must fit `uint32_t` |
| `CRC32_DECODE(start,size)` | optional | hardware or library CRC-32 replacement; zlib semantics; `start` is absolute |
| `HAND_ROLLED_MEMMOVE` | optional | private backward-copy loop instead of libc `memmove`; codegen only |
| `CRC32_READY()` | optional | notification hook: called once when source validation and the pre-body scans finish, immediately before the first body byte is pulled; lets a streaming sender pause after the first 27 bytes until the device is ready |
| `NO_GNU_EXTENSIONS` | optional | plain-C11 fallbacks for compilers with incomplete GNU attribute support |
| `PATCH_WIRE_VERSION` (1), `MAX_IMAGE` (64 MiB), `WINDOW_LOG` (11), `DR_KCAP_BL` (152), `DR_KCAP_EX` (88), `OUTROW` (256, erase-page size), `OUTROW_DEPTH` (2) | wire parameters; MUST match on encoder and decoder | adjust by editing `patch_config.h` and rebuilding both the CLI and the device decoder from the same headers (e.g. retarget `OUTROW` to the hardware erase-page size); predefining them per build is a compile error, which prevents silent mismatch; they remain readable after the include (use `OUTROW` for the `flash_write_page` buffer size) |

### Wire compatibility

The wire parameters in the table above are part of the same contract: an
encoder and a decoder built with different values (for example a retargeted
`OUTROW`) are incompatible. Change them only by editing `patch_config.h` for
both sides together, and when retargeting for a product line, also change
`PATCH_WIRE_VERSION` so patches from other builds reject before the first
flash write.

### Results and reject reasons

`patch_apply_run` returns `PATCH_APPLY_DONE` (0) or `PATCH_APPLY_ERROR` (1).
After an error, `patch_apply_reject()` returns:

- `REJ_RESOURCE`: the patch needs more than the configured partition or
  exceeds a decoder cap. A too-small `PATCH_IMAGE_CAPACITY` is the common
  cause.
- `REJ_CORRUPT`: everything else, such as a malformed or truncated stream, a
  wrong or dirty source image, a wire-revision mismatch, or a failed target
  CRC.

`patch_apply_from_size()`, `patch_apply_to_size()`, `patch_apply_image_span()`,
and `patch_apply_forward()` expose the decoded envelope geometry for logging
and manifest cross-checks; they are `static inline`, so unused ones add no
code.

For host-side triage, apply the patch to a copy of the source image with
`--decode` and compare with `cmp`; the host tool rejects a truncated patch
without modifying the input file.

## License and third-party notices

UltraPatch-authored source and documentation are licensed under the
[MIT license](LICENSE).

Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>.

`vendor/libdivsufsort/` is vendored from libdivsufsort by Yuta Mori and retains
its upstream MIT-style license headers in each vendored source file. Vendored
third-party code is used by the host encoder only; nothing third party compiles
into the device decoder.

The main test corpus (the committed firmware binaries under
`test-bench/images/` and `test-bench/fixtures/`) consists of builds from the
[Sensor Watch](https://github.com/joeycastillo/Sensor-Watch) project. The
committed binaries under `test-bench/foreign/` are test data derived from
official Adafruit CircuitPython releases (MIT). Full provenance and notices:
[test-bench/foreign/NOTICE.md](test-bench/foreign/NOTICE.md).
