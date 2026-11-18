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

from spu_disasm import spu_decode
from spu_lifter import LiftedFunction, SPULifter

RESERVED = {0, 1, 60, 70}
POOL = [r for r in range(2, 128) if r not in RESERVED and not 60 <= r < 80]
RRR = {"selb", "shufb", "mpya", "fnms", "fma", "fms"}
WIDE = {"il", "ila", "ilh", "ilhu", "iohl", "fsmbi", "lqa", "stqa", "lqr", "stqr"}
STORES = {"stqd", "stqa", "stqx", "stqr"}
SKIP = {"hbr", "hbra", "hbrr", "nop", "lnop", "sync", "dsync", "syncc", "fscrwr", "mtspr",
        "heq", "heqi", "hgt", "hgti", "hlgt", "hlgti"}
REJECT = ("TODO", "return", "goto", "SPU_", "spu_rdch", "spu_wrch", "spu_rchcnt", "spu_stop",
          "spu_halt", "fprintf", "ctx->pc", "ctx->stop", "ctx->mfc", "/*")


def ri16(op9, i16, rt): return ((op9 & 0x1FF) << 23) | ((i16 & 0xFFFF) << 7) | (rt & 0x7F)
def ri18(op7, i18, rt): return ((op7 & 0x7F) << 25) | ((i18 & 0x3FFFF) << 7) | (rt & 0x7F)
def ri10(op8, i10, ra, rt): return ((op8 & 0xFF) << 24) | ((i10 & 0x3FF) << 14) | ((ra & 0x7F) << 7) | (rt & 0x7F)
def rr(op11, rb, ra, rt): return ((op11 & 0x7FF) << 21) | ((rb & 0x7F) << 14) | ((ra & 0x7F) << 7) | (rt & 0x7F)


_probe = SPULifter()
_probe.promote = False


def compute(rng, pc):
    while True:
        d = spu_decode(rng.getrandbits(32), pc)
        mn = d.mnemonic
        if mn in SKIP:
            continue
        text = _probe._translate(d, LiftedFunction())
        if not text or any(t in text for t in REJECT):
            continue
        w = d.raw
        pick = lambda: rng.choice(POOL)
        if mn in RRR:
            w = (w & ~((0x7F << 21) | (0x7F << 14) | (0x7F << 7) | 0x7F)) | \
                (pick() << 21) | (pick() << 14) | (pick() << 7) | pick()
        elif mn in WIDE:
            w = (w & ~0x7F) | pick()
        elif d.operands.count("$r") >= 3:
            w = (w & ~0x1FFFFF) | (pick() << 14) | (pick() << 7) | pick()
        else:
            w = (w & ~0x3FFF) | (pick() << 7) | pick()
        e = spu_decode(w, pc)
        if e.mnemonic != mn:
            continue
        return w


