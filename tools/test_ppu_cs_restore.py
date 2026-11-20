#!/usr/bin/env python3
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ppu_disasm
from ppu_lifter import PPULifter

BASE = 0x10000


def w_ld(rt, ra, d):   return (58 << 26) | (rt << 21) | (ra << 16) | (d & 0xFFFC)
def w_addi(rt, ra, s): return (14 << 26) | (rt << 21) | (ra << 16) | (s & 0xFFFF)
def w_blr():           return (19 << 26) | (20 << 21) | (16 << 1)


words = [w_ld(23, 1, 0x918), w_addi(23, 23, 1), w_ld(23, 1, 0x888), w_blr()]
insns = [ppu_disasm.decode(w, BASE + 4 * i) for i, w in enumerate(words)]
func = PPULifter().lift_function(insns, BASE, BASE + 4 * len(words))
body = "\n".join(func.body_lines)

assert "uint64_t _cs_23 = vm_read64(ctx->gpr[1] + 0x888);" in body, body
assert "ctx->gpr[23] = vm_read64(ctx->gpr[1] + 0x918);" in body, body
print("PASS: second-slot reload stays a real load")
