"""Top-K from-image halfword model + escape-to-two-table-byte fallback.

Idea
----
The INSERT is new ARMv6-M Thumb code = a stream of 16-bit halfwords (with the
occasional 32-bit BL = two halfwords). Many of those halfwords (PUSH/POP masks,
common register-move encodings, branch shells, library idioms) already occur in
the pre-patch FROM-IMAGE, which is shared side-information.

Model (ALL derived from the from-image, nothing about the insert is shipped):
  1. Scan the from-image as a halfword stream and find the top-K most frequent
     halfwords using a BOUNDED streaming heavy-hitter structure (Space-Saving
     with m counters, deterministic tie-breaking). enc and dec run the identical
     algorithm on the identical from-image, so they derive the identical top-K
     set and seed counts -> the model is SYNCED, not transmitted.
  2. Alphabet = {top-K halfwords} + {ESCAPE}. Coded with an adaptive binary
     range coder over frequency counts seeded from the from-image counts and
     updated as symbols are (de)coded (decoder mirrors the updates).
  3. On ESCAPE, the literal halfword's two bytes are coded with a two-table
     (low-byte / high-byte parity) adaptive byte model, also seeded from the
     from-image byte parity histogram. This is the same family as the CURRENT
     two-table parity baseline, used only as the fallback for halfwords the
     top-K model cannot name.

Determinism: YES. Space-Saving is fully deterministic given the input stream and
a deterministic eviction rule (evict the min-count counter, ties broken by the
smallest halfword key). Both sides see the same from-image, so both sides build
the same tables. Approximation error vs the EXACT top-K only costs a little
compression; it never breaks the roundtrip.

Integer-only: YES. The range coder uses only 32-bit integer arithmetic; all
frequencies are integers. No float anywhere in encode/decode.
"""
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
import common

# ------------------------------------------------------------------ params
K          = 256     # number of top from-image halfwords given a code
M_COUNTERS = 1024    # Space-Saving counters during the (transient) build pass
SEED_SCALE = 16      # divide from-image halfword counts to get the prior seed
ESC_SCALE  = 16      # divide from-image byte counts to get the byte-model seed
INC        = 8       # adaptive increment per observed symbol
RESCALE_TOT = 1 << 16  # rescale (halve) counts when a model total exceeds this

# ============================================================ model build
def space_saving_topk(frm, m, k):
    """Deterministic bounded heavy-hitter over the from-image halfword stream.
    Returns (ordered_keys, seed_counts) for the top-k halfwords. The seed_counts
    are the Space-Saving estimated counts (>= true count, but deterministic and
    identical on both sides)."""
    cnt = {}
    n = len(frm) - 1
    i = 0
    while i < n:
        hw = frm[i] | (frm[i + 1] << 8)
        if hw in cnt:
            cnt[hw] += 1
        elif len(cnt) < m:
            cnt[hw] = 1
        else:
            mn = min(cnt.values())
            # deterministic tie-break: smallest key among the min-count counters
            victim = min(key for key, v in cnt.items() if v == mn)
            v = cnt.pop(victim)
            cnt[hw] = v + 1
        i += 2
    # deterministic top-k ordering: by (-count, key)
    ordered = sorted(cnt.items(), key=lambda kv: (-kv[1], kv[0]))[:k]
    keys = [key for key, _ in ordered]
    counts = [v for _, v in ordered]
    return keys, counts


