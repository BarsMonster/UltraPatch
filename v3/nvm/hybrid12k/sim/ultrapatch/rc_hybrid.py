"""ultrapatch v3 HYBRID golden (A1) — no-bake; on-device bl + ldr position derive.

Architecture (full design in ../../RESULT.md + ../../A1_FEASIBILITY.md):
  * NO baking. The [A] copy reads RAW from[fp]; corrections at the monotonic output frontier ([C]).
  * bl relocation positions are DERIVED on-device by a local halfword pattern (self-framing); the
    rare false-positives with literal-patched bytes are implicit suppressed-BL normal copies, and any
    residual is repaired by [C].
  * ldr relocation positions are DERIVED on-device PER OP (A1, _op_ldr_set): a field is ldr iff an
    ldr literal instruction in the SAME op's copy range targets it. The encoder and the bounded
    on-device decoder compute the identical per-op predicate, so POSITIONS are never shipped;
    cross-op pairs are absorbed by [C]. (data/code pointer fields are absent on Cortex-M0+.)
  * Per-field delta VALUES are pulled inline from the single range stream at detection (adaptive
    MTF dict + repeat-bit) — no resident delta store.
  * Output via a monotonic row write-back cache (C decoder) -> 1 write/row, nvm_rows_amplified=0.

Reuses the streaming core (rc_v3: pauseable LZSS content, per-op direct geometry/[P]/[C]). The C decoder
(c/rc_v3.c) mirrors this module bit-for-bit.
"""
import sys, os, struct, zlib, subprocess
_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = ['/ai_sw/detools-dev', os.path.dirname(_HERE), _HERE]
from rc_codec import (make_models, BinEnc, BinDec, UGolomb,
                      w_gz, r_gz, w_dz, r_dz, enc_int, dec_int,
                      Op, DEFAULT_OPT, _put_bits, _get_bits)
from m4reloc import (_parse_blocks, _disasm, _active_streams, ORDER, PACK,
                     zz, unzz, leb128, leb128_read)
from m4_oracle import _read_buffers
from rc_v3 import (_op_meta, _build_content, _content_tags, lz_optimal, _cut_tokens,
                   from_huff_dual, fit_k, _walk, _preserve_indices, _uleb, DET, PATHE_W,
                   _ContentScanner)

# --- bl pack/unpack on raw bytes (mirrors m4reloc.pack_bl + arm unpack) ---
def unpack_bl(up, lo):
    s = (up >> 10) & 1; imm10 = up & 0x3ff; imm11 = lo & 0x7ff
    j1 = (lo >> 13) & 1; j2 = (lo >> 11) & 1; i1 = 1 - (j1 ^ s); i2 = 1 - (j2 ^ s)
    v = (s << 23) | (i1 << 22) | (i2 << 21) | (imm10 << 11) | imm11
    if s: v -= (1 << 24)
    return v
def pack_bl_bytes(imm32):
    return PACK['bl'](imm32 & 0xffffffff)   # returns 4 bytes

_HYCAP = 0   # 0 = no op-split. >0 caps diff_len/op (bounds per-op delta store in the C decoder).
KIND = {'data': 0, 'code': 1, 'ldr': 2}
INV_KIND = {v: k for k, v in KIND.items()}
DR_HIT_INIT = 512      # P(hit-bit == 0); zero-seeded dicts make hits likely.
DR_KCAP = {'bl': 208, 'ex': 128}   # must match the production decoder defaults in c/rc_v3.c


def _norm_opt(opt):
    out = dict(DEFAULT_OPT)
    if opt:
        out.update(opt)
    if opt is None or 'W' not in opt:
        out['W'] = PATHE_W
    return out


def _delta_models():
    """Initial state for the streamed relocation-delta MTF models.

    Zero is both the initial last value and a seeded dictionary entry. The repeat
    path covers a leading zero delta, and after any nonzero escape the zero moves
    to index 1, which is the first encodable MTF hit index (index 0 remains the
    current last value and is therefore unreachable for non-repeat symbols).
    """
    return {
        'bl': {'dic': [0], 'hit': [DR_HIT_INIT], 'rep': [2048, 2048, 2048, 2048], 'rh': 0, 'last': 0},
        'ex': {'dic': [0], 'hit': [DR_HIT_INIT], 'rep': [2048, 2048, 2048, 2048], 'rh': 0, 'last': 0},
    }


def _op_ldr_set(frm, fp0, dl, from_size):
    """A1 SAME-OP ldr derive for ONE op: scan its copy range [fp0,fp0+dl) for ldr instructions and
    return the SET of target from-addresses that also land in [fp0,fp0+dl). Computed identically by
    the encoder (over `frm`) and the on-device decoder (over the op's pristine source, readable at op
    start before any in-op overwrite). Bounded by the op; the C derives it streaming. Cross-op pairs
    are not derived here -> absorbed by [C], so enc==dec by construction."""
    lo = fp0; hi = fp0 + dl; s = set()
    a = lo if (lo % 2 == 0) else lo + 1
    while a + 2 <= hi and a + 2 <= from_size:
        up = frm[a] | (frm[a + 1] << 8)
        if (up & 0xf800) == 0x4800:
            t = (a & ~3) + 4 * (up & 0xff) + 4
            if lo <= t and t + 4 <= hi and t + 4 <= from_size: s.add(t)
        a += 2
    return s


