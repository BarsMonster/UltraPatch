"""ultrapatch golden-model codecs (Python). Round-trip primitives + [B] reloc.
Correctness rule: every encode() must satisfy decode(encode(x)) == x exactly."""
import sys
from io import BytesIO
sys.path[:0]=['/ai_sw/detools-dev/m4dev/sim','/ai_sw/detools-dev/m4dev/sim/algos']
from detools.common import pack_size, unpack_size

# ---------- bit IO (MSB-first) ----------
class BW:
    def __init__(s): s.b=bytearray(); s.cur=0; s.nb=0
    def bit(s,v):
        s.cur=(s.cur<<1)|(v&1); s.nb+=1
        if s.nb==8: s.b.append(s.cur); s.cur=0; s.nb=0
    def bits(s,v,k):
        for i in range(k-1,-1,-1): s.bit((v>>i)&1)
    def gamma(s,m):            # m>=1
        n=m.bit_length()-1
        for _ in range(n): s.bit(0)
        for i in range(n,-1,-1): s.bit((m>>i)&1)
    def gz(s,x): s.gamma(x+1)   # x>=0
    def delta(s,m):            # Elias delta, m>=1
        n=m.bit_length(); s.gamma(n)
        for i in range(n-2,-1,-1): s.bit((m>>i)&1)
    def dz(s,x): s.delta(x+1)   # x>=0
    def out(s):
        if s.nb: s.b.append(s.cur<<(8-s.nb))
        return bytes(s.b)
class BR:
    def __init__(s,data): s.d=data; s.p=0
    def bit(s):
        b=(s.d[s.p>>3]>>(7-(s.p&7)))&1; s.p+=1; return b
    def bits(s,k):
        v=0
        for _ in range(k): v=(v<<1)|s.bit()
        return v
    def gamma(s):
        n=0
        while s.bit()==0: n+=1
        v=1
        for _ in range(n): v=(v<<1)|s.bit()
        return v
    def gz(s): return s.gamma()-1
    def delta(s):
        n=s.gamma(); m=1
        for _ in range(n-1): m=(m<<1)|s.bit()
        return m
    def dz(s): return s.delta()-1

def uleb(v):
    out=bytearray()
    while True:
        b=v&0x7f; v>>=7; out.append(b|0x80 if v else b)
        if not v: return bytes(out)
def _zz(v): return (v<<1) if v>=0 else ((-v)<<1)-1
def _unzz(u): return (u>>1) if (u&1)==0 else -((u+1)>>1)

# ---------- [B] per-stream value codec: freq-dict + double-RLE (used by codec_v2) ----------
def _enc_stream(bw, stream, opt=None):
    """One value stream, true DOUBLE-RLE (spec §3): freq-ordered dict (zigzag varint),
    then positions and values as SEPARATE sub-streams. opt['e6'] adds a per-stream
    adaptive dict-index code (4-bit selector: Rice k 0..9, or 15=gz)."""
    from collections import Counter
    nz=[v for v in stream if v]; N=len(nz)
    dic=[s for s,_ in Counter(nz).most_common()]; idx={s:i for i,s in enumerate(dic)}
    bw.gamma(len(dic)+1)
    for s in dic:
        for byteval in pack_size(s): bw.bits(byteval,8)
    bw.gamma(N+1)
    run=0                                       # positions: gap (zeros) before each nonzero
    for v in stream:
        if v==0: run+=1
        else: bw.gz(run); run=0                 # trailing zeros are implicit
    runs=[]; i=0                                 # values: RLE over the nonzero-only seq
    while i<N:
        v=nz[i]; rl=0
        while i<N and nz[i]==v: rl+=1; i+=1
        runs.append((idx[v],rl))
    if opt and opt.get('e6') and N>=16:
        ix=[a for a,_ in runs]
        def _gzl(x): return 2*(x+1).bit_length()-1
        def _csum(code): return (sum(_gzl(x) for x in ix) if code==15 else sum((x>>code)+1+code for x in ix))
        sel=min([15]+list(range(10)), key=_csum) if ix else 15
        bw.bits(sel,4)
        for (a,rl) in runs:
            (bw.gz(a) if sel==15 else _wr_rice(bw,a,sel)); bw.gamma(rl)
    else:
        for (a,rl) in runs: bw.gz(a); bw.gamma(rl)

