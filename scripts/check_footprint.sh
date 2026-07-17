#!/bin/sh
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

# Aggregate decoder resource gate. Measure both copy implementations because their cost depends
# on what the containing firmware already links. Flash/state use the reference static wrapper;
# stack is the maximum across the static and caller-owned integration shapes.
set -eu

# The Makefile is the single owner of the ARM measurement configuration and passes every knob
# via env (see check-footprint-internal). Require them here instead of re-defaulting, so this
# script cannot silently measure a stale configuration when run outside `make check-footprint` --
# the same env contract check_corpus.sh already enforces for ULTRAPATCH.
: "${ARM_CC:?run via make check-footprint}"
: "${ARM_SIZE:?run via make check-footprint}"
: "${ARM_OBJDUMP:?run via make check-footprint}"
: "${ARM_DEC_FLAGS:?run via make check-footprint}"
: "${ARM_OBJECT_OPT:?run via make check-footprint}"
: "${DECODER_INTEGRATION_TU:?run via make check-footprint}"
BASE_FOOTPRINT_FLASH=5264
BASE_FOOTPRINT_STATE=6716
BASE_FOOTPRINT_STACK=432
ARM_BSS_HARD_CAP=12288

# Temp dir cleaned on normal exit and on the Makefile time-cap group-kill. dash does not run an
# EXIT trap for an untrapped fatal signal, so TERM/INT clean up, restore the default disposition,
# and re-raise so the shell dies by the signal (exit status preserved).
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
trap 'rm -rf "$tmp"; trap - TERM INT EXIT; kill -s TERM "$$"' TERM
trap 'rm -rf "$tmp"; trap - TERM INT EXIT; kill -s INT "$$"' INT

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