def _field_deltas(frm, dfpatch, opt):
    """Return {faddr: (stream_name, delta)} for every captured relocation field (true disasm)."""
    blocks, vals, cfg7 = _parse_blocks(dfpatch)
    dp, fdo, fdb, fde, cp, fcb, fce = cfg7
    ranges = (fdo, fdb, fde, fcb, fce); streams = _active_streams(opt)
    fmaps = _disasm(frm, ranges, streams)
    out = {}; voff = 0
    for nm in ORDER:
        cnt = sum(b[2] for b in blocks[nm]); sv = vals[voff:voff + cnt]; voff += cnt
        if nm not in streams: continue
        sf = sorted(fmaps[nm]); di = 0
        for (fo, ta, n) in blocks[nm]:
            for k in range(n):
                faddr = sf[fo + k]; delta = sv[di]; di += 1
                out[faddr] = (nm, delta)   # later block wins on overlap (ldr last in ORDER)
    return out, cfg7


def _coerce_reloc_literals(ops, frm, fielddelta, from_size, to_size):
    """Encoder-only cleanup: if a true BL / same-op LDR field is currently blocked only by bsdiff
    literal bytes in its 4-byte field, zero those diff bytes so the existing pure-field decoder path
    handles it and residual [C] shrinks. Geometry is unchanged, and false positives stay untouched."""
    FWD = to_size <= from_size
    out = []; fp0 = 0
    for o in ops:
        ndiff = None
        pred = _op_ldr_set(frm, fp0, o.diff_len, from_size)
        for ev in _scan_op_fields(frm, fp0, o.diff_len, from_size, fielddelta, diff=None,
                                  desc=not FWD, pred_set=pred):
            tag = ev[0]
            if tag == 'bl':
                _, k, fpk, _delta = ev
                real = fielddelta.get(fpk)
                if not (real and real[0] == 'bl'):
                    continue
            elif tag == 'ex':
                _, k, fpk, kind, _delta = ev
                real = fielddelta.get(fpk)
                if kind != KIND['ldr'] or not (real and real[0] == 'ldr'):
                    continue
            else:
                continue
            if any(o.diff[k + b] for b in range(4)):
                if ndiff is None:
                    ndiff = bytearray(o.diff)
                for b in range(4):
                    ndiff[k + b] = 0
        out.append(Op(o.diff_len, bytes(ndiff) if ndiff is not None else o.diff, o.extra, o.adj))
        fp0 += o.diff_len + o.adj
    return out


# ---------- build the to-order DEREL plan (encoder & decoder share the walk) ----------
def is_local_bl(frm, fpk, from_size):
    """PURE per-position bl predicate (GLOBAL even alignment, no scan state). 0% misframe, 0
    false-neg, ~0.17% false-pos on corpus. Both encoder & decoder evaluate it identically."""
    if fpk & 1: return False
    if fpk + 4 > from_size: return False
    up = frm[fpk] | (frm[fpk + 1] << 8); lo = frm[fpk + 2] | (frm[fpk + 3] << 8)
    return (up & 0xf800) == 0xf000 and (lo & 0xd000) == 0xd000

def _scan_op_fields(frm, fp0, dl, from_size, fielddelta, diff=None, desc=False, pred_set=None):
    """The CANONICAL field scan over one op's diff range [0,dl), in WRITE order: ascending k for
    shrink (desc=False), DESCENDING for grow (desc=True). A field [k,k+3] is detected only when
    its 4 bytes lie inside [0,dl) and the from-image AND are PURE COPIES (no literal patch). The
    detection KEY is the field START k (its 4 bytes); in descending order we reach a field by its
    TOP offset and resolve k=top-3. Both encoder and decoder run this identical scan so detection
    order, value-stream consumption order, and override positions match exactly.
    Yields ('bl',k,fpk,delta) / ('sbl',k,fpk) / ('ex',k,fpk,kind,delta), k = field START offset.
    A1: pred_set != None => ex (ldr) fields are DERIVED (fpk in pred_set), positions NOT shipped;
    the delta is the real reloc delta if captured, else 0 (a safe identity de-reloc)."""
    def pure(k):
        return diff is None or (diff[k] == 0 and diff[k + 1] == 0 and diff[k + 2] == 0 and diff[k + 3] == 0)
    def classify(k):
        fpk = fp0 + k
        if is_local_bl(frm, fpk, from_size):
            if pure(k):
                real = fielddelta.get(fpk)
                delta = real[1] if (real and real[0] == 'bl') else 0
                return ('bl', k, fpk, delta)
            return ('sbl', k, fpk)
        if pred_set is not None:
            if fpk in pred_set and fpk + 4 <= from_size and pure(k):
                real = fielddelta.get(fpk)
                delta = real[1] if (real and real[0] == 'ldr') else 0
                return ('ex', k, fpk, KIND['ldr'], delta)
            return None
        if (fpk in fielddelta and fielddelta[fpk][0] != 'bl' and fpk + 4 <= from_size and pure(k)):
            nm, delta = fielddelta[fpk]
            return ('ex', k, fpk, KIND[nm], delta)
        return None
    if not desc:
        k = 0
        while k < dl:
            if k + 4 <= dl:
                ev = classify(k)
                if ev is not None: yield ev; k += 4; continue
            k += 1
    else:
        k = dl - 1
        while k >= 0:
            ks = k - 3
            if ks >= 0:
                ev = classify(ks)
                if ev is not None: yield ev; k -= 4; continue
            k -= 1

