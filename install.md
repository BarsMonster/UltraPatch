# Install Requirements

Packages needed to run the full A1 verification, including ARM SRAM and divide-free checks:

```sh
apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
  gcc-arm-none-eabi \
  binutils-arm-none-eabi \
  make \
  python3
```

Optional but useful for local inspection:

```sh
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
  clang \
  file \
  git \
  ripgrep
```

Verification commands that need the ARM toolchain:

```sh
cd /ai_sw/v3/nvm/hybrid12k
arm-none-eabi-gcc -mcpu=cortex-m0plus -mthumb -Os -DRC_V3_ARM -I c -c c/rc_v3.c -o /tmp/rc_v3_arm.o
arm-none-eabi-size /tmp/rc_v3_arm.o
sh tools/check_divfree.sh
```

Known-good output after the A1 state-packing pass:

```text
W=10: text=5780 data=0 bss=11040
W=11: text=5772 data=0 bss=12064
check_divfree.sh: HW udiv/sdiv=0, soft-divide calls(total)=2, PASS
```

Note: in this container, `sudo` is blocked by the no-new-privileges setting. Run the install commands as root, or use a base image/container where package installation is allowed.
