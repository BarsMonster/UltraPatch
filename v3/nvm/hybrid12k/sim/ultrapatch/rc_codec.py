"""ultrapatch range-coder GOLDEN MODEL (final design) — DIVIDE-FREE.
Encoder + STRUCTURAL decoder over one binary range stream (LZMA bound: bound=(range>>12)*p;
compare — NO division, required for Cortex-M0+). Reconstructs the to-image byte-exact
(decode(encode(x))==x). 256/256 byte-exact, +7.05% vs production.

All models are binary decisions, SEEDED from the production code's own optimal prior so
static==production (no degradation), with gentle LZMA adaptation (1/32 rate) that only improves:
  literal      : 8-level bit-tree per parity, node probs seeded from from-image histogram
  token_flag   : adaptive binary ORDER-2 (context = previous 2 flags)
  backref_len  : adaptive gamma 'g' (beats static Rice on len 3-7); backref_dist stays Rice
  token_count/block_n/exc_gap/exc_tpos : Rice 'r' with baked global k (11/7/7/14)
  lit_t1       : adaptation rate 1/16 (opcode byte clusters locally)
  backref_dist/len : seeded UGolomb 'r' (per-patch Rice k, shipped 4b)  -> static == Rice(k)
  span_len/positions/run_len/block_*/nblk/dict_size/nz/exc_* : UGolomb 'g' -> static == gamma
  dict_values  : pack_size bytes via adaptive byte bit-tree                -> static == pack_size
  dict_idx     : UGolomb 'g'
Optimal flush (deterministic: round low to trailing-zero bytes, strip them; decoder
zero-fills past EOF). Same wire STRUCTURE as codec_v2 (LZSS [A] + relocation [B]).
C decoder: c/rc_ultrapatch.c + c/rc_models.h (divide-free, ~5x faster than the divide form)."""
import sys,struct,zlib,math,os
_HERE=os.path.dirname(os.path.abspath(__file__))
sys.path[:0]=[os.path.dirname(_HERE),_HERE,
              '/ai_sw/detools-dev/m4dev/sim','/ai_sw/detools-dev/m4dev/sim/ultrapatch']
import codec
from codec import build_control,from_huff_dual,ctrl_tags,lz_optimal,parse_control,CtrlScanner,Op
from m4reloc import (_parse_blocks,_disasm,_segs,_topos,zz,unzz,leb128,leb128_read,
                      _active_streams,ORDER,PACK,DEFAULT_OPT)
KTOP=1<<24; PBIT=4096; PHALF=2048
# ---------- DIVIDE-FREE binary range coder (LZMA bound; 12-bit prob, 1/32 adaptation) ----------
class BinEnc:
    def __init__(s): s.low=0; s.range=0xFFFFFFFF; s.cache=0; s.csz=1; s.out=bytearray()
    def _sl(s):
        if s.low<0xFF000000 or s.low>0xFFFFFFFF:
            c=s.cache
            while True:
                s.out.append((c+(s.low>>32))&0xFF); c=0xFF; s.csz-=1
                if s.csz==0: break
            s.cache=(s.low>>24)&0xFF
        s.csz+=1; s.low=(s.low<<8)&0xFFFFFFFF
    def encode_bit(s,arr,idx,bit,rate=5):   # adaptive: arr[idx]=P(bit==0) in [1,4095]; bound=(range>>12)*p
        p=arr[idx]; bound=(s.range>>12)*p
        if bit==0: s.range=bound; arr[idx]=p+((PBIT-p)>>rate)
        else: s.low+=bound; s.range-=bound; arr[idx]=p-(p>>rate)
        while s.range<KTOP: s.range<<=8; s._sl()
    def encode_raw(s,bit):                  # uniform p=0.5 (shipped bits)
        bound=s.range>>1
        if bit==0: s.range=bound
        else: s.low+=bound; s.range-=bound
        while s.range<KTOP: s.range<<=8; s._sl()
    def flush(s):
        for _ in range(5): s._sl()
        return bytes(s.out)
    def flush_opt(s):                       # optimal flush: round low to trailing-zero bytes, strip them
        t=s.range.bit_length()-1; mask=(1<<t)-1
        if s.low & mask: s.low=(s.low+(1<<t))&~mask
        base=len(s.out)
        for _ in range(5): s._sl()
        while len(s.out)>base and s.out[-1]==0: s.out.pop()
        return bytes(s.out)
