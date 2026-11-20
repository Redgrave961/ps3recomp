#!/usr/bin/env python3
import os
import sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)

import ppu_disasm
from ppu_lifter import discover_jump_tables

TEXT_LO, TEXT_HI = 0x10000, 0x20000
TOC = 0x100000


def w_cmplwi(bf, ra, ui):  return (10 << 26) | (bf << 23) | (ra << 16) | (ui & 0xFFFF)
def w_lwz(rt, ra, d):      return (32 << 26) | (rt << 21) | (ra << 16) | (d & 0xFFFF)
def w_lwzx(rt, ra, rb):    return (31 << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (23 << 1)
def w_lwax(rt, ra, rb):    return (31 << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (341 << 1)
def w_add(rt, ra, rb):     return (31 << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (266 << 1)
def w_slwi(ra, rs, n):     return (21 << 26) | (rs << 21) | (ra << 16) | (n << 11) | (0 << 6) | ((31 - n) << 1)
def w_bctr():              return (19 << 26) | (20 << 21) | (528 << 1)
def w_blr():               return (19 << 26) | (20 << 21) | (16 << 1)
def w_ld(rt, ra, d):       return (58 << 26) | (rt << 21) | (ra << 16) | (d & 0xFFFC)
def w_std(rs, ra, d):      return (62 << 26) | (rs << 21) | (ra << 16) | (d & 0xFFFC)
def w_li(rt, si):          return (14 << 26) | (rt << 21) | (si & 0xFFFF)
def w_addis(rt, ra, si):   return (15 << 26) | (rt << 21) | (ra << 16) | (si & 0xFFFF)
def w_stdu(rs, ra, d):     return (62 << 26) | (rs << 21) | (ra << 16) | (d & 0xFFFC) | 1
def w_bc(bo, bi, rel):     return (16 << 26) | (bo << 21) | (bi << 16) | (rel & 0xFFFC)
def w_b(rel):              return (18 << 26) | (rel & 0x3FFFFFC)


def w_mtctr(rs):
    spr = ((9 & 0x1F) << 5) | ((9 >> 5) & 0x1F)
    return (31 << 26) | (rs << 21) | (spr << 11) | (467 << 1)


def decode_all(words):
    return [ppu_disasm.decode(w, TEXT_LO + 4 * i) for i, w in enumerate(words)]


def discover(words, mem):
    insns = decode_all(words)
    tables = discover_jump_tables(insns, lambda a: mem.get(a & 0xFFFFFFFF),
                                  [TOC], TEXT_LO, TEXT_HI)
    return tables.get(insns[-1].addr)


FAILS = []


def check(name, got, want):
    if got == want:
        print(f"ok   {name}")
    else:
        show = lambda v: None if v is None else [f"0x{t:X}" for t in v]
        print(f"FAIL {name}: got {show(got)}, want {show(want)}")
        FAILS.append(name)


def test_encodings():
    ins = decode_all([w_cmplwi(0, 9, 3), w_lwz(11, 2, 16), w_lwzx(0, 9, 11),
                      w_add(0, 0, 11), w_slwi(9, 3, 2), w_mtctr(0), w_bctr(),
                      w_lwax(0, 9, 11)])
    got = [i.mnemonic for i in ins]
    want = ['cmplwi', 'lwz', 'lwzx', 'add', 'rlwinm', 'mtctr', 'bctr', 'lwax']
    if got != want:
        print(f"ENCODING mismatch: got {got}, want {want}")
        sys.exit(2)
    print("ok   encodings decode as the vectors assume")


def test_absolute_base_is_second_operand():
    table = 0x200400
    mem = {TOC + 8: 0,
           TOC + 16: table,
           table + 0: 0x13000, table + 4: 0x13004, table + 8: 0x13008,
           table + 12: 0}
    got = discover([w_cmplwi(0, 9, 2), w_lwz(9, 2, 8), w_lwz(11, 2, 16),
                    w_lwzx(0, 9, 11), w_mtctr(0), w_bctr()], mem)
    check("absolute table, base is the second lwzx operand",
          got, [0x13000, 0x13004, 0x13008])


def test_offset_idiom():
    table = 0x11000
    mem = {TOC + 16: table,
           table + 0: 0x100, table + 4: 0x200, table + 8: 0x300,
           table + 12: 0xFFFFFFFF}
    got = discover([w_cmplwi(0, 3, 2), w_lwz(11, 2, 16), w_slwi(9, 3, 2),
                    w_lwzx(0, 9, 11), w_add(0, 0, 11), w_mtctr(0), w_bctr()], mem)
    check("offset table, gcc idiom", got, [0x11100, 0x11200, 0x11300])


def test_offset_idiom_lwax():
    table = 0x11000
    mem = {TOC + 16: table,
           table + 0: 0x100, table + 4: (-0x200) & 0xFFFFFFFF, table + 8: 0x300,
           table + 12: 0xFFFFFFFF}
    got = discover([w_cmplwi(0, 3, 2), w_lwz(11, 2, 16), w_slwi(9, 3, 2),
                    w_lwax(0, 9, 11), w_add(0, 0, 11), w_mtctr(0), w_bctr()], mem)
    check("offset table loaded with lwax", got, [0x10E00, 0x11100, 0x11300])


def test_offset_both_operands_r2_loaded():
    table, other = 0x11000, 0x200700
    mem = {TOC + 8: other, other + 0: 0x16000, other + 4: 0,
           TOC + 16: table,
           table + 0: 0x100, table + 4: 0x200, table + 8: 0xFFFFFFFF}
    got = discover([w_cmplwi(0, 9, 2), w_lwz(9, 2, 8), w_lwz(11, 2, 16),
                    w_lwzx(0, 9, 11), w_add(0, 0, 11), w_mtctr(0), w_bctr()], mem)
    check("offset table, both operands r2-loaded", got, [0x11100, 0x11200])


def test_tie_keeps_first_candidate():
    ta, tb = 0x200500, 0x200600
    mem = {TOC + 8: ta, ta + 0: 0x14000, ta + 4: 0x14004, ta + 8: 0,
           TOC + 16: tb, tb + 0: 0x15000, tb + 4: 0x15004, tb + 8: 0}
    got = discover([w_cmplwi(0, 9, 2), w_lwz(9, 2, 8), w_lwz(11, 2, 16),
                    w_lwzx(0, 9, 11), w_mtctr(0), w_bctr()], mem)
    check("tie between operands keeps the first", got, [0x14000, 0x14004])


def test_two_level_base_past_early_return():
    mid, table = 0x200800, 0x11000
    mem = {TOC + 8: mid, mid - 16: table,
           table + 0: 0x100, table + 4: 0x200, table + 8: 0xFFFFFFFF}
    words = [w_lwz(30, 2, 8), w_cmplwi(0, 3, 2), w_bc(4, 1, 12),
             w_ld(30, 1, 0), w_blr(),
             w_lwz(11, 30, -16), w_slwi(9, 3, 2), w_lwzx(0, 9, 11),
             w_add(0, 0, 11), w_mtctr(0), w_bctr()]
    insns = decode_all(words)
    tables = discover_jump_tables(insns, lambda a: mem.get(a & 0xFFFFFFFF),
                                  [TOC], TEXT_LO, TEXT_HI, func_starts=[TEXT_LO])
    check("two-level base past an early return", tables.get(insns[-1].addr),
          [0x11100, 0x11200])


def test_two_level_base_past_tail_call():
    mid, table = 0x200A00, 0x11000
    mem = {TOC + 8: mid, mid - 16: table,
           table + 0: 0x100, table + 4: 0x200, table + 8: 0xFFFFFFFF}

    def words(tail_rel):
        return [w_lwz(30, 2, 8), w_cmplwi(0, 3, 2), w_bc(4, 1, 12),
                w_ld(30, 1, 0), w_b(tail_rel),
                w_lwz(11, 30, -16), w_slwi(9, 3, 2), w_lwzx(0, 9, 11),
                w_add(0, 0, 11), w_mtctr(0), w_bctr()]

    insns = decode_all(words(-0x110))
    tables = discover_jump_tables(insns, lambda a: mem.get(a & 0xFFFFFFFF),
                                  [TOC], TEXT_LO, TEXT_HI, func_starts=[TEXT_LO])
    check("two-level base past a backward tail call", tables.get(insns[-1].addr),
          [0x11100, 0x11200])

    insns = decode_all(words(0x100 - 16))
    tables = discover_jump_tables(insns, lambda a: mem.get(a & 0xFFFFFFFF),
                                  [TOC], TEXT_LO, TEXT_HI,
                                  func_starts=[TEXT_LO, TEXT_LO + 0x100])
    check("two-level base past a forward tail call", tables.get(insns[-1].addr),
          [0x11100, 0x11200])

    insns = decode_all(words(-0x110))
    tables = discover_jump_tables(insns, lambda a: mem.get(a & 0xFFFFFFFF),
                                  [TOC], TEXT_LO, TEXT_HI)
    check("tail call needs func_starts to count as an exit",
          tables.get(insns[-1].addr), None)


def test_base_before_fallthrough_split():
    mid, table = 0x200900, 0x11000
    mem = {TOC + 8: mid, mid - 16: table,
           table + 0: 0x100, table + 4: 0x200, table + 8: 0xFFFFFFFF}
    words = [w_lwz(30, 2, 8), w_std(30, 1, 0x450), w_slwi(9, 3, 2),
             w_lwz(11, 30, -16), w_lwzx(0, 9, 11), w_add(0, 0, 11),
             w_mtctr(0), w_bctr()]
    insns = decode_all(words)
    tables = discover_jump_tables(insns, lambda a: mem.get(a & 0xFFFFFFFF),
                                  [TOC], TEXT_LO, TEXT_HI,
                                  func_starts=[TEXT_LO, TEXT_LO + 12])
    check("base before a fall-through split point", tables.get(insns[-1].addr),
          [0x11100, 0x11200])


def test_multi_toc_prefers_targets_near_the_dispatcher():
    text_hi = 0x01000000
    wrong_toc, right_toc = 0x200000, 0x300000
    near, far = 0x11000, 0x800000
    mem = {wrong_toc + 16: far,
           far + 0: 0x10, far + 4: 0x20, far + 8: 0x30, far + 12: 0xFFFFFFFF,
           right_toc + 16: near,
           near + 0: 0x100, near + 4: 0x200, near + 8: 0xFFFFFFFF}
    insns = decode_all([w_cmplwi(0, 3, 2), w_lwz(11, 2, 16), w_slwi(9, 3, 2),
                        w_lwzx(0, 9, 11), w_add(0, 0, 11), w_mtctr(0), w_bctr()])
    tables = discover_jump_tables(insns, lambda a: mem.get(a & 0xFFFFFFFF),
                                  [wrong_toc, right_toc], TEXT_LO, text_hi)
    check("multi-TOC: near targets beat more targets far away",
          tables.get(insns[-1].addr), [0x11100, 0x11200])


def w_ori(ra, rs, ui):     return (24 << 26) | (rs << 21) | (ra << 16) | (ui & 0xFFFF)


def test_bound_compare_beyond_window():
    n, table = 300, 0x11000
    first = table + 4 * n
    mem = {TOC + 16: table}
    for k in range(n):
        mem[table + 4 * k] = first + 4 * k - table
    words = ([w_cmplwi(0, 3, n - 1), w_lwz(11, 2, 16)]
             + [w_ori(20, 20, k) for k in range(31)]
             + [w_slwi(9, 3, 2), w_lwzx(0, 9, 11), w_add(0, 0, 11),
                w_mtctr(0), w_bctr()])
    got = discover(words, mem)
    check("bound compare beyond the 30-instruction window", got,
          [first + 4 * k for k in range(n)])


def main() -> int:
    test_encodings()
    test_absolute_base_is_second_operand()
    test_offset_idiom()
    test_offset_idiom_lwax()
    test_offset_both_operands_r2_loaded()
    test_tie_keeps_first_candidate()
    test_two_level_base_past_early_return()
    test_two_level_base_past_tail_call()
    test_base_before_fallthrough_split()
    test_const_base_no_table()
    test_const_base_needs_constant()
    test_multi_toc_prefers_targets_near_the_dispatcher()
    test_bound_compare_beyond_window()
    test_spilled_base_reused_as_target()
    if FAILS:
        print(f"FAILED: {', '.join(FAILS)}")
        return 1
    print("all jump-table vectors passed")
    return 0


def test_const_base_no_table():
    base = TEXT_LO + 0x14
    mem = {base + 4 * k: w_stdu(4, 3, 8) for k in range(3)}
    mem[base + 12] = w_blr()
    got = discover([w_li(8, 0x14), w_addis(8, 8, 1), w_add(8, 8, 7),
                    w_mtctr(8), w_bctr()], mem)
    check("constant-base computed jump", got, [base, base + 4, base + 8, base + 12])


def test_const_base_needs_constant():
    base = TEXT_LO + 0x14
    mem = {base: w_blr(), TOC + 8: base}
    got = discover([w_lwz(8, 2, 8), w_add(8, 8, 7), w_mtctr(8), w_bctr()], mem)
    check("non-constant base is not a constant-base jump", got, None)


def test_spilled_base_reused_as_target():
    table = 0x11000
    mem = {TOC + 16: table, table + 0: 0x100, table + 4: 0x200, table + 8: 0x300,
           table + 12: 0xFFFFFFFF}
    words = [w_lwz(10, 2, 16), w_std(10, 1, 0x2D0), w_li(5, 0), w_cmplwi(0, 0, 2),
             w_ld(11, 1, 0x2D0), w_slwi(9, 0, 2), w_lwzx(0, 9, 11), w_add(11, 0, 11),
             w_mtctr(11), w_bctr()]
    check("spilled base, base register reused for the target", discover(words, mem),
          [0x11100, 0x11200, 0x11300])
    words[1] = w_std(10, 1, 0x2D8)
    check("reload of a slot nothing stored is not a base", discover(words, mem), None)


if __name__ == "__main__":
    sys.exit(main())
