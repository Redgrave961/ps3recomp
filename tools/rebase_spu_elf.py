#!/usr/bin/env python3
import struct, sys

def be32(b, o): return struct.unpack_from('>I', b, o)[0]
def wbe32(b, o, v): struct.pack_into('>I', b, o, v & 0xFFFFFFFF)
def be16(b, o): return struct.unpack_from('>H', b, o)[0]

def main():
    src, dst, base = sys.argv[1], sys.argv[2], int(sys.argv[3], 0)
    b = bytearray(open(src, 'rb').read())
    assert b[:4] == b'\x7fELF'
    e_entry, e_phoff, e_shoff = be32(b,0x18), be32(b,0x1C), be32(b,0x20)
    phentsz, phnum = be16(b,0x2A), be16(b,0x2C)
    shentsz, shnum = be16(b,0x2E), be16(b,0x30)
    wbe32(b, 0x18, e_entry + base)
    for i in range(phnum):
        ph = e_phoff + i*phentsz
        if be32(b, ph) != 1: continue
        wbe32(b, ph+8,  be32(b, ph+8)  + base)
        wbe32(b, ph+12, be32(b, ph+12) + base)
    for i in range(shnum):
        sh = e_shoff + i*shentsz
        addr = be32(b, sh+12)
        if addr:
            wbe32(b, sh+12, addr + base)
    open(dst, 'wb').write(b)
    print(f"rebased {src} -> {dst} (+{base:#x})")

if __name__ == '__main__':
    main()
