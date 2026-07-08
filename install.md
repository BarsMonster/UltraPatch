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
gates the authoritative ARM `text`/`data`/`bss` sizes against the Makefile
pins (the raw command needs `-DCORTEX_M0` or `rc_models.h` #errors out); it
also runs as one leg of `make gate`.
