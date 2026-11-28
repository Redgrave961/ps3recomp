#!/usr/bin/env python3
"""The RSX doorbell skips its bell increment and wake while the RSX thread is
not sleeping, and no RSX wait ever loses a wake.

The real libs/video/rsx_core.c runs a waiter thread through the predicates its
wait sites use (FIFO idle and fault waits, park, semaphore acquire, and the
main-thread label wait) with infinite timeouts, against a writer thread that
changes the awaited state at random offsets via every notifier: PUT plus
doorbell, guest window writes, immediate flip, label retire and FIFO reset. A
lost wake hangs the waiter and fails the run.
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ROUNDS = 40000

DRIVER = r"""
#include "rsx_core.c"
#include <stdint.h>

uint8_t* vm_base;
uint64_t ppu_timebase_now(void) { return 0; }
void (*g_rsx_window_write)(void);
uint32_t g_ww_lo, g_ww_hi;
void ps3_ww_report_inline(uint32_t addr, uint64_t val, int width) { (void)addr; (void)val; (void)width; }
int spu_coh_is_reserved(uint32_t addr) { (void)addr; return 0; }
void spu_lockline_lock(uint32_t ea) { (void)ea; }
void spu_lockline_unlock(uint32_t ea) { (void)ea; }
void spu_lockline_lock_range(uint32_t ea, uint32_t size) { (void)ea; (void)size; }
void spu_lockline_unlock_range(uint32_t ea, uint32_t size) { (void)ea; (void)size; }
void spu_coh_notify_write(uint32_t ea) { (void)ea; }
void rsx_transfer_reset(void) {}
u32 rsx_transfer_methods(u32 reg, u32 inc, const u32* be_args, u32 count) { (void)reg; (void)inc; (void)be_args; return count; }
struct ppu_context* ppu_thread_adopt(const char* name, uint32_t stack_size, int32_t priority) { (void)name; (void)stack_size; (void)priority; return 0; }

static volatile u32 ready, go, finished;

static u32 next_rand(u32* r)
{
    u32 x = *r;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *r = x;
    return x;
}

static void jitter(u32* r, u32 i)
{
    for (u32 d = next_rand(r) % ((i & 16u) ? 2048u : 96u); d; d--) __asm__ volatile("" ::: "memory");
}

static void put_be(u32 ea, u32 v) { __atomic_store_n((u32*)(vm_base + ea), __builtin_bswap32(v), __ATOMIC_RELAXED); }

static void start_round(u32 i)
{
    __atomic_store_n(&ready, i + 1u, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&go, __ATOMIC_SEQ_CST) != i + 1u) __builtin_ia32_pause();
}

static void rsx_side(u32 i, u32* r)
{
    const u32 kind = i % 7u;
    const u32 word_ea = RSX_SEMAPHORE_EA + 0x100u;
    const u32 put = rsx_load(RSX_PUT_EA) & ~3u;
    const u32 word = rsx_load(word_ea);
    const rsx_hold idle = { put, s.get, 0, 0 };
    const rsx_hold park = { put, s.get, word_ea, word };
    const rsx_hold sema = { 0, 0, word_ea, i };
    const rsx_hold held = { 0, 0, word_ea, ~word };
    start_round(i);
    jitter(r, i);
    if (kind == 0u || kind == 4u || kind == 5u) {
        rsx_wait_change(put);
        if (!rsx_idle_over(&idle)) { printf("idle returned unchanged at %u\n", i); _Exit(4); }
        if (kind == 4u && !__atomic_exchange_n(&s.immediate_flip, 0u, __ATOMIC_ACQ_REL)) { printf("flip missing at %u\n", i); _Exit(4); }
    } else if (kind == 1u) {
        while (!rsx_idle_over(&park)) rsx_await(rsx_idle_over, &park, INFINITE);
    } else if (kind == 2u) {
        __atomic_add_fetch(&s_sema_waiters, 1u, __ATOMIC_SEQ_CST);
        while (rsx_load(word_ea) != i) rsx_await(rsx_label_over, &sema, INFINITE);
        __atomic_sub_fetch(&s_sema_waiters, 1u, __ATOMIC_RELEASE);
    } else if (kind == 3u) {
        rsx_core_wait_label(word_ea, word);
    } else {
        while (!__atomic_exchange_n(&s.immediate_flip, 0u, __ATOMIC_ACQ_REL)) rsx_await(rsx_label_over, &held, INFINITE);
    }
    __atomic_store_n(&finished, i + 1u, __ATOMIC_SEQ_CST);
}

static DWORD WINAPI rsx_waiter(LPVOID n)
{
    u32 r = 0x9E3779B9u;
    for (u32 i = 0; i < (u32)(uintptr_t)n; i++) rsx_side(i, &r);
    return 0;
}

