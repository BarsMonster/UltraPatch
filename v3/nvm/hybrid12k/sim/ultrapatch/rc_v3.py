"""ultrapatch v3 streaming core — shared helper library for the production A1 golden (rc_hybrid).

The PRODUCTION codec is `rc_hybrid.encode_v3`/`decode_v3` (no-bake; see ../../RESULT.md). This module
provides the shared streaming machinery rc_hybrid imports: the apply-order walk (_walk), preserve
analysis (_preserve_indices), the cut-LZSS content builder (_op_meta/_op_content/_build_content/
_content_tags/_cut_tokens), the per-op preserve/correction layout (_preserve_corr_per_op), the parity
content scanner (_ContentScanner), op splitting (_split_ops), and the wire constants (PATHE_W etc.).

The old top-level BAKED codec (encode_v3/decode_v3 + _bake/_encode_B/_decode_B/_encode_A/_encode_A_pc/
_decode_A_apply/_corrections) was RETIRED — A1 is no-bake, so baking de-relocated values into source
rows that the in-place apply later overwrites would violate the ≤1-write/page requirement. (Recover the
removed baked tree from git tag `pre-pathf-removal` if ever needed.)

Wire / range-coder framing details now live with the production codec in rc_hybrid.
"""
import sys, os, struct, zlib, subprocess
from collections import Counter
# Import the AGENT-LOCAL sim copies (UG_CTX=8 etc.). The harness puts agent07/sim and
# agent07/sim/ultrapatch on sys.path first; we only add detools-dev for the `detools` package.
_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = ['/ai_sw/detools-dev', os.path.dirname(_HERE), _HERE]
import rc_codec as rc
from rc_codec import (make_models, BinEnc, BinDec, UGolomb, Flag1, BitTree, ByteVarint,
                      w_gz, r_gz, w_dz, r_dz, w_rice, r_rice, enc_int, dec_int,
                      Op, DEFAULT_OPT, lit_tree_seed, _put_bits, _get_bits)
from codec import (build_control, from_huff_dual, ctrl_tags, lz_optimal, parse_control,
                   CtrlScanner)
from rc_codec import fit_k
from m4reloc import (_parse_blocks, _disasm, _segs, _topos, _active_streams, ORDER, PACK,
                     zz, unzz, leb128, leb128_read)
from m4_oracle import _read_buffers

DET = os.path.expanduser('~/.local/bin/detools')
JCAP = 1024  # v3 journal budget (slots); corpus peak ~903 -> 1024 covers it.
OPCAP = 1 << 30  # Path F: split_ops DISABLED (cap = infinity). The streaming decoder writes
                 # in stream order with NO per-op literal/extra buffer, so ops are kept whole.
                 # (kept as a knob for A/B size measurement; default is no-split.)
# Path E SUPERSOLUTION: W=10 (LZSS ring 1024) is the locked RAM lever that crosses <=8192 B true
# SRAM (it cuts the C decoder's SA ring 2048->1024, the single biggest reducible apply-phase
# structure, -1024 B). Measured size cost: 256-pair total +1.472% vs v2 (vs +1.312% at W=11),
# still <=+1.5%. DEFAULT_OPT comes from the read-only upstream m4reloc (W=11), so we inject W=10
# here in encode_v3/decode_v3. The C decoder c/rc_v3.c SA_W MUST equal this. Override via opt={'W':N}.
PATHE_W = 10


def _split_ops(ops, cap=OPCAP):
    """Split each control op so diff_len<=cap and len(extra)<=cap (SPEC §3.4). Preserves the
    fp/tp accumulation: leading chunks carry only their diff (adj=0, no extra); the final chunk
    carries the remaining diff + the op's extra + adj. Apply order is unchanged."""
    out = []
    for o in ops:
        dl = o.diff_len; diff = o.diff
        off = 0
        # all-but-last diff chunks (only emitted when there is more diff or extra to follow)
        while dl - off > cap:
            out.append(Op(cap, bytes(diff[off:off + cap]), b'', 0)); off += cap
        rem = dl - off
        extra = o.extra
        if len(extra) <= cap:
            out.append(Op(rem, bytes(diff[off:]), bytes(extra), o.adj))
        else:
            # emit the remaining diff with no extra/adj, then chunk the extra
            out.append(Op(rem, bytes(diff[off:]), b'', 0))
            eo = 0
            while len(extra) - eo > cap:
                out.append(Op(0, b'', bytes(extra[eo:eo + cap]), 0)); eo += cap
            out.append(Op(0, b'', bytes(extra[eo:]), o.adj))
    return out


