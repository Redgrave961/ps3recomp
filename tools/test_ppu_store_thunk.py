#!/usr/bin/env python3
"""The lifted store path: inline page check, out-of-line slow store through ppu_st_thunk.

Lifted-style functions are compiled with the lifter's real source preamble and
run twice, once with every page fast and once with every page slow, against a
runtime whose ppu_store_slow performs the store. Checked:
  - every store form (8/16/32/64/128-bit, raw 1..16 bytes, dcbz) lands the same
    bytes either way, and the slow path reports the exact address, form, value
    and the lifted caller's return address;
  - values live across slow stores (GPRs and vectors) survive the thunk;
  - a function that holds the reservation in registers hands the runtime its
    current reserve_addr/reserve_time and reads back the time the runtime set;
  - a stack walk from inside the slow store reaches the lifted function and its
    caller;
  - racing an SPU's first GETLLAR of a line (the real spu_coh_reserve), the
    store is either in the SPU's copy or ends the reservation, never neither.
"""
from __future__ import annotations

import importlib.util
import os
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("ppu_lifter", ROOT / "tools" / "ppu_lifter.py")
L = importlib.util.module_from_spec(spec)
spec.loader.exec_module(L)

LIFTED = r"""
extern "C" {
void t_ops(ppu_context* __restrict ctx)
{
    PPU_MM_SNAP
    vm_write8(ctx->gpr[3] + 1, ctx->gpr[4]);
    vm_write16(ctx->gpr[3] + 2, ctx->gpr[4]);
    vm_write32(ctx->gpr[3] + 4, ctx->gpr[4]);
    vm_write64(ctx->gpr[3] + 8, ctx->gpr[4]);
    vm_write128(ctx->gpr[3] + 16, _mm_set_epi64x((long long)ctx->gpr[5], (long long)ctx->gpr[4]));
    { uint8_t s[16]; for (int i = 0; i < 16; i++) s[i] = (uint8_t)(ctx->gpr[6] + 17 * i);
      vm_write_raw(ctx->gpr[3] + 32 + ctx->gpr[7], s, (uint32_t)ctx->gpr[8]); }
    { uint64_t raw = ctx->gpr[4]; vm_write_raw(ctx->gpr[3] + 64, &raw, 8); }
    { uint32_t raw = (uint32_t)ctx->gpr[5]; vm_write_raw(ctx->gpr[3] + 72, &raw, 4); }
    { uint16_t raw = (uint16_t)ctx->gpr[6]; vm_write_raw(ctx->gpr[3] + 76, &raw, 2); }
    { static const uint8_t zero[128] = {0}; vm_write_raw((ctx->gpr[3] + 128) & ~0x7FULL, zero, 128); }
}

void t_pressure(ppu_context* __restrict ctx)
{
    PPU_MM_SNAP
    uint64_t g[24];
    for (int i = 0; i < 24; i++) g[i] = ctx->gpr[i + 4] * 0x9E3779B97F4A7C15ull + (uint64_t)i;
    __m128i v[14];
    for (int i = 0; i < 14; i++) v[i] = _mm_add_epi32(vmx_ld(&ctx->vr[i]), _mm_set1_epi32(i));
    for (int k = 0; k < 6; k++) {
        for (int i = 0; i < 24; i++) {
            vm_write32(ctx->gpr[3] + 4 * (uint32_t)i, g[i] ^ (uint64_t)k);
            g[i] = g[i] * 3 + g[(i + 5) % 24];
        }
        for (int i = 0; i < 14; i++) {
            vm_write128(ctx->gpr[3] + 0x100 + 16 * (uint32_t)i, v[i]);
            v[i] = _mm_xor_si128(_mm_add_epi64(v[i], v[(i + 3) % 14]), _mm_set1_epi8((char)k));
        }
    }
    for (int i = 0; i < 24; i++) ctx->gpr[i + 4] = g[i];
    for (int i = 0; i < 14; i++) vmx_st(&ctx->vr[i], v[i]);
}

void t_resv(ppu_context* __restrict ctx)
{
    PPU_MM_SNAP_RESV
    ctx->reserve_addr = (uint32_t)ctx->gpr[3];
    ctx->reserve_time = ctx->gpr[4];
    vm_write32(ctx->gpr[3], ctx->gpr[5]);
    ctx->gpr[6] = ctx->reserve_time;
    ctx->reserve_time = ctx->gpr[6] + 100;
    vm_write32(ctx->gpr[3] + 4, ctx->gpr[5]);
    ctx->gpr[7] = ctx->reserve_time;
}

void t_race(ppu_context* __restrict ctx)
{
    PPU_MM_SNAP
    vm_write32(ctx->gpr[3] + 4, ctx->gpr[4]);
}
}
"""

