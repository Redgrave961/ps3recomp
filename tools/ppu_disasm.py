#!/usr/bin/env python3
import argparse
import json
import os
import struct
import sys

SPR_NAMES = {
    1: "XER",
    8: "LR",
    9: "CTR",
    18: "DSISR",
    19: "DAR",
    22: "DEC",
    25: "SDR1",
    256: "VRSAVE",
    26: "SRR0",
    27: "SRR1",
    268: "TBL",
    269: "TBU",
    272: "SPRG0",
    273: "SPRG1",
    274: "SPRG2",
    275: "SPRG3",
    287: "PVR",
    1008: "HID0",
    1009: "HID1",
    1013: "DABR",
}


def sign_extend(value: int, bits: int) -> int:
    if value & (1 << (bits - 1)):
        value -= 1 << bits
    return value


def bits(insn: int, start: int, end: int) -> int:
    shift = 31 - end
    mask = (1 << (end - start + 1)) - 1
    return (insn >> shift) & mask


def bit(insn: int, pos: int) -> int:
    return (insn >> (31 - pos)) & 1


class Instruction:
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
        line = f"{self.addr:08X}:  {hexb}  {self.mnemonic:<10s} {self.operands}"
        if self.comment:
            line += f"  ; {self.comment}"
        return line


def _branch_cond_mnemonic(bo: int, bi: int) -> tuple[str, str]:
    cr = bi >> 2
    cond = bi & 3
    cond_names_true = ["lt", "gt", "eq", "so"]
    cond_names_false = ["ge", "le", "ne", "ns"]

    decr = not (bo & 0x04)
    cond_flag = not (bo & 0x10)
    true_cond = bool(bo & 0x08)

    if not cond_flag and not decr:
        return "", ""

    if cond_flag and not decr:
        name = cond_names_true[cond] if true_cond else cond_names_false[cond]
        cr_str = f"cr{cr}, " if cr != 0 else ""
        return name, cr_str

    if decr and not cond_flag:
        name = "dnz" if bo & 0x02 == 0 else "dz"
        return name, ""

    name = ("dnz" if bo & 0x02 == 0 else "dz")
    if true_cond:
        name += cond_names_true[cond]
    else:
        name += cond_names_false[cond]
    cr_str = f"cr{cr}, " if cr != 0 else ""
    return name, cr_str