# ----------------------------------------------------------------------------------------
# apply-order walk: yields (write_index, tp, fp, is_diff, db) — identical to the C SA walk.
# ----------------------------------------------------------------------------------------
def _walk(ops, from_size, to_size):
    FWD = to_size <= from_size
    meta = []; tp = fp = 0
    for o in ops:
        meta.append((tp, fp, o.diff_len, o.extra, o.diff))
        tp += o.diff_len + len(o.extra); fp += o.diff_len + o.adj
    order = range(len(meta)) if FWD else range(len(meta) - 1, -1, -1)
    wi = 0
    for op in order:
        (tp0, fp0, dl, ex, di) = meta[op]
        if FWD:
            for k in range(dl): yield wi, tp0 + k, fp0 + k, True, di[k]; wi += 1
            for e in range(len(ex)): yield wi, tp0 + dl + e, -1, False, ex[e]; wi += 1
        else:
            for e in range(len(ex) - 1, -1, -1): yield wi, tp0 + dl + e, -1, False, ex[e]; wi += 1
            for k in range(dl - 1, -1, -1): yield wi, tp0 + k, fp0 + k, True, di[k]; wi += 1


# ----------------------------------------------------------------------------------------
# preserve indices: which write-indices (in apply order) must journal the old byte first.
# Mirrors the C sa_write preserve check exactly (FWD: source read later; BWD: read earlier).
# ----------------------------------------------------------------------------------------
def _preserve_indices(ops, from_size, to_size):
    FWD = to_size <= from_size; INF = 1 << 62
    SENT = -1 if FWD else INF; readarr = [SENT] * from_size
    tp = fp = 0
    for o in ops:
        for k in range(o.diff_len):
            a = fp + k
            if 0 <= a < from_size:
                t = tp + k
                if FWD: readarr[a] = t if readarr[a] < t else readarr[a]
                else:   readarr[a] = t if readarr[a] > t else readarr[a]
        tp += o.diff_len + len(o.extra); fp += o.diff_len + o.adj
    pres = []
    for wi, tpw, fpw, isd, db in _walk(ops, from_size, to_size):
        if 0 <= tpw < from_size:
            later = (readarr[tpw] > tpw) if FWD else (0 <= readarr[tpw] < tpw)
            if later: pres.append(wi)
    return pres


# ========================================================================================
# Path F NO-SPLIT wire (recovers ~1.5% size, frees the 768 B per-op buffer + SA_OPCAP).
# ----------------------------------------------------------------------------------------
# Streaming [A] of ops of ANY size. The decoder writes each output byte immediately (no
# lit_pos/lit_byte/opextra buffer):
#   * shrink/self (FWD): write ascending; literal patches via an O(1) next-position cursor.
#   * grow (BWD): the encoder emits each op's content bytes (literal patches + extra) in
#     DESCENDING write order, so the decoder writes them immediately, descending.
# Per-op GEOMETRY (dl, el, adj) is emitted as DIRECT range-coder symbols BEFORE the op's
# content; preserves [P] are journaled EAGERLY at op start (so the old byte is captured
# before any write overwrites it — needs only geometry + tp0, NO preserve bitmap); the few
# corrections [C] are read into an O(count) cursor array (bounded, not O(op)).
# The op's CONTENT bytes (nl, gaps, literal/extra bytes) form ONE whole-stream LZSS over all
# ops (cross-op backrefs preserved == the size win), with tokens CUT at op-content boundaries
# so the per-op direct [geom|P|C] symbols slot cleanly between ops (the only legal interleave
# point in an LZSS-coded stream — a backref reproduces bytes with zero range-coder symbols).
# ----------------------------------------------------------------------------------------
from codec import uleb as _uleb

def _op_meta(ops, from_size, to_size):
    """Per-op (tp0, fp0, dl, el, adj, diff, extra) in APPLY order, plus FWD flag and the
    emit-order op list (FWD: as-is; grow: reversed). tp0/fp0 are the running accumulators."""
    FWD = to_size <= from_size
    base = []; tp = fp = 0
    for o in ops:
        base.append((tp, fp, o.diff_len, len(o.extra), o.adj, o.diff, o.extra))
        tp += o.diff_len + len(o.extra); fp += o.diff_len + o.adj
    emit_ops = ops if FWD else list(reversed(ops))
    apply_meta = base if FWD else list(reversed(base))   # apply order == emit order
    return FWD, emit_ops, apply_meta

