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
CFLAGS += -Ilibdivsufsort
CFLAGS += $(CFLAGS_EXTRA)

DIVSUF := libdivsufsort/divsufsort.c

BASE_FULL_TOTAL ?= 4595908
BASE_NONSELF_TOTAL ?= 4595377
BASE_ONEFACE_GROW ?= 873
BASE_ONEFACE_REVERT ?= 582
BASE_ARM_TEXT ?= 5159
BASE_ARM_DATA ?= 0
BASE_ARM_BSS ?= 11664
BASE_ARM_SOFT_DIV ?= 1

.PHONY: all clean check check-arm check-corpus

all: hy_enc hy_dec

hy_enc: rc_v3_enc.c arm_cortex_m4.c $(DIVSUF)
	$(CC) $(CFLAGS) -DRC_V3_ENC_MAIN $^ -o $@

hy_dec: rc_v3.c flash_nvm.c
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=200809L -DRC_V3_MAIN -DRC_V3_NVM $^ -o $@

check: all
	@set -e; \
	tmp=$$(mktemp -d); \
	trap 'rm -rf "$$tmp"' EXIT; \
	cp ../fixtures/v0_base/watch.bin "$$tmp/mem.bin"; \
	./hy_enc ../fixtures/v0_base ../fixtures/v1_one_face "$$tmp/grow.blob" 10; \
	./hy_dec "$$tmp/mem.bin" "$$tmp/grow.blob" 1 >/dev/null; \
	cmp "$$tmp/mem.bin" ../fixtures/v1_one_face/watch.bin; \
	cp ../fixtures/v1_one_face/watch.bin "$$tmp/mem.bin"; \
	./hy_enc ../fixtures/v1_one_face ../fixtures/v0_base "$$tmp/revert.blob" 10; \
	./hy_dec "$$tmp/mem.bin" "$$tmp/revert.blob" 1 >/dev/null; \
	cmp "$$tmp/mem.bin" ../fixtures/v0_base/watch.bin; \
	grow_sz=$$(wc -c < "$$tmp/grow.blob"); \
	revert_sz=$$(wc -c < "$$tmp/revert.blob"); \
	wc -c "$$tmp/grow.blob" "$$tmp/revert.blob"; \
	test "$$grow_sz" -le "$(BASE_ONEFACE_GROW)"; \
	test "$$revert_sz" -le "$(BASE_ONEFACE_REVERT)"; \
	cp "$$tmp/grow.blob" "$$tmp/bad.blob"; \
	printf '\377' | dd of="$$tmp/bad.blob" bs=1 seek=40 count=1 conv=notrunc >/dev/null 2>&1; \
	cp ../fixtures/v0_base/watch.bin "$$tmp/mem.bin"; \
	if ./hy_dec "$$tmp/mem.bin" "$$tmp/bad.blob" 1 >/dev/null 2>/dev/null; then echo "corrupt body accepted"; exit 1; fi; \
	cmp "$$tmp/mem.bin" ../fixtures/v0_base/watch.bin; \
	head -c -1 "$$tmp/grow.blob" > "$$tmp/trunc.blob"; \
	cp ../fixtures/v0_base/watch.bin "$$tmp/mem.bin"; \
	if ./hy_dec "$$tmp/mem.bin" "$$tmp/trunc.blob" 1 >/dev/null 2>/dev/null; then echo "truncated blob accepted"; exit 1; fi; \
	cmp "$$tmp/mem.bin" ../fixtures/v0_base/watch.bin; \
	cp "$$tmp/grow.blob" "$$tmp/bad_from.blob"; \
	printf '\000' | dd of="$$tmp/bad_from.blob" bs=1 seek=0 count=1 conv=notrunc >/dev/null 2>&1; \
	cp ../fixtures/v0_base/watch.bin "$$tmp/mem.bin"; \
	if ./hy_dec "$$tmp/mem.bin" "$$tmp/bad_from.blob" 1 >/dev/null 2>/dev/null; then echo "bad from CRC accepted"; exit 1; fi; \
	cmp "$$tmp/mem.bin" ../fixtures/v0_base/watch.bin; \
	cp "$$tmp/grow.blob" "$$tmp/bad_to.blob"; \
	last=$$((grow_sz - 1)); \
	printf '\000' | dd of="$$tmp/bad_to.blob" bs=1 seek="$$last" count=1 conv=notrunc >/dev/null 2>&1; \
	cp ../fixtures/v0_base/watch.bin "$$tmp/mem.bin"; \
	if ./hy_dec "$$tmp/mem.bin" "$$tmp/bad_to.blob" 1 >/dev/null 2>/dev/null; then echo "bad to CRC accepted"; exit 1; fi; \
	cmp "$$tmp/mem.bin" ../fixtures/v0_base/watch.bin

check-arm:
	@set -e; \
	tmp=$$(mktemp -d); \
	trap 'rm -rf "$$tmp"' EXIT; \
	arm-none-eabi-gcc -mcpu=cortex-m0plus -mthumb -Os -DRC_V3_ARM -I . -c rc_v3.c -o "$$tmp/rc_v3_arm.o"; \
	size_out=$$(arm-none-eabi-size "$$tmp/rc_v3_arm.o"); \
	printf '%s\n' "$$size_out"; \
	set -- $$(printf '%s\n' "$$size_out" | awk 'NR==2 { print $$1, $$2, $$3 }'); \
	test "$$1" -le "$(BASE_ARM_TEXT)"; \
	test "$$2" -le "$(BASE_ARM_DATA)"; \
	test "$$3" -le "$(BASE_ARM_BSS)"; \
	arm-none-eabi-objdump -d "$$tmp/rc_v3_arm.o" > "$$tmp/rc_v3_arm.dump"; \
	if grep -Eq '\b(udiv|sdiv)\b' "$$tmp/rc_v3_arm.dump"; then \
		echo "hardware divide instruction found"; exit 1; \
	fi; \
	soft=$$(grep -Ec '__aeabi_.*div|__aeabi_.*mod' "$$tmp/rc_v3_arm.dump" || true); \
	echo "soft_div_calls=$$soft"; \
	test "$$soft" -eq "$(BASE_ARM_SOFT_DIV)"

# The 256 (from,to) pairs are independent, so the matrix runs in parallel across all cores via
# check_corpus.sh (each worker gets its own mktemp dir — no shared blob path, contamination-safe).
# It prints the same nine metric lines the old serial loop did; the BASE_* size/safety gates stay
# asserted here. Override parallelism with JOBS=N (defaults to nproc).
check-corpus: all
	@set -e; \
	tmp=$$(mktemp -d); \
	trap 'rm -rf "$$tmp"' EXIT; \
	./check_corpus.sh 10 $(JOBS) > "$$tmp/m.txt"; \
	cat "$$tmp/m.txt"; \
	ok=$$(sed -n 's#^matrix_ok=\([0-9][0-9]*\)/256#\1#p' "$$tmp/m.txt"); \
	full=$$(sed -n 's/^full_total=//p' "$$tmp/m.txt"); \
	non=$$(sed -n 's/^nonself_total=//p' "$$tmp/m.txt"); \
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
	test "$$non" -le "$(BASE_NONSELF_TOTAL)"; \
	test "$$one_g" -le "$(BASE_ONEFACE_GROW)"; \
	test "$$one_r" -le "$(BASE_ONEFACE_REVERT)"

clean:
	rm -f hy_enc hy_dec
