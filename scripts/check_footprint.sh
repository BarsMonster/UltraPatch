#!/bin/sh
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

# Aggregate decoder resource gate. Measure both copy implementations because their cost depends
# on what the containing firmware already links. Flash/state use the reference static wrapper;
# stack is the maximum across the static and caller-owned integration shapes.
set -eu

ARM_CC=${ARM_CC:-arm-none-eabi-gcc}
ARM_SIZE=${ARM_SIZE:-arm-none-eabi-size}
ARM_OBJDUMP=${ARM_OBJDUMP:-arm-none-eabi-objdump}
ARM_DEC_FLAGS=${ARM_DEC_FLAGS:-"-mcpu=cortex-m0plus -mthumb -std=c11 -DPATCH_IMAGE_BASE=8192u -DPATCH_IMAGE_CAPACITY=67108864u -I src"}
ARM_OBJECT_OPT=${ARM_OBJECT_OPT:--Os}
DECODER_INTEGRATION_TU=${DECODER_INTEGRATION_TU:-test-bench/decoder-integration.c}
BASE_FOOTPRINT_FLASH=5205
BASE_FOOTPRINT_STATE=5436
BASE_FOOTPRINT_STACK=432
ARM_BSS_HARD_CAP=12288

. ./scripts/tempdir.sh

max_flash=0
max_state=0
max_bss=0
max_stack=0
for variant in library-static library-generic hand-rolled-static hand-rolled-generic; do
	case "$variant" in
		library-static)      mode_flags=; shape_flags=-DDECODER_INTEGRATION_STATIC ;;
		library-generic)     mode_flags=; shape_flags=-DDECODER_INTEGRATION_GENERIC ;;
		hand-rolled-static)  mode_flags=-DHAND_ROLLED_MEMMOVE; shape_flags=-DDECODER_INTEGRATION_STATIC ;;
		hand-rolled-generic) mode_flags=-DHAND_ROLLED_MEMMOVE; shape_flags=-DDECODER_INTEGRATION_GENERIC ;;
	esac
	obj="$tmp/footprint-$variant.o"
	# ARM_*FLAGS are Make configuration strings and intentionally undergo word splitting.
	$ARM_CC $ARM_DEC_FLAGS $ARM_OBJECT_OPT $mode_flags $shape_flags \
		-fcallgraph-info=su -c "$DECODER_INTEGRATION_TU" -o "$obj"
	case "$variant" in
		*-static)
			set -- $($ARM_SIZE "$obj" | awk 'NR == 2 { print $1, $2, $3 }')
			if [ "$#" -ne 3 ]; then
				echo "cannot read ARM size for $variant decoder" >&2
				exit 1
			fi
			flash=$(( $1 + $2 ))
			state=$(( $2 + $3 ))
			[ "$flash" -le "$max_flash" ] || max_flash=$flash
			[ "$state" -le "$max_state" ] || max_state=$state
			[ "$3" -le "$max_bss" ] || max_bss=$3
			;;
	esac
	bound=$(OBJDUMP="$ARM_OBJDUMP" python3 scripts/stack_bound.py --quiet "$obj" |
		sed -n 's/^stack_bound_bytes=//p')
	case "$bound" in
		''|*[!0-9]*) echo "cannot read ARM stack bound for $variant decoder" >&2; exit 1 ;;
	esac
	[ "$bound" -le "$max_stack" ] || max_stack=$bound
done

echo "footprint_static_flash_bytes=$max_flash"
echo "footprint_static_state_bytes=$max_state"
echo "footprint_stack_bytes=$max_stack"
echo "footprint_bss_hard_cap=$ARM_BSS_HARD_CAP"

if [ "$max_bss" -gt "$ARM_BSS_HARD_CAP" ]; then
	echo "decoder .bss hard cap exceeded: $max_bss > $ARM_BSS_HARD_CAP" >&2
	exit 1
fi
if [ "$max_flash" -gt "$BASE_FOOTPRINT_FLASH" ]; then
	echo "decoder flash regression: $max_flash > $BASE_FOOTPRINT_FLASH" >&2
	exit 1
fi
if [ "$max_state" -gt "$BASE_FOOTPRINT_STATE" ]; then
	echo "decoder state regression: $max_state > $BASE_FOOTPRINT_STATE" >&2
	exit 1
fi
if [ "$max_stack" -gt "$BASE_FOOTPRINT_STACK" ]; then
	echo "decoder stack regression: $max_stack > $BASE_FOOTPRINT_STACK" >&2
	exit 1
fi

echo "decoder_footprint=OK (static flash $max_flash/$BASE_FOOTPRINT_FLASH, static state $max_state/$BASE_FOOTPRINT_STATE, stack $max_stack/$BASE_FOOTPRINT_STACK)"
