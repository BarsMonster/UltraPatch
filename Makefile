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
CFLAGS += -I.
CFLAGS += -Isrc
CFLAGS += -Ivendor/libdivsufsort
CFLAGS += $(CFLAGS_EXTRA)

DIVSUF := vendor/libdivsufsort/divsufsort.c
CONFIG_HDR := src/patch_config.h
APPLY_HDR := src/patch_apply.h
ADAPTER_HDR := src/patch_apply_push_adapter.h
# Shared host-side NVM emulator, #included by patch_selfcheck.c (in hy_enc) and
# patch_apply_demo.c (hy_dec) before their patch_apply.h.
NVM_EMU := src/nvm_emu.inc
# patch_generate.c is a thin umbrella TU that #includes the ordered enc_*.inc modules
# (the host encoder is one translation unit -- see the include block in patch_generate.c).
# Listed here so editing any module re-triggers every target that compiles the encoder.
ENC_MODULES := src/enc_util.inc src/enc_elf.inc src/enc_bsdiff.inc src/enc_field.inc \
               src/enc_rc.inc src/enc_lz.inc src/enc_emit.inc src/enc_plan.inc src/enc_cli.inc
GEN_HDR := src/rc_models.h $(CONFIG_HDR) src/arm_cortex_m4.h $(ENC_MODULES)
ENC_SRCS := src/patch_generate.c src/arm_cortex_m4.c src/patch_selfcheck.c $(DIVSUF)
DEC_SRCS := src/patch_apply_demo.c

FIXTURES ?= test-bench/fixtures
IMAGES ?= test-bench/images
FOREIGN ?= test-bench/foreign
CORPUS_MANIFEST ?= test-bench/corpus.sha256
FOREIGN_MANIFEST ?= test-bench/foreign.sha256

BASE_FULL_TOTAL ?= 4151558
# Foreign lineage (CircuitPython feather_m0_express, 34 pair-directions): summed blob bytes.
# Ratchets like BASE_FULL_TOTAL — a wire regression on firmware A1 was NOT tuned on fails here.
# Re-pin on intentional wire changes. See docs/foreign-firmware-study.md.
BASE_FOREIGN_TOTAL ?= 1333476
BASE_ONEFACE_GROW ?= 574
BASE_ONEFACE_REVERT ?= 287
BASE_ARM_TEXT ?= 6253
BASE_ARM_DATA ?= 0
BASE_ARM_BSS ?= 11360
BASE_ARM_SOFT_DIV ?= 1
# Worst-case caller-stack ceiling for patch_apply_run(), gcc -O2, Cortex-M0+ (bytes). The
# decode runs entirely on the caller's stack (no fiber since 44eee88); scripts/stack_bound.py
# derives the exact static bound from -fstack-usage frames + the call graph. Measured 336 B
# (was 368 B before the CRC32(to)-in-header change deleted the trailer-withhold raw_next rotation,
# which sat on the worst path); pinned ceiling gives ample headroom. check-stack fails above this.
BASE_STACK_CEIL_O2 ?= 480

# ---- hard 60 s execution cap on EVERY public target ---------------------------------
# Owner rule: we do not throw compute just for fun — an operation that cannot finish in
# 60 s gets fixed or deleted, not waited for. Each public name is a thin cap over its
# *-internal twin: coreutils `timeout` bounds the whole subtree (it runs the child in
# its own process group, so backgrounded gate legs die with it) and an overrun reports
# an explicit error instead of a bare status 124. One-off override (never commit a
# longer default): A1_TIMEOUT=<secs> make <target>.
A1_TIMEOUT ?= 60
CAPPED := all check check-arm check-stack check-stack-qemu check-assets check-decoder-contract \
          check-malformed check-corpus check-edge check-degrade check-golden \
          check-models golden-update gate check-analyze clean
.PHONY: $(CAPPED) $(addsuffix -internal,$(CAPPED))
$(CAPPED): %:
	@timeout $(A1_TIMEOUT) $(MAKE) --no-print-directory $*-internal; s=$$?; \
	if [ $$s -eq 124 ]; then echo "Execution timelimit $(A1_TIMEOUT) exceeded" >&2; fi; \
	exit $$s

