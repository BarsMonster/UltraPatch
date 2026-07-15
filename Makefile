# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

override SHELL := /bin/sh

# A release gate must actually execute and propagate failures; dry-run, touch, and ignore-error
# modes would otherwise let Make report success without establishing the release outcome.
override CANONICAL_GOAL := $(firstword $(filter gate,$(MAKECMDGOALS)))
override CANONICAL_SHORT_FLAGS := $(filter-out -%,$(firstword $(MAKEFLAGS)))
override GATE_CONTROL_OVERRIDES := $(strip $(foreach v,MAKE MAKEFLAGS GNUMAKEFLAGS MAKECMDGOALS,$(if $(filter command line,$(origin $(v))),$(v))))
ifneq ($(CANONICAL_GOAL),)
ifneq ($(strip $(foreach f,n t i,$(if $(findstring $(f),$(CANONICAL_SHORT_FLAGS)),$(f)))),)
$(error gate rejects Make launch mode: $(CANONICAL_SHORT_FLAGS))
endif
endif

CC = $(CROSS_COMPILE)gcc
ARM_PREFIX ?= arm-none-eabi-
ARM_CC ?= $(ARM_PREFIX)gcc
ARM_SIZE ?= $(ARM_PREFIX)size
ARM_OBJDUMP ?= $(ARM_PREFIX)objdump
ARM_OBJECT_OPT ?= -Os

export LANG := C
export LANGUAGE := C
export LC_ALL := C

OPT ?= -O2
DECODER_CONFIG_FLAGS ?= -DPATCH_IMAGE_BASE=8192u -DPATCH_IMAGE_CAPACITY=67108864u
CONTRACT_FLAGS := -std=c11 -I. -Isrc -Ivendor/libdivsufsort
CFLAGS += $(CONTRACT_FLAGS)
CFLAGS += -g
CFLAGS += -Wall
CFLAGS += -Wextra
CFLAGS += -Wdouble-promotion
CFLAGS += -Wfloat-equal
CFLAGS += -Wformat=2
CFLAGS += -Wshadow
CFLAGS += -Werror
CFLAGS += $(OPT)
CFLAGS += -ffunction-sections
CFLAGS += -fdata-sections
CFLAGS += $(CFLAGS_EXTRA)
DECODER_CFLAGS = $(CFLAGS) $(DECODER_CONFIG_FLAGS)
LDFLAGS += -Wl,--gc-sections
HOST_BACKEND_DEFINES := -D_POSIX_C_SOURCE=200809L

DIVSUF := vendor/libdivsufsort/divsufsort.c
ENC_MODULE_SRCS := src/enc_util.c src/enc_bsdiff.c src/enc_field.c \
                   src/enc_rc.c src/enc_lz.c src/enc_emit.c src/enc_plan.c
HOST_BACKEND_SRC := src/patch_host_backend.c
TOOL_SRCS := src/patch_generate.c $(ENC_MODULE_SRCS) $(DIVSUF) $(HOST_BACKEND_SRC)
DECODER_PUBLIC_HDRS := src/patch_config.h src/rc_models.h src/patch_apply.h

BUILD_DIR ?= .build
ifneq ($(words $(BUILD_DIR)),1)
$(error BUILD_DIR must not contain whitespace)
endif
ifeq ($(abspath $(BUILD_DIR)),$(CURDIR))
$(error BUILD_DIR must not be the repository root)
endif
override HOST_TOOL := $(abspath $(BUILD_DIR))/ultrapatch
override ULTRAPATCH := $(HOST_TOOL)
export ULTRAPATCH

ARM_DEC_FLAGS := -mcpu=cortex-m0plus -mthumb -std=c11 $(DECODER_CONFIG_FLAGS) -I src
DECODER_INTEGRATION_TU := test-bench/decoder-integration.c

GATE_TIMEOUT ?= 80
override RELEASE_GATE_TIMEOUT := 80
CAPPED := all check-corpus check-footprint clean clean-all
.PHONY: $(CAPPED) $(addsuffix -internal,$(CAPPED)) gate gate-internal
$(CAPPED): %:
	@timeout $(GATE_TIMEOUT) $(MAKE) --no-print-directory $*-internal; s=$$?; \
	if [ $$s -eq 124 ]; then echo "Execution timelimit $(GATE_TIMEOUT) exceeded" >&2; fi; \
	exit $$s

gate:
	$(if $(GATE_CONTROL_OVERRIDES),$(error gate rejects override of Make control variable(s): $(GATE_CONTROL_OVERRIDES)))
	@mkdir -p "$(BUILD_DIR)"; build_dir=$$(mktemp -d "$(BUILD_DIR)/gate.XXXXXX"); \
	trap 'rm -rf "$$build_dir"' EXIT; \
	timeout $(RELEASE_GATE_TIMEOUT) \
	  $(MAKE) --no-print-directory BUILD_DIR="$$build_dir" gate-internal; s=$$?; \
	if [ $$s -eq 124 ]; then echo "Execution timelimit $(RELEASE_GATE_TIMEOUT) exceeded" >&2; fi; \
	exit $$s

all-internal: ultrapatch

.PHONY: host-tool-path ultrapatch FORCE
host-tool-path: $(HOST_TOOL)
	@printf '%s\n' "$(HOST_TOOL)"

ultrapatch: $(HOST_TOOL)

FORCE:

# Rebuild the CLI atomically on every request via FORCE (no source prereqs), so compiler and flag changes cannot reuse a stale tool.
$(HOST_TOOL): FORCE
	@mkdir -p "$(dir $@)"
	@set -e; tmp="$@.$$$$.tmp"; cleanup(){ rm -f "$$tmp"; }; trap 'cleanup' EXIT; \
	trap 'cleanup; trap - TERM INT EXIT; kill -s TERM "$$$$"' TERM; \
	trap 'cleanup; trap - INT TERM EXIT; kill -s INT "$$$$"' INT; \
	$(CC) $(DECODER_CFLAGS) $(HOST_BACKEND_DEFINES) $(TOOL_SRCS) $(LDFLAGS) -o "$$tmp"; \
	mv -f "$$tmp" "$@"; trap - EXIT TERM INT

check-corpus-internal: ultrapatch check_corpus.sh
	@./check_corpus.sh $(JOBS)

check-footprint-internal: $(DECODER_PUBLIC_HDRS) $(DECODER_INTEGRATION_TU) \
                          scripts/check_footprint.sh scripts/stack_bound.py
	@ARM_CC="$(ARM_CC)" ARM_SIZE="$(ARM_SIZE)" ARM_OBJDUMP="$(ARM_OBJDUMP)" \
	  ARM_DEC_FLAGS="$(ARM_DEC_FLAGS)" ARM_OBJECT_OPT="$(ARM_OBJECT_OPT)" \
	  DECODER_INTEGRATION_TU="$(DECODER_INTEGRATION_TU)" \
	  scripts/check_footprint.sh

# Post-development acceptance is deliberately outcome-only: compression/correctness over the full
# release corpus, followed by static-wrapper flash/state and worst supported stack measurements.
gate-internal:
	@$(MAKE) --no-print-directory check-corpus-internal
	@$(MAKE) --no-print-directory check-footprint-internal

clean-internal:
	@root=$$(realpath -m .build); dir=$$(realpath -m "$(BUILD_DIR)"); \
	case "$$dir" in "$$root"|"$$root"/*) rm -rf -- "$$dir" ;; \
	  *) echo "refusing to clean noncanonical build dir: $$dir" >&2; exit 1 ;; esac
	rm -f ultrapatch

clean-all-internal:
	rm -rf .build
	rm -f ultrapatch
