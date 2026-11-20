#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
import os
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

BLR, BCTR, BCTRL = 0x4E800020, 0x4E800420, 0x4E800421


def addi(rd, ra, v):
    return (14 << 26) | (rd << 21) | (ra << 16) | (v & 0xFFFF)


def b(frm, to, link=False):
    return (18 << 26) | ((to - frm) & 0x3FFFFFC) | link


def mtctr(rs):
    return (31 << 26) | (rs << 21) | (9 << 16) | (467 << 1)


def cmpwi(ra, v):
    return (11 << 26) | (ra << 16) | (v & 0xFFFF)


def bne(frm, to):
    return (16 << 26) | (4 << 21) | (2 << 16) | ((to - frm) & 0xFFFC)


PROGRAM = {
    0x1000: [b(0x1000, 0x1100, True), addi(3, 3, 1), BLR],
    0x1100: [addi(3, 3, 2), b(0x1104, 0x1200)],
    0x1200: [addi(3, 3, 10), mtctr(5), BCTRL, addi(3, 3, 1000), BLR],
    0x1300: [addi(3, 3, 100), BLR],
    0x1400: [addi(3, 3, 5), BLR],
    0x1500: [addi(3, 3, 3), BLR],
    0x1600: [b(0x1600, 0x1400, True), addi(3, 3, 9), BLR],
    0x1700: [addi(4, 4, -1), cmpwi(4, 0), bne(0x1708, 0x1800), BLR],
    0x1800: [addi(3, 3, 1), b(0x1804, 0x1700)],
    0x1900: [mtctr(5), BCTR, BLR],
}
WRAPPED = 0x1400

DRIVER = r"""
#define PPU_NO_INLINE_LOADS
#include "ppu_recomp.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
extern "C" {
uint8_t* vm_base;
int vm_inline_ok = 1, vm_inline_st_ok = 1;
uint32_t ppu_vm_size, ppu_hle_inject_base = 0x20000000u, g_null_sweep_hi;
const unsigned char* g_spu_coh_bits;
unsigned char ppu_slow_page[1u << 20];
void ppu_store_recheck(uint64_t) { abort(); }
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
static int fallbacks;
void ps3_indirect_call(ppu_context* ctx)
{
    fallbacks++;
    for (uint64_t i = 0; i < function_table_count; i++)
        if (function_table[i].addr == (uint32_t)ctx->ctr) {
            function_table[i].func(ctx);
            while (g_trampoline_fn) { void (*t)(void*) = g_trampoline_fn; g_trampoline_fn = 0; t(ctx); }
        }
}
void ps3_indirect_call_to(ppu_context* ctx, uint32_t t) { ctx->ctr = t; ps3_indirect_call(ctx); }
}
void func_00001400(ppu_context* ctx) { ctx->gpr[3] *= 7; g_trampoline_fn = (void (*)(void*))func_00001500; }
static int fails;
static void expect(const char* what, uint64_t got, uint64_t want)
{
    if (got != want) { printf("FAIL %s: %llu want %llu\n", what, (unsigned long long)got, (unsigned long long)want); fails++; }
}
static ppu_context run(void (*f)(ppu_context*), uint64_t r3, uint64_t r4, uint64_t r5)
{
    ppu_context c; memset(&c, 0, sizeof c);
    c.gpr[3] = r3; c.gpr[4] = r4; c.gpr[5] = r5;
    f(&c);
    while (g_trampoline_fn) { void (*t)(void*) = g_trampoline_fn; g_trampoline_fn = 0; t(&c); }
    return c;
}
int main(void)
{
    uint32_t lo = ~0u, hi = 0;
    for (uint64_t i = 0; i < function_table_lifted; i++) {
        if (function_table[i].addr < lo) lo = (uint32_t)function_table[i].addr;
        if (function_table[i].addr > hi) hi = (uint32_t)function_table[i].addr;
    }
    static int32_t tab[4096];
    for (uint64_t i = 0; i < function_table_lifted; i++)
        tab[(function_table[i].addr - lo) >> 2] = (int32_t)((const char*)function_table[i].func - (const char*)ps3_indirect_call);
    ppu_exec_tab = tab; ppu_exec_lo = lo; ppu_exec_n = ((hi - lo) >> 2) + 1;
    expect("wrapped entries follow the lifted ones", function_table_count - function_table_lifted, 1);
    expect("wrapped entry", function_table[function_table_count - 1].addr, 0x1400);
    expect("direct call, tail call, exec-table call", run(func_00001000, 0, 0, 0x1300).gpr[3], 1113);
    expect("exec-table hit takes no fallback", fallbacks, 0);
    expect("indirect call to a wrapped function drains", run(func_00001000, 0, 0, 0x1400).gpr[3], 1088);
    expect("wrapped target falls back", fallbacks, 1);
    expect("direct call to a wrapped function drains", run(func_00001600, 1, 0, 0).gpr[3], 19);
    expect("tail-call loop", run(func_00001700, 0, 3000000, 0).gpr[3], 2999999);
    expect("indirect tail jump", run(func_00001900, 0, 0, 0x1300).gpr[3], 100);
    expect("unaligned target", run(func_00001900, 0, 0, 0x1302).gpr[3], 0);
    expect("out-of-range target", run(func_00001900, 0, 0, 0x9000).gpr[3], 0);
    expect("misses fall back", fallbacks, 3);
    printf("%d failures\n", fails);
    return fails != 0;
}
"""