all-internal: hy_enc hy_dec

hy_enc: $(ENC_SRCS) $(GEN_HDR) $(APPLY_HDR) $(NVM_EMU)
	$(CC) $(CFLAGS) -DRC_V3_ENC_MAIN $(ENC_SRCS) -o $@

# The decoder TU is additionally -Wconversion-clean (the safety-critical artifact carries
# the stricter bar; the host-side encoder does not). Single decode mode: patch_apply_run()
# + integrator callback; byte_mode (arg3) streams through the optional push adapter.
hy_dec: $(DEC_SRCS) $(APPLY_HDR) $(ADAPTER_HDR) $(NVM_EMU)
	$(CC) $(CFLAGS) -Wconversion -D_POSIX_C_SOURCE=200809L $(DEC_SRCS) -o $@

check-internal: all-internal
	@set -e; \
	tmp=$$(mktemp -d); \
	trap 'rm -rf "$$tmp"' EXIT; \
	cp "$(FIXTURES)/v0_base/watch.bin" "$$tmp/mem.bin"; \
	./hy_enc "$(FIXTURES)/v0_base" "$(FIXTURES)/v1_one_face" "$$tmp/grow.blob"; \
	./hy_dec "$$tmp/mem.bin" "$$tmp/grow.blob" 1 >/dev/null; \
	cmp "$$tmp/mem.bin" "$(FIXTURES)/v1_one_face/watch.bin"; \
	cp "$(FIXTURES)/v1_one_face/watch.bin" "$$tmp/mem.bin"; \
	./hy_enc "$(FIXTURES)/v1_one_face" "$(FIXTURES)/v0_base" "$$tmp/revert.blob"; \
	./hy_dec "$$tmp/mem.bin" "$$tmp/revert.blob" 1 >/dev/null; \
	cmp "$$tmp/mem.bin" "$(FIXTURES)/v0_base/watch.bin"; \
	grow_sz=$$(wc -c < "$$tmp/grow.blob"); \
	revert_sz=$$(wc -c < "$$tmp/revert.blob"); \
	wc -c "$$tmp/grow.blob" "$$tmp/revert.blob"; \
	echo "oneface_grow=$$grow_sz"; \
	echo "oneface_revert=$$revert_sz"; \
	test "$$grow_sz" -le "$(BASE_ONEFACE_GROW)"; \
	test "$$revert_sz" -le "$(BASE_ONEFACE_REVERT)"

check-arm-internal:
	@set -e; \
	tmp=$$(mktemp -d); \
	trap 'rm -rf "$$tmp"' EXIT; \
	{ printf '%s\n' '#include "patch_apply.h"'; \
	  printf '%s\n' 'static PatchApply g_patch_apply_state;'; \
	  printf '%s\n' 'int rcv3_run(int (*next)(void*, uint8_t*), void *ctx){ return patch_apply_run(&g_patch_apply_state, next, ctx); }'; \
	} > "$$tmp/patch_apply_arm.c"; \
	arm-none-eabi-gcc -mcpu=cortex-m0plus -mthumb -Os -DCORTEX_M0 -I src -c "$$tmp/patch_apply_arm.c" -o "$$tmp/patch_apply_arm.o"; \
	size_out=$$(arm-none-eabi-size "$$tmp/patch_apply_arm.o"); \
	printf '%s\n' "$$size_out"; \
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
# integrator must reserve. Builds the RC_V3_ARM decoder standalone with -fstack-usage (gcc -O2,
# the pessimistic level — deeper than -Os here) and runs scripts/stack_bound.py, which sums the
# per-function .su frames (each already includes its own pushed LR/regs) along the deepest path
# of the static call graph extracted from the disassembly. It fails loudly on recursion, an
# indirect call that could reach internal code, or a dynamic/VLA frame — any of which would
# invalidate the static method. The bound EXCLUDES the integrator's externs (flash_read/
# flash_write/byte-callback) and toolchain leaves (memmove/memset/__aeabi_uidiv); the ceiling's
# headroom absorbs those + interrupt-frame slack. Same cross-gcc as check-arm.
check-stack-internal:
	@set -e; \
	tmp=$$(mktemp -d); \
	trap 'rm -rf "$$tmp"' EXIT; \
	{ printf '%s\n' '#include "patch_apply.h"'; \
	  printf '%s\n' 'static PatchApply g_patch_apply_state;'; \
	  printf '%s\n' 'int rcv3_run(int (*next)(void*, uint8_t*), void *ctx){ return patch_apply_run(&g_patch_apply_state, next, ctx); }'; \
	} > "$$tmp/patch_apply_arm.c"; \
	arm-none-eabi-gcc -mcpu=cortex-m0plus -mthumb -O2 -DCORTEX_M0 -I src -c "$$tmp/patch_apply_arm.c" -o "$$tmp/patch_apply_arm.o" -fstack-usage; \
	python3 scripts/stack_bound.py "$$tmp/patch_apply_arm.o" > "$$tmp/stack.txt"; \
	cat "$$tmp/stack.txt"; \
	echo "stack_ceiling_o2=$(BASE_STACK_CEIL_O2)"; \
	bound=$$(sed -n 's/^stack_bound_bytes=//p' "$$tmp/stack.txt"); \
	test -n "$$bound"; \
	test "$$bound" -le "$(BASE_STACK_CEIL_O2)"