def _op_delta_content_pos(o, FWD, frm, fp0, dl, from_size, fielddelta, pred_set=None):
    """STREAMED-DELTA interleave: mirror the decoder's per-op content consumption + write-order
    field detection, and return a list of (content_pos, kind, delta) — content_pos = the number of
    THIS OP's content bytes consumed at the instant the decoder pulls the field's delta (i.e. when
    field_at() runs, BEFORE the field's own copy bytes). kind in {'bl','ex'}; 'sbl' carries no value
    so it is omitted. Both encoder (here, to pause tokens + interleave the enc) and decoder (pulls the
    delta inline at field detection) reach each field with the IDENTICAL content-consumption count,
    so the single range stream stays in sync with ZERO resident delta store."""
    lits = [(k, b) for k, b in enumerate(o.diff) if b]
    nl = len(lits)
    # content layout (decoder consumption order):
    #   FWD : nl_uleb, [gap_uleb, litb]*nl, extra*el
    #   grow: nl_uleb, extra*el, [descgap_uleb, litb]*nl
    out = []
    cc = len(_uleb(nl))                      # bytes consumed reading nl
    if FWD:
        # prefetch state mirrors the decoder: nextpos/li advance lazily; the gap-uleb+litb for the
        # NEXT patch are consumed when the previous patch position is passed (here: at first prefetch
        # and at each take_db). We walk k ascending; field_at(k) pulls BEFORE take_db(k).
        li = 0; prev = 0; nextpos = -1
        if nl > 0:
            g0 = lits[0][0] - 0
            cc += len(_uleb(g0)) + 1; nextpos = lits[0][0]; li_pf = 1   # first patch prefetched
        else:
            li_pf = 0
        def consume_copy(kk):
            nonlocal cc, li, li_pf, nextpos
            if kk == nextpos:
                li += 1
                if li_pf < nl:
                    g = lits[li_pf][0] - lits[li_pf - 1][0]
                    cc += len(_uleb(g)) + 1; nextpos = lits[li_pf][0]; li_pf += 1
                else:
                    nextpos = -1
        k = 0
        while k < dl:
            if k + 4 <= dl:
                ev = _classify_field(frm, fp0, k, from_size, fielddelta, o.diff, pred_set)
                if ev is not None:
                    if ev[0] != 'sbl':
                        out.append((cc, ev[0], ev[-1]))      # bl/ex override: 4 bytes, NO content
                    else:
                        for b in range(4): consume_copy(k + b)  # sbl: 4 normal copies
                    k += 4; continue
            consume_copy(k); k += 1
    else:
        # grow: extra consumed first (el bytes), then descending literals.
        el = len(o.extra); cc += el
        li_pf = 0; nextpos = -1
        if nl > 0:
            top = dl
            g0 = top - lits[-1][0]
            cc += len(_uleb(g0)) + 1; nextpos = lits[-1][0]; li_pf = 1
        def consume_copy_d(kk):
            nonlocal cc, li_pf, nextpos
            if kk == nextpos:
                if li_pf < nl:
                    idx = nl - 1 - li_pf
                    g = lits[idx + 1][0] - lits[idx][0]
                    cc += len(_uleb(g)) + 1; nextpos = lits[idx][0]; li_pf += 1
                else:
                    nextpos = -1
        k = dl - 1
        while k >= 0:
            ks = k - 3
            if ks >= 0:
                ev = _classify_field(frm, fp0, ks, from_size, fielddelta, o.diff, pred_set)
                if ev is not None:
                    if ev[0] != 'sbl':
                        out.append((cc, ev[0], ev[-1]))
                    else:
                        for b in range(3, -1, -1): consume_copy_d(ks + b)
                    k -= 4; continue
            consume_copy_d(k); k -= 1
    return out


