#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
LS = 0x40000
M32 = 0xFFFFFFFF


def ri10(op8, i10, ra, rt): return ((op8 & 0xFF) << 24) | ((i10 & 0x3FF) << 14) | ((ra & 0x7F) << 7) | (rt & 0x7F)
def ri16(op9, i16, rt): return ((op9 & 0x1FF) << 23) | ((i16 & 0xFFFF) << 7) | (rt & 0x7F)
def rr(op11, rb, ra, rt): return ((op11 & 0x7FF) << 21) | ((rb & 0x7F) << 14) | ((ra & 0x7F) << 7) | (rt & 0x7F)
def rrr(op4, rt, rb, ra, rc): return ((op4 & 0xF) << 28) | ((rt & 0x7F) << 21) | ((rb & 0x7F) << 14) | ((ra & 0x7F) << 7) | (rc & 0x7F)


def encode(i, pc, labels):
    op = i[0]
    if op == "ai":
        return ri10(0x1C, i[3], i[2], i[1])
    if op == "lqd":
        return ri10(0x34, i[3] >> 4, i[2], i[1])
    if op == "stqd":
        return ri10(0x24, i[3] >> 4, i[2], i[1])
    if op == "cbd":
        return rr(0x1F4, i[3], i[2], i[1])
    if op == "rotqby":
        return rr(0x1DC, i[3], i[2], i[1])
    if op == "ceq":
        return rr(0x3C0, i[3], i[2], i[1])
    if op == "shufb":
        return rrr(0xB, i[1], i[3], i[2], i[4])
    if op in ("brz", "brnz"):
        return ri16(0x040 if op == "brz" else 0x042, (labels[i[2]] - pc) >> 2, i[1])
    if op == "lnop":
        return 0x00200000
    if op == "stop":
        return 0
    raise ValueError(op)


def qbytes(v):
    return b"".join(struct.pack(">I", x) for x in v)


def qwords(b):
    return list(struct.unpack(">4I", b))


def run_model(prog, regs, ls):
    regs = [list(r) for r in regs]
    ls = bytearray(ls)
    labels = {i[1]: k for k, i in enumerate(prog) if i[0] == "label"}
    k = 0
    while True:
        i = prog[k]
        k += 1
        op = i[0]
        if op == "label" or op == "lnop":
            continue
        if op == "stop":
            return regs, ls
        if op == "ai":
            regs[i[1]] = [(x + i[3]) & M32 for x in regs[i[2]]]
        elif op == "lqd":
            a = (regs[i[2]][0] + i[3]) & 0x3FFF0
            regs[i[1]] = qwords(ls[a:a + 16])
        elif op == "stqd":
            a = (regs[i[2]][0] + i[3]) & 0x3FFF0
            ls[a:a + 16] = qbytes(regs[i[1]])
        elif op == "cbd":
            c = bytearray(range(0x10, 0x20))
            c[(regs[i[2]][0] + i[3]) & 15] = 3
            regs[i[1]] = qwords(bytes(c))
        elif op == "rotqby":
            b = qbytes(regs[i[2]])
            s = regs[i[3]][0] & 15
            regs[i[1]] = qwords(b[s:] + b[:s])
        elif op == "ceq":
            regs[i[1]] = [M32 if x == y else 0 for x, y in zip(regs[i[2]], regs[i[3]])]
        elif op == "shufb":
            cat = qbytes(regs[i[2]]) + qbytes(regs[i[3]])
            out = bytearray()
            for c in qbytes(regs[i[4]]):
                out.append(cat[c & 0x1F] if c < 0x80 else 0 if c < 0xC0 else 0xFF if c < 0xE0 else 0x80)
            regs[i[1]] = qwords(bytes(out))
        elif op in ("brz", "brnz"):
            if (regs[i[1]][0] == 0) == (op == "brz"):
                k = labels[i[2]]
        else:
            raise ValueError(op)