DRIVER = r"""
#include "ppu_recomp.h"
#include "ppu_store_thunk.h"
#include "spu_coherency.h"
#include "spu_coh_bitmap.h"
#include <atomic>
#include <thread>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <execinfo.h>
#endif
extern "C" {
void t_ops(ppu_context*);
void t_pressure(ppu_context*);
void t_resv(ppu_context*);
void t_race(ppu_context*);
uint8_t* vm_base;
void spu_ch_wake(spu_context* c) { (void)c; }
int32_t ppu_thread_priority(uint64_t tid) { (void)tid; return 0; }
}
static ppu_context* g_ctx;
static int g_log_on, g_log_n, g_fail, g_bt_ok;
static uint64_t g_log[64][4];
static uint64_t g_seen_addr, g_seen_time, g_set_time;
static void* g_bt_lo;
static void* g_bt_hi;
static void notify(uint32_t a) { spu_lockline_lock(a); spu_coh_notify_write(a); spu_lockline_unlock(a); }
static void land(uint32_t a, const void* src, uint32_t n)
{
    if (spu_coh_line_reserved(a)) { spu_lockline_lock(a); memcpy(vm_base + a, src, n); spu_coh_notify_write(a); spu_lockline_unlock(a); return; }
    memcpy(vm_base + a, src, n);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (spu_coh_line_reserved(a)) notify(a);
}
extern "C" __attribute__((used)) void ppu_store_slow(uint64_t w, uint64_t lo, uint64_t hi, void* ra)
{
    const uint32_t a = (uint32_t)w, op = (uint32_t)(w >> 32);
    if (g_log_on && g_log_n < 64) { g_log[g_log_n][0] = w; g_log[g_log_n][1] = lo; g_log[g_log_n][2] = hi; g_log[g_log_n][3] = (uint64_t)(uintptr_t)ra; g_log_n++; }
    if (g_ctx) {
        g_seen_addr = g_ctx->reserve_addr; g_seen_time = g_ctx->reserve_time;
        if (g_set_time) g_ctx->reserve_time = g_set_time;
    }
    if (g_bt_lo) {
        void* fr[16] = {};
#ifdef _WIN32
        const int n = RtlCaptureStackBackTrace(0, 16, fr, 0);
#else
        const int n = backtrace(fr, 16);
#endif
        int lifted = 0, caller = 0;
        for (int i = 0; i < n; i++) {
            if (fr[i] == ra) lifted = 1;
            if ((char*)fr[i] > (char*)g_bt_lo && (char*)fr[i] < (char*)g_bt_hi) caller = 1;
        }
        g_bt_ok = lifted && caller;
    }
    if (op == 0) { if (spu_coh_line_reserved(a)) notify(a); return; }
    uint64_t d[2] = {lo, hi};
    if (op == 0x41 || op == 0x42 || op == 0x44 || op == 0x48) { land(a, d, op & 15); return; }
    land(a, op > 16 ? (const void*)(uintptr_t)lo : d, op);
}
static void expect(int ok, const char* what) { if (!ok) { printf("FAIL %s\n", what); g_fail++; } }
static void set_slow(int on) { memset(ppu_slow_page, on, 1u << 20); }

__attribute__((noinline)) static void walk_from(ppu_context* c) { t_ops(c); __asm__ volatile("" ::: "memory"); }

static void ops_case(uint8_t* mem, uint32_t base, uint32_t off, uint32_t n)
{
    uint8_t ref[0x200];
    for (int slow = 0; slow < 2; slow++) {
        memset(mem, 0xA5, 0x200000);
        ppu_context c; memset(&c, 0, sizeof c);
        c.gpr[3] = base; c.gpr[4] = 0x0123456789ABCDEFull; c.gpr[5] = 0xFEDCBA9876543210ull;
        c.gpr[6] = 0x31; c.gpr[7] = off; c.gpr[8] = n;
        set_slow(slow); g_log_on = slow; g_log_n = 0;
        t_ops(&c);
        set_slow(0); g_log_on = 0;
        if (!slow) { memcpy(ref, mem + base, sizeof ref); continue; }
        expect(memcmp(ref, mem + base, sizeof ref) == 0, "slow stores land the same bytes");
        expect(g_log_n == 10, "one slow call per store");
        const uint32_t ops[10] = {0x41, 0x42, 0x44, 0x48, 16, n, 8, 4, 2, 0x80};
        const uint32_t eas[10] = {base + 1, base + 2, base + 4, base + 8, base + 16, base + 32 + off, base + 64, base + 72, base + 76,
                                  (base + 128) & ~0x7Fu};
        for (int i = 0; i < 10 && i < g_log_n; i++) {
            expect((uint32_t)g_log[i][0] == eas[i] && (uint32_t)(g_log[i][0] >> 32) == ops[i], "slow call address and form");
            expect((char*)(uintptr_t)g_log[i][3] > (char*)&t_ops && (char*)(uintptr_t)g_log[i][3] < (char*)&t_ops + 0x4000,
                   "return address is in the lifted function");
        }
        expect(g_log[0][1] == 0xEF && g_log[1][1] == 0xEFCD && g_log[2][1] == 0xEFCDAB89u && g_log[3][1] == 0xEFCDAB8967452301ull,
               "typed values arrive in guest byte order");
        expect(g_log[4][1] == 0x0123456789ABCDEFull && g_log[4][2] == 0xFEDCBA9876543210ull, "vector value");
    }
}

static void pressure(uint8_t* mem, int slow, ppu_context* out)
{
    memset(mem, 0, 0x200000);
    ppu_context c; memset(&c, 0, sizeof c);
    c.gpr[3] = 0x10000;
    for (int i = 4; i < 32; i++) c.gpr[i] = 0x1111111111111111ull * (uint64_t)i;
    for (int i = 0; i < 32; i++) for (int j = 0; j < 16; j++) ((uint8_t*)&c.vr[i])[j] = (uint8_t)(i * 16 + j);
    set_slow(slow);
    t_pressure(&c);
    set_slow(0);
    *out = c;
}

static std::atomic<int> round_ready, ppu_done, spu_done;

int main(void)
{
    uint8_t* mem = (uint8_t*)calloc(1, 0x200000);
    vm_base = mem;
    for (uint32_t n = 1; n <= 16; n++)
        for (uint32_t off = 0; off + n <= 16; off += 5) ops_case(mem, 0x10000 + 0x40 * (n & 1), off, n);
    ppu_context a, b;
    pressure(mem, 0, &a);
    uint8_t ref[0x400]; memcpy(ref, mem + 0x10000, sizeof ref);
    pressure(mem, 1, &b);
    expect(memcmp(&a, &b, sizeof a) == 0, "registers survive slow stores");
    expect(memcmp(ref, mem + 0x10000, sizeof ref) == 0, "pressure stores land the same bytes");

    ppu_context r; memset(&r, 0, sizeof r);
    r.gpr[3] = 0x20000; r.gpr[4] = 0x40; r.gpr[5] = 7;
    g_ctx = &r; g_set_time = 0x42;
    set_slow(1); t_resv(&r); set_slow(0);
    expect(g_seen_addr == 0x20000, "reserve_addr in memory at the slow store");
    expect(g_seen_time == 0x42 + 100, "reserve_time in memory at the second slow store");
    expect(r.gpr[6] == 0x42 && r.gpr[7] == 0x42 && r.reserve_time == 0x42, "the runtime's reserve_time is read back");
    g_ctx = 0; g_set_time = 0;

    ppu_context w; memset(&w, 0, sizeof w);
    w.gpr[3] = 0x30000; w.gpr[4] = 1; w.gpr[5] = 2; w.gpr[6] = 3; w.gpr[7] = 0; w.gpr[8] = 4;
    g_bt_lo = (void*)&walk_from; g_bt_hi = (void*)((char*)&walk_from + 0x100);
    set_slow(1); walk_from(&w); set_slow(0);
    g_bt_lo = 0;
    expect(g_bt_ok, "stack walk from the slow store reaches the lifted function and its caller");

    const int rounds = 100000;
    uint8_t* big = (uint8_t*)calloc(1, 0x100000u + (size_t)(rounds / 32 + 1) * 0x1000u);
    vm_base = big;
    spu_context* sctx = (spu_context*)calloc(1, sizeof(spu_context));
    uint8_t copy[128];
    uint32_t line = 0;
    std::thread ppu([&] {
        for (int k = 0; k < rounds; k++) {
            while (round_ready.load(std::memory_order_acquire) != k + 1) {}
            for (volatile int d = (k * 7) % 96; d > 0; d--) {}
            ppu_context c; memset(&c, 0, sizeof c);
            c.gpr[3] = line; c.gpr[4] = 0x5EED0000u + (uint32_t)k;
            t_race(&c);
            ppu_done.store(k + 1, std::memory_order_release);
        }
    });
    std::thread spu([&] {
        for (int k = 0; k < rounds; k++) {
            while (round_ready.load(std::memory_order_acquire) != k + 1) {}
            spu_lockline_lock(line);
            spu_coh_reserve(sctx, line);
            memcpy(copy, vm_base + (line & ~127u), 128);
            sctx->resv = (line & ~127u) | 1u;
            spu_lockline_unlock(line);
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
        spu_lockline_lock(line);
        const int l = !(sctx->resv & 1u) && (sctx->event_status & SPU_EVENT_LR);
        sctx->resv = 0;
        SPU_EVENT_CLEAR(sctx, SPU_EVENT_LR);
        spu_lockline_unlock(line);
        if (v == 0x5EED0000u + (uint32_t)k) seen++; else if (l) lost++; else missed++;
    }
    ppu.join();
    spu.join();
    expect(missed == 0, "race: store in the copy or reservation lost");
    printf("%d rounds: store in copy %d, reservation lost %d, missed %d; %d failures\n", rounds, seen, lost, missed, g_fail);
    return g_fail != 0;
}
"""