def _dec_stream(br, c, opt=None):
    K=br.gamma()-1; dic=[]
    for _ in range(K):
        b0=br.bits(8); is_signed=b0&0x40; val=b0&0x3f; off=6
        while b0&0x80:
            b0=br.bits(8); val|=(b0&0x7f)<<off; off+=7
        if is_signed: val=-val
        dic.append(val)
    N=br.gamma()-1
    gaps=[br.gz() for _ in range(N)]
    sel=br.bits(4) if (opt and opt.get('e6') and N>=16) else 15
    nzvals=[]
    while len(nzvals)<N:
        di=(br.gz() if sel==15 else _rd_rice(br,sel)); rl=br.gamma(); nzvals.extend([dic[di]]*rl)
    nzvals=nzvals[:N]
    out=[]
    for k in range(N): out.extend([0]*gaps[k]); out.append(nzvals[k])
    out.extend([0]*(c-len(out)))
    return out[:c]

# ---------- control op-stream (de)serialize ----------
class Op:
    __slots__=('diff_len','diff','extra','adj')
    def __init__(s,dl,d,e,a): s.diff_len=dl; s.diff=d; s.extra=e; s.adj=a
def build_control(ops, opt=None):
    leb=bool(opt and opt.get('leb')); w=bytearray()
    pu=(lambda v: w.extend(uleb(v))) if leb else (lambda v: w.extend(pack_size(v)))
    ps=(lambda v: w.extend(uleb(_zz(v)))) if leb else (lambda v: w.extend(pack_size(v)))
    pu(len(ops))
    for op in ops:
        pu(op.diff_len)
        lits=[(k,b) for k,b in enumerate(op.diff) if b]
        pu(len(lits)); prev=0
        for k,b in lits: pu(k-prev); w.append(b); prev=k
        pu(len(op.extra)); w+=op.extra
        if not (opt and opt.get('adj_out')): ps(op.adj)
    return bytes(w)
def parse_control(buf, opt=None):
    leb=bool(opt and opt.get('leb')); fp=BytesIO(buf)
    if leb:
        def U():
            v=0; sh=0
            while True:
                b=fp.read(1)[0]; v|=(b&0x7f)<<sh; sh+=7
                if not (b&0x80): return v
        US=lambda: _unzz(U())
    else:
        U=lambda:unpack_size(fp); US=U
    nops=U(); ops=[]
    for _ in range(nops):
        dl=U(); nl=U(); diff=bytearray(dl); prev=0
        for _ in range(nl):
            prev+=U(); diff[prev]=fp.read(1)[0]
        el=U(); extra=fp.read(el)
        adj=0 if (opt and opt.get('adj_out')) else US()
        ops.append(Op(dl,bytes(diff),extra,adj))
    return ops

# ---------- canonical Huffman from a 256-byte histogram (Laplace+1, len<=15) ----------
import heapq
def huff_lengths(freq):
    # deterministic: leaves 0..255, merge two smallest by (weight,id); tie-break by id.
    N=256
    weight=list(freq); parent=[-1]*(2*N); wt=list(freq)+[0]*N
    active=list(range(N)); m=N
    while len(active)>1:
        # two smallest by (weight,id)
        a=b=-1
        for nid in active:
            if a<0 or (wt[nid],nid)<(wt[a],a): b=a; a=nid
            elif b<0 or (wt[nid],nid)<(wt[b],b): b=nid
        wt[m]=wt[a]+wt[b]; parent[a]=m; parent[b]=m
        active.remove(a); active.remove(b); active.append(m); m+=1
    L={}
    for s in range(N):
        d=0; p=s
        while parent[p]!=-1: p=parent[p]; d+=1
        L[s]=d
    return L
def canon(L):
    # canonical codes from lengths
    syms=sorted(L, key=lambda s:(L[s],s)); code=0; prev=0; enc={}
    for s in syms:
        l=L[s]; code<<=(l-prev); prev=l; enc[s]=(code,l); code+=1
    return enc
def from_huff_dual(frm):
    """Two from-image canonical Huffman tables (spec §2): table 0 = low byte of each
    halfword (even from-addresses frm[0::2]), table 1 = high byte (odd frm[1::2])."""
    f0=[1]*256; f1=[1]*256
    for i,b in enumerate(frm):
        (f1 if (i&1) else f0)[b]+=1
    L0=huff_lengths(f0); L1=huff_lengths(f1)
    return (canon(L0),L0),(canon(L1),L1)