class BinDec:                               # matching divide-free decoder (reads past EOF as 0)
    def __init__(s,data): s.d=data; s.p=1; s.range=0xFFFFFFFF; s.code=0
    def _start(s):
        for _ in range(4): s.code=(s.code<<8)|s._b()
    def _b(s):
        v=s.d[s.p] if s.p<len(s.d) else 0; s.p+=1; return v
    def decode_bit(s,arr,idx,rate=5):
        p=arr[idx]; bound=(s.range>>12)*p
        if s.code<bound: s.range=bound; arr[idx]=p+((PBIT-p)>>rate); b=0
        else: s.code-=bound; s.range-=bound; arr[idx]=p-(p>>rate); b=1
        while s.range<KTOP: s.code=((s.code<<8)|s._b())&0xFFFFFFFF; s.range<<=8
        return b
    def decode_raw(s):
        bound=s.range>>1
        if s.code<bound: s.range=bound; b=0
        else: s.code-=bound; s.range-=bound; b=1
        while s.range<KTOP: s.code=((s.code<<8)|s._b())&0xFFFFFFFF; s.range<<=8
        return b

def _eb(rc,st,b): rc.encode_bit(st,0,b)     # binary bit model st=[prob]
def _db(rc,st): return rc.decode_bit(st,0)
def _put_bits(rc,v,nb):
    for sh in range(nb-1,-1,-1): rc.encode_raw((v>>sh)&1)
def _get_bits(rc,nb):
    v=0
    for _ in range(nb): v=(v<<1)|rc.decode_raw()
    return v

# ---- RAM-less static prefix codes via raw 0.5 bits (no model state). Used for the small
# [B] fields where adaptation doesn't pay (FIELD_AUDIT.md): keeps the M0+ decoder RAM low.
# Bit order mirrors codec.BW/BR exactly so the patterns are canonical Elias/Rice. ----
def _w_gamma(rc,m):                               # m>=1
    n=m.bit_length()-1
    for _ in range(n): rc.encode_raw(0)
    for i in range(n,-1,-1): rc.encode_raw((m>>i)&1)
def _r_gamma(rc):
    n=0
    while rc.decode_raw()==0: n+=1
    v=1
    for _ in range(n): v=(v<<1)|rc.decode_raw()
    return v
def w_gz(rc,x): _w_gamma(rc,x+1)                  # x>=0
def r_gz(rc): return _r_gamma(rc)-1
def w_dz(rc,x):                                   # x>=0  (Elias-delta of x+1)
    m=x+1; n=m.bit_length(); _w_gamma(rc,n)
    for i in range(n-2,-1,-1): rc.encode_raw((m>>i)&1)
def r_dz(rc):
    n=_r_gamma(rc); m=1
    for _ in range(n-1): m=(m<<1)|rc.decode_raw()
    return m-1
def w_rice(rc,x,k):                               # x>=0, q ones + 0 + k mantissa (MSB first)
    q=x>>k
    for _ in range(q): rc.encode_raw(1)
    rc.encode_raw(0)
    for i in range(k-1,-1,-1): rc.encode_raw((x>>i)&1)
def r_rice(rc,k):
    q=0
    while rc.decode_raw()==1: q+=1
    m=0
    for _ in range(k): m=(m<<1)|rc.decode_raw()
    return (q<<k)|m

