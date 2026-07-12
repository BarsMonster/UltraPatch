# Device Integration Contract

The production decoder is header-only. Install `patch_apply.h`, `rc_models.h`,
and `patch_config.h` together, then include only `patch_apply.h` from integration
code. The entrypoint uses normal local includes for its two companion headers;
it is not required to be a self-contained physical file.

The decoder requires explicit `PATCH_IMAGE_BASE` and `PATCH_IMAGE_CAPACITY` and
the two flash primitives below.
The decoder owns no global static state and uses no heap; the integrator owns
the `PatchApply` state object so the Cortex-M0+ SRAM gate remains meaningful.

The repository also builds `ultrapatch`, a unified host CLI. Its default mode is
encode, and its `--decode` mode is a reference/debug path that uses the same host
backend as encoder self-verification. Repository builds place it at
`.build/<profile-id>/ultrapatch`; `make host-tool-path` prints the exact selected
path, and Make exports that path to its test processes. That host CLI is not part
of the device decoder artifact; embedded integrations include only the decoder
header set.

The decoder owns the whole patch blob: envelope parsing, both CRC gates, the
apply direction, and the image span are all derived internally from the pushed
bytes. The integrator does not parse the envelope and cannot get it wrong.

## Ownership

Include the decoder from one update translation unit and allocate one
caller-owned `PatchApply` object for the run. Application code includes only
`patch_apply.h`; the support headers are included by it. Entropy models, the
journal arena, and the output page cache live inside
the `PatchApply` object. There is no coroutine, no fiber, no private decode
stack, and no heap allocation -- the decode runs on the caller's stack plus the
caller-owned state object.

The decoder implementation has internal header-only linkage. To avoid
accidental duplicate decoder text in final firmware, include the decoder from a
single update `.c` file and route other application modules through that update
module's local API.

### C identifier namespace

The public decoder names are `PatchApply`, `PatchApplyResult`, `PatchPull`, the
`patch_apply_*` entry points and accessors, `PATCH_APPLY_*`, `PATCH_PULL_*`, and
`REJ_*` results, the required `flash_read` / `flash_write_page` primitives, and
the documented build/configuration macros in this contract. Decoder-specific
private identifiers use the `up_` / `UP_`
prefix; shared encoder/decoder wire-model helpers deliberately use the established
`rc_` / `RC_` internal namespace. Private decoder macros are undefined at
the end of the public header; namespaced include guards remain defined normally.

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
| Relocatable static-wrapper object | 6309 B | 0 B | 9752 B |
| No-startup linked image | 6889 B | 0 B | 9752 B |

The linked text includes minimal `flash_read`/`flash_write_page` stubs
and the pulled `memcpy`, `memmove`, and `memset` implementations. It excludes
vector tables, CRT initialization, syscalls, board support, and real flash
driver/callback code. Both state measurements enforce the immutable 12288-byte
`.bss` product cap. The generic caller-owned call shape shown below is the public
API contract, but it is not the shape used for these pinned ARM ratchets;
product firmware using a different wrapper, library, or platform code should
size its final image in its own build.

Before including the decoder, define `PATCH_IMAGE_BASE` as the absolute device
address of the patchable image and `PATCH_IMAGE_CAPACITY` as the physical byte
capacity starting there. Both must be aligned to `OUTROW`, capacity must be
nonzero, and the complete `[base, base + capacity)` range must fit in `uint32_t`
address space; the header enforces these conditions at compile time.

`MAX_IMAGE` remains the encoder/decoder shared plausibility ceiling for envelope
sizes. `PATCH_IMAGE_CAPACITY` is decoder-only deployment geometry and may be
smaller. After parsing the two logical sizes, the decoder rejects when the
page-rounded `max(from_size, to_size)` exceeds capacity. This happens before the
`CRC32(from)` scan, so an oversized untrusted envelope cannot cause a flash read
or write outside the configured partition.

That guard protects only the untouched, pre-apply state. The format has no
rollback, checkpoint, or recovery protocol. Once any page has been written, a
later decoder, transport, power, or flash failure requires a full external
reflash. Detecting such a failure earlier cannot restore the old image and is
not a decoder design objective.

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
3. **Write-failure policy.** `flash_write_page` intentionally returns `void`;
   there is no decoder-level `REJ_IO`. The flash driver may synchronously verify
   and retry a page internally before returning, where hardware-specific status
   is still actionable. Once any page has been written, however, the old image
   cannot be reconstructed. Reporting an exhausted program failure to the
   decoder earlier would not create a recovery path, so call-site error
   detection is not a decoder design goal. Any unrecoverable write failure after
   the image is touched requires a full external reflash. If a failed write
   returns silently, the final `CRC32(to)` may detect the wrong image as
   `REJ_CORRUPT`; that CRC is an integrity verdict, not rollback or recovery.

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

Supply a `PatchPull` callback that returns `PATCH_PULL_BYTE` with the next blob
byte (it may poll a UART/BLE buffer internally), or `PATCH_PULL_END` to
abort/end a truncated source. Allocate a `PatchApply`, and call
`patch_apply_run(&state, callback, ctx)`. The whole decode runs synchronously on the
caller's stack: no coroutine, no fiber stack, no platform context-switch code,
no compiler-specific stack sizing.