def lifted():
    lifter = L.PPULifter(wrap=frozenset({WRAPPED}))
    for a, words in PROGRAM.items():
        insns = D.disassemble_bytes(b"".join(struct.pack(">I", w) for w in words), a, True)
        lifter.lift_function(insns, a, a + 4 * len(words))
    return lifter, lifter.emit_source()


def test_emission():
    lifter, src = lifted()
    assert "g_trampoline_fn = " not in src.split("void func_", 1)[1], "a lifted body sets the trampoline"
    body = lambda a: src.split(f"void func_{a:08X}", 1)[1].split("\n}", 1)[0]
    assert "PPU_TAIL return func_00001200(ctx);" in body(0x1100)
    assert "DRAIN_TRAMPOLINE" not in body(0x1000)
    assert "func_00001400(ctx); DRAIN_TRAMPOLINE(ctx);" in body(0x1600)
    assert "ppu_call_ctr(ctx);" in body(0x1200) and L._JUMP_CTR in body(0x1900)


def test_runs():
    cxx = shutil.which(os.environ.get("CXX", "c++")) or shutil.which("clang++")
    if cxx is None or "clang" not in Path(cxx).name and "clang" not in subprocess.run(
            [cxx, "--version"], capture_output=True, text=True).stdout:
        if os.environ.get("CI"):
            raise SystemExit("FAIL: guaranteed tail calls need clang")
        print("skip test_runs: no clang++")
        return
    lifter, src = lifted()
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        (td / "ppu_recomp.h").write_text(lifter.emit_header())
        (td / "lifted.cpp").write_text(src)
        (td / "driver.cpp").write_text(DRIVER)
        exe = td / ("t.exe" if os.name == "nt" else "t")
        for opt in ("-O0", "-O2"):
            r = subprocess.run([cxx, "-std=c++20", opt, "-w", "-I" + str(td), "-I" + str(ROOT / "runtime" / "ppu"),
                                "-o", str(exe), str(td / "lifted.cpp"), str(td / "driver.cpp")],
                               capture_output=True, text=True)
            assert r.returncode == 0, r.stdout + r.stderr
            out = subprocess.run([str(exe)], capture_output=True, text=True)
            assert out.returncode == 0, f"{opt}: {out.stdout}{out.stderr}"


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print("ok", name)