def decode(insn: int, addr: int = 0) -> Instruction:
    opcd = bits(insn, 0, 5)
    result = Instruction(addr=addr, raw=insn)


    if opcd == 18:
        li = bits(insn, 6, 29)
        aa = bit(insn, 30)
        lk = bit(insn, 31)
        target = sign_extend(li << 2, 26)
        if not aa:
            target += addr
        else:
            target = target & 0xFFFFFFFF
        mne = "b"
        if lk:
            mne += "l"
        if aa:
            mne += "a"
        result.mnemonic = mne
        result.operands = f"0x{target & 0xFFFFFFFF:X}"
        return result

    if opcd == 16:
        bo = bits(insn, 6, 10)
        bi = bits(insn, 11, 15)
        bd = bits(insn, 16, 29)
        aa = bit(insn, 30)
        lk = bit(insn, 31)
        target = sign_extend(bd << 2, 16)
        if not aa:
            target += addr
        else:
            target = target & 0xFFFFFFFF

        cond_suf, cr_str = _branch_cond_mnemonic(bo, bi)
        mne = "b"
        if cond_suf:
            mne += cond_suf
        else:
            mne += "c"
        if lk:
            mne += "l"
        if aa:
            mne += "a"
        if cond_suf:
            result.mnemonic = mne
            result.operands = f"{cr_str}0x{target & 0xFFFFFFFF:X}"
        else:
            result.mnemonic = mne
            result.operands = f"{bo}, {bi}, 0x{target & 0xFFFFFFFF:X}"
        return result

    if opcd == 17:
        if bit(insn, 30):
            result.mnemonic = "sc"
            return result

    if opcd == 19:
        xo = bits(insn, 21, 30)
        bo = bits(insn, 6, 10)
        bi = bits(insn, 11, 15)
        lk = bit(insn, 31)

        if xo == 16:
            cond_suf, cr_str = _branch_cond_mnemonic(bo, bi)
            if bo == 20 and bi == 0:
                mne = "blr"
                if lk:
                    mne += "l"
                result.mnemonic = mne
            else:
                mne = "b" + cond_suf + "lr" if cond_suf else "bclr"
                if lk:
                    mne += "l"
                result.mnemonic = mne
                if not cond_suf:
                    result.operands = f"{bo}, {bi}"
                elif cr_str:
                    result.operands = cr_str.rstrip(", ")
            return result

        if xo == 528:
            cond_suf, cr_str = _branch_cond_mnemonic(bo, bi)
            if bo == 20 and bi == 0:
                mne = "bctr"
                if lk:
                    mne += "l"
                result.mnemonic = mne
            else:
                mne = "b" + cond_suf + "ctr" if cond_suf else "bcctr"
                if lk:
                    mne += "l"
                result.mnemonic = mne
                if not cond_suf:
                    result.operands = f"{bo}, {bi}"
                elif cr_str:
                    result.operands = cr_str.rstrip(", ")
            return result

        bt = bits(insn, 6, 10)
        ba = bits(insn, 11, 15)
        bb = bits(insn, 16, 20)
        cr_ops = {
            257: "crand", 449: "cror", 193: "crxor",
            33: "crnor", 225: "crnand", 289: "creqv",
            129: "crandc", 417: "crorc",
            0: "mcrf",
        }
        if xo in cr_ops:
            result.mnemonic = cr_ops[xo]
            if xo == 0:
                result.operands = f"cr{bt >> 2}, cr{ba >> 2}"
            else:
                result.operands = f"{bt}, {ba}, {bb}"
            return result

        if xo == 50:
            result.mnemonic = "rfi"
            return result

        if xo == 150:
            result.mnemonic = "isync"
            return result

    rd = bits(insn, 6, 10)
    ra = bits(insn, 11, 15)
    si = sign_extend(bits(insn, 16, 31), 16)
    ui = bits(insn, 16, 31)

    if opcd == 14:
        if ra == 0:
            result.mnemonic = "li"
            result.operands = f"r{rd}, {si}"
        else:
            result.mnemonic = "addi"
            result.operands = f"r{rd}, r{ra}, {si}"
        return result

    if opcd == 15:
        if ra == 0:
            result.mnemonic = "lis"
            result.operands = f"r{rd}, 0x{ui:X}"
        else:
            result.mnemonic = "addis"
            result.operands = f"r{rd}, r{ra}, 0x{ui:X}"
        return result

    if opcd == 24:
        if rd == 0 and ra == 0 and ui == 0:
            result.mnemonic = "nop"
        else:
            result.mnemonic = "ori"
            result.operands = f"r{ra}, r{rd}, 0x{ui:X}"
        return result
    if opcd == 25:
        result.mnemonic = "oris"
        result.operands = f"r{ra}, r{rd}, 0x{ui:X}"
        return result
    if opcd == 26:
        result.mnemonic = "xori"
        result.operands = f"r{ra}, r{rd}, 0x{ui:X}"
        return result
    if opcd == 27:
        result.mnemonic = "xoris"
        result.operands = f"r{ra}, r{rd}, 0x{ui:X}"
        return result
    if opcd == 28:
        result.mnemonic = "andi."
        result.operands = f"r{ra}, r{rd}, 0x{ui:X}"
        return result
    if opcd == 29:
        result.mnemonic = "andis."
        result.operands = f"r{ra}, r{rd}, 0x{ui:X}"
        return result

    if opcd == 7:
        result.mnemonic = "mulli"
        result.operands = f"r{rd}, r{ra}, {si}"
        return result

    if opcd == 8:
        result.mnemonic = "subfic"
        result.operands = f"r{rd}, r{ra}, {si}"
        return result

    if opcd == 10:
        bf = bits(insn, 6, 8)
        l_bit = bit(insn, 10)
        result.mnemonic = "cmpldi" if l_bit else "cmplwi"
        cr_str = f"cr{bf}, " if bf else ""
        result.operands = f"{cr_str}r{ra}, 0x{ui:X}"
        return result
    if opcd == 11:
        bf = bits(insn, 6, 8)
        l_bit = bit(insn, 10)
        result.mnemonic = "cmpdi" if l_bit else "cmpwi"
        cr_str = f"cr{bf}, " if bf else ""
        result.operands = f"{cr_str}r{ra}, {si}"
        return result

    if opcd == 12:
        result.mnemonic = "addic"
        result.operands = f"r{rd}, r{ra}, {si}"
        return result
    if opcd == 13:
        result.mnemonic = "addic."
        result.operands = f"r{rd}, r{ra}, {si}"
        return result

    load_store = {
        32: ("lwz", True), 33: ("lwzu", True),
        34: ("lbz", True), 35: ("lbzu", True),
        36: ("stw", False), 37: ("stwu", False),
        38: ("stb", False), 39: ("stbu", False),
        40: ("lhz", True), 41: ("lhzu", True),
        42: ("lha", True), 43: ("lhau", True),
        44: ("sth", False), 45: ("sthu", False),
        46: ("lmw", True), 47: ("stmw", False),
        48: ("lfs", True), 49: ("lfsu", True),
        50: ("lfd", True), 51: ("lfdu", True),
        52: ("stfs", False), 53: ("stfsu", False),
        54: ("stfd", False), 55: ("stfdu", False),
    }
    if opcd in load_store:
        mne, is_load = load_store[opcd]
        d = sign_extend(bits(insn, 16, 31), 16)
        if d < 0:
            disp = f"-0x{-d:X}"
        else:
            disp = f"0x{d:X}"
        result.mnemonic = mne
        result.operands = f"r{rd}, {disp}(r{ra})"
        return result

    if opcd == 58:
        ds = sign_extend(bits(insn, 16, 29) << 2, 16)
        sub = bits(insn, 30, 31)
        mne = {0: "ld", 1: "ldu", 2: "lwa"}.get(sub, "ld?")
        disp = f"-0x{-ds:X}" if ds < 0 else f"0x{ds:X}"
        result.mnemonic = mne
        result.operands = f"r{rd}, {disp}(r{ra})"
        return result
    if opcd == 62:
        ds = sign_extend(bits(insn, 16, 29) << 2, 16)
        sub = bits(insn, 30, 31)
        mne = {0: "std", 1: "stdu"}.get(sub, "std?")
        disp = f"-0x{-ds:X}" if ds < 0 else f"0x{ds:X}"
        result.mnemonic = mne
        result.operands = f"r{rd}, {disp}(r{ra})"
        return result

    if opcd == 21:
        sh = bits(insn, 16, 20)
        mb = bits(insn, 21, 25)
        me = bits(insn, 26, 30)
        rc = bit(insn, 31)
        mne = "rlwinm" + ("." if rc else "")
        result.mnemonic = mne
        result.operands = f"r{ra}, r{rd}, {sh}, {mb}, {me}"
        return result
    if opcd == 23:
        rb = bits(insn, 16, 20)
        mb = bits(insn, 21, 25)
        me = bits(insn, 26, 30)
        rc = bit(insn, 31)
        mne = "rlwnm" + ("." if rc else "")
        result.mnemonic = mne
        result.operands = f"r{ra}, r{rd}, r{rb}, {mb}, {me}"
        return result
    if opcd == 20:
        sh = bits(insn, 16, 20)
        mb = bits(insn, 21, 25)
        me = bits(insn, 26, 30)
        rc = bit(insn, 31)
        mne = "rlwimi" + ("." if rc else "")
        result.mnemonic = mne
        result.operands = f"r{ra}, r{rd}, {sh}, {mb}, {me}"
        return result

    if opcd == 30:
        rc = bit(insn, 31)

        sub4 = bits(insn, 27, 30)
        sub3 = bits(insn, 27, 29)

        md_ops = {0: "rldicl", 1: "rldicr", 2: "rldic", 3: "rldimi"}
        if sub3 in md_ops:
            sh = (bits(insn, 16, 20) | (bit(insn, 30) << 5))
            mb = (bits(insn, 21, 25) | (bit(insn, 26) << 5))
            mne = md_ops[sub3] + ("." if rc else "")
            result.mnemonic = mne
            result.operands = f"r{ra}, r{rd}, {sh}, {mb}"
        elif sub4 == 8:
            rb = bits(insn, 16, 20)
            mb = (bits(insn, 21, 25) | (bit(insn, 26) << 5))
            mne = "rldcl" + ("." if rc else "")
            result.mnemonic = mne
            result.operands = f"r{ra}, r{rd}, r{rb}, {mb}"
        elif sub4 == 9:
            rb = bits(insn, 16, 20)
            me = (bits(insn, 21, 25) | (bit(insn, 26) << 5))
            mne = "rldcr" + ("." if rc else "")
            result.mnemonic = mne
            result.operands = f"r{ra}, r{rd}, r{rb}, {me}"
        else:
            sh = (bits(insn, 16, 20) | (bit(insn, 30) << 5))
            mb = (bits(insn, 21, 25) | (bit(insn, 26) << 5))
            result.mnemonic = "rld??"
            result.operands = f"r{ra}, r{rd}, {sh}, {mb}"
        return result

    if opcd == 31:
        xo_full = bits(insn, 21, 30)
        xo_9 = bits(insn, 22, 30)
        rb = bits(insn, 16, 20)
        rc = bit(insn, 31)
        oe = bit(insn, 21)

        xo_arith_2op = {
            202: "addze", 234: "addme",
            200: "subfze", 232: "subfme",
            104: "neg",
        }
        xo_arith_3op = {
            266: "add", 10: "addc", 138: "adde",
            40: "subf", 8: "subfc", 136: "subfe",
            235: "mullw", 75: "mulhw", 11: "mulhwu",
            491: "divw", 459: "divwu",
            233: "mulld", 73: "mulhd", 9: "mulhdu",
            489: "divd", 457: "divdu",
        }
        if xo_9 in xo_arith_2op:
            mne = xo_arith_2op[xo_9]
            if oe: mne += "o"
            if rc: mne += "."
            result.mnemonic = mne
            result.operands = f"r{rd}, r{ra}"
            return result
        if xo_9 in xo_arith_3op:
            mne = xo_arith_3op[xo_9]
            if oe: mne += "o"
            if rc: mne += "."
            result.mnemonic = mne
            result.operands = f"r{rd}, r{ra}, r{rb}"
            return result

        x_logical = {
            28: "and", 444: "or", 316: "xor",
            476: "nand", 124: "nor",
            284: "eqv", 60: "andc", 412: "orc",
            954: "extsb", 922: "extsh", 986: "extsw",
            26: "cntlzw", 58: "cntlzd",
        }
        if xo_full in x_logical:
            mne = x_logical[xo_full]
            if rc:
                mne += "."
            result.mnemonic = mne
            if xo_full in (954, 922, 986, 26, 58):
                result.operands = f"r{ra}, r{rd}"
            else:
                result.operands = f"r{ra}, r{rd}, r{rb}"
            return result

        x_shift = {
            24: "slw", 536: "srw", 792: "sraw", 824: "srawi",
            27: "sld", 539: "srd", 794: "srad", 826: "sradi",
            827: "sradi",
        }
        if xo_full in x_shift:
            mne = x_shift[xo_full]
            if rc:
                mne += "."
            result.mnemonic = mne
            if xo_full in (824, 826, 827):
                sh = rb + (32 if xo_full == 827 else 0)
                result.operands = f"r{ra}, r{rd}, {sh}"
            else:
                result.operands = f"r{ra}, r{rd}, r{rb}"
            return result

        if xo_full == 0:
            bf = bits(insn, 6, 8)
            l_bit = bit(insn, 10)
            mne = "cmpd" if l_bit else "cmpw"
            cr_str = f"cr{bf}, " if bf else ""
            result.mnemonic = mne
            result.operands = f"{cr_str}r{ra}, r{rb}"
            return result
        if xo_full == 32:
            bf = bits(insn, 6, 8)
            l_bit = bit(insn, 10)
            mne = "cmpld" if l_bit else "cmplw"
            cr_str = f"cr{bf}, " if bf else ""
            result.mnemonic = mne
            result.operands = f"{cr_str}r{ra}, r{rb}"
            return result

        x_ldst = {
            23: ("lwzx", False), 55: ("lwzux", False),
            87: ("lbzx", False), 119: ("lbzux", False),
            151: ("stwx", False), 183: ("stwux", False),
            215: ("stbx", False), 247: ("stbux", False),
            279: ("lhzx", False), 311: ("lhzux", False),
            343: ("lhax", False), 375: ("lhaux", False),
            407: ("sthx", False), 439: ("sthux", False),
            21: ("ldx", False), 53: ("ldux", False),
            149: ("stdx", False), 181: ("stdux", False),
            20: ("lwarx", False), 150: ("stwcx.", True),
            84: ("ldarx", False), 214: ("stdcx.", True),
            535: ("lfsx", False), 567: ("lfsux", False),
            599: ("lfdx", False), 631: ("lfdux", False),
            663: ("stfsx", False), 695: ("stfsux", False),
            727: ("stfdx", False), 759: ("stfdux", False),
            983: ("stfiwx", False),
        }
        if xo_full in x_ldst:
            mne, has_dot = x_ldst[xo_full]
            result.mnemonic = mne
            result.operands = f"r{rd}, r{ra}, r{rb}"
            return result

        if xo_full == 339:
            spr_raw = bits(insn, 11, 20)
            spr = ((spr_raw & 0x1F) << 5) | ((spr_raw >> 5) & 0x1F)
            spr_name = SPR_NAMES.get(spr, f"spr{spr}")
            if spr == 8:
                result.mnemonic = "mflr"
                result.operands = f"r{rd}"
            elif spr == 9:
                result.mnemonic = "mfctr"
                result.operands = f"r{rd}"
            else:
                result.mnemonic = "mfspr"
                result.operands = f"r{rd}, {spr_name}"
            return result

        if xo_full == 467:
            spr_raw = bits(insn, 11, 20)
            spr = ((spr_raw & 0x1F) << 5) | ((spr_raw >> 5) & 0x1F)
            spr_name = SPR_NAMES.get(spr, f"spr{spr}")
            if spr == 8:
                result.mnemonic = "mtlr"
                result.operands = f"r{rd}"
            elif spr == 9:
                result.mnemonic = "mtctr"
                result.operands = f"r{rd}"
            else:
                result.mnemonic = "mtspr"
                result.operands = f"{spr_name}, r{rd}"
            return result

        if xo_full == 19:
            result.mnemonic = "mfcr"
            result.operands = f"r{rd}"
            return result
        if xo_full == 144:
            crm = bits(insn, 12, 19)
            if crm == 0xFF:
                result.mnemonic = "mtcr"
                result.operands = f"r{rd}"
            else:
                result.mnemonic = "mtcrf"
                result.operands = f"0x{crm:02X}, r{rd}"
            return result

        if xo_full == 598:
            result.mnemonic = {1: "lwsync", 2: "ptesync"}.get(bits(insn, 9, 10),
                                                              "sync")
            return result

        misc_x = {
            854: "eieio", 278: "dcbt", 246: "dcbtst",
            86: "dcbf", 54: "dcbst", 982: "icbi", 758: "dcba",
            1014: "dcbz",
        }
        if xo_full in misc_x:
            result.mnemonic = misc_x[xo_full]
            if xo_full in (278, 246, 86, 54, 982, 758, 1014):
                result.operands = f"r{ra}, r{rb}"
            return result

        if xo_full == 4:
            result.mnemonic = "tw"
            result.operands = f"{rd}, r{ra}, r{rb}"
            return result

        if xo_full == 534:
            result.mnemonic = "lwbrx"
            result.operands = f"r{rd}, r{ra}, r{rb}"
            return result
        if xo_full == 662:
            result.mnemonic = "stwbrx"
            result.operands = f"r{rd}, r{ra}, r{rb}"
            return result
        if xo_full == 790:
            result.mnemonic = "lhbrx"
            result.operands = f"r{rd}, r{ra}, r{rb}"
            return result
        if xo_full == 918:
            result.mnemonic = "sthbrx"
            result.operands = f"r{rd}, r{ra}, r{rb}"
            return result

        if xo_full == 341:
            result.mnemonic = "lwax"
            result.operands = f"r{rd}, r{ra}, r{rb}"
            return result
        if xo_full == 373:
            result.mnemonic = "lwaux"
            result.operands = f"r{rd}, r{ra}, r{rb}"
            return result

        if xo_full == 371:
            tbr_raw = bits(insn, 11, 20)
            tbr = ((tbr_raw & 0x1F) << 5) | ((tbr_raw >> 5) & 0x1F)
            if tbr == 268:
                result.mnemonic = "mftb"
            elif tbr == 269:
                result.mnemonic = "mftbu"
            else:
                result.mnemonic = "mftb"
            result.operands = f"r{rd}"
            return result

        if xo_full == 597:
            nb = rb
            result.mnemonic = "lswi"
            result.operands = f"r{rd}, r{ra}, {nb}"
            return result
        if xo_full == 725:
            nb = rb
            result.mnemonic = "stswi"
            result.operands = f"r{rd}, r{ra}, {nb}"
            return result

        if xo_full == 6:
            result.mnemonic = "lvsl"
            result.operands = f"v{rd}, r{ra}, r{rb}"
            return result
        if xo_full == 38:
            result.mnemonic = "lvsr"
            result.operands = f"v{rd}, r{ra}, r{rb}"
            return result

        if xo_full == 103:
            result.mnemonic = "lvx"
            result.operands = f"v{rd}, r{ra}, r{rb}"
            return result
        if xo_full == 231:
            result.mnemonic = "stvx"
            result.operands = f"v{rd}, r{ra}, r{rb}"
            return result
        if xo_full == 7:
            result.mnemonic = "lvebx"
            result.operands = f"v{rd}, r{ra}, r{rb}"
            return result
        if xo_full == 39:
            result.mnemonic = "lvehx"
            result.operands = f"v{rd}, r{ra}, r{rb}"
            return result
        if xo_full == 71:
            result.mnemonic = "lvewx"
            result.operands = f"v{rd}, r{ra}, r{rb}"
            return result
        if xo_full == 135:
            result.mnemonic = "stvebx"
            result.operands = f"v{rd}, r{ra}, r{rb}"
            return result
        if xo_full == 167:
            result.mnemonic = "stvehx"
            result.operands = f"v{rd}, r{ra}, r{rb}"
            return result
        if xo_full == 199:
            result.mnemonic = "stvewx"
            result.operands = f"v{rd}, r{ra}, r{rb}"
            return result
        if xo_full == 359:
            result.mnemonic = "lvxl"
            result.operands = f"v{rd}, r{ra}, r{rb}"
            return result
        if xo_full == 487:
            result.mnemonic = "stvxl"
            result.operands = f"v{rd}, r{ra}, r{rb}"
            return result
        if xo_full == 6:
            result.mnemonic = "lvsl"
            result.operands = f"v{rd}, r{ra}, r{rb}"
            return result
        if xo_full == 38:
            result.mnemonic = "lvsr"
            result.operands = f"v{rd}, r{ra}, r{rb}"
            return result
        if xo_full == 519:
            result.mnemonic = "lvlx"
            result.operands = f"v{rd}, r{ra}, r{rb}"
            return result
        if xo_full == 551:
            result.mnemonic = "lvrx"
            result.operands = f"v{rd}, r{ra}, r{rb}"
            return result
        if xo_full == 775:
            result.mnemonic = "lvlxl"
            result.operands = f"v{rd}, r{ra}, r{rb}"
            return result
        if xo_full == 807:
            result.mnemonic = "lvrxl"
            result.operands = f"v{rd}, r{ra}, r{rb}"
            return result
        if xo_full == 660:
            result.mnemonic = "stdbrx"
            result.operands = f"r{rd}, r{ra}, r{rb}"
            return result
        if xo_full == 533:
            result.mnemonic = "lswx"
            result.operands = f"r{rd}, r{ra}, r{rb}"
            return result
        if xo_full == 661:
            result.mnemonic = "stswx"
            result.operands = f"r{rd}, r{ra}, r{rb}"
            return result
        if xo_full == 68:
            result.mnemonic = "td"
            result.operands = f"{rd}, r{ra}, r{rb}"
            return result
        if xo_full in (342, 374):
            result.mnemonic = "dst" if xo_full == 342 else "dstst"
            result.operands = f"r{ra}, r{rb}, {rd & 3}"
            return result
        if xo_full == 822:
            result.mnemonic = "dss"
            result.operands = f"{rd & 3}"
            return result
        if xo_full == 532:
            result.mnemonic = "ldbrx"
            result.operands = f"r{rd}, r{ra}, r{rb}"
            return result

        if xo_full == 647:
            result.mnemonic = "stvlx"
            result.operands = f"v{rd}, r{ra}, r{rb}"
            return result
        if xo_full == 679:
            result.mnemonic = "stvrx"
            result.operands = f"v{rd}, r{ra}, r{rb}"
            return result
        if xo_full == 903:
            result.mnemonic = "stvlxl"
            result.operands = f"v{rd}, r{ra}, r{rb}"
            return result
        if xo_full == 935:
            result.mnemonic = "stvrxl"
            result.operands = f"v{rd}, r{ra}, r{rb}"
            return result

        result.mnemonic = f"op31_x{xo_full}"
        result.operands = f"r{rd}, r{ra}, r{rb}"
        return result

    if opcd == 63:
        xo_full = bits(insn, 21, 30)
        xo_5 = bits(insn, 26, 30)
        frd = rd
        fra = ra
        frb = bits(insn, 16, 20)
        frc = bits(insn, 21, 25)
        rc = bit(insn, 31)

        fp_a = {
            21: "fadd", 20: "fsub", 25: "fmul", 18: "fdiv",
            29: "fmadd", 28: "fmsub", 31: "fnmadd", 30: "fnmsub",
            23: "fsel", 22: "fsqrt", 26: "frsqrte", 24: "fre",
        }
        if xo_5 in fp_a:
            mne = fp_a[xo_5]
            if rc:
                mne += "."
            result.mnemonic = mne
            if xo_5 == 25:
                result.operands = f"f{frd}, f{fra}, f{frc}"
            elif xo_5 in (29, 28, 31, 30):
                result.operands = f"f{frd}, f{fra}, f{frc}, f{frb}"
            elif xo_5 == 23:
                result.operands = f"f{frd}, f{fra}, f{frc}, f{frb}"
            elif xo_5 in (26, 24, 22):
                result.operands = f"f{frd}, f{frb}"
            else:
                result.operands = f"f{frd}, f{fra}, f{frb}"
            return result

        fp_x = {
            0: "fcmpu", 32: "fcmpo",
            12: "frsp", 14: "fctiw", 15: "fctiwz",
            814: "fctid", 815: "fctidz", 846: "fcfid",
            40: "fneg", 72: "fmr", 264: "fabs", 136: "fnabs",
            64: "mcrfs",
            583: "mffs", 711: "mtfsf",
            70: "mtfsb0", 38: "mtfsb1", 134: "mtfsfi",
        }
        if xo_full in fp_x:
            mne = fp_x[xo_full]
            if rc:
                mne += "."
            result.mnemonic = mne
            if xo_full in (0, 32):
                bf = bits(insn, 6, 8)
                result.operands = f"cr{bf}, f{fra}, f{frb}"
            elif xo_full in (40, 72, 264, 136):
                result.operands = f"f{frd}, f{frb}"
            elif xo_full == 583:
                result.operands = f"f{frd}"
            elif xo_full == 711:
                fm = bits(insn, 7, 14)
                result.operands = f"0x{fm:02X}, f{frb}"
            else:
                result.operands = f"f{frd}, f{frb}"
            return result

    if opcd == 59:
        xo_5 = bits(insn, 26, 30)
        frd = rd
        fra = ra
        frb = bits(insn, 16, 20)
        frc = bits(insn, 21, 25)
        rc = bit(insn, 31)
        fps = {21: "fadds", 20: "fsubs", 25: "fmuls", 18: "fdivs",
               29: "fmadds", 28: "fmsubs", 31: "fnmadds", 30: "fnmsubs",
               22: "fsqrts", 24: "fres", 26: "frsqrtes"}
        if xo_5 in fps:
            mne = fps[xo_5]
            if rc:
                mne += "."
            result.mnemonic = mne
            if xo_5 == 25:
                result.operands = f"f{frd}, f{fra}, f{frc}"
            elif xo_5 in (29, 28, 31, 30):
                result.operands = f"f{frd}, f{fra}, f{frc}, f{frb}"
            elif xo_5 in (24, 26, 22):
                result.operands = f"f{frd}, f{frb}"
            else:
                result.operands = f"f{frd}, f{fra}, f{frb}"
            return result

    if opcd == 4:
        vd = rd
        va = ra
        vb = bits(insn, 16, 20)
        vc = bits(insn, 21, 25)
        xo_full = bits(insn, 21, 31)
        xo_6 = bits(insn, 26, 31)

        vmx_va = {
            32: "vmhaddshs", 33: "vmhraddshs", 34: "vmladduhm",
            36: "vmsumubm", 37: "vmsummbm", 38: "vmsumuhm", 39: "vmsumuhs",
            40: "vmsumshm", 41: "vmsumshs",
            42: "vsel", 43: "vperm", 44: "vsldoi",
            46: "vmaddfp", 47: "vnmsubfp",
        }
        if xo_6 in vmx_va:
            result.mnemonic = vmx_va[xo_6]
            if xo_6 == 44:
                result.operands = f"v{vd}, v{va}, v{vb}, {vc}"
            else:
                result.operands = f"v{vd}, v{va}, v{vb}, v{vc}"
            return result

        vmx_cmp_rc = bit(insn, 21)

        vmx_cmp = {
            6: "vcmpequb", 70: "vcmpequh", 134: "vcmpequw",
            198: "vcmpeqfp", 966: "vcmpbfp", 454: "vcmpgefp",
            710: "vcmpgtfp",
            774: "vcmpgtsb", 838: "vcmpgtsh", 902: "vcmpgtsw",
            518: "vcmpgtub", 582: "vcmpgtuh", 646: "vcmpgtuw",
        }
        base_cmp_xo = xo_full & ~(1 << 10)
        if base_cmp_xo in vmx_cmp:
            mne = vmx_cmp[base_cmp_xo]
            if vmx_cmp_rc:
                mne += "."
            result.mnemonic = mne
            result.operands = f"v{vd}, v{va}, v{vb}"
            return result

        vmx_vx = {
            10: "vaddfp", 74: "vsubfp",
            1034: "vmaxfp", 1098: "vminfp",
            266: "vrefp", 330: "vrsqrtefp", 394: "vexptefp", 458: "vlogefp",
            522: "vrfin", 586: "vrfiz", 650: "vrfip", 714: "vrfim",

            0: "vaddubm", 64: "vadduhm", 128: "vadduwm",
            1024: "vsububm", 1088: "vsubuhm", 1152: "vsubuwm",
            512: "vaddubs", 576: "vadduhs", 640: "vadduws",
            768: "vaddsbs", 832: "vaddshs", 896: "vaddsws",
            1536: "vsububs", 1600: "vsubuhs", 1664: "vsubuws",
            1792: "vsubsbs", 1856: "vsubshs", 1920: "vsubsws",

            2: "vmaxub", 66: "vmaxuh", 130: "vmaxuw",
            258: "vmaxsb", 322: "vmaxsh", 386: "vmaxsw",
            514: "vminub", 578: "vminuh", 642: "vminuw",
            770: "vminsb", 834: "vminsh", 898: "vminsw",

            1028: "vand", 1092: "vandc", 1156: "vor", 1220: "vxor",
            1284: "vnor",

            260: "vslb", 324: "vslh", 388: "vslw",
            516: "vsrb", 580: "vsrh", 644: "vsrw",
            772: "vsrab", 836: "vsrah", 900: "vsraw",


            12: "vmrghb", 76: "vmrghh", 140: "vmrghw",
            268: "vmrglb", 332: "vmrglh", 396: "vmrglw",


            14: "vpkuhum", 78: "vpkuwum",
            142: "vpkuhus", 206: "vpkuwus",
            270: "vpkshus", 334: "vpkswus",
            398: "vpkshss", 462: "vpkswss",
            526: "vupkhsb", 590: "vupkhsh",
            654: "vupklsb", 718: "vupklsh",

            4: "vrlb", 68: "vrlh", 132: "vrlw",

            1026: "vavgub", 1090: "vavguh", 1154: "vavguw",
            1282: "vavgsb", 1346: "vavgsh", 1410: "vavgsw",

            8: "vmuloub", 72: "vmulouh",
            264: "vmulosb", 328: "vmulosh",
            520: "vmuleub", 584: "vmuleuh",
            776: "vmulesb", 840: "vmulesh",

            1928: "vsumsws", 1672: "vsum2sws",
            1544: "vsum4ubs", 1800: "vsum4sbs", 1608: "vsum4shs",
        }
        if xo_full in vmx_vx:
            result.mnemonic = vmx_vx[xo_full]
            result.operands = f"v{vd}, v{va}, v{vb}"
            return result

        vmx_uimm = {
            524: "vspltb", 588: "vsplth", 652: "vspltw",
            778: "vcfux", 842: "vcfsx", 906: "vctuxs", 970: "vctsxs",
        }
        if xo_full in vmx_uimm:
            result.mnemonic = vmx_uimm[xo_full]
            result.operands = f"v{vd}, v{vb}, {va}"
            return result

        vmx_simm = {780: "vspltisb", 844: "vspltish", 908: "vspltisw"}
        if xo_full in vmx_simm:
            result.mnemonic = vmx_simm[xo_full]
            simm = va - 32 if va >= 16 else va
            result.operands = f"v{vd}, {simm}"
            return result

        if xo_full == 1540:
            result.mnemonic, result.operands = "mfvscr", f"v{vd}"
            return result
        if xo_full == 1604:
            result.mnemonic, result.operands = "mtvscr", f"v{vb}"
            return result

        result.mnemonic = f"vmx_x{xo_full}"
        result.operands = f"v{vd}, v{va}, v{vb}"
        return result


    if opcd == 3:
        to = rd
        result.mnemonic = "twi"
        result.operands = f"{to}, r{ra}, {si}"
        return result

    if opcd == 2:
        to = rd
        result.mnemonic = "tdi"
        result.operands = f"{to}, r{ra}, {si}"
        return result

    result.mnemonic = f".word"
    result.operands = f"0x{insn:08X}"
    return result


