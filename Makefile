# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

CC = $(CROSS_COMPILE)gcc
CLANG ?= clang
NM ?= nm
ARM_PREFIX ?= arm-none-eabi-
ARM_CC ?= $(ARM_PREFIX)gcc
ARM_SIZE ?= $(ARM_PREFIX)size
ARM_OBJDUMP ?= $(ARM_PREFIX)objdump
ARM_NM ?= $(ARM_PREFIX)nm
ARM_OBJECT_OPT ?= -Os
ARM_STACK_OPT ?= -O2

OPT ?= -O2
# Every wire-affecting override belongs here, under the exact macro name consumed by
# patch_config.h/rc_models.h. Encoder and decoder builds MUST use this same value.
WIRE_CONFIG_FLAGS ?= -DCORTEX_M0
# Decoder/device integration only: these partition values are not part of the patch wire.
# Repository host/ARM harnesses intentionally model a 64 MiB image partition based at zero.
DECODER_CONFIG_FLAGS ?= -DPATCH_IMAGE_BASE=0u -DPATCH_IMAGE_CAPACITY=67108864u
# Language/include contract shared by host builds and the analyzer. Warning/optimization policy
# stays leg-local; decoder-containing commands append DECODER_CONFIG_FLAGS explicitly.
CONTRACT_FLAGS := -std=c99 -I. -Isrc -Ivendor/libdivsufsort
CFLAGS += $(CONTRACT_FLAGS)
CFLAGS += $(WIRE_CONFIG_FLAGS)
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

# Host builds are isolated by the compiler identity and every effective build flag. The profile
# helper emits canonical JSON without workspace paths; its SHA-256 is both the build-directory
# key and the provenance identity. Tests receive the exact binary through ULTRAPATCH, so a GCC,
# Clang, or alternate-config invocation never executes whichever shared root binary linked last.
override UP_PROFILE_CC := $(CC)
override UP_PROFILE_CLANG := $(CLANG)
override UP_PROFILE_ENCODER_CFLAGS := $(CFLAGS)
override UP_PROFILE_BACKEND_CFLAGS := $(DECODER_CFLAGS) $(HOST_BACKEND_DEFINES)
override UP_PROFILE_LINK_CFLAGS := $(CFLAGS)
override UP_PROFILE_LDFLAGS := $(LDFLAGS)
override UP_PROFILE_WIRE_FLAGS := $(WIRE_CONFIG_FLAGS)
override UP_PROFILE_DECODER_FLAGS := $(DECODER_CONFIG_FLAGS)
override UP_PROFILE_ARM_CC := $(ARM_CC)
override UP_PROFILE_ARM_SIZE := $(ARM_SIZE)
override UP_PROFILE_ARM_OBJDUMP := $(ARM_OBJDUMP)
override UP_PROFILE_ARM_NM := $(ARM_NM)
override UP_PROFILE_ARM_FLAGS = $(ARM_DEC_FLAGS)
override UP_PROFILE_ARM_OBJECT_OPT := $(ARM_OBJECT_OPT)
override UP_PROFILE_ARM_STACK_OPT := $(ARM_STACK_OPT)
export UP_PROFILE_CC UP_PROFILE_CLANG UP_PROFILE_ENCODER_CFLAGS UP_PROFILE_BACKEND_CFLAGS
export UP_PROFILE_LINK_CFLAGS UP_PROFILE_LDFLAGS
export UP_PROFILE_WIRE_FLAGS UP_PROFILE_DECODER_FLAGS
export UP_PROFILE_ARM_CC UP_PROFILE_ARM_SIZE UP_PROFILE_ARM_OBJDUMP UP_PROFILE_ARM_NM
export UP_PROFILE_ARM_FLAGS UP_PROFILE_ARM_OBJECT_OPT UP_PROFILE_ARM_STACK_OPT

BUILD_ROOT ?= .build
override BUILD_PROFILE_ID := $(shell python3 scripts/build_profile.py host-id)
ifeq ($(strip $(BUILD_PROFILE_ID)),)
$(error failed to derive the host build profile)
endif
BUILD_DIR ?= $(BUILD_ROOT)/$(BUILD_PROFILE_ID)
ifeq ($(abspath $(BUILD_DIR)),$(CURDIR))
$(error BUILD_DIR must not be the repository root)
endif
override PROFILE_MANIFEST := $(BUILD_DIR)/profile.json
override HOST_TOOL := $(abspath $(BUILD_DIR))/ultrapatch
override HOST_OBJ_DIR := $(abspath $(BUILD_DIR))/obj
override HOST_BUILD_RECIPE := $(abspath $(BUILD_DIR))/host-build.recipe.json
RELEASE_PROFILE_LOCK ?= toolchains/release-profile.json
override ULTRAPATCH := $(HOST_TOOL)
override ULTRAPATCH_DECODE := $(HOST_TOOL)
export ULTRAPATCH ULTRAPATCH_DECODE

DIVSUF := vendor/libdivsufsort/divsufsort.c
CONFIG_HDR := src/patch_config.h
APPLY_HDR := src/patch_apply.h
DECODER_PUBLIC_HDRS := $(CONFIG_HDR) src/rc_models.h $(APPLY_HDR)
DECODER_SINGLE_HDR ?= artifacts/patch_apply_single.h
override DECODER_CANONICAL_HDR := $(abspath $(DECODER_SINGLE_HDR))
# Shared host-side NVM emulator, #included by patch_host_backend.c before patch_apply.h.
NVM_EMU := src/nvm_emu.inc
# Host encoder modules. The encoder is a normal CLI tool and may rely on this Makefile;
# the device decoder remains header-only for integrators.
ENC_MODULE_SRCS := src/enc_util.c src/enc_elf.c src/enc_bsdiff.c src/enc_field.c \
                   src/enc_rc.c src/enc_lz.c src/enc_emit.c src/enc_plan.c
GEN_HDR := src/rc_models.h $(CONFIG_HDR) src/enc_internal.h
# The host backend owns the single reference-decoder copy used by encode
# selfcheck, CLI decode, and standalone decoder builds.
HOST_BACKEND_SRC := src/patch_host_backend.c
ENC_SEAM_SRCS := $(filter-out src/enc_plan.c,$(ENC_MODULE_SRCS)) $(HOST_BACKEND_SRC) $(DIVSUF)
# Standalone host-decoder TU pair + demo defines, shared once by dec_portable, check_degrade's D=1, and all-internal.
DEC_STANDALONE_SRCS := $(HOST_BACKEND_SRC) src/enc_util.c
DEC_DEMO_DEFINES := -DPATCH_APPLY_DEMO_MAIN -D_POSIX_C_SOURCE=200809L
TOOL_SRCS := src/patch_generate.c $(ENC_MODULE_SRCS) $(HOST_BACKEND_SRC) $(DIVSUF)

# The CLI is built per translation unit under its compiler/flag profile. Encoder and vendored
# sources use the encoder flags; only the TU that embeds patch_apply.h receives the decoder
# integration flags. The recipe tag deliberately participates in the build signature so a recipe
# semantic change can invalidate an otherwise unchanged compiler profile.
HOST_BUILD_RECIPE_TAG ?= per-tu-v1
override HOST_ENCODER_SRCS := $(filter-out $(HOST_BACKEND_SRC),$(TOOL_SRCS))
override HOST_ENCODER_OBJS := $(addprefix $(HOST_OBJ_DIR)/,$(HOST_ENCODER_SRCS:.c=.o))
override HOST_BACKEND_OBJS := $(addprefix $(HOST_OBJ_DIR)/,$(HOST_BACKEND_SRC:.c=.o))
override HOST_TOOL_OBJECTS := $(HOST_ENCODER_OBJS) $(HOST_BACKEND_OBJS)
override HOST_TOOL_DEPFILES := $(HOST_TOOL_OBJECTS:.o=.d)

override UP_BUILD_CC := $(CC)
override UP_BUILD_ENCODER_SOURCES := $(HOST_ENCODER_SRCS)
override UP_BUILD_BACKEND_SOURCES := $(HOST_BACKEND_SRC)
override UP_BUILD_OBJECTS := $(patsubst $(HOST_OBJ_DIR)/%,obj/%,$(HOST_TOOL_OBJECTS))
override UP_BUILD_ENCODER_FLAGS := $(CFLAGS)
override UP_BUILD_BACKEND_FLAGS := $(DECODER_CFLAGS) $(HOST_BACKEND_DEFINES)
override UP_BUILD_LINK_FLAGS := $(CFLAGS)
override UP_BUILD_LDFLAGS := $(LDFLAGS)
override UP_BUILD_RECIPE_TAG := $(HOST_BUILD_RECIPE_TAG)
export UP_BUILD_CC UP_BUILD_ENCODER_SOURCES UP_BUILD_BACKEND_SOURCES UP_BUILD_OBJECTS
export UP_BUILD_ENCODER_FLAGS UP_BUILD_BACKEND_FLAGS UP_BUILD_LINK_FLAGS UP_BUILD_LDFLAGS
export UP_BUILD_RECIPE_TAG

FIXTURES ?= test-bench/fixtures
IMAGES ?= test-bench/images
FOREIGN ?= test-bench/foreign
CORPUS_MANIFEST ?= test-bench/corpus.sha256
FOREIGN_MANIFEST ?= test-bench/foreign.sha256
CORPUS_INVENTORY ?= test-bench/release-inventory.tsv
CORPUS_SIZE_BASELINE ?= test-bench/home-size-baseline.tsv
CORPUS_WIRE_MANIFEST ?= test-bench/corpus-wire.sha256
GOLDEN_MANIFEST ?= test-bench/golden.sha256

