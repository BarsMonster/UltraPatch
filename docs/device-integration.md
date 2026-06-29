# Device Integration Contract

`src/patch_apply.h` is the production decoder artifact. It is deliberately
small and static-state driven so the Cortex-M0+ `.bss` gate remains meaningful.
Treat it as a bootloader/update component, not as a general reentrant library.

## Ownership

Include `src/patch_apply.h` in exactly one translation unit. The header
owns decoder static state, the byte FIFO, the coroutine stack, entropy models, the
journal arena, and the output row cache.

The target must provide:

```c
uint8_t flash_read(uint32_t addr);
void flash_write(uint32_t addr, uint8_t value);
uint32_t g_image_span;
```

`g_image_span` must be `max(from_size, to_size)` for the patch being applied.
`flash_read` must return the current byte at the image address. `flash_write`
must implement the platform flash programming behavior. The decoder writes rows
monotonically through its resident row buffer, so each row should be erased and
programmed at most once for valid A1 patches.

## Patch Envelope

The decoder consumes only the range-coded body. Production code must parse the
outer envelope before calling the decoder:

```text
CRC32(from)[4]
from_size uLEB
zigzag(to_size - from_size) uLEB
zigzag(fp_end - from_size) uLEB   # present only when to_size > from_size
range-coded body bytes
CRC32(to)[4]
```

CRC32 is an integrity check against accidental corruption. It is not an
authenticity mechanism. A production OTA flow should authenticate the whole
delivery manifest and patch blob before any flash write is attempted.

## Call Sequence

1. Authenticate the update envelope and reject rollback or wrong-target updates.
2. Parse the envelope and verify `CRC32(from)` against the current image.
3. Set `g_image_span = max(from_size, to_size)`.
4. Call `patch_apply_set(from_size, to_size, fp_end, to_size <= from_size)`.
5. Call `patch_apply_init()`.
6. Feed each body byte with `patch_apply_push(byte)` until it returns
   `PATCH_APPLY_DONE` or `PATCH_APPLY_ERROR`.
7. After the last body byte, call `patch_apply_finish()` if the decoder still
   needs input.
8. Verify `CRC32(to)` over the final image before booting it.

The decoder status values are:

```c
PATCH_APPLY_NEED_MORE
PATCH_APPLY_DONE
PATCH_APPLY_ERROR
```

The API is single-instance, non-reentrant, and not thread-safe. Do not run two
decoders concurrently in one image. Do not call `patch_apply_set` for a new patch
until the previous patch has reached `DONE` or `ERROR` and the bootloader has
chosen a recovery path.

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
emulator. It is useful as a reference for envelope parsing and metrics, but it is
not the device integration layer.
