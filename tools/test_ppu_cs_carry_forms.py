#!/usr/bin/env python3
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ppu_disasm
from ppu_lifter import PPULifter

BASE = 0x10000


def w_std(rs, ra, d):  return (62 << 26) | (rs << 21) | (ra << 16) | (d & 0xFFFC)
def w_ld(rt, ra, d):   return (58 << 26) | (rt << 21) | (ra << 16) | (d & 0xFFFC)
def w_mr(ra, rs):      return (31 << 26) | (rs << 21) | (ra << 16) | (rs << 11) | (444 << 1)
def w_addi(rt, ra, s): return (14 << 26) | (rt << 21) | (ra << 16) | (s & 0xFFFF)
def w_addic(rt, ra, s): return (12 << 26) | (rt << 21) | (ra << 16) | (s & 0xFFFF)
def w_blr():           return (19 << 26) | (20 << 21) | (16 << 1)


def lift(define):
    words = [define, w_std(22, 1, 0xA8), w_mr(3, 4), w_ld(22, 1, 0xA8), w_blr()]
    insns = [ppu_disasm.decode(w, BASE + 4 * i) for i, w in enumerate(words)]
    func = PPULifter().lift_function(insns, BASE, BASE + 4 * len(words))
    return "\n".join(func.body_lines)


def test_carry_form_definition_is_seen():
    body = lift(w_addic(22, 27, 1))
    assert "ctx->gpr[22] = _r;" in body, "addic no longer emitted in the compound form this test targets:\n" + body
    assert "_cs_22" not in body, "reload of an addic result became the entry value:\n" + body
    assert "ctx->gpr[22] = vm_read64(ctx->gpr[1] + 0xA8);" in body, body


def test_plain_definition_unchanged():
    body = lift(w_addi(22, 27, 1))
    assert "_cs_22" not in body, body
    assert "ctx->gpr[22] = vm_read64(ctx->gpr[1] + 0xA8);" in body, body


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print(f"PASS: {name}")
