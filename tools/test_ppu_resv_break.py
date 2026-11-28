#!/usr/bin/env python3
"""An SPU-side line write skips the PPU reservation counter bump only when no
PPU lwarx/ldarx ever used that line's slot, and never loses a break.

Lifted lwarx and stwcx. (real lifter output) run against the real
ppu_store_conditional and ppu_resv_break (ppu_resv.h). Checked:
  - a write that only ever touches lines no PPU thread reserved leaves the
    counter untouched;
  - an ABA write (Y, then back to X, each with a notify) after lwarx makes
    stwcx. fail, and a write before lwarx lets it succeed;
  - racing a writer's ABA pair against the first lwarx of a fresh slot, a
    stwcx. never succeeds when the writer had not started before lwarx read.
"""
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


def _load(name, rel):
    spec = importlib.util.spec_from_file_location(name, ROOT / rel)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


D = _load("ppu_disasm", "tools/ppu_disasm.py")
L = _load("ppu_lifter", "tools/ppu_lifter.py")

BLR = 0x4E800020
LWARX = (31 << 26) | (5 << 21) | (0 << 16) | (3 << 11) | (20 << 1)
STWCX = (31 << 26) | (6 << 21) | (0 << 16) | (3 << 11) | (150 << 1) | 1

DRIVER = r"""
#define PPU_NO_INLINE_LOADS
#include "ppu_recomp.h"
#include "ppu_resv.h"
#include "spu_coherency.h"
#include <atomic>
#include <thread>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern "C" {
uint8_t* vm_base;
alignas(64) uint64_t g_ppu_resv[PPU_RESV_MASK + 1];
PPU_THREAD_LOCAL ppu_context* g_active_ctx;
PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
int vm_inline_ok = 1, vm_inline_st_ok = 1;
uint32_t ppu_vm_size, ppu_hle_inject_base = 0x20000000u, g_null_sweep_hi;
const int32_t* ppu_exec_tab;
uint32_t ppu_exec_lo, ppu_exec_n;
void spu_ch_wake(spu_context* c) { (void)c; }
int32_t ppu_thread_priority(uint64_t tid) { (void)tid; return 0; }
void ppu_resv_break_line(uint32_t ea) { ppu_resv_break(ea); }
int ppu_stwcx32(ppu_context* c, uint64_t ea, uint32_t e, uint32_t v) { return ppu_store_conditional<uint32_t>(c, (uint32_t)ea, e, v); }
int ppu_stdcx64(ppu_context* c, uint64_t ea, uint64_t e, uint64_t v) { return ppu_store_conditional<uint64_t>(c, (uint32_t)ea, e, v); }
void ppu_store_recheck(uint64_t) { abort(); }
void ppu_store_block(uint64_t, const void*, uint32_t) { abort(); }
uint8_t vm_read8(uint64_t) { abort(); } uint16_t vm_read16(uint64_t) { abort(); }
uint32_t vm_read32(uint64_t) { abort(); } uint64_t vm_read64(uint64_t) { abort(); }
void vm_write8(uint64_t, uint8_t) { abort(); } void vm_write16(uint64_t, uint16_t) { abort(); }
void vm_write32(uint64_t, uint32_t) { abort(); } void vm_write64(uint64_t, uint64_t) { abort(); }
void ppu_st_thunk(void) { abort(); }
void lv2_syscall(ppu_context*) { abort(); }
void ps3_hle_call(unsigned, ppu_context*) { abort(); }
uint64_t ppu_timebase_now(void) { return 0; }
void ps3_indirect_call(ppu_context*) { abort(); }
void ps3_indirect_call_to(ppu_context*, uint32_t) { abort(); }
}
static int g_fail;
static void expect(int ok, const char* what) { if (!ok) { printf("FAIL %s\n", what); g_fail++; } }
static void put(uint32_t ea, uint32_t v)
{
    ppu_context* const self = g_active_ctx;
    g_active_ctx = 0;
    v = __builtin_bswap32(v);
    spu_lockline_lock(ea);
    memcpy(vm_base + ea, &v, 4);
    spu_coh_notify_write(ea);
    spu_lockline_unlock(ea);
    g_active_ctx = self;
}
static uint32_t lwarx(ppu_context* c, uint32_t ea) { c->gpr[3] = ea; func_00010000(c); return (uint32_t)c->gpr[5]; }
static int stwcx(ppu_context* c, uint32_t ea, uint32_t v) { c->gpr[3] = ea; c->gpr[6] = v; func_00010100(c); uint32_t w; memcpy(&w, vm_base + ea, 4); return __builtin_bswap32(w) == v; }

int main(void)
{
    vm_base = (uint8_t*)calloc(1, 0x4000000);
    ppu_context c; memset(&c, 0, sizeof c);
    g_active_ctx = &c;
    put(0x10000, 5);
    expect(g_ppu_resv[(0x10000 >> 7) & PPU_RESV_MASK] == 0, "a write to a never-reserved line leaves the counter alone");
    expect(lwarx(&c, 0x10080) == 0, "lwarx reads the word");
    put(0x10080, 7);
    put(0x10080, 0);
    expect(!stwcx(&c, 0x10080, 9), "an ABA write after lwarx fails stwcx.");
    put(0x10080, 3);
    expect(lwarx(&c, 0x10080) == 3 && stwcx(&c, 0x10080, 4), "a write before lwarx lets stwcx. succeed");

    const int rounds = 200000;
    std::atomic<int> go{0}, writer_done{0};
    std::atomic<uint32_t> writes{0};
    uint32_t line = 0;
    std::thread writer([&] {
        for (int k = 1; k <= rounds; k++) {
            while (go.load(std::memory_order_acquire) != k) {}
            for (volatile int d = (k * 11) % 64; d > 0; d--) {}
            writes.fetch_add(1, std::memory_order_seq_cst);
            put(line, 0xBEEF0000u + (uint32_t)k);
            put(line, 0);
            writer_done.store(k, std::memory_order_release);
        }
    });
    long checked = 0, violations = 0, succeeded = 0;
    for (int k = 1; k <= rounds; k++) {
        line = 0x100000u + (uint32_t)k * 128u;
        writes.store(0, std::memory_order_seq_cst);
        go.store(k, std::memory_order_release);
        for (volatile int d = (k * 7) % 64; d > 0; d--) {}
        const uint32_t v = lwarx(&c, line);
        const uint32_t w0 = writes.load(std::memory_order_seq_cst);
        while (writer_done.load(std::memory_order_acquire) != k) {}
        const int ok = stwcx(&c, line, 1);
        succeeded += ok;
        if (w0 == 0) {
            checked++;
            if (v != 0 || ok) violations++;
        }
    }
    writer.join();
    expect(violations == 0, "race: no stwcx. succeeds across an unseen ABA write");
    printf("%d rounds: checked %ld, succeeded %ld, violations %ld; %d failures\n", rounds, checked, succeeded, violations, g_fail);
    return g_fail != 0;
}
"""


