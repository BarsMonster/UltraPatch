# Install Requirements

On Debian or Ubuntu, install the host compilers, Cortex-M0+ toolchain, and build
helpers:

```sh
apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
  binutils-arm-none-eabi \
  clang \
  gcc \
  gcc-arm-none-eabi \
  git \
  libnewlib-arm-none-eabi \
  make \
  python3
```

Build and verify from the repository root:

```sh
make
make check
make check-build-profile
make gate
```

Host outputs are profile-scoped under `.build/`. Obtain the executable selected
by the current Make arguments with:

```sh
make -s host-tool-path
```

Do not assume a root-level `./ultrapatch` or guess a profile directory.

For a release, run the preflight below from a clean `main` checkout and follow
[the release checklist](docs/release-checklist.md):

```sh
/usr/bin/python3 scripts/release_gate.py
```