```c
/* Return PATCH_PULL_BYTE and write one byte to *out, or PATCH_PULL_END to abort/end.
 * May block internally (for example, wait on a UART ring) before returning. */
int next_byte(void *ctx, uint8_t *out);

PatchApply state;
PatchApplyResult result = patch_apply_run(&state, next_byte, &my_ctx);
if (result != PATCH_APPLY_DONE) {
    /* Inspect patch_apply_reject() and patch_apply_flash_touched(). */
}
```

`src/patch_host_backend.c` (`PullCtx` / `pull_next`) is a minimal reference
implementation of the callback. The callback type remains `int`-returning for
simple driver integration, but only the exact value `PATCH_PULL_BYTE` (`1`) is
accepted as a byte. `PATCH_PULL_END` (`0`), negative errors, and every other
value abort the stream. `patch_apply_run` returns `PATCH_APPLY_DONE` (`0`) on
success and `PATCH_APPLY_ERROR` (nonzero) on failure. GNU-compatible builds warn
when this result is discarded; portable fallback builds retain the same runtime
contract without requiring compiler attributes.

**`patch_apply_run` blocks the caller for the whole decode** — two full-image
CRC passes (`CRC32(from)` before the first write and `CRC32(to)` at the end)
plus the entire apply — and never returns partway, with no internal watchdog
hook. Pet the watchdog INSIDE the integrator's byte callback and flash
primitives (the per-byte and per-row call-outs the decode already makes), not
around the single `patch_apply_run` call.

### Aborting a transfer

To abandon a decode mid-blob — a lost link, a user cancel, a transport timeout —
the byte callback returns `PATCH_PULL_END`. The decoder latches EOF, zero-fills any later range reads,
and terminates in BOUNDED time (an `O(to_size)` wind-down at worst, never an
unbounded hang) with `PATCH_APPLY_ERROR`. If the abort occurs before the first
page write, the original image remains intact and a later apply may start from
that original image. If the abort occurs after the first page write, the image
must be fully reflashed externally. The patch cannot be resumed. The
truncated-blob cases in `make gate` drive this bounded reject path.

## Stack Budget

The whole decode runs on the caller's stack (no fiber since commit 44eee88), so the
integrator must reserve stack for it on top of the interrupt/RTOS frames. These stack
numbers exclude the `PatchApply` object itself; place that object in static/noinit storage
if the bootloader stack is tight. The statically measured Cortex-M0+ bounds are wrapper
shape-specific:

| Integration shape (gcc `-O2`, Cortex-M0+ `-mthumb`) | Worst-case caller stack | Gated ceiling |
| --------------------------------------------------- | ----------------------- | ------------- |
| Static `PatchApply` object; `rcv3_run(next, ctx)` | **408 B** | 480 B |
| Caller-owned pointer; `rcv3_run(state, next, ctx)` | **424 B** | 480 B |

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
   image was written and both `CRC32(from)` and `CRC32(to)` verified. `DONE` is
   zero; every error is nonzero.

Two accessors classify the terminal state: `patch_apply_reject(&state)` gives the
reject reason, and `patch_apply_flash_touched(&state)` reports whether any flash
write happened during the run (like `patch_apply_reject`, it compiles out when
unused). The required-action matrix:

| Terminal state | Flash content | Required action |
| -------------- | ------------- | --------------- |
| `DONE` | new image fully written, both CRCs verified | boot the new image |
| `ERROR`, flash untouched (`patch_apply_flash_touched(&state) == 0`) | old image intact, still bootable | correct the cause; a later apply may start from the intact original image |
| `ERROR`, flash touched (`patch_apply_flash_touched(&state) != 0`) | partially overwritten; old image destroyed | full external reflash |
| `ERROR` at the final `CRC32(to)` gate | fully written, wrong image | full external reflash |

`REJ_RESOURCE` (a decoder resource cap was exceeded) can raise mid-apply, so
check `patch_apply_flash_touched(&state)` before taking any next action. If flash
is untouched, a corrected build or authenticated transfer may start later from
the intact original image. If flash was touched, perform a full external
reflash first. Never run the patch again against the partially overwritten
image. A later `CRC32(from)` mismatch would merely reject that wrong starting
image; it is not an interruption detector or a recovery mechanism.

`REJ_CORRUPT` means a malformed, truncated, or wrong-image patch; a silent
hardware write failure can land in the same
`REJ_CORRUPT` bucket but with flash TOUCHED — a fully-written, wrong image caught
at the final `CRC32(to)` gate (the last matrix row). Driver-level verification
may identify or retry the failing page sooner, but it does not change the
terminal policy: once flash was touched, recover by full external reflash, not
by re-transfer or decoder retry.

Do not run two decodes concurrently against one flash image. Do not start a new
patch after a touched error; externally reflash the complete image first.

## Build-Time Contract

