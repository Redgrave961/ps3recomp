#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import test_spu_register_promotion as gen

HARNESS = r"""
#include "spu_recomp.h"
#include "spu_helpers.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*);
SPU_THREAD_LOCAL uint32_t g_spu_pch[8];
SPU_THREAD_LOCAL unsigned g_spu_pch_n;
volatile int g_spu_direct = 1;
uint32_t g_spu_depth_guard_limit = 4096;
uint8_t* vm_base;

static struct { uint32_t a; void (*f)(spu_context*); } g_tab[8192];
static int g_ntab;
static uint32_t g_tick = 0x2545F491u;
static unsigned g_events;

static uint32_t next(void) { g_tick = g_tick * 1664525u + 1013904223u; return g_tick; }
static void snap(spu_context* c, const char* what, uint32_t a)
{
    uint64_t h = 1469598103934665603ull;
    const uint8_t* g = (const uint8_t*)c->gpr;
    for (size_t i = 0; i < sizeof c->gpr; i++) h = (h ^ g[i]) * 1099511628211ull;
    uint64_t l = 1469598103934665603ull;
    for (uint32_t i = 0; i < SPU_LS_SIZE; i++) l = (l ^ c->ls[i]) * 1099511628211ull;
    printf("%s %X pc=%05X gpr=%016llX ls=%016llX\n", what, a, c->pc & SPU_LS_MASK, (unsigned long long)h, (unsigned long long)l);
    c->event_status = next() & 1u;
    if (++g_events > 20000) { printf("runaway\n"); exit(8); }
}
void spu_register_function(uint32_t a, void (*f)(spu_context*)) { g_tab[g_ntab].a = a; g_tab[g_ntab++].f = f; }
void (*spu_lifted_lookup(const spu_context* c, uint32_t a))(spu_context*)
{
    (void)c;
    for (int i = 0; i < g_ntab; i++) if (g_tab[i].a == (a & SPU_LS_MASK)) return g_tab[i].f;
    return 0;
}
void spu_depth_guard(spu_context* c) { if (c->host_depth > 512) { printf("depth\n"); exit(5); } }
void spu_indirect_branch(spu_context* c)
{
    snap(c, "branch", c->pc);
    void (*f)(spu_context*) = spu_lifted_lookup(c, c->pc);
    if (!f) { printf("unlifted pc %05X\n", c->pc); exit(3); }
    f(c);
}
static void hop(spu_context* c)
{
    while (g_spu_trampoline_fn) {
        void (*f)(spu_context*) = g_spu_trampoline_fn;
        g_spu_trampoline_fn = 0;
        snap(c, "hop", 0);
        f(c);
    }
}
void spu_drain_call(spu_context* c, uint32_t ret) { (void)ret; hop(c); }
u128 spu_rdch(spu_context* c, uint32_t ch)
{
    snap(c, "rdch", ch);
    u128 r; memset(&r, 0, sizeof r); r._u32[0] = next(); r._u32[1] = next(); return r;
}
uint32_t spu_rchcnt(spu_context* c, uint32_t ch) { snap(c, "rchcnt", ch); return next() & 3u; }
void spu_wrch(spu_context* c, uint32_t ch, u128 v) { snap(c, "wrch", ch ^ v._u32[0] ^ v._u32[3]); }
void spu_stop(spu_context* c)
{
    snap(c, "stop", c->stop_code);
    if (c->stop_code == 0x110u) {
        for (int r = 2; r < 128; r += 5) c->gpr[r]._u32[r & 3] ^= next();
        c->status = SPU_STATUS_RUNNING;
    }
}
void spu_halt(spu_context* c) { snap(c, "halt", 0); exit(4); }

int main(void)
{
    static spu_context ctx;
    spu_context_init(&ctx, 0);
    uint32_t s = 0x9E3779B9u;
    for (int r = 0; r < 128; r++)
        for (int k = 0; k < 4; k++) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; ctx.gpr[r]._u32[k] = s; }
    for (uint32_t i = 0x20000; i < SPU_LS_SIZE; i++) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; ctx.ls[i] = (uint8_t)s; }
    ctx.status = SPU_STATUS_RUNNING;
    ctx.steps = 1;
    ctx.int_enable = 1;
    ctx.event_mask = 1;
    spu_recomp_register();
    spu_func_00000000(&ctx);
    hop(&ctx);
    snap(&ctx, "end", ctx.status);
    return 0;
}
"""

LIFT = ("import sys; sys.path.insert(0, sys.argv[1]); import spu_lifter as m; L = m.SPULifter; i = L.__init__; "
        "p = int(sys.argv[2]); L.__init__ = lambda s, *a, **k: (i(s, *a, **k), setattr(s, 'promote_regs', p))[0]; "
        "L._locals = L._locals if sys.argv[3] == 'live' else lambda *a: None; sys.argv = sys.argv[3:]; m.main()")