def disassemble_bytes(data: bytes, base_addr: int = 0,
                      big_endian: bool = True) -> list[Instruction]:
    fmt = ">I" if big_endian else "<I"
    instructions: list[Instruction] = []
    for off in range(0, len(data) - 3, 4):
        raw = struct.unpack_from(fmt, data, off)[0]
        addr = base_addr + off
        insn = decode(raw, addr)
        instructions.append(insn)
    return instructions


def detect_functions(instructions: list[Instruction]) -> list[dict]:
    functions: list[dict] = []
    n = len(instructions)
    current_func_start = None

    for i, insn in enumerate(instructions):
        if insn.mnemonic == "mflr" and insn.operands == "r0":
            for j in range(1, min(4, n - i)):
                nxt = instructions[i + j]
                if nxt.mnemonic in ("stw", "std") and "r0," in nxt.operands and "(r1)" in nxt.operands:
                    current_func_start = insn.addr
                    break

        if insn.mnemonic == "blr" and current_func_start is not None:
            functions.append({
                "start": current_func_start,
                "end": insn.addr + 4,
                "size": insn.addr + 4 - current_func_start,
            })
            current_func_start = None

        if insn.mnemonic == "bctr" and current_func_start is not None:
            if i + 1 < n and instructions[i + 1].mnemonic == "mflr":
                functions.append({
                    "start": current_func_start,
                    "end": insn.addr + 4,
                    "size": insn.addr + 4 - current_func_start,
                })
                current_func_start = None

    return functions


