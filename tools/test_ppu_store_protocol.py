#!/usr/bin/env python3
"""The page-map store fast path never loses a lost-reservation event.

A PPU thread stores to a line through the lifter's inline store (store, then
recheck the page byte) while an SPU thread reserves that line for the first
time through the real spu_coh_reserve (mark the page, set the line bit, flush
every core's store buffer, then copy the line). Raced on a fresh page each
round: the SPU's copy must hold the store, or the store must have ended the
reservation with SPU_EVENT_LR. Never neither.
"""
from __future__ import annotations

import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("ppu_lifter", ROOT / "tools" / "ppu_lifter.py")
L = importlib.util.module_from_spec(spec)
spec.loader.exec_module(L)

HELPER = r"""
#include "spu_coherency.h"
#include "spu_coh_bitmap.h"
#include <stdlib.h>
#include <string.h>
extern uint8_t* vm_base;
void spu_ch_wake(spu_context* ctx) { (void)ctx; }
int32_t ppu_thread_priority(uint64_t tid) { (void)tid; return 0; }
void* coh_ctx(void) { return calloc(1, sizeof(spu_context)); }
void coh_reserve(void* p, uint32_t ea, uint8_t* copy)
{
    spu_context* c = (spu_context*)p;
    spu_lockline_lock(ea);
    spu_coh_reserve(c, ea);
    memcpy(copy, vm_base + (ea & ~127u), 128);
    c->resv_ea = ea & ~127u;
    c->resv_valid = 1;
    spu_lockline_unlock(ea);
}
int coh_lost(void* p)
{
    spu_context* c = (spu_context*)p;
    const uint32_t ea = c->resv_ea;
    spu_lockline_lock(ea);
    const int lost = !c->resv_valid && (c->event_status & SPU_EVENT_LR);
    c->resv_valid = 0;
    SPU_EVENT_CLEAR(c, SPU_EVENT_LR);
    spu_lockline_unlock(ea);
    return lost;
}
static void notify(uint32_t a) { spu_lockline_lock(a); spu_coh_notify_write(a); spu_lockline_unlock(a); }
void ppu_store_recheck(uint64_t ea) { if (spu_coh_line_reserved((uint32_t)ea)) notify((uint32_t)ea); }
void vm_write32(uint64_t ea, uint32_t v)
{
    const uint32_t a = (uint32_t)ea;
    v = __builtin_bswap32(v);
    if (spu_coh_line_reserved(a)) { spu_lockline_lock(a); memcpy(vm_base + a, &v, 4); spu_coh_notify_write(a); spu_lockline_unlock(a); return; }
    memcpy(vm_base + a, &v, 4);
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    if (spu_coh_line_reserved(a)) notify(a);
}
"""