# Release scope pins. The inventory names every member in order; these cardinalities make a
# deletion or reduced release set an explicit policy change rather than a self-consistent shrink.
BASE_RELEASE_FIXTURES ?= 2
BASE_RELEASE_HOME_IMAGES ?= 16
BASE_RELEASE_FOREIGN_IMAGES ?= 18
BASE_RELEASE_GOLDEN_BLOBS ?= 8
BASE_FULL_TOTAL ?= 4151373
# Foreign lineage (CircuitPython feather_m0_express, 34 pair-directions): summed blob bytes.
# Ratchets like BASE_FULL_TOTAL — a wire regression on firmware A1 was NOT tuned on fails here.
# Re-pin on intentional wire changes. See docs/foreign-firmware-study.md.
BASE_FOREIGN_TOTAL ?= 1333390
BASE_ONEFACE_GROW ?= 573
BASE_ONEFACE_REVERT ?= 287
BASE_ARM_TEXT ?= 6073
BASE_ARM_DATA ?= 0
BASE_ARM_BSS ?= 10296
BASE_ARM_LINKED_TEXT ?= 6653
BASE_ARM_LINKED_DATA ?= 0
BASE_ARM_LINKED_BSS ?= 10296
BASE_ARM_SOFT_DIV ?= 0
# Optional exactly-once linkage, measured as one implementation object plus one declarations-only
# static-state wrapper. These are separate soft text ceilings; the default static form above stays
# the production footprint ratchet.
BASE_ARM_EXTERNAL_TEXT ?= 6281
BASE_ARM_EXTERNAL_LINKED_TEXT ?= 6861
# Product SRAM ceiling: unlike the configurable size ratchet above, command-line and
# environment overrides must never be able to raise or disable this limit.
override ARM_BSS_HARD_CAP := 12288
ARM_COMMON_FLAGS := -mcpu=cortex-m0plus -mthumb $(WIRE_CONFIG_FLAGS) $(DECODER_CONFIG_FLAGS)
ARM_DEC_FLAGS := $(ARM_COMMON_FLAGS) -I src
ARM_SINGLE_DEC_FLAGS := $(ARM_COMMON_FLAGS)
SINGLE_DECODER_CFLAGS = $(filter-out -Isrc,$(DECODER_CFLAGS))
ARM_LINK_STUBS ?= scripts/arm_link_stubs.c
ARM_LINK_LAYOUT := scripts/arm_link.ld
# The production ARM size gate intentionally measures the static-state wrapper integration
# used by rcv3_run below. A generic caller-owned PatchApply * wrapper may compile differently;
# product notes/gate output must not present this number as shape-independent.
ARM_APPLY_HARNESS = printf '%s\n' '\#include "patch_apply.h"' 'static PatchApply g_patch_apply_state;' 'PatchApplyResult rcv3_run(PatchPull next, void *ctx){ return patch_apply_run(&g_patch_apply_state, next, ctx); }' > "$$tmp/patch_apply_arm.c"
STACK_GENERIC_HARNESS = printf '%s\n' '\#include "patch_apply.h"' 'PatchApplyResult rcv3_run(PatchApply *state, PatchPull next, void *ctx){ return patch_apply_run(state, next, ctx); }' > "$$tmp/patch_apply_stack_generic.c"
ARM_SINGLE_APPLY_HARNESS = printf '%s\n' '\#include "$(DECODER_CANONICAL_HDR)"' 'static PatchApply g_patch_apply_state;' 'PatchApplyResult rcv3_run(PatchPull next, void *ctx){ return patch_apply_run(&g_patch_apply_state, next, ctx); }' > "$$tmp/patch_apply_arm_single.c"
STACK_SINGLE_GENERIC_HARNESS = printf '%s\n' '\#include "$(DECODER_CANONICAL_HDR)"' 'PatchApplyResult rcv3_run(PatchApply *state, PatchPull next, void *ctx){ return patch_apply_run(state, next, ctx); }' > "$$tmp/patch_apply_stack_generic_single.c"
ARM_EXTERNAL_IMPL_HARNESS = printf '%s\n' '\#define ULTRAPATCH_IMPLEMENTATION' '\#include "patch_apply.h"' > "$$tmp/patch_apply_external_impl.c"
ARM_EXTERNAL_APPLY_HARNESS = printf '%s\n' '\#define ULTRAPATCH_DECLARATIONS_ONLY' '\#include "patch_apply.h"' 'static PatchApply g_patch_apply_state;' 'PatchApplyResult rcv3_run(PatchPull next, void *ctx){ return patch_apply_run(&g_patch_apply_state, next, ctx); }' > "$$tmp/patch_apply_external_arm.c"
STACK_EXTERNAL_GENERIC_HARNESS = printf '%s\n' '\#define ULTRAPATCH_DECLARATIONS_ONLY' '\#include "patch_apply.h"' 'PatchApplyResult rcv3_run(PatchApply *state, PatchPull next, void *ctx){ return patch_apply_run(state, next, ctx); }' > "$$tmp/patch_apply_external_stack_generic.c"
ARM_SINGLE_EXTERNAL_IMPL_HARNESS = printf '%s\n' '\#define ULTRAPATCH_IMPLEMENTATION' '\#include "$(DECODER_CANONICAL_HDR)"' > "$$tmp/patch_apply_external_impl_single.c"
ARM_SINGLE_EXTERNAL_APPLY_HARNESS = printf '%s\n' '\#define ULTRAPATCH_DECLARATIONS_ONLY' '\#include "$(DECODER_CANONICAL_HDR)"' 'static PatchApply g_patch_apply_state;' 'PatchApplyResult rcv3_run(PatchPull next, void *ctx){ return patch_apply_run(&g_patch_apply_state, next, ctx); }' > "$$tmp/patch_apply_external_arm_single.c"
STACK_SINGLE_EXTERNAL_GENERIC_HARNESS = printf '%s\n' '\#define ULTRAPATCH_DECLARATIONS_ONLY' '\#include "$(DECODER_CANONICAL_HDR)"' 'PatchApplyResult rcv3_run(PatchApply *state, PatchPull next, void *ctx){ return patch_apply_run(state, next, ctx); }' > "$$tmp/patch_apply_external_stack_generic_single.c"
# Independent worst-case caller-stack ceilings for the two real integration shapes, gcc -O2,
# Cortex-M0+ (bytes). scripts/stack_bound.py derives each exact static bound from
# -fstack-usage frames + its harness object's call graph. check-stack reports and gates both.
BASE_STACK_STATIC_CEIL_O2 ?= 480
BASE_STACK_GENERIC_CEIL_O2 ?= 480
BASE_STACK_EXTERNAL_STATIC_CEIL_O2 ?= 432
BASE_STACK_EXTERNAL_GENERIC_CEIL_O2 ?= 432

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
CAPPED := all decoder-header check check-arm check-stack check-assets check-ab-matrix check-decoder-contract check-decoder-sanitize \
          check-wire-config check-build-profile check-release-profile check-release-gate-contract check-release-inventory check-pack-corpus \
          check-models check-malformed check-corpus check-edge check-degrade check-golden \
          golden-update check-analyze clean clean-all
.PHONY: $(CAPPED) $(addsuffix -internal,$(CAPPED)) gate gate-internal
$(CAPPED): %:
	@if [ "$*" = golden-update ] && [ "$(origin MAKE)" != default ]; then \
	  echo "golden update rejects runtime override: MAKE (origin $(origin MAKE))" >&2; exit 1; fi
	@timeout $(GATE_TIMEOUT) $(MAKE) --no-print-directory $*-internal; s=$$?; \
	if [ $$s -eq 124 ]; then echo "Execution timelimit $(GATE_TIMEOUT) exceeded" >&2; fi; \
	exit $$s

gate:
	@if [ "$(origin MAKE)" != default ]; then \
	  echo "release gate rejects runtime override: MAKE (origin $(origin MAKE))" >&2; exit 1; fi
	@timeout $(RELEASE_GATE_TIMEOUT) flock --shared "$(WIRE_BASELINE_LOCK)" \
	  $(MAKE) --no-print-directory gate-internal; s=$$?; \
	if [ $$s -eq 124 ]; then echo "Execution timelimit $(RELEASE_GATE_TIMEOUT) exceeded" >&2; fi; \
	exit $$s

all-internal: ultrapatch
	$(CC) $(DECODER_CFLAGS) -Wconversion $(DEC_DEMO_DEFINES) -c $(HOST_BACKEND_SRC) -o /dev/null

.PHONY: host-tool-path release-profile-json profile-check ultrapatch
host-tool-path:
	@printf '%s\n' "$(HOST_TOOL)"

release-profile-json:
	@python3 scripts/build_profile.py release-json

# Validate on every public use, including when an explicit BUILD_DIR points at a manifest created
# by another compiler/config. Identical concurrent checks are safe because ensure-host publishes
# the canonical manifest atomically.
profile-check: scripts/build_profile.py
	@python3 scripts/build_profile.py ensure-host "$(PROFILE_MANIFEST)" >/dev/null

ultrapatch: profile-check $(HOST_TOOL)

$(PROFILE_MANIFEST): scripts/build_profile.py
	@python3 scripts/build_profile.py ensure-host "$@" >/dev/null

check-release-profile-internal: scripts/build_profile.py $(RELEASE_PROFILE_LOCK)
	@python3 scripts/build_profile.py verify-release "$(RELEASE_PROFILE_LOCK)"

check-build-profile-internal: scripts/check_build_profile.sh scripts/build_profile.py
	@MAKE="$(MAKE)" RELEASE_PROFILE_LOCK="$(RELEASE_PROFILE_LOCK)" scripts/check_build_profile.sh

decoder-header-internal: $(DECODER_PUBLIC_HDRS) scripts/gen_single_header.py
	@python3 scripts/gen_single_header.py "$(DECODER_CANONICAL_HDR)" $(DECODER_PUBLIC_HDRS)
	@echo "decoder_header=$(DECODER_CANONICAL_HDR)"

$(DECODER_CANONICAL_HDR): $(DECODER_PUBLIC_HDRS) scripts/gen_single_header.py
	@python3 scripts/gen_single_header.py "$(DECODER_CANONICAL_HDR)" $(DECODER_PUBLIC_HDRS)

WIRE_CONFIG_PROBE_FLAGS := -DCORTEX_M0 -DMAX_IMAGE=1048576u \
                           -DWINDOW_LOG=11 -DJSLOTS=769u -DOPC_CAP=81 \
                           -DOUTROW=128u -DOUTROW_DEPTH=4u -DDR_KCAP_BL=209u -DDR_KCAP_EX=129u

