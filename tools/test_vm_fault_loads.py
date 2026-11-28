#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
import os
import platform
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
spec = importlib.util.spec_from_file_location("ppu_lifter", ROOT / "tools" / "ppu_lifter.py")
L = importlib.util.module_from_spec(spec)
spec.loader.exec_module(L)

DRIVER = r"""
#include "ppu_recomp.h"
#include "vm_fault_x86.h"
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <sys/mman.h>
#include <ucontext.h>
#endif
extern "C" uint8_t* vm_base;
uint8_t* vm_base;
static uint8_t* host;
static uint8_t shadow[0x1000];
static unsigned reads;
static const uint32_t PROT = 0x11000, SPECIAL = PROT + 0x24;

static uint8_t byte_at(uint32_t g) { return g - PROT < 0x1000 ? shadow[g - PROT] : vm_base[g]; }
static void reader(uint32_t g, int w, uint8_t* out)
{
    if (w == 4 && g == SPECIAL) {
        const uint32_t v = 0xA0000000u + ++reads;
        out[0] = (uint8_t)(v >> 24); out[1] = (uint8_t)(v >> 16); out[2] = (uint8_t)(v >> 8); out[3] = (uint8_t)v;
        return;
    }
    for (int i = 0; i < w; i++) out[i] = byte_at(g + (uint32_t)i);
}
static int in_prot(uintptr_t a) { return a - (uintptr_t)(vm_base + PROT) < 0x1000; }
#ifdef _WIN32
static LONG WINAPI veh(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || !in_prot(ep->ExceptionRecord->ExceptionInformation[1]))
        return EXCEPTION_CONTINUE_SEARCH;
    return vm_emulate_load(&ep->ContextRecord->Rax, &ep->ContextRecord->Rip, vm_base, reader)
        ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}
static void arm(void)
{
    host = (uint8_t*)VirtualAlloc(NULL, 0x3000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    DWORD old;
    VirtualProtect(host + 0x1000, 0x1000, PAGE_NOACCESS, &old);
    AddVectoredExceptionHandler(1, veh);
}
#else
static const int greg[16] = { REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP, REG_RSI, REG_RDI,
                              REG_R8, REG_R9, REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15 };
static void segv(int, siginfo_t* si, void* p)
{
    ucontext_t* uc = (ucontext_t*)p;
    uint64_t r[16], rip = (uint64_t)uc->uc_mcontext.gregs[REG_RIP];
    for (int i = 0; i < 16; i++) r[i] = (uint64_t)uc->uc_mcontext.gregs[greg[i]];
    if (!in_prot((uintptr_t)si->si_addr) || !vm_emulate_load(r, &rip, vm_base, reader)) abort();
    for (int i = 0; i < 16; i++) uc->uc_mcontext.gregs[greg[i]] = (greg_t)r[i];
    uc->uc_mcontext.gregs[REG_RIP] = (greg_t)rip;
}
static void arm(void)
{
    host = (uint8_t*)mmap(NULL, 0x3000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    mprotect(host + 0x1000, 0x1000, PROT_NONE);
    struct sigaction sa = {};
    sa.sa_sigaction = segv;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
}
#endif
static int fails;
static void expect(const char* what, uint32_t g, uint64_t got, uint64_t want)
{
    if (got != want) {
        if (fails++ < 20) printf("FAIL %s at 0x%X: %llX want %llX\n", what, g, (unsigned long long)got, (unsigned long long)want);
    }
}
static uint64_t le(uint32_t g, int w) { uint64_t x = 0; for (int i = 0; i < w; i++) x |= (uint64_t)byte_at(g + i) << (8 * i); return x; }
static uint64_t be(uint32_t g, int w) { uint64_t x = 0; for (int i = 0; i < w; i++) x = x << 8 | byte_at(g + i); return x; }
__attribute__((noinline)) static uint64_t many(uint32_t g)
{
    const uint8_t* p = vm_base + g;
    uint64_t a = vm_ld32(p), b = vm_ld64(p + 8), c = vm_ld16(p + 2), d = vm_ld8(p + 5), e = vm_ldr32(p + 12),
             f = vm_ldr64(p + 16), h = vm_ldr16(p + 6), i = vm_ld32(p + 28), j = vm_ld64(p + 40), k = vm_ld8(p + 63),
             l = vm_ld32(p + 100), m = vm_ld64(p + 200), n = vm_ld16(p + 301), o = vm_ldr32(p + 403);
    return a * 3 ^ b * 5 ^ c * 7 ^ d * 11 ^ e * 13 ^ f * 17 ^ h * 19 ^ i * 23 ^ j * 29 ^ k * 31 ^ l * 37 ^ m * 41 ^ n * 43 ^ o * 47;
}
static uint64_t many_ref(uint32_t g)
{
    return be(g, 4) * 3 ^ be(g + 8, 8) * 5 ^ be(g + 2, 2) * 7 ^ be(g + 5, 1) * 11 ^ le(g + 12, 4) * 13 ^ le(g + 16, 8) * 17 ^
           le(g + 6, 2) * 19 ^ be(g + 28, 4) * 23 ^ be(g + 40, 8) * 29 ^ be(g + 63, 1) * 31 ^ be(g + 100, 4) * 37 ^
           be(g + 200, 8) * 41 ^ be(g + 301, 2) * 43 ^ le(g + 403, 4) * 47;
}
__attribute__((noinline)) static uint32_t indexed(uint32_t base, uint64_t i) { return vm_ld32(vm_base + base + i * 4); }
int main(void)
{
    arm();
    vm_base = host - 0x10000;
    for (int i = 0; i < 0x1000; i++) shadow[i] = (uint8_t)(i * 37 + 11);
    for (int i = 0; i < 0x1000; i++) host[i] = (uint8_t)(i * 13 + 5), host[0x2000 + i] = (uint8_t)(i * 29 + 3);
    static const uint32_t offs[] = { 0x10FF0, 0x10FF9, 0x10FFC, 0x10FFE, 0x10FFF, 0x11000, 0x11001, 0x11003, 0x11010,
                                     0x11025, 0x117F7, 0x11FF8, 0x11FF9, 0x11FFC, 0x11FFE, 0x11FFF };
    for (uint32_t g : offs) {
        const uint8_t* p = vm_base + g;
        expect("vm_ld8", g, vm_ld8(p), be(g, 1));
        expect("vm_ldr16", g, vm_ldr16(p), le(g, 2));
        expect("vm_ld16", g, vm_ld16(p), be(g, 2));
        expect("vm_ldr32", g, vm_ldr32(p), le(g, 4));
        expect("vm_ld32", g, vm_ld32(p), be(g, 4));
        expect("vm_ldr64", g, vm_ldr64(p), le(g, 8));
        expect("vm_ld64", g, vm_ld64(p), be(g, 8));
    }
    for (uint32_t g = 0x11000; g < 0x11E00; g += 0x1F3) expect("register mix", g, many(g), many_ref(g));
    for (uint64_t i = 0; i < 0x400; i += 37) expect("indexed", 0x11000 + (uint32_t)i * 4, indexed(0x11000, i), be(0x11000 + (uint32_t)i * 4, 4));
    const unsigned before = reads;
    expect("side-effect read 1", SPECIAL, vm_ld32(vm_base + SPECIAL), 0xA0000001u + before);
    expect("side-effect read 2", SPECIAL, vm_ld32(vm_base + SPECIAL), 0xA0000002u + before);
    expect("reads per load", SPECIAL, reads - before, 2);
    expect("narrow read has no side effect", SPECIAL, vm_ld16(vm_base + SPECIAL), be(SPECIAL, 2));
    expect("reads after narrow", SPECIAL, reads - before, 2);
    printf("%d failures\n", fails);
    return fails != 0;
}
"""