def test_store_thunk():
    cc = shutil.which(os.environ.get("CC", "cc")) or shutil.which("clang")
    cxx = shutil.which(os.environ.get("CXX", "c++")) or shutil.which("clang++")
    if cc is None or cxx is None:
        if os.environ.get("CI"):
            raise SystemExit("FAIL: no C/C++ compiler found")
        print("skip: no compiler")
        return
    import tempfile
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        lifter = L.PPULifter()
        (td / "ppu_recomp.h").write_text(lifter.emit_header())
        (td / "lifted.cpp").write_text("\n".join(lifter._preamble_lines()) + LIFTED)
        (td / "driver.cpp").write_text(DRIVER)
        inc = ["-I" + str(td), "-I" + str(ROOT / "include"), "-I" + str(ROOT / "runtime" / "ppu"), "-I" + str(ROOT / "runtime" / "spu"),
               "-I" + str(ROOT / "runtime" / "platform")]
        for opt in ("-O0", "-O2"):
            objs = []
            for src, comp, std in ((ROOT / "runtime/spu/spu_coherency.c", cc, "-std=gnu17"), (ROOT / "runtime/platform/ps3_sync.c", cc, "-std=gnu17"),
                                   (td / "lifted.cpp", cxx, "-std=c++20"), (td / "driver.cpp", cxx, "-std=c++20")):
                obj = td / (src.stem + opt + ".o")
                r = subprocess.run([comp, std, opt, "-w", "-march=native", "-fno-strict-aliasing", "-D_CRT_SECURE_NO_WARNINGS", *inc, "-c", str(src),
                                    "-o", str(obj)], capture_output=True, text=True)
                assert r.returncode == 0, r.stdout + r.stderr
                objs.append(str(obj))
            if os.name != "nt" and (ROOT / "runtime/platform/win32_compat.c").exists():
                obj = td / ("win32_compat" + opt + ".o")
                r = subprocess.run([cc, "-std=gnu17", opt, "-w", *inc, "-c", str(ROOT / "runtime/platform/win32_compat.c"), "-o", str(obj)],
                                   capture_output=True, text=True)
                assert r.returncode == 0, r.stdout + r.stderr
                objs.append(str(obj))
            exe = td / ("t" + opt + (".exe" if os.name == "nt" else ""))
            libs = ["-lsynchronization"] if os.name == "nt" else ["-lpthread", "-lm", "-rdynamic"]
            r = subprocess.run([cxx, "-o", str(exe), *objs, *libs], capture_output=True, text=True)
            assert r.returncode == 0, r.stdout + r.stderr
            out = subprocess.run([str(exe)], capture_output=True, text=True)
            print("  " + opt + ": " + out.stdout.strip().replace("\n", "\n  "))
            assert out.returncode == 0, out.stdout + out.stderr


if __name__ == "__main__":
    test_store_thunk()
    print("ok test_store_thunk")