# `make gate` is a release certification, not a configurable measurement. These variables may
# still be overridden on their individual measurement targets and A/B runs; the release gate and
# canonical updater reject any runtime origin so they cannot accidentally certify/publish a
# reduced corpus, relaxed ratchet,
# substituted source/harness, alternate build directory, or disabled contract mode. Repository
# edits remain the intentional, reviewable way to change a release input. The profile verifier
# separately proves the exact compiler versions, flags, and multilib contents.
override RELEASE_GATE_FIXED_VARS := \
	FIXTURES IMAGES FOREIGN CORPUS_MANIFEST FOREIGN_MANIFEST \
	CORPUS_INVENTORY CORPUS_SIZE_BASELINE CORPUS_WIRE_MANIFEST GOLDEN_MANIFEST \
	BASE_RELEASE_FIXTURES BASE_RELEASE_HOME_IMAGES BASE_RELEASE_FOREIGN_IMAGES BASE_RELEASE_GOLDEN_BLOBS \
	BASE_FULL_TOTAL BASE_FOREIGN_TOTAL BASE_ONEFACE_GROW BASE_ONEFACE_REVERT \
	BASE_ARM_TEXT BASE_ARM_DATA BASE_ARM_BSS BASE_ARM_LINKED_TEXT BASE_ARM_LINKED_DATA \
	BASE_ARM_LINKED_BSS BASE_ARM_SOFT_DIV BASE_ARM_EXTERNAL_TEXT BASE_ARM_EXTERNAL_LINKED_TEXT \
	BASE_STACK_STATIC_CEIL_O2 BASE_STACK_GENERIC_CEIL_O2 \
	BASE_STACK_EXTERNAL_STATIC_CEIL_O2 BASE_STACK_EXTERNAL_GENERIC_CEIL_O2 \
	RELEASE_PROFILE_LOCK BUILD_ROOT BUILD_DIR GATE_TIMEOUT WIRE_BASELINE_LOCK \
	CC CLANG ARM_PREFIX ARM_CC ARM_SIZE ARM_OBJDUMP ARM_NM ARM_OBJECT_OPT ARM_STACK_OPT OPT \
	WIRE_CONFIG_FLAGS DECODER_CONFIG_FLAGS CONTRACT_FLAGS CFLAGS DECODER_CFLAGS SINGLE_DECODER_CFLAGS \
	LDFLAGS ARM_COMMON_FLAGS ARM_DEC_FLAGS ARM_SINGLE_DEC_FLAGS \
	DIVSUF CONFIG_HDR APPLY_HDR DECODER_PUBLIC_HDRS DECODER_SINGLE_HDR NVM_EMU ENC_MODULE_SRCS \
	GEN_HDR HOST_BACKEND_SRC ENC_SEAM_SRCS DEC_STANDALONE_SRCS DEC_DEMO_DEFINES TOOL_SRCS \
	HOST_BACKEND_DEFINES HOST_BUILD_RECIPE_TAG \
	PORTABLE_FALLBACK_FLAGS WIRE_CONFIG_PROBE_FLAGS ARM_LINK_STUBS ARM_LINK_LAYOUT \
	ARM_APPLY_HARNESS STACK_GENERIC_HARNESS ARM_SINGLE_APPLY_HARNESS \
	STACK_SINGLE_GENERIC_HARNESS ARM_EXTERNAL_IMPL_HARNESS ARM_EXTERNAL_APPLY_HARNESS \
	STACK_EXTERNAL_GENERIC_HARNESS ARM_SINGLE_EXTERNAL_IMPL_HARNESS \
	ARM_SINGLE_EXTERNAL_APPLY_HARNESS STACK_SINGLE_EXTERNAL_GENERIC_HARNESS
override RELEASE_GATE_UNSET_VARS := \
	CROSS_COMPILE CFLAGS_EXTRA CORPUS_SIZE_DUMP CORPUS_WIRE_DUMP \
	DECODER_API_REGULAR DECODER_API_SANITIZE CRASH_DISPATCH_MODE CRASH_DISPATCH_MARKER \
	REAL_ULTRAPATCH ONEFACE_ROUNDTRIP ONEFACE_WIRE_HASHES

.PHONY: release-gate-origin-probe-internal release-gate-inputs-internal
release-gate-origin-probe-internal:
	@set -eu; bad=0; \
	$(foreach v,$(RELEASE_GATE_FIXED_VARS),if [ "$(origin $(v))" != file ]; then echo "release gate rejects runtime override: $(v) (origin $(origin $(v)))" >&2; bad=1; fi; ) \
	$(foreach v,$(RELEASE_GATE_UNSET_VARS),if [ "$(origin $(v))" != undefined ]; then echo "release gate requires unset mode: $(v) (origin $(origin $(v)))" >&2; bad=1; fi; ) \
	test "$$bad" -eq 0

release-gate-inputs-internal: scripts/publish_wire_baselines.py
	@$(MAKE) --no-print-directory release-gate-origin-probe-internal
	@python3 scripts/publish_wire_baselines.py assert-clean --root test-bench
	@python3 scripts/build_profile.py verify-release "$(RELEASE_PROFILE_LOCK)" >/dev/null
	@echo "release_gate_inputs=OK (canonical repository inputs + pinned release profile)"

.PHONY: golden-update-origin-probe-internal golden-update-inputs-internal \
        golden-update-validate-canonical-internal
golden-update-origin-probe-internal:
	@set -eu; bad=0; \
	$(foreach v,$(RELEASE_GATE_FIXED_VARS),if [ "$(origin $(v))" != file ]; then echo "golden update rejects runtime override: $(v) (origin $(origin $(v)))" >&2; bad=1; fi; ) \
	$(foreach v,$(RELEASE_GATE_UNSET_VARS),if [ "$(origin $(v))" != undefined ]; then echo "golden update requires unset mode: $(v) (origin $(origin $(v)))" >&2; bad=1; fi; ) \
	test "$$bad" -eq 0

golden-update-validate-canonical-internal:
	@$(MAKE) --no-print-directory check-release-inventory-internal
	@$(MAKE) --no-print-directory check-assets-internal
	@echo "golden_update_canonical_inputs=OK (inventory + corpus/foreign asset hashes)"

golden-update-inputs-internal:
	@$(MAKE) --no-print-directory golden-update-origin-probe-internal
	@python3 scripts/build_profile.py verify-release "$(RELEASE_PROFILE_LOCK)" >/dev/null
	@flock --shared "$(WIRE_BASELINE_LOCK)" \
	  $(MAKE) --no-print-directory golden-update-validate-canonical-internal >/dev/null
	@echo "golden_update_inputs=OK (canonical repository inputs + pinned release profile)"

check-release-gate-contract-internal: scripts/check_release_gate_contract.sh scripts/run_gate.sh
	@MAKE="$(MAKE)" scripts/check_release_gate_contract.sh

check-release-inventory-internal: scripts/check_release_inventory.py $(CORPUS_INVENTORY) \
                                  $(CORPUS_MANIFEST) $(FOREIGN_MANIFEST) \
                                  $(CORPUS_SIZE_BASELINE) $(CORPUS_WIRE_MANIFEST) $(GOLDEN_MANIFEST)
	@python3 scripts/check_release_inventory.py \
	  --inventory "$(CORPUS_INVENTORY)" \
	  --corpus-assets "$(CORPUS_MANIFEST)" --foreign-assets "$(FOREIGN_MANIFEST)" \
	  --home-sizes "$(CORPUS_SIZE_BASELINE)" --wire "$(CORPUS_WIRE_MANIFEST)" \
	  --golden "$(GOLDEN_MANIFEST)" --home-total "$(BASE_FULL_TOTAL)" \
	  --oneface-grow "$(BASE_ONEFACE_GROW)" --oneface-revert "$(BASE_ONEFACE_REVERT)" \
	  --fixtures "$(BASE_RELEASE_FIXTURES)" --home-images "$(BASE_RELEASE_HOME_IMAGES)" \
	  --foreign-images "$(BASE_RELEASE_FOREIGN_IMAGES)" \
	  --golden-blobs "$(BASE_RELEASE_GOLDEN_BLOBS)"

check-pack-corpus-internal: scripts/pack_corpus.sh scripts/check_pack_corpus.sh \
                            scripts/check_release_inventory.py $(CORPUS_INVENTORY)
	@scripts/check_pack_corpus.sh

.PHONY: check-wire-config-probe-internal
check-wire-config-internal: ultrapatch
	@$(MAKE) --no-print-directory WIRE_CONFIG_FLAGS='$(WIRE_CONFIG_PROBE_FLAGS)' \
		DEFAULT_ULTRAPATCH='$(HOST_TOOL)' \
		check-wire-config-probe-internal

check-wire-config-probe-internal: ultrapatch $(DECODER_CANONICAL_HDR) scripts/check_wire_config.sh \
                                scripts/synth_gen.py test-bench/decoder-contract.c \
                                $(DEC_STANDALONE_SRCS) $(NVM_EMU) $(ARM_LINK_STUBS) $(ARM_LINK_LAYOUT)
	@CC="$(CC)" CFLAGS="$(CFLAGS)" DECODER_CFLAGS="$(DECODER_CFLAGS)" \
	  SINGLE_DECODER_CFLAGS="$(SINGLE_DECODER_CFLAGS)" \
	  DECODER_SINGLE_HDR="$(DECODER_CANONICAL_HDR)" \
	  DEC_STANDALONE_SRCS="$(DEC_STANDALONE_SRCS)" DEC_DEMO_DEFINES="$(DEC_DEMO_DEFINES)" \
	  ARM_CC="$(ARM_CC)" ARM_SIZE="$(ARM_SIZE)" ARM_OBJDUMP="$(ARM_OBJDUMP)" \
	  ARM_DEC_FLAGS="$(ARM_DEC_FLAGS)" ARM_SINGLE_DEC_FLAGS="$(ARM_SINGLE_DEC_FLAGS)" \
	  ARM_OBJECT_OPT="$(ARM_OBJECT_OPT)" ARM_BSS_HARD_CAP="$(ARM_BSS_HARD_CAP)" \
	  ARM_LINK_STUBS="$(ARM_LINK_STUBS)" ARM_LINK_LAYOUT="$(ARM_LINK_LAYOUT)" \
	  DEFAULT_ULTRAPATCH="$(DEFAULT_ULTRAPATCH)" ULTRAPATCH="$(HOST_TOOL)" \
	  FIXTURES="$(FIXTURES)" scripts/check_wire_config.sh