def _classify_field(frm, fp0, k, from_size, fielddelta, diff, pred_set=None):
    """Single-field classifier (matches _scan_op_fields.classify): returns ('bl',delta) /
    ('ex',delta) / ('sbl',) / None. delta is the LAST element so callers can index ev[-1].
    A1: pred_set != None => ex (ldr) derived (fpk in pred_set), delta = real-or-0."""
    def pure(kk):
        return diff[kk] == 0 and diff[kk + 1] == 0 and diff[kk + 2] == 0 and diff[kk + 3] == 0
    fpk = fp0 + k
    if is_local_bl(frm, fpk, from_size):
        if pure(k):
            real = fielddelta.get(fpk)
            delta = real[1] if (real and real[0] == 'bl') else 0
            return ('bl', delta)
        return ('sbl',)
    if pred_set is not None:
        if fpk in pred_set and fpk + 4 <= from_size and pure(k):
            real = fielddelta.get(fpk)
            return ('ex', real[1] if (real and real[0] == 'ldr') else 0)
        return None
    if (fpk in fielddelta and fielddelta[fpk][0] != 'bl' and fpk + 4 <= from_size and pure(k)):
        nm, delta = fielddelta[fpk]
        return ('ex', delta)
    return None


# ---------- top-level encode/decode ----------
def encode_v3(F, T, opt=None):
    opt = _norm_opt(opt)
    fbin = F + '/watch.bin'; tbin = T + '/watch.bin'
    p = '/tmp/hybrid_es_%d.patch' % os.getpid()
    try:
        subprocess.run([DET, 'create_patch', '-t', 'sequential', '-a', 'bsdiff',
                        '--data-format', 'arm-cortex-m4', '--from-elf-file', F + '/watch.elf',
                        '--to-elf-file', T + '/watch.elf', '-c', 'none', fbin, tbin, p],
                       check=True, capture_output=True)
        d = _read_buffers(fbin, p)
    finally:
        if os.path.exists(p): os.remove(p)
    ops = [Op(o.diff_len, o.diff, o.extra, o.adj) for o in d['ops']]
    if _HYCAP:
        from rc_v3 import _split_ops as _split
        ops = _split(ops, cap=_HYCAP)
    frm = d['from_image']; true_to = open(tbin, 'rb').read()
    from_size = len(frm); to_size = sum(o.diff_len + len(o.extra) for o in ops)
    fp_end = sum(o.diff_len + o.adj for o in ops)
    crc_to = zlib.crc32(true_to) & 0xffffffff

    fielddelta, cfg7 = _field_deltas(frm, d['dfpatch'], opt)
    ops = _coerce_reloc_literals(ops, frm, fielddelta, from_size, to_size)
    # ---- compute the residual corrections [C] = want - (db + derel_src) for every tp ----
    # derel_src reproduces exactly what the DECODER will write at relocation fields (bl-derive +
    # ex-events), so [C] only carries true residual noise + bl false-positive repairs.
    pres = _preserve_indices(ops, from_size, to_size); presset = set(pres)
    corr = _corrections_hybrid(ops, frm, fielddelta, from_size, to_size, presset, true_to)

    M = make_models(frm); M['hy_dd'] = _delta_models(); rc_ = BinEnc()
    # --- STREAMED DELTAS (12 KiB build): NO up-front DEREL store. Each field's delta VALUE is emitted
    # INLINE at its detection point during [A]'s content walk (interleaved with the content tokens, in
    # the exact order the decoder pulls them). The decoder pulls the next delta the instant it detects
    # a field (the field TYPE is known from detection, so no untyped up-front decode is needed). This
    # removes BOTH resident DEREL stores (~8 KB) -> SRAM well under 12 KiB. The dict + zero-gap RLE of
    # the old codec is dropped; deltas are coded as zigzag values via per-stream UGolomb models. ---
    _encode_A_hybrid(rc_, M, ops, opt, from_size, to_size, presset, corr, fielddelta)
    body = rc_.flush_opt()

    hdr = (struct.pack('<I', zlib.crc32(frm) & 0xffffffff) + leb128(from_size) +
           leb128(to_size) + leb128(fp_end))
    return hdr + body + struct.pack('<I', crc_to), cfg7


