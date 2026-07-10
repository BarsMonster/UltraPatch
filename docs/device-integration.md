# Device Integration Contract

The production decoder is header-only. Integrators have two supported packaging
choices:

- Include the generated public single header from `make decoder-header`
  (`artifacts/patch_apply_single.h` by default).
- Include the source header set rooted at `src/patch_apply.h`, with
  `src/rc_models.h` and `src/patch_config.h` beside it on the include path.

Both forms compile the same decoder, require an explicit `PATCH_IMAGE_BASE`, and
require the two flash primitives below.
The decoder owns no global static state and uses no heap; the integrator owns
the `PatchApply` state object so the Cortex-M0+ SRAM gate remains meaningful.

The repository also builds `ultrapatch`, a unified host CLI. Its default mode is
encode, and its `--decode` mode is a reference/debug path that uses the same host
backend as encoder self-verification. That host CLI is not part of the device
decoder artifact; embedded integrations include only the decoder header set.

The decoder owns the whole patch blob: envelope parsing, both CRC gates, the
apply direction, and the image span are all derived internally from the pushed
bytes. The integrator does not parse the envelope and cannot get it wrong.

## Ownership

Include the decoder from one update translation unit and allocate one
caller-owned `PatchApply` object for the run. With the source-header-set form,
application code includes only `patch_apply.h`; the support headers are included
by it. Entropy models, the journal arena, and the output page cache live inside
the `PatchApply` object. There is no coroutine, no fiber, no private decode
stack, and no heap allocation -- the decode runs on the caller's stack plus the
caller-owned state object.

The decoder implementation has internal header-only linkage by default. To avoid
accidental duplicate decoder text in final firmware, include the decoder from a
single update `.c` file and route other application modules through that update
module's local API.

The repository ARM release metrics use the static-wrapper integration built by
`make check-arm`: one file-scope `PatchApply` object and a small
`rcv3_run(next, ctx)` wrapper that passes that object to `patch_apply_run()`.
The relocatable-object metric remains the direct decoder compiler output. A
second no-startup link places that same object in an explicit Cortex-M0+ FLASH/RAM
layout, resolves the two platform symbols with minimal stubs, and links only the
`libc`/`libgcc` members that the decoder actually references. On the pinned GNU
Arm toolchain the ratchets are:

| Footprint form (`gcc -Os`, Cortex-M0+ `-mthumb`) | text | data | bss |
| ------------------------------------------------ | ----:| ----:| ---:|
| Relocatable static-wrapper object | 6057 B | 0 B | 10296 B |
| No-startup linked image | 6637 B | 0 B | 10296 B |

The linked text includes minimal `flash_read`/`flash_write_page` stubs
and the pulled `memcpy`, `memmove`, and `memset` implementations. It excludes
vector tables, CRT initialization, syscalls, board support, and real flash
driver/callback code. Both state measurements enforce the immutable 12288-byte
`.bss` product cap. The generic caller-owned call shape shown below is the public
API contract, but it is not the shape used for these pinned ARM ratchets;
product firmware using a different wrapper, library, or platform code should
size its final image in its own build.

Before including the decoder, define `PATCH_IMAGE_BASE` as the absolute device
address of the patchable image. It must be aligned to `OUTROW`, and the complete
`MAX_IMAGE` span including its final physical page must fit in `uint32_t` address
space; the header enforces all three conditions at compile time.

The target must provide exactly two flash primitives:

```c
uint8_t flash_read(uint32_t absolute_addr);
void    flash_write_page(uint32_t absolute_page_addr,
                         const uint8_t page[OUTROW]);
```

Both addresses are absolute device addresses, not image-relative offsets.
`flash_read` must return the current byte at that address. One
`flash_write_page` call must synchronously erase and fully program exactly one
aligned `OUTROW` page from the complete supplied buffer. No byte-write decoder
API exists. The decoder writes pages monotonically through its resident page
buffers, so each page is erased and programmed at most once for valid A1
patches.

Beyond that, the decoder relies on three contract properties of the pair:

1. **Read-back coherence.** `flash_read` must observe every prior
   `flash_write_page`
   immediately. The decoder reads its own writes back — on the committed-page
   path of `out_read` (a page already flushed out of the RAM buffer is re-read
   through `flash_read`) and during the final `CRC32(to)` pass over the whole
   image. A driver with a page-write buffer must flush before returning from
   `flash_write_page`, or the read-back sees stale bytes and the patch fails at
   `CRC32(to)`.
