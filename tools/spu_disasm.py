#!/usr/bin/env python3
import argparse
import json
import os
import struct
import sys


def sign_extend(value: int, nbits: int) -> int:
    if value & (1 << (nbits - 1)):
        value -= 1 << nbits
    return value


def bits(insn: int, hi: int, lo: int) -> int:
    shift = 31 - lo
    mask = (1 << (lo - hi + 1)) - 1
    return (insn >> shift) & mask


class SPUInstruction:
    __slots__ = ("addr", "raw", "mnemonic", "operands", "comment")

    def __init__(self, addr: int = 0, raw: int = 0, mnemonic: str = "???",
                 operands: str = "", comment: str = ""):
        self.addr = addr
        self.raw = raw
        self.mnemonic = mnemonic
        self.operands = operands
        self.comment = comment

    def __str__(self) -> str:
        hexb = f"{self.raw:08X}"
        line = f"{self.addr:08X}:  {hexb}  {self.mnemonic:<12s} {self.operands}"
        if self.comment:
            line += f"  ; {self.comment}"
        return line


RRR_TABLE: dict[int, str] = {
    0b1000: "selb",
    0b1011: "shufb",
    0b1100: "mpya",
    0b1101: "fnms",
    0b1110: "fma",
    0b1111: "fms",
}

RI18_TABLE: dict[int, str] = {
    0b0100001: "ila",
    0b0001000: "hbra",
    0b0001001: "hbrr",
}

RI16_TABLE: dict[int, str] = {
    0b010000001: "il",
    0b010000010: "ilhu",
    0b010000011: "ilh",
    0b011000001: "iohl",
    0b001100000: "bra",
    0b001100001: "lqa",
    0b001100010: "brasl",
    0b001100100: "br",
    0b001100101: "fsmbi",
    0b001100110: "brsl",
    0b001100111: "lqr",
    0b001000000: "brz",
    0b001000001: "stqa",
    0b001000010: "brnz",
    0b001000100: "brhz",
    0b001000110: "brhnz",
    0b001000111: "stqr",
}

RI10_TABLE: dict[int, str] = {
    0b00110100: "lqd",
    0b00100100: "stqd",
    0b00011100: "ai",
    0b00001100: "sfi",
    0b00011101: "ahi",
    0b00001101: "sfhi",
    0b00010100: "andi",
    0b00000100: "ori",
    0b00000101: "orhi",
    0b00000110: "orbi",
    0b00010101: "andhi",
    0b00010110: "andbi",
    0b01000100: "xori",
    0b01000101: "xorhi",
    0b01000110: "xorbi",
    0b01111100: "ceqi",
    0b01111101: "ceqhi",
    0b01111110: "ceqbi",
    0b01001100: "cgti",
    0b01001101: "cgthi",
    0b01001110: "cgtbi",
    0b01011100: "clgti",
    0b01011101: "clgthi",
    0b01011110: "clgtbi",
    0b01110100: "mpyi",
    0b01110101: "mpyui",
    0b01111111: "heqi",
    0b01001111: "hgti",
    0b01011111: "hlgti",
}