def main() -> None:
    parser = argparse.ArgumentParser(description="PPU (PowerPC 64) disassembler for PS3 binaries")
    parser.add_argument("input", help="Input ELF file or raw binary")
    parser.add_argument("--base", type=lambda x: int(x, 0), default=0,
                        help="Base address for raw binary (hex ok)")
    parser.add_argument("--raw", action="store_true",
                        help="Treat input as raw binary (not ELF)")
    parser.add_argument("--functions", action="store_true",
                        help="Detect and list function boundaries")
    parser.add_argument("--json", action="store_true",
                        help="Output as JSON instead of text")
    parser.add_argument("--offset", type=lambda x: int(x, 0), default=0,
                        help="Start offset within file (for raw mode)")
    parser.add_argument("--length", type=lambda x: int(x, 0), default=0,
                        help="Number of bytes to disassemble (0=all)")
    parser.add_argument("--va", type=lambda x: int(x, 0), default=None,
                        help="Virtual address to disassemble (ELF: maps va->file via PT_LOAD segments; "
                             "use with --length). Correct alternative to --raw --offset, which treats "
                             "--offset as a raw FILE offset (off by the segment base).")
    parser.add_argument("--little-endian", action="store_true",
                        help="Decode as little-endian")
    args = parser.parse_args()

    with open(args.input, "rb") as f:
        file_data = f.read()

    big_endian = not args.little_endian
    base_addr = args.base

    if args.va is not None:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from elf_parser import ELFFile, PT_LOAD
        elf = ELFFile(args.input); elf.load()
        big_endian = elf.elf_header.big_endian
        va = args.va; seg = None
        for ph in elf.program_headers:
            if ph.p_type == PT_LOAD and ph.p_vaddr <= va < ph.p_vaddr + ph.p_filesz:
                seg = ph; break
        if seg is None:
            print(f"Error: vaddr 0x{va:08X} not in any PT_LOAD file-backed range", file=sys.stderr)
            sys.exit(1)
        file_off = seg.p_offset + (va - seg.p_vaddr)
        n = args.length or 0x40
        data = file_data[file_off:file_off + n]
        for i in disassemble_bytes(data, va, big_endian):
            print(i)
        return

    if args.raw:
        data = file_data[args.offset:]
        if args.length:
            data = data[:args.length]
    else:
        try:
            sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
            from elf_parser import ELFFile, PT_LOAD
            elf = ELFFile(args.input)
            elf.load()
            big_endian = elf.elf_header.big_endian

            segments = []
            for ph in elf.program_headers:
                if ph.p_type == PT_LOAD and (ph.p_flags & 1):
                    seg_data = elf.get_segment_data(elf.program_headers.index(ph))
                    segments.append((ph.p_vaddr, seg_data))

            if not segments:
                for ph in elf.program_headers:
                    if ph.p_type == PT_LOAD and ph.p_filesz > 0:
                        seg_data = elf.get_segment_data(elf.program_headers.index(ph))
                        segments.append((ph.p_vaddr, seg_data))
                        break

            all_insns: list[Instruction] = []
            for seg_base, seg_data in segments:
                all_insns.extend(disassemble_bytes(seg_data, seg_base, big_endian))

            if args.functions:
                funcs = detect_functions(all_insns)
                if args.json:
                    out = [{"start": f"0x{f['start']:X}", "end": f"0x{f['end']:X}",
                            "size": f['size']} for f in funcs]
                    print(json.dumps(out, indent=2))
                else:
                    print(f"Detected {len(funcs)} functions:")
                    for f in funcs:
                        print(f"  0x{f['start']:08X} - 0x{f['end']:08X}  ({f['size']} bytes)")
                return

            if args.json:
                out = [{"addr": f"0x{i.addr:08X}", "hex": f"{i.raw:08X}",
                        "mnemonic": i.mnemonic, "operands": i.operands} for i in all_insns]
                print(json.dumps(out, indent=2))
            else:
                for i in all_insns:
                    print(i)
            return

        except Exception as exc:
            print(f"Warning: Could not parse as ELF ({exc}), treating as raw binary",
                  file=sys.stderr)
            data = file_data[args.offset:]
            if args.length:
                data = data[:args.length]

    insns = disassemble_bytes(data, base_addr, big_endian)

    if args.functions:
        funcs = detect_functions(insns)
        if args.json:
            out = [{"start": f"0x{f['start']:X}", "end": f"0x{f['end']:X}",
                    "size": f['size']} for f in funcs]
            print(json.dumps(out, indent=2))
        else:
            print(f"Detected {len(funcs)} functions:")
            for f in funcs:
                print(f"  0x{f['start']:08X} - 0x{f['end']:08X}  ({f['size']} bytes)")
        return

    if args.json:
        out = [{"addr": f"0x{i.addr:08X}", "hex": f"{i.raw:08X}",
                "mnemonic": i.mnemonic, "operands": i.operands} for i in insns]
        print(json.dumps(out, indent=2))
    else:
        for i in insns:
            print(i)


if __name__ == "__main__":
    main()
