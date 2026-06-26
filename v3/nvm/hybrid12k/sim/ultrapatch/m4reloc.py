"""Shared m4 relocation / in-place reconstruction helpers (format-agnostic).

Used by the shipping range-coder golden model (rc_codec.py) and the field-audit tooling.
Provides: the relocation stream set + device config, ARM packers (bl/bw/s32 -> bytes),
zigzag / uLEB128, the from-image disassembly (_disasm), the op fp->tp segment map (_segs /
_topos), and the dfpatch block parser (_parse_blocks). No entropy coding lives here — that is
the codec's job (rc_codec.py). (Split out of the retired codec_v2.py; the legacy prefix-code
encoder/decoder was removed — see reference/ultrapatch_v2_results.md and git history.)"""
import io, struct
from detools.common import unpack_size
from detools.data_format.arm_cortex_m4 import disassemble

ORDER=['data','code','bw','bl','ldr','ldrw']
# Production default: the four Thumb-1/ARMv6-M (Cortex-M0+) relocation categories.
# B.W and LDR.W are Thumb-2-only and cannot occur on this core, so they are dropped
# (the disassembler is told not to emit them — see _disasm). Agreed out-of-band
# (encoder CLI flag / decoder #define) like the device ranges: 0 bytes on the wire.
DEFAULT_STREAMS=['data','code','bl','ldr']
# Format parameters shared by both codecs (LZSS window, the [A]/[B] structure switches).
# NOTE: rc_codec.py / codec.py prepend /ai_sw/detools-dev/m4dev/sim/ultrapatch to sys.path, so the
# AUTHORITATIVE m4reloc (and this DEFAULT_OPT) is loaded from THERE (read-only, W=11). This pathE
# copy is shadowed/unused. Path E's W=10 supersolution lever is injected in rc_v3.py (encode_v3/
# decode_v3 default opt) instead, so this stays at the upstream W=11 to avoid confusion.
DEFAULT_OPT={'leb':True,'rice':True,'lenrice':True,'W':11,'e5':True,'e6':True,'streams':DEFAULT_STREAMS}
def _active_streams(opt):
    """Which relocation streams this firmware family ships. Default = DEFAULT_STREAMS (M0+);
    pass opt['streams'] to override. Result is always in ORDER order."""
    s=(opt.get('streams') if opt else None) or DEFAULT_STREAMS
    return [nm for nm in ORDER if nm in s]
def pack_bl(imm32):
    if imm32<0: imm32+=(1<<24)
    s=(imm32>>23)&1;i1=(imm32>>22)&1;i2=(imm32>>21)&1;j1=1-(i1^s);j2=1-(i2^s)
    return struct.pack('<HH',0xF000|(s<<10)|((imm32>>11)&0x3ff),0xD000|(j1<<13)|(j2<<11)|(imm32&0x7ff))
def pack_bw(value):
    if value<0: value+=(1<<25)
    t=value&1;cond=(value>>1)&0xf;imm=value>>5;s=(imm>>19)&1;j2=(imm>>18)&1;j1=(imm>>17)&1
    return struct.pack('<HH',0xF000|(s<<10)|(cond<<6)|((imm>>11)&0x3f),0x8000|(j1<<13)|(t<<12)|(j2<<11)|(imm&0x7ff))
def pack_s32(v): return struct.pack('<I', v & 0xffffffff)
PACK={'bl':pack_bl,'bw':pack_bw,'ldr':pack_s32,'ldrw':pack_s32,'data':pack_s32,'code':pack_s32}
def zz(g): return (g<<1) if g>=0 else ((-g)<<1)-1            # zigzag: block gaps can be <0 (detools blocks overlap)
def unzz(u): return (u>>1) if (u&1)==0 else -((u+1)>>1)
def leb128(v):                                              # unsigned LEB128 (sign-less, 7 bits/byte)
    out=bytearray()
    while True:
        b=v&0x7f; v>>=7
        out.append(b|0x80 if v else b)
        if not v: return bytes(out)
def leb128_read(buf,p):
    v=0; s=0
    while True:
        b=buf[p]; p+=1; v|=(b&0x7f)<<s; s+=7
        if not (b&0x80): return v,p

def _disasm(frm, ranges, streams=ORDER):
    fdo,fdb,fde,fcb,fce=ranges
    raw=disassemble(io.BytesIO(bytes(frm)),fdo,fdo+(fde-fdb),fdb,fde,fcb,fce,
                    emit_bw=('bw' in streams), emit_ldr_w=('ldrw' in streams))
    return dict(zip(['bw','bl','ldr','ldrw','data','code'],raw))
def _segs(ops):
    s=[];tp=0;fp=0
    for o in ops: s.append((fp,tp,o.diff_len));tp+=o.diff_len;fp+=o.diff_len;tp+=len(o.extra);fp+=o.adj
    return s
def _topos(segs,a):
    for f0,t0,L in segs:
        if f0<=a<f0+L: return t0+(a-f0)
    return None

def _parse_blocks(dfpatch):
    fp=io.BytesIO(dfpatch); U=lambda:unpack_size(fp)
    dp=fp.read(1)==b'\x01'; fdo=fdb=fde=fcb=fce=0
    if dp: fdo=U();fdb=U();fde=U()
    cp=fp.read(1)==b'\x01'
    if cp: fcb=U();fce=U()
    def rdb():
        nb=U(); return [(U(),U(),U()) for _ in range(nb)]
    blocks={nm:rdb() for nm in ORDER}
    nvals=sum(b[2] for nm in ORDER for b in blocks[nm]); vals=[U() for _ in range(nvals)]
    cfg7=(int(dp),fdo,fdb,fde,int(cp),fcb,fce)
    return blocks,vals,cfg7