# ---------- control-stream table-selection scanner (identical enc/dec) ----------
# Each emitted control byte gets a table id: 1 only for an EXTRA (new-code) byte at an
# odd to-address, else 0 (low table). The decoder runs this incrementally as it
# reconstructs the control stream, so no per-byte tag is shipped.
class CtrlScanner:
    __slots__=('ph','acc','shift','first','ops','lits','exleft','expos','exstart','tp','dl','leb','noadj')
    def __init__(s, leb=False, noadj=False):
        s.ph='NOPS'; s.acc=0; s.shift=0; s.first=True; s.leb=leb; s.noadj=noadj
        s.ops=0; s.lits=0; s.exleft=0; s.expos=0; s.exstart=0; s.tp=0; s.dl=0
    def next_tag(s):
        return ((s.exstart+s.expos)&1) if s.ph=='EXTRA' else 0
    def _vstart(s,b):
        if s.leb: s.acc=b&0x7f; s.shift=7
        else: s.acc=b&0x3f; s.shift=6
    def _vcont(s,b): s.acc|=(b&0x7f)<<s.shift; s.shift+=7
    def _opend(s): s.ops-=1; s.ph='DIFFLEN' if s.ops>0 else 'DONE'
    def _after_extra(s): s._opend() if s.noadj else setattr(s,'ph','ADJ')  # adj_out: no ADJ phase
    def advance(s,b):
        ph=s.ph
        if ph in ('NOPS','DIFFLEN','NLITS','GAP','EXTRALEN','ADJ'):
            if s.first: s._vstart(b); s.first=False
            else: s._vcont(b)
            if b&0x80: return                 # varint continues
            v=s.acc; s.first=True              # varint complete
            if ph=='NOPS':
                s.ops=v; s.ph='DONE' if v==0 else 'DIFFLEN'
            elif ph=='DIFFLEN':
                s.dl=v; s.exstart=s.tp+v; s.ph='NLITS'
            elif ph=='NLITS':
                s.lits=v; s.ph='GAP' if v>0 else 'EXTRALEN'
            elif ph=='GAP':
                s.ph='LITVAL'
            elif ph=='EXTRALEN':
                s.exleft=v; s.expos=0; s.tp=s.exstart+v
                if v>0: s.ph='EXTRA'
                else: s._after_extra()
            elif ph=='ADJ':
                s._opend()
        elif ph=='LITVAL':
            s.lits-=1; s.ph='GAP' if s.lits>0 else 'EXTRALEN'
        elif ph=='EXTRA':
            s.expos+=1; s.exleft-=1
            if s.exleft==0: s._after_extra()
def ctrl_tags(ctrl, opt=None):
    sc=CtrlScanner(leb=bool(opt and opt.get('leb')), noadj=bool(opt and opt.get('adj_out'))); tags=bytearray(len(ctrl))
    for p,b in enumerate(ctrl): tags[p]=sc.next_tag(); sc.advance(b)
    return tags

# ---------- [A]: LZSS-C (optimal parse, format-C pack) + from-image Huffman literals ----------
W=9   # baseline backref-distance bits; production uses Rice (opt['rice']) with opt['W']=11
def lz_optimal(data, litbits, win=512, maxm=2048, maxchain=64, maxrun=1024, wbits=9,
               rice_dist=False, rice_passes=2, match_mode='longest'):
    """Cost-aware optimal LZSS parse (encoder-only; produces the SAME token format the
    decoder replays, so no decoder change). litbits[p] = Huffman code length of the
    literal at position p (per-position: depends on its table id).
    By default this preserves the legacy fixed-distance cost: literal span =
    1+gamma(len)+sum(litbits), backref = 1+W+gamma(len). A1 can opt into an
    encoder-only approximation of its fitted Rice distance model and keep both the
    nearest and longest match candidates at each position."""
    n=len(data)
    def gammalen(x): return 2*x.bit_length()-1          # bits of elias gamma(x), x>=1
    ph=[0]*(n+1)
    for i in range(n): ph[i+1]=ph[i]+litbits[i]
    from collections import defaultdict
    chains=defaultdict(list); match=[None]*n
    for i in range(n):
        bl=bd=0; near=None
        if i+3<=n:
            cand=chains.get(data[i:i+3])
            if cand:
                c=0
                for pj in reversed(cand):
                    if i-pj>win or c>=maxchain: break
                    c+=1; l=0; ml=min(maxm,n-i)
                    while l<ml and data[pj+l]==data[i+l]: l+=1
                    if l>=3 and near is None: near=(i-pj,l)
                    if l>bl: bl=l; bd=i-pj
            chains[data[i:i+3]].append(i)
        cands=[]
        if match_mode=='near_long' and near: cands.append(near)
        if bl>=3 and (match_mode!='near_long' or near!=(bd,bl)): cands.append((bd,bl))
        match[i]=cands
    def parse(dist_bits):
        INF=1<<60; cost=[INF]*(n+1); cost[n]=0; nxt=[None]*(n+1)
        for i in range(n-1,-1,-1):
            best=INF; bc=None; runbits=0
            for j in range(i+1,min(n,i+maxrun)+1):
                runbits+=litbits[j-1]
                c=1+gammalen(j-i)+runbits+cost[j]
                if c<best: best=c; bc=('S',i,j)
            for bd,bl in match[i]:
                db=dist_bits(bd)
                for l in range(3,bl+1):
                    c=1+db+gammalen(l)+cost[i+l]
                    if c<best: best=c; bc=('R',bd,l)
            cost[i]=best; nxt[i]=bc
        toks=[]; i=0
        while i<n:
            ch=nxt[i]
            if ch[0]=='S': toks.append(("S",list(data[i:ch[2]]))); i=ch[2]
            else: toks.append(("R",ch[1],ch[2])); i+=ch[2]
        return toks
    seq=parse(lambda _d: wbits)
    if rice_dist:
        def fit(vals):
            if not vals: return 0
            return min(range(0,10), key=lambda kk: sum((v>>kk)+1+kk for v in vals))
        k=fit([x[1]-1 for x in seq if x[0]=='R'])
        for _ in range(rice_passes):
            seq=parse(lambda d, kk=k: ((d-1)>>kk)+1+kk)
            nk=fit([x[1]-1 for x in seq if x[0]=='R'])
            if nk==k: break
            k=nk
    return seq