check-decoder-contract-internal:
	@set -e; \
	if grep -nE '^[[:space:]]*#include[[:space:]]*"' src/patch_apply.h; then \
		echo "patch_apply.h must be self-contained and not include project headers" >&2; exit 1; \
	fi; \
	if grep -nE '\b(malloc|calloc|realloc|free)[[:space:]]*\(' src/patch_apply.h; then \
		echo "patch_apply.h must not use dynamic memory" >&2; exit 1; \
	fi; \
	if awk '/^static[[:space:]]/ && $$0 !~ /\(/ { print FILENAME ":" FNR ":" $$0; bad=1 } END{ exit bad ? 1 : 0 }' src/patch_apply.h; then :; else \
		echo "patch_apply.h must not declare file-scope static variables" >&2; exit 1; \
	fi; \
	tmp=$$(mktemp -d); \
	trap 'rm -rf "$$tmp"' EXIT; \
	{ printf '%s\n' '#include <stdint.h>'; \
	  printf '%s\n' '#include "patch_apply.h"'; \
	  printf '%s\n' 'uint8_t flash_read(uint32_t a){ (void)a; return 0xffu; }'; \
	  printf '%s\n' 'void flash_write(uint32_t a, uint8_t v){ (void)a; (void)v; }'; \
	  printf '%s\n' 'static int next(void *c, uint8_t *out){ (void)c; (void)out; return 0; }'; \
	  printf '%s\n' 'int main(void){ PatchApply pa; return patch_apply_run(&pa, next, 0); }'; \
	} > "$$tmp/smoke.c"; \
	$(CC) $(CFLAGS) -Wconversion -DCORTEX_M0 -Isrc -c "$$tmp/smoke.c" -o "$$tmp/smoke.o"; \
	echo "decoder_contract=OK"

check-assets-internal:
	@scripts/verify_corpus.sh "$(CORPUS_MANIFEST)"
	@scripts/verify_corpus.sh "$(FOREIGN_MANIFEST)" foreign_assets

# qemu-based decode validation REMOVED — permanent decision (owner, 2026-07-03): too slow
# for its marginal value (the 260-pair matrix re-encoded every corpus pair just to apply it
# under emulation; ~45 CPU-min per run). Host-vs-ARM divergence is systematic when it exists,
# not pair-specific; the ARM cross-build + size/divide gate (check-arm) still compiles the
# real Thumb-1 decoder every cycle, and a one-time 260-pair qemu study (db6d693) found ZERO
# divergence. Do not reintroduce qemu legs into the gate.