def _op_content(o, FWD):
    """The op's CONTENT bytes (what LZSS runs on), in DECODER-CONSUMPTION == WRITE order:
       FWD : nl (uLEB), [gap uLEB, litbyte] ascending,  extra ascending.
       grow: nl (uLEB), extra DESCENDING,  [descgap uLEB, litbyte] descending.
       (grow writes extra-desc THEN literals-desc, so the stream presents them in that order
        and the decoder writes each byte immediately — no per-op buffer.)
       geometry dl/el/adj are NOT here (emitted as direct symbols)."""
    lits = [(k, b) for k, b in enumerate(o.diff) if b]
    out = bytearray(); out += _uleb(len(lits))
    if FWD:
        prev = 0
        for k, b in lits: out += _uleb(k - prev); out.append(b); prev = k
        out += o.extra
    else:
        out += bytes(reversed(o.extra))     # extra written first (descending) in grow apply
        prev = o.diff_len                   # descending gaps measured down from dl (one-past-top)
        for k, b in reversed(lits): out += _uleb(prev - k); out.append(b); prev = k
    return bytes(out), lits

def _build_content(emit_ops, FWD):
    """Concatenate per-op content; return (content_bytes, op_content_ends, per_op_tags).
    per_op gives, for parity tagging of extra bytes, the (tp0, dl, el) of each op."""
    content = bytearray(); ends = []; geom = []
    tp = 0; fp = 0
    # Recompute tp0 per emit-order op (emit order == apply order, tp accumulates in apply order).
    for o in emit_ops:
        c, _ = _op_content(o, FWD)
        content += c; ends.append(len(content))
    return bytes(content), ends

def _content_tags(content, ends, emit_ops, apply_meta, FWD):
    """Tag each content byte: 0 (low literal table) for everything except an EXTRA (new-code)
    byte, which is tagged by its true to-address parity (direction-independent)."""
    tags = bytearray(len(content)); pos = 0
    for oi, o in enumerate(emit_ops):
        tp0, fp0, dl, el, adj, diff, extra = apply_meta[oi]
        lits = [(k, b) for k, b in enumerate(o.diff) if b]
        # nl
        nlb = _uleb(len(lits))
        for _ in nlb: tags[pos] = 0; pos += 1
        if FWD:
            prev = 0
            for k, b in lits:
                for _ in _uleb(k - prev): tags[pos] = 0; pos += 1
                tags[pos] = 0; pos += 1   # literal byte -> table 0
                prev = k
            exstart = tp0 + dl
            for e in range(el): tags[pos] = (exstart + e) & 1; pos += 1
        else:
            # grow order: nl, extra DESCENDING, then literals descending
            exstart = tp0 + dl
            for e in range(el):           # emitted order: extra[el-1..0]
                tags[pos] = (exstart + (el - 1 - e)) & 1; pos += 1
            prev = dl
            for k, b in reversed(lits):
                for _ in _uleb(prev - k): tags[pos] = 0; pos += 1
                tags[pos] = 0; pos += 1
                prev = k
    return bytes(tags)

def _cut_tokens(seq, bounds):
    """Cut the LZSS token stream so no token spans a content-byte boundary in `bounds`
    (sorted list). Spans are split at boundaries; backrefs are split too, but 1- or 2-byte
    fragments created by the cut are cheaper as literal spans. Adjacent literal spans are merged
    only when no boundary lies between them, preserving every op/delta injection point."""
    import bisect
    bs = bounds; bset = set(bounds); out = []; pos = 0; replay = bytearray()

    def emit_span(data, start):
        if not data:
            return
        data = bytes(data)
        if out and out[-1][0] == 'S' and start not in bset:
            out[-1] = ('S', out[-1][1] + data)
        else:
            out.append(('S', data))

    def replay_backref(dist, ln):
        if dist <= 0 or dist > len(replay):
            raise ValueError("invalid LZ backref")
        data = bytearray()
        for _ in range(ln):
            b = replay[-dist]
            replay.append(b)
            data.append(b)
        return bytes(data)

    for x in seq:
        if x[0] == 'S':
            data = x[1]; start = pos
            while data:
                k = bisect.bisect_right(bs, start)
                nb = bs[k] if (k < len(bs) and bs[k] < start + len(data)) else None
                if nb is None:
                    emit_span(data, start); replay.extend(data); start += len(data); data = []
                else:
                    cut = nb - start; chunk = data[:cut]
                    emit_span(chunk, start); replay.extend(chunk); data = data[cut:]; start = nb
            pos += len(x[1])
        else:
            dist = x[1]; ln = x[2]; start = pos
            while ln > 0:
                k = bisect.bisect_right(bs, start)
                nb = bs[k] if (k < len(bs) and bs[k] < start + ln) else None
                seg = (nb - start) if nb is not None else ln
                data = replay_backref(dist, seg)
                if seg < 3:
                    emit_span(data, start)
                else:
                    out.append(('R', dist, seg))
                start += seg; ln -= seg
            pos += x[2]
    return out