def test_fault_loads():
    if platform.machine().lower() not in ("amd64", "x86_64"):
        print("skip: x86-64 only")
        return
    cxx = shutil.which(os.environ.get("CXX", "c++")) or shutil.which("clang++")
    if cxx is None:
        if os.environ.get("CI"):
            raise SystemExit("FAIL: no C++ compiler found")
        print("skip: no C++ compiler")
        return
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        (td / "ppu_recomp.h").write_text(L.PPULifter().emit_header())
        (td / "driver.cpp").write_text(DRIVER)
        exe = td / ("t.exe" if os.name == "nt" else "t")
        static = ["-static"] if os.name == "nt" else []
        for flags in (["-O0", "-march=x86-64-v3"], ["-O2", "-march=x86-64-v3"], ["-O2", "-march=x86-64"]):
            r = subprocess.run([cxx, "-std=c++20", "-w", *flags, "-I" + str(td), "-I" + str(ROOT / "runtime" / "memory"),
                                "-o", str(exe), str(td / "driver.cpp"), *static], capture_output=True, text=True)
            assert r.returncode == 0, r.stdout + r.stderr
            out = subprocess.run([str(exe)], capture_output=True, text=True)
            assert out.returncode == 0, f"{flags}: {out.stdout}{out.stderr}"


if __name__ == "__main__":
    test_fault_loads()
    print("ok test_fault_loads")