def dense(rng):
    code, starts = [], {0}
    for _ in range(rng.randint(4, 9)):
        if rng.random() < 0.5:
            starts.add(len(code) * 4)
        loop = rng.random() < 0.4
        if loop:
            code.append(gen.ri16(0x081, rng.randint(1, 3), 60))
            top = len(code)
        items = [rng.random() for _ in range(rng.randint(3, 14))]
        for i, r in enumerate(items):
            pc = len(code) * 4
            if r < 0.12:
                n = rng.randint(1, len(items) - i)
                code.append(gen.ri16(rng.choice((0x040, 0x042)), n, rng.choice(gen.POOL)))
            elif r < 0.17:
                code.append(gen.rr(0x10D, 0, 28, rng.choice(gen.POOL)))
            elif r < 0.21:
                code.append(gen.rr(0x00D, 0, 29, rng.choice(gen.POOL)))
            elif r < 0.25:
                code.append(0x110)
            else:
                code.append(gen.compute(rng, pc))
        if loop:
            code.append(gen.ri10(0x1C, -1, 60, 60))
            code.append(gen.ri16(0x042, top - len(code), 60))
    code.append(0)
    order = sorted(starts) + [len(code) * 4]
    return b"".join(w.to_bytes(4, "big") for w in code), list(zip(order, order[1:]))


def find_cc(explicit):
    for c in (explicit, os.environ.get("CC"), "clang", "gcc"):
        if c and shutil.which(c):
            return shutil.which(c)
    return None


def run_variant(cc, pdir, code, functions, harness, live, merge, promote):
    tag = f"{'l' if live else 'r'}{'m' if merge else 's'}"
    out = os.path.join(pdir, tag)
    cmd = [sys.executable, "-c", LIFT, HERE, str(promote), "live" if live else "ref",
           code, "--base", "0", "--functions", functions, "--return-entries", "-o", out]
    if merge:
        cmd.append("--merge-chunks")
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    exe = os.path.join(pdir, tag + (".exe" if os.name == "nt" else ""))
    b = subprocess.run([cc, "-std=gnu17", "-O2", "-msse4.1", "-w", "-I", out, "-I", os.path.join(REPO, "include"),
                        "-I", os.path.join(REPO, "runtime"), "-I", os.path.join(REPO, "runtime", "spu"),
                        os.path.join(out, "spu_recomp.c"), harness, "-o", exe], capture_output=True, text=True)
    if b.returncode:
        raise RuntimeError(f"{tag}: build failed\n{b.stderr[-3000:]}")
    with open(os.path.join(out, "spu_recomp.c")) as f:
        src = f.read()
    try:
        r = subprocess.run([exe], capture_output=True, text=True, timeout=60)
    except subprocess.TimeoutExpired:
        return -1, "timeout", src
    return r.returncode, r.stdout, src


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--programs", type=int, default=60)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--cc")
    ap.add_argument("--keep")
    args = ap.parse_args()
    cc = find_cc(args.cc)
    if not cc:
        print("SKIP: no C compiler")
        return 0
    rng = random.Random(args.seed)
    work = args.keep or tempfile.mkdtemp(prefix="spulive_")
    os.makedirs(work, exist_ok=True)
    harness = os.path.join(work, "harness.c")
    with open(harness, "w") as f:
        f.write(HARNESS)
    failures = locals_ = events = 0
    for p in range(args.programs):
        if p % 2:
            gen.POOL = list(range(2, rng.randint(6, 14)))
            image, bounds = dense(rng)
        else:
            gen.POOL = [r for r in range(2, 128) if r not in gen.RESERVED and not 60 <= r < 80][:rng.randint(20, 60)]
            image, bounds = gen.program(rng)
            cuts = {rng.randrange(1, len(image) // 4) * 4 for _ in range(rng.randint(0, 4))}
            bounds = [(a, b) for s, e in bounds for a, b in zip([s] + sorted(c for c in cuts if s < c < e),
                                                                sorted(c for c in cuts if s < c < e) + [e])]
        pdir = os.path.join(work, f"p{p}")
        os.makedirs(pdir, exist_ok=True)
        code = os.path.join(pdir, "code.bin")
        with open(code, "wb") as f:
            f.write(image)
        functions = os.path.join(pdir, "functions.json")
        with open(functions, "w") as f:
            json.dump([{"start": s, "end": e} for s, e in bounds], f)
        promote = rng.choice((2, 4, 8, 16))
        for merge in (False, True):
            ref = run_variant(cc, pdir, code, functions, harness, False, merge, promote)
            got = run_variant(cc, pdir, code, functions, harness, True, merge, promote)
            locals_ += len(re.findall(r"\btr\d+ = ", got[2]))
            events += ref[1].count("\n")
            if ref[0] not in (0, 4) or "unlifted" in ref[1] or "runaway" in ref[1]:
                failures += 1
                print(f"program {p} seed {args.seed} merge={merge}: reference exited {ref[0]}: {ref[1][-300:]}")
            elif got[:2] != ref[:2]:
                failures += 1
                diff = [f"  {a!r} != {b!r}" for a, b in zip(ref[1].splitlines(), got[1].splitlines()) if a != b][:6]
                print(f"program {p} seed {args.seed} merge={merge}: rc {ref[0]}/{got[0]}\n" + "\n".join(diff))
    print(f"{args.programs} programs x 2 variants, {events} observed states, "
          f"{locals_} register writes kept in locals, {failures} mismatches")
    if not args.keep:
        shutil.rmtree(work, ignore_errors=True)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