def _encode_A_hybrid(rc_, M, ops, opt, from_size, to_size, presset, corr, fielddelta, pred_set=None):
    """Path F [A] (pauseable LZSS content + per-op direct [dl,el,adj,P,C]) plus inline relocation-delta
    values. BL suppression is implicit (`!pure`), and ldr positions are derived per op."""
    from rc_v3 import (_op_meta, _build_content, _content_tags, _preserve_corr_per_op)
    FWD, emit_ops, apply_meta = _op_meta(ops, from_size, to_size)
    content, ends = _build_content(emit_ops, FWD)
    tags = _content_tags(content, ends, emit_ops, apply_meta, FWD)
    (_, L0), (_, L1) = from_huff_dual(M['frm']); Ls = (L0, L1)
    litbits = [Ls[tags[p]][content[p]] for p in range(len(content))]
    import codec as _c
    Wuse = opt.get('W', _c.W)
    seq = lz_optimal(content, litbits, win=(1 << Wuse), wbits=Wuse)
    out_p, out_c = _preserve_corr_per_op(ops, from_size, to_size, presset, corr)
    # Per-op ldr candidate sets are reused by correction simulation and delta injection planning.
    op_ldr = []
    for oi in range(len(emit_ops)):
        tp0, fp0, dl, el, adj, diff, extra = apply_meta[oi]
        op_ldr.append(_op_ldr_set(M['frm'], fp0, dl, from_size))
    # ---- STREAMED-DELTA injection plan: each bl/ex field's delta VALUE is emitted INLINE at the
    # content position the decoder pulls it (op content base + per-op content-consumption count).
    # LZSS tokens are pauseable at those interleave points: the token header is emitted once, then
    # active span/backref state survives while geometry or deltas are emitted between content bytes.
    # The decoder already keeps the same active token state in sa_next_content(). ----
    # per-op delta list (cc within the op, in detection order) — the op's deltas interleave with its
    # OWN content; op N's deltas are all emitted before op N+1's block (decode order).
    op_inj = []   # op_inj[oi] = sorted list of (cc, delta) for op oi
    for oi in range(len(emit_ops)):
        o = emit_ops[oi]
        tp0, fp0, dl, el, adj, diff, extra = apply_meta[oi]
        evs = [(cc, kind, delta) for (cc, kind, delta) in
               _op_delta_content_pos(o, FWD, M['frm'], fp0, dl, from_size, fielddelta, op_ldr[oi])]
        op_inj.append(evs)
    kd = fit_k([x[1] - 1 for x in seq if x[0] == 'R'])
    M['k'] = {'backref_dist': kd}
    enc_int(rc_, M, 'token_count', len(seq)); _put_bits(rc_, kd, 4)
    w_gz(rc_, len(emit_ops))
    gP = UGolomb('g'); gC = UGolomb('g')
    def emit_geom_pc(oi):
        tp0, fp0, dl, el, adj, diff, extra = apply_meta[oi]
        w_dz(rc_, dl); w_dz(rc_, el); w_dz(rc_, zz(adj))
        pl = out_p[oi]; cl = out_c[oi]
        gP.encode(rc_, len(pl)); pw = 0
        for w in pl: gP.encode(rc_, w - pw); pw = w
        gC.encode(rc_, len(cl)); pt = 0
        for (t, bb) in cl:
            gC.encode(rc_, t - pt); pt = t
            # Reuse the resident DEREL escape byte tree for correction bytes; no decoder SRAM cost.
            M['dval'].t.encode(rc_, bb)
    # token cursor: tokens are in content order and may span op/delta interleave boundaries.
    # A span emits its literal bytes only as far as the current content boundary; a backref emits
    # only its header, then advances without more range symbols until exhausted.
    tok_i = [0]; pos = [0]
    tok_mode = [None]; tok_left = [0]; span_data = [b'']; span_pos = [0]
    def start_token():
        if tok_i[0] >= len(seq):
            raise RuntimeError("content token underrun")
        x = seq[tok_i[0]]; tok_i[0] += 1
        if x[0] == 'S':
            M['flag'].encode(rc_, 0); enc_int(rc_, M, 'span_len', len(x[1]) - 1)
            tok_mode[0] = 'S'; tok_left[0] = len(x[1]); span_data[0] = x[1]; span_pos[0] = 0
        else:
            M['flag'].encode(rc_, 1); enc_int(rc_, M, 'backref_dist', x[1] - 1)
            enc_int(rc_, M, 'backref_len_v3', x[2] - 1)
            tok_mode[0] = 'R'; tok_left[0] = x[2]
    def emit_tokens_to(end):
        if end < pos[0] or end > len(content):
            raise RuntimeError("invalid content cursor")
        while pos[0] < end:
            if tok_mode[0] is None:
                start_token()
            n = min(tok_left[0], end - pos[0])
            if tok_mode[0] == 'S':
                st = span_pos[0]
                for byte in span_data[0][st:st + n]:
                    M['lit'][tags[pos[0]]].encode(rc_, byte); pos[0] += 1
                span_pos[0] += n
            else:
                pos[0] += n
            tok_left[0] -= n
            if tok_left[0] == 0:
                tok_mode[0] = None
    # STREAMABLE ADAPTIVE-DICT value coder (recovers the old dict+RLE gain without a resident store):
    # per-stream the en/decoder build an IDENTICAL incremental dictionary of seen delta values. Each
    # delta codes: hit-flag (adaptive bit) + either its dict-index (adaptive UGolomb -> learns the
    # frequent offsets ~ the old dict) OR an escape (new value via the dval bit-tree, then appended).
    # Relocation deltas come from a tiny set (corpus dict peak ~180), so indices are cheap; 0 is the
    # most frequent value -> smallest index. O(dict) state, no per-detection resident array.
    dd = M['hy_dd']        # {'bl': {'dic', 'hit', 'rep', 'last'}, 'ex': ...}
    dcap = {'bl': opt.get('DR_KCAP_BL', DR_KCAP['bl']), 'ex': opt.get('DR_KCAP_EX', DR_KCAP['ex'])}
    def emit_delta(kind, delta):
        D = dd[kind]
        # 1) "repeat last" fast path (consecutive identical deltas == the old RLE run) — 1 cheap bit.
        ri = D['rh'] | (2 if D['last'] == 0 else 0)
        if delta == D['last']:
            rc_.encode_bit(D['rep'], ri, 1); D['rh'] = 1; return
        rc_.encode_bit(D['rep'], ri, 0); D['rh'] = 0; D['last'] = delta
        # 2) dict hit (adaptive index over the small distinct-value set ~ the old dict) / escape.
        # MOVE-TO-FRONT: a hit moves its value to index 0, so frequently-repeated offsets keep tiny
        # indices (the adaptive UGolomb then spends ~1-2 bits on them) — recovers the old dict gain.
        dic = D['dic']
        try:
            j = dic.index(delta)
        except ValueError:
            j = -1
        if j >= 0:
            rc_.encode_bit(D['hit'], 0, 1); enc_int(rc_, M, 'hy_di_' + kind, j - 1)
            if j: dic.insert(0, dic.pop(j))
        else:
            if len(dic) >= dcap[kind]:
                raise ValueError("%s delta dictionary cap exceeded (%d)" % (kind, dcap[kind]))
            rc_.encode_bit(D['hit'], 0, 0); M['dval'].encode(rc_, delta)
            dic.insert(0, delta)
    for oi in range(len(emit_ops)):
        emit_geom_pc(oi)                       # op block (decoder reads it at op start)
        base = 0 if oi == 0 else ends[oi - 1]
        op_end = ends[oi]
        for (cc, kind, delta) in op_inj[oi]:
            emit_tokens_to(base + cc)          # all content tokens before this delta's content pos
            emit_delta(kind, delta)            # delta pulled here (before the content byte at cc)
        emit_tokens_to(op_end)                 # remaining content of this op
    if pos[0] != len(content) or tok_i[0] != len(seq) or tok_mode[0] is not None:
        raise RuntimeError("content token cursor ended out of sync")


