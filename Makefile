# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

CC = $(CROSS_COMPILE)gcc

OPT ?= -O2
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
APPLY_HDR := src/patch_apply.h src/rc_models.h
GEN_HDR := src/rc_models.h src/arm_cortex_m4.h
ENC_SRCS := src/patch_generate.c src/arm_cortex_m4.c $(DIVSUF)
DEC_SRCS := src/patch_apply_demo.c

FIXTURES ?= test-bench/fixtures
IMAGES ?= test-bench/images
CORPUS_MANIFEST ?= test-bench/corpus.sha256

BASE_FULL_TOTAL ?= 4210683
BASE_ONEFACE_GROW ?= 590
BASE_ONEFACE_REVERT ?= 305
BASE_ARM_TEXT ?= 5528
BASE_ARM_DATA ?= 0
BASE_ARM_BSS ?= 10864
BASE_ARM_SOFT_DIV ?= 1

.PHONY: all clean check check-arm check-assets check-malformed check-corpus gate

all: hy_enc hy_dec

hy_enc: $(ENC_SRCS) $(GEN_HDR)
	$(CC) $(CFLAGS) -DRC_V3_ENC_MAIN $(ENC_SRCS) -o $@

hy_dec: $(DEC_SRCS) $(APPLY_HDR)
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=200809L $(DEC_SRCS) -o $@

check: all
	@set -e; \
	tmp=$$(mktemp -d); \
	trap 'rm -rf "$$tmp"' EXIT; \
	cp "$(FIXTURES)/v0_base/watch.bin" "$$tmp/mem.bin"; \
	./hy_enc "$(FIXTURES)/v0_base" "$(FIXTURES)/v1_one_face" "$$tmp/grow.blob" 10; \
	./hy_dec "$$tmp/mem.bin" "$$tmp/grow.blob" 1 >/dev/null; \
	cmp "$$tmp/mem.bin" "$(FIXTURES)/v1_one_face/watch.bin"; \
	cp "$(FIXTURES)/v1_one_face/watch.bin" "$$tmp/mem.bin"; \
	./hy_enc "$(FIXTURES)/v1_one_face" "$(FIXTURES)/v0_base" "$$tmp/revert.blob" 10; \
	./hy_dec "$$tmp/mem.bin" "$$tmp/revert.blob" 1 >/dev/null; \
	cmp "$$tmp/mem.bin" "$(FIXTURES)/v0_base/watch.bin"; \
	grow_sz=$$(wc -c < "$$tmp/grow.blob"); \
	revert_sz=$$(wc -c < "$$tmp/revert.blob"); \
	wc -c "$$tmp/grow.blob" "$$tmp/revert.blob"; \
	test "$$grow_sz" -le "$(BASE_ONEFACE_GROW)"; \
	test "$$revert_sz" -le "$(BASE_ONEFACE_REVERT)"; \
	cp "$$tmp/grow.blob" "$$tmp/bad.blob"; \
	printf '\377' | dd of="$$tmp/bad.blob" bs=1 seek=40 count=1 conv=notrunc >/dev/null 2>&1; \
	cp "$(FIXTURES)/v0_base/watch.bin" "$$tmp/mem.bin"; \
	if ./hy_dec "$$tmp/mem.bin" "$$tmp/bad.blob" 1 >/dev/null 2>/dev/null; then echo "corrupt body accepted"; exit 1; fi; \
	cmp "$$tmp/mem.bin" "$(FIXTURES)/v0_base/watch.bin"; \
	head -c -1 "$$tmp/grow.blob" > "$$tmp/trunc.blob"; \
	cp "$(FIXTURES)/v0_base/watch.bin" "$$tmp/mem.bin"; \
	if ./hy_dec "$$tmp/mem.bin" "$$tmp/trunc.blob" 1 >/dev/null 2>/dev/null; then echo "truncated blob accepted"; exit 1; fi; \
	cmp "$$tmp/mem.bin" "$(FIXTURES)/v0_base/watch.bin"; \
	cp "$$tmp/grow.blob" "$$tmp/bad_from.blob"; \
	printf '\000' | dd of="$$tmp/bad_from.blob" bs=1 seek=0 count=1 conv=notrunc >/dev/null 2>&1; \
	cp "$(FIXTURES)/v0_base/watch.bin" "$$tmp/mem.bin"; \
	if ./hy_dec "$$tmp/mem.bin" "$$tmp/bad_from.blob" 1 >/dev/null 2>/dev/null; then echo "bad from CRC accepted"; exit 1; fi; \
	cmp "$$tmp/mem.bin" "$(FIXTURES)/v0_base/watch.bin"; \
	cp "$$tmp/grow.blob" "$$tmp/bad_to.blob"; \
	last=$$((grow_sz - 1)); \
	printf '\000' | dd of="$$tmp/bad_to.blob" bs=1 seek="$$last" count=1 conv=notrunc >/dev/null 2>&1; \
	cp "$(FIXTURES)/v0_base/watch.bin" "$$tmp/mem.bin"; \
	if ./hy_dec "$$tmp/mem.bin" "$$tmp/bad_to.blob" 1 >/dev/null 2>/dev/null; then echo "bad to CRC accepted"; exit 1; fi; \
	cmp "$$tmp/mem.bin" "$(FIXTURES)/v0_base/watch.bin"

check-arm:
	@set -e; \
	tmp=$$(mktemp -d); \
	trap 'rm -rf "$$tmp"' EXIT; \
	arm-none-eabi-gcc -mcpu=cortex-m0plus -mthumb -Os -DRC_V3_ARM -I src -x c -c src/patch_apply.h -o "$$tmp/patch_apply_arm.o"; \
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