# Runtime cross-check for the static caller-stack bound (check-stack / scripts/stack_bound.py).
# Runs a REAL decode under qemu-arm with the stack painted with a sentinel, then reports the
# measured high-water mark (test/stack_probe.c). This is hosted ARMv7 Thumb, not Cortex-M0+
# -O2, so the number differs from the pinned static bound by codegen; it exists to confirm a
# full decode costs a few HUNDRED bytes of caller stack (same order as the static bound), never
# thousands -- i.e. the static call-graph method missed no deep/unbounded path. Informational
# and STANDALONE (not in `make gate`, which pins the authoritative static bound); auto-skips
# without the cross-gcc / qemu-user.
check-stack-qemu-internal: hy_enc
	@set -e; \
	if ! command -v arm-linux-gnueabi-gcc >/dev/null 2>&1 || ! command -v qemu-arm >/dev/null 2>&1; then \
		echo "stack_probe=SKIPPED (need gcc-arm-linux-gnueabi + qemu-user)"; exit 0; fi; \
	tmp=$$(mktemp -d); \
	trap 'rm -rf "$$tmp"' EXIT; \
	for o in Os O2; do \
		arm-linux-gnueabi-gcc -static -mthumb -$$o -std=c99 -Wall -Wextra -Werror \
			-DCORTEX_M0 -D_POSIX_C_SOURCE=200809L -Isrc test/stack_probe.c -o "$$tmp/probe_$$o"; \
	done; \
	./hy_enc "$(FIXTURES)/v0_base" "$(FIXTURES)/v1_one_face" "$$tmp/grow.blob" >/dev/null; \
	cp "$(FIXTURES)/v0_base/watch.bin" "$$tmp/mem.bin"; \
	for o in Os O2; do \
		out=$$(qemu-arm "$$tmp/probe_$$o" "$$tmp/mem.bin" "$$tmp/grow.blob"); \
		rc=$$(printf '%s\n' "$$out" | sed -n 's/^stack_probe_rc=//p'); \
		hw=$$(printf '%s\n' "$$out" | sed -n 's/^stack_probe_highwater_bytes=//p'); \
		test "$$rc" = "1"; \
		echo "stack_probe_highwater_$$o=$$hw (hosted ARMv7 Thumb -$$o, decode=DONE)"; \
	done

check-malformed-internal: all-internal
	@FIXTURES="$(FIXTURES)" scripts/check_malformed.sh

# Synthetic edge inputs the firmware corpus never exercises (empty/tiny/equal/random/text/
# page-boundary/>384KiB-span pairs). hy_enc self-verifies every blob, so each case must
# either round-trip byte-exactly through BOTH host decoders or refuse cleanly.
check-edge-internal: all-internal
	@scripts/check_edge.sh

# Degradation / direction / row-window / big-span gate: synthetic pairs that FORCE each encoder
# path the golden set and home corpus never exercise (journal-budget degradation, OPC_CAP
# op-split, unnatural apply direction, row-window-oracle reliance, big-span journal), asserting
# the path was actually taken — not merely that the blob round-trips. Builds a D=1 variant decoder
# to prove the monotone larger-window compatibility contract. Small synthetic fixtures, fast.
check-degrade-internal: all-internal
	@scripts/check_degrade.sh

# Golden-wire regression: sha256 of eight representative blobs pinned in test-bench/golden.sha256.
# Catches size-neutral wire drift and enforces the wire freeze. On an INTENDED wire change run
# `make golden-update` and commit the manifest (plus size baselines) in the SAME commit.
check-golden-internal: hy_enc
	@FIXTURES="$(FIXTURES)" IMAGES="$(IMAGES)" scripts/check_golden.sh check

golden-update-internal: hy_enc
	@FIXTURES="$(FIXTURES)" IMAGES="$(IMAGES)" scripts/check_golden.sh update

