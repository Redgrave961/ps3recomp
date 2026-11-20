#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import random
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
FREGS = list(range(3, 15))
LOOP = [40, 41]
BASE = 2


def ri10(op8, i10, ra, rt): return ((op8 & 0xFF) << 24) | ((i10 & 0x3FF) << 14) | ((ra & 0x7F) << 7) | (rt & 0x7F)
def ri16(op9, i16, rt): return ((op9 & 0x1FF) << 23) | ((i16 & 0xFFFF) << 7) | (rt & 0x7F)
def rr(op11, rb, ra, rt): return ((op11 & 0x7FF) << 21) | ((rb & 0x7F) << 14) | ((ra & 0x7F) << 7) | (rt & 0x7F)
def rrr(op4, rt, rb, ra, rc): return ((op4 & 0xF) << 28) | ((rt & 0x7F) << 21) | ((rb & 0x7F) << 14) | ((ra & 0x7F) << 7) | (rc & 0x7F)


RR = {"fa": 0x2C4, "fs": 0x2C5, "fm": 0x2C6, "xor": 0x241, "and": 0x0C1, "andc": 0x2C1, "or": 0x041,
      "fcgt": 0x2C2, "fceq": 0x3C2, "ceq": 0x3C0}
RI16 = {"il": 0x081, "ilhu": 0x082, "ilh": 0x083, "iohl": 0x0C1}
RRR = {"fma": 0xE, "fms": 0xF, "fnms": 0xD, "selb": 0x8}
RI10 = {"ori": 0x04, "ai": 0x1C}
RI7 = {"shlqbyi": 0x1FF, "rotqbyi": 0x1FC}


def encode(i, pc, labels):
    op = i[0]
    if op in RR:
        return rr(RR[op], i[3], i[2], i[1])
    if op in RRR:
        return rrr(RRR[op], i[1], i[3], i[2], i[4])
    if op in RI10:
        return ri10(RI10[op], i[3], i[2], i[1])
    if op in RI7:
        return rr(RI7[op], i[3], i[2], i[1])
    if op in RI16:
        return ri16(RI16[op], i[2], i[1])
    if op == "ila":
        return ((0x21 << 25) | ((i[2] & 0x3FFFF) << 7) | i[1]) & 0xFFFFFFFF
    if op == "lqd":
        return ri10(0x34, i[3] >> 4, i[2], i[1])
    if op == "stqd":
        return ri10(0x24, i[3] >> 4, i[2], i[1])
    if op in ("brz", "brnz"):
        return ri16(0x040 if op == "brz" else 0x042, (labels[i[2]] - pc) >> 2, i[1])
    if op == "stop":
        return 0
    raise ValueError(op)


def reference(i, labels):
    op = i[0]
    g = lambda r: f"c->gpr[{r}]"
    if op in RR:
        return f"{g(i[1])} = spu_{op}({g(i[2])}, {g(i[3])});"
    if op in ("fma", "fms", "fnms", "selb"):
        return f"{g(i[1])} = spu_{op}({g(i[2])}, {g(i[3])}, {g(i[4])});"
    if op in ("ori", "ai", "shlqbyi", "rotqbyi"):
        return f"{g(i[1])} = spu_{op}({g(i[2])}, {i[3]});"
    if op == "iohl":
        return f"{g(i[1])} = spu_iohl({g(i[1])}, {i[2]});"
    if op in ("il", "ila", "ilh", "ilhu"):
        return f"{g(i[1])} = spu_{op}({i[2]});"
    if op == "lqd":
        return f"{g(i[1])} = spu_ls_ld128(c->ls, {g(i[2])}._u32[0] + {i[3]});"
    if op == "stqd":
        return f"spu_ls_st128(c->ls, {g(i[2])}._u32[0] + {i[3]}, {g(i[1])});"
    if op in ("brz", "brnz"):
        return f"if ({g(i[1])}._u32[0] {'==' if op == 'brz' else '!='} 0) goto {i[2]};"
    if op == "stop":
        return "return;"
    raise ValueError(op)