def _corrections_hybrid(ops, frm, fielddelta, from_size, to_size, presset, true_to, pred_set=None):
    """Simulate the DECODER's no-bake reconstruction (raw copy + bl-derive + ex de-reloc) via the
    SAME canonical per-op scan, and return corr[tp] = want - produced for every differing tp."""
    FWD = to_size <= from_size
    meta = []; tp = fp = 0
    for o in ops:
        meta.append((tp, fp, o.diff_len, o.adj, o.diff)); tp += o.diff_len + len(o.extra); fp += o.diff_len + o.adj
    order = range(len(meta)) if FWD else range(len(meta) - 1, -1, -1)
    override = {}
    for opi in order:
        (tp0, fp0, dl, adj, diff) = meta[opi]
        os = _op_ldr_set(frm, fp0, dl, from_size)
        for ev in _scan_op_fields(frm, fp0, dl, from_size, fielddelta, diff, desc=not FWD, pred_set=os):
            if ev[0] == 'bl':
                _, k, fpk, delta = ev
                up = frm[fpk] | (frm[fpk + 1] << 8); lo = frm[fpk + 2] | (frm[fpk + 3] << 8)
                packed = pack_bl_bytes(unpack_bl(up, lo) - delta)
            elif ev[0] == 'ex':
                _, k, fpk, kind, delta = ev
                val = frm[fpk] | (frm[fpk + 1] << 8) | (frm[fpk + 2] << 16) | (frm[fpk + 3] << 24)
                packed = PACK[INV_KIND[kind]]((val - delta) & 0xffffffff)
            else:
                continue   # 'sbl': no override, normal db+raw copy
            for b in range(4): override[tp0 + k + b] = packed[b]
    span = max(from_size, to_size); buf = bytearray(span); buf[:from_size] = frm; J = {}
    corr = {}
    for wi, tp, fp, isd, db in _walk(ops, from_size, to_size):
        if wi in presset: J[tp] = buf[tp] if tp < from_size else 0
        if tp in override:
            produced = override[tp]
        else:
            src = (J.get(fp, buf[fp]) if (isd and 0 <= fp < from_size) else 0)
            produced = (db + src) & 0xff
        want = true_to[tp]
        c = (want - produced) & 0xff
        if c != 0: corr[tp] = c
        buf[tp] = want
    return corr