**Repository build profile.** The persistent host executable and its profile
manifest are isolated beneath `.build/<profile-id>/`. The profile identifier
covers the host compiler identity and every effective build flag, so changing
one selects a different output directory instead of silently reusing a stale
host tool. Use `make host-tool-path` when a repository-built encoder path is
needed; do not assume `./ultrapatch` or guess a profile directory.

`make check-build-profile` is a separate collision regression for this output
isolation. Release builds additionally require the selected host and Arm GCC
drivers plus their named `cc1`, `collect2`, assembler, and linker programs;
required Clang and named binutils; `libc.a`/`libgcc.a` content hashes; clean
compiler environment; effective compile/link flags; and the configured CI
container digest recorded in `toolchains/release-profile.json`. `make gate`
validates that descriptor before forking its verification legs and reports
`release_profile` in its consolidated output. The local release driver verifies
a fresh archive of one clean `main` commit; only successful push CI at the exact
commit runs authoritatively inside the pinned container. An accepted identity,
archive, environment policy, or flag update is a deliberate release change and
must be reviewed with all footprint, wire, and round-trip evidence. The profile
scope is the selected drivers/subtools/binutils and named runtime archives, not
a recursive dynamic-library closure or a claim of byte-for-byte reproducibility
across hosts.

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

**Shared override rule (mandatory).** Encoder and decoder builds **MUST** use
the exact same wire macro names with the exact same values. In the repository,
put every explicit wire override in the one `WIRE_CONFIG_FLAGS` value, for
example:

```sh
make WIRE_CONFIG_FLAGS='-DCORTEX_M0 -DWINDOW_LOG=10 -DJSLOTS=768u'
```

This rule covers `WINDOW_LOG`, `JSLOTS`, `OPC_CAP`, `OUTROW`,
`OUTROW_DEPTH`, `DR_KCAP_BL`, `DR_KCAP_EX`, the target family, and any
wire-affecting model override. Do not create encoder/decoder aliases or pass an
override to only one side. `src/patch_config.h` remains the single source of
defaults.

`PATCH_IMAGE_BASE` and `PATCH_IMAGE_CAPACITY` are the explicit exceptions: they
are decoder-only partition configuration and are not wire macros. Set them for
the decoder TU, never mirror them into an encoder-specific profile. Repository
host and ARM decoder harnesses keep them separately in
`DECODER_CONFIG_FLAGS='-DPATCH_IMAGE_BASE=0u -DPATCH_IMAGE_CAPACITY=67108864u'`.

The decoder defaults and wire-model helpers are shared through
`src/patch_config.h` and `src/rc_models.h`; `patch_apply.h` is the only header
application code includes. Ship all three files in one include directory. The
gate copies that header set into an isolated integration directory and compiles
a consumer that includes only `patch_apply.h`, then runs the API, portability,
model, wire, ARM-footprint, and stack checks once against the real headers. The
model/golden gates continue to enforce encoder/decoder bit-exactness.

The LZ window `WINDOW_LOG` is a single shared define in `src/patch_config.h`, used by
both the decoder and the encoder's distance coding; matching builds therefore use the
same name and value.
The production default is `WINDOW_LOG=10`, and `make gate` verifies it.

**NVM page window (encoding-affecting).** The decoder keeps its last
`OUTROW_DEPTH` output pages (of `OUTROW` bytes) uncommitted in RAM, and the
encoder's plans exploit the fact that the OLD flash content of uncommitted
pages is still physically readable (journal-free reads behind the write
frontier). `OUTROW` is also the public flash-program page size.
`OUTROW` and `OUTROW_DEPTH` are shared defines used by both
the encoder's `row_covered` oracle and the decoder — production default
256 x 2 (512 B of page buffers in `.bss`) — so the window assumption matches by
construction. A hardware page-size change therefore requires matching encoder
and decoder builds. A deeper page ring remains a monotone-safe superset; a
smaller window rejects at the `CRC32(to)` gate rather than silently accepting a
wrong image.

Resource caps such as `JSLOTS`, `DR_KCAP_BL`, `DR_KCAP_EX`, and `OPC_CAP` are
intentional reject limits on the decoder. Each is a shared define in
`src/patch_config.h`, so the encoder plans against the exact same cap (retune
through `WIRE_CONFIG_FLAGS` to move both sides together)
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

## Flash And Reflash Policy

A1 provides no power-fail rollback or resume. It deliberately has no persistent
in-progress marker, checkpoint, journal replay, or boot-time interruption
detection protocol. The decode state is volatile: preserved source bytes,
adaptive entropy models, and output page buffers live only in RAM, while the
source image below the write frontier has already been destroyed. Do not add a
resume or marker scheme to the decoder.

Any transport loss, decoder error, reset, or power loss after the first page
write requires a full external reflash. If power is lost while an apply may have
been writing, treat the image as touched and fully reflash it; do not retry or
resume the patch. `CRC32(from)` and `CRC32(to)` validate the image seen during a
normal decoder invocation. They are not persistent interruption detectors and
do not provide recovery.

The host backend in `src/patch_host_backend.c` is a verification harness and NVM
emulator. It is useful as a reference for driving the push API and collecting
NVM metrics, but it is not the device integration layer.
