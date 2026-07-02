# Device Integration Contract

`src/patch_apply.h` is the production decoder artifact. It is deliberately
small and static-state driven so the Cortex-M0+ `.bss` gate remains meaningful.
Treat it as a bootloader/update component, not as a general reentrant library.

The decoder owns the whole patch blob: envelope parsing, both CRC gates, the
apply direction, and the image span are all derived internally from the pushed
bytes. The integrator does not parse the envelope and cannot get it wrong.

## Ownership

Include `src/patch_apply.h` in exactly one translation unit. The header
owns decoder static state, the byte FIFO, the coroutine stack, entropy models, the
journal arena, and the output row cache.

The target must provide exactly two flash primitives:

```c
uint8_t flash_read(uint32_t addr);
void    flash_write(uint32_t addr, uint8_t value);
```

`flash_read` must return the current byte at the image address. `flash_write`
must implement the platform flash programming behavior (program the byte,
erasing its row first when a 0-to-1 transition requires it). The decoder writes
rows monotonically through its resident row buffer, so each row is erased and
programmed at most once for valid A1 patches.

## Patch Envelope

The blob layout (all of it is pushed through the decoder):

```text
CRC32(from)[4]
from_size uLEB
zigzag(to_size - from_size) uLEB
zigzag(fp_end - from_size) uLEB   # present only when to_size > from_size
range-coded body bytes
CRC32(to)[4]
```

The decoder verifies `CRC32(from)` against the current flash content BEFORE
its first flash write (wrong or dirty current image rejects with flash
untouched) and verifies `CRC32(to)` over the final image inside
`patch_apply_finish()` — `PATCH_APPLY_DONE` is returned only after both gates
pass. Header sizes above 64 MiB are rejected as implausible.

CRC32 is an integrity check against accidental corruption. It is not an
authenticity mechanism. A production OTA flow should authenticate the whole
delivery manifest and patch blob before any flash write is attempted.

## Two Integration Modes

**PULL mode (`-DPATCH_APPLY_PULL`) — recommended when the update task can
block.** Supply a callback that returns the next blob byte (it may poll a
UART/BLE buffer internally) or 0 at end-of-blob, and call
`patch_apply_run(callback, ctx)`. The whole decode runs synchronously on the
caller's stack: no coroutine, no fiber stack, no platform context-switch code,
and ~640 B less `.bss`.

**PUSH mode (default) — for event-driven producers.** Bytes are fed from the
outside; the decoder suspends on a private coroutine stack while waiting. The
platform must supply the context switch: `CO_SWAP_TO_MAIN`/`CO_SWAP_TO_DEC`
under `RC_V3_ARM` are SIZING STUBS in this repository (the x86 host test
carries a real fiber; a Cortex-M device needs its own SP-swap, typically a
PendSV pair). Additionally, the coroutine stack size is COMPILER-SPECIFIC:
`DEC_STACK_BYTES` defaults are measured for gcc (520 B high-water) and clang
(3752 B!); re-measure with `-DRC_V3_STACKMEAS` whenever the compiler, version,
or flags change. The deepest 8 bytes are an overflow canary — an overflow
rejects the patch at decode end.

## Call Sequence (push mode)

1. Authenticate the update envelope and reject rollback or wrong-target updates.
2. Call `patch_apply_init()`.
3. Feed EVERY blob byte, in order, with `patch_apply_push(byte)`. It returns
   `PATCH_APPLY_NEED_MORE` while streaming and `PATCH_APPLY_ERROR` on early
   reject (stop feeding then).
4. After the last blob byte, call `patch_apply_finish()` and use its return —
   `PATCH_APPLY_DONE` or `PATCH_APPLY_ERROR` — as the verdict. `DONE` means the
   image was written and `CRC32(to)` verified.

In pull mode, steps 2–4 collapse into one `patch_apply_run()` call whose return
is the verdict.

The decoder status values are:

```c
PATCH_APPLY_NEED_MORE
PATCH_APPLY_DONE
PATCH_APPLY_ERROR
```

On `ERROR`, `g_reject` distinguishes `REJ_RESOURCE` (a decoder resource cap was
exceeded — this firmware needs a larger-sized build) from `REJ_CORRUPT`
(malformed, truncated, or wrong-image patch).

The API is single-instance, non-reentrant, and not thread-safe. Do not run two
decoders concurrently in one image. Do not start a new patch until the previous
patch has reached `DONE` or `ERROR` and the bootloader has chosen a recovery
path.

## Build-Time Contract

The encoder `W` argument must match decoder `SA_W`. The production default is
`W=10` / `SA_W=10`, and this is what `make gate` verifies.

Resource caps such as `JSLOTS`, `DR_KCAP_BL`, `DR_KCAP_EX`, and `OPC_CAP` are
intentional reject limits. Raising them costs SRAM and must be followed by the
plain release gate:

```sh
make gate
```

Do not use deployment-only CFLAGS to pass the SRAM gate. The default build must
remain representative of the shipping decoder.

## Flash And Recovery Policy

A1 does not provide power-fail rollback. If power is lost during apply, the host
or bootloader must be able to detect the interrupted state and recover by full
reflash or another product-defined recovery path.

The host wrapper in `src/patch_apply_demo.c` is a verification harness and NVM
emulator. It is useful as a reference for driving the push API and collecting
NVM metrics, but it is not the device integration layer.
