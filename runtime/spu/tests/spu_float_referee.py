#!/usr/bin/env python3
import re
import sys
from fractions import Fraction as F

FLOAT_OPS = ("fa", "fs", "fm", "fma", "fms", "fnms", "cflts", "cfltu", "csflt", "cuflt",
             "fceq", "fcgt", "fcmeq", "fcmgt", "rotmai")


def value(w):
    e = (w >> 23) & 0xFF
    if e == 0:
        return F(0)
    return (-1 if w >> 31 else 1) * F((w & 0x7FFFFF) | 0x800000) * F(2) ** (e - 150)


def to_single(v):
    if v == 0:
        return 0
    sign = 0x80000000 if v < 0 else 0
    a = abs(v)
    e = a.numerator.bit_length() - a.denominator.bit_length()
    if F(2) ** e > a:
        e -= 1
    if F(2) ** (e + 1) <= a:
        e += 1
    be = e + 127
    if be > 255:
        return sign | 0x7FFFFFFF
    if be < 1:
        return 0
    return sign | (be << 23) | (int(a / F(2) ** (e - 23)) & 0x7FFFFF)


def spec(op, insn, a, b, c):
    if op in ("fa", "fs", "fm", "fma", "fms", "fnms"):
        x, y, z = value(a), value(b), value(c)
        return to_single({"fa": x + y, "fs": x - y, "fm": x * y,
                          "fma": x * y + z, "fms": x * y - z, "fnms": z - x * y}[op])
    if op in ("fceq", "fcgt", "fcmeq", "fcmgt"):
        x, y = value(a), value(b)
        if op in ("fcmeq", "fcmgt"):
            x, y = abs(x), abs(y)
        return 0xFFFFFFFF if (x == y if op in ("fceq", "fcmeq") else x > y) else 0
    if op == "rotmai":
        n = (0 - ((insn >> 14) & 0x7F)) & 0x3F
        sv = a - (1 << 32) if a >> 31 else a
        return (sv >> min(n, 31)) & 0xFFFFFFFF
    i8 = (insn >> 14) & 0xFF
    if op in ("cflts", "cfltu"):
        scale = 173 - i8
        if not 0 <= scale <= 127:
            return None
        t = int(value(a) * F(2) ** scale)
        if op == "cflts":
            return 0x7FFFFFFF if t > 2 ** 31 - 1 else 0x80000000 if t < -2 ** 31 else t & 0xFFFFFFFF
        return 0 if t < 0 else 0xFFFFFFFF if t > 2 ** 32 - 1 else t
    scale = 155 - i8
    if not 0 <= scale <= 127:
        return None
    iv = a if op == "cuflt" else (a - (1 << 32) if a >> 31 else a)
    return to_single(F(iv) / F(2) ** scale)


def words(h):
    return [int(x, 16) for x in h.split("_")]


def main():
    lines = open(sys.argv[1], encoding="latin-1").read().split("\n")
    ours = undefined = 0
    bad = []
    for i, l in enumerate(lines):
        m = re.search(r"MISMATCH (\w+) insn=([0-9A-F]+) .*?: r\d+ got ([0-9A-F_]+) exp ([0-9A-F_]+)", l)
        if not m:
            if "MISMATCH" in l:
                bad.append(l.strip())
            continue
        op, insn, got, exp = m.group(1), int(m.group(2), 16), words(m.group(3)), words(m.group(4))
        o = re.search(r"ra=([0-9A-F_]+) rb=([0-9A-F_]+) rt/rc=([0-9A-F_]+)", lines[i + 1] if i + 1 < len(lines) else "")
        if op not in FLOAT_OPS or not o:
            bad.append(l.strip()); continue
        ra, rb, rc = words(o.group(1)), words(o.group(2)), words(o.group(3))
        for k in range(4):
            if got[k] == exp[k]:
                continue
            s = spec(op, insn, ra[k], rb[k], rc[k])
            if s is None:
                undefined += 1
            elif got[k] == s:
                ours += 1
            else:
                bad.append("%s insn=%08X lane %d: ours %08X rpcs3 %08X isa %08X" % (op, insn, k, got[k], exp[k], s))
    print("float referee: %d lane(s) where ours is the ISA's exact result and RPCS3 is not; "
          "%d with an ISA-undefined scale; %d unresolved" % (ours, undefined, len(bad)))
    for b in bad[:20]:
        print("  UNRESOLVED", b)
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