def _decode_apply_hybrid(rc_, M, buf, from_size, to_size, fp_end, opt, cfg7=None):
    """Self-contained no-bake streaming apply (golden mirror of the hybrid C decoder). Per op:
    DIRECT geometry+P+C, then per-detection bl/ex deltas. Inline write-order field detection:
    bl by local pattern (`!pure` => implicit sbl), ex (ldr) DERIVED (fpk in pred_set, A1);
    override 4 packed bytes."""
    FWD = to_size <= from_size; span = max(from_size, to_size)
    frm_snap = bytes(buf[:from_size])     # original from-image (buf will be overwritten in place)
    # A1: ldr derive is PER-OP (same-op causal) — computed at each op start from the op's PRISTINE
    # source range, exactly what the bounded on-device decoder reads (committed output never overlaps
    # the current op's [fp0,fp0+dl), so direct flash is pristine there). cfg7 unused by causal derive.
    # ---- [A] header ----
    nseq = dec_int(rc_, M, 'token_count'); kd = _get_bits(rc_, 4); M['k'] = {'backref_dist': kd}
    M['ints'].pop('backref_dist', None)
    nops = r_gz(rc_)
    gP = UGolomb('g'); gC = UGolomb('g')
    sc = _ContentScanner(FWD)
    J = {}; peak = [0]
    ring = bytearray()
    tok = {'left': nseq, 'mode': None, 'span_left': 0, 'br_src': 0, 'br_left': 0}
    st = {'tp': 0 if FWD else to_size, 'fp': 0 if FWD else fp_end}

    def next_content_byte():
        while True:
            if tok['mode'] == 'S' and tok['span_left'] > 0:
                t = sc.next_tag(); bb = M['lit'][t].decode(rc_); ring.append(bb)
                tok['span_left'] -= 1
                if tok['span_left'] == 0: tok['mode'] = None
                return bb
            if tok['mode'] == 'R' and tok['br_left'] > 0:
                bb = ring[tok['br_src']]; ring.append(bb); tok['br_src'] += 1; tok['br_left'] -= 1
                if tok['br_left'] == 0: tok['mode'] = None
                return bb
            if tok['left'] <= 0: raise RuntimeError('content underrun')
            if M['flag'].decode(rc_) == 0:
                ln = dec_int(rc_, M, 'span_len') + 1; tok['mode'] = 'S'; tok['span_left'] = ln
            else:
                dist = dec_int(rc_, M, 'backref_dist') + 1; ln = dec_int(rc_, M, 'backref_len_v3') + 1
                if dist < 1 or dist > len(ring): raise RuntimeError('invalid backref distance')
                tok['mode'] = 'R'; tok['br_src'] = len(ring) - dist; tok['br_left'] = ln
            tok['left'] -= 1

    def read_uleb():
        acc = 0; sh = 0
        while True:
            bb = next_content_byte(); sc.advance(bb)
            if sh > 28: raise RuntimeError('uleb too large')
            acc |= (bb & 0x7f) << sh; sh += 7
            if not (bb & 0x80): return acc

    def rawsrc(fp):
        return J.get(fp, buf[fp]) if (0 <= fp < from_size) else 0

    for _ in range(nops):
        dl = r_dz(rc_); el = r_dz(rc_); adj = unzz(r_dz(rc_)); nw = dl + el
        if FWD: tp0 = st['tp']; fp0 = st['fp']; st['tp'] += nw; st['fp'] += dl + adj
        else:   st['tp'] -= nw; st['fp'] -= dl + adj; tp0 = st['tp']; fp0 = st['fp']
        op_ldr = _op_ldr_set(frm_snap, fp0, dl, from_size)   # A1: this op's derived ldr targets
        # P
        npv = gP.decode(rc_); pw = 0; pres = []
        for _ in range(npv): pw += gP.decode(rc_); pres.append(pw)
        for off in pres:
            tp = tp0 + off
            if 0 <= tp < from_size:
                if tp not in J: J[tp] = buf[tp]
                if len(J) > peak[0]: peak[0] = len(J)
        # C
        ncv = gC.decode(rc_); cmap = {}; pt = 0
        for _ in range(ncv):
            pt += gC.decode(rc_); bb = M['dval'].t.decode(rc_)
            cmap[pt] = bb
        # ---- INLINE write-order field detection + streaming write (no override buffer). bl/ex deltas
        # pulled from the up-front per-stream cursors in detection order (C: O(1) lazy generators). ----
        sc.begin_op(tp0, dl, el)
        nl = read_uleb()
        def pull_delta(kind):
            D = M['hy_dd'][kind]; dic = D['dic']
            ri = D['rh'] | (2 if D['last'] == 0 else 0)
            rb = rc_.decode_bit(D['rep'], ri); D['rh'] = rb
            if rb == 1: return D['last']
            if rc_.decode_bit(D['hit'], 0) == 1:
                j = dec_int(rc_, M, 'hy_di_' + kind) + 1; v = dic[j]
                if j: dic.insert(0, dic.pop(j))
            else:
                v = M['dval'].decode(rc_); dic.insert(0, v)
            D['last'] = v; return v
        def field_at(ks, pure):
            """Return (kind, packed4) if a de-reloc field starts at ks (consuming its INLINE delta);
            else None. The delta is pulled from the SINGLE range stream the moment a field is detected
            (no resident store) — the encoder emitted it at this exact content-consumption point.
            A1: ex (ldr) is detected by DERIVE (fpk in pred_set) gated by `pure` (no literal patch in
            the 4 bytes) — mirroring the encoder's pure(k) test; positions are no longer shipped."""
            fpk = fp0 + ks
            if is_local_bl(frm_snap, fpk, from_size):
                if not pure: return ('sbl', None)
                delta = pull_delta('bl')
                up = frm_snap[fpk] | (frm_snap[fpk + 1] << 8); lo = frm_snap[fpk + 2] | (frm_snap[fpk + 3] << 8)
                return ('bl', pack_bl_bytes(unpack_bl(up, lo) - delta))
            if pure and fpk in op_ldr and fp0 + ks + 4 <= from_size:
                delta = pull_delta('ex')
                val = (frm_snap[fpk] | (frm_snap[fpk + 1] << 8) | (frm_snap[fpk + 2] << 16) |
                       (frm_snap[fpk + 3] << 24))
                return ('ex', PACK['ldr']((val - delta) & 0xffffffff))
            return None
        if FWD:
            nextpos = -1; nlbyte = 0; li = 0
            if nl > 0:
                nextpos = read_uleb(); nlbyte = next_content_byte(); sc.advance(nlbyte)
            def take_db(k):
                nonlocal nextpos, nlbyte, li
                if k == nextpos:
                    db = nlbyte; li += 1
                    if li < nl: nextpos += read_uleb(); nlbyte = next_content_byte(); sc.advance(nlbyte)
                    else: nextpos = -1
                    return db
                return 0
            def wr_copy(k):   # normal copy: db + raw + corr
                buf[tp0 + k] = ((take_db(k) + rawsrc(fp0 + k)) + cmap.get(k, 0)) & 0xff
            k = 0
            while k < dl:
                if k + 4 <= dl:
                    pure_w = (nextpos < 0 or nextpos >= k + 4)   # no literal patch in [k,k+3]
                    fa = field_at(k, pure_w)
                    if fa is not None:
                        if fa[0] == 'sbl':
                            for b in range(4): wr_copy(k + b)
                        else:
                            for b in range(4): buf[tp0 + k + b] = (fa[1][b] + cmap.get(k + b, 0)) & 0xff
                        k += 4; continue
                wr_copy(k); k += 1
            for e in range(el):
                eb = next_content_byte(); sc.advance(eb)
                buf[tp0 + dl + e] = (eb + cmap.get(dl + e, 0)) & 0xff
        else:
            for e in range(el - 1, -1, -1):
                eb = next_content_byte(); sc.advance(eb)
                buf[tp0 + dl + e] = (eb + cmap.get(dl + e, 0)) & 0xff
            nextpos = -1; nlbyte = 0; li = 0
            if nl > 0:
                gap = read_uleb(); nextpos = dl - gap; nlbyte = next_content_byte(); sc.advance(nlbyte)
            def take_db_d(k):
                nonlocal nextpos, nlbyte, li
                if k == nextpos:
                    db = nlbyte; li += 1
                    if li < nl: gap = read_uleb(); nextpos -= gap; nlbyte = next_content_byte(); sc.advance(nlbyte)
                    else: nextpos = -1
                    return db
                return 0
            def wr_copy_d(k):
                buf[tp0 + k] = ((take_db_d(k) + rawsrc(fp0 + k)) + cmap.get(k, 0)) & 0xff
            k = dl - 1
            while k >= 0:
                ks = k - 3
                if ks >= 0:
                    pure_w = (nextpos < ks)   # no literal patch in [ks,ks+3] (nextpos<0 -> pure)
                    fa = field_at(ks, pure_w)
                    if fa is not None:
                        if fa[0] == 'sbl':
                            for b in range(3, -1, -1): wr_copy_d(ks + b)
                        else:
                            for b in range(3, -1, -1): buf[tp0 + ks + b] = (fa[1][b] + cmap.get(ks + b, 0)) & 0xff
                        k -= 4; continue
                wr_copy_d(k); k -= 1
    return None, peak[0]


