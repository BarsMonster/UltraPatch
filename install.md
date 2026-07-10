# Install Requirements

Packages needed to build and inspect the final A1 C encoder/decoder on a
Debian/Ubuntu system:

```sh
apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
  gcc \
  gcc-arm-none-eabi \
  binutils-arm-none-eabi \
  make \
  python3
```

The host `gcc` builds the `ultrapatch` encoder CLI and the host decoder wrapper
(the Makefile uses `$(CROSS_COMPILE)gcc`, i.e. plain `gcc`, when `CROSS_COMPILE`
is empty); `python3` drives the single-header generator and the stack-bound
analysis that several `make gate` legs run.

Optional but useful for local inspection:

```sh
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
  clang \
  file \
  git \
  ripgrep
```

Verification commands, run from the repository root:

```sh
make
make check
make check-assets
make check-arm
make gate
```

`make check-arm` cross-builds the Cortex-M0+ decoder object and prints and
gates both its relocatable and no-startup linked ARM `text`/`data`/`bss` sizes
against the Makefile pins. The link uses an explicit FLASH/RAM layout, minimal
platform stubs, and only the runtime-library members pulled by the decoder; it
does not include CRT/startup or board code. A decoder compile needs both
`-DCORTEX_M0` and an aligned `PATCH_IMAGE_BASE`; repository host/ARM harnesses
explicitly use `-DPATCH_IMAGE_BASE=0u`. Missing, misaligned, or uint32-overflowing
image bases are compile-time errors. This check also runs as one leg of `make gate`.
