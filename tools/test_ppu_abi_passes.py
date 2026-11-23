#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))


def _load(name, rel_path):
    spec = importlib.util.spec_from_file_location(name, ROOT / rel_path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


L = _load("ppu_lifter", "tools/ppu_lifter.py")
D = sys.modules["ppu_disasm"]

CMP7 = "{ int64_t a = (int32_t)ctx->gpr[4]; int64_t b = (int32_t)ctx->gpr[5]; uint32_t cr_val = (a < b) ? 8 : (a > b) ? 4 : 2; ctx->cr = (ctx->cr & ~(0xFu << 0)) | (cr_val << 0); }"
CMP0 = CMP7.replace("<< 0)", "<< 28)")
CMP3 = CMP7.replace("<< 0)", "<< 16)")
READ7 = "if (((ctx->cr >> 3) & 1)) goto loc_2;"
READ0 = "if (((ctx->cr >> 31) & 1)) goto loc_2;"
READ3 = "if (((ctx->cr >> 19) & 1)) goto loc_2;"
CALL = "ctx->lr = 0x1; func_00002000(ctx);"


def fused(lines):
    return [x.strip() for x in L._fuse_cr(["    " + x if not x.endswith(":") else x for x in lines])]


def test_cr_call_clobbers_volatile_fields():
    cases = {
        "direct call clobbers cr7": ([CMP7, CALL, CMP7.replace("gpr[4]", "gpr[6]"), READ7, "return;", "loc_2:", "return;"], CMP7, True),
        "direct call clobbers cr0": ([CMP0, CALL, CMP0.replace("gpr[4]", "gpr[6]"), "return;"], CMP0, True),
        "read after the call keeps cr7": ([CMP7, CALL, READ7, "return;", "loc_2:", "return;"], CMP7, False),
        "cr3 survives a call": ([CMP3, CALL, READ3, "return;", "loc_2:", "return;"], CMP3, False),
        "read before the call": ([CMP7, "ctx->gpr[3] = 1;", READ7, CALL, "return;", "loc_2:", "return;"], CMP7, False),
        "draining call": ([CMP7, CALL + " DRAIN_TRAMPOLINE(ctx);", READ7, "return;", "loc_2:", "return;"], CMP7, False),
        "indirect call": ([CMP7, "ctx->lr = 0x1; ppu_call_ctr(ctx);", "{ PPU_TAIL return func_00003000(ctx); }"], CMP7, False),
        "call to self never returns": ([CMP7, "ctx->lr = 0x2004; func_00002000(ctx);", READ7, "return;", "loc_2:", "return;"], CMP7, True),
        "syscall keeps cr": ([CMP7, "lv2_syscall(ctx);", READ7, "return;", "loc_2:", "return;"], CMP7, False),
        "return drops volatile cr": ([CMP7, "return;"], CMP7, True),
        "return keeps cr3": ([CMP3, "return;"], CMP3, False),
        "conditional return keeps cr3": ([CMP3, "if ((int64_t)ctx->gpr[3] == 0) return;", CMP3.replace("gpr[4]", "gpr[6]"), "return;"], CMP3, False),
        "hle return drops volatile cr": ([CMP0, "ps3_hle_call(0x12u, ctx); return;"], CMP0, True),
        "tail call keeps cr": ([CMP7, "{ PPU_TAIL return func_00002000(ctx); }"], CMP7, False),
        "trampoline keeps cr": ([CMP7, "g_trampoline_fn = (void(*)(void*))func_00002000; return;"], CMP7, False),
        "dead without a branch": ([CMP7, CMP7.replace("gpr[4]", "gpr[6]"), "return;"], CMP7, True),
    }
    for name, (lines, line, dropped) in cases.items():
        out = fused(lines)
        assert (line not in out) == dropped, (name, out)


def test_cr_exact_forms():
    cror = "{ uint32_t a = (ctx->cr >> (31 - 0)) & 1; uint32_t b = (ctx->cr >> (31 - 2)) & 1; ctx->cr = (ctx->cr & ~(1u << (31 - 6))) | ((a | b) << (31 - 6)); }"
    br6 = "if (((ctx->cr >> 25) & 1)) goto loc_2;"
    out = fused([CMP0, cror, CALL, "return;", "loc_2:", "return;"])
    assert CMP0 in out and cror not in out, out
    out = fused([CMP0, cror, br6, CALL, "return;", "loc_2:", "return;"])
    assert CMP0 in out and cror in out, out
    mtcrf = "ctx->cr = (ctx->cr & 0x0FFFFFFFu) | ((uint32_t)ctx->gpr[12] & 0xF0000000u);"
    out = fused([CMP0, mtcrf, "{ PPU_TAIL return func_00002000(ctx); }"])
    assert CMP0 not in out and mtcrf in out, out
    out = fused([mtcrf, CMP0, "{ PPU_TAIL return func_00002000(ctx); }"])
    assert mtcrf not in out, out
    mfcr = "ctx->gpr[12] = ctx->cr;"
    rlw = "ctx->gpr[12] = ppc_rlw64(ctx->gpr[12], 1, 31, 31, 0, 0);"
    out = fused([CMP0, CMP7, mfcr, rlw, CALL, "return;"])
    assert CMP0 in out and CMP7 not in out, out
    out = fused([CMP0, CMP7, mfcr, "vm_write32(ctx->gpr[1] + 0x8, ctx->gpr[12]);", CALL, "return;"])
    assert CMP0 in out and CMP7 in out, out
    mfocrf = "ctx->gpr[12] = ctx->cr & 0x0000000Fu;"
    out = fused([CMP0, CMP7, mfocrf, CALL, "return;"])
    assert CMP0 not in out and CMP7 in out, out
    rec = ("ctx->gpr[3] = ctx->gpr[4] & ctx->gpr[5]; { int64_t _r = (int64_t)ctx->gpr[3]; "
           "uint32_t _c = (_r < 0) ? 8u : (_r > 0) ? 4u : 2u; ctx->cr = (ctx->cr & 0x0FFFFFFFu) | (_c << 28); }")
    assert fused([rec, CALL, "return;"])[0] == "ctx->gpr[3] = ctx->gpr[4] & ctx->gpr[5];"
    vcmp = "{ uint32_t _c6 = vmx_vcmpgtfp(&ctx->vr[1], &ctx->vr[2], &ctx->vr[3]); ctx->cr = (ctx->cr & ~0xF0u) | (_c6 << 4); }"
    assert fused([vcmp, CALL, "return;"])[0] == "{ vmx_vcmpgtfp(&ctx->vr[1], &ctx->vr[2], &ctx->vr[3]); }"
    stwcx = ("{ uint64_t ea = ctx->gpr[3]; int _sc = (ctx->reserve_addr == (uint32_t)ea) && ppu_stwcx32(ctx, ea, "
             "(uint32_t)ctx->reserve_value, (uint32_t)ctx->gpr[4]); ctx->cr = (ctx->cr & ~(0xFu << 28)) | "
             "(_sc ? (2u << 28) : 0u); ctx->reserve_addr = 0; }")
    assert fused([stwcx, CALL, "return;"])[0] == (
        "{ uint64_t ea = ctx->gpr[3]; (ctx->reserve_addr == (uint32_t)ea) && ppu_stwcx32(ctx, ea, "
        "(uint32_t)ctx->reserve_value, (uint32_t)ctx->gpr[4]); ctx->reserve_addr = 0; }")


def _fn(name, addr, lines):
    return L.LiftedFunction(name=name, start_addr=addr, end_addr=addr + 0x100, body_lines=["    " + x for x in lines])


def test_cr_call_abi():
    callee = _fn("func_00002000", 0x2000, [CMP0, "return;"])
    reader = _fn("func_00003000", 0x3000, [READ0, "return;", "loc_2:", "return;"])
    cases = [
        ([CALL, CMP0, READ0, "return;", "loc_2:", "return;"], 0),
        ([CALL, READ3, "return;", "loc_2:", "return;"], 0),
        ([CALL, "ctx->gpr[12] = ctx->cr;", "ctx->gpr[12] = ppc_rlw64(ctx->gpr[12], 13, 31, 31, 0, 0);", "return;"], 0x20000),
        ([CALL, READ0, "return;", "loc_2:", "return;"], 0x80000000),
        (["ctx->lr = 0x1; ppu_call_ctr(ctx);", READ7, "return;", "loc_2:", "return;"], 0x8),
        ([CALL, "{ PPU_TAIL return func_00003000(ctx); }"], 0x80000000),
        (["ctx->lr = 0x1004; func_00001000(ctx);", READ0, "return;", "loc_2:", "return;"], 0),
        (["return;", CALL, READ0, "return;", "loc_2:", "return;"], 0),
    ]
    for body, across in cases:
        entry, keep = L.cr_call_abi([_fn("func_00001000", 0x1000, body), callee, reader], frozenset())
        assert keep == L._CR_SAVED | across, (body, hex(keep))
        assert entry["func_00003000"] == 0x80000000 and entry.get("func_00002000", 0) == 0, entry
    fall = _fn("func_00001000", 0x1000, [CALL])
    fall.fallthrough_to = 0x3000
    assert L.cr_call_abi([fall, callee, reader], frozenset())[1] == L._CR_SAVED | 0x80000000
    out = [x.strip() for x in L._fuse_cr(["    " + x for x in [CMP0, "return;"]], {}, L._CR_SAVED | 0x80000000)]
    assert CMP0 in out, out
    out = [x.strip() for x in L._fuse_cr(["    " + x for x in [CMP0, CALL, CMP0.replace("gpr[4]", "gpr[6]"), "return;"]],
                                         {"func_00002000": 0x20000000}, L._CR_SAVED)]
    assert CMP0 in out, out


def li(rd, v): return (14 << 26) | (rd << 21) | (v & 0xFFFF)
def addi(rd, ra, v): return (14 << 26) | (rd << 21) | (ra << 16) | (v & 0xFFFF)
def add(rd, ra, rb): return (31 << 26) | (rd << 21) | (ra << 16) | (rb << 11) | (266 << 1)
def mullw(rd, ra, rb): return (31 << 26) | (rd << 21) | (ra << 16) | (rb << 11) | (235 << 1)
def orr(ra, rs, rb): return (31 << 26) | (rs << 21) | (ra << 16) | (rb << 11) | (444 << 1)
def stw(rs, d, ra): return (36 << 26) | (rs << 21) | (ra << 16) | (d & 0xFFFF)
def std(rs, d, ra): return (62 << 26) | (rs << 21) | (ra << 16) | (d & 0xFFFC)
def stdu(rs, d, ra): return (62 << 26) | (rs << 21) | (ra << 16) | (d & 0xFFFC) | 1
def ld(rd, d, ra): return (58 << 26) | (rd << 21) | (ra << 16) | (d & 0xFFFC)
def cmpwi(bf, ra, v): return (11 << 26) | (bf << 23) | (ra << 16) | (v & 0xFFFF)
def bc(bo, bi, frm, to): return (16 << 26) | (bo << 21) | (bi << 16) | ((to - frm) & 0xFFFC)
def bl(frm, to): return (18 << 26) | ((to - frm) & 0x3FFFFFC) | 1
def mfcr(rd): return (31 << 26) | (rd << 21) | (19 << 1)
def rlwinm(ra, rs, sh, mb, me): return (21 << 26) | (rs << 21) | (ra << 16) | (sh << 11) | (mb << 6) | (me << 1)
def mtctr(rs): return (31 << 26) | (rs << 21) | (9 << 16) | (467 << 1)
def bdnz(frm, to): return bc(16, 0, frm, to)


MFLR0, MTLR0, BLR = 0x7C0802A6, 0x7C0803A6, 0x4E800020


def leaf(base):
    words = [cmpwi(0, 3, 0), bc(12, 2, base + 4, base + 24), mullw(5, 3, 4), add(3, 5, 3), stw(3, 0, 6), BLR,
             li(3, 77), cmpwi(7, 4, 5), BLR]
    return words


def caller(base, callee):
    loop = base + 0x28
    return [stdu(1, -0x70, 1), MFLR0, std(0, 0x80, 1), std(29, 0x58, 1), std(30, 0x60, 1), std(31, 0x68, 1),
            orr(31, 3, 3), li(30, 4), orr(29, 6, 6), cmpwi(3, 3, 9),
            orr(3, 31, 31), orr(4, 30, 30), addi(6, 29, 0), bl(loop + 12, callee), add(31, 31, 3), stw(31, 4, 29),
            addi(29, 29, 8), addi(30, 30, -1), cmpwi(0, 30, 0), bc(4, 2, loop + 0x24, loop),
            mfcr(12), rlwinm(12, 12, 16, 28, 31), stw(12, 0x40, 29), bc(12, 14, loop + 0x34, loop + 0x3C), addi(31, 31, 1000),
            orr(3, 31, 31), ld(29, 0x58, 1), ld(30, 0x60, 1), ld(31, 0x68, 1), ld(0, 0x80, 1), MTLR0,
            addi(1, 1, 0x70), BLR]

def counter(base):
    return [li(3, 0), mtctr(4), add(3, 3, 4), addi(4, 4, -1), bdnz(base + 16, base + 8), BLR]


def cr5_caller(base, callee):
    return [MFLR0, std(0, 16, 1), stdu(1, -0x70, 1), bl(base + 12, callee), addi(1, 1, 0x70), ld(0, 16, 1), MTLR0,
            bc(12, 22, base + 28, base + 40), li(3, 1), BLR, li(3, 2), BLR]


PROGRAM = {0x10000: caller(0x10000, 0x10200), 0x10200: leaf(0x10200), 0x10400: counter(0x10400),
           0x10600: cr5_caller(0x10600, 0x10700), 0x10700: [cmpwi(5, 3, 0), BLR]}

DRIVER = r"""
#define PPU_NO_INLINE_LOADS
#include "ppu_recomp.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
extern "C" {
uint8_t* vm_base;
int vm_inline_ok = 1, vm_inline_st_ok = 1;
uint32_t ppu_vm_size = 0x100000u, ppu_hle_inject_base = 0x20000000u, g_null_sweep_hi;
const unsigned char* g_spu_coh_bits;
unsigned char ppu_slow_page[1u << 20];
void ppu_store_recheck(uint64_t) { abort(); }
void ppu_st_thunk(void) { abort(); }
uint8_t vm_read8(uint64_t) { abort(); } uint16_t vm_read16(uint64_t) { abort(); }
uint32_t vm_read32(uint64_t) { abort(); } uint64_t vm_read64(uint64_t) { abort(); }
void vm_write8(uint64_t, uint8_t) { abort(); } void vm_write16(uint64_t, uint16_t) { abort(); }
void vm_write32(uint64_t, uint32_t) { abort(); } void vm_write64(uint64_t, uint64_t) { abort(); }
void ppu_store_block(uint64_t, const void*, uint32_t) { abort(); }
int ppu_stwcx32(ppu_context*, uint64_t, uint32_t, uint32_t) { abort(); }
int ppu_stdcx64(ppu_context*, uint64_t, uint64_t, uint64_t) { abort(); }
void lv2_syscall(ppu_context*) { abort(); }
void ps3_hle_call(unsigned, ppu_context*) { abort(); }
uint64_t ppu_timebase_now(void) { return 0; }
PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
const int32_t* ppu_exec_tab;
uint32_t ppu_exec_lo, ppu_exec_n;
void ps3_indirect_call(ppu_context*) { abort(); }
void ps3_indirect_call_to(ppu_context*, uint32_t) { abort(); }
}
int main(void)
{
    static uint8_t mem[0x100000];
    vm_base = mem;
    unsigned long long h = 1469598103934665603ull;
    for (int r3 = -3; r3 < 12; r3++) for (int r4 = 0; r4 < 5; r4++) {
        memset(mem, 0, sizeof mem);
        ppu_context c; memset(&c, 0, sizeof c);
        c.gpr[1] = 0x80000; c.gpr[3] = (uint64_t)(int64_t)r3; c.gpr[4] = r4; c.gpr[6] = 0x30000;
        for (int i = 13; i < 32; i++) c.gpr[i] = 0x1111111111111111ull * (uint64_t)(i & 15);
        c.cr = 0x13572468u;
        func_00010000(&c);
        ppu_context d; memset(&d, 0, sizeof d);
        d.gpr[3] = 7; d.gpr[4] = (uint64_t)r4 + 2; func_00010400(&d);
        ppu_context e; memset(&e, 0, sizeof e);
        e.gpr[1] = 0x80000; e.gpr[3] = (uint64_t)(int64_t)(r3 - 4); func_00010600(&e);
        const uint64_t keep[] = {c.gpr[1], c.gpr[2], c.gpr[3], c.gpr[13], c.gpr[14], c.gpr[29], c.gpr[30], c.gpr[31],
                                 c.cr & 0x00FFF000u, d.gpr[3], d.ctr, e.gpr[3]};
        for (unsigned i = 0; i < sizeof keep / sizeof *keep; i++) h = (h ^ keep[i]) * 1099511628211ull;
        for (int i = 0x30000; i < 0x31000; i++) h = (h ^ mem[i]) * 1099511628211ull;
    }
    printf("%016llx\n", h);
    return 0;
}
"""


def _cxx():
    cxx = shutil.which(os.environ.get("CXX", "c++")) or shutil.which("clang++")
    if cxx is None or "clang" not in Path(cxx).name and "clang" not in subprocess.run(
            [cxx, "--version"], capture_output=True, text=True).stdout:
        return None
    return cxx


def _emit(plain):
    lifter = L.PPULifter()
    for a, words in PROGRAM.items():
        insns = D.disassemble_bytes(b"".join(struct.pack(">I", w) for w in words), a, True)
        lifter.lift_function(insns, a, a + 4 * len(words))
    saved = L.drop_callee_saves, L._fuse_cr
    if plain:
        L.drop_callee_saves, L._fuse_cr = (lambda f, body, *a: body), (lambda b, *a: b)
    try:
        return lifter.emit_header(), lifter.emit_source()
    finally:
        L.drop_callee_saves, L._fuse_cr = saved


def test_saves_dropped():
    body = _emit(False)[1].split("void func_00010000", 1)[1].split("\n}", 1)[0]
    for off, reg in ((0x58, 29), (0x60, 30), (0x68, 31)):
        assert f"vm_write64(ctx->gpr[1] + 0x{off:X}, ctx->gpr[{reg}]);" not in body, body
        assert f"ctx->gpr[{reg}] = _cs_{reg};" in body, body
    assert "vm_write64(ctx->gpr[1] + 0x80, ctx->gpr[0]);" not in body, body
    assert "uint64_t _cs_lr = ctx->lr;" in body and "ctx->gpr[0] = _cs_lr;" in body, body
    plain = _emit(True)[1]
    assert "vm_write64(ctx->gpr[1] + 0x68, ctx->gpr[31]);" in plain, plain


def test_runs_like_plain():
    cxx = _cxx()
    if cxx is None:
        if os.environ.get("CI"):
            raise SystemExit("FAIL: guaranteed tail calls need clang")
        print("skip test_runs_like_plain: no clang++")
        return
    outs = []
    for plain in (True, False):
        hdr, src = _emit(plain)
        with tempfile.TemporaryDirectory() as td:
            td = Path(td)
            (td / "ppu_recomp.h").write_text(hdr)
            (td / "lifted.cpp").write_text(src)
            (td / "driver.cpp").write_text(DRIVER)
            exe = td / ("t.exe" if os.name == "nt" else "t")
            for opt in ("-O0", "-O2"):
                r = subprocess.run([cxx, "-std=c++20", opt, "-w", "-march=x86-64-v3", "-I" + str(td),
                                    "-I" + str(ROOT / "runtime" / "ppu"), "-o", str(exe),
                                    str(td / "lifted.cpp"), str(td / "driver.cpp")], capture_output=True, text=True)
                assert r.returncode == 0, r.stdout + r.stderr
                out = subprocess.run([str(exe)], capture_output=True, text=True)
                assert out.returncode == 0, out.stdout + out.stderr
                outs.append(out.stdout.strip())
    assert len(set(outs)) == 1, outs


def vx(vd, va, vb, xo): return (4 << 26) | (vd << 21) | (va << 16) | (vb << 11) | xo


VX = {"vxor": 1220, "vor": 1156, "vand": 1028, "vandc": 1092, "vnor": 1284, "vmrghb": 12, "vmrghh": 76,
      "vmrghw": 140, "vmrglb": 268, "vmrglh": 332, "vmrglw": 396}


def vmx_cases():
    cases = [(mn, vx(4, 1, 2, xo)) for mn, xo in VX.items()]
    cases += [(f"vspltb{u}", vx(4, u, 2, 524)) for u in (0, 5, 15)]
    cases += [(f"vsplth{u}", vx(4, u, 2, 588)) for u in (0, 3, 7)]
    cases += [(f"vspltw{u}", vx(4, u, 2, 652)) for u in range(4)]
    cases += [(f"vspltisb{s}", vx(4, s & 31, 0, 780)) for s in (-16, -1, 0, 7, 15)]
    cases += [(f"vsldoi{s}", (4 << 26) | (4 << 21) | (1 << 16) | (2 << 11) | (s << 6) | 44) for s in (0, 1, 7, 15)]
    cases += [("vsel", (4 << 26) | (4 << 21) | (1 << 16) | (2 << 11) | (3 << 6) | 42)]
    cases += [("lvsl", (31 << 26) | (4 << 21) | (5 << 16) | (6 << 11) | (6 << 1)),
              ("lvsr", (31 << 26) | (4 << 21) | (5 << 16) | (6 << 11) | (38 << 1)),
              ("mfvscr", vx(4, 0, 0, 1540))]
    return cases


def vref(mn, a, b, c, ea, vscr):
    if mn in ("vxor", "vor", "vand", "vandc", "vnor"):
        f = {"vxor": lambda x, y: x ^ y, "vor": lambda x, y: x | y, "vand": lambda x, y: x & y,
             "vandc": lambda x, y: x & ~y & 0xFF, "vnor": lambda x, y: ~(x | y) & 0xFF}[mn]
        return bytes(f(x, y) for x, y in zip(a, b))
    if mn.startswith("vmrg"):
        n = {"b": 1, "h": 2, "w": 4}[mn[5]]
        k = 16 // n // 2
        off = 0 if mn[4] == "h" else k
        out = b""
        for i in range(k):
            out += a[(off + i) * n:(off + i + 1) * n] + b[(off + i) * n:(off + i + 1) * n]
        return out
    if mn.startswith("vspltis"):
        return bytes([int(mn[8:]) & 0xFF] * 16)
    if mn.startswith("vsplt"):
        n = {"b": 1, "h": 2, "w": 4}[mn[5]]
        u = int(mn[6:])
        return b[u * n:(u + 1) * n] * (16 // n)
    if mn.startswith("vsldoi"):
        s = int(mn[6:])
        return (a + b)[s:s + 16]
    if mn == "vsel":
        return bytes((x & ~z | y & z) & 0xFF for x, y, z in zip(a, b, c))
    if mn == "lvsl":
        return bytes((ea & 15) + i for i in range(16))
    if mn == "lvsr":
        return bytes(16 - (ea & 15) + i for i in range(16))
    if mn == "mfvscr":
        return bytes(12) + struct.pack(">I", ((vscr >> 16) & 0xFF) << 16 | (vscr & 0xFF))
    raise KeyError(mn)


def test_vmx_whole_register_forms():
    cc = shutil.which(os.environ.get("CC", "cc")) or shutil.which("clang") or shutil.which("gcc")
    if cc is None:
        if os.environ.get("CI"):
            raise SystemExit("FAIL: no C compiler found")
        print("skip test_vmx_whole_register_forms: no C compiler")
        return
    rng = random.Random(9)
    cases = vmx_cases()
    bodies = []
    for mn, word in cases:
        insns = D.disassemble_bytes(struct.pack(">II", word, BLR), 0x1000, True)
        body = "\n".join(x for x in L.PPULifter().lift_function(insns, 0x1000, 0x1008).body_lines if "return" not in x)
        assert "TODO" not in body and "(uint8_t*)&ctx->vr" not in body and "(uint32_t*)&ctx->vr" not in body \
            and "(uint64_t*)&ctx->vr" not in body and "memset(&ctx->vr" not in body, (mn, body)
        bodies.append(body)
    inputs = [(bytes(rng.randrange(256) for _ in range(16)), bytes(rng.randrange(256) for _ in range(16)),
               bytes(rng.randrange(256) for _ in range(16)), rng.randrange(1 << 32), rng.randrange(1 << 32))
              for _ in range(64)]
    arr = lambda v: "{" + ",".join(str(x) for x in v) + "}"
    src = "\n".join([
        "#include <stdint.h>", "#include <string.h>", "#include <stdio.h>", "#include \"ppu_vmx.h\"",
        "typedef struct { uint64_t gpr[32]; uint8_t vr[32][16]; uint32_t vscr; } ctx_t;",
        f"static const unsigned char IN[{len(inputs)}][3][16] = {{" +
        ",".join("{" + ",".join(arr(v) for v in t[:3]) + "}" for t in inputs) + "};",
        f"static const uint32_t EA[{len(inputs)}] = {{" + ",".join(str(t[3]) for t in inputs) + "};",
        f"static const uint32_t VS[{len(inputs)}] = {{" + ",".join(str(t[4]) for t in inputs) + "};",
        "int main(void) { static ctx_t c; ctx_t* ctx = &c;",
        f"  for (int n = 0; n < {len(inputs)}; n++) {{",
    ] + [
        "    for (int i = 0; i < 16; i++) { ctx->vr[1][i] = IN[n][0][15 - i]; ctx->vr[2][i] = IN[n][1][15 - i]; "
        "ctx->vr[3][i] = IN[n][2][15 - i]; } memset(ctx->vr[4], 0xCD, 16); "
        "ctx->gpr[5] = EA[n] / 2; ctx->gpr[6] = EA[n] - EA[n] / 2; ctx->vscr = VS[n];\n    { " + body + " }\n"
        "    for (int i = 0; i < 16; i++) printf(\"%02x\", ctx->vr[4][15 - i]); printf(\"\\n\");"
        for body in bodies
    ] + ["  }", "  return 0; }"])
    with tempfile.TemporaryDirectory() as td:
        cpath = Path(td) / "t.c"
        exe = Path(td) / ("t.exe" if os.name == "nt" else "t")
        cpath.write_text(src)
        r = subprocess.run([cc, "-std=c11", "-O1", "-march=x86-64-v3", "-I" + str(ROOT / "runtime" / "ppu"),
                            "-o", str(exe), str(cpath)], capture_output=True, text=True)
        assert r.returncode == 0, r.stderr
        out = subprocess.run([str(exe)], capture_output=True, text=True).stdout.split()
    k = 0
    for a, b, c, ea, vs in inputs:
        for mn, _ in cases:
            want = vref(mn, a, b, c, ea, vs).hex()
            assert out[k] == want, (mn, a.hex(), b.hex(), out[k], want)
            k += 1
    print(f"  {len(cases)} VMX forms x {len(inputs)} inputs agree with the reference")


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print("ok", name)