static u32 put_value;

static void writer_side(u32 i, u32* r)
{
    const u32 kind = i % 7u;
    const u32 word_ea = RSX_SEMAPHORE_EA + 0x100u;
    jitter(r, i);
    if (kind == 0u) {
        put_value += 4u;
        if (i & 8u) {
            put_be(RSX_PUT_EA, put_value);
            rsx_core_window_written();
        } else {
            vm_write32(RSX_PUT_EA, put_value);
            rsx_core_doorbell();
        }
    } else if (kind == 1u) {
        put_be(word_ea, i ^ 0x5A5A0000u);
        rsx_core_window_written();
    } else if (kind == 2u) {
        if (i & 8u) {
            const u32 label[2] = { word_ea, i };
            rsx_label_retire(label);
        } else {
            put_be(word_ea, i);
            rsx_core_window_written();
        }
    } else if (kind == 3u) {
        put_be(word_ea, i ^ 0xA5A50000u);
        rsx_core_window_written();
    } else if (kind == 4u || kind == 6u) {
        rsx_core_flip_immediate(i);
    } else if (kind == 5u) {
        put_value += 4u;
        rsx_core_fifo_reset(put_value, s.get + 4u);
    }
}
int main(int argc, char** argv)
{
    const u32 rounds = (u32)strtoul(argv[1], 0, 10);
    (void)argc;
    ppu_hle_inject_base = 0x10000u;
    vm_base = (uint8_t*)calloc(1, 0x40000u);
    s.call_pos = RSX_NO_CALL;
    for (u32 i = 0; i < 1000u; i++) rsx_core_doorbell();
    for (u32 i = 0; i < 1000u; i++) rsx_core_window_written();
    if (s.bell) { printf("doorbell bumped with no sleeper: %u\n", s.bell); return 5; }
    u32 r = 0x2545F491u;
    HANDLE t = CreateThread(0, 0, rsx_waiter, (LPVOID)(uintptr_t)rounds, 0, 0);
    for (u32 i = 0; i < rounds; i++) {
        while (__atomic_load_n(&ready, __ATOMIC_SEQ_CST) != i + 1u) __builtin_ia32_pause();
        __atomic_store_n(&go, i + 1u, __ATOMIC_SEQ_CST);
        writer_side(i, &r);
        const DWORD t0 = GetTickCount();
        while (__atomic_load_n(&finished, __ATOMIC_SEQ_CST) != i + 1u) {
            if (GetTickCount() - t0 > 5000u) { printf("lost wake at round %u kind %u\n", i, i % 7u); fflush(stdout); _Exit(3); }
            SwitchToThread();
        }
    }
    WaitForSingleObject(t, INFINITE);
    const u32 notified = rounds - (rounds + 3u) / 7u;
    printf("ok %u notified %u bell %u\n", rounds, notified, s.bell);
    if (!s.bell || s.bell >= notified) { printf("both doorbell paths not exercised\n"); return 6; }
    return 0;
}
"""

def main() -> int:
    cc = shutil.which(os.environ.get("CC", "cc")) or shutil.which("clang")
    if cc is None:
        print("SKIP: no C compiler")
        return 0
    inc = ["-I" + str(ROOT / "libs" / "video"), "-I" + str(ROOT / "include"), "-I" + str(ROOT / "runtime" / "ppu"),
           "-I" + str(ROOT / "runtime" / "memory"), "-I" + str(ROOT / "runtime" / "platform")]
    libs = ["-lsynchronization", "-luser32"] if os.name == "nt" else ["-lpthread", "-lm"]
    with tempfile.TemporaryDirectory() as td:
        drv = Path(td) / "drv.c"
        drv.write_text(DRIVER)
        for opt in ("-O0", "-O2"):
            exe = Path(td) / f"drv{opt}.exe"
            srcs = [str(drv)]
            if os.name != "nt":
                srcs.append(str(ROOT / "runtime/platform/win32_compat.c"))
            r = subprocess.run([cc, "-std=gnu17", opt, "-w", "-march=native", "-fno-strict-aliasing",
                                "-D_CRT_SECURE_NO_WARNINGS", *inc, "-o", str(exe), *srcs, *libs],
                               capture_output=True, text=True)
            if r.returncode:
                print(r.stdout + r.stderr)
                return 1
            out = subprocess.run([str(exe), str(ROUNDS)], capture_output=True, text=True, timeout=600)
            if out.returncode:
                print(f"FAIL {opt}: {out.stdout}{out.stderr}")
                return 1
            print(f"{opt}: {out.stdout.strip()}")
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