class BitTree:                              # 256-symbol byte via 8 binary decisions; probs[1..255]
    __slots__=('p','rate')                  # rate: LZMA adaptation shift (5=1/32 default; 4=1/16 for lit_t1)
    def __init__(s,probs=None,rate=5): s.p=probs if probs is not None else [PHALF]*256; s.rate=rate
    def encode(s,rc,byte):
        m=1
        for i in range(7,-1,-1):
            bit=(byte>>i)&1; rc.encode_bit(s.p,m,bit,s.rate); m=(m<<1)|bit
    def decode(s,rc):
        m=1
        for _ in range(8): m=(m<<1)|rc.decode_bit(s.p,m,s.rate)
        return m-256

# ---------- seeded Golomb: adaptive unary prefix + adaptive mantissa (static==gamma/Rice) ----------
UG_CTX=7                                    # context cap (bounds decoder model arrays); A1 RAM lever (was 32->16->8->7). MUST match c/rc_models.h.
class UGolomb:
    def __init__(s,code,k=0): s.code=code; s.k=k; s.u={}; s.m={}
    def _u(s,pos): pos=pos if pos<UG_CTX else UG_CTX; return s.u.setdefault(pos,[PHALF])
    def _mb(s,cl,pos):
        cl=cl if cl<UG_CTX else UG_CTX; pos=pos if pos<UG_CTX else UG_CTX
        return s.m.setdefault((cl,pos),[PHALF])
    def encode(s,rc,v):
        if s.code=='r':
            q=v>>s.k; cl=q; mant=[(v>>b)&1 for b in range(s.k-1,-1,-1)]
        else:
            mm=v+1; B=mm.bit_length()-1; cl=B; mant=[(mm>>b)&1 for b in range(B-1,-1,-1)]
        for pos in range(cl): _eb(rc,s._u(pos),1)
        _eb(rc,s._u(cl),0)
        for pos,b in enumerate(mant): _eb(rc,s._mb(cl,pos),b)
    def decode(s,rc):
        cl=0
        while _db(rc,s._u(cl))==1: cl+=1
        if s.code=='r':
            v=cl<<s.k
            for pos in range(s.k): v|=_db(rc,s._mb(cl,pos))<<(s.k-1-pos)
            return v
        mm=1<<cl
        for pos in range(cl): mm|=_db(rc,s._mb(cl,pos))<<(cl-1-pos)
        return mm-1

def fit_k(vals):
    if not vals: return 0
    return min(range(0,10), key=lambda kk: sum((v>>kk)+1+kk for v in vals))

from detools.common import pack_size
class ByteVarint:                           # dict_values: pack_size bytes via adaptive byte bit-tree
    __slots__=('t',)
    def __init__(s): s.t=BitTree()          # uniform start == pack_size; adaptive <= it (no degradation)
    def encode(s,rc,v):
        for byte in pack_size(v): s.t.encode(rc,byte)
    def decode(s,rc):
        b0=s.t.decode(rc); sgn=b0&0x40; val=b0&0x3f; off=6
        while b0&0x80:
            b0=s.t.decode(rc); val|=(b0&0x7f)<<off; off+=7
        return -val if sgn else val

class Flag1:                                # order-2 token-flag: 4 contexts (prev 2 flags)
    def __init__(s): s.m=[[PHALF],[PHALF],[PHALF],[PHALF]]; s.h=0
    def encode(s,rc,b): _eb(rc,s.m[s.h],b); s.h=((s.h<<1)|b)&3
    def decode(s,rc): b=_db(rc,s.m[s.h]); s.h=((s.h<<1)|b)&3; return b

# ---------- literal bit-tree seed: node P(bit==0)=P(left subtree) from from-image hist ----------
def _tree_probs(hist):
    w=[0]*512
    for sym in range(256): w[256+sym]=hist[sym]
    for m in range(255,0,-1): w[m]=w[2*m]+w[2*m+1]
    p=[PHALF]*256
    for m in range(1,256):
        num=w[2*m]; den=w[m]
        pr=(2*PBIT*num+den)//(2*den) if den else PHALF      # round(4096*P(left)), portable integer
        p[m]=min(PBIT-1,max(1,pr))
    return p
