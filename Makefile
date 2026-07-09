# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

CC = $(CROSS_COMPILE)gcc

OPT ?= -O2
# Target-family wire contract: CORTEX_M0 must be defined for BOTH the encoder and the
# decoder TU. CORTEX_M4 is reserved (future wire).
CFLAGS += -DCORTEX_M0
CFLAGS += -g
CFLAGS += -Wall
CFLAGS += -Wextra
CFLAGS += -Wdouble-promotion
CFLAGS += -Wfloat-equal
CFLAGS += -Wformat=2
CFLAGS += -Wshadow
CFLAGS += -Werror
CFLAGS += -std=c99
CFLAGS += $(OPT)
CFLAGS += -ffunction-sections
CFLAGS += -fdata-sections
CFLAGS += -I.
CFLAGS += -Isrc
CFLAGS += -Ivendor/libdivsufsort
CFLAGS += $(CFLAGS_EXTRA)
LDFLAGS += -Wl,--gc-sections

DIVSUF := vendor/libdivsufsort/divsufsort.c
CONFIG_HDR := src/patch_config.h
APPLY_HDR := src/patch_apply.h
DECODER_PUBLIC_HDRS := $(CONFIG_HDR) src/rc_models.h $(APPLY_HDR)
DECODER_SINGLE_HDR ?= artifacts/patch_apply_single.h
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
ENC_SRCS := src/patch_generate.c $(ENC_MODULE_SRCS) $(HOST_BACKEND_SRC) $(DIVSUF)
DEC_SRCS := $(HOST_BACKEND_SRC)
TOOL_SRCS := $(ENC_SRCS)

FIXTURES ?= test-bench/fixtures
IMAGES ?= test-bench/images
FOREIGN ?= test-bench/foreign
CORPUS_MANIFEST ?= test-bench/corpus.sha256
FOREIGN_MANIFEST ?= test-bench/foreign.sha256
CORPUS_SIZE_BASELINE ?= test-bench/home-size-baseline.tsv

BASE_FULL_TOTAL ?= 4151373
# Foreign lineage (CircuitPython feather_m0_express, 34 pair-directions): summed blob bytes.
# Ratchets like BASE_FULL_TOTAL — a wire regression on firmware A1 was NOT tuned on fails here.
# Re-pin on intentional wire changes. See docs/foreign-firmware-study.md.
BASE_FOREIGN_TOTAL ?= 1333390
BASE_ONEFACE_GROW ?= 573
BASE_ONEFACE_REVERT ?= 287
BASE_ARM_TEXT ?= 5973
BASE_ARM_DATA ?= 0
BASE_ARM_BSS ?= 10308
BASE_ARM_SOFT_DIV ?= 1
ARM_DEC_FLAGS := -mcpu=cortex-m0plus -mthumb -DCORTEX_M0 -I src
# The production ARM size gate intentionally measures the static-state wrapper integration
# used by rcv3_run below. A generic caller-owned PatchApply * wrapper may compile differently;
# product notes/gate output must not present this number as shape-independent.
ARM_APPLY_HARNESS = printf '%s\n' '\#include "patch_apply.h"' 'static PatchApply g_patch_apply_state;' 'int rcv3_run(int (*next)(void*, uint8_t*), void *ctx){ return patch_apply_run(&g_patch_apply_state, next, ctx); }' > "$$tmp/patch_apply_arm.c"
# Worst-case caller-stack ceiling for patch_apply_run(), gcc -O2, Cortex-M0+ (bytes). The
# decode runs entirely on the caller's stack (no fiber since 44eee88); scripts/stack_bound.py
# derives the exact static bound from -fstack-usage frames + the call graph. The current
# measured bound is printed by `make check-stack` and pinned in docs/device-integration.md
# (single source of truth for the number); the ceiling below gives ample headroom and
# check-stack fails above it.
BASE_STACK_CEIL_O2 ?= 480

# ---- hard 60 s execution cap on EVERY public target ---------------------------------
# Owner rule: we do not throw compute just for fun — an operation that cannot finish in
# 60 s gets fixed or deleted, not waited for. Each public name is a thin cap over its
# *-internal twin: coreutils `timeout` bounds the whole subtree (it runs the child in
# its own process group, so backgrounded gate legs die with it) and an overrun reports
# an explicit error instead of a bare status 124. One-off override (never commit a
# longer default): A1_TIMEOUT=<secs> make <target>.
A1_TIMEOUT ?= 60
CAPPED := all decoder-header check check-arm check-stack check-assets check-decoder-contract \
          check-models check-malformed check-corpus check-edge check-degrade check-golden \
          golden-update gate check-analyze clean