.PHONY: FORCE
FORCE:

$(HOST_BUILD_RECIPE): FORCE Makefile scripts/write_build_recipe.py $(PROFILE_MANIFEST) | profile-check
	@python3 scripts/write_build_recipe.py "$@"

$(HOST_OBJ_DIR)/src/patch_host_backend.o: $(HOST_BACKEND_SRC) Makefile \
                                     $(HOST_BUILD_RECIPE) $(PROFILE_MANIFEST) | profile-check
	@mkdir -p "$(dir $@)"
	@set -e; obj="$@.$$$$.o.tmp"; dep="$@.$$$$.d.tmp"; \
	cleanup(){ rm -f "$$obj" "$$dep"; }; trap 'cleanup' EXIT; \
	trap 'cleanup; trap - TERM INT EXIT; kill -s TERM "$$$$"' TERM; \
	trap 'cleanup; trap - TERM INT EXIT; kill -s INT "$$$$"' INT; \
	$(CC) $(DECODER_CFLAGS) $(HOST_BACKEND_DEFINES) -MMD -MP -MF "$$dep" -MT "$@" \
		-c "$<" -o "$$obj"; \
	mv -f "$$dep" "$(@:.o=.d)"; mv -f "$$obj" "$@"; trap - EXIT TERM INT

$(HOST_OBJ_DIR)/%.o: %.c Makefile $(HOST_BUILD_RECIPE) $(PROFILE_MANIFEST) | profile-check
	@mkdir -p "$(dir $@)"
	@set -e; obj="$@.$$$$.o.tmp"; dep="$@.$$$$.d.tmp"; \
	cleanup(){ rm -f "$$obj" "$$dep"; }; trap 'cleanup' EXIT; \
	trap 'cleanup; trap - TERM INT EXIT; kill -s TERM "$$$$"' TERM; \
	trap 'cleanup; trap - TERM INT EXIT; kill -s INT "$$$$"' INT; \
	$(CC) $(CFLAGS) -MMD -MP -MF "$$dep" -MT "$@" \
		-c "$<" -o "$$obj"; \
	mv -f "$$dep" "$(@:.o=.d)"; mv -f "$$obj" "$@"; trap - EXIT TERM INT

-include $(HOST_TOOL_DEPFILES)

$(HOST_TOOL): $(HOST_TOOL_OBJECTS) Makefile $(HOST_BUILD_RECIPE) $(PROFILE_MANIFEST) | profile-check
	@python3 scripts/build_profile.py ensure-host "$(PROFILE_MANIFEST)" >/dev/null
	@mkdir -p "$(dir $(HOST_TOOL))"
	@set -e; tmp="$@.$$$$.tmp"; cleanup(){ rm -f "$$tmp"; }; trap 'cleanup' EXIT; \
	trap 'cleanup; trap - TERM INT EXIT; kill -s TERM "$$$$"' TERM; \
	trap 'cleanup; trap - TERM INT EXIT; kill -s INT "$$$$"' INT; \
	$(CC) $(CFLAGS) $(HOST_TOOL_OBJECTS) $(LDFLAGS) -o "$$tmp"; \
	mv -f "$$tmp" "$@"; trap - EXIT TERM INT
	@echo "host_tool=$(HOST_TOOL)"

