#!/usr/bin/env python3
"""Guest write tracking through slow-page bits (runtime/memory/vm_track.c).

vm_track arms pages in ppu_slow_page; a write to an armed page reports it once
(vm_track_write disarms and marks it dirty) and vm_track_drain re-arms every
dirty page before reporting it. Checked against a renderer-style reader that
re-reads a page whenever it is reported:
  - first tracking reports every page once, and an untouched page is never
    reported again;
  - a pinned range (vm_io_begin/vm_io_end) is reported on every drain until
    unpinned, then once more;
  - racing a writer against back-to-back drains, the reader always ends up
    with the writer's last value: a host writer that marks after its copy, and
    a lifted guest store that takes the inline page check and the re-check
    after the store (the path the PPU uses).
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
}
"""

DRIVER = r"""
#include "ppu_recomp.h"
#include "ppu_store_thunk.h"
#include "vm_track.h"
#include <atomic>
#include <thread>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern "C" {
void t_store(ppu_context*);
uint8_t* vm_base;
__attribute__((used)) void ppu_store_slow(uint64_t w, uint64_t lo, uint64_t hi, void* ra)
{
    (void)hi; (void)ra;
    const uint32_t a = (uint32_t)w, op = (uint32_t)(w >> 32);
    if (op == 0x44) memcpy(vm_base + a, &lo, 4);
    vm_track_write(a, op ? 4 : 1);
}
}
static int g_fail;
static uint32_t g_seen[64];
static int g_hits[64];
static void expect(int ok, const char* what) { if (!ok) { printf("FAIL %s\n", what); g_fail++; } }
static void note(void* user, uint32_t page) { (void)user; g_hits[page - 0x100]++; }
static uint32_t rd(uint32_t ea) { uint32_t v; memcpy(&v, vm_base + ea, 4); return v; }
static void seen(void* user, uint32_t page) { uint32_t* last = (uint32_t*)user; *last = rd(page << 12); g_hits[page - 0x100]++; }

static uint32_t race(int lifted, int rounds)
{
    const uint32_t ea = 0x150000;
    uint32_t last = 0, bad = 0;
    for (int r = 0; r < rounds; r++) {
        std::atomic<int> go{0}, done{0};
        const uint32_t n = 200 + (uint32_t)(r % 37);
        std::thread w([&] {
            while (!go.load(std::memory_order_acquire)) {}
            for (uint32_t k = 1; k <= n; k++) {
                const uint32_t v = (uint32_t)r * 100000u + k;
                if (lifted) {
                    ppu_context c; memset(&c, 0, sizeof c);
                    c.gpr[3] = ea; c.gpr[4] = __builtin_bswap32(v);
                    t_store(&c);
                } else {
                    memcpy(vm_base + ea, &v, 4);
                    vm_track_write(ea, 4);
                }
            }
            done.store(1, std::memory_order_release);
        });
        go.store(1, std::memory_order_release);
        while (!done.load(std::memory_order_acquire)) vm_track_drain(seen, &last);
        w.join();
        vm_track_drain(seen, &last);
        if (last != (uint32_t)r * 100000u + n) bad++;
    }
    return bad;
}

int main(void)
{
    vm_base = (uint8_t*)calloc(1, 0x200000);
    vm_track(0x100000, 0x8000, note, 0);
    for (int i = 0; i < 8; i++) expect(g_hits[i] == 1, "first tracking reports each page once");
    memset(g_hits, 0, sizeof g_hits);
    expect(vm_track_drain(note, 0) == 0, "nothing written, nothing reported");
    vm_track_write(0x101004, 4);
    vm_track_write(0x101008, 4);
    vm_track_write(0x103FFE, 4);
    expect(vm_track_drain(note, 0) == 3 && g_hits[1] == 1 && g_hits[3] == 1 && g_hits[4] == 1, "written pages reported once");
    expect(vm_track_drain(note, 0) == 0, "re-armed pages stay quiet");
    memset(g_hits, 0, sizeof g_hits);
    vm_io_begin(vm_base + 0x105000, 0x1000);
    vm_track_drain(note, 0);
    vm_track_drain(note, 0);
    vm_io_end(vm_base + 0x105000, 0x1000);
    vm_track_drain(note, 0);
    expect(g_hits[5] == 3 && vm_track_drain(note, 0) == 0, "pinned page reported every drain, then once after unpin");
    vm_track(0x150000, 0x1000, note, 0);
    vm_track_drain(note, 0);
    const uint32_t host = race(0, 3000), lifted = race(1, 3000);
    expect(host == 0, "host writer: reader ends with the last value");
    expect(lifted == 0, "lifted writer: reader ends with the last value");
    printf("race misses: host %u, lifted %u; %d failures\n", host, lifted, g_fail);
    return g_fail != 0;
}
"""


def test_vm_track():
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
            srcs = [(ROOT / "runtime/memory/vm_track.c", cc, "-std=gnu17"), (td / "lifted.cpp", cxx, "-std=c++20"), (td / "driver.cpp", cxx, "-std=c++20")]
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
    test_vm_track()
    print("ok test_vm_track")