SPU_RR: dict[int, str] = {
    0x000: "stop", 0x001: "lnop", 0x002: "sync", 0x003: "dsync",
    0x00c: "mfspr", 0x00d: "rdch", 0x00f: "rchcnt",
    0x10c: "mtspr", 0x10d: "wrch", 0x201: "nop", 0x140: "stopd",
    0x040: "sf", 0x041: "or", 0x042: "bg", 0x048: "sfh", 0x049: "nor",
    0x053: "absdb", 0x0c0: "a", 0x0c1: "and", 0x0c2: "cg", 0x0c8: "ah",
    0x0c9: "nand", 0x0d3: "avgb",
    0x340: "addx", 0x341: "sfx", 0x342: "cgx", 0x343: "bgx",
    0x346: "mpyhha", 0x34e: "mpyhhau",
    0x058: "rot", 0x059: "rotm", 0x05a: "rotma", 0x05b: "shl",
    0x05c: "roth", 0x05d: "rothm", 0x05e: "rotmah", 0x05f: "shlh",
    0x1cc: "rotqbybi", 0x1cd: "rotqmbybi", 0x1cf: "shlqbybi",
    0x1d8: "rotqbi", 0x1d9: "rotqmbi", 0x1db: "shlqbi",
    0x1dc: "rotqby", 0x1dd: "rotqmby", 0x1df: "shlqby",
    0x1b0: "gb", 0x1b1: "gbh", 0x1b2: "gbb",
    0x1b4: "fsm", 0x1b5: "fsmh", 0x1b6: "fsmb",
    0x2a5: "clz", 0x2a6: "xswd", 0x2ae: "xshw", 0x2b4: "cntb", 0x2b6: "xsbh",
    0x1f0: "orx",
    0x1d4: "cbx", 0x1d5: "chx", 0x1d6: "cwx", 0x1d7: "cdx",
    0x1c4: "lqx", 0x144: "stqx",
    0x1a8: "bi", 0x1a9: "bisl", 0x1aa: "iret", 0x1ab: "bisled",
    0x128: "biz", 0x129: "binz", 0x12a: "bihz", 0x12b: "bihnz", 0x1ac: "hbr",
    0x241: "xor", 0x249: "eqv", 0x2c1: "andc", 0x2c9: "orc",
    0x240: "cgt", 0x248: "cgth", 0x250: "cgtb", 0x253: "sumb", 0x258: "hgt",
    0x2c0: "clgt", 0x2c8: "clgth", 0x2d0: "clgtb", 0x2d8: "hlgt",
    0x3c0: "ceq", 0x3c8: "ceqh", 0x3d0: "ceqb", 0x3d8: "heq",
    0x3c4: "mpy", 0x3c5: "mpyh", 0x3c6: "mpyhh", 0x3c7: "mpys",
    0x3cc: "mpyu", 0x3ce: "mpyhhu",
    0x2c2: "fcgt", 0x2c3: "dfcgt", 0x2c4: "fa", 0x2c5: "fs", 0x2c6: "fm",
    0x2ca: "fcmgt", 0x2cb: "dfcmgt", 0x2cc: "dfa", 0x2cd: "dfs", 0x2ce: "dfm",
    0x35c: "dfma", 0x35d: "dfms", 0x35e: "dfnms", 0x35f: "dfnma",
    0x3c2: "fceq", 0x3c3: "dfceq", 0x3ca: "fcmeq", 0x3cb: "dfcmeq",
    0x3b8: "fesd", 0x3b9: "frds", 0x398: "fscrrd", 0x3ba: "fscrwr",
    0x3bf: "dftsv", 0x3d4: "fi", 0x1b8: "frest", 0x1b9: "frsqest",
}

CHANNEL_NAMES = {
    0:  "SPU_RdEventStat",
    1:  "SPU_WrEventMask",
    2:  "SPU_WrEventAck",
    3:  "SPU_RdSigNotify1",
    4:  "SPU_RdSigNotify2",
    7:  "SPU_WrDec",
    8:  "SPU_RdDec",
    9:  "MFC_WrMSSyncReq",
    11: "SPU_RdEventMask",
    13: "SPU_RdMachStat",
    14: "SPU_WrSRR0",
    15: "SPU_RdSRR0",
    16: "MFC_LSA",
    17: "MFC_EAH",
    18: "MFC_EAL",
    19: "MFC_Size",
    20: "MFC_TagID",
    21: "MFC_Cmd",
    22: "MFC_WrTagMask",
    23: "MFC_WrTagUpdate",
    24: "MFC_RdTagStat",
    25: "MFC_RdListStallStat",
    26: "MFC_WrListStallAck",
    27: "MFC_RdAtomicStat",
    28: "SPU_WrOutMbox",
    29: "SPU_RdInMbox",
    30: "SPU_WrOutIntrMbox",
}