check-internal: ultrapatch
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
check-arm-measure-internal: $(DECODER_CANONICAL_HDR) $(ARM_LINK_STUBS) $(ARM_LINK_LAYOUT)
	@set -e; \
	. ./scripts/tempdir.sh; \
	$(ARM_APPLY_HARNESS); \
	$(ARM_SINGLE_APPLY_HARNESS); \
	$(ARM_EXTERNAL_IMPL_HARNESS); \
	$(ARM_EXTERNAL_APPLY_HARNESS); \
	$(ARM_SINGLE_EXTERNAL_IMPL_HARNESS); \
	$(ARM_SINGLE_EXTERNAL_APPLY_HARNESS); \
	$(ARM_CC) $(ARM_DEC_FLAGS) $(ARM_OBJECT_OPT) -c "$$tmp/patch_apply_arm.c" -o "$$tmp/patch_apply_arm.o"; \
	$(ARM_CC) $(ARM_SINGLE_DEC_FLAGS) $(ARM_OBJECT_OPT) -c "$$tmp/patch_apply_arm_single.c" -o "$$tmp/patch_apply_arm_single.o"; \
	$(ARM_CC) $(ARM_DEC_FLAGS) $(ARM_OBJECT_OPT) -c "$$tmp/patch_apply_external_impl.c" -o "$$tmp/patch_apply_external_impl.o"; \
	$(ARM_CC) $(ARM_DEC_FLAGS) $(ARM_OBJECT_OPT) -c "$$tmp/patch_apply_external_arm.c" -o "$$tmp/patch_apply_external_arm.o"; \
	$(ARM_CC) $(ARM_SINGLE_DEC_FLAGS) $(ARM_OBJECT_OPT) -c "$$tmp/patch_apply_external_impl_single.c" -o "$$tmp/patch_apply_external_impl_single.o"; \
	$(ARM_CC) $(ARM_SINGLE_DEC_FLAGS) $(ARM_OBJECT_OPT) -c "$$tmp/patch_apply_external_arm_single.c" -o "$$tmp/patch_apply_external_arm_single.o"; \
	size_out=$$($(ARM_SIZE) "$$tmp/patch_apply_arm.o"); \
	single_size_out=$$($(ARM_SIZE) "$$tmp/patch_apply_arm_single.o"); \
	external_size_out=$$($(ARM_SIZE) "$$tmp/patch_apply_external_impl.o" "$$tmp/patch_apply_external_arm.o"); \
	external_single_size_out=$$($(ARM_SIZE) "$$tmp/patch_apply_external_impl_single.o" "$$tmp/patch_apply_external_arm_single.o"); \
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
	set -- $$(printf '%s\n' "$$single_size_out" | awk 'NR==2 { print $$1, $$2, $$3 }'); \
	test "$$obj_text/$$obj_data/$$obj_bss" = "$$1/$$2/$$3" || { \
		echo "ARM source/single object sizes differ: $$obj_text/$$obj_data/$$obj_bss != $$1/$$2/$$3" >&2; exit 1; }; \
	set -- $$(printf '%s\n' "$$external_size_out" | awk 'NR>1 { t+=$$1; d+=$$2; b+=$$3 } END { print t, d, b }'); \
	external_obj_text=$$1; external_obj_data=$$2; external_obj_bss=$$3; \
	set -- $$(printf '%s\n' "$$external_single_size_out" | awk 'NR>1 { t+=$$1; d+=$$2; b+=$$3 } END { print t, d, b }'); \
	test "$$external_obj_text/$$external_obj_data/$$external_obj_bss" = "$$1/$$2/$$3" || { \
		echo "ARM external source/single object totals differ: $$external_obj_text/$$external_obj_data/$$external_obj_bss != $$1/$$2/$$3" >&2; exit 1; }; \
	echo "arm_external_size_integration=one implementation object + declarations-only static PatchApply wrapper"; \
	echo "arm_external_object_text=$$external_obj_text"; \
	echo "arm_external_object_data=$$external_obj_data"; \
	echo "arm_external_object_bss=$$external_obj_bss"; \
	echo "arm_external_object_text_delta=$$((external_obj_text-obj_text))"; \
	echo "arm_external_object_bss_delta=$$((external_obj_bss-obj_bss))"; \
	test "$$external_obj_data/$$external_obj_bss" = "$$obj_data/$$obj_bss" || { \
		echo "ARM external object changed state footprint: $$external_obj_data/$$external_obj_bss != $$obj_data/$$obj_bss" >&2; exit 1; }; \
	test "$$external_obj_text" -le "$(BASE_ARM_EXTERNAL_TEXT)" || { \
		echo "ARM external object text ceiling exceeded: $$external_obj_text > $(BASE_ARM_EXTERNAL_TEXT)" >&2; exit 1; }; \
	if [ "$$external_obj_bss" -gt "$(ARM_BSS_HARD_CAP)" ]; then \
		echo "ARM external .bss hard cap exceeded: $$external_obj_bss > $(ARM_BSS_HARD_CAP)" >&2; exit 1; \
	fi; \
	impl_defs=$$($(ARM_NM) -g --defined-only "$$tmp/patch_apply_external_impl.o" | awk '$$3=="patch_apply_run"{n++} END{print n+0}'); \
	caller_defs=$$($(ARM_NM) -g --defined-only "$$tmp/patch_apply_external_arm.o" | awk '$$3=="patch_apply_run"{n++} END{print n+0}'); \
	test "$$impl_defs/$$caller_defs" = "1/0" || { echo "ARM external linkage does not contain exactly one implementation" >&2; exit 1; }; \
	for form in source single; do \
		if [ "$$form" = source ]; then obj="$$tmp/patch_apply_arm.o"; ext_impl="$$tmp/patch_apply_external_impl.o"; ext_call="$$tmp/patch_apply_external_arm.o"; \
		else obj="$$tmp/patch_apply_arm_single.o"; ext_impl="$$tmp/patch_apply_external_impl_single.o"; ext_call="$$tmp/patch_apply_external_arm_single.o"; fi; \
		$(ARM_OBJDUMP) -d "$$obj" > "$$tmp/patch_apply_arm_$$form.dump"; \
		$(ARM_OBJDUMP) -d "$$ext_impl" > "$$tmp/patch_apply_external_impl_$$form.dump"; \
		$(ARM_OBJDUMP) -d "$$ext_call" > "$$tmp/patch_apply_external_call_$$form.dump"; \
		if grep -Eq '\b(udiv|sdiv)\b' "$$tmp/patch_apply_arm_$$form.dump" \
		   "$$tmp/patch_apply_external_impl_$$form.dump" "$$tmp/patch_apply_external_call_$$form.dump"; then \
			echo "hardware divide instruction found in $$form decoder" >&2; exit 1; \
		fi; \
	done; \
	soft=$$(grep -Ec '__aeabi_.*div|__aeabi_.*mod' "$$tmp/patch_apply_arm_source.dump" || true); \
	single_soft=$$(grep -Ec '__aeabi_.*div|__aeabi_.*mod' "$$tmp/patch_apply_arm_single.dump" || true); \
	test "$$soft" -eq "$$single_soft" || { echo "ARM source/single divide references differ" >&2; exit 1; }; \
	external_soft=$$(grep -Ehc '__aeabi_.*div|__aeabi_.*mod' "$$tmp/patch_apply_external_impl_source.dump" "$$tmp/patch_apply_external_call_source.dump" | awk '{n+=$$1} END{print n+0}'); \
	external_single_soft=$$(grep -Ehc '__aeabi_.*div|__aeabi_.*mod' "$$tmp/patch_apply_external_impl_single.dump" "$$tmp/patch_apply_external_call_single.dump" | awk '{n+=$$1} END{print n+0}'); \
	test "$$external_soft" -eq "$$external_single_soft" || { echo "ARM external source/single divide references differ" >&2; exit 1; }; \
	test "$$external_soft" -eq "$(BASE_ARM_SOFT_DIV)" || { echo "ARM external soft-divide policy failed" >&2; exit 1; }; \
	echo "soft_div_calls=$$soft"; \
	test "$$soft" -eq "$(BASE_ARM_SOFT_DIV)"; \
	$(ARM_CC) $(ARM_DEC_FLAGS) $(ARM_OBJECT_OPT) -c "$(ARM_LINK_STUBS)" -o "$$tmp/arm_link_stubs.o"; \
	$(ARM_CC) -mcpu=cortex-m0plus -mthumb -nostdlib \
		-Wl,--gc-sections,--orphan-handling=error,-T,"$(ARM_LINK_LAYOUT)",-Map,"$$tmp/patch_apply_arm.map" \
		"$$tmp/patch_apply_arm.o" "$$tmp/arm_link_stubs.o" -lc -lgcc -o "$$tmp/patch_apply_arm.elf"; \
	$(ARM_CC) -mcpu=cortex-m0plus -mthumb -nostdlib \
		-Wl,--gc-sections,--orphan-handling=error,-T,"$(ARM_LINK_LAYOUT)",-Map,"$$tmp/patch_apply_arm_single.map" \
		"$$tmp/patch_apply_arm_single.o" "$$tmp/arm_link_stubs.o" -lc -lgcc -o "$$tmp/patch_apply_arm_single.elf"; \
	$(ARM_CC) -mcpu=cortex-m0plus -mthumb -nostdlib \
		-Wl,--gc-sections,--orphan-handling=error,-T,"$(ARM_LINK_LAYOUT)",-Map,"$$tmp/patch_apply_external.map" \
		"$$tmp/patch_apply_external_impl.o" "$$tmp/patch_apply_external_arm.o" "$$tmp/arm_link_stubs.o" \
		-lc -lgcc -o "$$tmp/patch_apply_external.elf"; \
	$(ARM_CC) -mcpu=cortex-m0plus -mthumb -nostdlib \
		-Wl,--gc-sections,--orphan-handling=error,-T,"$(ARM_LINK_LAYOUT)",-Map,"$$tmp/patch_apply_external_single.map" \
		"$$tmp/patch_apply_external_impl_single.o" "$$tmp/patch_apply_external_arm_single.o" "$$tmp/arm_link_stubs.o" \
		-lc -lgcc -o "$$tmp/patch_apply_external_single.elf"; \
	linked_size_out=$$($(ARM_SIZE) "$$tmp/patch_apply_arm.elf"); \
	single_linked_size_out=$$($(ARM_SIZE) "$$tmp/patch_apply_arm_single.elf"); \
	external_linked_size_out=$$($(ARM_SIZE) "$$tmp/patch_apply_external.elf"); \
	external_single_linked_size_out=$$($(ARM_SIZE) "$$tmp/patch_apply_external_single.elf"); \
	printf '%s\n' "$$linked_size_out"; \
	echo "arm_linked_integration=no-startup static PatchApply wrapper + minimal flash stubs"; \
	set -- $$(printf '%s\n' "$$linked_size_out" | awk 'NR==2 { print $$1, $$2, $$3 }'); \
	linked_text=$$1; linked_data=$$2; linked_bss=$$3; \
	set -- $$(printf '%s\n' "$$single_linked_size_out" | awk 'NR==2 { print $$1, $$2, $$3 }'); \
	test "$$linked_text/$$linked_data/$$linked_bss" = "$$1/$$2/$$3" || { \
		echo "ARM source/single linked sizes differ: $$linked_text/$$linked_data/$$linked_bss != $$1/$$2/$$3" >&2; exit 1; }; \
	echo "arm_linked_text=$$linked_text"; \
	echo "arm_linked_data=$$linked_data"; \
	echo "arm_linked_bss=$$linked_bss"; \
	if [ "$$linked_bss" -gt "$(ARM_BSS_HARD_CAP)" ]; then \
		echo "ARM linked .bss hard cap exceeded: $$linked_bss > $(ARM_BSS_HARD_CAP)" >&2; exit 1; \
	fi; \
	test "$$linked_text" -le "$(BASE_ARM_LINKED_TEXT)"; \
	test "$$linked_data" -le "$(BASE_ARM_LINKED_DATA)"; \
	test "$$linked_bss" -le "$(BASE_ARM_LINKED_BSS)"; \
	set -- $$(printf '%s\n' "$$external_linked_size_out" | awk 'NR==2 { print $$1, $$2, $$3 }'); \
	external_linked_text=$$1; external_linked_data=$$2; external_linked_bss=$$3; \
	set -- $$(printf '%s\n' "$$external_single_linked_size_out" | awk 'NR==2 { print $$1, $$2, $$3 }'); \
	test "$$external_linked_text/$$external_linked_data/$$external_linked_bss" = "$$1/$$2/$$3" || { \
		echo "ARM external source/single linked sizes differ: $$external_linked_text/$$external_linked_data/$$external_linked_bss != $$1/$$2/$$3" >&2; exit 1; }; \
	echo "arm_external_linked_integration=no-startup one implementation + declarations-only static wrapper + minimal flash stubs"; \
	echo "arm_external_linked_text=$$external_linked_text"; \
	echo "arm_external_linked_data=$$external_linked_data"; \
	echo "arm_external_linked_bss=$$external_linked_bss"; \
	echo "arm_external_linked_text_delta=$$((external_linked_text-linked_text))"; \
	echo "arm_external_linked_bss_delta=$$((external_linked_bss-linked_bss))"; \
	test "$$external_linked_data/$$external_linked_bss" = "$$linked_data/$$linked_bss" || { \
		echo "ARM external link changed state footprint: $$external_linked_data/$$external_linked_bss != $$linked_data/$$linked_bss" >&2; exit 1; }; \
	test "$$external_linked_text" -le "$(BASE_ARM_EXTERNAL_LINKED_TEXT)" || { \
		echo "ARM external linked text ceiling exceeded: $$external_linked_text > $(BASE_ARM_EXTERNAL_LINKED_TEXT)" >&2; exit 1; }; \
	if [ "$$external_linked_bss" -gt "$(ARM_BSS_HARD_CAP)" ]; then \
		echo "ARM external linked .bss hard cap exceeded: $$external_linked_bss > $(ARM_BSS_HARD_CAP)" >&2; exit 1; \
	fi; \
	helpers=$$($(ARM_NM) --defined-only "$$tmp/patch_apply_arm.elf" | \
		awk '$$3 == "memcpy" || $$3 == "memmove" || $$3 == "memset" { print $$3 }' | \
		sort | paste -sd, -); \
	single_helpers=$$($(ARM_NM) --defined-only "$$tmp/patch_apply_arm_single.elf" | \
		awk '$$3 == "memcpy" || $$3 == "memmove" || $$3 == "memset" { print $$3 }' | \
		sort | paste -sd, -); \
	test "$$helpers" = "$$single_helpers" || { echo "ARM source/single runtime helpers differ" >&2; exit 1; }; \
	external_helpers=$$($(ARM_NM) --defined-only "$$tmp/patch_apply_external.elf" | \
		awk '$$3 == "memcpy" || $$3 == "memmove" || $$3 == "memset" { print $$3 }' | \
		sort | paste -sd, -); \
	external_single_helpers=$$($(ARM_NM) --defined-only "$$tmp/patch_apply_external_single.elf" | \
		awk '$$3 == "memcpy" || $$3 == "memmove" || $$3 == "memset" { print $$3 }' | \
		sort | paste -sd, -); \
	test "$$external_helpers" = "$$external_single_helpers" || { echo "ARM external source/single runtime helpers differ" >&2; exit 1; }; \
	test "$$external_helpers" = "$$helpers" || { echo "ARM external/default runtime helper policies differ" >&2; exit 1; }; \
	echo "arm_linked_runtime_helpers=$$helpers"; \
	echo "arm_package_parity=OK (source + canonical single header; object + linked)"; \
	echo "arm_external_linkage=OK (exactly one implementation; source + canonical single; compared with one static decoder)"

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
check-stack-internal: check-release-profile-internal $(DECODER_CANONICAL_HDR)
	@set -e; \
	. ./scripts/tempdir.sh; \
	$(ARM_APPLY_HARNESS); \
	$(STACK_GENERIC_HARNESS); \
	$(ARM_SINGLE_APPLY_HARNESS); \
	$(STACK_SINGLE_GENERIC_HARNESS); \
	$(ARM_EXTERNAL_IMPL_HARNESS); \
	$(ARM_EXTERNAL_APPLY_HARNESS); \
	$(STACK_EXTERNAL_GENERIC_HARNESS); \
	$(ARM_SINGLE_EXTERNAL_IMPL_HARNESS); \
	$(ARM_SINGLE_EXTERNAL_APPLY_HARNESS); \
	$(STACK_SINGLE_EXTERNAL_GENERIC_HARNESS); \
	$(ARM_CC) $(ARM_DEC_FLAGS) $(ARM_STACK_OPT) -c "$$tmp/patch_apply_arm.c" -o "$$tmp/patch_apply_arm.o" -fstack-usage; \
	$(ARM_CC) $(ARM_DEC_FLAGS) $(ARM_STACK_OPT) -c "$$tmp/patch_apply_stack_generic.c" -o "$$tmp/patch_apply_stack_generic.o" -fstack-usage; \
	$(ARM_CC) $(ARM_SINGLE_DEC_FLAGS) $(ARM_STACK_OPT) -c "$$tmp/patch_apply_arm_single.c" -o "$$tmp/patch_apply_arm_single.o" -fstack-usage; \
	$(ARM_CC) $(ARM_SINGLE_DEC_FLAGS) $(ARM_STACK_OPT) -c "$$tmp/patch_apply_stack_generic_single.c" -o "$$tmp/patch_apply_stack_generic_single.o" -fstack-usage; \
	$(ARM_CC) $(ARM_DEC_FLAGS) $(ARM_STACK_OPT) -c "$$tmp/patch_apply_external_impl.c" -o "$$tmp/patch_apply_external_impl_stack.o" -fstack-usage; \
	$(ARM_CC) $(ARM_DEC_FLAGS) $(ARM_STACK_OPT) -c "$$tmp/patch_apply_external_arm.c" -o "$$tmp/patch_apply_external_arm_stack.o" -fstack-usage; \
	$(ARM_CC) $(ARM_DEC_FLAGS) $(ARM_STACK_OPT) -c "$$tmp/patch_apply_external_stack_generic.c" -o "$$tmp/patch_apply_external_generic_stack.o" -fstack-usage; \
	$(ARM_CC) $(ARM_SINGLE_DEC_FLAGS) $(ARM_STACK_OPT) -c "$$tmp/patch_apply_external_impl_single.c" -o "$$tmp/patch_apply_external_impl_single_stack.o" -fstack-usage; \
	$(ARM_CC) $(ARM_SINGLE_DEC_FLAGS) $(ARM_STACK_OPT) -c "$$tmp/patch_apply_external_arm_single.c" -o "$$tmp/patch_apply_external_arm_single_stack.o" -fstack-usage; \
	$(ARM_CC) $(ARM_SINGLE_DEC_FLAGS) $(ARM_STACK_OPT) -c "$$tmp/patch_apply_external_stack_generic_single.c" -o "$$tmp/patch_apply_external_generic_single_stack.o" -fstack-usage; \
	$(ARM_CC) -nostdlib -r "$$tmp/patch_apply_external_impl_stack.o" "$$tmp/patch_apply_external_arm_stack.o" -o "$$tmp/patch_apply_external_static_combined.o"; \
	$(ARM_CC) -nostdlib -r "$$tmp/patch_apply_external_impl_stack.o" "$$tmp/patch_apply_external_generic_stack.o" -o "$$tmp/patch_apply_external_generic_combined.o"; \
	$(ARM_CC) -nostdlib -r "$$tmp/patch_apply_external_impl_single_stack.o" "$$tmp/patch_apply_external_arm_single_stack.o" -o "$$tmp/patch_apply_external_static_single_combined.o"; \
	$(ARM_CC) -nostdlib -r "$$tmp/patch_apply_external_impl_single_stack.o" "$$tmp/patch_apply_external_generic_single_stack.o" -o "$$tmp/patch_apply_external_generic_single_combined.o"; \
	cat "$$tmp/patch_apply_external_impl_stack.su" "$$tmp/patch_apply_external_arm_stack.su" > "$$tmp/patch_apply_external_static_combined.su"; \
	cat "$$tmp/patch_apply_external_impl_stack.su" "$$tmp/patch_apply_external_generic_stack.su" > "$$tmp/patch_apply_external_generic_combined.su"; \
	cat "$$tmp/patch_apply_external_impl_single_stack.su" "$$tmp/patch_apply_external_arm_single_stack.su" > "$$tmp/patch_apply_external_static_single_combined.su"; \
	cat "$$tmp/patch_apply_external_impl_single_stack.su" "$$tmp/patch_apply_external_generic_single_stack.su" > "$$tmp/patch_apply_external_generic_single_combined.su"; \
	OBJDUMP="$(ARM_OBJDUMP)" python3 scripts/stack_bound.py "$$tmp/patch_apply_arm.o" > "$$tmp/stack_static.txt"; \
	OBJDUMP="$(ARM_OBJDUMP)" python3 scripts/stack_bound.py "$$tmp/patch_apply_stack_generic.o" > "$$tmp/stack_generic.txt"; \
	OBJDUMP="$(ARM_OBJDUMP)" python3 scripts/stack_bound.py "$$tmp/patch_apply_arm_single.o" > "$$tmp/stack_static_single.txt"; \
	OBJDUMP="$(ARM_OBJDUMP)" python3 scripts/stack_bound.py "$$tmp/patch_apply_stack_generic_single.o" > "$$tmp/stack_generic_single.txt"; \
	OBJDUMP="$(ARM_OBJDUMP)" python3 scripts/stack_bound.py "$$tmp/patch_apply_external_static_combined.o" > "$$tmp/stack_external_static.txt"; \
	OBJDUMP="$(ARM_OBJDUMP)" python3 scripts/stack_bound.py "$$tmp/patch_apply_external_generic_combined.o" > "$$tmp/stack_external_generic.txt"; \
	OBJDUMP="$(ARM_OBJDUMP)" python3 scripts/stack_bound.py "$$tmp/patch_apply_external_static_single_combined.o" > "$$tmp/stack_external_static_single.txt"; \
	OBJDUMP="$(ARM_OBJDUMP)" python3 scripts/stack_bound.py "$$tmp/patch_apply_external_generic_single_combined.o" > "$$tmp/stack_external_generic_single.txt"; \
	cmp "$$tmp/stack_static.txt" "$$tmp/stack_static_single.txt" || { \
		echo "ARM source/single static-wrapper stack results differ" >&2; exit 1; }; \
	cmp "$$tmp/stack_generic.txt" "$$tmp/stack_generic_single.txt" || { \
		echo "ARM source/single generic-wrapper stack results differ" >&2; exit 1; }; \
	cmp "$$tmp/stack_external_static.txt" "$$tmp/stack_external_static_single.txt" || { \
		echo "ARM external source/single static-wrapper stack results differ" >&2; exit 1; }; \
	cmp "$$tmp/stack_external_generic.txt" "$$tmp/stack_external_generic_single.txt" || { \
		echo "ARM external source/single generic-wrapper stack results differ" >&2; exit 1; }; \
	echo "stack_static_integration=static PatchApply wrapper"; \
	sed 's/^stack_/stack_static_/' "$$tmp/stack_static.txt"; \
	echo "stack_static_ceiling_o2=$(BASE_STACK_STATIC_CEIL_O2)"; \
	echo "stack_generic_integration=caller-owned PatchApply * wrapper"; \
	sed 's/^stack_/stack_generic_/' "$$tmp/stack_generic.txt"; \
	echo "stack_generic_ceiling_o2=$(BASE_STACK_GENERIC_CEIL_O2)"; \
	static_bound=$$(sed -n 's/^stack_bound_bytes=//p' "$$tmp/stack_static.txt"); \
	generic_bound=$$(sed -n 's/^stack_bound_bytes=//p' "$$tmp/stack_generic.txt"); \
	external_static_bound=$$(sed -n 's/^stack_bound_bytes=//p' "$$tmp/stack_external_static.txt"); \
	external_generic_bound=$$(sed -n 's/^stack_bound_bytes=//p' "$$tmp/stack_external_generic.txt"); \
	test -n "$$static_bound"; \
	test -n "$$generic_bound"; \
	test -n "$$external_static_bound"; \
	test -n "$$external_generic_bound"; \
	if [ "$$static_bound" -gt "$(BASE_STACK_STATIC_CEIL_O2)" ]; then \
		echo "Static PatchApply wrapper stack ceiling exceeded: $$static_bound > $(BASE_STACK_STATIC_CEIL_O2)" >&2; exit 1; \
	fi; \
	if [ "$$generic_bound" -gt "$(BASE_STACK_GENERIC_CEIL_O2)" ]; then \
		echo "Caller-owned PatchApply * wrapper stack ceiling exceeded: $$generic_bound > $(BASE_STACK_GENERIC_CEIL_O2)" >&2; exit 1; \
	fi; \
	if [ "$$external_static_bound" -gt "$(BASE_STACK_EXTERNAL_STATIC_CEIL_O2)" ]; then \
		echo "External static PatchApply wrapper stack ceiling exceeded: $$external_static_bound > $(BASE_STACK_EXTERNAL_STATIC_CEIL_O2)" >&2; exit 1; \
	fi; \
	if [ "$$external_generic_bound" -gt "$(BASE_STACK_EXTERNAL_GENERIC_CEIL_O2)" ]; then \
		echo "External caller-owned PatchApply pointer stack ceiling exceeded: $$external_generic_bound > $(BASE_STACK_EXTERNAL_GENERIC_CEIL_O2)" >&2; exit 1; \
	fi; \
	echo "stack_external_static_integration=one implementation + declarations-only static PatchApply wrapper"; \
	echo "stack_external_static_bound_bytes=$$external_static_bound"; \
	echo "stack_external_static_delta_bytes=$$((external_static_bound-static_bound))"; \
	echo "stack_external_generic_integration=one implementation + declarations-only caller-owned PatchApply pointer"; \
	echo "stack_external_generic_bound_bytes=$$external_generic_bound"; \
	echo "stack_external_generic_delta_bytes=$$((external_generic_bound-generic_bound))"; \
	echo "stack_package_parity=OK (source + canonical single header; static + generic)"; \
	echo "stack_external_linkage=OK (source + canonical single; compared with one static decoder)"