def copy_loop(rng, tag, regs, used, positive):
    pool = [r for r in range(2, 128) if r not in used]
    rng.shuffle(pool)
    rs, rd_, rn, q1, k1, b, q2, m, v, rc = pool[:10]
    used.update(pool[:10])
    n = rng.choice([1, 2, 3, 4, 5, 7, 9, 16, 17, 31, 64, rng.randint(1, 300), rng.randint(1, 3000)])
    near = rng.random()
    if near < 0.15:
        s = LS - rng.randint(0, 40)
        d = rng.randrange(LS)
    elif near < 0.3:
        d = LS - rng.randint(0, 40)
        s = rng.randrange(LS)
    elif near < 0.8:
        s = rng.randrange(LS)
        d = (s + rng.randint(-24, 24)) % LS
    else:
        s, d = rng.randrange(LS), rng.randrange(LS)
    s_hi = rng.getrandbits(32) & ~(LS - 1) if rng.random() < 0.3 else 0
    inc_s = rng.randint(0, 5)
    inc_d_after = rng.random() < 0.8
    form = rng.choice(["counter", "counter_up", "ceq_after", "ceq_before"])
    body = []
    src_after = lambda pos: 1 if pos >= inc_s else 0
    order = ["k1", "q1", "q2", "m", "rot", "shuf", "st"]
    rng.shuffle(order)
    deps = {"rot": {"k1", "q1"}, "shuf": {"rot", "q2", "m"}, "st": {"shuf"}}
    seq = []
    while order:
        for o in order:
            if deps.get(o, set()) <= set(seq):
                seq.append(o)
                order.remove(o)
                break
    inc_s = min(inc_s, len(seq))
    q1_off = src_after(seq.index("q1"))
    k1_eff = 13 + q1_off + (0 if positive else rng.choice([1, 2, 15]))
    k1_imm = k1_eff - src_after(seq.index("k1"))
    for pos, o in enumerate(seq):
        if pos == inc_s:
            body.append(("ai", rs, rs, 1))
        if o == "k1":
            body.append(("ai", k1, rs, k1_imm))
        elif o == "q1":
            body.append(("lqd", q1, rs, 0))
        elif o == "q2":
            body.append(("lqd", q2, rd_, 0))
        elif o == "m":
            body.append(("cbd", m, rd_, 0))
        elif o == "rot":
            body.append(("rotqby", b, q1, k1))
        elif o == "shuf":
            body.append(("shufb", v, b, q2, m))
        elif o == "st":
            body.append(("stqd", v, rd_, 0))
        if rng.random() < 0.15:
            body.append(("lnop",))
    if inc_s >= len(seq):
        body.append(("ai", rs, rs, 1))
    body = body + [("ai", rd_, rd_, 1)] if inc_d_after else [("ai", rd_, rd_, 1)] + body
    head = lambda lane0: [lane0 & M32] + [rng.getrandbits(32) for _ in range(3)]
    regs[rs] = head(s | s_hi)
    regs[rd_] = head(d | (rng.getrandbits(32) & ~(LS - 1) if rng.random() < 0.3 else 0))
    if form in ("counter", "counter_up"):
        step = -1 if form == "counter" else 1
        body.insert(rng.randint(0, len(body)), ("ai", rn, rn, step))
        regs[rn] = head(n if step < 0 else -n)
        body.append(("brnz", rn, tag))
    else:
        pos = next(j for j, x in enumerate(body) if x == ("ai", rs, rs, 1))
        at = rng.randint(pos + 1, len(body)) if form == "ceq_after" else rng.randint(0, pos)
        swap = rng.random() < 0.5
        body.insert(at, ("ceq", rc, rn, rs) if swap else ("ceq", rc, rs, rn))
        regs[rn] = head(((s | s_hi) + n - (0 if form == "ceq_after" else 1)) & M32)
        body.append(("brz", rc, tag))
    return [("label", tag)] + body


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
static struct { uint32_t a; void (*f)(spu_context*); } g_tab[4096];
static int g_ntab;
void spu_register_function(uint32_t a, void (*f)(spu_context*)) { g_tab[g_ntab].a = a; g_tab[g_ntab++].f = f; }
void (*spu_lifted_lookup(const spu_context* c, uint32_t a))(spu_context*)
{
    (void)c;
    for (int i = 0; i < g_ntab; i++) if (g_tab[i].a == (a & SPU_LS_MASK)) return g_tab[i].f;
    return 0;
}
void spu_depth_guard(spu_context* c) { (void)c; }
void spu_indirect_branch(spu_context* c)
{
    void (*f)(spu_context*) = spu_lifted_lookup(c, c->pc);
    if (!f) exit(3);
    f(c);
}
void spu_drain_call(spu_context* c, uint32_t ret)
{
    (void)ret;
    while (g_spu_trampoline_fn) { void (*f)(spu_context*) = g_spu_trampoline_fn; g_spu_trampoline_fn = 0; f(c); }
}
u128 spu_rdch(spu_context* c, uint32_t ch) { (void)c; (void)ch; exit(6); }
uint32_t spu_rchcnt(spu_context* c, uint32_t ch) { (void)c; (void)ch; exit(6); }
void spu_wrch(spu_context* c, uint32_t ch, u128 v) { (void)c; (void)ch; (void)v; exit(6); }
void spu_stop(spu_context* c) { (void)c; }
void spu_halt(spu_context* c) { (void)c; exit(4); }