def _ricelen(v,k): return (v>>k)+1+k
def _wr_rice(bw,v,k):
    q=v>>k
    for _ in range(q): bw.bit(1)
    bw.bit(0); bw.bits(v&((1<<k)-1),k)
def _rd_rice(br,k):
    q=0
    while br.bit()==1: q+=1
    return (q<<k)|br.bits(k)
def encodeA(ops, frm, opt=None):
    opt=opt or {}
    ctrl=build_control(ops, opt)
    (enc0,L0),(enc1,L1)=from_huff_dual(frm)
    tags=ctrl_tags(ctrl, opt)
    Ls=(L0,L1); encs=(enc0,enc1)
    litbits=[Ls[tags[p]][ctrl[p]] for p in range(len(ctrl))]
    Wuse=opt.get('W', W)
    seq=lz_optimal(ctrl, litbits, win=(1<<Wuse), wbits=Wuse)
    rice=opt.get('rice'); lenrice=opt.get('lenrice')   # E8: Rice-code backref length (len-3)
    bw=BW()
    if rice:                                   # fit k over the parse's backref distances, ship 4 bits
        dists=[s[1]-1 for s in seq if s[0]=='R']
        k=min(range(0,10), key=lambda kk: sum(_ricelen(d,kk) for d in dists)) if dists else 0
        bw.bits(k,4)
    if lenrice:                                # fit k over the parse's backref lengths (len-3), ship 4 bits
        lens=[s[2]-3 for s in seq if s[0]=='R']
        lk=min(range(0,10), key=lambda kk: sum(_ricelen(x,kk) for x in lens)) if lens else 0
        bw.bits(lk,4)
    bw.gamma(len(seq)+1)
    pos=0
    for s in seq:
        if s[0]=='S':
            bw.bit(0); bw.gamma(len(s[1]))
            for byte in s[1]:
                c,l=encs[tags[pos]][byte]; bw.bits(c,l); pos+=1
        else:
            bw.bit(1)
            if rice: _wr_rice(bw, s[1]-1, k)
            else: bw.bits(s[1]-1,W)
            if lenrice: _wr_rice(bw, s[2]-3, lk)
            else: bw.gamma(s[2]-1+1)
            pos+=s[2]
    return bw.out(), len(ctrl)
def decodeA_st(blob, frm, opt=None):
    """Self-terminating decodeA (v2): no shipped clen/lenA. Returns (ctrl_bytes,
    bytes_consumed) — [A] ends at the byte-aligned end of its nseq-token bitstream."""
    (enc0,L0),(enc1,L1)=from_huff_dual(frm)
    dec=[{},{}]
    for sym,(c,l) in enc0.items(): dec[0][(l,c)]=sym
    for sym,(c,l) in enc1.items(): dec[1][(l,c)]=sym
    opt=opt or {}
    sc=CtrlScanner(leb=bool(opt.get('leb')), noadj=bool(opt.get('adj_out')))
    rice=opt.get('rice'); lenrice=opt.get('lenrice')
    br=BR(blob)
    rk=br.bits(4) if rice else 0
    lk=br.bits(4) if lenrice else 0
    nseq=br.gamma()-1; out=bytearray()
    for _ in range(nseq):
        if br.bit()==0:
            ln=br.gamma()
            for _ in range(ln):
                d=dec[sc.next_tag()]; c=0; l=0
                while True:
                    c=(c<<1)|br.bit(); l+=1
                    if (l,c) in d: b=d[(l,c)]; break
                out.append(b); sc.advance(b)
        else:
            dist=(_rd_rice(br,rk) if rice else br.bits(W))+1
            if dist < 1 or dist > len(out):
                raise ValueError("invalid backref distance")
            ln=(_rd_rice(br,lk)+3) if lenrice else (br.gamma()-1+1); st=len(out)-dist
            for i in range(ln): b=out[st+i]; out.append(b); sc.advance(b)
    return bytes(out), (br.p+7)//8