DRIVER = r"""
#define PPU_NO_INLINE_LOADS
#include "ppu_recomp.h"
#include <atomic>
#include <stdio.h>
#include <stdlib.h>
#include <thread>
extern "C" {
uint8_t* vm_base;
void* coh_ctx(void);
void coh_reserve(void*, uint32_t, uint8_t*);
int coh_lost(void*);
uint8_t vm_read8(uint64_t) { abort(); } uint16_t vm_read16(uint64_t) { abort(); }
uint32_t vm_read32(uint64_t) { abort(); } uint64_t vm_read64(uint64_t) { abort(); }
void vm_write8(uint64_t, uint8_t) { abort(); } void vm_write16(uint64_t, uint16_t) { abort(); }
void vm_write64(uint64_t, uint64_t) { abort(); }
void ppu_store_block(uint64_t, const void*, uint32_t) { abort(); }
}
static std::atomic<int> round_ready, ppu_done, spu_done;
int main(void)
{
    const int rounds = 200000;
    vm_base = (uint8_t*)calloc(1, 0x100000u + (size_t)(rounds / 32 + 1) * 0x1000u);
    void* ctx = coh_ctx();
    uint8_t copy[128];
    uint32_t line = 0;
    std::thread ppu([&] {
        for (int k = 0; k < rounds; k++) {
            while (round_ready.load(std::memory_order_acquire) != k + 1) {}
            for (volatile int d = (k * 7) % 96; d > 0; d--) {}
            vm_st32(vm_base, line + 4, 0x5EED0000u + (uint32_t)k);
            ppu_done.store(k + 1, std::memory_order_release);
        }
    });
    std::thread spu([&] {
        for (int k = 0; k < rounds; k++) {
            while (round_ready.load(std::memory_order_acquire) != k + 1) {}
            coh_reserve(ctx, line, copy);
            spu_done.store(k + 1, std::memory_order_release);
        }
    });
    int missed = 0, seen = 0, lost = 0;
    for (int k = 0; k < rounds; k++) {
        line = 0x100000u + (uint32_t)(k / 32) * 0x1000u + 0x80u * (uint32_t)(k % 32);
        ppu_slow_page[line >> 12] = 0;
        round_ready.store(k + 1, std::memory_order_release);
        while (ppu_done.load(std::memory_order_acquire) != k + 1 || spu_done.load(std::memory_order_acquire) != k + 1) {}
        const uint32_t v = __builtin_bswap32(*(uint32_t*)(copy + 4));
        const int l = coh_lost(ctx);
        if (v == 0x5EED0000u + (uint32_t)k) seen++; else if (l) lost++; else missed++;
    }
    ppu.join();
    spu.join();
    printf("%d rounds: store in copy %d, reservation lost %d, missed %d\n", rounds, seen, lost, missed);
    return missed != 0;
}
"""


def test_store_protocol():
    cc = shutil.which(os.environ.get("CC", "cc")) or shutil.which("clang")
    cxx = shutil.which(os.environ.get("CXX", "c++")) or shutil.which("clang++")
    if cc is None or cxx is None:
        if os.environ.get("CI"):
            raise SystemExit("FAIL: no C/C++ compiler found")
        print("skip: no compiler")
        return
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        (td / "ppu_recomp.h").write_text(L.PPULifter().emit_header())
        (td / "helper.c").write_text(HELPER)
        (td / "driver.cpp").write_text(DRIVER)
        inc = ["-I" + str(ROOT / "include"), "-I" + str(ROOT / "runtime" / "spu"), "-I" + str(ROOT / "runtime" / "platform")]
        objs = []
        for src, comp, std in ((td / "helper.c", cc, "-std=gnu17"), (ROOT / "runtime/spu/spu_coherency.c", cc, "-std=gnu17"),
                               (ROOT / "runtime/platform/ps3_sync.c", cc, "-std=gnu17"), (td / "driver.cpp", cxx, "-std=c++20")):
            obj = td / (src.stem + ".o")
            r = subprocess.run([comp, std, "-O2", "-w", "-D_CRT_SECURE_NO_WARNINGS", "-I" + str(td), *inc, "-c", str(src),
                                "-o", str(obj)], capture_output=True, text=True)
            assert r.returncode == 0, r.stdout + r.stderr
            objs.append(str(obj))
        if os.name != "nt" and (ROOT / "runtime/platform/win32_compat.c").exists():
            obj = td / "win32_compat.o"
            r = subprocess.run([cc, "-std=gnu17", "-O2", "-w", *inc, "-c", str(ROOT / "runtime/platform/win32_compat.c"), "-o", str(obj)],
                               capture_output=True, text=True)
            assert r.returncode == 0, r.stdout + r.stderr
            objs.append(str(obj))
        exe = td / ("t.exe" if os.name == "nt" else "t")
        libs = ["-lsynchronization"] if os.name == "nt" else ["-lpthread", "-lm"]
        r = subprocess.run([cxx, "-o", str(exe), *objs, *libs], capture_output=True, text=True)
        assert r.returncode == 0, r.stdout + r.stderr
        out = subprocess.run([str(exe)], capture_output=True, text=True)
        print("  " + out.stdout.strip())
        assert out.returncode == 0, out.stdout + out.stderr


if __name__ == "__main__":
    test_store_protocol()
    print("ok test_store_protocol")
