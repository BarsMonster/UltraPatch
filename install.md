# Install Requirements

On Debian or Ubuntu, install the host compilers, Cortex-M0+ toolchain, and build
helpers:

```sh
apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
  binutils-arm-none-eabi \
  gcc \
  gcc-arm-none-eabi \
  git \
  libc6-dev \
  libnewlib-dev \
  make \
  python3
```

Build and verify from the repository root:

```sh
make
make gate
```

The default host executable is `.build/ultrapatch`. Obtain the exact executable
selected by the current Make arguments with:

```sh
make -s host-tool-path
```

Use that printed path for both host CLI modes:

```sh
tool=$(make -s host-tool-path)
"$tool" [--encode] <from_image> <to_image> <patch>
"$tool" --decode <image> <patch>
```

`--decode` applies the patch to the host image file in place. The CLI replaces the file only after
the production decoder accepts the complete patch; a rejected or truncated patch leaves the file
unchanged. Device flash updates follow the separate recovery contract in
[`docs/device-integration.md`](docs/device-integration.md).

Do not assume a root-level `./ultrapatch`. For parallel compiler or measurement
runs, pass a distinct `BUILD_DIR` to every command in each run.

For a release, run the command below from a clean `main` checkout and follow
[the release checklist](docs/release-checklist.md):

```sh
make gate
```
