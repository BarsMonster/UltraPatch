#!/usr/bin/env python3
"""m4_oracle.py -- reusable oracle for the m4 in-place patcher study.

Loads a fixture pair, builds the canonical detools arm-cortex-m4 apply-side
reference buffers, and parses the bsdiff op-stream into an explicit list of
(diff_len, diff_bytes, extra_bytes, adjustment) tuples.

The detools sequential m4 reconstruction is, for each output byte i:

    to[i] = patch_data[i] + fromzero[fp(i)] + dfdiff[i]      (mod 256)

where:
  * patch_data[i] is the bsdiff diff/extra byte for output position i,
  * fromzero is the from-image with the 4-byte relocation fields zeroed
    (bl/b.w/ldr/ldr.w/data&code pointers); read with a moving from-pointer
    fp that advances by diff_len during diff runs, stays during extra runs,
    and is repositioned by a signed adjustment at the end of each op,
  * dfdiff is a to_size overlay containing the re-encoded to-address field
    bytes (zero everywhere except inside reloc fields of the to-image).

This module exposes:
  load_pair(name_from, name_to, ...) -> Oracle
  Oracle.ops            : list of Op(diff_len, diff, extra, adj)
  Oracle.fromzero       : bytes (len == from_size)
  Oracle.dfdiff         : bytes (len == to_size)
  Oracle.from_image     : bytes (original from-image)
  Oracle.to_image       : bytes (golden to-image)
  Oracle.reloc_from     : sorted list of from-image reloc field start offsets
  Oracle.reloc_to       : sorted list of to-image reloc field start offsets (dfdiff nonzero)
"""
import os
import sys
from io import BytesIO
from collections import namedtuple

_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..', '..'))
DETOOLS_DEV = os.environ.get('DETOOLS_DEV') or os.path.join(_ROOT, 'detools-dev')
if os.path.isdir(DETOOLS_DEV) and DETOOLS_DEV not in sys.path:
    sys.path.insert(0, DETOOLS_DEV)

import detools.apply as A
from detools.common import unpack_size_with_length, unpack_size
from detools.data_format import create_readers
from detools import bsdiff

Op = namedtuple('Op', ['diff_len', 'diff', 'extra', 'adj'])


def _read_buffers(from_path, patch_path):
    """Replicate dump_oracle but with the correct create_readers signature,
    and additionally parse the full bsdiff op-stream."""
    with open(patch_path, 'rb') as fpatch:
        compression, to_size = A.read_header_sequential(fpatch)
        pr = A.PatchReader(fpatch, compression)
        dfpatch_size = unpack_size(pr)
        assert dfpatch_size > 0, "patch has no data-format"
        data_format = unpack_size(pr)
        dfpatch = pr.decompress(dfpatch_size)

        with open(from_path, 'rb') as ffrom_f: from_image = ffrom_f.read()
        ffrom = BytesIO(from_image)
        dfdiff_reader, fromzero_reader = create_readers(
            data_format, ffrom, dfpatch, to_size)
        fromzero = fromzero_reader._ffrom.getvalue()
        dfdiff = dfdiff_reader._fdiff.getvalue()

        # Now parse the remaining op-stream (diff/extra/adjustment) the same
        # way apply_patch_sequential consumes it, recording each op.
        ops = []
        to_pos = 0
        while to_pos < to_size:
            # diff
            diff_len = unpack_size(pr)
            assert to_pos + diff_len <= to_size
            diff = pr.decompress(diff_len) if diff_len else b''
            to_pos += diff_len
            # extra
            extra_len = unpack_size(pr)
            assert to_pos + extra_len <= to_size
            extra = pr.decompress(extra_len) if extra_len else b''
            to_pos += extra_len
            # adjustment (signed) -- moves the from pointer
            adj = unpack_size(pr)
            ops.append(Op(diff_len, diff, extra, adj))

    return {
        'compression': compression,
        'to_size': to_size,
        'data_format': data_format,
        'dfpatch': dfpatch,
        'dfpatch_size': dfpatch_size,
        'fromzero': fromzero,
        'dfdiff': dfdiff,
        'from_image': from_image,
        'ops': ops,
    }


