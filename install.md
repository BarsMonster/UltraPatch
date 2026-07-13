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
make gate
```

The default host executable is `.build/ultrapatch`. Obtain the exact executable
selected by the current Make arguments with:

```sh
make -s host-tool-path
```

Do not assume a root-level `./ultrapatch`. For parallel compiler or measurement
runs, pass a distinct `BUILD_DIR` to every command in each run.

For a release, run the full sequence below from a clean `main` checkout and follow
[the release checklist](docs/release-checklist.md):

```sh
make gate
make check-decoder-sanitize
make check-encoder-sanitize
make check-clang
```
