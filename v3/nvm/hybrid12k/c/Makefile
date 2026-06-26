CC = $(CROSS_COMPILE)gcc
AR = $(CROSS_COMPILE)ar

# Install prefix.
PREFIX ?= /usr/local

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
CFLAGS += -Iheatshrink
CFLAGS += $(CFLAGS_EXTRA)

SRC += heatshrink/heatshrink_decoder.c
SRC += detools.c
SRC += main.c

ROOT ?= ../../../..
DIVSUF := $(ROOT)/detools-dev/detools/libdivsufsort
A1_CFLAGS := $(CFLAGS) -I. -I$(DIVSUF)

.PHONY: all legacy a1

all: a1

a1: hy_enc hy_dec

legacy: detools

detools:
	$(CC) $(CFLAGS) $(SRC) -llzma -o detools

hy_enc: rc_v3_enc.c arm_cortex_m4.c $(DIVSUF)/divsufsort.c
	$(CC) $(A1_CFLAGS) -DRC_V3_ENC_MAIN $^ -o $@

hy_dec: rc_v3.c flash_nvm.c
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=200809L -DRC_V3_MAIN -DRC_V3_NVM -I. $^ -o $@

clean:
	rm -f detools hy_enc hy_dec detools.o libdetools.a esp8266-20190125-v1.10.bin

test: all
	./detools apply_patch \
	    ../tests/files/micropython/esp8266-20180511-v1.9.4.bin \
	    ../tests/files/micropython/esp8266-20180511-v1.9.4--20190125-v1.10-heatshrink.patch \
	    esp8266-20190125-v1.10.bin
	cmp \
	    esp8266-20190125-v1.10.bin \
	    ../tests/files/micropython/esp8266-20190125-v1.10.bin

library:
	$(CC) $(CFLAGS) detools.c -c -o detools.o
	$(AR) cr libdetools.a detools.o

install:
	mkdir -p $(PREFIX)/include
	install -c -m 644 detools.h $(PREFIX)/include
	mkdir -p $(PREFIX)/lib
	install -c -m 644 libdetools.a $(PREFIX)/lib