def lit_tree_seed(frm):
    f0=[1]*256; f1=[1]*256
    for i,b in enumerate(frm): (f1 if (i&1) else f0)[b]+=1
    return _tree_probs(f0),_tree_probs(f1)

RICE_FIELDS={'backref_dist'}                 # primed by a per-patch Rice k (shipped 4b)
# token_count: short [A] field, Rice-shaped -> baked global k. (block_n/exc_gap/exc_tpos are
# now RAM-less STATIC Rice in [B], see encode_B/decode_B — not adaptive UGolomb.)
RICE_KCONST={'token_count':11}
# [B] hybrid (FIELD_AUDIT.md): keep adaptive UGolomb only where it pays (positions, dict_idx,
# run_len); the other 8 [B] fields use RAM-less static codes -> [B] model RAM 24.8 KB -> ~7 KB.
def make_models(frm):
    p0,p1=lit_tree_seed(frm)
    # lit_t1 (high/opcode byte) clusters locally -> faster 1/16 adaptation (-9.1 KB/corpus).
    return dict(frm=frm, lit=[BitTree(p0,5),BitTree(p1,4)], flag=Flag1(),
                ints={}, k={}, dval=ByteVarint())
def _imodel(M,key):
    m=M['ints'].get(key)
    if m is None:
        if key in RICE_FIELDS: m=UGolomb('r',M['k'][key])
        elif key in RICE_KCONST: m=UGolomb('r',RICE_KCONST[key])
        else: m=UGolomb('g')
        M['ints'][key]=m
    return m
def enc_int(rc,M,key,v): _imodel(M,key).encode(rc,v)
def dec_int(rc,M,key): return _imodel(M,key).decode(rc)

# ---------- [A] control stream ----------
def encode_A(rc,M,ops,opt):
    ctrl=build_control(ops,opt)
    (_,L0),(_,L1)=from_huff_dual(M['frm']); Ls=(L0,L1)
    tags=ctrl_tags(ctrl,opt)
    litbits=[Ls[tags[p]][ctrl[p]] for p in range(len(ctrl))]
    Wuse=opt.get('W',codec.W)
    seq=lz_optimal(ctrl,litbits,win=(1<<Wuse),wbits=Wuse)
    kd=fit_k([x[1]-1 for x in seq if x[0]=='R'])   # backref_len now uses adaptive gamma (no kl)
    M['k']={'backref_dist':kd}
    enc_int(rc,M,'token_count',len(seq)); _put_bits(rc,kd,4); pos=0
    for x in seq:
        if x[0]=='S':
            M['flag'].encode(rc,0); enc_int(rc,M,'span_len',len(x[1])-1)
            for byte in x[1]: M['lit'][tags[pos]].encode(rc,byte); pos+=1
        else:
            M['flag'].encode(rc,1); enc_int(rc,M,'backref_dist',x[1]-1)
            enc_int(rc,M,'backref_len',x[2]-3); pos+=x[2]

def decode_A(rc,M,opt):
    sc=CtrlScanner(leb=bool(opt.get('leb')),noadj=bool(opt.get('adj_out')))
    nseq=dec_int(rc,M,'token_count'); M['k']={'backref_dist':_get_bits(rc,4)}
    ctrl=bytearray()
    for _ in range(nseq):
        if M['flag'].decode(rc)==0:
            ln=dec_int(rc,M,'span_len')+1
            for _ in range(ln):
                t=sc.next_tag(); b=M['lit'][t].decode(rc); ctrl.append(b); sc.advance(b)
        else:
            dist=dec_int(rc,M,'backref_dist')+1; length=dec_int(rc,M,'backref_len')+3
            if dist < 1 or dist > len(ctrl):
                raise ValueError("invalid backref distance")
            st=len(ctrl)-dist
            for k in range(length): b=ctrl[st+k]; ctrl.append(b); sc.advance(b)
    return bytes(ctrl)

