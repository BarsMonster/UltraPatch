# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

override SHELL := /bin/sh

# A release gate must actually execute and propagate failures; dry-run, touch, and ignore-error
# modes would otherwise let Make report success without establishing the release outcome.
override CANONICAL_GOAL := $(firstword $(filter gate,$(MAKECMDGOALS)))
override CANONICAL_SHORT_FLAGS := $(firstword $(MAKEFLAGS))
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
ARM_OBJCOPY ?= $(ARM_PREFIX)objcopy
ARM_OBJECT_OPT ?= -Os
ARM_STACK_OPT ?= -O2

export LANG := C
export LANGUAGE := C
export LC_ALL := C

OPT ?= -O2
DECODER_CONFIG_FLAGS ?= -DPATCH_IMAGE_BASE=0u -DPATCH_IMAGE_CAPACITY=67108864u
CONTRACT_FLAGS := -std=c99 -I. -Isrc -Ivendor/libdivsufsort
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
ENC_MODULE_SRCS := src/enc_util.c src/enc_elf.c src/enc_bsdiff.c src/enc_field.c \
                   src/enc_rc.c src/enc_lz.c src/enc_emit.c src/enc_plan.c
HOST_BACKEND_SRC := src/patch_host_backend.c
TOOL_SRCS := src/patch_generate.c $(ENC_MODULE_SRCS) $(DIVSUF) $(HOST_BACKEND_SRC)
GEN_HDR := src/rc_models.h src/patch_config.h src/enc_internal.h
DECODER_PUBLIC_HDRS := src/patch_config.h src/rc_models.h src/patch_apply.h
NVM_EMU := src/nvm_emu.inc

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

override CORPUS_BUILD_DIR := $(abspath $(BUILD_DIR))/corpus
override FIXTURES := $(CORPUS_BUILD_DIR)/fixtures
override IMAGES := $(CORPUS_BUILD_DIR)/images
override FOREIGN := test-bench/foreign
CORPUS_SOURCE_ELFS := $(wildcard test-bench/fixtures/*/watch.elf) \
                      $(wildcard test-bench/images/*/watch.elf)
CORPUS_FOREIGN_BINS := $(wildcard test-bench/foreign/*/watch.bin)

ARM_DEC_FLAGS := -mcpu=cortex-m0plus -mthumb $(DECODER_CONFIG_FLAGS) -I src
DECODER_INTEGRATION_TU := test-bench/decoder-integration.c
override BASE_FOOTPRINT_FLASH := 6129
override BASE_FOOTPRINT_STATE := 8460
override BASE_FOOTPRINT_STACK := 480

GATE_TIMEOUT ?= 80
override RELEASE_GATE_TIMEOUT := 80
CAPPED := all check-corpus check-footprint clean clean-all
.PHONY: $(CAPPED) $(addsuffix -internal,$(CAPPED)) gate gate-internal
$(CAPPED): %:
	@timeout $(GATE_TIMEOUT) $(MAKE) --no-print-directory $*-internal; s=$$?; \
	if [ $$s -eq 124 ]; then echo "Execution timelimit $(GATE_TIMEOUT) exceeded" >&2; fi; \
	exit $$s

gate:
	@mkdir -p "$(BUILD_DIR)"; build_dir=$$(mktemp -d "$(BUILD_DIR)/gate.XXXXXX"); \
	trap 'rm -rf "$$build_dir"' EXIT; \
	timeout $(RELEASE_GATE_TIMEOUT) \
	  $(MAKE) --no-print-directory BUILD_DIR="$$build_dir" gate-internal; s=$$?; \
	if [ $$s -eq 124 ]; then echo "Execution timelimit $(RELEASE_GATE_TIMEOUT) exceeded" >&2; fi; \
	exit $$s

all-internal: ultrapatch

.PHONY: host-tool-path ultrapatch FORCE corpus-assets-internal
host-tool-path: $(HOST_TOOL)
	@printf '%s\n' "$(HOST_TOOL)"

ultrapatch: $(HOST_TOOL)

FORCE:

# Rebuild the CLI atomically on every request so compiler and flag changes cannot reuse a stale tool.
$(HOST_TOOL): FORCE $(TOOL_SRCS) $(GEN_HDR) $(NVM_EMU) Makefile
	@mkdir -p "$(dir $@)"
	@set -e; tmp="$@.$$$$.tmp"; cleanup(){ rm -f "$$tmp"; }; trap 'cleanup' EXIT; \
	trap 'cleanup; trap - TERM INT EXIT; kill -s TERM "$$$$"' TERM; \
	trap 'cleanup; trap - INT TERM EXIT; kill -s INT "$$$$"' INT; \
	$(CC) $(DECODER_CFLAGS) $(HOST_BACKEND_DEFINES) $(TOOL_SRCS) $(LDFLAGS) -o "$$tmp"; \
	mv -f "$$tmp" "$@"; trap - EXIT TERM INT

# Derive the exact home and fixture binaries beside their ELF sidecars. Foreign binaries are
# already tracked and are consumed in place. The fresh directory prevents stale corpus members.
corpus-assets-internal: $(CORPUS_SOURCE_ELFS) $(CORPUS_FOREIGN_BINS)
	@set -e; rm -rf "$(CORPUS_BUILD_DIR)"; count=0; \
	for role in fixtures images; do \
	  for source in test-bench/$$role/*/watch.elf; do \
	    id=$$(basename "$$(dirname "$$source")"); dest="$(CORPUS_BUILD_DIR)/$$role/$$id"; \
	    mkdir -p "$$dest"; cp "$$source" "$$dest/watch.elf"; \
	    "$(ARM_OBJCOPY)" -O binary "$$source" "$$dest/watch.bin"; count=$$((count + 1)); \
	  done; \
	done; \
	test "$$count" -eq 18; \
	echo "corpus_materialized=$$count binaries in $(CORPUS_BUILD_DIR)"

check-corpus-internal: ultrapatch corpus-assets-internal check_corpus.sh
	@IMAGES="$(IMAGES)" FIXTURES="$(FIXTURES)" FOREIGN="$(FOREIGN)" \
	  ./check_corpus.sh $(JOBS)

check-footprint-internal: $(DECODER_PUBLIC_HDRS) $(DECODER_INTEGRATION_TU) \
                          scripts/check_footprint.sh scripts/stack_bound.py scripts/tempdir.sh
	@ARM_CC="$(ARM_CC)" ARM_SIZE="$(ARM_SIZE)" ARM_OBJDUMP="$(ARM_OBJDUMP)" \
	  ARM_DEC_FLAGS="$(ARM_DEC_FLAGS)" ARM_OBJECT_OPT="$(ARM_OBJECT_OPT)" \
	  ARM_STACK_OPT="$(ARM_STACK_OPT)" DECODER_INTEGRATION_TU="$(DECODER_INTEGRATION_TU)" \
	  BASE_FOOTPRINT_FLASH="$(BASE_FOOTPRINT_FLASH)" \
	  BASE_FOOTPRINT_STATE="$(BASE_FOOTPRINT_STATE)" \
	  BASE_FOOTPRINT_STACK="$(BASE_FOOTPRINT_STACK)" \
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