def block(rng, n, tag):
    out = []
    pick = lambda: rng.choice(FREGS)
    for j in range(n):
        r = rng.random()
        if r < 0.45:
            op = rng.choice(["fa", "fs", "fm", "fma", "fms", "fnms"])
            out.append((op, pick(), pick(), pick()) + ((pick(),) if op in RRR else ()))
        elif r < 0.62:
            out.append((rng.choice(["ori", "ai", "shlqbyi", "rotqbyi"]), pick(), pick(), rng.choice([0] * 6 + [1, 2, 3, 4, 8, 12, 16, 20, -4])))
        elif r < 0.7:
            out.append(("ilhu", pick(), rng.choice([0x7F80, 0xFF80, 0x7FC0, 0x7FFF, 0x0040, 0x8000, 0x3F80])))
        elif r < 0.75:
            op = rng.choice(["il", "ilh", "iohl", "ila"])
            imm = rng.choice([rng.randrange(0x10000), 0x7F80, 0xFF80, 0x8000, 0x7FFF, 0x3F80, 0])
            out.append((op, pick(), rng.randrange(0x40000) if op == "ila" else imm - 0x10000 if op == "il" and imm & 0x8000 else imm))
        elif r < 0.84:
            op = rng.choice(["xor", "and", "andc", "or", "fcgt", "fceq", "ceq", "selb", "selb"])
            out.append((op, pick(), pick(), pick()) + ((pick(),) if op == "selb" else ()))
        elif r < 0.9:
            out.append(("lqd", pick(), BASE, rng.randrange(16) * 16))
        elif r < 0.94:
            out.append(("stqd", pick(), BASE, rng.randrange(16) * 16))
        else:
            skip = f"{tag}s{j}"
            out.append((rng.choice(["brz", "brnz"]), pick(), skip))
            out += block(rng, rng.randint(1, 4), skip)
            out.append(("label", skip))
    return out


def program(rng):
    body = []
    for b in range(rng.randint(1, 4)):
        if rng.random() < 0.5:
            cnt = LOOP[b % 2]
            top = f"L{b}"
            body.append(("il", cnt, rng.randint(1, 4)))
            body.append(("label", top))
            body += block(rng, rng.randint(3, 14), top)
            body.append(("ai", cnt, cnt, -1))
            body.append(("brnz", cnt, top))
        else:
            body += block(rng, rng.randint(3, 14), f"B{b}")
    body.append(("stop",))
    return body


HARNESS = r"""
#include "spu_recomp.h"
#include "spu_helpers.h"
#include "reference.h"
#include <immintrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*);
SPU_THREAD_LOCAL uint32_t g_spu_pch[8];
SPU_THREAD_LOCAL unsigned g_spu_pch_n;
volatile int g_spu_direct = 1;
uint32_t g_spu_depth_guard_limit = 4096;
uint8_t* vm_base;
void spu_register_function(uint32_t a, void (*f)(spu_context*)) { (void)a; (void)f; }
void (*spu_lifted_lookup(const spu_context* c, uint32_t a))(spu_context*) { (void)c; (void)a; return 0; }
void spu_depth_guard(spu_context* c) { (void)c; }
void spu_indirect_branch(spu_context* c) { (void)c; exit(3); }
void spu_drain_call(spu_context* c, uint32_t ret) { (void)c; (void)ret; exit(3); }
u128 spu_rdch(spu_context* c, uint32_t ch) { (void)c; (void)ch; exit(6); }
uint32_t spu_rchcnt(spu_context* c, uint32_t ch) { (void)c; (void)ch; exit(6); }
void spu_wrch(spu_context* c, uint32_t ch, u128 v) { (void)c; (void)ch; (void)v; exit(6); }
void spu_stop(spu_context* c) { (void)c; }
void spu_halt(spu_context* c) { (void)c; exit(4); }

static uint64_t g_s;
static uint32_t rnd(void) { g_s ^= g_s << 13; g_s ^= g_s >> 7; g_s ^= g_s << 17; return (uint32_t)(g_s >> 11); }
static uint32_t special(void)
{
    static const uint32_t k[] = { 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0xFFFFFFFFu, 0x7F800001u, 0x7F7FFFFFu, 0xFF7FFFFFu,
                                  0x00000000u, 0x80000000u, 0x00000001u, 0x807FFFFFu, 0x00800000u, 0x80800000u, 0x3F800000u,
                                  0xBF800000u, 0x7F000000u, 0x00FFFFFFu, 0x4B800000u };
    const uint32_t r = rnd() % 8;
    if (r < 3) return k[rnd() % (sizeof k / sizeof k[0])];
    if (r < 5) return (rnd() & 0x807FFFFFu) | ((0x70u + rnd() % 32u) << 23);
    return rnd();
}

int main(int argc, char** argv)
{
    static spu_context a, b;
    const int runs = atoi(argv[1]);
    unsigned bad = 0;
    for (int run = 0; run < runs; run++) {
        g_s = 0x9E3779B97F4A7C15ull + (uint64_t)run * 0x632BE59BD9B4E019ull;
        spu_context_init(&a, 0);
        for (int r = 0; r < 128; r++) for (int k = 0; k < 4; k++) a.gpr[r]._u32[k] = special();
        a.gpr[2]._u32[0] = 0x1000;
        for (int r = 40; r < 42; r++) a.gpr[r]._u32[0] = 1;
        for (uint32_t i = 0x1000; i < 0x1100; i += 4) spu_ls_write32(&a, i, special());
        a.status = SPU_STATUS_RUNNING;
        a.steps = 1;
        memcpy(&b, &a, sizeof a);
        b.ls = b.ls_store;
        const unsigned csr = _mm_getcsr();
        _mm_setcsr(0xFFC0u);
        spu_func_00000000(&a);
        while (g_spu_trampoline_fn) { void (*t)(spu_context*) = g_spu_trampoline_fn; g_spu_trampoline_fn = 0; t(&a); }
        reference_run(&b);
        _mm_setcsr(csr);
        if (memcmp(a.gpr, b.gpr, sizeof a.gpr) || memcmp(a.ls_store + 0x1000, b.ls_store + 0x1000, 0x100)) {
            if (bad++ < 4)
                for (int r = 0; r < 128; r++)
                    if (memcmp(&a.gpr[r], &b.gpr[r], 16))
                        printf("run %d r%d lifted %08X %08X %08X %08X reference %08X %08X %08X %08X\n", run, r,
                               a.gpr[r]._u32[0], a.gpr[r]._u32[1], a.gpr[r]._u32[2], a.gpr[r]._u32[3],
                               b.gpr[r]._u32[0], b.gpr[r]._u32[1], b.gpr[r]._u32[2], b.gpr[r]._u32[3]);
        }
    }
    printf("%u\n", bad);
    (void)argc;
    return bad != 0;
}
"""