# ---------- [B] relocation streams ----------
from collections import Counter
def encode_B(rc,M,ops,dfpatch,frm,opt):
    blocks,vals,cfg7=_parse_blocks(dfpatch)
    ranges=(cfg7[1],cfg7[2],cfg7[3],cfg7[5],cfg7[6]); streams=_active_streams(opt)
    fmaps=_disasm(frm,ranges,streams); segs=_segs(ops); voff=0
    for nm in ORDER:
        cnt=sum(b[2] for b in blocks[nm]); sv=vals[voff:voff+cnt]; voff+=cnt
        if nm not in streams: assert not any(sv); continue
        sf=sorted(fmaps[nm])
        w_gz(rc,len(blocks[nm])); prev_end=0                            # nblk: static gamma
        for (fo,ta,n) in blocks[nm]:
            w_dz(rc,zz(fo-prev_end)); w_rice(rc,n-1,7); prev_end=fo+n   # block_gap: static delta; block_n: static Rice k7
        nz=[v for v in sv if v]; N=len(nz)
        dic=[t for t,_ in Counter(nz).most_common()]; idx={t:i for i,t in enumerate(dic)}
        w_dz(rc,len(dic))                                                # dict_size: static delta
        for t in dic: M['dval'].encode(rc,t)
        w_dz(rc,N); run=0                                                # nz_count: static delta
        for v in sv:
            if v==0: run+=1
            else: enc_int(rc,M,'positions',run); run=0                   # positions: ADAPTIVE
        runs=[]; i=0
        while i<N:
            v=nz[i]; rl=0
            while i<N and nz[i]==v: rl+=1; i+=1
            runs.append((idx[v],rl))
        for (a,rl) in runs: enc_int(rc,M,'dict_idx',a); enc_int(rc,M,'run_len',rl-1)  # dict_idx,run_len: ADAPTIVE
        exc=[]; capseq=0
        for (fo,ta,n) in blocks[nm]:
            base=sf[fo]
            for k in range(n):
                faddr=sf[fo+k]; det=ta+faddr-base; der=_topos(segs,faddr)
                if der!=det: exc.append((capseq,det))
                capseq+=1
        w_gz(rc,len(exc)); pe=0; pt=0                                    # exc_count: static gamma
        for (cs,tpos) in exc:
            w_rice(rc,cs-pe,7); w_rice(rc,zz(tpos-pt),14); pe=cs; pt=tpos  # exc_gap: Rice k7; exc_tpos: Rice k14

def decode_B(rc,M,ops,frm,cfg7,opt,to_size):
    dp,fdo,fdb,fde,cp,fcb,fce=cfg7; ranges=(fdo,fdb,fde,fcb,fce); streams=_active_streams(opt)
    fmaps=_disasm(frm,ranges,streams); segs=_segs(ops)
    fromzero=bytearray(frm); dfdiff=bytearray(to_size)
    for nm in ORDER:
        if nm not in streams: continue
        sf=sorted(fmaps[nm]); nblk=r_gz(rc); gapn=[]; cnt=0                     # nblk: static gamma
        for _ in range(nblk):
            gap=unzz(r_dz(rc)); nn=r_rice(rc,7)+1; gapn.append((gap,nn)); cnt+=nn  # block_gap: delta; block_n: Rice k7
        K=r_dz(rc); dic=[M['dval'].decode(rc) for _ in range(K)]                # dict_size: static delta
        N=r_dz(rc); gaps=[dec_int(rc,M,'positions') for _ in range(N)]          # nz_count: static delta; positions: ADAPTIVE
        nzvals=[]
        while len(nzvals)<N:
            di=dec_int(rc,M,'dict_idx'); rl=dec_int(rc,M,'run_len')+1; nzvals.extend([dic[di]]*rl)  # ADAPTIVE
        nzvals=nzvals[:N]; sv=[]
        for k in range(N): sv.extend([0]*gaps[k]); sv.append(nzvals[k])
        sv.extend([0]*(cnt-len(sv))); sv=sv[:cnt]
        nexc=r_gz(rc); excmap={}; cs=0; pt=0                                    # exc_count: static gamma
        for _ in range(nexc):
            cs+=r_rice(rc,7); tpos=pt+unzz(r_rice(rc,14)); excmap[cs]=tpos; pt=tpos  # exc_gap k7; exc_tpos k14
        di2=0; capseq=0; pos=0
        for (gap,nn) in gapn:
            pos+=gap
            for k in range(nn):
                faddr=sf[pos+k]; fval=fmaps[nm][faddr]; delta=sv[di2]; di2+=1
                fromzero[faddr:faddr+4]=b'\x00\x00\x00\x00'
                der=_topos(segs,faddr); dst=excmap.get(capseq,der); capseq+=1
                if dst is not None: dfdiff[dst:dst+4]=PACK[nm]((fval-delta)&0xffffffff)
            pos+=nn
    return fromzero,dfdiff

