#!/usr/bin/env python3
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT
#
# Unpack a Microsoft UF2 firmware container into a flat application image.
#
# CircuitPython 10.x ships as UF2 (bootloader flashing format), not raw .bin.
# The application lives above the bootloader at flash base 0x2000; this tool
# reassembles the main-flash payload blocks into a contiguous image whose byte 0
# is the content at 0x2000 (erased-flash gaps 0xFF-filled), matching the raw
# .bin layout of the pre-10.x releases so both lineages feed hy_enc identically.
#
# Usage: uf2_unpack.py <in.uf2> <out.bin> [app_base_hex]   (app_base default 0x2000)

import sys
import struct

UF2_MAGIC0 = 0x0A324655
UF2_MAGIC1 = 0x9E5D5157
UF2_FLAG_NOT_MAIN_FLASH = 0x00000001


def unpack(path, app_base=0x2000):
    data = open(path, "rb").read()
    if len(data) % 512 != 0:
        raise ValueError(f"{path}: length {len(data)} is not a multiple of 512")
    blocks = {}
    hi = 0
    for off in range(0, len(data), 512):
        blk = data[off:off + 512]
        magic0, magic1, flags, addr, plsize, _blkno, _numblk, _fam = \
            struct.unpack("<8I", blk[:32])
        if magic0 != UF2_MAGIC0 or magic1 != UF2_MAGIC1:
            raise ValueError(f"{path}: bad UF2 magic at block {off // 512}")
        if flags & UF2_FLAG_NOT_MAIN_FLASH:
            continue
        if addr < app_base:            # bootloader region below the app — drop
            continue
        blocks[addr] = blk[32:32 + plsize]
        hi = max(hi, addr + plsize)
    if not blocks:
        raise ValueError(f"{path}: no main-flash blocks at or above 0x{app_base:x}")
    img = bytearray(b"\xff" * (hi - app_base))
    for addr, payload in blocks.items():
        img[addr - app_base:addr - app_base + len(payload)] = payload
    return bytes(img)


def main(argv):
    if not 3 <= len(argv) <= 4:
        sys.stderr.write("usage: uf2_unpack.py <in.uf2> <out.bin> [app_base_hex]\n")
        return 2
    app_base = int(argv[3], 16) if len(argv) == 4 else 0x2000
    img = unpack(argv[1], app_base)
    open(argv[2], "wb").write(img)
    sys.stderr.write(f"{argv[2]}: {len(img)} bytes (app base 0x{app_base:x})\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