def find_cc(explicit):
    for c in (explicit, os.environ.get("CC"), "clang", "gcc"):
        if c and shutil.which(c):
            return shutil.which(c)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--programs", type=int, default=60)
    ap.add_argument("--runs", type=int, default=400)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--cc")
    ap.add_argument("--keep")
    args = ap.parse_args()
    cc = find_cc(args.cc)
    if not cc:
        print("SKIP: no C compiler")
        return 0
    rng = random.Random(args.seed)
    work = args.keep or tempfile.mkdtemp(prefix="spuclean_")
    os.makedirs(work, exist_ok=True)
    harness = os.path.join(work, "harness.c")
    with open(harness, "w") as f:
        f.write(HARNESS)
    failures = skipped = 0
    for p in range(args.programs):
        prog = program(rng)
        labels, pc = {}, 0
        for i in prog:
            if i[0] == "label":
                labels[i[1]] = pc
            else:
                pc += 4
        words, pc = [], 0
        for i in prog:
            if i[0] != "label":
                words.append(encode(i, pc, labels))
                pc += 4
        pdir = os.path.join(work, f"p{p}")
        os.makedirs(pdir, exist_ok=True)
        with open(os.path.join(pdir, "reference.h"), "w") as f:
            f.write("static void reference_run(spu_context* c) {\n")
            f.write("\n".join(f"{i[1]}:;" if i[0] == "label" else "    " + reference(i, labels) for i in prog))
            f.write("\n}\n")
        code = os.path.join(pdir, "code.bin")
        with open(code, "wb") as f:
            f.write(b"".join(w.to_bytes(4, "big") for w in words))
        end = len(words) * 4
        for merge in (False, True):
            starts = sorted({0} | ({rng.randrange(1, end // 4) * 4 for _ in range(rng.randint(1, 3))} if merge and end > 8 else set()))
            functions = os.path.join(pdir, "functions.json")
            with open(functions, "w") as f:
                json.dump([{"start": s, "end": starts[k + 1] if k + 1 < len(starts) else end} for k, s in enumerate(starts)], f)
            out = os.path.join(pdir, "m" if merge else "s")
            cmd = [sys.executable, os.path.join(HERE, "spu_lifter.py"), code, "--base", "0", "--functions", functions, "-o", out]
            if merge:
                cmd.append("--merge-chunks")
            subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            with open(os.path.join(out, "spu_recomp.c")) as f:
                if "_k(" not in f.read():
                    skipped += 1
            exe = os.path.join(pdir, ("m" if merge else "s") + ".exe")
            b = subprocess.run([cc, "-std=gnu17", "-O2", "-march=x86-64-v3", "-w", "-I", out, "-I", pdir,
                                "-I", os.path.join(REPO, "include"), "-I", os.path.join(REPO, "runtime"),
                                "-I", os.path.join(REPO, "runtime", "spu"), os.path.join(out, "spu_recomp.c"), harness,
                                "-o", exe], capture_output=True, text=True)
            if b.returncode:
                raise RuntimeError(b.stderr[-3000:])
            r = subprocess.run([exe, str(args.runs)], capture_output=True, text=True, timeout=300)
            if r.returncode:
                failures += 1
                print(f"program {p} seed {args.seed} merge={merge}: rc={r.returncode}\n{r.stdout[-1500:]}")
    print(f"{args.programs} programs x 2 variants x {args.runs} inputs, {2 * args.programs - skipped} with elided clamps, "
          f"{failures} mismatches")
    if not args.keep:
        shutil.rmtree(work, ignore_errors=True)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