# ---------- top-level ----------
import subprocess,os
from m4_oracle import _read_buffers
DET=os.path.expanduser('~/.local/bin/detools')
def encode(F,T,felf=None,telf=None,fbin=None,tbin=None,opt=None):
    if opt is None: opt=dict(DEFAULT_OPT)
    fbin=fbin or f'{F}/watch.bin'; tbin=tbin or f'{T}/watch.bin'
    felf=felf or f'{F}/watch.elf'; telf=telf or f'{T}/watch.elf'
    p='/tmp/_rce_%d.patch'%os.getpid()
    try:
        subprocess.run([DET,'create_patch','-t','sequential','-a','bsdiff','--data-format','arm-cortex-m4',
          '--from-elf-file',felf,'--to-elf-file',telf,'-c','none',fbin,tbin,p],check=True,capture_output=True)
        d=_read_buffers(fbin,p)
    finally:
        if os.path.exists(p): os.remove(p)
    ops=[Op(o.diff_len,o.diff,o.extra,o.adj) for o in d['ops']]; frm=d['from_image']
    with open(tbin,'rb') as f: crc_to=zlib.crc32(f.read())&0xffffffff
    _,_,cfg7=_parse_blocks(d['dfpatch'])
    M=make_models(frm); rc=BinEnc()
    encode_A(rc,M,ops,opt); encode_B(rc,M,ops,d['dfpatch'],frm,opt)
    hdr=struct.pack('<I',zlib.crc32(frm)&0xffffffff)+leb128(len(frm)); tail=struct.pack('<I',crc_to&0xffffffff)
    body=rc.flush() if opt.get('flush5') else rc.flush_opt()
    return hdr+body+tail,cfg7

def decode(blob,frm,cfg7,opt=None):
    if opt is None: opt=dict(DEFAULT_OPT)
    crc_to=struct.unpack('<I',blob[-4:])[0]; b=blob[:-4]
    crc_from=struct.unpack('<I',b[:4])[0]
    if crc_from!=(zlib.crc32(frm)&0xffffffff):
        raise ValueError("CRC(from) mismatch")
    from_size,p=leb128_read(b,4)
    if from_size!=len(frm):
        raise ValueError("from_size mismatch")
    body=b[p:]
    M=make_models(frm); rc=BinDec(body); rc._start()
    ctrl=decode_A(rc,M,opt); ops=parse_control(ctrl,opt)
    to_size=sum(o.diff_len+len(o.extra) for o in ops)
    fromzero,dfdiff=decode_B(rc,M,ops,frm,cfg7,opt,to_size)
    out=bytearray(to_size); fpos=0; tp=0
    for o in ops:
        for k in range(o.diff_len): out[tp]=(o.diff[k]+fromzero[fpos]+dfdiff[tp])&0xff; tp+=1; fpos+=1
        for k in range(len(o.extra)): out[tp]=(o.extra[k]+dfdiff[tp])&0xff; tp+=1
        fpos+=o.adj
    if (zlib.crc32(bytes(out))&0xffffffff)!=crc_to:
        raise ValueError("CRC(to) mismatch")
    return bytes(out)