.PHONY: $(CAPPED) $(addsuffix -internal,$(CAPPED))
$(CAPPED): %:
	@timeout $(A1_TIMEOUT) $(MAKE) --no-print-directory $*-internal; s=$$?; \
	if [ $$s -eq 124 ]; then echo "Execution timelimit $(A1_TIMEOUT) exceeded" >&2; fi; \
	exit $$s

all-internal: ultrapatch
	$(CC) $(CFLAGS) -Wconversion -D_POSIX_C_SOURCE=200809L -DPATCH_APPLY_DEMO_MAIN -c $(DEC_SRCS) -o /dev/null

decoder-header-internal: $(DECODER_PUBLIC_HDRS) scripts/gen_single_header.py
	@python3 scripts/gen_single_header.py "$(DECODER_SINGLE_HDR)" $(DECODER_PUBLIC_HDRS)
	@echo "decoder_header=$(DECODER_SINGLE_HDR)"

ultrapatch: $(TOOL_SRCS) $(GEN_HDR) $(APPLY_HDR) $(NVM_EMU)
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=200809L $(TOOL_SRCS) $(LDFLAGS) -o $@

check-internal: ultrapatch
	@set -e; \
	. ./scripts/tempdir.sh; \
	./ultrapatch --help >"$$tmp/help.txt"; \
	./ultrapatch -h >"$$tmp/help-short.txt"; \
	grep -q '^usage: .*ultrapatch' "$$tmp/help.txt"; \
	grep -q '^usage: .*ultrapatch' "$$tmp/help-short.txt"; \
	grep -q '^  --decode' "$$tmp/help.txt"; \
	if ./ultrapatch >"$$tmp/noargs.out" 2>"$$tmp/noargs.err"; then \
		echo "ultrapatch with no arguments unexpectedly succeeded" >&2; exit 1; \
	fi; \
	grep -q '^usage: .*ultrapatch' "$$tmp/noargs.err"; \
	FIXTURES="$(FIXTURES)" ONEFACE_ROUNDTRIP=1 \
	  BASE_ONEFACE_GROW="$(BASE_ONEFACE_GROW)" BASE_ONEFACE_REVERT="$(BASE_ONEFACE_REVERT)" \
	  scripts/oneface_metrics.sh ./ultrapatch ./ultrapatch

check-arm-internal:
	@set -e; \
	. ./scripts/tempdir.sh; \
	$(ARM_APPLY_HARNESS); \
	arm-none-eabi-gcc $(ARM_DEC_FLAGS) -Os -c "$$tmp/patch_apply_arm.c" -o "$$tmp/patch_apply_arm.o"; \
	size_out=$$(arm-none-eabi-size "$$tmp/patch_apply_arm.o"); \
	printf '%s\n' "$$size_out"; \
	echo "arm_size_integration=static PatchApply wrapper"; \
	set -- $$(printf '%s\n' "$$size_out" | awk 'NR==2 { print $$1, $$2, $$3 }'); \
	test "$$1" -le "$(BASE_ARM_TEXT)"; \
	test "$$2" -le "$(BASE_ARM_DATA)"; \
	test "$$3" -le "$(BASE_ARM_BSS)"; \
	arm-none-eabi-objdump -d "$$tmp/patch_apply_arm.o" > "$$tmp/patch_apply_arm.dump"; \
	if grep -Eq '\b(udiv|sdiv)\b' "$$tmp/patch_apply_arm.dump"; then \
		echo "hardware divide instruction found"; exit 1; \
	fi; \
	soft=$$(grep -Ec '__aeabi_.*div|__aeabi_.*mod' "$$tmp/patch_apply_arm.dump" || true); \
	echo "soft_div_calls=$$soft"; \
	test "$$soft" -eq "$(BASE_ARM_SOFT_DIV)"

# Worst-case caller-stack bound for patch_apply_run(). Since the fiber was deleted (44eee88)
# the whole decode runs on the CALLER's stack; docs/device-integration.md pins the budget an
# integrator must reserve. Builds the decoder harness with -fstack-usage (gcc -O2,
# the pessimistic level — deeper than -Os here) and runs scripts/stack_bound.py, which sums the
# per-function .su frames (each already includes its own pushed LR/regs) along the deepest path
# of the static call graph extracted from the disassembly. It fails loudly on recursion, an
# indirect call that could reach internal code, or a dynamic/VLA frame — any of which would
# invalidate the static method. The bound EXCLUDES the integrator's externs (flash_read/
# flash_write/byte-callback) and toolchain leaves (memmove/memset/__aeabi_uidiv); the ceiling's
# headroom absorbs those + interrupt-frame slack. Same cross-gcc as check-arm.
check-stack-internal:
	@set -e; \
	. ./scripts/tempdir.sh; \
	$(ARM_APPLY_HARNESS); \
	arm-none-eabi-gcc $(ARM_DEC_FLAGS) -O2 -c "$$tmp/patch_apply_arm.c" -o "$$tmp/patch_apply_arm.o" -fstack-usage; \
	python3 scripts/stack_bound.py "$$tmp/patch_apply_arm.o" > "$$tmp/stack.txt"; \
	cat "$$tmp/stack.txt"; \
	echo "stack_ceiling_o2=$(BASE_STACK_CEIL_O2)"; \
	bound=$$(sed -n 's/^stack_bound_bytes=//p' "$$tmp/stack.txt"); \
	test -n "$$bound"; \
	test "$$bound" -le "$(BASE_STACK_CEIL_O2)"

