#!/bin/sh
# vp-5: assert the DECODE HOT PATH is divide-free (hard requirement #5; Cortex-M0+/ARMv6-M has no
# hardware divide). Software-divide helpers (__aeabi_*div*) are permitted ONLY in the once-per-decode
# init path (rcv3_init/decoder_init seeds the literal trees from parity histograms); a HW udiv/sdiv
# must NEVER appear. Note: a naive `grep udiv` also matches `__aeabi_uidiv` (false positive) — we match
# the whole mnemonic after a tab, and bucket soft-divide call sites by their enclosing function.
set -e
OBJ="${1:-/tmp/rc_v3_divfree.o}"
SRC="${2:-c/rc_v3.c}"
: "${CC:=arm-none-eabi-gcc}"
"$CC" -mcpu=cortex-m0plus -mthumb -Os -DRC_V3_ARM -I c -c "$SRC" -o "$OBJ"
DIS=$(arm-none-eabi-objdump -d "$OBJ")
# HW divide instructions (whole mnemonic, tab-delimited) — must be 0 on this core
hw=$(printf '%s\n' "$DIS" | grep -Ec '	(udiv|sdiv)([[:space:]]|$)' || true)
# soft-divide call sites bucketed by enclosing function; any OUTSIDE an *init* function fails
bad=$(printf '%s\n' "$DIS" | awk '
  /^[0-9a-f]+ <.*>:/ { fn=$2; sub(/:$/,"",fn); sub(/^</,"",fn); sub(/>$/,"",fn); next }
  /__aeabi_(u?idiv|uidivmod|idivmod)/ { if (fn !~ /init/) print "    "fn": "$0 }
')
total=$(printf '%s\n' "$DIS" | grep -Ec '__aeabi_(u?idiv|uidivmod|idivmod)' || true)
echo "divide-free check: HW udiv/sdiv=$hw  soft-divide calls(total)=$total"
if [ -n "$bad" ]; then echo "  hot-path (non-init) divide sites:"; printf '%s\n' "$bad"; fi
if [ "$hw" -ne 0 ] || [ -n "$bad" ]; then echo "FAIL: req#5 (divide-free hot path) violated"; exit 1; fi
echo "PASS: req#5 — decode hot path is divide-free (soft divides confined to init)"
