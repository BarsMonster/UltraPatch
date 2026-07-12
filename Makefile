# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

# Release certification and canonical input publishers are authorities, not configurable builds.
# Establish their executable search path before BUILD_PROFILE_ID invokes Python during Makefile
# parsing, and reject launch controls before any recipe can be skipped or ignored.
override CANONICAL_AUTHORITY_GOAL := $(firstword $(filter gate golden-update \
	release-profile-json release-profile-update encoder-kernel-baseline-update,$(MAKECMDGOALS)))
ifneq ($(CANONICAL_AUTHORITY_GOAL),)
override PATH := /usr/bin:/bin
export PATH
# Appending below would otherwise retain ambient CFLAGS/LDFLAGS while changing their reported
# origin to `file`, defeating the later fixed-origin check.
ifneq ($(filter environment environment\ override command\ line,$(origin CFLAGS)),)
$(error $(CANONICAL_AUTHORITY_GOAL) rejects runtime override: CFLAGS (origin $(origin CFLAGS)))
endif
ifneq ($(filter environment environment\ override command\ line,$(origin LDFLAGS)),)
$(error $(CANONICAL_AUTHORITY_GOAL) rejects runtime override: LDFLAGS (origin $(origin LDFLAGS)))
endif
ifneq ($(filter default undefined,$(origin GNUMAKEFLAGS)),$(origin GNUMAKEFLAGS))
$(error $(CANONICAL_AUTHORITY_GOAL) rejects launch control: GNUMAKEFLAGS (origin $(origin GNUMAKEFLAGS)))
endif
ifneq ($(filter default undefined,$(origin MAKEFILES)),$(origin MAKEFILES))
$(error $(CANONICAL_AUTHORITY_GOAL) rejects launch control: MAKEFILES (origin $(origin MAKEFILES)))
endif
ifneq ($(origin MAKE),default)
$(error $(CANONICAL_AUTHORITY_GOAL) rejects launch control: MAKE (origin $(origin MAKE)))
endif
ifneq ($(filter default file,$(origin SHELL)),$(origin SHELL))
$(error $(CANONICAL_AUTHORITY_GOAL) rejects launch control: SHELL (origin $(origin SHELL)))
endif
ifneq ($(SHELL),/bin/sh)
$(error $(CANONICAL_AUTHORITY_GOAL) rejects launch control: SHELL ($(SHELL)))
endif
ifneq ($(origin .SHELLFLAGS),default)
$(error $(CANONICAL_AUTHORITY_GOAL) rejects launch control: .SHELLFLAGS (origin $(origin .SHELLFLAGS)))
endif
ifneq ($(strip $(filter-out --no-print-directory,$(MAKEFLAGS))),)
$(error $(CANONICAL_AUTHORITY_GOAL) rejects launch control: MAKEFLAGS ($(MAKEFLAGS)))
endif
endif

CC = $(CROSS_COMPILE)gcc
CLANG ?= clang
NM ?= nm
ARM_PREFIX ?= arm-none-eabi-
ARM_CC ?= $(ARM_PREFIX)gcc
ARM_SIZE ?= $(ARM_PREFIX)size
ARM_OBJDUMP ?= $(ARM_PREFIX)objdump
ARM_OBJCOPY ?= $(ARM_PREFIX)objcopy
ARM_NM ?= $(ARM_PREFIX)nm
ARM_OBJECT_OPT ?= -Os
ARM_STACK_OPT ?= -O2

# Release builds and profile probes never inherit compiler search/injection state. The direct
# release driver starts from an even smaller environment; keeping the Make boundary clean also
# makes direct development-gate invocations representative of the recorded profile.
TOOLCHAIN_ENV_UNSET := COMPILER_PATH CPATH CPLUS_INCLUDE_PATH C_INCLUDE_PATH \
	DEPENDENCIES_OUTPUT GCC_COMPARE_DEBUG GCC_EXEC_PREFIX GCC_SPECS \
	LD_LIBRARY_PATH LD_PRELOAD LIBRARY_PATH OBJC_INCLUDE_PATH SOURCE_DATE_EPOCH \
	SUNPRO_DEPENDENCIES ZERO_AR_DATE
unexport $(TOOLCHAIN_ENV_UNSET)
export LANG := C
export LANGUAGE := C
export LC_ALL := C

OPT ?= -O2
# Wire/resource constants live only in patch_config.h and are not build variables.
# Decoder/device integration only: these partition values are not part of the patch wire.
# Repository host/ARM harnesses intentionally model a 64 MiB image partition based at zero.
DECODER_CONFIG_FLAGS ?= -DPATCH_IMAGE_BASE=0u -DPATCH_IMAGE_CAPACITY=67108864u
# Language/include contract shared by host builds and the analyzer. Warning/optimization policy
# stays leg-local; decoder-containing commands append DECODER_CONFIG_FLAGS explicitly.
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

# Host-tool build roles and link order are part of the profile identity. A recipe semantic change,
# source-list edit, or object reorder therefore selects a fresh profile directory instead of
# invalidating unrelated artifacts through a second signature file inside an existing profile.
DIVSUF := vendor/libdivsufsort/divsufsort.c
ENC_MODULE_SRCS := src/enc_util.c src/enc_elf.c src/enc_bsdiff.c src/enc_field.c \
                   src/enc_rc.c src/enc_lz.c src/enc_emit.c src/enc_plan.c
HOST_BACKEND_SRC := src/patch_host_backend.c
TOOL_SRCS := src/patch_generate.c $(ENC_MODULE_SRCS) $(HOST_BACKEND_SRC) $(DIVSUF)
HOST_BUILD_RECIPE_TAG ?= per-tu-v1
override HOST_ENCODER_SRCS := $(filter-out $(HOST_BACKEND_SRC),$(TOOL_SRCS))
override HOST_BACKEND_SRCS := $(HOST_BACKEND_SRC)
override HOST_LINK_OBJECTS := $(addprefix obj/,$(HOST_ENCODER_SRCS:.c=.o) \
                                                   $(HOST_BACKEND_SRCS:.c=.o))

# Host builds and derived corpus binaries are isolated by compiler/objcopy identity and every
# effective build flag. The profile helper emits canonical JSON without workspace paths; its
# SHA-256 is both the build-directory
# key and the provenance identity. Tests receive the exact binary through ULTRAPATCH, so a GCC,
# Clang, or alternate-config invocation never executes whichever shared root binary linked last.
override UP_PROFILE_CC := $(CC)
override UP_PROFILE_CLANG := $(CLANG)
override UP_PROFILE_NM := $(NM)
override UP_PROFILE_ENV_UNSET := $(TOOLCHAIN_ENV_UNSET)
override UP_PROFILE_ENCODER_CFLAGS := $(CFLAGS)
override UP_PROFILE_BACKEND_CFLAGS := $(DECODER_CFLAGS) $(HOST_BACKEND_DEFINES)
override UP_PROFILE_LINK_CFLAGS := $(CFLAGS)
override UP_PROFILE_LDFLAGS := $(LDFLAGS)
override UP_PROFILE_DECODER_FLAGS := $(DECODER_CONFIG_FLAGS)
override UP_PROFILE_RECIPE_REVISION := $(HOST_BUILD_RECIPE_TAG)
override UP_PROFILE_ENCODER_SOURCES := $(HOST_ENCODER_SRCS)
override UP_PROFILE_BACKEND_SOURCES := $(HOST_BACKEND_SRCS)
override UP_PROFILE_LINK_OBJECTS := $(HOST_LINK_OBJECTS)
override UP_PROFILE_ARM_CC := $(ARM_CC)
override UP_PROFILE_ARM_SIZE := $(ARM_SIZE)
override UP_PROFILE_ARM_OBJDUMP := $(ARM_OBJDUMP)
override UP_PROFILE_ARM_OBJCOPY := $(ARM_OBJCOPY)
override UP_PROFILE_ARM_NM := $(ARM_NM)
override UP_PROFILE_ARM_SOURCE_FLAGS = $(ARM_DEC_FLAGS)
override UP_PROFILE_ARM_LINK_FLAGS = $(ARM_LINK_FLAGS)
override UP_PROFILE_ARM_LINK_LIBS = $(ARM_LINK_LIBS)
override UP_PROFILE_ARM_OBJECT_OPT := $(ARM_OBJECT_OPT)
override UP_PROFILE_ARM_STACK_OPT := $(ARM_STACK_OPT)
export UP_PROFILE_CC UP_PROFILE_CLANG UP_PROFILE_NM UP_PROFILE_ENV_UNSET
export UP_PROFILE_ENCODER_CFLAGS UP_PROFILE_BACKEND_CFLAGS
export UP_PROFILE_LINK_CFLAGS UP_PROFILE_LDFLAGS
export UP_PROFILE_DECODER_FLAGS
export UP_PROFILE_RECIPE_REVISION UP_PROFILE_ENCODER_SOURCES
export UP_PROFILE_BACKEND_SOURCES UP_PROFILE_LINK_OBJECTS
export UP_PROFILE_ARM_CC UP_PROFILE_ARM_SIZE UP_PROFILE_ARM_OBJDUMP UP_PROFILE_ARM_OBJCOPY
export UP_PROFILE_ARM_NM
export UP_PROFILE_ARM_SOURCE_FLAGS
export UP_PROFILE_ARM_LINK_FLAGS UP_PROFILE_ARM_LINK_LIBS
export UP_PROFILE_ARM_OBJECT_OPT UP_PROFILE_ARM_STACK_OPT

