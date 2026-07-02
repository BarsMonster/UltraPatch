# Device Integration Contract

`src/patch_apply.h` is the production decoder artifact. It is deliberately
small and static-state driven so the Cortex-M0+ `.bss` gate remains meaningful.
Treat it as a bootloader/update component, not as a general reentrant library.

The decoder owns the whole patch blob: envelope parsing, both CRC gates, the
apply direction, and the image span are all derived internally from the pushed
bytes. The integrator does not parse the envelope and cannot get it wrong.

## Ownership

Include `src/patch_apply.h` in exactly one translation unit. The header
owns decoder static state: entropy models, the journal arena, and the output
row cache. There is no coroutine, no fiber, and no private decode stack — the
decode runs on the caller's stack.

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

## Integration

Supply a callback that returns the next blob byte (it may poll a UART/BLE
buffer internally) or 0 at end-of-blob, and call
`patch_apply_run(callback, ctx)`. The whole decode runs synchronously on the
caller's stack: no coroutine, no fiber stack, no platform context-switch code,
no compiler-specific stack sizing.

```c
/* return 1 and write one blob byte to *out; return 0 at end-of-blob.
 * May block internally (e.g. wait on a UART ring buffer) before returning. */
int next_byte(void *ctx, uint8_t *out);

int rc = patch_apply_run(next_byte, &my_ctx);   /* PATCH_APPLY_DONE / _ERROR */
```

`src/patch_apply_demo.c` (`PullCtx` / `pull_next`) is a minimal reference
implementation of the callback.

**Event-driven producers (ISR push)** adapt through the optional
single-producer/single-consumer byte ring in `src/patch_apply_push_adapter.h`:
the RX interrupt calls `patch_ring_push(&ring, byte)` (then `patch_ring_eof()`
after the last byte), and the update task calls
`patch_apply_run(patch_ring_next, &ring)`. While the ring is empty the adapter
invokes an integrator wait hook (typically WFI/WFE, an RTOS yield, or a
transport poll) — the hook must allow the producer to run; never call
`patch_apply_run` from the producer's own context. The adapter is deliberately
not part of the device decoder artifact: `patch_apply.h` compiles, fuzzes, and
gets sized without it.

## Call Sequence

1. Authenticate the update envelope and reject rollback or wrong-target updates.
2. Call `patch_apply_run(callback, ctx)` and use its return —
   `PATCH_APPLY_DONE` or `PATCH_APPLY_ERROR` — as the verdict. `DONE` means the
   image was written and both `CRC32(from)` and `CRC32(to)` verified.

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