def program(rng):
    code = []
    patches = []
    leaves = rng.randint(2, 4)

    def emit(w):
        code.append(w)
        return (len(code) - 1) * 4

    emit(ri16(0x081, rng.randint(2, 5), 60))
    loop_top = len(code) * 4
    blocks = rng.randint(3, 7)
    block_pcs = []
    pending = []
    jumps = []
    ret_targets = []
    ret_loop = rng.randrange(blocks)
    for b in range(blocks):
        block_pcs.append(len(code) * 4)
        if b == ret_loop:
            emit(ri16(0x081, rng.randint(2, 3), 61))
            again = len(code) * 4
            ret_targets.append(again)
            for _ in range(rng.randint(1, 5)):
                emit(compute(rng, len(code) * 4))
            emit(ri10(0x1C, -1, 61, 61))
            emit(ri16(0x040, 4, 61))
            emit(ri18(0x21, again, 62))
            emit(ri10(0x04, 0, 62, 0))
            emit(rr(0x1A8, 0, 0, 0))
            ret_targets.append(len(code) * 4)
            continue
        for _ in range(rng.randint(2, 10)):
            emit(compute(rng, len(code) * 4))
        roll = rng.random()
        if roll < 0.2:
            emit(rr(0x10D, 0, 28, rng.choice(POOL)))
        elif roll < 0.35:
            emit(rr(0x00D, 0, 29, rng.choice(POOL)))
        elif roll < 0.45:
            emit(rr(0x00F, 0, 29, rng.choice(POOL)))
        elif roll < 0.55:
            emit(0x110)
        if rng.random() < 0.6:
            leaf = rng.randrange(leaves)
            if rng.random() < 0.5:
                patches.append(("call", emit(0), leaf))
            else:
                patches.append(("ila", emit(0), leaf))
                emit(rr(0x1A9, 0, 70, 0))
        if b + 2 <= blocks and rng.random() < 0.6:
            pending.append((emit(0), rng.choice((0x040, 0x042, 0x044, 0x046)), rng.choice(POOL), b + 2))
        elif b + 2 <= blocks and rng.random() < 0.5:
            jumps.append((emit(0), b + 2))
            if rng.random() < 0.5:
                emit(rr(0x1A8, 0, 71, 0))
            else:
                emit(rr(rng.choice((0x128, 0x129, 0x12A, 0x12B)), 0, 71, rng.choice(POOL)))
    block_pcs.append(len(code) * 4)
    emit(ri10(0x1C, -1, 60, 60))
    emit(ri16(0x042, (loop_top - len(code) * 4) >> 2, 60))
    end_pc = emit(0x0)
    targets = {loop_top, *ret_targets}
    for at, op, reg, tb in pending:
        tgt = block_pcs[tb] if tb < len(block_pcs) else end_pc
        targets.add(tgt)
        code[at // 4] = ri16(op, (tgt - at) >> 2, reg)
    for at, tb in jumps:
        tgt = block_pcs[tb] if tb < len(block_pcs) else end_pc
        targets.add(tgt)
        code[at // 4] = ri18(0x21, tgt, 71)
    symbols = [{"name": "main", "addr": 0, "size": len(code) * 4}]
    leaf_pcs = []
    for j in range(leaves):
        start = len(code) * 4
        leaf_pcs.append(start)
        for _ in range(rng.randint(1, 8)):
            emit(compute(rng, len(code) * 4))
        if rng.random() < 0.6:
            emit(rr(rng.choice((0x128, 0x129, 0x12A, 0x12B)), 0, 0, rng.choice(POOL)))
            for _ in range(rng.randint(1, 6)):
                emit(compute(rng, len(code) * 4))
        emit(rr(0x1A8, 0, 0, 0))
        symbols.append({"name": f"leaf{j}", "addr": start, "size": len(code) * 4 - start})
    for kind, at, leaf in patches:
        if kind == "call":
            code[at // 4] = ri16(0x066, (leaf_pcs[leaf] - at) >> 2, 0)
        else:
            code[at // 4] = ri18(0x21, leaf_pcs[leaf], 70)
    main_end = symbols[0]["size"]
    starts = {0, *leaf_pcs} | {a for a in targets if 0 < a < main_end}
    starts |= {rng.randrange(1, main_end // 4) * 4 for _ in range(rng.randint(0, 3))}
    order = sorted(starts)
    bounds = [(s, order[i + 1] if i + 1 < len(order) else len(code) * 4) for i, s in enumerate(order)]
    return b"".join(w.to_bytes(4, "big") for w in code), bounds


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
unsigned g_spu_stack_reset_count;
uint64_t g_spu_stack_reset_hint[SPU_LS_SIZE / 256];
uint32_t g_spu_depth_guard_limit = 4096;
uint8_t* vm_base;

static struct { uint32_t a; void (*f)(spu_context*); } g_tab[8192];
static int g_ntab;
static char g_log[1 << 16];
static size_t g_logn;
static uint32_t g_tick = 0x12345678u;

static void note(const char* s) { size_t n = strlen(s); if (g_logn + n < sizeof g_log) { memcpy(g_log + g_logn, s, n); g_logn += n; } }
static uint32_t next(void) { g_tick = g_tick * 1664525u + 1013904223u; return g_tick; }

void spu_register_function(uint32_t a, void (*f)(spu_context*)) { g_tab[g_ntab].a = a; g_tab[g_ntab++].f = f; }
void (*spu_lifted_lookup(const spu_context* c, uint32_t a))(spu_context*)
{
    (void)c;
    for (int i = 0; i < g_ntab; i++) if (g_tab[i].a == (a & SPU_LS_MASK)) return g_tab[i].f;
    return 0;
}
void spu_depth_guard(spu_context* c) { if (c->host_depth > 512) { printf("depth\n"); exit(5); } }
static unsigned g_dispatches;
void spu_indirect_branch(spu_context* c)
{
    g_dispatches++;
    void (*f)(spu_context*) = spu_lifted_lookup(c, c->pc);
    if (!f) { printf("unlifted pc %05X\n", c->pc); exit(3); }
    f(c);
}
void spu_drain_call(spu_context* c, uint32_t ret)
{
    (void)ret;
    while (g_spu_trampoline_fn) { void (*f)(spu_context*) = g_spu_trampoline_fn; g_spu_trampoline_fn = 0; f(c); }
}
u128 spu_rdch(spu_context* c, uint32_t ch)
{
    char b[64]; uint32_t v = next(); snprintf(b, sizeof b, "rd%u:%08X ", ch, v); note(b);
    u128 r; memset(&r, 0, sizeof r); r._u32[0] = v; r._u32[1] = c->gpr[5]._u32[1]; return r;
}
uint32_t spu_rchcnt(spu_context* c, uint32_t ch)
{
    char b[64]; uint32_t v = next() & 3u; snprintf(b, sizeof b, "cnt%u:%u/%08X ", ch, v, c->gpr[7]._u32[2]); note(b); return v;
}
void spu_wrch(spu_context* c, uint32_t ch, u128 v)
{
    char b[96]; snprintf(b, sizeof b, "wr%u:%08X%08X%08X%08X/%08X ", ch, v._u32[0], v._u32[1], v._u32[2], v._u32[3], c->gpr[9]._u32[3]); note(b);
}
void spu_stop(spu_context* c)
{
    char b[64]; snprintf(b, sizeof b, "stop%X:%08X ", c->stop_code, c->gpr[11]._u32[0]); note(b);
    if (c->stop_code == 0x110u) {
        for (int r = 2; r < 128; r += 7) c->gpr[r]._u32[r & 3] ^= next();
        c->status = SPU_STATUS_RUNNING;
    }
}
void spu_halt(spu_context* c) { (void)c; printf("halt\n"); exit(4); }

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
    spu_recomp_register();
#ifdef SPU_TEST_ALIAS_HINTS
    for (int i = 0; i < g_ntab; i++) {
        const uint32_t alias = (g_tab[i].a + 0x1000u) & SPU_LS_MASK;
        int taken = 0;
        for (int j = 0; j < g_ntab; j++) taken |= g_tab[j].a == alias;
        if (!taken) g_spu_stack_reset_hint[alias >> 8] |= 1ull << ((alias >> 2) & 63u);
    }
    g_spu_stack_reset_count = 1;
#endif
    spu_func_00000000(&ctx);
    while (g_spu_trampoline_fn) { void (*f)(spu_context*) = g_spu_trampoline_fn; g_spu_trampoline_fn = 0; f(&ctx); }
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; i < SPU_LS_SIZE; i++) h = (h ^ ctx.ls[i]) * 1099511628211ull;
    printf("status=%X stop=%X pc=%05X depth=%d ls=%016llX\n", ctx.status, ctx.stop_code, ctx.pc & SPU_LS_MASK, ctx.host_depth, (unsigned long long)h);
    for (int r = 0; r < 128; r++)
        printf("r%d %08X%08X%08X%08X\n", r, ctx.gpr[r]._u32[0], ctx.gpr[r]._u32[1], ctx.gpr[r]._u32[2], ctx.gpr[r]._u32[3]);
    fwrite(g_log, 1, g_logn, stdout);
    printf("\n");
    fprintf(stderr, "%u\n", g_dispatches);
    return 0;
}
"""


def find_cc(explicit):
    for c in (explicit, os.environ.get("CC"), "clang", "gcc"):
        if c and shutil.which(c):
            return shutil.which(c)
    return None


def lift(image, functions, out, promote, merge):
    cmd = [sys.executable, os.path.join(HERE, "spu_lifter.py"), image, "--base", "0",
           "--functions", functions, "--return-entries", "-o", out]
    if promote:
        cmd += ["--promote-regs", str(promote)]
    else:
        cmd.append("--no-promote")
    if merge:
        cmd.append("--merge-chunks")
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def run_variant(cc, work, image, functions, harness, promote, merge, tag, defines=()):
    out = os.path.join(work, tag)
    lift(image, functions, out, promote, merge)
    exe = os.path.join(work, tag + (".exe" if os.name == "nt" else ""))
    cmd = [cc, "-std=gnu17", "-O2", "-msse4.1", "-w", "-I", out, "-I", os.path.join(REPO, "include"),
           "-I", os.path.join(REPO, "runtime"), "-I", os.path.join(REPO, "runtime", "spu"),
           *defines, os.path.join(out, "spu_recomp.c"), harness, "-o", exe]
    b = subprocess.run(cmd, capture_output=True, text=True)
    if b.returncode:
        raise RuntimeError(f"{tag}: build failed\n{b.stderr[-3000:]}")
    r = subprocess.run([exe], capture_output=True, text=True, timeout=60)
    with open(os.path.join(out, "spu_recomp.c")) as f:
        src = f.read()
    dispatches = int(r.stderr.split()[-1]) if r.stderr.split() and r.stderr.split()[-1].isdigit() else 0
    return r.returncode, r.stdout, src, dispatches


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--programs", type=int, default=40)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--cc")
    ap.add_argument("--keep")
    ap.add_argument("--promote-regs", type=int, default=16)
    args = ap.parse_args()
    cc = find_cc(args.cc)
    if not cc:
        print("SKIP: no C compiler")
        return 0
    rng = random.Random(args.seed)
    work = args.keep or tempfile.mkdtemp(prefix="spuprom_")
    os.makedirs(work, exist_ok=True)
    harness = os.path.join(work, "harness.c")
    with open(harness, "w") as f:
        f.write(HARNESS)
    failures = 0
    promoted = 0
    dispatches = {}
    for p in range(args.programs):
        image, bounds = program(rng)
        pdir = os.path.join(work, f"p{p}")
        os.makedirs(pdir, exist_ok=True)
        code = os.path.join(pdir, "code.bin")
        with open(code, "wb") as f:
            f.write(image)
        functions = os.path.join(pdir, "functions.json")
        with open(functions, "w") as f:
            json.dump([{"start": s, "end": e} for s, e in bounds], f)
        results = {}
        for promote in (0, args.promote_regs):
            for merge in (False, True):
                tag = f"{'p' if promote else 'n'}{'m' if merge else 's'}"
                results[tag] = run_variant(cc, pdir, code, functions, harness, promote, merge, tag)
        alias = run_variant(cc, pdir, code, functions, harness, args.promote_regs, True, "pa", ("-DSPU_TEST_ALIAS_HINTS",))
        ref = results["ns"]
        if ref[0] != 0:
            print(f"program {p}: reference exited {ref[0]}: {ref[1][:200]}")
            failures += 1
            continue
        promoted += results["ps"][2].count(" sr") > 0
        dispatches["pa"] = dispatches.get("pa", 0) + alias[3]
        if alias[:2] != results["pm"][:2] or alias[3] != results["pm"][3]:
            failures += 1
            print(f"program {p} seed {args.seed}: aliased stack-reset hints changed the run ({alias[3]} vs {results['pm'][3]} dispatches)")
        for tag, res in results.items():
            dispatches[tag] = dispatches.get(tag, 0) + res[3]
            same = results["n" + tag[1]]
            strip = lambda s: re.sub(r" pc=\w+", "", s)
            for want, got in ((same[1], res[1]), (strip(ref[1]), strip(res[1]))):
                if res[0] != ref[0] or want != got:
                    failures += 1
                    diff = [f"  {a!r} != {b!r}" for a, b in zip(want.splitlines(), got.splitlines()) if a != b][:6]
                    print(f"program {p} seed {args.seed}: variant {tag} differs\n" + "\n".join(diff))
                    break
    print(f"{args.programs} programs x 5 variants, {promoted} with promoted registers, {failures} mismatches")
    print("generic dispatches per variant: " + ", ".join(f"{k}={v}" for k, v in sorted(dispatches.items())))
    if not args.keep:
        shutil.rmtree(work, ignore_errors=True)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
