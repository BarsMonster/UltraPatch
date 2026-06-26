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

.PHONY: all clean check

all: hy_enc hy_dec

hy_enc: rc_v3_enc.c arm_cortex_m4.c $(DIVSUF)
	$(CC) $(CFLAGS) -DRC_V3_ENC_MAIN $^ -o $@

hy_dec: rc_v3.c flash_nvm.c
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=200809L -DRC_V3_MAIN -DRC_V3_NVM $^ -o $@

check: all
	@tmp=$$(mktemp -d); \
	trap 'rm -rf "$$tmp"' EXIT; \
	cp ../fixtures/v0_base/watch.bin "$$tmp/mem.bin"; \
	./hy_enc ../fixtures/v0_base ../fixtures/v1_one_face "$$tmp/grow.blob" 10; \
	./hy_dec "$$tmp/mem.bin" "$$tmp/grow.blob" 1 >/dev/null; \
	cmp "$$tmp/mem.bin" ../fixtures/v1_one_face/watch.bin; \
	cp ../fixtures/v1_one_face/watch.bin "$$tmp/mem.bin"; \
	./hy_enc ../fixtures/v1_one_face ../fixtures/v0_base "$$tmp/revert.blob" 10; \
	./hy_dec "$$tmp/mem.bin" "$$tmp/revert.blob" 1 >/dev/null; \
	cmp "$$tmp/mem.bin" ../fixtures/v0_base/watch.bin; \
	wc -c "$$tmp/grow.blob" "$$tmp/revert.blob"

clean:
	rm -f hy_enc hy_dec