# Portability contract: the decoder is standard C (C99 + C11 _Static_assert); GNU
# attributes/builtins are optional codegen hints behind guards with live fallbacks (rc_models.h
# note). -DNO_GNU_EXTENSIONS is the documented knob that forces the fallback branch — a
# first-party switch, so system headers are untouched (no fragile __GNUC__ masking against
# glibc). Wire correctness is compiler-independent; only the gated size/stack budgets are
# GNU-toolchain-measured. Enforced below three ways: (a) both header forms smoke-compile with
# the fallbacks forced, (b) the preprocessed fallback smoke TU must contain ZERO
# __attribute__/__builtin_ tokens in first-party (src/) lines — this catches a future bare
# GNU-ism that a gcc compile alone would accept (__builtin_offsetof is exempt: it is the
# SYSTEM stddef.h's conforming implementation of the standard offsetof macro our asserts use;
# a non-GNU toolchain's stddef.h supplies its own), (c) a fallback-built host decoder must
# round-trip the real one-face patch byte-exactly.
PORTABLE_FALLBACK_FLAGS := -DNO_GNU_EXTENSIONS
check-decoder-contract-internal: ultrapatch $(DECODER_CANONICAL_HDR)
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
	single="$(DECODER_CANONICAL_HDR)"; \
	if grep -nE '^[[:space:]]*#include[[:space:]]*"' "$$single" | grep -E '"(patch_config|rc_models|patch_apply)\.h"'; then \
		echo "generated single decoder header must not include local decoder support headers" >&2; exit 1; \
	fi; \
	for form in source single; do \
		if [ "$$form" = source ]; then inc=patch_apply.h; stem=patch_apply; else inc="$$single"; stem=patch_apply_single; fi; \
		csrc="$$tmp/$${stem}_smoke.c"; obj="$$tmp/$${stem}_smoke.o"; \
		{ printf '%s\n' '#include <stdint.h>'; \
		  printf '%s\n' "#include \"$$inc\""; \
		  printf '%s\n' 'uint8_t flash_read(uint32_t a){ (void)a; return 0xffu; }'; \
		  printf '%s\n' 'void flash_write_page(uint32_t a, const uint8_t p[OUTROW]){ (void)a; (void)p; }'; \
		  printf '%s\n' 'static int next(void *c, uint8_t *out){ (void)c; (void)out; return PATCH_PULL_END; }'; \
		  printf '%s\n' 'int main(void){ PatchApply pa; return patch_apply_run(&pa, next, 0); }'; \
		} > "$$csrc"; \
		if [ "$$form" = single ]; then \
			$(CC) $(SINGLE_DECODER_CFLAGS) -Wconversion -c "$$csrc" -o "$$obj"; \
			$(CC) $(SINGLE_DECODER_CFLAGS) $(PORTABLE_FALLBACK_FLAGS) -Wconversion -c "$$csrc" -o "$$obj"; \
		else \
			$(CC) $(DECODER_CFLAGS) -Wconversion -Isrc -c "$$csrc" -o "$$obj"; \
			$(CC) $(DECODER_CFLAGS) $(PORTABLE_FALLBACK_FLAGS) -Wconversion -Isrc -c "$$csrc" -o "$$obj"; \
		fi; \
	done; \
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
	  SINGLE_DECODER_CFLAGS="$(SINGLE_DECODER_CFLAGS)" \
	  DECODER_SINGLE_HDR="$(DECODER_CANONICAL_HDR)" \
	  DECODER_PUBLIC_HDRS="$(DECODER_PUBLIC_HDRS)" FIXTURES="$(FIXTURES)" \
	  scripts/check_decoder_api.sh; \
	echo "decoder_address_contract=OK (mandatory base/capacity + page alignment + uint32 headroom)"; \
	echo "decoder_portable=OK (fallback branch: compile + GNU-free purity + one-face round-trip)"; \
	echo "decoder_contract=OK"

