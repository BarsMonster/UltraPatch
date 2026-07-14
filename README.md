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
produced output, and adaptive entropy coding. Relocation-aware source deltas,
the choice among competing plans, and the two-page delayed-write window leave
few bytes that need this treatment, so the decoder does not need a general
old-byte journal.

Every generated patch is applied by the production decoder on the host before
it is accepted. On the device, source CRC is checked before the first write,
each changed flash page is erased and programmed at most once, and target CRC
is checked after the final buffered pages are committed.

## Build and CLI

```sh
make

tool=$(make -s host-tool-path)
"$tool" [--encode] <from_image> <to_image> <patch>
"$tool" --decode <image> <patch>
"$tool" --help
```

The encoder accepts image files, not directories. Product encoding requires each
image to have its authentic matching same-basename `.elf` beside it; the encoder
uses ELF load and symbol information for relocation-aware planning, and
pre-extracted offsets are not an acceptable product artifact. The CLI still
tolerates absent ELF for non-product regression inputs, including the foreign
corpus; universal enforcement is deferred until matching foreign ELFs are
available.
The default host executable is `.build/ultrapatch`; always use
`make host-tool-path` to obtain its exact path instead of assuming a root-level
`./ultrapatch`. Set `BUILD_DIR` to a private directory when parallel builds or
measurements need isolation.

See [install.md](install.md) for packages and common commands.

## Device decoder

Define the patch partition through `PATCH_IMAGE_BASE` and
`PATCH_IMAGE_CAPACITY`, provide the required page-flash functions, allocate a
caller-owned `PatchApply`, and invoke `patch_apply_run()`. The decoder uses no
heap or global static state.

Read [the device integration contract](docs/device-integration.md) before
integrating it into a bootloader. It defines the flash, memory, callback,
authentication, watchdog, concurrency, and recovery requirements.

### Decoder configuration

`PATCH_IMAGE_BASE` and `PATCH_IMAGE_CAPACITY` are required and describe the
physical patch partition. Define them before including `patch_apply.h`.

The default decoder computes reflected IEEE CRC-32 in a tableless internal
routine, using no additional buffer. Platforms with a compatible CRC library
or hardware peripheral should define `CRC32_DECODE(start,end)` before including
any UltraPatch header (normally `patch_apply.h`). `start` and `end` are
image-relative byte offsets describing the half-open range `[start,end)`; the
macro must return the same CRC-32 as zlib
(polynomial `0xedb88320`, initial and final XOR `0xffffffff`) and must observe
flash writes completed during the apply. For example:

```c
#include <stdint.h>
uint32_t platform_image_crc32(uint32_t start, uint32_t end);
#define CRC32_DECODE(start,end) platform_image_crc32((start),(end))
#include "patch_apply.h"
```

The decoder uses the C library's `memmove` by default. It deliberately uses
that same primitive for non-overlapping model copies as well as overlapping
shifts, avoiding the flash cost of linking both `memcpy` and `memmove`. Leave
`HAND_ROLLED_MEMMOVE` undefined when the final firmware already links
`memmove`. If it does not, define `HAND_ROLLED_MEMMOVE` before including
`patch_apply.h` to select the decoder's smaller private backward-copy loop.
The option changes code generation only, not the wire format or RAM layout;
the private byte loop may be slower, so compare the final linked image and
update latency with the intended compiler and firmware.

## Verification and release

```sh
make gate
```

The post-development release gate checks the complete corpus size regression, reference
static-wrapper flash/state, and worst supported decoder stack. Every generated corpus patch
self-verifies through the production decoder before its size is accepted. The authoritative
procedure is in [the release checklist](docs/release-checklist.md).

## License

UltraPatch is MIT licensed except where a vendored dependency states otherwise.
Vendored notices and the `enc_bsdiff.c` attribution are collected in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>.
