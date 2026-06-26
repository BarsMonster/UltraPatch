#!/bin/bash
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ARMGCC=${ARMGCC:-"$ROOT/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi"}
export PATH="$HOME/.local/bin:$ARMGCC/bin:$PATH"
cd "$ROOT/artifacts/pic"
A=gcc-nopic/base.bin; B=gcc-nopic/mod.bin; AE=gcc-nopic/base.elf; BE=gcc-nopic/mod.elf
MEM=237568          # firmware region 0x2000..0x3C000
tosize=$(stat -c%s "$B")

mkmem() { python3 -c "
import sys
d=open('$A','rb').read()
open('/tmp/mem.bin','wb').write(d + b'\xff'*($MEM-len(d)))
"; }

verify() { # patchfile -> echoes OK/FAIL
  mkmem
  detools apply_patch_in_place /tmp/mem.bin "$1" >/dev/null 2>&1 || { echo FAIL-apply; return; }
  python3 -c "
to=open('$B','rb').read(); mem=open('/tmp/mem.bin','rb').read($tosize)
print('OK' if mem==to else 'FAIL-cmp')
"
}

echo "reference: sequential bsdiff arm-cortex-m4 heatshrink(w12/l11) = 1857 B"
echo
echo "=== IN-PLACE: heatshrink(w12/l11) + arm-cortex-m4, sweep segment-size (RAM-neutral) ==="
printf "%-10s %10s  %s\n" segment patchB verify
for seg in 256 512 1024 2048 4096 8192 16384; do
  detools create_patch_in_place --memory-size $MEM --segment-size $seg \
    --data-format arm-cortex-m4 --from-elf-file $AE --to-elf-file $BE \
    -c heatshrink --heatshrink-window-sz2 12 --heatshrink-lookahead-sz2 11 \
    "$A" "$B" /tmp/ip.patch >/dev/null 2>&1 || { printf "%-10s %10s\n" $seg ERR; continue; }
  printf "%-10s %10s  %s\n" $seg "$(stat -c%s /tmp/ip.patch)" "$(verify /tmp/ip.patch)"
done
echo
echo "=== effect of the arm-cortex-m4 transform on in-place (segment=1024) ==="
for df in none arm; do
  if [ $df = none ]; then DFARGS=""; else DFARGS="--data-format arm-cortex-m4 --from-elf-file $AE --to-elf-file $BE"; fi
  detools create_patch_in_place --memory-size $MEM --segment-size 1024 $DFARGS \
    -c heatshrink --heatshrink-window-sz2 12 --heatshrink-lookahead-sz2 11 \
    "$A" "$B" /tmp/ip2.patch >/dev/null 2>&1
  printf "  data-format=%-4s : %8s B  %s\n" $df "$(stat -c%s /tmp/ip2.patch)" "$(verify /tmp/ip2.patch)"
done
