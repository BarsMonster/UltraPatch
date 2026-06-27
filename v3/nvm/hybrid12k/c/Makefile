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

BASE_FULL_TOTAL ?= 4669478
BASE_NONSELF_TOTAL ?= 4668931
BASE_ONEFACE_GROW ?= 875
BASE_ONEFACE_REVERT ?= 588
BASE_ARM_TEXT ?= 5343
BASE_ARM_DATA ?= 0
BASE_ARM_BSS ?= 11808
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

check-corpus: all
	@set -e; \
	tmp=$$(mktemp -d); \
	trap 'rm -rf "$$tmp"' EXIT; \
	full=0; non=0; ok=0; max_j=0; max_amp=0; max_row=0; max_inv=0; \
	for from in ../images/img_*; do \
		for to in ../images/img_*; do \
			blob="$$tmp/patch.blob"; mem="$$tmp/mem.bin"; log="$$tmp/dec.log"; \
			./hy_enc "$$from" "$$to" "$$blob" 10 >/dev/null; \
			sz=$$(wc -c < "$$blob"); full=$$((full + sz)); \
			if [ "$$from" != "$$to" ]; then non=$$((non + sz)); fi; \
			cp "$$from/watch.bin" "$$mem"; \
			./hy_dec "$$mem" "$$blob" 1 >/dev/null 2>"$$log"; \
			cmp -s "$$mem" "$$to/watch.bin"; \
			ok=$$((ok + 1)); \
			j=$$(sed -n 's/.*journal_used=\([0-9][0-9]*\).*/\1/p' "$$log"); \
			vals=$$(sed -n 's/.*amplified=\([0-9][0-9]*\).*maxrowerase=\([0-9][0-9]*\).*inversions=\([-0-9][0-9]*\).*/\1 \2 \3/p' "$$log"); \
			if [ -z "$$j" ] || [ -z "$$vals" ]; then echo "failed to parse decoder metrics for $$from -> $$to"; cat "$$log"; exit 1; fi; \
			set -- $$vals; amp=$$1; row=$$2; inv=$$3; \
			if [ "$$j" -gt "$$max_j" ]; then max_j=$$j; fi; \
			if [ "$$amp" -gt "$$max_amp" ]; then max_amp=$$amp; fi; \
			if [ "$$row" -gt "$$max_row" ]; then max_row=$$row; fi; \
			if [ "$$inv" -gt "$$max_inv" ]; then max_inv=$$inv; fi; \
		done; \
	done; \
	./hy_enc ../fixtures/v0_base ../fixtures/v1_one_face "$$tmp/grow.blob" 10 >/dev/null; \
	./hy_enc ../fixtures/v1_one_face ../fixtures/v0_base "$$tmp/revert.blob" 10 >/dev/null; \
	one_g=$$(wc -c < "$$tmp/grow.blob"); one_r=$$(wc -c < "$$tmp/revert.blob"); \
	echo "matrix_ok=$$ok/256"; \
	echo "full_total=$$full"; \
	echo "nonself_total=$$non"; \
	echo "max_journal=$$max_j"; \
	echo "max_amplified=$$max_amp"; \
	echo "max_maxrowerase=$$max_row"; \
	echo "max_inversions=$$max_inv"; \
	echo "oneface_grow=$$one_g"; \
	echo "oneface_revert=$$one_r"; \
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