int main(int argc, char** argv)
{
    static spu_context ctx;
    spu_context_init(&ctx, 0);
    FILE* f = fopen(argv[1], "rb");
    if (fread(ctx.gpr, 16, 128, f) != 128 || fread(ctx.ls, 1, SPU_LS_SIZE, f) != SPU_LS_SIZE) return 7;
    fclose(f);
    ctx.status = SPU_STATUS_RUNNING;
    ctx.steps = 1;
    spu_recomp_register();
    spu_func_00000000(&ctx);
    while (g_spu_trampoline_fn) { void (*t)(spu_context*) = g_spu_trampoline_fn; g_spu_trampoline_fn = 0; t(&ctx); }
    f = fopen(argv[2], "wb");
    fwrite(ctx.gpr, 16, 128, f);
    fwrite(ctx.ls, 1, SPU_LS_SIZE, f);
    fclose(f);
    (void)argc;
    return 0;
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
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--cc")
    ap.add_argument("--keep")
    args = ap.parse_args()
    cc = find_cc(args.cc)
    if not cc:
        print("SKIP: no C compiler")
        return 0
    rng = random.Random(args.seed)
    work = args.keep or tempfile.mkdtemp(prefix="spucopy_")
    os.makedirs(work, exist_ok=True)
    harness = os.path.join(work, "harness.c")
    with open(harness, "w") as f:
        f.write(HARNESS)
    failures = fast = expected = 0
    for p in range(args.programs):
        regs = [[rng.getrandbits(32) for _ in range(4)] for _ in range(128)]
        ls = bytearray(rng.getrandbits(8) for _ in range(LS))
        used = {0, 1}
        prog = []
        heads = []
        positives = 0
        for j in range(rng.randint(1, 3)):
            positive = rng.random() < 0.85
            loop = copy_loop(rng, f"L{j}", regs, used, positive)
            heads.append(loop[0][1])
            prog += loop
            positives += positive
        prog.append(("stop",))
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
        want_regs, want_ls = run_model(prog, regs, ls)
        pdir = os.path.join(work, f"p{p}")
        os.makedirs(pdir, exist_ok=True)
        code = os.path.join(pdir, "code.bin")
        with open(code, "wb") as f:
            f.write(b"".join(w.to_bytes(4, "big") for w in words))
        state = os.path.join(pdir, "in.bin")
        with open(state, "wb") as f:
            f.write(b"".join(struct.pack("<4I", *r) for r in regs) + bytes(ls))
        end = len(words) * 4
        for merge in (False, True):
            starts = sorted({0} | ({labels[h] for h in heads} if merge else set()))
            bounds = [{"start": s, "end": starts[k + 1] if k + 1 < len(starts) else end} for k, s in enumerate(starts)]
            functions = os.path.join(pdir, "functions.json")
            with open(functions, "w") as f:
                json.dump(bounds, f)
            out = os.path.join(pdir, "m" if merge else "s")
            cmd = [sys.executable, os.path.join(HERE, "spu_lifter.py"), code, "--base", "0", "--functions", functions, "-o", out]
            if merge:
                cmd.append("--merge-chunks")
            subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            with open(os.path.join(out, "spu_recomp.c")) as f:
                hits = f.read().count("spu_ls_copy_fwd(")
            if not merge:
                fast += hits
                expected += positives
            exe = os.path.join(pdir, ("m" if merge else "s") + ".exe")
            b = subprocess.run([cc, "-std=gnu17", "-O2", "-msse4.1", "-w", "-I", out, "-I", os.path.join(REPO, "include"),
                                "-I", os.path.join(REPO, "runtime"), "-I", os.path.join(REPO, "runtime", "spu"),
                                os.path.join(out, "spu_recomp.c"), harness, "-o", exe], capture_output=True, text=True)
            if b.returncode:
                raise RuntimeError(b.stderr[-3000:])
            res = os.path.join(pdir, "out.bin")
            r = subprocess.run([exe, state, res], capture_output=True, timeout=120)
            with open(res, "rb") as f:
                got = f.read()
            got_regs = [list(struct.unpack("<4I", got[16 * k:16 * k + 16])) for k in range(128)]
            if r.returncode or got_regs != want_regs or got[2048:] != bytes(want_ls) or (hits != positives and not merge):
                failures += 1
                bad = [k for k in range(128) if got_regs[k] != want_regs[k]][:4]
                ldiff = [a for a in range(LS) if got[2048 + a] != want_ls[a]][:4]
                print(f"program {p} seed {args.seed} merge={merge}: rc={r.returncode} regs {bad} ls {ldiff} "
                      f"fast paths {hits}/{positives}")
    print(f"{args.programs} programs x 2 variants, {fast}/{expected} copy loops on the fast path, {failures} mismatches")
    if not args.keep:
        shutil.rmtree(work, ignore_errors=True)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
