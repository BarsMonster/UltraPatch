"""Shared dataset + scoring for IMM (new-code literal) encoding evaluation.
Goal: losslessly encode the INSERT (new Thumb code) literal bytes using ONLY the
from-image as shared side-information (synced, no shipped table beyond a small
bounded one you declare). Report compressed bits + MCU feasibility."""
import sys,os,math
sys.path[:0]=['/ai_sw/detools-dev/m4dev/sim','/ai_sw/detools-dev/m4dev/sim/algos']
from m4_oracle import load_pair,_FIX
_M={'small':('v0_base','v1_one_face','v0v1_seq_m4.patch'),
    'medium':('v1_one_face','v2_three_faces','v1v2_seq_m4.patch'),
    'large':('v0_base','v2_three_faces','v0v2_seq_m4.patch')}
def get(name):
    fd,td,pn=_M[name]; orc=load_pair(fd,td,pn)
    insert=b''.join(bytes(op.extra) for op in orc.ops)
    frm=open(f"{_FIX}/{fd}/watch.bin",'rb').read()
    return insert, frm
def B(bits): return math.ceil(bits/8)
# reference baselines (bits for the INSERT new-code literals), measured earlier:
BASELINES_B={  # bytes
 'small':{'raw':346,'byte_o0':309,'two_table_parity':300,'byte_o1':290,'halfword_o0':267},
 'large':{'raw':3760,'byte_o0':3446,'two_table_parity':3388,'byte_o1':3243,'halfword_o0':3041}}
def report(name, sizes_B, ram_bytes, integer_only, shipped_table_B, notes):
    """sizes_B: dict {fixture: compressed_bytes}; verified lossless by caller."""
    print(f"[{name}] sizes_B={sizes_B} ram={ram_bytes} int_only={integer_only} shipped={shipped_table_B}B :: {notes}")
