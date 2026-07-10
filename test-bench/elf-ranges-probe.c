/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 *
 * Synthetic ELF32 oracle for file-backed symbol range extraction.
 */
#include "enc_internal.h"

#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)

enum {
    ELF_SIZE = 0x240,
    PHOFF = 52,
    SHOFF = 0x100,
    DATA_OFF = 0x80,
    SYM_OFF = 0x180
};

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void valid_elf(uint8_t elf[ELF_SIZE]) {
    uint8_t *ph, *data, *sym;
    memset(elf, 0, ELF_SIZE);
    memcpy(elf, "\177" "ELF", 4);
    elf[4] = 1;
    elf[5] = 1;
    put32(elf + 28, PHOFF);
    put32(elf + 32, SHOFF);
    put16(elf + 40, 52);
    put16(elf + 42, 32);
    put16(elf + 44, 1);
    put16(elf + 46, 40);
    put16(elf + 48, 3);

    ph = elf + PHOFF;
    put32(ph, 1);               /* PT_LOAD */
    put32(ph + 4, DATA_OFF);
    put32(ph + 8, 0x1000);
    put32(ph + 12, 0x2000);
    put32(ph + 16, 0x40);
    put32(ph + 20, 0x40);

    data = elf + SHOFF + 40;
    put32(data + 4, 1);         /* SHT_PROGBITS */
    put32(data + 8, 2);         /* SHF_ALLOC */
    put32(data + 12, 0x1000);
    put32(data + 16, DATA_OFF);
    put32(data + 20, 0x40);

    sym = elf + SHOFF + 80;
    put32(sym + 4, 2);          /* SHT_SYMTAB */
    put32(sym + 16, SYM_OFF);
    put32(sym + 20, 32);
    put32(sym + 36, 16);
    sym = elf + SYM_OFF + 16;
    put32(sym + 4, 0x1010);
    put32(sym + 8, 8);
    sym[12] = 1;                /* STT_OBJECT */
    put16(sym + 14, 1);
}

static int expect(const char *path, uint8_t elf[ELF_SIZE], const Buf *bin,
                  uint32_t begin, uint32_t end) {
    write_file(path, elf, ELF_SIZE);
    Ranges r = elf_ranges(path, bin, "probe");
    CHECK(r.data_off_begin == begin);
    CHECK(r.data_off_end == end);
    return 0;
}

int main(int argc, char **argv) {
    uint8_t elf[ELF_SIZE], image[0x40] = {0};
    Buf bin = {image, sizeof(image), sizeof(image)};
    int r;
    CHECK(argc == 2);

    valid_elf(elf);
    CHECK(expect(argv[1], elf, &bin, 0x10, 0x18) == 0);

    valid_elf(elf); put32(elf + SHOFF + 40 + 8, 0);
    CHECK(expect(argv[1], elf, &bin, 0, 0) == 0);       /* unallocated section */

    valid_elf(elf); put32(elf + SHOFF + 40 + 4, 8);
    CHECK(expect(argv[1], elf, &bin, 0, 0) == 0);       /* SHT_NOBITS */

    valid_elf(elf); put32(elf + SHOFF + 40 + 16, 0x220);
    CHECK(expect(argv[1], elf, &bin, 0, 0) == 0);       /* section bytes absent */

    valid_elf(elf); put32(elf + SYM_OFF + 16 + 4, 0x103c);
    CHECK(expect(argv[1], elf, &bin, 0, 0) == 0);       /* symbol extent escapes section */

    valid_elf(elf); put32(elf + PHOFF + 4, 0x220);
    CHECK(expect(argv[1], elf, &bin, 0, 0) == 0);       /* segment bytes absent from ELF */

    valid_elf(elf); put32(elf + PHOFF + 16, 0x48);
    CHECK(expect(argv[1], elf, &bin, 0, 0) == 0);       /* full segment absent from bin */

    valid_elf(elf); put32(elf + PHOFF + 8, 0x1020);
    CHECK(expect(argv[1], elf, &bin, 0, 0) == 0);       /* symbol outside PT_LOAD */

    valid_elf(elf); put32(elf + SHOFF + 40 + 16, DATA_OFF + 8);
    CHECK(expect(argv[1], elf, &bin, 0, 0) == 0);       /* section/segment file mismatch */

    valid_elf(elf); put32(elf + SHOFF + 80 + 20, 16);
    CHECK(expect(argv[1], elf, &bin, 0, 0) == 0);       /* no symbol ranges */

    valid_elf(elf); put32(elf + 28, 0); put16(elf + 44, 0);
    CHECK(expect(argv[1], elf, &bin, 0, 0) == 0);       /* valid ELF with no phdrs */

    valid_elf(elf); put32(elf + 32, 0); put16(elf + 48, 0);
    r = expect(argv[1], elf, &bin, 0, 0);               /* valid ELF with no sections */
    return r;
}
