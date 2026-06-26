#!/bin/bash
# Wrapper: compile .o with clang (arm-none-eabi target), link with arm-none-eabi-gcc.
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ARMGCC=${ARMGCC:-"$ROOT/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi"}
export PATH="$ARMGCC/bin:$PATH"

is_compile=0
for a in "$@"; do [ "$a" = "-c" ] && is_compile=1; done

if [ "$is_compile" = 1 ]; then
  # strip flags clang doesn't accept for this target
  args=()
  for a in "$@"; do
    case "$a" in
      -fno-diagnostics-show-caret|-funsigned-bitfields) ;;  # drop
      *) args+=("$a") ;;
    esac
  done
  exec clang --target=arm-none-eabi \
    --sysroot="$ARMGCC/arm-none-eabi" --gcc-toolchain="$ARMGCC" \
    -isystem "$ARMGCC/arm-none-eabi/include" \
    -Wno-unknown-warning-option -Wno-multilib-not-found -Qunused-arguments \
    "${args[@]}"
else
  exec arm-none-eabi-gcc "$@"
fi