class Oracle:
    def __init__(self, d, to_image):
        self.compression = d['compression']
        self.to_size = d['to_size']
        self.data_format = d['data_format']
        self.dfpatch = d['dfpatch']
        self.dfpatch_size = d['dfpatch_size']
        self.fromzero = d['fromzero']
        self.dfdiff = d['dfdiff']
        self.from_image = d['from_image']
        self.from_size = len(d['from_image'])
        self.to_image = to_image
        self.ops = d['ops']
        # reloc field offsets: where fromzero differs from from_image (zeroed
        # bytes) -- start offsets of 4-byte fields
        self.reloc_from = self._field_starts(
            bytes(a ^ b for a, b in zip(self.from_image, self.fromzero)))
        # to-side reloc fields: where dfdiff is nonzero
        self.reloc_to = self._field_starts(self.dfdiff)

    @staticmethod
    def _field_starts(mask_bytes):
        """Group nonzero runs in mask into 4-byte field start offsets."""
        starts = []
        n = len(mask_bytes)
        i = 0
        while i < n:
            if mask_bytes[i]:
                starts.append(i)
                # skip the run (reloc fields are 4 bytes but runs may be
                # shorter if a zero byte coincides); advance to next zero
                j = i
                while j < n and mask_bytes[j]:
                    j += 1
                i = j
            else:
                i += 1
        return starts

    def reconstruct_sequential(self):
        """Reference byte-exact reconstruction using a SEPARATE output buffer
        (sequential floor; needs whole from-image live). Verifies the model."""
        out = bytearray(self.to_size)
        fp = 0  # from-pointer into fromzero
        to_pos = 0
        for op in self.ops:
            # diff: out = diff + fromzero[fp:fp+diff_len] + dfdiff
            if op.diff_len:
                seg = bsdiff.add_bytes(op.diff, self.fromzero[fp:fp + op.diff_len])
                seg = bsdiff.add_bytes(seg, self.dfdiff[to_pos:to_pos + op.diff_len])
                out[to_pos:to_pos + op.diff_len] = seg
                fp += op.diff_len
                to_pos += op.diff_len
            # extra: out = extra + dfdiff
            el = len(op.extra)
            if el:
                seg = bsdiff.add_bytes(op.extra, self.dfdiff[to_pos:to_pos + el])
                out[to_pos:to_pos + el] = seg
                to_pos += el
            # adjustment
            fp += op.adj
        return bytes(out)


_FIX = os.environ.get('ULTRAPATCH_FIXTURES',
                      os.path.join(_ROOT, 'test-bench', 'fixtures'))
_ART = os.environ.get('ULTRAPATCH_ORACLE_ARTIFACTS',
                      os.path.join(os.path.dirname(__file__), 'artifacts'))


def load_pair(from_dir, to_dir, patch_name):
    from_path = os.path.join(_FIX, from_dir, 'watch.bin')
    to_path = os.path.join(_FIX, to_dir, 'watch.bin')
    patch_path = os.path.join(_ART, patch_name)
    d = _read_buffers(from_path, patch_path)
    with open(to_path, 'rb') as f: to_image = f.read()
    return Oracle(d, to_image)


if __name__ == '__main__':
    for fd, td, pn, label in [
        ('v0_base', 'v1_one_face', 'v0v1_seq_m4.patch', 'v0->v1 (+360B)'),
        ('v0_base', 'v2_three_faces', 'v0v2_seq_m4.patch', 'v0->v2 (+3856B)'),
    ]:
        orc = load_pair(fd, td, pn)
        recon = orc.reconstruct_sequential()
        ok = recon == orc.to_image
        print(f"{label}: from={orc.from_size} to={orc.to_size} "
              f"ops={len(orc.ops)} reloc_from={len(orc.reloc_from)} "
              f"reloc_to={len(orc.reloc_to)} "
              f"dfpatch={orc.dfpatch_size} BYTE-EXACT={ok}")
        assert ok, "model reconstruction mismatch!"