# Portability contract: the decoder is standard C (C99 + C11 _Static_assert); GNU
# attributes/builtins are optional codegen hints behind guards with live fallbacks (rc_models.h
# note). -DA1_NO_GNU_EXTENSIONS is the documented knob that forces the fallback branch — a
# first-party switch, so system headers are untouched (no fragile __GNUC__ masking against
# glibc). Wire correctness is compiler-independent; only the gated size/stack budgets are
# GNU-toolchain-measured. Enforced below three ways: (a) both header forms smoke-compile with
# the fallbacks forced, (b) the preprocessed fallback smoke TU must contain ZERO
# __attribute__/__builtin_ tokens in first-party (src/) lines — this catches a future bare
# GNU-ism that a gcc compile alone would accept (__builtin_offsetof is exempt: it is the
# SYSTEM stddef.h's conforming implementation of the standard offsetof macro our asserts use;
# a non-GNU toolchain's stddef.h supplies its own), (c) a fallback-built host decoder must
# round-trip the real one-face patch byte-exactly.
PORTABLE_FALLBACK_FLAGS := -DA1_NO_GNU_EXTENSIONS
check-decoder-contract-internal: ultrapatch
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
	single="$$tmp/patch_apply_single.h"; \
	python3 scripts/gen_single_header.py "$$single" $(DECODER_PUBLIC_HDRS); \
	if grep -nE '^[[:space:]]*#include[[:space:]]*"' "$$single" | grep -E '"(patch_config|rc_models|patch_apply)\.h"'; then \
		echo "generated single decoder header must not include local decoder support headers" >&2; exit 1; \
	fi; \
	for inc in patch_apply.h patch_apply_single.h; do \
		csrc="$$tmp/$${inc%.h}_smoke.c"; obj="$$tmp/$${inc%.h}_smoke.o"; \
		{ printf '%s\n' '#include <stdint.h>'; \
		  printf '%s\n' "#include \"$$inc\""; \
		  printf '%s\n' 'uint8_t flash_read(uint32_t a){ (void)a; return 0xffu; }'; \
		  printf '%s\n' 'void flash_write(uint32_t a, uint8_t v){ (void)a; (void)v; }'; \
		  printf '%s\n' 'static int next(void *c, uint8_t *out){ (void)c; (void)out; return 0; }'; \
		  printf '%s\n' 'int main(void){ PatchApply pa; return patch_apply_run(&pa, next, 0); }'; \
		} > "$$csrc"; \
		if [ "$$inc" = patch_apply_single.h ]; then \
			$(CC) $(filter-out -Isrc,$(CFLAGS)) -Wconversion -DCORTEX_M0 -I"$$tmp" -c "$$csrc" -o "$$obj"; \
			$(CC) $(filter-out -Isrc,$(CFLAGS)) $(PORTABLE_FALLBACK_FLAGS) -Wconversion -DCORTEX_M0 -I"$$tmp" -c "$$csrc" -o "$$obj"; \
		else \
			$(CC) $(CFLAGS) -Wconversion -DCORTEX_M0 -Isrc -c "$$csrc" -o "$$obj"; \
			$(CC) $(CFLAGS) $(PORTABLE_FALLBACK_FLAGS) -Wconversion -DCORTEX_M0 -Isrc -c "$$csrc" -o "$$obj"; \
		fi; \
	done; \
	$(CC) $(CFLAGS) $(PORTABLE_FALLBACK_FLAGS) -DCORTEX_M0 -Isrc -E "$$tmp/patch_apply_smoke.c" | \
	awk '/^# [0-9]+ "/ { inours = ($$3 ~ /"src\//) } \
	     inours { line = $$0; gsub(/__builtin_offsetof/, "", line); \
	              if (line ~ /__attribute__|__builtin_/) { print "GNU construct in portable decoder build: " $$0; bad=1 } } \
	     END { exit bad ? 1 : 0 }'; \
	$(CC) $(CFLAGS) $(PORTABLE_FALLBACK_FLAGS) -D_POSIX_C_SOURCE=200809L -DPATCH_APPLY_DEMO_MAIN \
	    src/patch_host_backend.c src/enc_util.c -o "$$tmp/dec_portable"; \
	FIXTURES="$(FIXTURES)" ONEFACE_ROUNDTRIP=1 \
	    scripts/oneface_metrics.sh ./ultrapatch "$$tmp/dec_portable" >/dev/null; \
	echo "decoder_portable=OK (fallback branch: compile + GNU-free purity + one-face round-trip)"; \
	echo "decoder_contract=OK"

