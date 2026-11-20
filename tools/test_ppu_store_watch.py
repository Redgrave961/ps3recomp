#!/usr/bin/env python3
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
void t_forms(ppu_context* __restrict ctx)
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

void t_word(ppu_context* __restrict ctx)
{
    PPU_MM_SNAP
    vm_write32(ctx->gpr[3] + 0x40, ctx->gpr[4]);
    vm_write64(ctx->gpr[3] + 0x48, ctx->gpr[4]);
    vm_write32(ctx->gpr[3], ctx->gpr[4]);
    vm_write32(ctx->gpr[3] + 0x80, ctx->gpr[4]);
}
}
"""

DRIVER = r"""
#include "ppu_recomp.h"
#include "ppu_store_thunk.h"
#include "vm_page_map.h"
#ifdef _WIN32
#include <windows.h>
#else
#include "win32_compat.h"
#endif
#include <atomic>
#include <thread>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern "C" {
void t_forms(ppu_context*);
void t_word(ppu_context*);
uint8_t* vm_base;
}
static std::atomic<long> g_slow, g_notify;
extern "C" __attribute__((used)) void ppu_store_slow(uint64_t w, uint64_t lo, uint64_t hi, void* ra)
{
    (void)ra;
    const uint32_t a = (uint32_t)w, op = (uint32_t)(w >> 32);
    g_slow.fetch_add(1);
    uint32_t n = 16;
    if (op) {
        const uint64_t d[2] = {lo, hi};
        n = (op & 0xF0u) == 0x40u ? op & 15u : op;
        memcpy(vm_base + a, n > 16 ? (const void*)(uintptr_t)lo : d, n);
    }
    if (vm_watched(a, n)) g_notify.fetch_add(1);
}
static int g_fail;
static void expect(int ok, const char* what) { if (!ok) { printf("FAIL %s\n", what); g_fail++; } }

static void forms(uint32_t base, uint32_t off, uint32_t n)
{
    static uint8_t ref[0x200];
    for (int watched = 0; watched < 2; watched++) {
        memset(vm_base + 0x10000, 0xA5, 0x2000);
        ppu_context c; memset(&c, 0, sizeof c);
        c.gpr[3] = base; c.gpr[4] = 0x0123456789ABCDEFull; c.gpr[5] = 0xFEDCBA9876543210ull;
        c.gpr[6] = 0x31; c.gpr[7] = off; c.gpr[8] = n;
        if (watched) vm_watch(0x10FF0);
        g_slow = 0; g_notify = 0;
        t_forms(&c);
        if (watched) vm_unwatch(0x10FF0);
        if (!watched) { memcpy(ref, vm_base + base, sizeof ref); continue; }
        expect(memcmp(ref, vm_base + base, sizeof ref) == 0, "watched page stores land the same bytes");
        const int done_inline = n == 1 || n == 2 || n == 4 || n == 8 || n == 16;
        expect(g_slow == (done_inline ? 1 : 2), "only dcbz and odd raw sizes reach the runtime");
        expect(g_notify == 0, "stores beside the watched word do not notify");
    }
}

int main(void)
{
    vm_base = (uint8_t*)calloc(1, 0x200000);
    for (uint32_t n = 1; n <= 16; n++)
        for (uint32_t off = 0; off + n <= 16; off += 3) forms(0x10000 + 0x40 * (n & 1), off, n);

    ppu_context c; memset(&c, 0, sizeof c);
    c.gpr[3] = 0x20000; c.gpr[4] = 0x11223344;
    vm_watch(0x20000);
    g_slow = 0; g_notify = 0;
    t_word(&c);
    vm_unwatch(0x20000);
    expect(g_notify == 1 && g_slow == 1, "the store over the watched word notifies, its neighbours stay inline");
    expect(*(uint32_t*)(vm_base + 0x20000) == __builtin_bswap32(0x11223344u), "watched word written");

    const int rounds = 60000;
    std::atomic<int> go{0}, stored{0};
    long lost = 0, seen = 0, woke = 0;
    uint32_t word = 0;
    std::thread writer([&] {
        for (int k = 1; k <= rounds; k++) {
            while (go.load(std::memory_order_acquire) != k) {}
            for (volatile int d = (k * 13) % 200; d > 0; d--) {}
            ppu_context w; memset(&w, 0, sizeof w);
            w.gpr[3] = word; w.gpr[4] = (uint64_t)k;
            t_word(&w);
            stored.store(k, std::memory_order_release);
        }
    });
    for (int k = 1; k <= rounds; k++) {
        word = 0x30000u + (uint32_t)(k % 16) * 0x1000u + (uint32_t)(k % 7) * 0x100u;
        memset(vm_base + word, 0xEE, 4);
        const long before = g_notify.load();
        go.store(k, std::memory_order_release);
        for (volatile int d = (k * 7) % 150; d > 0; d--) {}
        vm_watch(word);
        FlushProcessWriteBuffers();
        const uint32_t v = __atomic_load_n((uint32_t*)(vm_base + word), __ATOMIC_ACQUIRE);
        while (stored.load(std::memory_order_acquire) != k) {}
        if (v != 0xEEEEEEEEu) seen++;
        else if (g_notify.load() != before) woke++;
        else lost++;
        vm_unwatch(word);
    }
    writer.join();
    expect(lost == 0, "race: the RSX re-reads the new word or is notified");
    printf("%d rounds: seen by re-check %ld, notified %ld, lost %ld; %d failures\n", rounds, seen, woke, lost, g_fail);
    return g_fail != 0;
}
"""


def test_store_watch():
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
               "-I" + str(ROOT / "runtime" / "platform")]
        for opt in ("-O0", "-O2"):
            objs = []
            srcs = [(td / "lifted.cpp", cxx, "-std=c++20"), (td / "driver.cpp", cxx, "-std=c++20")]
            if os.name != "nt":
                srcs.append((ROOT / "runtime/platform/win32_compat.c", cc, "-std=gnu17"))
            for src, comp, std in srcs:
                obj = td / (src.stem + opt + ".o")
                r = subprocess.run([comp, std, opt, "-w", "-march=native", "-fno-strict-aliasing", "-D_CRT_SECURE_NO_WARNINGS", *inc, "-c", str(src),
                                    "-o", str(obj)], capture_output=True, text=True)
                assert r.returncode == 0, r.stdout + r.stderr
                objs.append(str(obj))
            exe = td / ("t" + opt + (".exe" if os.name == "nt" else ""))
            libs = [] if os.name == "nt" else ["-lpthread", "-lm"]
            r = subprocess.run([cxx, "-o", str(exe), *objs, *libs], capture_output=True, text=True)
            assert r.returncode == 0, r.stdout + r.stderr
            out = subprocess.run([str(exe)], capture_output=True, text=True)
            print("  " + opt + ": " + out.stdout.strip().replace("\n", "\n  "))
            assert out.returncode == 0, out.stdout + out.stderr


if __name__ == "__main__":
    test_store_watch()
    print("ok test_store_watch")
