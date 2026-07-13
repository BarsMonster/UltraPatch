# Device Integration Contract

## Artifact and ownership

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
`HAND_ROLLED_MEMMOVE` is a separate, non-wire decoder code-generation option;
the [README configuration guide](../README.md#decoder-configuration) explains
when to enable it. The installed wire
mode targets Cortex-M0/ARMv6-M. `CORTEX_M4` is a reserved wire-selection macro
and defining it is rejected; compiling the same C for another CPU does not
select a different wire.

## Partition and flash contract

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

## Applying a patch

The decoder owns envelope parsing, direction, sizes, and both CRC checks. The
integrator supplies the complete authenticated blob through a `PatchPull`
callback:

```c
int next_byte(void *ctx, uint8_t *out);

PatchApply state; /* Caller-owned; use reserved storage if the stack is tight. */
PatchApplyResult result = patch_apply_run(&state, next_byte, &my_ctx);
if (result != PATCH_APPLY_DONE) {
    /* Inspect patch_apply_reject() and patch_apply_flash_touched(). */
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
watchdog hook. Service the watchdog inside `flash_read`, `flash_write_page`, and
the byte callback.

CRC32 detects accidental corruption; it does not authenticate an update.
Authenticate the complete manifest and blob, and enforce target and anti-rollback
policy, before calling the decoder. The old-image CRC is checked before the
first write. The final-image CRC is necessarily checked after apply, when flash
may already have been changed.

## Memory budget

The repository release gate has an absolute 12,288-byte `.bss` ceiling and ratchets the reference
static-wrapper flash/state plus the worst supported call graph. It measures both library and
hand-rolled copy modes and reports the current values in `make gate` output.

Flash remains the image backing store. `PatchApply` stages at most
`OUTROW_DEPTH` dirty output pages in RAM and never holds a full-image buffer.

The stack limit includes the repository integration wrapper but excludes
`PatchApply` storage, the integrator's flash functions and pull callback, and
bounded toolchain leaves. Place the state object in reserved static/noinit
storage if necessary, then add those external frames, interrupt nesting, and
RTOS frames to the product budget. A different wrapper or toolchain must be
measured in the final firmware build.

## Failure and recovery

Use both the return value and `patch_apply_flash_touched()` to decide what is
safe:

| Terminal state | Flash state | Required action |
| --- | --- | --- |
| `PATCH_APPLY_DONE` | Target image present and both CRCs verified | Boot the new image |
| Error, flash untouched | Original image intact | Correct the cause; a later apply may start from the original image |
| Error, flash touched | Original image destroyed or result unverified | Full external reflash |

This includes resource rejection, transport loss, a silent write failure found
by the final CRC, and any other decoder error. Never retry a patch against a
partially overwritten image.

The decoder has no persistent progress marker, rollback, checkpoint, or resume
protocol. After a reset or power loss that may have occurred after the first
write, treat the image as touched and perform a full external reflash.