# Pointer-rich in-memory decoder/backend contract under dynamic sanitizers. Standalone so its
# instrumented compile does not contend with the CPU-saturated corpus workers in `make gate`.
check-decoder-sanitize-internal: ultrapatch $(DECODER_CANONICAL_HDR)
	@CC="$(CC)" NM="$(NM)" CFLAGS="$(DECODER_CFLAGS)" \
	  SINGLE_DECODER_CFLAGS="$(SINGLE_DECODER_CFLAGS)" \
	  DECODER_SINGLE_HDR="$(DECODER_CANONICAL_HDR)" \
	  DECODER_PUBLIC_HDRS="$(DECODER_PUBLIC_HDRS)" FIXTURES="$(FIXTURES)" \
	  DECODER_API_REGULAR=0 DECODER_API_SANITIZE=1 scripts/check_decoder_api.sh

check-models-internal: $(DECODER_CANONICAL_HDR)
	@CC="$(CC)" CFLAGS="$(DECODER_CFLAGS)" \
	  SINGLE_DECODER_CFLAGS="$(SINGLE_DECODER_CFLAGS)" \
	  DECODER_SINGLE_HDR="$(DECODER_CANONICAL_HDR)" scripts/check_models.sh

check-assets-internal:
	@scripts/verify_corpus.sh "$(CORPUS_MANIFEST)"
	@scripts/verify_corpus.sh "$(FOREIGN_MANIFEST)" foreign_assets

# qemu-based decode validation REMOVED - permanent decision (owner, 2026-07-03): too slow
# for its marginal value (the 260-pair matrix re-encoded every corpus pair just to apply it
# under emulation; ~45 CPU-min per run). Host-vs-ARM divergence is systematic when it exists,
# not pair-specific; the ARM cross-build + size/divide gate (check-arm) still compiles the
# real Thumb-1 decoder every cycle, and a one-time 260-pair qemu study (db6d693) found ZERO
# divergence. Do not reintroduce qemu legs into the gate.

check-malformed-internal: ultrapatch
	@FIXTURES="$(FIXTURES)" scripts/check_malformed.sh
	@CC="$(CC)" CFLAGS="$(CFLAGS)" FIXTURES="$(FIXTURES)" scripts/check_transactional.sh
	@CC="$(CC)" CFLAGS="$(CFLAGS)" scripts/check_elf_ranges.sh
	@FIXTURES="$(FIXTURES)" scripts/check_dispatch_crash.sh malformed

# Synthetic edge inputs the firmware corpus never exercises (empty/tiny/equal/random/text/
# page-boundary/>384KiB-span pairs). ultrapatch self-verifies every encoded blob, so each case must
# either round-trip byte-exactly through BOTH host decoders or refuse cleanly.
check-edge-internal: ultrapatch
	@scripts/check_edge.sh
	@scripts/check_dispatch_crash.sh edge

# Degradation / direction / row-window / big-span gate: synthetic pairs that FORCE each encoder
# path the golden set and home corpus never exercise (journal-budget degradation, OPC_CAP
# op-split, unnatural apply direction, row-window-oracle reliance, big-span journal), asserting
# the path was actually taken — not merely that the blob round-trips. Builds a D=1 variant decoder
# to prove the monotone larger-window compatibility contract. Small synthetic fixtures, fast.
check-degrade-internal: ultrapatch
	@CC="$(CC)" CFLAGS="$(DECODER_CFLAGS)" \
	  ENC_SEAM_SRCS="$(ENC_SEAM_SRCS)" DEC_STANDALONE_SRCS="$(DEC_STANDALONE_SRCS)" \
	  DEC_DEMO_DEFINES="$(DEC_DEMO_DEFINES)" scripts/check_degrade.sh

# Golden-wire regression: sha256 of eight representative blobs pinned in test-bench/golden.sha256.
# Catches size-neutral wire drift and enforces the wire freeze. On an INTENDED wire change run
# `make golden-update`: it stages the eight-blob golden manifest, all 290 corpus hashes, and the
# 256 home per-pair sizes, validates the full round-trip/write-safety matrix and no-regression
# policy, then transactionally publishes the manifests and their four Makefile ratchets as one
# recoverable generation. Commit all resulting changes in the SAME commit.
check-golden-internal: ultrapatch scripts/check_wire_baseline_update.py \
                       scripts/publish_wire_baselines.py
	@FIXTURES="$(FIXTURES)" IMAGES="$(IMAGES)" GOLDEN_MANIFEST="$(GOLDEN_MANIFEST)" \
	  scripts/check_golden.sh check
	@python3 scripts/check_wire_baseline_update.py --host-tool "$(HOST_TOOL)" \
	  --release-profile-lock "$(RELEASE_PROFILE_LOCK)"

# Intentional-wire-change A/B regression: a small real home+foreign matrix verifies that both
# measurement runs bypass the committed wire manifest while retaining round-trip and NVM checks.
check-ab-matrix-internal: ultrapatch
	@scripts/check_ab_matrix.sh

golden-update-internal: scripts/publish_wire_baselines.py
	@python3 scripts/publish_wire_baselines.py recover --root test-bench
	@$(MAKE) --no-print-directory golden-update-after-recovery-internal

.PHONY: golden-update-after-recovery-internal
golden-update-after-recovery-internal:
	@$(MAKE) --no-print-directory golden-update-inputs-internal
	@$(MAKE) --no-print-directory golden-update-measure-internal