def test_resv_break():
    cc = shutil.which(os.environ.get("CC", "cc")) or shutil.which("clang")
    cxx = shutil.which(os.environ.get("CXX", "c++")) or shutil.which("clang++")
    if cc is None or cxx is None:
        if os.environ.get("CI"):
            raise SystemExit("FAIL: no C/C++ compiler found")
        print("skip: no compiler")
        return
    lifter = L.PPULifter()
    for a, words in ((0x10000, [LWARX, BLR]), (0x10100, [STWCX, BLR])):
        insns = D.disassemble_bytes(b"".join(struct.pack(">I", w) for w in words), a, True)
        lifter.lift_function(insns, a, a + 4 * len(words))
    src = lifter.emit_source()
    assert "ppu_resv_take((uint32_t)ea)" in src, src
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        (td / "ppu_recomp.h").write_text(lifter.emit_header())
        (td / "lifted.cpp").write_text(src)
        (td / "driver.cpp").write_text(DRIVER)
        inc = ["-I" + str(td), "-I" + str(ROOT / "include"), "-I" + str(ROOT / "runtime" / "ppu"), "-I" + str(ROOT / "runtime" / "memory"),
               "-I" + str(ROOT / "runtime" / "spu"), "-I" + str(ROOT / "runtime" / "platform")]
        for opt in ("-O0", "-O2"):
            objs = []
            srcs = [(ROOT / "runtime/spu/spu_coherency.c", cc, "-std=gnu17"), (ROOT / "runtime/platform/ps3_sync.c", cc, "-std=gnu17"),
                    (td / "lifted.cpp", cxx, "-std=c++20"), (td / "driver.cpp", cxx, "-std=c++20")]
            if os.name != "nt":
                srcs.append((ROOT / "runtime/platform/win32_compat.c", cc, "-std=gnu17"))
            for s, comp, std in srcs:
                obj = td / (s.stem + opt + ".o")
                r = subprocess.run([comp, std, opt, "-w", "-march=native", "-fno-strict-aliasing", "-D_CRT_SECURE_NO_WARNINGS", *inc, "-c", str(s),
                                    "-o", str(obj)], capture_output=True, text=True)
                assert r.returncode == 0, r.stdout + r.stderr
                objs.append(str(obj))
            exe = td / ("t" + opt + (".exe" if os.name == "nt" else ""))
            libs = ["-static", "-lsynchronization"] if os.name == "nt" else ["-lpthread", "-lm"]
            r = subprocess.run([cxx, "-o", str(exe), *objs, *libs], capture_output=True, text=True)
            assert r.returncode == 0, r.stdout + r.stderr
            out = subprocess.run([str(exe)], capture_output=True, text=True)
            print("  " + opt + ": " + out.stdout.strip().replace("\n", "\n  "))
            assert out.returncode == 0, out.stdout + out.stderr


if __name__ == "__main__":
    test_resv_break()
    print("ok test_resv_break")