def decode_v3(blob, frm, cfg7, opt=None):
    opt = _norm_opt(opt)
    crc_to = struct.unpack('<I', blob[-4:])[0]; b = blob[:-4]
    crc_from = struct.unpack('<I', b[:4])[0]
    if crc_from != (zlib.crc32(frm) & 0xffffffff):
        raise ValueError("CRC(from) mismatch")
    from_size, p = leb128_read(b, 4)
    if from_size != len(frm):
        raise ValueError("from_size mismatch")
    to_size, p = leb128_read(b, p); fp_end, p = leb128_read(b, p)
    body = b[p:]
    M = make_models(frm); M['hy_dd'] = _delta_models(); rc_ = BinDec(body); rc_._start()
    # --- STREAMED DELTAS: NO up-front DEREL store. Each field's delta is pulled INLINE from the range
    # stream at field detection (M['dval'].decode in field_at) — zero resident delta arrays. ---
    span = max(from_size, to_size); buf = bytearray(frm) + bytearray(span - from_size)
    out, peak = _decode_apply_hybrid(rc_, M, buf, from_size, to_size, fp_end, opt, cfg7)
    res = bytes(buf[:to_size])
    if (zlib.crc32(res) & 0xffffffff) != crc_to:
        raise ValueError("CRC(to) mismatch")
    return res, dict(jpeak=peak)
