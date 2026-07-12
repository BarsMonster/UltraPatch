# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

# Single source of truth for the patch wire envelope's uLEB header layout, shared by the gate
# scripts (check_malformed.sh, check_degrade.sh). The device decoder is authoritative
# (src/patch_apply.h decode_header); this mirrors ONLY the header uLEB walk those gates need to
# synthesize malformed fixtures and to detect the direction-flip overlong marker.
#
# Envelope: (CRC32(from)^PATCH_WIRE_VERSION)[4] | CRC32(to)[4] | <uLEB fields...>
#   Header uLEB fields (0-based, after the 8-byte CRC pair) for a DESCENDING grow patch are:
#     0 from_size | 1 zz(size-delta) | 2 zz(fp_end) | 3 body_len
#   fp_end ships iff DESCENDING, fp_start iff ASCENDING (mutually exclusive), so a descending
#   grow blob carries exactly these four header ulebs before the compressed body.
# The overlong marker is a multi-byte uLEB whose final byte is 0x00 (mirrors rc_uleb_overlong in
# src/rc_models.h: n>1 && last==0).
#
# Subcommands (path/index args first so the shell wrappers stay one-liners):
#   header   <out> <crc_src> <from_size> <size_delta_zz>
#       synthesize a header: the tagged-source/target CRC pair copied from <crc_src>, then
#       uLEB(from_size),
#       uLEB(size_delta_zz), then 16 zero pad bytes.
#   overlong <in> <field> <out>
#       copy <in> with header uLEB #<field> re-encoded overlong (one extra 0x00 byte).
#   detect   <blob> <field>
#       print OVERLONG if header uLEB #<field> carries the overlong marker, else canonical.
import sys

CRC_BYTES = 8

def put_uleb(v):
    out = bytearray()
    while True:
        b = v & 0x7f
        v >>= 7
        if v:
            b |= 0x80
        out.append(b)
        if not v:
            break
    return bytes(out)

def uleb_end(buf, off):  # offset just past the uLEB starting at off (exclusive end)
    while buf[off] & 0x80:
        off += 1
    return off + 1

def field_start(buf, field):  # start offset of header uLEB #field (0-based, after the CRC pair)
    off = CRC_BYTES
    for _ in range(field):
        off = uleb_end(buf, off)
    return off

def make_overlong(buf, off):  # re-encode the uLEB at off with a trailing continuation + 0x00
    end = uleb_end(buf, off)
    return buf[:end - 1] + bytes([buf[end - 1] | 0x80, 0x00]) + buf[end:]

def is_overlong(buf, off):
    end = uleb_end(buf, off)
    return (end - off) > 1 and buf[end - 1] == 0

def main():
    cmd = sys.argv[1]
    if cmd == "header":
        out, crc_src, from_size, sd = sys.argv[2], sys.argv[3], int(sys.argv[4]), int(sys.argv[5])
        with open(out, "wb") as f:
            f.write(open(crc_src, "rb").read(CRC_BYTES))
            f.write(put_uleb(from_size))
            f.write(put_uleb(sd))
            f.write(b"\x00" * 16)
    elif cmd == "overlong":
        buf = open(sys.argv[2], "rb").read()
        open(sys.argv[4], "wb").write(make_overlong(buf, field_start(buf, int(sys.argv[3]))))
    elif cmd == "detect":
        buf = open(sys.argv[2], "rb").read()
        print("OVERLONG" if is_overlong(buf, field_start(buf, int(sys.argv[3]))) else "canonical")
    else:
        sys.exit("bad cmd " + cmd)

main()
