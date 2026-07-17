# UltraPatch

UltraPatch creates compact, in-place firmware delta patches for Cortex-M0/M0+-class
devices. It provides:

- `ultrapatch`, a host CLI that creates patches and can apply them for testing.
- A header-only device decoder in `src/patch_apply.h`.

The host does the expensive analysis. The device decoder applies the patch to
internal flash directly from a byte stream: no second image slot, no heap, no
full-image RAM buffer, and no internal global state. New bytes are staged in two
256-byte RAM pages.

A wrong or stale patch is rejected by the source CRC before the first flash write.
Applying is not power-fail-safe. A partially applied patch can only be recovered by
a full reflash. UltraPatch can also do a full reflash — use an empty (zero-length)
source image to generate a full-reflash patch (it is still compressed by ~30%).

The decoder targets Cortex-M0/ARMv6-M and is plain C11 with no heap, no
file-scope state, no 64-bit integers, no floating point, and no division. All
working state lives in one caller-owned `PatchApply`.

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

Every encode self-applies its patch with the production decoder and refuses to
emit a patch that does not reproduce the exact target image. The project release
gate additionally round-trips a 290-direction image corpus and bounds the
decoder footprint; see AGENTS.md.

## Device integration

The decoder is three headers in `src/`: `patch_apply.h`, `patch_config.h`, and
`rc_models.h`.

**The decoder cannot execute from the flash range it is patching.** It erases
and reprograms pages inside `PATCH_IMAGE_BASE .. PATCH_IMAGE_BASE +
PATCH_IMAGE_CAPACITY` while running, so the updater code, the flash driver, and
the active vector table must all live outside that range. Either extend the
bootloader and link UltraPatch into it, or copy the updater to SRAM and execute
it from there — practical on microcontrollers with at least 16 KiB of SRAM,
given the decoder's roughly 12.5 KiB required for code, state, and stack.

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
static, no-init, or reused bootloader work RAM — `patch_apply_run` zeroes the
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

Pull mode — the callback fetches the next byte itself. This fits transports
that already buffer and flow-control in the driver or protocol (a TCP socket,
USB with NAK, an RTS/CTS UART driver), and reads from staged local storage the
same way:

```c
int stream_pull(void *ctx, uint8_t *out) {
    (void)ctx;
    int c = transport_read_byte_blocking();  /* service the watchdog inside */
    if (c < 0) return PATCH_PULL_END;        /* link closed or timed out */
    *out = (uint8_t)c;
    return PATCH_PULL_BYTE;
}
```

Push mode — the transport delivers bytes asynchronously (interrupt or DMA)
into a ring buffer; the callback drains the ring and sleeps while it is empty:

```c
int ring_pull(void *ctx, uint8_t *out) {
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

Size the ring for the decoder's work bursts, not the average byte rate. Most
patch bytes decode in microseconds, but a byte that completes an output page
triggers a synchronous page erase and program, and the header bytes trigger
the full-image source CRC scan — the longest stall by far (with the default
bit-at-a-time CRC, on the order of seconds for a 100 KiB image; a hardware
`CRC32_DECODE` shortens exactly this). While the decoder is stalled, incoming
bytes accumulate in the ring. With link-level flow control a small ring is
enough — assert flow control while the ring is more than half full. Without
flow control, the ring must absorb the byte rate times the longest stall.

The `CRC32_READY` hook (see Optional hooks) removes the scan stall from the
ring budget entirely. Protocol: always send exactly the first 27 bytes of the
patch (27 covers the envelope header of any valid patch; send the whole patch
if it is smaller), wait for the device to report that `CRC32_READY` fired,
then stream the remainder. The ring then only has to absorb page-commit
bursts, so a 64-128 byte ring is typically enough.

### Flash contract

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
also preserve bytes outside the supplied page.

`flash_write_page` has no return value. Verify or retry the hardware operation
inside the driver. A valid patch writes each changed output page at most once;
after a write failure, the decoder cannot reconstruct the original image.

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
instead of pulling the library implementation into flash — a code-size saving
when `memmove` would be linked only for the decoder. This changes code
generation, not the patch format or RAM layout.

`CRC32_READY()` is an optional notification hook for streaming senders. When
defined before including the headers, the decoder invokes it exactly once per
apply — after the source image is fully validated (source CRC match) and the
pre-body flash scans are complete, immediately before the first
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
It returns `PATCH_APPLY_DONE` only when all steps succeed. A mismatched wire
revision, truncated header, implausible size, oversized image, or wrong or
dirty source image all reject before the first flash write.

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
| `CRC32_READY()` | optional | notification hook: called once when source validation and the pre-body scans finish, immediately before the first body byte is pulled; enables the send-header-then-wait streaming protocol |
| `NO_GNU_EXTENSIONS` | optional | plain-C11 fallbacks for compilers with incomplete GNU attribute support |
| `PATCH_WIRE_VERSION` (1), `MAX_IMAGE` (64 MiB), `WINDOW_LOG` (11), `DR_KCAP_BL` (152), `DR_KCAP_EX` (88), `OUTROW` (256, erase-page size), `OUTROW_DEPTH` (2) | wire parameters — MUST match on encoder and decoder | adjust by editing `patch_config.h` and rebuilding both the CLI and the device decoder from the same headers (e.g. retarget `OUTROW` to the hardware erase-page size); predefining them per build is a compile error, which prevents silent mismatch; they remain readable after the include — use `OUTROW` for the `flash_write_page` buffer size |

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

- `REJ_RESOURCE` — the patch needs more than the configured partition or
  exceeds a decoder cap. A too-small `PATCH_IMAGE_CAPACITY` is the common
  cause.
- `REJ_CORRUPT` — everything else: malformed or truncated stream, wrong or
  dirty source image, wire-revision mismatch, or a failed target CRC.

`patch_apply_from_size()`, `patch_apply_to_size()`, `patch_apply_image_span()`,
and `patch_apply_forward()` expose the decoded envelope geometry for logging
and manifest cross-checks; they are `static inline` and cost nothing when
unused.

For host-side triage, apply the patch to a copy of the source image with
`--decode` and compare with `cmp`; the host tool rejects a truncated patch
without modifying the input file.

## License and third-party notices

UltraPatch-authored source and documentation are licensed under
[LICENSE](LICENSE).

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