# Model-LEVEL encoder/decoder differential test. The golden gate proves the WHOLE wire is bit-exact
# for the corpus-exercised symbol values; this proves each entropy-model PAIR is bit-exact across its
# full value space in isolation, so a future mirror bug localizes to the exact model (not "some blob's
# sha changed"). test/model_diff.c #includes the host encoder (src/patch_generate.c, no RC_V3_ENC_MAIN
# => no main, wire untouched) and drives every ENCODER model; test/model_diff_dec.c #includes the REAL
# decoder (src/patch_apply.h) unchanged and decodes with the mirror DECODER model. --gc-sections drops
# the encoder's unused planner (and its divsufsort/arm deps), so the test links from just the two TUs.
# Host-only, deterministic (fixed-seed LCG), wire-neutral, fast (<1 s).
MODEL_DIFF_SRCS := test/model_diff.c test/model_diff_dec.c
model_diff: $(MODEL_DIFF_SRCS) test/model_diff.h src/patch_generate.c $(APPLY_HDR) $(GEN_HDR)
	$(CC) $(CFLAGS) -Itest -Wno-unused-function -ffunction-sections -fdata-sections -Wl,--gc-sections \
		$(MODEL_DIFF_SRCS) -o $@

check-models-internal: model_diff
	@./model_diff

# The 256 home (from,to) pairs PLUS 34 foreign pair-directions (a second, unrelated Cortex-M0+
# lineage — CircuitPython feather_m0_express; see docs/foreign-firmware-study.md) are independent,
# so they run in ONE parallel pool across all cores via check_corpus.sh (each worker gets its own
# mktemp dir — no shared blob path, contamination-safe). The foreign set's cross-major pair is the
# single slowest job and is scheduled first so it overlaps everything (LPT), keeping the leg near
# its home-only wall. matrix_ok/full_total gate the home set; foreign_ok(=34)/foreign_total gate
# the foreign set (foreign_total ratchets vs BASE_FOREIGN_TOTAL); NVM write-safety maxima cover
# BOTH. Override parallelism with JOBS=N (defaults to nproc).
check-corpus-internal: all-internal check-assets-internal
	@set -e; \
	tmp=$$(mktemp -d); \
	trap 'rm -rf "$$tmp"' EXIT; \
	IMAGES="$(IMAGES)" FIXTURES="$(FIXTURES)" FOREIGN="$(FOREIGN)" ./check_corpus.sh $(JOBS) > "$$tmp/m.txt"; \
	cat "$$tmp/m.txt"; \
	ok=$$(sed -n 's#^matrix_ok=\([0-9][0-9]*\)/256#\1#p' "$$tmp/m.txt"); \
	full=$$(sed -n 's/^full_total=//p' "$$tmp/m.txt"); \
	fok=$$(sed -n 's#^foreign_ok=\([0-9][0-9]*\)/34#\1#p' "$$tmp/m.txt"); \
	fforeign=$$(sed -n 's/^foreign_total=//p' "$$tmp/m.txt"); \
	max_amp=$$(sed -n 's/^max_amplified=//p' "$$tmp/m.txt"); \
	max_row=$$(sed -n 's/^max_maxrowerase=//p' "$$tmp/m.txt"); \
	max_inv=$$(sed -n 's/^max_inversions=//p' "$$tmp/m.txt"); \
	one_g=$$(sed -n 's/^oneface_grow=//p' "$$tmp/m.txt"); \
	one_r=$$(sed -n 's/^oneface_revert=//p' "$$tmp/m.txt"); \
	test "$$ok" -eq 256; \
	test "$$fok" -eq 34; \
	test "$$max_amp" -eq 0; \
	test "$$max_row" -le 1; \
	test "$$max_inv" -eq 0; \
	test "$$full" -le "$(BASE_FULL_TOTAL)"; \
	test "$$fforeign" -le "$(BASE_FOREIGN_TOTAL)"; \
	test "$$one_g" -le "$(BASE_ONEFACE_GROW)"; \
	test "$$one_r" -le "$(BASE_ONEFACE_REVERT)"

