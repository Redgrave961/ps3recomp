#!/usr/bin/env python3
"""ppu_st_thunk stores inline on pages that are slow only for SPU coherency
(another line on the page is reserved), write tracking or the RSX watch.

Lifted-style stores run through the real thunk, the real spu_coherency.c and
vm_track.c, against a ppu_store_slow that mirrors the runtime's coherent store.
Checked:
  - a store to an unreserved line of a coherency page lands without calling
    the runtime; a store to a reserved line takes the locked runtime path and
    ends the reservation; a page marked special always takes the runtime path;
  - a store to an armed tracked page lands inline and the re-check reports the
    page on the next drain;
  - racing an SPU's first GETLLAR of a line on a page that is already slow for
    another reserved line, the store is in the SPU's copy or ends its
    reservation, never neither.
"""
from __future__ import annotations

import importlib.util
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("ppu_lifter", ROOT / "tools" / "ppu_lifter.py")
L = importlib.util.module_from_spec(spec)
spec.loader.exec_module(L)

LIFTED = r"""
extern "C" {
void t_store(ppu_context* __restrict ctx)
{
    PPU_MM_SNAP
    vm_write32(ctx->gpr[3], ctx->gpr[4]);
}
void t_vec(ppu_context* __restrict ctx)
{
    PPU_MM_SNAP
    vm_write128(ctx->gpr[3], _mm_set_epi64x((long long)ctx->gpr[5], (long long)ctx->gpr[4]));
}
}
"""

DRIVER = r"""
#include "ppu_recomp.h"
#include "ppu_store_thunk.h"
#include "spu_coherency.h"
#include "vm_track.h"
#include <atomic>
#include <thread>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern "C" {
void t_store(ppu_context*);
void t_vec(ppu_context*);
uint8_t* vm_base;
void spu_ch_wake(spu_context* c) { (void)c; }
int32_t ppu_thread_priority(uint64_t tid) { (void)tid; return 0; }
}
static std::atomic<long> g_full, g_recheck;
static void notify(uint32_t a) { spu_lockline_lock(a); spu_coh_notify_write(a); spu_lockline_unlock(a); }
extern "C" __attribute__((used)) void ppu_store_slow(uint64_t w, uint64_t lo, uint64_t hi, void* ra)
{
    (void)ra;
    const uint32_t a = (uint32_t)w, op = (uint32_t)(w >> 32);
    if (!op) {
        g_recheck.fetch_add(1);
        if (spu_coh_line_reserved(a)) notify(a);
        vm_track_write(a, 1);
        return;
    }
    g_full.fetch_add(1);
    const uint64_t d[2] = {lo, hi};
    const uint32_t n = (op & 0xF0u) == 0x40u ? op & 15u : op;
    if (spu_coh_line_reserved(a)) { spu_lockline_lock(a); memcpy(vm_base + a, d, n); spu_coh_notify_write(a); spu_lockline_unlock(a); }
    else { memcpy(vm_base + a, d, n); std::atomic_thread_fence(std::memory_order_seq_cst); if (spu_coh_line_reserved(a)) notify(a); }
    vm_track_write(a, n);
}
static int g_fail;
static void expect(int ok, const char* what) { if (!ok) { printf("FAIL %s\n", what); g_fail++; } }
static void st(uint32_t ea, uint32_t v) { ppu_context c; memset(&c, 0, sizeof c); c.gpr[3] = ea; c.gpr[4] = v; t_store(&c); }
static uint32_t rd(uint32_t ea) { uint32_t v; memcpy(&v, vm_base + ea, 4); return __builtin_bswap32(v); }
static int g_reported;
static void seen(void* user, uint32_t page) { (void)user; if (page == 0x42) g_reported++; }

static void reserve(spu_context* c, uint32_t ea, uint8_t* copy)
{
    spu_lockline_lock(ea);
    spu_coh_reserve(c, ea);
    memcpy(copy, vm_base + (ea & ~127u), 128);
    c->resv = (ea & ~127u) | 1u;
    spu_lockline_unlock(ea);
}
static int lost(spu_context* c)
{
    const uint32_t ea = c->resv & ~127u;
    spu_lockline_lock(ea);
    const int l = !(c->resv & 1u) && (c->event_status & SPU_EVENT_LR);
    c->resv = 0;
    SPU_EVENT_CLEAR(c, SPU_EVENT_LR);
    spu_lockline_unlock(ea);
    return l;
}

static std::atomic<int> round_ready, ppu_done, spu_done;

int main(void)
{
    vm_base = (uint8_t*)calloc(1, 0x4000000);
    uint8_t copy[128];
    spu_context* holder = (spu_context*)calloc(1, sizeof(spu_context));
    reserve(holder, 0x40000, copy);
    expect((ppu_slow_page[0x40] & VM_PAGE_COHERENT) != 0, "a reservation marks its page coherent");
    g_full = 0; g_recheck = 0;
    st(0x40100, 0x11111111u);
    st(0x40FFC, 0x22222222u);
    { ppu_context c; memset(&c, 0, sizeof c); c.gpr[3] = 0x40200; c.gpr[4] = 0x0123456789ABCDEFull; c.gpr[5] = 0xFEDCBA9876543210ull; t_vec(&c); }
    expect(g_full == 0 && g_recheck == 0, "unreserved lines of a coherency page store inline");
    expect(rd(0x40100) == 0x11111111u && rd(0x40FFC) == 0x22222222u, "inline stores land");
    expect(*(uint64_t*)(vm_base + 0x40200) == 0x0123456789ABCDEFull && *(uint64_t*)(vm_base + 0x40208) == 0xFEDCBA9876543210ull, "vector lands");
    st(0x40004, 0x33333333u);
    expect(g_full == 1 && rd(0x40004) == 0x33333333u, "the reserved line takes the runtime path");
    expect(lost(holder), "the store ends the reservation");

    ppu_special_pages(0x41000, 0x1000);
    g_full = 0;
    st(0x41010, 1);
    expect(g_full == 1, "a special page always takes the runtime path");

    vm_track(0x42000, 0x1000, seen, 0);
    vm_track_drain(seen, 0);
    g_reported = 0; g_full = 0; g_recheck = 0;
    st(0x42010, 7);
    expect(g_full == 0 && g_recheck == 1 && rd(0x42010) == 7, "tracked page: inline store, runtime re-check");
    vm_track_drain(seen, 0);
    expect(g_reported == 1, "tracked page reported after the store");
    st(0x42020, 8);
    vm_track_drain(seen, 0);
    expect(g_reported == 2 && rd(0x42020) == 8, "re-armed page reported again");

    const int rounds = 100000;
    spu_context* ctx = (spu_context*)calloc(1, sizeof(spu_context));
    uint32_t line = 0;
    std::thread ppu([&] {
        for (int k = 0; k < rounds; k++) {
            while (round_ready.load(std::memory_order_acquire) != k + 1) {}
            for (volatile int d = (k * 7) % 96; d > 0; d--) {}
            st(line + 4, 0x5EED0000u + (uint32_t)k);
            ppu_done.store(k + 1, std::memory_order_release);
        }
    });
    std::thread spu([&] {
        for (int k = 0; k < rounds; k++) {
            while (round_ready.load(std::memory_order_acquire) != k + 1) {}
            reserve(ctx, line, copy);
            spu_done.store(k + 1, std::memory_order_release);
        }
    });
    int missed = 0, in_copy = 0, ended = 0;
    spu_context* anchor = (spu_context*)calloc(1, sizeof(spu_context));
    uint8_t scratch[128];
    for (int k = 0; k < rounds; k++) {
        const uint32_t page = 0x100000u + (uint32_t)(k / 31) * 0x1000u;
        if (k % 31 == 0) reserve(anchor, page + 0xF80u, scratch);
        line = page + 0x80u * (uint32_t)(k % 31);
        round_ready.store(k + 1, std::memory_order_release);
        while (ppu_done.load(std::memory_order_acquire) != k + 1 || spu_done.load(std::memory_order_acquire) != k + 1) {}
        uint32_t v; memcpy(&v, copy + 4, 4);
        const int l = lost(ctx);
        if (__builtin_bswap32(v) == 0x5EED0000u + (uint32_t)k) in_copy++; else if (l) ended++; else missed++;
    }
    ppu.join();
    spu.join();
    expect(missed == 0, "race: store in the copy or reservation ended");
    printf("%d rounds: store in copy %d, reservation ended %d, missed %d; %d failures\n", rounds, in_copy, ended, missed, g_fail);
    return g_fail != 0;
}
"""