BUILD_ROOT ?= .build
# These maintenance targets neither build nor execute the encoder. Avoid probing every host/ARM
# tool merely to clean artifacts, inspect the release inventory, or run GCC's standalone analyzer.
# A mixed invocation remains profile-dependent if any requested goal needs the host tool.
override PROFILE_INDEPENDENT_GOALS := clean-all clean-all-internal \
	check-analyze check-analyze-internal \
	check-release-inventory check-release-inventory-internal
override PROFILE_INDEPENDENT_INVOCATION := $(and \
	$(filter default,$(origin MAKECMDGOALS)), \
	$(strip $(MAKECMDGOALS)), \
	$(if $(strip $(filter-out $(PROFILE_INDEPENDENT_GOALS),$(MAKECMDGOALS))),,1))
ifeq ($(strip $(PROFILE_INDEPENDENT_INVOCATION)),1)
override BUILD_PROFILE_ID := profile-not-required
else
override BUILD_PROFILE_ID := $(shell /usr/bin/python3 -I -S scripts/build_profile.py host-id)
ifeq ($(strip $(BUILD_PROFILE_ID)),)
$(error failed to derive the host build profile)
endif
endif
BUILD_DIR ?= $(BUILD_ROOT)/$(BUILD_PROFILE_ID)
ifeq ($(abspath $(BUILD_DIR)),$(CURDIR))
$(error BUILD_DIR must not be the repository root)
endif
override PROFILE_MANIFEST := $(BUILD_DIR)/profile.json
override HOST_TOOL := $(abspath $(BUILD_DIR))/ultrapatch
override HOST_OBJ_DIR := $(abspath $(BUILD_DIR))/obj
override HOST_TOOL_OBJECTS := $(addprefix $(abspath $(BUILD_DIR))/,$(HOST_LINK_OBJECTS))
override HOST_TOOL_DEPFILES := $(HOST_TOOL_OBJECTS:.o=.d)
RELEASE_PROFILE_LOCK ?= toolchains/release-profile.json
override ULTRAPATCH := $(HOST_TOOL)
override ULTRAPATCH_DECODE := $(HOST_TOOL)
export ULTRAPATCH ULTRAPATCH_DECODE

CONFIG_HDR := src/patch_config.h
APPLY_HDR := src/patch_apply.h
DECODER_PUBLIC_HDRS := $(CONFIG_HDR) src/rc_models.h $(APPLY_HDR)
# Shared host-side NVM emulator, #included by patch_host_backend.c before patch_apply.h.
NVM_EMU := src/nvm_emu.inc
GEN_HDR := src/rc_models.h $(CONFIG_HDR) src/enc_internal.h
# The host backend owns the single reference-decoder copy used by encode
# selfcheck, CLI decode, and standalone decoder builds.
ENC_SEAM_SRCS := $(filter-out src/enc_plan.c,$(ENC_MODULE_SRCS)) $(HOST_BACKEND_SRC) $(DIVSUF)
# Standalone host-decoder TU pair + demo defines, shared once by dec_portable, check_degrade's D=1, and all-internal.
DEC_STANDALONE_SRCS := $(HOST_BACKEND_SRC) src/enc_util.c
DEC_DEMO_DEFINES := -DPATCH_APPLY_DEMO_MAIN -D_POSIX_C_SOURCE=200809L
override CORPUS_BUILD_DIR := $(abspath $(BUILD_DIR))/corpus
override CORPUS_ASSET_STAMP := $(CORPUS_BUILD_DIR)/.ready
override CORPUS_SOURCE_ELFS := $(wildcard test-bench/fixtures/*/watch.elf) \
                               $(wildcard test-bench/images/*/watch.elf)