def _preserve_corr_per_op(ops, from_size, to_size, presset, corr):
    """Per op (APPLY order): list of preserve write-offsets (within [0,dl+el)) and list of
    (offset, byte) corrections. Offsets are op-local POSITIONS (direction-independent), so the
    decoder journals/corrects by the same offset regardless of write direction."""
    FWD = to_size <= from_size; meta = []; tp = fp = 0
    for o in ops:
        meta.append((tp, fp, o.diff_len, o.extra)); tp += o.diff_len + len(o.extra); fp += o.diff_len + o.adj
    order = range(len(meta)) if FWD else range(len(meta) - 1, -1, -1)
    out_p = []; out_c = []; wi = 0
    for opi in order:
        (tp0, fp0, dl, ex) = meta[opi]; pl = []; cl = []
        el = len(ex)
        if FWD:
            for k in range(dl):
                if wi in presset: pl.append(k)
                if (tp0 + k) in corr: cl.append((k, corr[tp0 + k]))
                wi += 1
            for e in range(el):
                if wi in presset: pl.append(dl + e)
                if (tp0 + dl + e) in corr: cl.append((dl + e, corr[tp0 + dl + e]))
                wi += 1
        else:
            for e in range(el - 1, -1, -1):
                if wi in presset: pl.append(dl + e)
                if (tp0 + dl + e) in corr: cl.append((dl + e, corr[tp0 + dl + e]))
                wi += 1
            for k in range(dl - 1, -1, -1):
                if wi in presset: pl.append(k)
                if (tp0 + k) in corr: cl.append((k, corr[tp0 + k]))
                wi += 1
        out_p.append(sorted(pl)); out_c.append(sorted(cl))
    return out_p, out_c   # indexed by apply-order op index


class _ContentScanner:
    """Parity tagger for the Path F CONTENT stream, fed geometry per op externally. Mirrors the
    C cs_* scanner. Per op: caller sets (tp0, dl, el) via begin_op(); then advance() each content
    byte. Tag is 0 except for EXTRA bytes, tagged by their true to-address parity.
    FWD  phase order: nl, [gap,litb]*, extra.
    grow phase order: nl, extra, [gap,litb]*."""
    def __init__(s, FWD):
        s.FWD = FWD; s.ph = 'nl'; s.first = True; s.acc = 0; s.sh = 0
        s.nl = 0; s.li = 0; s.el = 0; s.ei = 0; s.dl = 0; s.tp0 = 0
    def begin_op(s, tp0, dl, el):
        s.tp0 = tp0; s.dl = dl; s.el = el; s.ph = 'nl'; s.first = True; s.acc = 0; s.sh = 0
        s.nl = 0; s.li = 0; s.ei = 0
    def _after_nl(s):
        if s.FWD:
            if s.nl > 0: s.ph = 'gap'
            elif s.el > 0: s.ph = 'extra'; s.ei = 0
            else: s.ph = 'done'
        else:
            if s.el > 0: s.ph = 'extra'; s.ei = 0
            elif s.nl > 0: s.ph = 'gap'
            else: s.ph = 'done'
    def _after_lits(s):   # FWD: -> extra; grow: lits are last -> done
        if s.FWD and s.el > 0: s.ph = 'extra'; s.ei = 0
        else: s.ph = 'done'
    def _after_extra(s):  # FWD: extra is last -> done; grow: -> lits
        if (not s.FWD) and s.nl > 0: s.ph = 'gap'
        else: s.ph = 'done'
    def next_tag(s):
        if s.ph == 'extra':
            exstart = s.tp0 + s.dl
            off = s.ei if s.FWD else (s.el - 1 - s.ei)
            return (exstart + off) & 1
        return 0
    def advance(s, b):
        ph = s.ph
        if ph == 'nl' or ph == 'gap':
            if s.first: s.acc = b & 0x7f; s.sh = 7; s.first = False
            else: s.acc |= (b & 0x7f) << s.sh; s.sh += 7
            if b & 0x80: return False
            v = s.acc; s.first = True
            if ph == 'nl': s.nl = v; s.li = 0; s._after_nl()
            else: s.ph = 'litb'
            return s.ph == 'done'
        elif ph == 'litb':
            s.li += 1
            if s.li < s.nl: s.ph = 'gap'; s.first = True; s.acc = 0
            else: s._after_lits()
            return s.ph == 'done'
        elif ph == 'extra':
            s.ei += 1
            if s.ei >= s.el: s._after_extra()
            return s.ph == 'done'
        return False
