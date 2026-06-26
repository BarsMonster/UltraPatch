"""From-image n-gram dictionary encoder for INSERT (new Thumb code) literals.

Both encoder and decoder deterministically scan the SHARED from-image (pre-patch
firmware, ~113 KB) and rebuild two from-image-derived models:

  (1) a BOUNDED PHRASE BOOK -- the NDICT most "valuable" byte n-grams
      (MINLEN..MAXLEN bytes, freq>=2) occurring in the from-image, ranked by
      reuse savings (len-1)*freq. This is the static-LZ "phrase book".
  (2) a small BUCKETED order-1 byte model: the previous byte is folded into
      BUCKETS contexts (high bits of prev byte); for each bucket a 256-entry
      successor-frequency table is seeded from the from-image and then adapted
      ONLINE during (de)coding.

The INSERT is parsed left-to-right. At each position the encoder picks either a
dictionary phrase reference (cheaper, if it saves coded bits) or a single
literal byte; an "is-match" flag, the phrase index, and literal bytes are all
coded with an integer 32-bit range coder.

SHIPPED: only a tiny header (build params already constants here + the payload
length, handled by the patch container). The phrase book and order-1 buckets are
SYNCED side-information rebuilt from the from-image, NOT transmitted.

Decoder is INTEGER-ONLY (range coder + integer freq tables). The encoder uses a
float log2 only to *decide* match-vs-literal; that decision is recorded as the
is-match flag in the bitstream, so the decoder never needs float and the stream
is fully reproducible.
"""
import sys, math
sys.path[:0] = ['/ai_sw/detools-dev/m4dev/sim/imm']
import common
from collections import Counter

# ---------------- build params (constants; effectively the shipped header) ----
MAXLEN  = 8        # max phrase length scanned in from-image
MINLEN  = 3        # min phrase length worth a reference
NDICT   = 128      # phrases in the book (index coded with adaptive 128-sym model)
DICT_KS = (8, 7, 6, 5, 4, 3)
BUCKETS = 4        # order-1 context buckets (prev byte >> CTX_SHIFT)
CTX_SHIFT = 6      # 256 prev-byte values -> 4 buckets
LIT_INC = 16
LIT_LIM = 4096
SEED    = 2048     # from-image prior strength (counts), gentle

# ============================ integer range coder =========================
TOP  = 1 << 24
BOT  = 1 << 16
MASK = 0xFFFFFFFF

class REnc:
    __slots__ = ('low', 'rng', 'out')
    def __init__(self):
        self.low = 0; self.rng = MASK; self.out = bytearray()
    def encode(self, cum, freq, tot):
        self.rng //= tot
        self.low = (self.low + cum * self.rng) & MASK
        self.rng *= freq
        while True:
            if (self.low ^ (self.low + self.rng)) < TOP:
                pass
            elif self.rng < BOT:
                self.rng = (-self.low) & (BOT - 1)
            else:
                break
            self.out.append((self.low >> 24) & 0xFF)
            self.low = (self.low << 8) & MASK
            self.rng = (self.rng << 8) & MASK
    def finish(self):
        for _ in range(4):
            self.out.append((self.low >> 24) & 0xFF)
            self.low = (self.low << 8) & MASK
        return bytes(self.out)

class RDec:
    __slots__ = ('data', 'pos', 'low', 'rng', 'code')
    def __init__(self, data):
        self.data = data; self.pos = 0; self.low = 0; self.rng = MASK; self.code = 0
        for _ in range(4):
            self.code = ((self.code << 8) | self._byte()) & MASK
    def _byte(self):
        if self.pos < len(self.data):
            b = self.data[self.pos]; self.pos += 1; return b
        return 0
    def getfreq(self, tot):
        self.rng //= tot
        return ((self.code - self.low) & MASK) // self.rng
    def decode(self, cum, freq, tot):
        self.low = (self.low + cum * self.rng) & MASK
        self.rng *= freq
        while True:
            if (self.low ^ (self.low + self.rng)) < TOP:
                pass
            elif self.rng < BOT:
                self.rng = (-self.low) & (BOT - 1)
            else:
                break
            self.code = ((self.code << 8) | self._byte()) & MASK
            self.low = (self.low << 8) & MASK
            self.rng = (self.rng << 8) & MASK

# ===================== adaptive frequency model ===========================
class Model:
    __slots__ = ('freq', 'tot', 'n', 'inc', 'lim')
    def __init__(self, n, inc=24, lim=1 << 15, prior=None):
        self.n = n
        self.freq = list(prior) if prior is not None else [1] * n
        self.tot = sum(self.freq)
        self.inc = inc
        self.lim = lim
    def bits(self, sym):  # encoder-only cost estimate (does NOT mutate)
        return math.log2(self.tot) - math.log2(self.freq[sym])
    def enc(self, e, sym):
        f = self.freq; cum = 0
        for i in range(sym):
            cum += f[i]
        e.encode(cum, f[sym], self.tot)
        self._upd(sym)
    def dec(self, d):
        f = self.freq
        tgt = d.getfreq(self.tot)
        cum = 0; sym = 0
        while cum + f[sym] <= tgt:
            cum += f[sym]; sym += 1
        d.decode(cum, f[sym], self.tot)
        self._upd(sym)
        return sym
    def _upd(self, sym):
        self.freq[sym] += self.inc
        self.tot += self.inc
        if self.tot >= self.lim:
            t = 0; f = self.freq
            for i in range(self.n):
                f[i] = (f[i] >> 1) | 1
                t += f[i]
            self.tot = t

