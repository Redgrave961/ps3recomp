import os
import struct
import sys
import tempfile
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from unfself import FSelf
import elf_symbols


def build_elf() -> bytes:
    code = b"\x4e\x80\x00\x20" * 8
    vaddr = 0x10000

    strtab = b"\x00" + b".foo\x00"
    name_off = 1
    symtab = struct.pack(">IBBHQQ", 0, 0, 0, 0, 0, 0)
    symtab += struct.pack(">IBBHQQ", name_off, 2, 0, 1, vaddr, len(code))
    shstr = b"\x00.text\x00.symtab\x00.strtab\x00.shstrtab\x00"
    def sh_name(s):
        return shstr.index(b"\x00" + s + b"\x00") + 1

    off_ph = 64
    off_code = off_ph + 56
    off_sym = off_code + len(code)
    off_str = off_sym + len(symtab)
    off_shstr = off_str + len(strtab)
    off_sh = off_shstr + len(shstr)

    ehdr = bytearray(64)
    ehdr[0:4] = b"\x7fELF"
    ehdr[4] = 2
    ehdr[5] = 2
    ehdr[6] = 1
    struct.pack_into(">H", ehdr, 16, 2)
    struct.pack_into(">H", ehdr, 18, 21)
    struct.pack_into(">I", ehdr, 20, 1)
    struct.pack_into(">Q", ehdr, 24, vaddr)
    struct.pack_into(">Q", ehdr, 32, off_ph)
    struct.pack_into(">Q", ehdr, 40, off_sh)
    struct.pack_into(">H", ehdr, 52, 64)
    struct.pack_into(">H", ehdr, 54, 56)
    struct.pack_into(">H", ehdr, 56, 1)
    struct.pack_into(">H", ehdr, 58, 64)
    struct.pack_into(">H", ehdr, 60, 5)
    struct.pack_into(">H", ehdr, 62, 4)

    phdr = struct.pack(">IIQQQQQQ", 1, 5, off_code, vaddr, vaddr,
                       len(code), len(code), 0x10000)

    def shdr(name, typ, flags, addr, off, size, link, entsz):
        return struct.pack(">IIQQQQIIQQ", name, typ, flags, addr, off, size,
                           link, 0, 0, entsz)

    shtab = b"".join([
        shdr(0, 0, 0, 0, 0, 0, 0, 0),
        shdr(sh_name(b".text"), 1, 0x6, vaddr, off_code, len(code), 0, 0),
        shdr(sh_name(b".symtab"), 2, 0, 0, off_sym, len(symtab), 3, 24),
        shdr(sh_name(b".strtab"), 3, 0, 0, off_str, len(strtab), 0, 0),
        shdr(sh_name(b".shstrtab"), 3, 0, 0, off_shstr, len(shstr), 0, 0),
    ])
    return bytes(ehdr) + phdr + code + symtab + strtab + shstr + shtab


def build_fself(elf: bytes) -> bytes:
    e_phoff, e_shoff = struct.unpack(">QQ", elf[0x20:0x30])
    p_offset, p_vaddr = struct.unpack(">QQ", elf[e_phoff + 8:e_phoff + 24])
    p_filesz = struct.unpack(">Q", elf[e_phoff + 32:e_phoff + 40])[0]
    tail_start = p_offset + p_filesz

    seg_blob = zlib.compress(elf[p_offset:p_offset + p_filesz])

    off_elf = 0x60
    off_phdr = off_elf + 64
    off_seg = off_phdr + 56
    off_blob = off_seg + 32
    off_tail = off_blob + len(seg_blob)
    delta = off_tail - tail_start
    shdr_off_self = e_shoff + delta

    out = bytearray()
    out += struct.pack(">IIHHIQQ", 0x53434500, 2, 0x8000, 1, 0x60, off_elf, len(elf))
    out += struct.pack(">8Q", 3, off_elf, off_elf, off_phdr, shdr_off_self, off_seg, 0, 0)
    assert len(out) == off_elf
    out += elf[0:64]
    out += elf[e_phoff:e_phoff + 56]
    out += struct.pack(">QQIIII", off_blob, len(seg_blob), 2, 0, 0, 0)
    out += seg_blob
    out += elf[tail_start:]
    return bytes(out)


def main() -> int:
    elf = build_elf()
    fself = build_fself(elf)

    rebuilt = FSelf(fself).to_elf()
    assert rebuilt == elf, (
        f"round-trip mismatch: {len(rebuilt)} vs {len(elf)} bytes, "
        f"first diff at {next((i for i in range(min(len(rebuilt), len(elf))) if rebuilt[i] != elf[i]), 'n/a')}"
    )

    retail = bytearray(fself)
    struct.pack_into(">H", retail, 8, 0x0001)
    try:
        FSelf(bytes(retail)).to_elf()
        assert False, "retail SELF was not rejected"
    except Exception as e:
        assert "retail" in str(e).lower() or "key" in str(e).lower(), e

    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "t.elf")
        open(p, "wb").write(rebuilt)
        syms = elf_symbols.load_symbols(p)
    assert len(syms) == 1, syms
    assert syms[0]["name"] == "foo", syms
    assert syms[0]["start"] == "0x00010000", syms
    assert syms[0]["size"] == 32, syms

    print("ok: fSELF round-trip, retail rejection, and symbol recovery all pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())