def spu_decode(insn: int, addr: int = 0) -> SPUInstruction:
    result = SPUInstruction(addr=addr, raw=insn)

    op4 = (insn >> 28) & 0xF
    op7 = (insn >> 25) & 0x7F
    op8 = (insn >> 24) & 0xFF
    op9 = (insn >> 23) & 0x1FF
    op11 = (insn >> 21) & 0x7FF

    rt = insn & 0x7F
    ra = (insn >> 7) & 0x7F
    rb = (insn >> 14) & 0x7F
    rc = (insn >> 21) & 0x7F

    i10 = sign_extend((insn >> 14) & 0x3FF, 10)
    i16 = sign_extend((insn >> 7) & 0xFFFF, 16)
    i18 = (insn >> 7) & 0x3FFFF

    if op4 in RRR_TABLE:
        mne = RRR_TABLE[op4]
        result.mnemonic = mne
        result.operands = f"$r{rc}, $r{ra}, $r{rb}, $r{rt}"
        return result

    if op7 in RI18_TABLE:
        mne = RI18_TABLE[op7]
        result.mnemonic = mne

        if mne == "ila":
            result.operands = f"$r{rt}, 0x{i18:X}"
        elif mne == "hbra":
            r0h = (i18 >> 16) & 0x3
            i16_field = i18 & 0xFFFF
            brinst_raw = (r0h << 7) | rt
            brinst = sign_extend(brinst_raw, 9) * 4 + addr
            brtarg = i16_field * 4
            result.operands = f"0x{brinst & 0xFFFFFFFF:X}, 0x{brtarg & 0xFFFFFFFF:X}"
        elif mne == "hbrr":
            roh = (i18 >> 16) & 0x3
            brinst_raw = (roh << 7) | rt
            brinst = sign_extend(brinst_raw, 9) * 4 + addr
            brtarg = (sign_extend(i18, 18) * 4 + addr) & 0x3FFFF
            result.operands = f"0x{brinst & 0xFFFFFFFF:X}, 0x{brtarg:X}"
        elif mne in ("il",):
            result.operands = f"$r{rt}, {sign_extend(i18 & 0xFFFF, 16)}"
        elif mne in ("ilh", "ilhu"):
            result.operands = f"$r{rt}, 0x{i18 & 0xFFFF:X}"
        else:
            result.operands = f"$r{rt}, 0x{i18:X}"
        return result

    if op9 in RI16_TABLE:
        mne = RI16_TABLE[op9]
        result.mnemonic = mne
        if mne in ("lqa", "stqa"):
            lsa = ((i16 & 0xFFFF) << 2) & 0x3FFF0
            result.operands = f"$r{rt}, 0x{lsa:X}"
        elif mne in ("hbra", "hbrr"):
            result.operands = f"0x{i16 & 0xFFFF:X}, $r{rt}"
        elif mne == "il":
            result.operands = f"$r{rt}, {i16}"
        elif mne in ("iohl", "ilh", "ilhu"):
            result.operands = f"$r{rt}, 0x{i16 & 0xFFFF:X}"
        elif mne in ("brz", "brnz", "brhz", "brhnz"):
            target = i16 * 4 + addr
            result.operands = f"$r{rt}, 0x{target & 0x3FFFF:X}"
        elif mne == "br":
            target = (i16 * 4 + addr) & 0x3FFFF
            result.operands = f"0x{target:X}"
        elif mne == "brsl":
            target = (i16 * 4 + addr) & 0x3FFFF
            result.operands = f"$r{rt}, 0x{target:X}"
        elif mne == "bra":
            target = (i16 * 4) & 0x3FFFF
            result.operands = f"0x{target:X}"
        elif mne == "brasl":
            target = (i16 * 4) & 0x3FFFF
            result.operands = f"$r{rt}, 0x{target:X}"
        elif mne in ("lqr", "stqr"):
            target = (i16 * 4 + addr) & 0x3FFF0
            result.operands = f"$r{rt}, 0x{target:X}"
        else:
            result.operands = f"$r{rt}, 0x{i16 & 0xFFFF:X}"
        return result

    _ch_field = (insn >> 7) & 0x7F
    if op11 == 0b00000001101:
        _ch = CHANNEL_NAMES.get(_ch_field, f"ch{_ch_field}")
        result.mnemonic = "rdch"
        result.operands = f"$r{rt}, {_ch}"
        return result
    if op11 == 0b00100001101:
        _ch = CHANNEL_NAMES.get(_ch_field, f"ch{_ch_field}")
        result.mnemonic = "wrch"
        result.operands = f"{_ch}, $r{rt}"
        return result
    if op11 == 0b00000001111:
        _ch = CHANNEL_NAMES.get(_ch_field, f"ch{_ch_field}")
        result.mnemonic = "rchcnt"
        result.operands = f"$r{rt}, {_ch}"
        return result

    if op8 in RI10_TABLE:
        mne = RI10_TABLE[op8]
        result.mnemonic = mne
        if mne in ("lqd", "stqd"):
            offset = i10 << 4
            if offset < 0:
                disp = f"-0x{-offset:X}"
            else:
                disp = f"0x{offset:X}"
            result.operands = f"$r{rt}, {disp}($r{ra})"
        else:
            result.operands = f"$r{rt}, $r{ra}, {i10}"
        return result

    op10 = (insn >> 22) & 0x3FF
    ri8_table = {0b0111011000: "cflts", 0b0111011001: "cfltu",
                 0b0111011010: "csflt", 0b0111011011: "cuflt"}
    if op10 in ri8_table:
        result.mnemonic = ri8_table[op10]
        i8 = (insn >> 14) & 0xFF
        result.operands = f"$r{rt}, $r{ra}, {i8}"
        return result

    if op11 in SPU_RR:
        mne = SPU_RR[op11]
        result.mnemonic = mne

        if mne == "bi":
            result.operands = f"$r{ra}"
            return result
        if mne == "bisl":
            result.operands = f"$r{rt}, $r{ra}"
            return result
        if mne in ("biz", "binz", "bihz", "bihnz"):
            result.operands = f"$r{rt}, $r{ra}"
            return result
        if mne == "hbr":
            roh = (insn >> 14) & 0x3
            brinst_raw = (roh << 7) | rt
            brinst = sign_extend(brinst_raw, 9) * 4 + addr
            result.operands = f"0x{brinst & 0xFFFFFFFF:X}, $r{ra}"
            return result

        if mne == "stop":
            code = insn & 0x3FFF
            result.operands = f"0x{code:X}"
            return result
        if mne in ("lnop", "nop", "sync", "dsync"):
            if mne == "sync" and (insn & 0x00100000):
                result.mnemonic = "syncc"
            return result

        if mne in ("clz", "cntb", "gb", "gbh", "fscrrd", "orx", "fsm", "fsmb"):
            result.operands = f"$r{rt}, $r{ra}"
            return result

        result.operands = f"$r{rt}, $r{ra}, $r{rb}"
        return result

    ri7_table: dict[int, str] = {
        0b00001111000: "roti",
        0b00001111001: "rotmi",
        0b00001111010: "rotmai",
        0b00001111011: "shli",
        0b00001111100: "rothi",
        0b00001111101: "rothmi",
        0b00001111110: "rotmahi",
        0b00001111111: "shlhi",
        0b00111110100: "cbd",
        0b00111110101: "chd",
        0b00111110110: "cwd",
        0b00111110111: "cdd",
        0b00111111000: "rotqbii",
        0b00111111001: "rotqmbii",
        0b00111111011: "shlqbii",
        0b00111111100: "rotqbyi",
        0b00111111101: "rotqmbyi",
        0b00111111111: "shlqbyi",
    }
    RI7_UNSIGNED = {"cbd", "chd", "cwd", "cdd"}
    if op11 in ri7_table:
        mne = ri7_table[op11]
        if mne in RI7_UNSIGNED:
            i7 = rb
        else:
            i7 = sign_extend(rb, 7)
        result.mnemonic = mne
        result.operands = f"$r{rt}, $r{ra}, {i7}"
        return result

    lsx_table: dict[int, str] = {
        0b00111000100: "lqx",
        0b00101000100: "stqx",
    }
    if op11 in lsx_table:
        result.mnemonic = lsx_table[op11]
        result.operands = f"$r{rt}, $r{ra}, $r{rb}"
        return result

    result.mnemonic = ".word"
    result.operands = f"0x{insn:08X}"
    return result