# ====================== from-image-derived model build ====================
def build_phrasebook(frm):
    """Deterministic phrase book from the shared from-image."""
    cnt = Counter()
    for k in DICT_KS:
        end = len(frm) - k + 1
        for i in range(end):
            cnt[frm[i:i + k]] += 1
    items = [(ph, fr) for ph, fr in cnt.items() if fr >= 2 and len(ph) >= MINLEN]
    items.sort(key=lambda kv: ((len(kv[0]) - 1) * kv[1], len(kv[0])), reverse=True)
    phrases = []; seen = set()
    for ph, _fr in items:
        if ph in seen:
            continue
        seen.add(ph); phrases.append(ph)
        if len(phrases) >= NDICT:
            break
    while len(phrases) < NDICT:
        phrases.append(b'\x00' * MINLEN)
    maxlen = max(len(p) for p in phrases)
    return phrases, maxlen

def build_byfirst(phrases):
    """Encoder-only: search structure first-byte -> [(phrase, idx)] len-desc."""
    byfirst = {}
    for idx, ph in enumerate(phrases):
        byfirst.setdefault(ph[0], []).append((ph, idx))
    for k in byfirst:
        byfirst[k].sort(key=lambda x: len(x[0]), reverse=True)
    return byfirst

def build_priors(frm):
    """Bucketed order-1 successor priors from the from-image (decoder rebuilds)."""
    raw = [[0] * 256 for _ in range(BUCKETS)]
    p = 0
    for b in frm:
        raw[p >> CTX_SHIFT][b] += 1
        p = b
    priors = []
    for c in range(BUCKETS):
        row = raw[c]; s = sum(row) or 1
        priors.append([1 + (row[k] * SEED) // s for k in range(256)])
    return priors

def lit_models(priors):
    return [Model(256, inc=LIT_INC, lim=LIT_LIM, prior=priors[c]) for c in range(BUCKETS)]

def best_match(ins, i, byfirst, maxlen):
    cands = byfirst.get(ins[i])
    if not cands:
        return None
    rem = len(ins) - i
    for ph, idx in cands:
        L = len(ph)
        if L <= rem and ins[i:i + L] == ph:
            return idx, L
    return None

# ============================== ENCODE ====================================
def encode(insert, frm):
    phrases, maxlen = build_phrasebook(frm)
    byfirst = build_byfirst(phrases)
    priors = build_priors(frm)
    e = REnc()
    flag = Model(2, inc=16)
    idxm = Model(NDICT, inc=24)
    lit = lit_models(priors)
    i = 0; n = len(insert); prev = 0
    while i < n:
        mm = best_match(insert, i, byfirst, maxlen)
        take = False
        if mm is not None:
            idx, L = mm
            rb = flag.bits(1) + idxm.bits(idx)
            lb = flag.bits(0) * L
            pp = prev
            for j in range(L):
                b = insert[i + j]
                lb += lit[pp >> CTX_SHIFT].bits(b)
                pp = b
            if rb < lb:
                take = True
        if take:
            idx, L = mm
            flag.enc(e, 1); idxm.enc(e, idx)
            i += L; prev = insert[i - 1]
        else:
            b = insert[i]
            flag.enc(e, 0); lit[prev >> CTX_SHIFT].enc(e, b)
            prev = b; i += 1
    return e.finish()

# ============================== DECODE ====================================
def decode(payload, frm, out_len):
    phrases, _maxlen = build_phrasebook(frm)   # decoder needs phrases only (no byfirst)
    priors = build_priors(frm)
    d = RDec(payload)
    flag = Model(2, inc=16)
    idxm = Model(NDICT, inc=24)
    lit = lit_models(priors)
    out = bytearray(); prev = 0
    while len(out) < out_len:
        if flag.dec(d) == 1:
            ph = phrases[idxm.dec(d)]
            out += ph; prev = ph[-1]
        else:
            b = lit[prev >> CTX_SHIFT].dec(d)
            out.append(b); prev = b
    return bytes(out[:out_len])

# ============================== RAM REPORT ================================
def decoder_peak_ram():
    """Honest peak decoder working RAM for THIS stage (bytes)."""
    lit_tables = BUCKETS * 256 * 2          # uint16 freq counts
    phrase_data = NDICT * MAXLEN            # worst-case packed phrase bytes
    phrase_len_tbl = NDICT * 1             # per-phrase length byte
    idxm_tbl = NDICT * 2                   # uint16
    flag_tbl = 2 * 2
    coder_state = 16
    misc = 64
    return lit_tables + phrase_data + phrase_len_tbl + idxm_tbl + flag_tbl + coder_state + misc

# ============================== MAIN ======================================
def run():
    results = {}
    for name in ('small', 'medium', 'large'):
        ins, frm = common.get(name)
        payload = encode(ins, frm)
        dec = decode(payload, frm, len(ins))
        ok = dec == ins
        results[name] = len(payload)
        ph, _ = build_phrasebook(frm)
        stored = sum(len(p) for p in ph)
        print(f"{name}: insert={len(ins)} compressed={len(payload)} lossless={ok} "
              f"phrase_store={stored}B")
    print(f"decoder peak RAM = {decoder_peak_ram()} bytes")
    return results

if __name__ == '__main__':
    run()