check-models-internal:
	@CC="$(CC)" CFLAGS="$(CFLAGS)" scripts/check_models.sh

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
check-degrade-internal: ultrapatch
	@scripts/check_degrade.sh

# Golden-wire regression: sha256 of eight representative blobs pinned in test-bench/golden.sha256.
# Catches size-neutral wire drift and enforces the wire freeze. On an INTENDED wire change run
# `make golden-update` and commit the manifest (plus size baselines) in the SAME commit.
check-golden-internal: ultrapatch
	@FIXTURES="$(FIXTURES)" IMAGES="$(IMAGES)" scripts/check_golden.sh check

golden-update-internal: ultrapatch
	@FIXTURES="$(FIXTURES)" IMAGES="$(IMAGES)" scripts/check_golden.sh update

# The 256 home (from,to) pairs PLUS 34 foreign pair-directions (a second, unrelated Cortex-M0+
# lineage — CircuitPython feather_m0_express; see docs/foreign-firmware-study.md) are independent,
# so they run in ONE parallel pool across all cores via check_corpus.sh (each worker gets its own
# mktemp dir — no shared blob path, contamination-safe). The foreign set's cross-major pair is the
# single slowest job and is scheduled first so it overlaps everything (LPT), keeping the leg near
# its home-only wall. matrix_ok/full_total gate the home set; home_size_{better,worse,equal}
# compares each home pair against CORPUS_SIZE_BASELINE and rejects any worse pair;
# foreign_ok(=34)/foreign_total gate the foreign set (foreign_total ratchets vs
# BASE_FOREIGN_TOTAL); NVM write-safety maxima cover BOTH. Override parallelism with JOBS=N
# (defaults to nproc).
check-corpus-internal: ultrapatch
	@IMAGES="$(IMAGES)" FOREIGN="$(FOREIGN)" CORPUS_SIZE_BASELINE="$(CORPUS_SIZE_BASELINE)" \
	BASE_FULL_TOTAL="$(BASE_FULL_TOTAL)" BASE_FOREIGN_TOTAL="$(BASE_FOREIGN_TOTAL)" \
	./check_corpus.sh $(JOBS)

# THE gate — one target, everything, hard budget <= 60 s wall on the reference machine
# (measured ~29 s at 32 jobs, incl. the folded-in foreign lineage). Builds up-front, then runs
# every leg CONCURRENTLY:
# check-assets, check (one-face grow/revert round-trip + BASE_ONEFACE_* size gates),
# check-malformed, check-edge, check-degrade, check-golden, check-decoder-contract,
# check-models, check-arm (sizes + divide policy), check-stack, and the FULL 256-pair corpus
# matrix + 34 foreign
# pair-directions (corpus full_total vs BASE_FULL_TOTAL, home per-pair better/worse/equal
# split vs CORPUS_SIZE_BASELINE with zero worse pairs allowed, foreign_total vs
# BASE_FOREIGN_TOTAL, foreign 34/34 round-trips, NVM write-safety, journal peak —
# check-corpus). Wall time ~= the slowest leg
# (check-corpus), not the sum. Prints one consolidated summary with every tracked
# metric; exits nonzero if ANY gate fails and dumps the raw blocks so the offending
# metric is visible. ultrapatch is linked BEFORE the legs fork; run_gate.sh then invokes
# each forked leg with make's `-o ultrapatch` (assume-old), so no leg relinks the shared
# binary even if a source mtime became newer at sub-make startup (which would otherwise let
# several legs race concurrent `-o ultrapatch` links on the same path while other legs exec
# it). The sub-makes still stat sources for their own rules; only the prebuilt binary is pinned.
gate-internal: all-internal
	@MAKE="$(MAKE)" BASE_ARM_TEXT="$(BASE_ARM_TEXT)" BASE_ARM_DATA="$(BASE_ARM_DATA)" \
	BASE_ARM_BSS="$(BASE_ARM_BSS)" scripts/run_gate.sh

# Static-analysis leg: gcc -fanalyzer over first-party TUs (encoder modules + decoder + arm + selfcheck)
# with a curated flag set; clean baseline (exits nonzero on any NEW finding). STANDALONE (version-
# fragile + ~16 s), NOT in `make gate`; auto-skips where gcc -fanalyzer is unavailable.
check-analyze-internal:
	@ENC_MODULES="$(ENC_MODULE_SRCS)" scripts/check_analyze.sh

clean-internal:
	rm -f ultrapatch $(DECODER_SINGLE_HDR)