# THE gate — one target, everything, hard budget <= 60 s wall on the reference machine
# (measured ~29 s at 32 jobs, incl. the folded-in foreign lineage). Builds up-front, then runs
# every leg CONCURRENTLY:
# check-assets, check (one-face grow/revert round-trip + BASE_ONEFACE_* size gates),
# check-malformed, check-edge, check-degrade, check-golden, check-models, check-decoder-contract, check-arm
# (sizes + divide policy), check-stack, and the FULL 256-pair corpus matrix + 34 foreign
# pair-directions (corpus full_total vs BASE_FULL_TOTAL, foreign_total vs BASE_FOREIGN_TOTAL,
# foreign 34/34 round-trips, NVM write-safety, journal peak — check-corpus; the
# 256-pair better/worse judging rule lives here). Wall time ~= the slowest leg
# (check-corpus), not the sum. Prints one consolidated summary with every tracked
# metric; exits nonzero if ANY gate fails and dumps the raw blocks so the offending
# metric is visible. Binaries are built BEFORE the legs fork, so concurrent sub-makes
# never race on a compile; legs run undisturbed even if sources change mid-gate.
gate-internal: all-internal model_diff
	@set -e; \
	tmp=$$(mktemp -d); trap 'rm -rf "$$tmp"' EXIT; rc=0; \
	echo "running gate (all legs concurrent): check-assets + check + check-malformed + check-edge + check-degrade + check-golden + check-models + check-decoder-contract + check-arm + check-stack + check-corpus..."; \
	$(MAKE) --no-print-directory check-assets-internal >"$$tmp/assets.txt"    2>&1 & p_assets=$$!; \
	$(MAKE) --no-print-directory check-internal >"$$tmp/c.txt"         2>&1 & p_c=$$!; \
	$(MAKE) --no-print-directory check-malformed-internal >"$$tmp/malformed.txt" 2>&1 & p_mal=$$!; \
	$(MAKE) --no-print-directory check-edge-internal >"$$tmp/e.txt"         2>&1 & p_e=$$!; \
	$(MAKE) --no-print-directory check-degrade-internal >"$$tmp/dg.txt"        2>&1 & p_dg=$$!; \
	$(MAKE) --no-print-directory check-golden-internal >"$$tmp/g.txt"         2>&1 & p_g=$$!; \
	$(MAKE) --no-print-directory check-models-internal >"$$tmp/mdl.txt"       2>&1 & p_mdl=$$!; \
	$(MAKE) --no-print-directory check-decoder-contract-internal >"$$tmp/dec_contract.txt" 2>&1 & p_dec_contract=$$!; \
	$(MAKE) --no-print-directory check-arm-internal >"$$tmp/a.txt"         2>&1 & p_a=$$!; \
	$(MAKE) --no-print-directory check-stack-internal >"$$tmp/st.txt"        2>&1 & p_st=$$!; \
	$(MAKE) --no-print-directory check-corpus-internal >"$$tmp/m.txt"         2>&1 & p_m=$$!; \
	for p in $$p_assets $$p_c $$p_mal $$p_e $$p_dg $$p_g $$p_mdl $$p_dec_contract $$p_a $$p_st $$p_m; do \
		wait $$p || rc=1; \
	done; \
	echo "==================== A1 GATE ========================="; \
	sed -n 's/^corpus_assets=/corpus assets          : /p' "$$tmp/assets.txt"; \
	sed -n 's/^foreign_assets=/foreign assets         : /p' "$$tmp/assets.txt"; \
	sed -n 's/^malformed_rejects=/malformed rejects      : /p' "$$tmp/malformed.txt"; \
	awk -F= '/^edge_cases=/{c=$$2}/^edge_roundtrips=/{r=$$2}/^edge_refusals=/{f=$$2}END{if(c!="")printf "edge inputs             : %s round-trip + %s refused of %s\n",r,f,c}' "$$tmp/e.txt"; \
	sed -n 's/^golden_wire=/golden wire             : /p' "$$tmp/g.txt"; \
	sed -n 's/^model_diff=/model diff              : /p' "$$tmp/mdl.txt"; \
	sed -n 's/^decoder_contract=/decoder contract        : /p' "$$tmp/dec_contract.txt"; \
	awk -F= '/^degrade_journal_peak=/{j=$$2}/^degrade_opc_splits=/{o=$$2}/^degrade_direction=/{d=$$2}/^degrade_rowwindow=/{w=$$2}/^degrade_bigspan=/{f=$$2}/^degrade_cases=/{c=$$2}END{if(c!="")printf "degradation paths       : journal_peak=%s opc_splits=%s dir=%s rowwin=%s bigspan=%s (%s cases)\n",j,o,d,w,f,c}' "$$tmp/dg.txt"; \
	awk 'NR==2{printf "ARM   text / data / bss  : %s / %s / %s   (.bss cap 12288)\n",$$1,$$2,$$3}' "$$tmp/a.txt"; \
	sed -n 's/^soft_div_calls=/ARM   soft-divide calls  : /p' "$$tmp/a.txt"; \
	awk -F= '/^stack_bound_bytes=/{b=$$2}/^stack_ceiling_o2=/{c=$$2}END{if(b!="")printf "caller-stack bound       : %s B  (gcc -O2, ceiling %s, excl. externs)\n",b,c}' "$$tmp/st.txt"; \
	sed -n 's/^matrix_ok=/matrix round-trips      : /p' "$$tmp/m.txt"; \
	sed -n 's/^full_total=/corpus full_total       : /p' "$$tmp/m.txt"; \
	sed -n 's/^foreign_ok=/foreign round-trips     : /p' "$$tmp/m.txt"; \
	sed -n 's/^foreign_total=/foreign full_total      : /p' "$$tmp/m.txt"; \
	sed -n 's/^oneface_grow=/one-face grow            : /p' "$$tmp/c.txt"; \
	sed -n 's/^oneface_revert=/one-face revert          : /p' "$$tmp/c.txt"; \
	sed -n 's/^max_amplified=/NVM rows amplified       : /p' "$$tmp/m.txt"; \
	sed -n 's/^max_maxrowerase=/NVM max erases-per-row   : /p' "$$tmp/m.txt"; \
	sed -n 's/^max_inversions=/NVM frontier inversions  : /p' "$$tmp/m.txt"; \
	sed -n 's/^max_journal=/journal peak slots      : /p' "$$tmp/m.txt"; \
	if [ $$rc = 0 ]; then \
		echo "robustness check         : PASS (round-trip both dirs + corrupt/truncated/CRC rejects)"; \
		echo "RESULT                   : ALL GATES PASS"; \
	else \
		echo "RESULT                   : *** GATE FAILED (rc=$$rc) ***"; \
		echo "------------------ check-assets ------------------"; cat "$$tmp/assets.txt"; \
		echo "------------------ check ------------------";        cat "$$tmp/c.txt"; \
		echo "------------------ check-malformed ------------------"; cat "$$tmp/malformed.txt"; \
		echo "------------------ check-edge ------------------";   cat "$$tmp/e.txt"; \
		echo "------------------ check-degrade ------------------"; cat "$$tmp/dg.txt"; \
		echo "------------------ check-golden ------------------"; cat "$$tmp/g.txt"; \
		echo "------------------ check-models ------------------"; cat "$$tmp/mdl.txt"; \
		echo "------------------ check-decoder-contract ------------------"; cat "$$tmp/dec_contract.txt"; \
		echo "------------------ check-arm ------------------";    cat "$$tmp/a.txt"; \
		echo "------------------ check-stack ------------------";  cat "$$tmp/st.txt"; \
		if [ -s "$$tmp/m.txt" ]; then \
			echo "------------------ check-corpus ------------------"; cat "$$tmp/m.txt"; \
		fi; \
	fi; \
	echo "====================================================="; \
	exit $$rc

# Static-analysis leg: gcc -fanalyzer over BOTH first-party TUs (encoder + decoder + arm + selfcheck)
# with a curated flag set; clean baseline (exits nonzero on any NEW finding). STANDALONE (version-
# fragile + ~16 s), NOT in `make gate`; auto-skips where gcc -fanalyzer is unavailable.
check-analyze-internal:
	@scripts/check_analyze.sh

clean-internal:
	rm -f hy_enc hy_dec model_diff