override CORPUS_FOREIGN_BINS := $(wildcard test-bench/foreign/*/watch.bin)
FIXTURES ?= $(CORPUS_BUILD_DIR)/fixtures
IMAGES ?= $(CORPUS_BUILD_DIR)/images
FOREIGN ?= test-bench/foreign
CORPUS_INVENTORY ?= test-bench/corpus-inventory.tsv
WIRE_BASELINE ?= test-bench/wire-baseline.tsv
ENCODER_KERNEL_BASELINE := test-bench/encoder-kernel-baseline.tsv
# An empty inventory is the documented custom-measurement mode; callers then provide their own
# image/fixture roots and do not need the canonical profile-local corpus view.
CORPUS_ASSET_PREREQ := $(if $(strip $(CORPUS_INVENTORY)),$(CORPUS_ASSET_STAMP))

# Release scope pins. The inventory names every member in order; these cardinalities make a
# deletion or reduced release set an explicit policy change rather than a self-consistent shrink.
BASE_RELEASE_FIXTURES ?= 2
BASE_RELEASE_HOME_IMAGES ?= 16
BASE_RELEASE_FOREIGN_IMAGES ?= 18
BASE_RELEASE_FOREIGN_EDGES ?= 17
BASE_RELEASE_GOLDEN_BLOBS ?= 4
BASE_FULL_TOTAL ?= 4148576
# Foreign lineage (CircuitPython feather_m0_express, 34 pair-directions): summed blob bytes.
# Ratchets like BASE_FULL_TOTAL — a wire regression on the product firmware was NOT tuned on fails here.
# Re-pin on intentional wire changes. See docs/foreign-firmware-study.md.
BASE_FOREIGN_TOTAL ?= 1299431
BASE_ONEFACE_GROW ?= 573
BASE_ONEFACE_REVERT ?= 287
BASE_ARM_TEXT ?= 6297
BASE_ARM_DATA ?= 0
BASE_ARM_BSS ?= 9368
BASE_ARM_LINKED_TEXT ?= 6877
BASE_ARM_LINKED_DATA ?= 0
BASE_ARM_LINKED_BSS ?= 9368
BASE_ARM_SOFT_DIV ?= 0
# Product SRAM ceiling: unlike the configurable size ratchet above, command-line and
# environment overrides must never be able to raise or disable this limit.
override ARM_BSS_HARD_CAP := 12288
ARM_COMMON_FLAGS := -mcpu=cortex-m0plus -mthumb $(DECODER_CONFIG_FLAGS)
ARM_DEC_FLAGS := $(ARM_COMMON_FLAGS) -I src
ARM_LINK_STUBS ?= scripts/arm_link_stubs.c
ARM_LINK_LAYOUT := scripts/arm_link.ld
ARM_LINK_FLAGS := -mcpu=cortex-m0plus -mthumb -nostdlib \
	-Wl,--gc-sections,--orphan-handling=error,-T,$(ARM_LINK_LAYOUT)
ARM_LINK_LIBS := -lc -lgcc
DECODER_INTEGRATION_TU := test-bench/decoder-integration.c
# The production ARM size gate intentionally measures the static-state wrapper integration
# used by rcv3_run in the tracked integration TU. A generic caller-owned PatchApply * wrapper
# may compile differently; product notes/gate output must not present this number as
# shape-independent.
# Independent worst-case caller-stack ceilings for the two real integration shapes, gcc -O2,
# Cortex-M0+ (bytes). scripts/stack_bound.py derives each exact static bound from
# -fstack-usage frames + its harness object's call graph. check-stack reports and gates both.
BASE_STACK_STATIC_CEIL_O2 ?= 480
BASE_STACK_GENERIC_CEIL_O2 ?= 480

# ---- hard 80 s execution cap on EVERY public target ---------------------------------
# Owner rule: we do not throw compute just for fun — an operation that cannot finish in
# 80 s gets fixed or deleted, not waited for. Each public name is a thin cap over its
# *-internal twin: coreutils `timeout` bounds the whole subtree (it runs the child in
# its own process group, so backgrounded gate legs die with it) and an overrun reports
# an explicit error instead of a bare status 124. Local experiments may use
# GATE_TIMEOUT=<secs> make <target>. The release gate itself always uses the checked 80-second
# cap and rejects runtime input overrides; use the individual targets for experiments.
GATE_TIMEOUT ?= 80
override RELEASE_GATE_TIMEOUT := 80
WIRE_BASELINE_LOCK := test-bench/.wire-baseline-update.lock
ENCODER_KERNEL_BASELINE_LOCK := test-bench/.encoder-kernel-baseline-update.lock
CAPPED := all check check-arm check-stack check-assets check-ab-matrix check-clang check-decoder-contract check-decoder-sanitize check-encoder-sanitize \
	      check-build-profile check-release-profile check-release-inventory \
          check-models check-malformed check-corpus check-edge check-degrade check-golden \
          golden-update check-analyze clean clean-all
.PHONY: $(CAPPED) $(addsuffix -internal,$(CAPPED)) gate gate-internal
$(CAPPED): %:
	@timeout $(GATE_TIMEOUT) $(MAKE) --no-print-directory $*-internal; s=$$?; \
	if [ $$s -eq 124 ]; then echo "Execution timelimit $(GATE_TIMEOUT) exceeded" >&2; fi; \
	exit $$s

gate:
	@timeout $(RELEASE_GATE_TIMEOUT) flock --shared "$(WIRE_BASELINE_LOCK)" \
	  flock --shared "$(ENCODER_KERNEL_BASELINE_LOCK)" \
	  $(MAKE) --no-print-directory gate-internal; s=$$?; \
	if [ $$s -eq 124 ]; then echo "Execution timelimit $(RELEASE_GATE_TIMEOUT) exceeded" >&2; fi; \
	exit $$s

.PHONY: encoder-kernel-baseline-update
# Deliberate semantic changes only: this never runs from normal builds, gate, or golden-update.
# Review the changed result stream and commit the refreshed baseline with the implementation.
encoder-kernel-baseline-update: scripts/check_encoder_kernels.sh
	@CC="$(CC)" CLANG="$(CLANG)" CFLAGS="$(DECODER_CFLAGS)" \
	  ENC_SEAM_SRCS="$(ENC_SEAM_SRCS)" ENCODER_KERNEL_BASELINE="$(ENCODER_KERNEL_BASELINE)" \
	  /usr/bin/timeout $(GATE_TIMEOUT) /usr/bin/flock --exclusive "$(ENCODER_KERNEL_BASELINE_LOCK)" \
	  /bin/sh -eu -c 'candidate=$$(/usr/bin/mktemp test-bench/.encoder-kernel-baseline.prepare.XXXXXX); \
	    trap '\''/usr/bin/rm -f "$$candidate"'\'' EXIT TERM INT; \
	    ENCODER_KERNEL_BASELINE_DUMP="$$candidate" scripts/check_encoder_kernels.sh candidate; \
	    /usr/bin/chmod 0644 "$$candidate"; \
	    if /usr/bin/cmp -s "$$candidate" "$(ENCODER_KERNEL_BASELINE)"; then \
	      /usr/bin/chmod 0644 "$(ENCODER_KERNEL_BASELINE)"; \
	      echo "encoder_kernel_baseline_update=NOOP"; \
	    else \
	      /usr/bin/mv "$$candidate" "$(ENCODER_KERNEL_BASELINE)"; \
	      echo "encoder_kernel_baseline_update=COMMITTED"; \
	    fi'; s=$$?; \
	if [ $$s -eq 124 ]; then echo "Execution timelimit $(GATE_TIMEOUT) exceeded" >&2; fi; \
	exit $$s

.PHONY: release-profile-update release-profile-update-internal
release-profile-update:
	@/usr/bin/timeout $(GATE_TIMEOUT) /usr/bin/flock --exclusive "$(WIRE_BASELINE_LOCK)" \
	  /usr/bin/make --no-print-directory release-profile-update-internal; s=$$?; \
	if [ $$s -eq 124 ]; then echo "Execution timelimit $(GATE_TIMEOUT) exceeded" >&2; fi; \
	exit $$s

all-internal: ultrapatch
	$(CC) $(DECODER_CFLAGS) -Wconversion $(DEC_DEMO_DEFINES) -c $(HOST_BACKEND_SRC) -o /dev/null

.PHONY: host-tool-path release-profile-json profile-check ultrapatch
host-tool-path:
	@printf '%s\n' "$(HOST_TOOL)"

release-profile-json:
	@/usr/bin/python3 -I -S scripts/build_profile.py release-lock-json "$(RELEASE_PROFILE_LOCK)"

release-profile-update-internal: scripts/build_profile.py $(RELEASE_PROFILE_LOCK)
	@$(canonical_authority_origin_check) && \
	  /usr/bin/python3 -I -S scripts/build_profile.py refresh-release "$(RELEASE_PROFILE_LOCK)"

# Validate on every public use, including when an explicit BUILD_DIR points at a manifest created
# by another compiler/config. Identical concurrent checks are safe because ensure-host publishes
# the canonical manifest atomically.
profile-check: scripts/build_profile.py
	@python3 scripts/build_profile.py ensure-host "$(PROFILE_MANIFEST)" >/dev/null

ultrapatch: profile-check $(HOST_TOOL)

$(PROFILE_MANIFEST): scripts/build_profile.py
	@python3 scripts/build_profile.py ensure-host "$@" >/dev/null

check-release-profile-internal: scripts/build_profile.py $(RELEASE_PROFILE_LOCK) \
                                .github/workflows/gate.yml
	@python3 scripts/build_profile.py verify-release "$(RELEASE_PROFILE_LOCK)"
	@container=$$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1], encoding="utf-8"))["container"])' \
	  "$(RELEASE_PROFILE_LOCK)"); \
	grep -Fqx "      image: $$container" .github/workflows/gate.yml || { \
	  echo "release workflow container does not match $(RELEASE_PROFILE_LOCK)" >&2; exit 1; }

check-build-profile-internal: scripts/check_build_profile.sh scripts/build_profile.py
	@MAKE="$(MAKE)" CLANG="$(CLANG)" ARM_OBJCOPY="$(ARM_OBJCOPY)" \
	  scripts/check_build_profile.sh

# Required second-compiler leg. It remains outside `make gate`, but is a capped public target
# and uses the exact CLANG command pinned in the release descriptor.
check-clang-internal: check-release-profile-internal
	@$(MAKE) --no-print-directory CC="$(CLANG)" -B all-internal
	@$(MAKE) --no-print-directory CC="$(CLANG)" \
	  check-internal check-malformed-internal check-clang-golden-internal
	@echo "clang_contract=OK (descriptor-pinned build + checks + golden wire)"

.PHONY: check-clang-golden-internal
check-clang-golden-internal: ultrapatch $(CORPUS_ASSET_PREREQ)
	@FIXTURES="$(FIXTURES)" IMAGES="$(IMAGES)" WIRE_BASELINE="$(WIRE_BASELINE)" \
	  scripts/check_golden.sh check

# `make gate` is a release certification, not a configurable measurement. These variables may
# still be overridden on their individual measurement targets and A/B runs; the release gate and
# canonical updater reject any runtime origin so they cannot accidentally certify/publish a
# reduced corpus, relaxed ratchet,
# substituted source/harness, alternate build directory, or disabled contract mode. Repository
# edits remain the intentional, reviewable way to change a release input. The profile verifier
# separately proves the exact compiler versions, flags, and multilib contents.
override RELEASE_GATE_FIXED_VARS := \
	FIXTURES IMAGES FOREIGN \
	CORPUS_INVENTORY WIRE_BASELINE ENCODER_KERNEL_BASELINE \
	BASE_RELEASE_FIXTURES BASE_RELEASE_HOME_IMAGES BASE_RELEASE_FOREIGN_IMAGES \
	BASE_RELEASE_FOREIGN_EDGES BASE_RELEASE_GOLDEN_BLOBS \
	BASE_FULL_TOTAL BASE_FOREIGN_TOTAL BASE_ONEFACE_GROW BASE_ONEFACE_REVERT \
	BASE_ARM_TEXT BASE_ARM_DATA BASE_ARM_BSS BASE_ARM_LINKED_TEXT BASE_ARM_LINKED_DATA \
	BASE_ARM_LINKED_BSS BASE_ARM_SOFT_DIV BASE_STACK_STATIC_CEIL_O2 BASE_STACK_GENERIC_CEIL_O2 \
	RELEASE_PROFILE_LOCK BUILD_ROOT BUILD_DIR GATE_TIMEOUT WIRE_BASELINE_LOCK ENCODER_KERNEL_BASELINE_LOCK \
	CC CLANG NM ARM_PREFIX ARM_CC ARM_SIZE ARM_OBJDUMP ARM_OBJCOPY ARM_NM ARM_OBJECT_OPT ARM_STACK_OPT OPT \
	DECODER_CONFIG_FLAGS CONTRACT_FLAGS CFLAGS DECODER_CFLAGS \
	LDFLAGS ARM_COMMON_FLAGS ARM_DEC_FLAGS ARM_LINK_FLAGS ARM_LINK_LIBS \
	DIVSUF CONFIG_HDR APPLY_HDR DECODER_PUBLIC_HDRS NVM_EMU ENC_MODULE_SRCS \
	GEN_HDR HOST_BACKEND_SRC ENC_SEAM_SRCS DEC_STANDALONE_SRCS DEC_DEMO_DEFINES TOOL_SRCS \
	HOST_BACKEND_DEFINES HOST_BUILD_RECIPE_TAG \
	PORTABLE_FALLBACK_FLAGS TOOLCHAIN_ENV_UNSET \
	ARM_LINK_STUBS ARM_LINK_LAYOUT DECODER_INTEGRATION_TU
override RELEASE_GATE_UNSET_VARS := \
	CROSS_COMPILE CFLAGS_EXTRA CORPUS_SIZE_BASELINE CORPUS_SIZE_DUMP WIRE_BASELINE_DUMP ENCODER_KERNEL_BASELINE_DUMP \
	DECODER_API_REGULAR DECODER_API_SANITIZE DECODER_INTEGRATION_PROBE_FLAGS \
	ONEFACE_ROUNDTRIP ONEFACE_WIRE_HASHES

# A recipe-level rejection is insufficient: inherited -i can ignore it, while -n/-t and
# substituted shell/make commands can false-success. For public authority goals, reject all
# launch controls and release-input substitutions while Make is still parsing; $(error) cannot
# be suppressed by ignore-errors mode. Keep one internal probe as defense for direct use of the
# non-public recursive targets.
.PHONY: canonical-authority-origin-probe-internal release-gate-inputs-internal
define canonical_authority_origin_check
set -eu; bad=0; \
	$(foreach v,$(RELEASE_GATE_FIXED_VARS),if [ "$(origin $(v))" != file ]; then echo "canonical authority rejects runtime override: $(v) (origin $(origin $(v)))" >&2; bad=1; fi; ) \
	$(foreach v,$(RELEASE_GATE_UNSET_VARS),if [ "$(origin $(v))" != undefined ]; then echo "canonical authority requires unset mode: $(v) (origin $(origin $(v)))" >&2; bad=1; fi; ) \
	test "$$bad" -eq 0
endef

canonical-authority-origin-probe-internal:
	@$(canonical_authority_origin_check)

release-gate-inputs-internal: scripts/build_profile.py
	@$(canonical_authority_origin_check) && \
	  python3 scripts/build_profile.py verify-release "$(RELEASE_PROFILE_LOCK)" >/dev/null && \
	  echo "release_gate_inputs=OK (canonical repository inputs + pinned release profile)"

.PHONY: golden-update-inputs-internal golden-update-validate-canonical-internal
golden-update-validate-canonical-internal:
	@$(MAKE) --no-print-directory check-release-inventory-internal
	@$(MAKE) --no-print-directory check-assets-internal
	@echo "golden_update_canonical_inputs=OK (inventory + corpus/foreign asset hashes)"

golden-update-inputs-internal:
	@$(canonical_authority_origin_check) && \
	  python3 scripts/build_profile.py verify-release "$(RELEASE_PROFILE_LOCK)" >/dev/null && \
	  flock --shared "$(WIRE_BASELINE_LOCK)" \
	    $(MAKE) --no-print-directory golden-update-validate-canonical-internal >/dev/null && \
	  echo "golden_update_inputs=OK (canonical repository inputs + pinned release profile)"

check-release-inventory-internal: scripts/check_release_inventory.py scripts/corpus_topology.py \
                                  $(CORPUS_INVENTORY) \
                                  $(CORPUS_SOURCE_ELFS) $(CORPUS_FOREIGN_BINS) \
                                  $(WIRE_BASELINE)
	@python3 scripts/corpus_topology.py test
	@python3 scripts/check_release_inventory.py \
	  --inventory "$(CORPUS_INVENTORY)" \
	  --source-root test-bench \
	  --wire-baseline "$(WIRE_BASELINE)" --home-total "$(BASE_FULL_TOTAL)" \
	  --oneface-grow "$(BASE_ONEFACE_GROW)" --oneface-revert "$(BASE_ONEFACE_REVERT)" \
	  --fixtures "$(BASE_RELEASE_FIXTURES)" --home-images "$(BASE_RELEASE_HOME_IMAGES)" \
	  --foreign-images "$(BASE_RELEASE_FOREIGN_IMAGES)" \
	  --foreign-edges "$(BASE_RELEASE_FOREIGN_EDGES)" \
	  --golden-blobs "$(BASE_RELEASE_GOLDEN_BLOBS)"

.PHONY: corpus-assets-internal
corpus-assets-internal: $(CORPUS_ASSET_STAMP)

$(CORPUS_ASSET_STAMP): scripts/corpus_topology.py $(CORPUS_INVENTORY) \
                       $(CORPUS_SOURCE_ELFS) $(CORPUS_FOREIGN_BINS) | profile-check
	@python3 scripts/corpus_topology.py materialize --inventory "$(CORPUS_INVENTORY)" \
	  --source-root test-bench --output-root "$(CORPUS_BUILD_DIR)" \
	  --objcopy "$(ARM_OBJCOPY)"
	@set -e; tmp="$@.$$$$.tmp"; trap 'rm -f "$$tmp"' EXIT; \
	  printf '%s\n' 'corpus_assets=ready' >"$$tmp"; mv -f "$$tmp" "$@"; trap - EXIT

$(HOST_OBJ_DIR)/src/patch_host_backend.o: $(HOST_BACKEND_SRC) Makefile \
                                     $(PROFILE_MANIFEST) | profile-check
	@mkdir -p "$(dir $@)"
	@set -e; obj="$@.$$$$.o.tmp"; dep="$@.$$$$.d.tmp"; \
	cleanup(){ rm -f "$$obj" "$$dep"; }; trap 'cleanup' EXIT; \
	trap 'cleanup; trap - TERM INT EXIT; kill -s TERM "$$$$"' TERM; \
	trap 'cleanup; trap - TERM INT EXIT; kill -s INT "$$$$"' INT; \
	$(CC) $(DECODER_CFLAGS) $(HOST_BACKEND_DEFINES) -MMD -MP -MF "$$dep" -MT "$@" \
		-c "$<" -o "$$obj"; \
	mv -f "$$dep" "$(@:.o=.d)"; mv -f "$$obj" "$@"; trap - EXIT TERM INT

$(HOST_OBJ_DIR)/%.o: %.c Makefile $(PROFILE_MANIFEST) | profile-check
	@mkdir -p "$(dir $@)"
	@set -e; obj="$@.$$$$.o.tmp"; dep="$@.$$$$.d.tmp"; \
	cleanup(){ rm -f "$$obj" "$$dep"; }; trap 'cleanup' EXIT; \
	trap 'cleanup; trap - TERM INT EXIT; kill -s TERM "$$$$"' TERM; \
	trap 'cleanup; trap - TERM INT EXIT; kill -s INT "$$$$"' INT; \
	$(CC) $(CFLAGS) -MMD -MP -MF "$$dep" -MT "$@" \
		-c "$<" -o "$$obj"; \
	mv -f "$$dep" "$(@:.o=.d)"; mv -f "$$obj" "$@"; trap - EXIT TERM INT

ifneq ($(strip $(PROFILE_INDEPENDENT_INVOCATION)),1)
-include $(HOST_TOOL_DEPFILES)
endif

$(HOST_TOOL): $(HOST_TOOL_OBJECTS) Makefile $(PROFILE_MANIFEST) | profile-check
	@python3 scripts/build_profile.py ensure-host "$(PROFILE_MANIFEST)" >/dev/null
	@mkdir -p "$(dir $(HOST_TOOL))"
	@set -e; tmp="$@.$$$$.tmp"; cleanup(){ rm -f "$$tmp"; }; trap 'cleanup' EXIT; \
	trap 'cleanup; trap - TERM INT EXIT; kill -s TERM "$$$$"' TERM; \
	trap 'cleanup; trap - TERM INT EXIT; kill -s INT "$$$$"' INT; \
	$(CC) $(CFLAGS) $(HOST_TOOL_OBJECTS) $(LDFLAGS) -o "$$tmp"; \
	mv -f "$$tmp" "$@"; trap - EXIT TERM INT
	@echo "host_tool=$(HOST_TOOL)"

check-internal: ultrapatch $(CORPUS_ASSET_PREREQ)
	@set -e; \
	. ./scripts/tempdir.sh; \
	"$(HOST_TOOL)" --help >"$$tmp/help.txt"; \
	"$(HOST_TOOL)" -h >"$$tmp/help-short.txt"; \
	grep -q '^usage: .*ultrapatch' "$$tmp/help.txt"; \
	grep -q '^usage: .*ultrapatch' "$$tmp/help-short.txt"; \
	grep -q '^  --decode' "$$tmp/help.txt"; \
	if "$(HOST_TOOL)" >"$$tmp/noargs.out" 2>"$$tmp/noargs.err"; then \
		echo "ultrapatch with no arguments unexpectedly succeeded" >&2; exit 1; \
	fi; \
	grep -q '^usage: .*ultrapatch' "$$tmp/noargs.err"; \
	FIXTURES="$(FIXTURES)" ONEFACE_ROUNDTRIP=1 \
	  BASE_ONEFACE_GROW="$(BASE_ONEFACE_GROW)" BASE_ONEFACE_REVERT="$(BASE_ONEFACE_REVERT)" \
	  scripts/oneface_metrics.sh "$(HOST_TOOL)" "$(HOST_TOOL)"


# The footprint ratchets are meaningful only for the checked release toolchain. Invoke the
# measurement from the recipe after validation so even `make -j` cannot race it ahead.
check-arm-internal: check-release-profile-internal
	@$(MAKE) --no-print-directory check-arm-measure-internal
	@MAKE="$(MAKE)" scripts/check_arm_bss_cap.sh

.PHONY: check-arm-measure-internal
check-arm-measure-internal: $(DECODER_PUBLIC_HDRS) $(ARM_LINK_STUBS) $(ARM_LINK_LAYOUT) \
                            $(DECODER_INTEGRATION_TU)
	@set -e; \
	. ./scripts/tempdir.sh; \
	$(ARM_CC) $(ARM_DEC_FLAGS) $(ARM_OBJECT_OPT) $(DECODER_INTEGRATION_PROBE_FLAGS) -DDECODER_INTEGRATION_STATIC -c "$(DECODER_INTEGRATION_TU)" -o "$$tmp/patch_apply_arm.o"; \
	size_out=$$($(ARM_SIZE) "$$tmp/patch_apply_arm.o"); \
	printf '%s\n' "$$size_out"; \
	echo "arm_size_integration=static PatchApply wrapper"; \
	set -- $$(printf '%s\n' "$$size_out" | awk 'NR==2 { print $$1, $$2, $$3 }'); \
	obj_text=$$1; obj_data=$$2; obj_bss=$$3; \
	echo "arm_object_text=$$obj_text"; echo "arm_object_data=$$obj_data"; echo "arm_object_bss=$$obj_bss"; \
	if [ "$$obj_bss" -gt "$(ARM_BSS_HARD_CAP)" ]; then \
		echo "ARM .bss hard cap exceeded: $$obj_bss > $(ARM_BSS_HARD_CAP)" >&2; exit 1; \
	fi; \
	test "$$obj_text" -le "$(BASE_ARM_TEXT)"; \
	test "$$obj_data" -le "$(BASE_ARM_DATA)"; \
	test "$$obj_bss" -le "$(BASE_ARM_BSS)"; \
	$(ARM_OBJDUMP) -d "$$tmp/patch_apply_arm.o" > "$$tmp/patch_apply_arm.dump"; \
	if grep -Eq '\b(udiv|sdiv)\b' "$$tmp/patch_apply_arm.dump"; then \
		echo "hardware divide instruction found in decoder" >&2; exit 1; \
	fi; \
	soft=$$(grep -Ec '__aeabi_.*div|__aeabi_.*mod' "$$tmp/patch_apply_arm.dump" || true); \
	echo "soft_div_calls=$$soft"; \
	test "$$soft" -eq "$(BASE_ARM_SOFT_DIV)"; \
	$(ARM_CC) $(ARM_DEC_FLAGS) $(ARM_OBJECT_OPT) -c "$(ARM_LINK_STUBS)" -o "$$tmp/arm_link_stubs.o"; \
	$(ARM_CC) $(ARM_LINK_FLAGS) "$$tmp/patch_apply_arm.o" "$$tmp/arm_link_stubs.o" \
		$(ARM_LINK_LIBS) -o "$$tmp/patch_apply_arm.elf"; \
	linked_size_out=$$($(ARM_SIZE) "$$tmp/patch_apply_arm.elf"); \
	printf '%s\n' "$$linked_size_out"; \
	echo "arm_linked_integration=no-startup static PatchApply wrapper + minimal flash stubs"; \
	set -- $$(printf '%s\n' "$$linked_size_out" | awk 'NR==2 { print $$1, $$2, $$3 }'); \
	linked_text=$$1; linked_data=$$2; linked_bss=$$3; \
	echo "arm_linked_text=$$linked_text"; \
	echo "arm_linked_data=$$linked_data"; \
	echo "arm_linked_bss=$$linked_bss"; \
	if [ "$$linked_bss" -gt "$(ARM_BSS_HARD_CAP)" ]; then \
		echo "ARM linked .bss hard cap exceeded: $$linked_bss > $(ARM_BSS_HARD_CAP)" >&2; exit 1; \
	fi; \
	test "$$linked_text" -le "$(BASE_ARM_LINKED_TEXT)"; \
	test "$$linked_data" -le "$(BASE_ARM_LINKED_DATA)"; \
	test "$$linked_bss" -le "$(BASE_ARM_LINKED_BSS)"; \
	helpers=$$($(ARM_NM) --defined-only "$$tmp/patch_apply_arm.elf" | \
		awk '$$3 == "memcpy" || $$3 == "memmove" || $$3 == "memset" { print $$3 }' | \
		sort | paste -sd, -); \
	echo "arm_linked_runtime_helpers=$$helpers"; \
	echo "arm_decoder_build=OK (public header set; object + linked)"

# Worst-case caller-stack bounds for both supported wrapper shapes. Since the fiber was deleted
# (44eee88), the whole decode runs on the CALLER's stack; docs/device-integration.md pins the
# shape-specific budgets an integrator must reserve. Builds both harnesses with -fstack-usage
# (gcc -O2, the pessimistic level — deeper than -Os here) and runs scripts/stack_bound.py, which
# sums the per-function .su frames (each already includes its own pushed LR/regs) along the
# deepest path of each object's static call graph. It fails loudly on recursion, an indirect call
# that could reach internal code, or a dynamic/VLA frame — any of which would invalidate the
# static method. The bounds EXCLUDE the integrator's externs (flash_read/flash_write_page/byte-callback)
# and toolchain leaves (memmove/memset/__aeabi_uidiv); each ceiling's headroom absorbs those +
# interrupt-frame slack. Same cross-gcc as check-arm.
check-stack-internal: check-release-profile-internal $(DECODER_PUBLIC_HDRS) \
                      $(DECODER_INTEGRATION_TU)
	@set -e; \
	. ./scripts/tempdir.sh; \
	$(ARM_CC) $(ARM_DEC_FLAGS) $(ARM_STACK_OPT) -DDECODER_INTEGRATION_STATIC -c "$(DECODER_INTEGRATION_TU)" -o "$$tmp/patch_apply_arm.o" -fstack-usage; \
	$(ARM_CC) $(ARM_DEC_FLAGS) $(ARM_STACK_OPT) -DDECODER_INTEGRATION_GENERIC -c "$(DECODER_INTEGRATION_TU)" -o "$$tmp/patch_apply_stack_generic.o" -fstack-usage; \
	OBJDUMP="$(ARM_OBJDUMP)" python3 scripts/stack_bound.py "$$tmp/patch_apply_arm.o" > "$$tmp/stack_static.txt"; \
	OBJDUMP="$(ARM_OBJDUMP)" python3 scripts/stack_bound.py "$$tmp/patch_apply_stack_generic.o" > "$$tmp/stack_generic.txt"; \
	echo "stack_static_integration=static PatchApply wrapper"; \
	sed 's/^stack_/stack_static_/' "$$tmp/stack_static.txt"; \
	echo "stack_static_ceiling_o2=$(BASE_STACK_STATIC_CEIL_O2)"; \
	echo "stack_generic_integration=caller-owned PatchApply * wrapper"; \
	sed 's/^stack_/stack_generic_/' "$$tmp/stack_generic.txt"; \
	echo "stack_generic_ceiling_o2=$(BASE_STACK_GENERIC_CEIL_O2)"; \
	static_bound=$$(sed -n 's/^stack_bound_bytes=//p' "$$tmp/stack_static.txt"); \
	generic_bound=$$(sed -n 's/^stack_bound_bytes=//p' "$$tmp/stack_generic.txt"); \
	test -n "$$static_bound"; \
	test -n "$$generic_bound"; \
	if [ "$$static_bound" -gt "$(BASE_STACK_STATIC_CEIL_O2)" ]; then \
		echo "Static PatchApply wrapper stack ceiling exceeded: $$static_bound > $(BASE_STACK_STATIC_CEIL_O2)" >&2; exit 1; \
	fi; \
	if [ "$$generic_bound" -gt "$(BASE_STACK_GENERIC_CEIL_O2)" ]; then \
		echo "Caller-owned PatchApply * wrapper stack ceiling exceeded: $$generic_bound > $(BASE_STACK_GENERIC_CEIL_O2)" >&2; exit 1; \
	fi; \
	echo "stack_decoder_build=OK (public header set; static + generic)"

# Portability contract: the decoder is standard C (C99 + C11 _Static_assert); GNU
# attributes/builtins are optional codegen hints behind guards with live fallbacks (rc_models.h
# note). -DNO_GNU_EXTENSIONS is the documented knob that forces the fallback branch — a
# first-party switch, so system headers are untouched (no fragile __GNUC__ masking against
# glibc). Wire correctness is compiler-independent; only the gated size/stack budgets are
# GNU-toolchain-measured. Enforced below three ways: (a) the public header set smoke-compiles with
# the fallbacks forced, (b) the preprocessed fallback smoke TU must contain ZERO
# __attribute__/__builtin_ tokens in first-party (src/) lines — this catches a future bare
# GNU-ism that a gcc compile alone would accept (__builtin_offsetof is exempt: it is the
# SYSTEM stddef.h's conforming implementation of the standard offsetof macro our asserts use;
# a non-GNU toolchain's stddef.h supplies its own), (c) a fallback-built host decoder must
# round-trip the real one-face patch byte-exactly.
PORTABLE_FALLBACK_FLAGS := -DNO_GNU_EXTENSIONS
check-decoder-contract-internal: ultrapatch $(DECODER_PUBLIC_HDRS) $(CORPUS_ASSET_PREREQ)
	@set -e; \
	if awk 'FNR==1{allowed=prev; n=split(FILENAME,a,"/"); prev=a[n]} /^[[:space:]]*#include[[:space:]]*"/ && (allowed=="" || index($$0,"\"" allowed "\"")==0){print FILENAME ":" FNR ":" $$0; bad=1} END{exit bad?1:0}' $(DECODER_PUBLIC_HDRS); then :; else \
		echo "decoder headers may include only their previous shipped support header" >&2; exit 1; \
	fi; \
	if grep -nE '\b(malloc|calloc|realloc|free)[[:space:]]*\(' $(DECODER_PUBLIC_HDRS); then \
		echo "decoder header set must not use dynamic memory" >&2; exit 1; \
	fi; \
	if awk '/^static[[:space:]]/ && $$0 !~ /\(/ { print FILENAME ":" FNR ":" $$0; bad=1 } END{ exit bad ? 1 : 0 }' $(DECODER_PUBLIC_HDRS); then :; else \
		echo "decoder header set must not declare file-scope static variables" >&2; exit 1; \
	fi; \
	. ./scripts/tempdir.sh; \
	{ printf '%s\n' '#include <stdint.h>'; \
	  printf '%s\n' '#include "patch_apply.h"'; \
	  printf '%s\n' 'uint8_t flash_read(uint32_t a){ (void)a; return 0xffu; }'; \
	  printf '%s\n' 'void flash_write_page(uint32_t a, const uint8_t p[OUTROW]){ (void)a; (void)p; }'; \
	  printf '%s\n' 'static int next(void *c, uint8_t *out){ (void)c; (void)out; return PATCH_PULL_END; }'; \
	  printf '%s\n' 'int main(void){ PatchApply pa; return patch_apply_run(&pa, next, 0); }'; \
	} > "$$tmp/patch_apply_smoke.c"; \
	$(CC) $(DECODER_CFLAGS) -Wconversion -Isrc -c "$$tmp/patch_apply_smoke.c" -o "$$tmp/patch_apply_smoke.o"; \
	$(CC) $(DECODER_CFLAGS) $(PORTABLE_FALLBACK_FLAGS) -Wconversion -Isrc \
		-c "$$tmp/patch_apply_smoke.c" -o "$$tmp/patch_apply_smoke_fallback.o"; \
	cp "$$tmp/patch_apply_smoke.c" "$$tmp/discard_result.c"; \
	printf '%s\n' 'void discard_result(void){ PatchApply pa; patch_apply_run(&pa, next, 0); }' >> "$$tmp/discard_result.c"; \
	if $(CC) $(DECODER_CFLAGS) -Wconversion -Isrc -c "$$tmp/discard_result.c" -o "$$tmp/discard-result.o" >/dev/null 2>&1; then \
		echo "decoder accepted a discarded patch_apply_run result without a diagnostic" >&2; exit 1; \
	fi; \
	$(CC) $(DECODER_CFLAGS) $(PORTABLE_FALLBACK_FLAGS) -Wconversion -Isrc \
		-c "$$tmp/discard_result.c" -o "$$tmp/discard-result-fallback.o"; \
	if $(CC) $(CFLAGS) -c "$$tmp/patch_apply_smoke.c" -o "$$tmp/missing-base.o" >/dev/null 2>&1; then \
		echo "decoder accepted missing PATCH_IMAGE_BASE" >&2; exit 1; \
	fi; \
	if $(CC) $(CFLAGS) -DPATCH_IMAGE_BASE=0u -c "$$tmp/patch_apply_smoke.c" -o "$$tmp/missing-capacity.o" >/dev/null 2>&1; then \
		echo "decoder accepted missing PATCH_IMAGE_CAPACITY" >&2; exit 1; \
	fi; \
	if $(CC) $(CFLAGS) -DPATCH_IMAGE_BASE=1u -DPATCH_IMAGE_CAPACITY=256u -c "$$tmp/patch_apply_smoke.c" -o "$$tmp/unaligned-base.o" >/dev/null 2>&1; then \
		echo "decoder accepted unaligned PATCH_IMAGE_BASE" >&2; exit 1; \
	fi; \
	if $(CC) $(CFLAGS) -DPATCH_IMAGE_BASE=0u -DPATCH_IMAGE_CAPACITY=0u -c "$$tmp/patch_apply_smoke.c" -o "$$tmp/zero-capacity.o" >/dev/null 2>&1; then \
		echo "decoder accepted zero PATCH_IMAGE_CAPACITY" >&2; exit 1; \
	fi; \
	if $(CC) $(CFLAGS) -DPATCH_IMAGE_BASE=-256 -DPATCH_IMAGE_CAPACITY=256u -c "$$tmp/patch_apply_smoke.c" -o "$$tmp/negative-base.o" >/dev/null 2>&1; then \
		echo "decoder accepted negative PATCH_IMAGE_BASE" >&2; exit 1; \
	fi; \
	if $(CC) $(CFLAGS) -DPATCH_IMAGE_BASE=0u -DPATCH_IMAGE_CAPACITY=-256 -c "$$tmp/patch_apply_smoke.c" -o "$$tmp/negative-capacity.o" >/dev/null 2>&1; then \
		echo "decoder accepted negative PATCH_IMAGE_CAPACITY" >&2; exit 1; \
	fi; \
	if $(CC) $(CFLAGS) -DPATCH_IMAGE_BASE=0u -DPATCH_IMAGE_CAPACITY=257u -c "$$tmp/patch_apply_smoke.c" -o "$$tmp/unaligned-capacity.o" >/dev/null 2>&1; then \
		echo "decoder accepted non-page-aligned PATCH_IMAGE_CAPACITY" >&2; exit 1; \
	fi; \
	if $(CC) $(CFLAGS) -DPATCH_IMAGE_BASE=0xffffff00u -DPATCH_IMAGE_CAPACITY=512u -c "$$tmp/patch_apply_smoke.c" -o "$$tmp/overflow-base.o" >/dev/null 2>&1; then \
		echo "decoder accepted PATCH_IMAGE_BASE + PATCH_IMAGE_CAPACITY overflow" >&2; exit 1; \
	fi; \
	$(CC) $(DECODER_CFLAGS) $(PORTABLE_FALLBACK_FLAGS) -Isrc -E "$$tmp/patch_apply_smoke.c" | \
	awk '/^# [0-9]+ "/ { inours = ($$3 ~ /"src\//) } \
	     inours { line = $$0; gsub(/__builtin_offsetof/, "", line); \
	              if (line ~ /__attribute__|__builtin_/) { print "GNU construct in portable decoder build: " $$0; bad=1 } } \
	     END { exit bad ? 1 : 0 }'; \
	$(CC) $(DECODER_CFLAGS) $(PORTABLE_FALLBACK_FLAGS) $(DEC_DEMO_DEFINES) \
	    $(DEC_STANDALONE_SRCS) -o "$$tmp/dec_portable"; \
	FIXTURES="$(FIXTURES)" ONEFACE_ROUNDTRIP=1 \
	    scripts/oneface_metrics.sh "$(HOST_TOOL)" "$$tmp/dec_portable" >/dev/null; \
	CC="$(CC)" NM="$(NM)" CFLAGS="$(DECODER_CFLAGS)" \
	  DECODER_PUBLIC_HDRS="$(DECODER_PUBLIC_HDRS)" FIXTURES="$(FIXTURES)" \
	  scripts/check_decoder_api.sh; \
	echo "decoder_address_contract=OK (mandatory base/capacity + page alignment + uint32 headroom)"; \
	echo "decoder_portable=OK (fallback branch: compile + GNU-free purity + one-face round-trip)"; \
	echo "decoder_contract=OK"

# Pointer-rich in-memory decoder/backend contract under dynamic sanitizers. Standalone so its
# instrumented compile does not contend with the CPU-saturated corpus workers in `make gate`.
check-decoder-sanitize-internal: ultrapatch $(DECODER_PUBLIC_HDRS) $(CORPUS_ASSET_PREREQ)
	@CC="$(CC)" NM="$(NM)" CFLAGS="$(DECODER_CFLAGS)" \
	  DECODER_PUBLIC_HDRS="$(DECODER_PUBLIC_HDRS)" FIXTURES="$(FIXTURES)" \
	  DECODER_API_REGULAR=0 DECODER_API_SANITIZE=1 scripts/check_decoder_api.sh

# Host-encoder algorithm probes under dynamic sanitizers. Standalone so the instrumented
# builds do not contend with the CPU-saturated corpus workers in `make gate`.
check-encoder-sanitize-internal:
	@CC="$(CC)" CFLAGS="$(DECODER_CFLAGS)" ENC_SEAM_SRCS="$(ENC_SEAM_SRCS)" \
	  scripts/check_encoder_sanitize.sh

check-models-internal: $(DECODER_PUBLIC_HDRS)
	@CC="$(CC)" CFLAGS="$(DECODER_CFLAGS)" scripts/check_models.sh

check-assets-internal: $(CORPUS_ASSET_STAMP)
	@python3 scripts/corpus_topology.py verify --inventory "$(CORPUS_INVENTORY)" \
	  --source-root test-bench --output-root "$(CORPUS_BUILD_DIR)"

# qemu-based decode validation REMOVED - permanent decision (owner, 2026-07-03): too slow
# for its marginal value (the 260-pair matrix re-encoded every corpus pair just to apply it
# under emulation; ~45 CPU-min per run). Host-vs-ARM divergence is systematic when it exists,
# not pair-specific; the ARM cross-build + size/divide gate (check-arm) still compiles the
# real Thumb-1 decoder every cycle, and a one-time 260-pair qemu study (db6d693) found ZERO
# divergence. Do not reintroduce qemu legs into the gate.

check-malformed-internal: ultrapatch $(CORPUS_ASSET_PREREQ)
	@FIXTURES="$(FIXTURES)" scripts/check_malformed.sh
	@CC="$(CC)" CFLAGS="$(CFLAGS)" FIXTURES="$(FIXTURES)" scripts/check_transactional.sh
	@CC="$(CC)" CFLAGS="$(CFLAGS)" scripts/check_elf_ranges.sh

# Synthetic edge inputs the firmware corpus never exercises (empty/tiny/equal/random/text/
# page-boundary/>384KiB-span pairs). ultrapatch self-verifies every encoded blob, so each case must
# either round-trip byte-exactly through BOTH host decoders or refuse cleanly.
check-edge-internal: ultrapatch
	@scripts/check_edge.sh

# Degradation / direction / row-window / big-span gate: synthetic pairs that FORCE each encoder
# path the golden set and home corpus never exercise (journal-budget degradation, OPC_CAP
# op-split, unnatural apply direction, row-window-oracle reliance, big-span journal), asserting
# the path was actually taken — not merely that the blob round-trips. Builds a D=1 variant decoder
# to prove the monotone larger-window compatibility contract. Small synthetic fixtures, fast.
check-degrade-internal: ultrapatch $(CORPUS_ASSET_PREREQ) $(ENCODER_KERNEL_BASELINE)
	@CC="$(CC)" CLANG="$(CLANG)" CFLAGS="$(DECODER_CFLAGS)" FIXTURES="$(FIXTURES)" IMAGES="$(IMAGES)" \
	  ENC_SEAM_SRCS="$(ENC_SEAM_SRCS)" DEC_STANDALONE_SRCS="$(DEC_STANDALONE_SRCS)" \
	  DEC_DEMO_DEFINES="$(DEC_DEMO_DEFINES)" \
	  ENCODER_KERNEL_BASELINE="$(ENCODER_KERNEL_BASELINE)" scripts/check_degrade.sh

# Golden-wire regression for the four wires not already covered by the corpus matrix. The
# combined baseline also carries all 290 corpus hashes and 256 home per-pair sizes. On an
# INTENDED wire change, `make golden-update` measures a private candidate, validates the full
# round-trip/write-safety and no-regression policy, then replaces the baseline and its four
# Makefile ratchets. Commit both files in the SAME commit. If publication is interrupted between
# the ordinary replaces, restore both files from Git and rerun the target.
check-golden-internal: ultrapatch $(CORPUS_ASSET_PREREQ) $(WIRE_BASELINE)
	@FIXTURES="$(FIXTURES)" IMAGES="$(IMAGES)" WIRE_BASELINE="$(WIRE_BASELINE)" \
	  scripts/check_golden.sh check

# Intentional-wire-change A/B regression: a small real home+foreign matrix verifies that both
# measurement runs bypass the committed wire manifest while retaining round-trip and NVM checks.
check-ab-matrix-internal: ultrapatch $(CORPUS_ASSET_PREREQ)
	@FIXTURES="$(FIXTURES)" scripts/check_ab_matrix.sh

golden-update-internal: scripts/publish_wire_baselines.py scripts/wire_baseline.py \
                        scripts/corpus_topology.py
	@$(MAKE) --no-print-directory golden-update-inputs-internal
	@$(MAKE) --no-print-directory golden-update-measure-internal

.PHONY: golden-update-measure-internal
golden-update-measure-internal: ultrapatch $(CORPUS_ASSET_PREREQ) scripts/publish_wire_baselines.py \
                                scripts/corpus_topology.py $(CORPUS_INVENTORY)
	@set -e; \
	. ./scripts/tempdir.sh; \
	release_profile=$$(python3 scripts/build_profile.py verify-release "$(RELEASE_PROFILE_LOCK)"); \
	release_profile=$${release_profile#release_profile=}; \
	host_tool_hash=$$(sha256sum "$(HOST_TOOL)"); host_tool_hash=$${host_tool_hash%% *}; \
	exec 9<>"$(WIRE_BASELINE_LOCK)"; flock --shared 9; \
	preimage_baseline=$$(sha256sum "$(WIRE_BASELINE)"); preimage_baseline=$${preimage_baseline%% *}; \
	preimage_make=$$(sha256sum Makefile); preimage_make=$${preimage_make%% *}; \
	flock --unlock 9; exec 9>&-; \
	FIXTURES="$(FIXTURES)" IMAGES="$(IMAGES)" GOLDEN_DUMP="$$tmp/golden.tsv" \
	  scripts/check_golden.sh update >"$$tmp/golden.out"; \
	IMAGES="$(IMAGES)" FOREIGN="$(FOREIGN)" CORPUS_INVENTORY="$(CORPUS_INVENTORY)" \
	  CORPUS_SIZE_BASELINE="" WIRE_BASELINE="" \
	  WIRE_BASELINE_DUMP="$$tmp/pairs.tsv" BASE_FULL_TOTAL="" BASE_FOREIGN_TOTAL="" \
	  ./check_corpus.sh $(JOBS) >"$$tmp/corpus.out"; \
	cat "$$tmp/pairs.tsv" "$$tmp/golden.tsv" >"$$tmp/wire-baseline.tsv"; \
	eval "$$(python3 scripts/corpus_topology.py counts --inventory '$(CORPUS_INVENTORY)')"; \
	home_pairs=$$CORPUS_TOPOLOGY_HOME_PAIRS; \
	foreign_pairs=$$CORPUS_TOPOLOGY_FOREIGN_PAIRS; \
	test "$$(wc -l <"$$tmp/golden.tsv")" -eq "$(BASE_RELEASE_GOLDEN_BLOBS)"; \
	test "$$(wc -l <"$$tmp/pairs.tsv")" -eq "$$((home_pairs + foreign_pairs))"; \
	test "$$(wc -l <"$$tmp/wire-baseline.tsv")" -eq \
	  "$$((home_pairs + foreign_pairs + $(BASE_RELEASE_GOLDEN_BLOBS)))"; \
	test "$$(sha256sum "$(HOST_TOOL)" | awk '{print $$1}')" = "$$host_tool_hash"; \
	test "$$(python3 scripts/build_profile.py verify-release "$(RELEASE_PROFILE_LOCK)")" = \
	  "release_profile=$$release_profile"; \
	cp "$$tmp/corpus.out" "$$tmp/metrics.out"; \
	echo "measurement_release_profile=$$release_profile" >>"$$tmp/metrics.out"; \
	echo "measurement_host_tool_sha256=$$host_tool_hash" >>"$$tmp/metrics.out"; \
	echo "measurement_preimage_baseline_sha256=$$preimage_baseline" >>"$$tmp/metrics.out"; \
	echo "measurement_preimage_makefile_sha256=$$preimage_make" >>"$$tmp/metrics.out"; \
	exec 9<>"$(WIRE_BASELINE_LOCK)"; flock --shared 9; \
	$(MAKE) --no-print-directory golden-update-validate-canonical-internal \
	  >"$$tmp/post-measure-inputs.out"; \
	flock --unlock 9; exec 9>&-; \
	python3 scripts/publish_wire_baselines.py --root test-bench \
	  --inventory "$(CORPUS_INVENTORY)" --candidate-baseline "$$tmp/wire-baseline.tsv" \
	  --metrics "$$tmp/metrics.out" \
	  --host-tool "$(HOST_TOOL)" --release-profile-lock "$(RELEASE_PROFILE_LOCK)" \
	  --home-limit "$(BASE_FULL_TOTAL)" --foreign-limit "$(BASE_FOREIGN_TOTAL)" \
	  --oneface-grow-limit "$(BASE_ONEFACE_GROW)" \
	  --oneface-revert-limit "$(BASE_ONEFACE_REVERT)"; \
	cat "$$tmp/golden.out"; cat "$$tmp/corpus.out"; \
	awk '$$1=="G" && $$2=="oneface_grow.blob"{print "oneface_grow=" $$4} \
	     $$1=="G" && $$2=="oneface_revert.blob"{print "oneface_revert=" $$4}' "$(WIRE_BASELINE)"; \
	echo "wire baseline and Makefile BASE_* ratchets published"

# The 256 derived home (from,to) pairs PLUS 34 explicitly inventoried foreign pair-directions
# (a second, unrelated Cortex-M0+
# lineage — CircuitPython feather_m0_express; see docs/foreign-firmware-study.md) are independent,
# so they run in ONE parallel pool across all cores via check_corpus.sh (each worker gets its own
# mktemp dir — no shared blob path, contamination-safe). The foreign set's cross-major pair is the
# single slowest job and is scheduled first so it overlaps everything (LPT), keeping the leg near
# its home-only wall. matrix_ok/full_total gate the home set; home_size_{better,worse,equal}
# compares each home pair against WIRE_BASELINE and rejects any worse pair;
# foreign_ok(=34)/foreign_total gate the foreign set (foreign_total ratchets vs
# BASE_FOREIGN_TOTAL); NVM write-safety maxima cover BOTH. Override parallelism with JOBS=N
# and AB_MATRIX_TEST_JOBS=N. Standalone checks reserve one eighth of the available cores for
# the concurrent A/B check; the full gate chooses its own split because it has more auxiliary legs.
check-corpus-internal: ultrapatch
	@set -e; \
	. ./scripts/tempdir.sh; \
	jobs="$(JOBS)"; \
	ab_jobs="$${AB_MATRIX_TEST_JOBS:-}"; \
	if [ -z "$$jobs" ]; then \
	  cores=$$(nproc 2>/dev/null || echo 4); jobs=$$((cores * 7 / 8)); [ "$$jobs" -gt 0 ] || jobs=1; \
	  if [ -z "$$ab_jobs" ]; then ab_jobs=$$((cores - jobs)); [ "$$ab_jobs" -gt 0 ] || ab_jobs=1; fi; \
	fi; \
	[ -n "$$ab_jobs" ] || ab_jobs=8; \
	nice -n 10 $(MAKE) --no-print-directory -o "$(HOST_TOOL)" AB_MATRIX_TEST_JOBS="$$ab_jobs" check-ab-matrix-internal >"$$tmp/ab.txt" 2>&1 & apid=$$!; \
	$(MAKE) --no-print-directory -o "$(HOST_TOOL)" JOBS="$$jobs" check-corpus-matrix-internal >"$$tmp/matrix.txt" 2>&1 & mpid=$$!; \
	cleanup_children(){ kill "$$apid" "$$mpid" 2>/dev/null || :; wait "$$apid" 2>/dev/null || :; wait "$$mpid" 2>/dev/null || :; rm -rf "$$tmp"; }; \
	trap 'cleanup_children' EXIT; \
	trap 'cleanup_children; trap - TERM INT EXIT; kill -s TERM "$$$$"' TERM; \
	trap 'cleanup_children; trap - TERM INT EXIT; kill -s INT "$$$$"' INT; \
	rc=0; wait "$$apid" || rc=1; wait "$$mpid" || rc=1; \
	cat "$$tmp/ab.txt" "$$tmp/matrix.txt"; \
	exit "$$rc"

.PHONY: check-corpus-matrix-internal
check-corpus-matrix-internal: ultrapatch $(CORPUS_ASSET_PREREQ) scripts/corpus_topology.py \
                              $(CORPUS_INVENTORY)
	@IMAGES="$(IMAGES)" FOREIGN="$(FOREIGN)" WIRE_BASELINE="$(WIRE_BASELINE)" \
	CORPUS_INVENTORY="$(CORPUS_INVENTORY)" \
	BASE_FULL_TOTAL="$(BASE_FULL_TOTAL)" BASE_FOREIGN_TOTAL="$(BASE_FOREIGN_TOTAL)" \
	./check_corpus.sh $(JOBS)

# THE gate — one target, everything, hard budget <= 80 s wall on the reference machine
# (measured 35.1 s warm at 32 cores with resource-pressure direction fallback).
# Builds up-front, then runs
# every leg CONCURRENTLY:
# check-assets, check (one-face grow/revert round-trip + BASE_ONEFACE_* size gates),
# check-malformed, check-edge, check-degrade, check-golden, check-decoder-contract,
# check-ab-matrix,
# check-models, check-arm (sizes + divide policy), check-stack, and the FULL 256-pair corpus
# matrix + 34 foreign
# pair-directions (corpus full_total vs BASE_FULL_TOTAL, home per-pair better/worse/equal
# split vs WIRE_BASELINE with zero worse pairs allowed, foreign_total vs
# BASE_FOREIGN_TOTAL, foreign 34/34 round-trips, NVM write-safety, journal peak —
# check-corpus). Wall time ~= the slowest leg
# (check-corpus), not the sum. Prints one consolidated summary with every tracked
# metric; exits nonzero if ANY gate fails and dumps the raw blocks so the offending
# metric is visible. The profile-specific host tool is published atomically BEFORE the legs fork;
# run_gate.sh invokes each forked leg with make's `-o` (assume-old), so no leg rebuilds it if a
# source mtime changes at sub-make startup. Same-profile concurrent top-level builds may both
# publish equivalent bytes.
gate-internal:
	@$(MAKE) --no-print-directory release-gate-inputs-internal
	@$(MAKE) --no-print-directory all-internal
	@$(MAKE) --no-print-directory corpus-assets-internal
	@set -e; release_profile=$$(python3 scripts/build_profile.py verify-release "$(RELEASE_PROFILE_LOCK)"); \
	MAKE="$(MAKE)" HOST_TOOL="$(HOST_TOOL)" \
	CORPUS_ASSET_STAMP="$(CORPUS_ASSET_STAMP)" \
	RELEASE_PROFILE="$$release_profile" \
	BASE_ARM_TEXT="$(BASE_ARM_TEXT)" BASE_ARM_DATA="$(BASE_ARM_DATA)" \
	BASE_ARM_BSS="$(BASE_ARM_BSS)" BASE_ARM_LINKED_TEXT="$(BASE_ARM_LINKED_TEXT)" \
	BASE_ARM_LINKED_DATA="$(BASE_ARM_LINKED_DATA)" BASE_ARM_LINKED_BSS="$(BASE_ARM_LINKED_BSS)" \
	BASE_STACK_STATIC_CEIL_O2="$(BASE_STACK_STATIC_CEIL_O2)" \
	BASE_STACK_GENERIC_CEIL_O2="$(BASE_STACK_GENERIC_CEIL_O2)" \
	JOBS="$(JOBS)" scripts/run_gate.sh

# Static-analysis leg: gcc -fanalyzer over first-party TUs (encoder modules + decoder + arm + selfcheck)
# with a curated flag set; clean baseline (exits nonzero on any NEW finding). STANDALONE (version-
# fragile + ~16 s), NOT in `make gate`; auto-skips where gcc -fanalyzer is unavailable.
check-analyze-internal:
	@CC="$(CC)" CONTRACT_FLAGS="$(CONTRACT_FLAGS)" \
	  DECODER_CONFIG_FLAGS="$(DECODER_CONFIG_FLAGS)" ENC_MODULES="$(ENC_MODULE_SRCS)" scripts/check_analyze.sh

clean-internal:
	@root=$$(realpath -m .build); dir=$$(realpath -m "$(BUILD_DIR)"); \
	case "$$dir" in "$$root"/*) rm -rf -- "$$dir" ;; *) echo "refusing to clean noncanonical build dir: $$dir" >&2; exit 1 ;; esac
	rm -f ultrapatch

clean-all-internal:
	rm -rf .build
	rm -f ultrapatch

# This parse-time authority check must follow every release variable definition. It still runs
# before any recipe and therefore cannot be suppressed by MAKEFLAGS=-i/-n/-t.
ifneq ($(CANONICAL_AUTHORITY_GOAL),)
$(foreach v,$(RELEASE_GATE_FIXED_VARS),$(if $(filter file,$(origin $(v))),,$(error $(CANONICAL_AUTHORITY_GOAL) rejects runtime override: $(v) (origin $(origin $(v))))))
$(foreach v,$(RELEASE_GATE_UNSET_VARS),$(if $(filter undefined,$(origin $(v))),,$(error $(CANONICAL_AUTHORITY_GOAL) requires unset mode: $(v) (origin $(origin $(v))))))
endif
