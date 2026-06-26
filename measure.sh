#!/bin/bash
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ARMGCC=${ARMGCC:-"$ROOT/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi"}
export PATH="$ARMGCC/bin:$HOME/.local/bin:$PATH"
cd "$ROOT/artifacts/pic"
printf "%-12s %8s %8s %7s %8s %9s %9s %11s\n" CONFIG baseB modB rawDiff% diffRuns zstdPF dt-bsdiff dt-armcm4
for cfg in gcc-nopic gcc-pic clang-nopic clang-pic clang-ropi; do
  A=$cfg/base.bin; B=$cfg/mod.bin; AE=$cfg/base.elf; BE=$cfg/mod.elf
  bs=$(stat -c%s $A); ms=$(stat -c%s $B)
  common=$(( bs<ms ? bs : ms ))
  d=$(cmp -l "$A" "$B" 2>/dev/null | wc -l)
  pct=$(awk "BEGIN{printf \"%.1f\", 100*$d/$common}")
  runs=$(cmp -l "$A" "$B" 2>/dev/null | awk '{o=$1-1; if(o!=p+1)r++; p=o} END{print r+0}')
  # generic LZ delta
  zstd -q -19 --long=27 --patch-from="$A" "$B" -o /tmp/z.zst -f 2>/dev/null
  z=$(stat -c%s /tmp/z.zst)
  # detools generic bsdiff + heatshrink (no reloc knowledge)
  detools create_patch -a bsdiff -c heatshrink "$A" "$B" /tmp/p1 >/dev/null 2>&1 && db=$(stat -c%s /tmp/p1) || db=ERR
  # detools relocation-aware arm-cortex-m4 + heatshrink
  detools create_patch --data-format arm-cortex-m4 --from-elf-file "$AE" --to-elf-file "$BE" \
     -c heatshrink "$A" "$B" /tmp/p2 >/dev/null 2>&1 && da=$(stat -c%s /tmp/p2) || da=ERR
  printf "%-12s %8d %8d %7s %8d %9d %9s %11s\n" "$cfg" "$bs" "$ms" "$pct" "$runs" "$z" "$db" "$da"
done