check-assets:
	@scripts/verify_corpus.sh "$(CORPUS_MANIFEST)"

check-malformed: all
	@FIXTURES="$(FIXTURES)" scripts/check_malformed.sh 10

# The 256 (from,to) pairs are independent, so the matrix runs in parallel across all cores via
# check_corpus.sh (each worker gets its own mktemp dir — no shared blob path, contamination-safe).
# It prints the same nine metric lines the old serial loop did; the BASE_* size/safety gates stay
# asserted here. Override parallelism with JOBS=N (defaults to nproc).
check-corpus: all check-assets
	@set -e; \
	tmp=$$(mktemp -d); \
	trap 'rm -rf "$$tmp"' EXIT; \
	IMAGES="$(IMAGES)" FIXTURES="$(FIXTURES)" ./check_corpus.sh 10 $(JOBS) > "$$tmp/m.txt"; \
	cat "$$tmp/m.txt"; \
	ok=$$(sed -n 's#^matrix_ok=\([0-9][0-9]*\)/256#\1#p' "$$tmp/m.txt"); \
	full=$$(sed -n 's/^full_total=//p' "$$tmp/m.txt"); \
	max_amp=$$(sed -n 's/^max_amplified=//p' "$$tmp/m.txt"); \
	max_row=$$(sed -n 's/^max_maxrowerase=//p' "$$tmp/m.txt"); \
	max_inv=$$(sed -n 's/^max_inversions=//p' "$$tmp/m.txt"); \
	one_g=$$(sed -n 's/^oneface_grow=//p' "$$tmp/m.txt"); \
	one_r=$$(sed -n 's/^oneface_revert=//p' "$$tmp/m.txt"); \
	test "$$ok" -eq 256; \
	test "$$max_amp" -eq 0; \
	test "$$max_row" -le 1; \
	test "$$max_inv" -eq 0; \
	test "$$full" -le "$(BASE_FULL_TOTAL)"; \
	test "$$one_g" -le "$(BASE_ONEFACE_GROW)"; \
	test "$$one_r" -le "$(BASE_ONEFACE_REVERT)"

# ONE command for the full gate: builds, verifies corpus assets, and runs the host, malformed,
# ARM, and corpus gates,
# and prints a single consolidated summary with every metric the project tracks — ARM .text/.data/
# .bss + divide policy, corpus full total, the real one-face update (grow/revert), matrix
# round-trips, NVM write-safety, and journal peak. Exits nonzero if ANY gate fails, and on failure
# dumps the raw blocks so the offending metric is visible. Dominated by check-corpus, whose
# runtime scales with the configured JOBS value.
gate: all
	@set -e; \
	tmp=$$(mktemp -d); trap 'rm -rf "$$tmp"' EXIT; rc=0; \
	echo "running full gate: check-assets + check + check-malformed + check-arm + check-corpus..."; \
	$(MAKE) --no-print-directory check-assets >"$$tmp/assets.txt" 2>&1 || rc=1; \
	$(MAKE) --no-print-directory check       >"$$tmp/c.txt" 2>&1 || rc=1; \
	$(MAKE) --no-print-directory check-malformed >"$$tmp/malformed.txt" 2>&1 || rc=1; \
	$(MAKE) --no-print-directory check-arm    >"$$tmp/a.txt" 2>&1 || rc=1; \
	$(MAKE) --no-print-directory check-corpus >"$$tmp/m.txt" 2>&1 || rc=1; \
	echo "==================== A1 FULL GATE ===================="; \
	sed -n 's/^corpus_assets=/corpus assets          : /p' "$$tmp/assets.txt"; \
	sed -n 's/^malformed_rejects=/malformed rejects      : /p' "$$tmp/malformed.txt"; \
	awk 'NR==2{printf "ARM   text / data / bss  : %s / %s / %s   (.bss cap 12288)\n",$$1,$$2,$$3}' "$$tmp/a.txt"; \
	sed -n 's/^soft_div_calls=/ARM   soft-divide calls  : /p' "$$tmp/a.txt"; \
	sed -n 's/^matrix_ok=/matrix round-trips      : /p' "$$tmp/m.txt"; \
	sed -n 's/^full_total=/corpus full_total       : /p' "$$tmp/m.txt"; \
	sed -n 's/^oneface_grow=/one-face grow            : /p' "$$tmp/m.txt"; \
	sed -n 's/^oneface_revert=/one-face revert          : /p' "$$tmp/m.txt"; \
	sed -n 's/^max_amplified=/NVM rows amplified       : /p' "$$tmp/m.txt"; \
	sed -n 's/^max_maxrowerase=/NVM max erases-per-row   : /p' "$$tmp/m.txt"; \
	sed -n 's/^max_inversions=/NVM frontier inversions  : /p' "$$tmp/m.txt"; \
	sed -n 's/^max_journal=/journal peak slots      : /p' "$$tmp/m.txt"; \
	if [ $$rc = 0 ]; then \
		echo "correctness fuzz         : PASS (round-trip both dirs + corrupt/truncated/CRC rejects)"; \
		echo "RESULT                   : ALL GATES PASS"; \
	else \
		echo "RESULT                   : *** GATE FAILED (rc=$$rc) ***"; \
		echo "------------------ check-assets ------------------"; cat "$$tmp/assets.txt"; \
		echo "------------------ check ------------------";        cat "$$tmp/c.txt"; \
		echo "------------------ check-malformed ------------------"; cat "$$tmp/malformed.txt"; \
		echo "------------------ check-arm ------------------";    cat "$$tmp/a.txt"; \
		echo "------------------ check-corpus ------------------"; cat "$$tmp/m.txt"; \
	fi; \
	echo "====================================================="; \
	exit $$rc

clean:
	rm -f hy_enc hy_dec
