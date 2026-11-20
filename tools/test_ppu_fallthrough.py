#!/usr/bin/env python3
import os
import sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)

import ppu_disasm
from ppu_lifter import PPULifter, _last_line_is_terminator

BASE = 0x10000


def w_cmpwi(bf, ra, si):  return (11 << 26) | (bf << 23) | (ra << 16) | (si & 0xFFFF)
def w_bc(bo, bi, rel):    return (16 << 26) | (bo << 21) | (bi << 16) | (rel & 0xFFFC)
def w_blr():              return (19 << 26) | (20 << 21) | (16 << 1)
def w_li(rt, si):         return (14 << 26) | (rt << 21) | (si & 0xFFFF)
def w_sc():               return 0x44000002


def lift(words):
    insns = [ppu_disasm.decode(w, BASE + 4 * i) for i, w in enumerate(words)]
    return PPULifter(prefix="").lift_function(insns, BASE, BASE + 4 * len(words))


FAILS = []


def check(name, got, want):
    print(f"{'ok  ' if got == want else 'FAIL'} {name}" + ("" if got == want else f": got {got!r}, want {want!r}"))
    if got != want:
        FAILS.append(name)


def main() -> int:
    check("conditional goto is not a terminator",
          _last_line_is_terminator(["        if ((!((ctx->cr >> 0) & 2))) goto loc_00010000;"]), False)
    check("bdnz is not a terminator",
          _last_line_is_terminator(["        if (((ctx->ctr = (uint32_t)(ctx->ctr - 1)) != 0)) goto loc_00010000;"]), False)
    check("guarded tail call is not a terminator",
          _last_line_is_terminator(["        if (((ctx->cr >> 0) & 2)) { PPU_TAIL return func_00010100(ctx); }"]), False)
    check("unconditional goto is a terminator",
          _last_line_is_terminator(["        goto loc_00010000;"]), True)
    check("return is a terminator", _last_line_is_terminator(["        return;"]), True)
    check("syscall is not a terminator", _last_line_is_terminator(["        lv2_syscall(ctx);"]), False)

    func = lift([w_cmpwi(0, 3, 0), w_bc(4, 2, -4)])
    check("function ending in bne records its fall-through", func.fallthrough_to, BASE + 8)
    func = lift([w_li(11, 141), w_sc()])
    check("function ending in sc records its fall-through", func.fallthrough_to, BASE + 8)
    func = lift([w_cmpwi(0, 3, 0), w_blr()])
    check("function ending in blr has no fall-through", func.fallthrough_to, 0)

    if FAILS:
        print(f"FAILED: {', '.join(FAILS)}")
        return 1
    print("all fall-through checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
