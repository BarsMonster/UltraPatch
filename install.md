# Install Requirements

Packages needed to build and inspect the final A1 C encoder/decoder on a
Debian/Ubuntu system:

```sh
apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
  gcc-arm-none-eabi \
  binutils-arm-none-eabi \
  make
```

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
make gate
arm-none-eabi-gcc -mcpu=cortex-m0plus -mthumb -Os -DRC_V3_ARM -I src -x c -c src/patch_apply.h -o patch_apply_arm.o
arm-none-eabi-size patch_apply_arm.o
```

Known-good ARM object size at `SA_W=10`:

```text
text=4748 data=0 bss=10272
```