2. **Complete-page preservation.** The decoder preloads all `OUTROW` bytes before
   modifying a page, including bytes beyond the logical image end in the final
   physical page. The driver must consume the complete buffer exactly as passed;
   erasing a larger hardware region requires the driver to preserve any bytes
   outside that buffer itself.
3. **Write-failure surfacing.** `flash_write_page` returns `void`, so a hardware
   program failure is not reported at the call site — it surfaces only at the
   final `CRC32(to)` pass, as `REJ_CORRUPT`, indistinguishable from a corrupt
   blob. Integrators whose flash can fail silently should verify writes inside
   the driver.

## Patch Envelope

The blob layout (all of it is pushed through the decoder):

```text
CRC32(from)[4]
CRC32(to)[4]
from_size uLEB
zigzag(to_size - from_size) uLEB  # overlong encoding = unnatural apply direction
zigzag(fp_end - from_size) uLEB   # DESCENDING only — the sole source-seed field when descending
zigzag(fp_start) uLEB             # ASCENDING only  — the sole source-seed field when ascending
compressed_body_len uLEB
range-coded body bytes            # exactly compressed_body_len bytes
```

The apply direction is an encoder choice. The NATURAL direction (descending iff
the image grows — the historical derived rule) ships the size-delta as a
canonical uLEB; the unnatural direction is signaled by an overlong encoding
(one redundant trailing continuation byte, value-neutral) and is chosen only
when it wins by more than that byte. Exactly ONE source-seed field rides the
header, selected by that direction: `zigzag(fp_end - from_size)` when the apply
is descending (it seeds the descending source walk), `zigzag(fp_start)` when it
is ascending (the ascending walk's initial source seek). Neither field is ever
present in the other direction.

Both CRCs ride the header. The decoder verifies `CRC32(from)` against the
current flash content BEFORE its first flash write (wrong or dirty current
image rejects with flash untouched), and verifies `CRC32(to)` over the final
image at the end of the same `patch_apply_run()` call. Because `CRC32(to)`
gates the *result*, a wrong `CRC32(to)` is detected only AFTER the image has
been written to flash — the same order as a wrong trailer in the previous wire
revision; only `CRC32(from)` protects flash from being touched. The
range-coded body is the last thing on the wire. Its compressed byte length is
part of the envelope, so the decoder zero-fills the range coder internally
after exactly that many body bytes and `PATCH_APPLY_DONE` does not require a
transport EOF. Bytes after the counted body are outside the decoder contract
and must be handled by the authenticated outer framing/session. Header sizes
above 64 MiB are rejected as implausible.

CRC32 is an integrity check against accidental corruption. It is not an
authenticity mechanism. A production OTA flow should authenticate the whole
delivery manifest and patch blob before any flash write is attempted.

**No version byte — deliberate (decision recorded 2026-07-02).** The envelope
carries no format-version or magic field, per requirements: the envelope stays
minimal, and a blob in any future wire revision is rejected by the CRC/decode
gates as `REJ_CORRUPT`, which is the accepted behavior. Target-family wire
variants are governed at BUILD time by the `CORTEX_M0`/`CORTEX_M4` define
contract (see Build-Time Contract), not by an in-band version field. Do not
add one.

## Integration

Supply a callback that returns the next blob byte (it may poll a UART/BLE
buffer internally) or 0 to abort/end a truncated source, allocate a `PatchApply`, and call
`patch_apply_run(&state, callback, ctx)`. The whole decode runs synchronously on the
caller's stack: no coroutine, no fiber stack, no platform context-switch code,
no compiler-specific stack sizing.

```c
/* return 1 and write one blob byte to *out; return 0 to abort/end a truncated source.
 * May block internally (e.g. wait on a UART ring buffer) before returning. */
int next_byte(void *ctx, uint8_t *out);

PatchApply state;
int rc = patch_apply_run(&state, next_byte, &my_ctx);   /* PATCH_APPLY_DONE / _ERROR */
```

`src/patch_host_backend.c` (`PullCtx` / `pull_next`) is a minimal reference
implementation of the callback.

**`patch_apply_run` blocks the caller for the whole decode** — two full-image
CRC passes (`CRC32(from)` before the first write and `CRC32(to)` at the end)
plus the entire apply — and never returns partway, with no internal watchdog
hook. Pet the watchdog INSIDE the integrator's byte callback and flash
primitives (the per-byte and per-row call-outs the decode already makes), not
around the single `patch_apply_run` call.

### Aborting a transfer

To abandon a decode mid-blob — a lost link, a user cancel, a transport timeout —
the byte callback returns 0. The decoder latches EOF, zero-fills any later range reads,
and terminates in BOUNDED time (an `O(to_size)` wind-down at worst, never an
unbounded hang) with `PATCH_APPLY_ERROR`. Flash may already have been modified
when the abort lands, so recovery follows the same terminal-state matrix as any
other mid-apply error (the Call Sequence table below). No special code path is
needed; the truncated-blob cases in `make gate` drive exactly this wind-down and
reject.

## Stack Budget

The whole decode runs on the caller's stack (no fiber since commit 44eee88), so the
integrator must reserve stack for it on top of the interrupt/RTOS frames. These stack
numbers exclude the `PatchApply` object itself; place that object in static/noinit storage
if the bootloader stack is tight. The statically measured Cortex-M0+ bounds are wrapper
shape-specific:

| Integration shape (gcc `-O2`, Cortex-M0+ `-mthumb`) | Worst-case caller stack | Gated ceiling |
| --------------------------------------------------- | ----------------------- | ------------- |
| Static `PatchApply` object; `rcv3_run(next, ctx)` | **400 B** | 480 B |
| Caller-owned pointer; `rcv3_run(state, next, ctx)` | **440 B** | 480 B |

Method: `scripts/stack_bound.py` sums the deepest path through the static call graph, using
`arm-none-eabi-gcc -fstack-usage` frame sizes and `objdump` `bl` edges. It is exact because
the decoder has no recursion and no indirect call that can reach internal code — the script
fails loudly if any of those, or a dynamic/VLA frame, appears. Each `.su` frame already
includes that function's own pushed LR and callee-saved registers, so the sum accounts for
every on-stack return address; no per-call addend is added.

Each number **includes** its wrapper and all first-party decoder frames. It **excludes** the integrator's own
externs - `flash_read`, `flash_write_page`, and the byte callback (their stack is the integrator's
cost) — and the small toolchain leaves (`memmove`/`memset`); budget those plus
your worst-case interrupt-nesting frame on top. `make gate` measures and gates both gcc -O2
shapes independently via `check-stack`. Firmware with another wrapper shape must measure that
wrapper in its own build; neither repository bound is universal.

## Call Sequence

1. Authenticate the update envelope and reject rollback or wrong-target updates.
2. Allocate a `PatchApply` object, call `patch_apply_run(&state, callback, ctx)`, and use its return —
   `PATCH_APPLY_DONE` or `PATCH_APPLY_ERROR` — as the verdict. `DONE` means the
   image was written and both `CRC32(from)` and `CRC32(to)` verified.

Two accessors classify the terminal state: `patch_apply_reject(&state)` gives the
reject reason, and `patch_apply_flash_touched(&state)` reports whether any flash
write happened during the run (like `patch_apply_reject`, it compiles out when
unused). The terminal-state matrix:

| Terminal state | Flash content | Recovery |
| -------------- | ------------- | -------- |
| `DONE` | new image fully written, both CRCs verified | boot the new image |
| `ERROR`, flash untouched (`patch_apply_flash_touched(&state) == 0`) | old image intact, still bootable | fix the cause, retry with a corrected patch |
| `ERROR`, flash touched (`patch_apply_flash_touched(&state) != 0`) | partially overwritten — image destroyed | bootloader recovery (e.g. full reflash) |
| `ERROR` at the final `CRC32(to)` gate | fully written, wrong image | bootloader recovery (e.g. full reflash) |

`REJ_RESOURCE` (a decoder resource cap was exceeded) can raise mid-apply, so
it is NOT simply "rebuild with larger caps and retry": check
`patch_apply_flash_touched(&state)` first. If flash was touched, the device image is
already partially overwritten and must be recovered before any retry — a
larger-capped build fixes the cap, but only a recovered or still-intact image
can accept the retried patch (retrying the same patch on the half-written
image rejects cleanly at `CRC32(from)`). `REJ_CORRUPT` means a malformed,
truncated, or wrong-image patch; with flash untouched it is safe to re-request
the transfer and retry. A silent hardware write failure lands in the same
`REJ_CORRUPT` bucket but with flash TOUCHED — a fully-written, wrong image
caught at the final `CRC32(to)` gate (the last matrix row) — so a `REJ_CORRUPT`
with flash touched warrants a driver-level write check (see the flash-primitive
contract above), not just a re-transfer.

Do not run two decodes concurrently against one flash image. Do not start a new
patch until the previous patch has reached `DONE` or `ERROR` and the bootloader
has chosen a recovery path.

## Build-Time Contract

**Target family define (mandatory).** Define `CORTEX_M0` for BOTH the encoder
build and the decoder TU — the build fails with a clear `#error` without it (the
guard is in `patch_config.h`, reached transitively from `patch_apply.h` via
`rc_models.h`), so an encoder/decoder pair can never silently disagree about the
target family. The define is a build-time wire-contract ASSERTION only: nothing
is conditionally compiled on it. There is no Thumb-2 code path to select — the
encoder's ARM field scanner recognizes only the Thumb-1/ARMv6-M BL long-branch
and LDR-literal encodings, unconditionally, and the decoder derives exactly those
fields. `CORTEX_M4` is reserved for a future Thumb-2 wire revision that would
change the wire format; it is currently rejected at compile time by a second
`#error` in `patch_config.h`.

The decoder defaults and wire-model helpers are shared through
`src/patch_config.h` and `src/rc_models.h`; `patch_apply.h` is the only source
header application code includes. To ship one physical public header, run:

```sh
make decoder-header
```

The generated `artifacts/patch_apply_single.h` inlines the source header set and
must not include any local decoder support header. `make check-decoder-contract`
compiles both packaging forms and verifies the generated header without `-Isrc`.
The encoder uses the same source headers, and model/golden gates enforce
bit-exactness.

The LZ window `WINDOW_LOG` is a single shared define in `src/patch_config.h`, used by
both the decoder and the encoder's distance coding, so the two cannot disagree.
The production default is `WINDOW_LOG=10`, and `make gate` verifies it.

**NVM page window (encoding-affecting).** The decoder keeps its last
`OUTROW_DEPTH` output pages (of `OUTROW` bytes) uncommitted in RAM, and the
encoder's plans exploit the fact that the OLD flash content of uncommitted
pages is still physically readable (journal-free reads behind the write
frontier). `OUTROW` is also the public flash-program page size.
`OUTROW` and `OUTROW_DEPTH` are single shared defines used by both
the encoder's `row_covered` oracle and the decoder — production default
256 x 2 (512 B of page buffers in `.bss`) — so the window assumption matches by
construction. A hardware page-size change therefore requires matching encoder
and decoder builds. A deeper page ring remains a monotone-safe superset; a
smaller window rejects at the `CRC32(to)` gate rather than silently accepting a
wrong image.

Resource caps such as `JSLOTS`, `DR_KCAP_BL`, `DR_KCAP_EX`, and `OPC_CAP` are
intentional reject limits on the decoder. Each is a single shared define in
`src/patch_config.h`, so the encoder plans against the exact same cap (retune
the one define to move both sides together)
and degrades gracefully instead of refusing where the plan allows it: reads
that would overflow the journal budget ship as plain extra bytes, and ops
whose per-op corrections exceed `OPC_CAP` are split — worse compression, never
a bad blob. A pair is refused only when no plan variant fits any cap. Raising
a cap costs SRAM and must be followed by the plain release gate:

```sh
make gate
```

Do not use deployment-only CFLAGS to pass the SRAM gate. The default build must
remain representative of the shipping decoder.

## Flash And Recovery Policy

A1 does not provide power-fail rollback or resume, and never will — this is a
permanent non-goal (decision recorded 2026-07-02), not a missing feature. Resume
is impossible by construction: the decode state is volatile. The never-evict
journal (the preserved source bytes the output frontier has already overwritten
in flash), the adaptive entropy-model state, and the output page cache all live
only in RAM and cannot be reconstructed mid-stream after power loss — the
source image below the write frontier is already destroyed. Do not attempt to
retrofit checkpointing.

If power is lost during apply, the host or bootloader must detect the
interrupted state and recover by full reflash or another product-defined
recovery path. A retry of the same patch on the interrupted image rejects
cleanly at the `CRC32(from)` gate with no further flash writes, which is the
supported detection mechanism.

The host backend in `src/patch_host_backend.c` is a verification harness and NVM
emulator. It is useful as a reference for driving the push API and collecting
NVM metrics, but it is not the device integration layer.