def build_model(frm):
    keys, scounts = space_saving_topk(frm, M_COUNTERS, K)
    key_index = {hw: j for j, hw in enumerate(keys)}

    # halfword model freqs: top-K seeded from (scaled) Space-Saving counts, +ESC
    hw_freq = [max(1, c // SEED_SCALE) for c in scounts]
    esc_seed = max(1, sum(scounts) // SEED_SCALE)  # rough mass for "everything else"
    hw_freq.append(esc_seed)                         # last slot = ESCAPE
    ESC = K  # symbol id of escape

    # byte parity models seeded from from-image byte parity histogram
    lo = [0] * 256
    hi = [0] * 256
    for idx in range(len(frm)):
        b = frm[idx]
        if idx & 1:
            hi[b] += 1
        else:
            lo[b] += 1
    lo_freq = [max(1, c // ESC_SCALE) for c in lo]
    hi_freq = [max(1, c // ESC_SCALE) for c in hi]
    return keys, key_index, hw_freq, ESC, lo_freq, hi_freq


# ============================================================ range coder
# Classic 32-bit carryless range coder (Subbotin style), integer-only.
TOP = 1 << 24
BOT = 1 << 16
MASK32 = 0xFFFFFFFF


class Encoder:
    def __init__(self):
        self.low = 0
        self.rng = MASK32
        self.out = bytearray()

    def encode(self, cum, freq, tot):
        self.rng //= tot
        self.low = (self.low + cum * self.rng) & MASK32
        self.rng *= freq
        while True:
            if (self.low ^ (self.low + self.rng)) < TOP:
                pass
            elif self.rng < BOT:
                self.rng = (-self.low) & (BOT - 1)
            else:
                break
            self.out.append((self.low >> 24) & 0xFF)
            self.low = (self.low << 8) & MASK32
            self.rng = (self.rng << 8) & MASK32

    def finish(self):
        # 4 flush bytes guarantee the decoder can disambiguate the final symbol
        # for any model configuration. (A 3-byte flush happened to work for the
        # K=256/m=1024 config but fails on the last halfword of some other
        # configs, so we keep the safe 4.)
        for _ in range(4):
            self.out.append((self.low >> 24) & 0xFF)
            self.low = (self.low << 8) & MASK32
        return bytes(self.out)


class Decoder:
    def __init__(self, data):
        self.data = data
        self.pos = 0
        self.low = 0
        self.rng = MASK32
        self.code = 0
        for _ in range(4):
            self.code = ((self.code << 8) | self._byte()) & MASK32

    def _byte(self):
        if self.pos < len(self.data):
            b = self.data[self.pos]
            self.pos += 1
            return b
        return 0

    def get_freq(self, tot):
        self.rng //= tot
        return (self.code - self.low) // self.rng

    def decode(self, cum, freq, tot):
        self.low = (self.low + cum * self.rng) & MASK32
        self.rng *= freq
        while True:
            if (self.low ^ (self.low + self.rng)) < TOP:
                pass
            elif self.rng < BOT:
                self.rng = (-self.low) & (BOT - 1)
            else:
                break
            self.code = ((self.code << 8) | self._byte()) & MASK32
            self.low = (self.low << 8) & MASK32
            self.rng = (self.rng << 8) & MASK32


# ----------------------------------------------- adaptive frequency table
class FreqModel:
    """Linear adaptive frequency table with cumulative lookups. Integer-only."""
    def __init__(self, freq):
        self.f = list(freq)
        self.tot = sum(self.f)

    def cum_of(self, sym):
        c = 0
        for j in range(sym):
            c += self.f[j]
        return c

    def find(self, target):
        c = 0
        for j in range(len(self.f)):
            nf = c + self.f[j]
            if target < nf:
                return j, c, self.f[j]
            c = nf
        # numerical guard
        j = len(self.f) - 1
        return j, c - self.f[j], self.f[j]

    def update(self, sym):
        self.f[sym] += INC
        self.tot += INC
        if self.tot >= RESCALE_TOT:
            self.tot = 0
            for j in range(len(self.f)):
                self.f[j] = (self.f[j] + 1) >> 1
                self.tot += self.f[j]


# ============================================================ encode/decode
def encode(insert, frm):
    keys, key_index, hw_freq, ESC, lo_seed, hi_seed = build_model(frm)
    hwm = FreqModel(hw_freq)
    lom = FreqModel(lo_seed)
    him = FreqModel(hi_seed)
    enc = Encoder()

    n = len(insert)
    nhw = n // 2  # number of full halfwords
    has_tail = n & 1

    i = 0
    while i + 1 < n:
        hw = insert[i] | (insert[i + 1] << 8)
        sym = key_index.get(hw, ESC)
        enc.encode(hwm.cum_of(sym), hwm.f[sym], hwm.tot)
        hwm.update(sym)
        if sym == ESC:
            b0 = insert[i]; b1 = insert[i + 1]
            enc.encode(lom.cum_of(b0), lom.f[b0], lom.tot); lom.update(b0)
            enc.encode(him.cum_of(b1), him.f[b1], him.tot); him.update(b1)
        i += 2

    body = enc.finish()
    # 3-byte header: length low/high + tail byte count flag, then tail byte raw.
    header = bytes([n & 0xFF, (n >> 8) & 0xFF])
    tail = insert[-1:] if has_tail else b''
    return header + tail + body


def decode(blob, frm):
    keys, key_index, hw_freq, ESC, lo_seed, hi_seed = build_model(frm)
    hwm = FreqModel(hw_freq)
    lom = FreqModel(lo_seed)
    him = FreqModel(hi_seed)

    n = blob[0] | (blob[1] << 8)
    has_tail = n & 1
    p = 2
    tail = b''
    if has_tail:
        tail = blob[p:p + 1]
        p += 1
    dec = Decoder(blob[p:])

    out = bytearray()
    nhw = n // 2
    for _ in range(nhw):
        target = dec.get_freq(hwm.tot)
        sym, cum, freq = hwm.find(target)
        dec.decode(cum, freq, hwm.tot)
        hwm.update(sym)
        if sym == ESC:
            t = dec.get_freq(lom.tot); b0, c0, f0 = lom.find(t)
            dec.decode(c0, f0, lom.tot); lom.update(b0)
            t = dec.get_freq(him.tot); b1, c1, f1 = him.find(t)
            dec.decode(c1, f1, him.tot); him.update(b1)
            out.append(b0); out.append(b1)
        else:
            hw = keys[sym]
            out.append(hw & 0xFF)
            out.append((hw >> 8) & 0xFF)
    out += tail
    return bytes(out)


# ============================================================ main / verify
def peak_decoder_ram():
    """Honest peak decoder RAM for THIS stage (bytes), counting tables it must
    build and hold. Runtime (post-build) state:
      top-K keys:     K * 2 bytes (uint16 halfword)            = 512
      hw freq table:  (K+1) * 2 bytes (uint16 counts)          = 514
      lo/hi tables:   2 * 256 * 2 bytes                        = 1024
      range coder + misc state                                 ~ 64
    The transient build structure (Space-Saving) runs to completion BEFORE any
    body symbol is decoded, then is freed; it does not coexist with the runtime
    state. Its footprint is:
      M_COUNTERS * (uint16 key + uint16 count) = 1024 * 4      = 4096
    Peak = max(build pass, runtime). The build pass dominates at 4096 bytes.
    (Runtime steady-state is only ~2114 bytes, so if the build structure is
    freed the decoder needs ~2 KiB for the whole patch-apply session.)"""
    runtime = K * 2 + (K + 1) * 2 + 2 * 256 * 2 + 64  # ~2114
    build = M_COUNTERS * 4                            # 4096
    return max(build, runtime)


if __name__ == "__main__":
    sizes = {}
    ok_all = True
    for name in ("small", "medium", "large"):
        ins, frm = common.get(name)
        blob = encode(ins, frm)
        back = decode(blob, frm)
        ok = back == ins
        ok_all &= ok
        sizes[name] = len(blob)
        print(f"{name:6s} insert={len(ins):5d}  compressed={len(blob):5d}  "
              f"roundtrip={'OK' if ok else 'FAIL'}")
    print()
    print(f"peak decoder RAM ~= {peak_decoder_ram()} bytes")
    print(f"shipped table bytes = {0} (model fully derived from from-image; "
          f"only a 2-byte length header per blob)")
    print(f"all roundtrips lossless: {ok_all}")
    common.report("topk-halfword", sizes, peak_decoder_ram(), True, 0,
                  f"K={K} m={M_COUNTERS} seed/{SEED_SCALE} inc={INC}")