def disassemble_spu(data: bytes, base_addr: int = 0) -> list[SPUInstruction]:
    instructions: list[SPUInstruction] = []
    for off in range(0, len(data) - 3, 4):
        raw = struct.unpack_from(">I", data, off)[0]
        addr = base_addr + off
        insn = spu_decode(raw, addr)
        instructions.append(insn)
    return instructions


def main() -> None:
    parser = argparse.ArgumentParser(description="SPU disassembler for PS3 binaries")
    parser.add_argument("input", help="Input binary or ELF file")
    parser.add_argument("--base", type=lambda x: int(x, 0), default=0,
                        help="Base address (hex ok)")
    parser.add_argument("--offset", type=lambda x: int(x, 0), default=0,
                        help="Start offset in file")
    parser.add_argument("--length", type=lambda x: int(x, 0), default=0,
                        help="Bytes to disassemble (0=all)")
    parser.add_argument("--json", action="store_true", help="Output as JSON")
    args = parser.parse_args()

    with open(args.input, "rb") as f:
        file_data = f.read()

    data = file_data[args.offset:]
    if args.length:
        data = data[:args.length]

    instructions = disassemble_spu(data, args.base)

    if args.json:
        out = [{"addr": f"0x{i.addr:08X}", "hex": f"{i.raw:08X}",
                "mnemonic": i.mnemonic, "operands": i.operands}
               for i in instructions]
        print(json.dumps(out, indent=2))
    else:
        for i in instructions:
            print(i)


if __name__ == "__main__":
    main()