.PHONY: golden-update-measure-internal
golden-update-measure-internal: ultrapatch scripts/publish_wire_baselines.py
	@set -e; \
	. ./scripts/tempdir.sh; \
	release_profile=$$(python3 scripts/build_profile.py verify-release "$(RELEASE_PROFILE_LOCK)"); \
	release_profile=$${release_profile#release_profile=}; \
	host_tool_hash=$$(sha256sum "$(HOST_TOOL)"); host_tool_hash=$${host_tool_hash%% *}; \
	exec 9<>"$(WIRE_BASELINE_LOCK)"; flock --shared 9; \
	preimage_golden=$$(sha256sum test-bench/golden.sha256); preimage_golden=$${preimage_golden%% *}; \
	preimage_home=$$(sha256sum test-bench/home-size-baseline.tsv); preimage_home=$${preimage_home%% *}; \
	preimage_wire=$$(sha256sum test-bench/corpus-wire.sha256); preimage_wire=$${preimage_wire%% *}; \
	preimage_make=$$(sha256sum Makefile); preimage_make=$${preimage_make%% *}; \
	cp test-bench/golden.sha256 "$$tmp/golden.sha256"; flock --unlock 9; exec 9>&-; \
	FIXTURES="$(FIXTURES)" IMAGES="$(IMAGES)" GOLDEN_MANIFEST="$$tmp/golden.sha256" \
	  scripts/check_golden.sh update >"$$tmp/golden.out"; \
	IMAGES="$(IMAGES)" FOREIGN="$(FOREIGN)" CORPUS_SIZE_BASELINE="" \
	  CORPUS_SIZE_DUMP="$$tmp/home-size-baseline.tsv" CORPUS_WIRE_MANIFEST="" \
	  CORPUS_WIRE_DUMP="$$tmp/corpus-wire.sha256" BASE_FULL_TOTAL="" BASE_FOREIGN_TOTAL="" \
	  ./check_corpus.sh $(JOBS) >"$$tmp/corpus.out"; \
	home_pairs=$$(( $(BASE_RELEASE_HOME_IMAGES) * $(BASE_RELEASE_HOME_IMAGES) )); \
	foreign_pairs=$$(( ($(BASE_RELEASE_FOREIGN_IMAGES) - 1) * 2 )); \
	test "$$(wc -l <"$$tmp/golden.sha256")" -eq "$(BASE_RELEASE_GOLDEN_BLOBS)"; \
	test "$$(wc -l <"$$tmp/home-size-baseline.tsv")" -eq "$$home_pairs"; \
	test "$$(wc -l <"$$tmp/corpus-wire.sha256")" -eq "$$((home_pairs + foreign_pairs))"; \
	test "$$(sha256sum "$(HOST_TOOL)" | awk '{print $$1}')" = "$$host_tool_hash"; \
	test "$$(python3 scripts/build_profile.py verify-release "$(RELEASE_PROFILE_LOCK)")" = \
	  "release_profile=$$release_profile"; \
	cp "$$tmp/corpus.out" "$$tmp/metrics.out"; \
	echo "measurement_release_profile=$$release_profile" >>"$$tmp/metrics.out"; \
	echo "measurement_host_tool_sha256=$$host_tool_hash" >>"$$tmp/metrics.out"; \
	echo "measurement_preimage_golden_sha256=$$preimage_golden" >>"$$tmp/metrics.out"; \
	echo "measurement_preimage_home_sha256=$$preimage_home" >>"$$tmp/metrics.out"; \
	echo "measurement_preimage_wire_sha256=$$preimage_wire" >>"$$tmp/metrics.out"; \
	echo "measurement_preimage_makefile_sha256=$$preimage_make" >>"$$tmp/metrics.out"; \
	exec 9<>"$(WIRE_BASELINE_LOCK)"; flock --shared 9; \
	$(MAKE) --no-print-directory golden-update-validate-canonical-internal \
	  >"$$tmp/post-measure-inputs.out"; \
	flock --unlock 9; exec 9>&-; \
	python3 scripts/publish_wire_baselines.py publish --root test-bench \
	  --inventory "$(CORPUS_INVENTORY)" --candidate-golden "$$tmp/golden.sha256" \
	  --candidate-home-sizes "$$tmp/home-size-baseline.tsv" \
	  --candidate-wire "$$tmp/corpus-wire.sha256" --metrics "$$tmp/metrics.out" \
	  --host-tool "$(HOST_TOOL)" --release-profile-lock "$(RELEASE_PROFILE_LOCK)" \
	  --home-limit "$(BASE_FULL_TOTAL)" --foreign-limit "$(BASE_FOREIGN_TOTAL)" \
	  --oneface-grow-limit "$(BASE_ONEFACE_GROW)" \
	  --oneface-revert-limit "$(BASE_ONEFACE_REVERT)"; \
	cat "$$tmp/golden.out"; cat "$$tmp/corpus.out"; \
	awk '$$3=="oneface_grow.blob"{print "oneface_grow=" $$2} \
	     $$3=="oneface_revert.blob"{print "oneface_revert=" $$2}' test-bench/golden.sha256; \
	echo "wire manifests and Makefile BASE_* ratchets published as one recoverable generation"

# The 256 home (from,to) pairs PLUS 34 foreign pair-directions (a second, unrelated Cortex-M0+
# lineage — CircuitPython feather_m0_express; see docs/foreign-firmware-study.md) are independent,
# so they run in ONE parallel pool across all cores via check_corpus.sh (each worker gets its own
# mktemp dir — no shared blob path, contamination-safe). The foreign set's cross-major pair is the
# single slowest job and is scheduled first so it overlaps everything (LPT), keeping the leg near
# its home-only wall. matrix_ok/full_total gate the home set; home_size_{better,worse,equal}
# compares each home pair against CORPUS_SIZE_BASELINE and rejects any worse pair;
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
check-corpus-matrix-internal: ultrapatch
	@IMAGES="$(IMAGES)" FOREIGN="$(FOREIGN)" CORPUS_SIZE_BASELINE="$(CORPUS_SIZE_BASELINE)" \
	CORPUS_INVENTORY="$(CORPUS_INVENTORY)" CORPUS_WIRE_MANIFEST="$(CORPUS_WIRE_MANIFEST)" \
	BASE_FULL_TOTAL="$(BASE_FULL_TOTAL)" BASE_FOREIGN_TOTAL="$(BASE_FOREIGN_TOTAL)" \
	./check_corpus.sh $(JOBS)

# THE gate — one target, everything, hard budget <= 80 s wall on the reference machine
# (measured 35.1 s warm at 32 cores with resource-pressure direction fallback).
# Builds up-front, then runs
# every leg CONCURRENTLY:
# check-assets, check (one-face grow/revert round-trip + BASE_ONEFACE_* size gates),
# check-malformed, check-edge, check-degrade, check-golden, check-decoder-contract,
# check-ab-matrix,
# check-models, check-wire-config, check-arm (sizes + divide policy), check-stack, and the FULL 256-pair corpus
# matrix + 34 foreign
# pair-directions (corpus full_total vs BASE_FULL_TOTAL, home per-pair better/worse/equal
# split vs CORPUS_SIZE_BASELINE with zero worse pairs allowed, foreign_total vs
# BASE_FOREIGN_TOTAL, foreign 34/34 round-trips, NVM write-safety, journal peak —
# check-corpus). Wall time ~= the slowest leg
# (check-corpus), not the sum. Prints one consolidated summary with every tracked
# metric; exits nonzero if ANY gate fails and dumps the raw blocks so the offending
# metric is visible. The profile-specific host tool and canonical generated decoder header are
# published atomically BEFORE the legs fork; run_gate.sh invokes each forked leg with make's `-o`
# (assume-old) for both exact artifacts, so no leg rebuilds either if a source mtime changes at
# sub-make startup. Same-profile concurrent top-level builds may both publish equivalent bytes.
gate-internal:
	@$(MAKE) --no-print-directory release-gate-inputs-internal
	@$(MAKE) --no-print-directory all-internal
	@$(MAKE) --no-print-directory decoder-header-internal
	@set -e; release_profile=$$(python3 scripts/build_profile.py verify-release "$(RELEASE_PROFILE_LOCK)"); \
	MAKE="$(MAKE)" HOST_TOOL="$(HOST_TOOL)" DECODER_CANONICAL_HDR="$(DECODER_CANONICAL_HDR)" \
	RELEASE_PROFILE="$$release_profile" \
	BASE_RELEASE_FIXTURES="$(BASE_RELEASE_FIXTURES)" \
	BASE_RELEASE_HOME_IMAGES="$(BASE_RELEASE_HOME_IMAGES)" \
	BASE_RELEASE_FOREIGN_IMAGES="$(BASE_RELEASE_FOREIGN_IMAGES)" \
	BASE_RELEASE_GOLDEN_BLOBS="$(BASE_RELEASE_GOLDEN_BLOBS)" \
	BASE_FULL_TOTAL="$(BASE_FULL_TOTAL)" BASE_FOREIGN_TOTAL="$(BASE_FOREIGN_TOTAL)" \
	BASE_ONEFACE_GROW="$(BASE_ONEFACE_GROW)" BASE_ONEFACE_REVERT="$(BASE_ONEFACE_REVERT)" \
	BASE_ARM_TEXT="$(BASE_ARM_TEXT)" BASE_ARM_DATA="$(BASE_ARM_DATA)" \
	BASE_ARM_BSS="$(BASE_ARM_BSS)" BASE_ARM_LINKED_TEXT="$(BASE_ARM_LINKED_TEXT)" \
	BASE_ARM_LINKED_DATA="$(BASE_ARM_LINKED_DATA)" BASE_ARM_LINKED_BSS="$(BASE_ARM_LINKED_BSS)" \
	BASE_ARM_SOFT_DIV="$(BASE_ARM_SOFT_DIV)" \
	BASE_ARM_EXTERNAL_TEXT="$(BASE_ARM_EXTERNAL_TEXT)" \
	BASE_ARM_EXTERNAL_LINKED_TEXT="$(BASE_ARM_EXTERNAL_LINKED_TEXT)" \
	BASE_STACK_STATIC_CEIL_O2="$(BASE_STACK_STATIC_CEIL_O2)" \
	BASE_STACK_GENERIC_CEIL_O2="$(BASE_STACK_GENERIC_CEIL_O2)" \
	BASE_STACK_EXTERNAL_STATIC_CEIL_O2="$(BASE_STACK_EXTERNAL_STATIC_CEIL_O2)" \
	BASE_STACK_EXTERNAL_GENERIC_CEIL_O2="$(BASE_STACK_EXTERNAL_GENERIC_CEIL_O2)" \
	JOBS="$(JOBS)" scripts/run_gate.sh

# Static-analysis leg: gcc -fanalyzer over first-party TUs (encoder modules + decoder + arm + selfcheck)
# with a curated flag set; clean baseline (exits nonzero on any NEW finding). STANDALONE (version-
# fragile + ~16 s), NOT in `make gate`; auto-skips where gcc -fanalyzer is unavailable.
check-analyze-internal:
	@CC="$(CC)" CONTRACT_FLAGS="$(CONTRACT_FLAGS)" WIRE_CONFIG_FLAGS="$(WIRE_CONFIG_FLAGS)" \
	  DECODER_CONFIG_FLAGS="$(DECODER_CONFIG_FLAGS)" ENC_MODULES="$(ENC_MODULE_SRCS)" scripts/check_analyze.sh

clean-internal:
	@root=$$(realpath -m .build); dir=$$(realpath -m "$(BUILD_DIR)"); \
	case "$$dir" in "$$root"/*) rm -rf -- "$$dir" ;; *) echo "refusing to clean noncanonical build dir: $$dir" >&2; exit 1 ;; esac
	rm -f ultrapatch artifacts/patch_apply_single.h

clean-all-internal:
	rm -rf .build
	rm -f ultrapatch artifacts/patch_apply_single.h
