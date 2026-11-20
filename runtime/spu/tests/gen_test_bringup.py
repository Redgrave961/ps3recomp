#!/usr/bin/env python3
import struct, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "tools"))
from wrap_spu_elf import wrap

def w(v): return struct.pack(">I", v & 0xFFFFFFFF)
def ri16(op9, i16, rt): return w(((op9 & 0x1FF) << 23) | ((i16 & 0xFFFF) << 7) | (rt & 0x7F))
def ri10(op8, i10, ra, rt): return w(((op8 & 0xFF) << 24) | ((i10 & 0x3FF) << 14) | ((ra & 0x7F) << 7) | (rt & 0x7F))
def rr(op11, rb, ra, rt): return w(((op11 & 0x7FF) << 21) | ((rb & 0x7F) << 14) | ((ra & 0x7F) << 7) | (rt & 0x7F))
def ch(op11, channel, rt): return w(((op11 & 0x7FF) << 21) | ((channel & 0x1F) << 7) | (rt & 0x7F))

IL, STQD, WRCH, STOP = 0x81, 0x24, 0x10D, 0x000
MFC_LSA, MFC_EAH, MFC_EAL, MFC_Size, MFC_TagID, MFC_Cmd = 16, 17, 18, 19, 20, 21
WrOutMbox = 28
MFC_PUT = 0x20

b = b""
b += ri16(IL, 0x1234,  11)
b += ri16(IL, 0,        9)
b += ri10(STQD, 0x10,  9, 11)
b += ri16(IL, 0x100,    3)
b += ri16(IL, 0,        4)
b += ri16(IL, 0,        5)
b += ri16(IL, 16,       6)
b += ri16(IL, 0,        7)
b += ri16(IL, MFC_PUT,  8)
b += ch(WRCH, MFC_LSA,   3)
b += ch(WRCH, MFC_EAH,   4)
b += ch(WRCH, MFC_EAL,   5)
b += ch(WRCH, MFC_Size,  6)
b += ch(WRCH, MFC_TagID, 7)
b += ch(WRCH, MFC_Cmd,   8)
b += ch(WRCH, WrOutMbox, 11)
b += rr(STOP, 0, 0, 0)

elf = wrap(b, base=0, entry=0, symbols=[{"name": "main", "addr": 0, "size": len(b)}])
open(os.path.join(HERE, "test_bringup.elf"), "wb").write(elf)
print(f"Wrote test_bringup.elf ({len(b)} bytes code)")