def test_store_coherent():
    cc = shutil.which(os.environ.get("CC", "cc")) or shutil.which("clang")
    cxx = shutil.which(os.environ.get("CXX", "c++")) or shutil.which("clang++")
    if cc is None or cxx is None:
        if os.environ.get("CI"):
            raise SystemExit("FAIL: no C/C++ compiler found")
        print("skip: no compiler")
        return
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        lifter = L.PPULifter()
        (td / "ppu_recomp.h").write_text(lifter.emit_header())
        (td / "lifted.cpp").write_text("\n".join(lifter._preamble_lines()) + LIFTED)
        (td / "driver.cpp").write_text(DRIVER)
        inc = ["-I" + str(td), "-I" + str(ROOT / "include"), "-I" + str(ROOT / "runtime" / "ppu"), "-I" + str(ROOT / "runtime" / "memory"),
               "-I" + str(ROOT / "runtime" / "spu"), "-I" + str(ROOT / "runtime" / "platform")]
        for opt in ("-O0", "-O2"):
            objs = []
            srcs = [(ROOT / "runtime/spu/spu_coherency.c", cc, "-std=gnu17"), (ROOT / "runtime/platform/ps3_sync.c", cc, "-std=gnu17"),
                    (ROOT / "runtime/memory/vm_track.c", cc, "-std=gnu17"), (td / "lifted.cpp", cxx, "-std=c++20"), (td / "driver.cpp", cxx, "-std=c++20")]
            if os.name != "nt":
                srcs.append((ROOT / "runtime/platform/win32_compat.c", cc, "-std=gnu17"))
            for src, comp, std in srcs:
                obj = td / (src.stem + opt + ".o")
                r = subprocess.run([comp, std, opt, "-w", "-march=native", "-fno-strict-aliasing", "-D_CRT_SECURE_NO_WARNINGS", *inc, "-c", str(src),
                                    "-o", str(obj)], capture_output=True, text=True)
                assert r.returncode == 0, r.stdout + r.stderr
                objs.append(str(obj))
            exe = td / ("t" + opt + (".exe" if os.name == "nt" else ""))
            libs = ["-lsynchronization"] if os.name == "nt" else ["-lpthread", "-lm"]
            r = subprocess.run([cxx, "-o", str(exe), *objs, *libs], capture_output=True, text=True)
            assert r.returncode == 0, r.stdout + r.stderr
            out = subprocess.run([str(exe)], capture_output=True, text=True)
            print("  " + opt + ": " + out.stdout.strip().replace("\n", "\n  "))
            assert out.returncode == 0, out.stdout + out.stderr


if __name__ == "__main__":
    test_store_coherent()
    print("ok test_store_coherent")
