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

import test_spu_float_clean as fc

fc.RR.update({"cbx": 0x1D4, "chx": 0x1D5, "cwx": 0x1D6, "cdx": 0x1D7})
fc.RI7.update({"cbd": 0x1F4, "chd": 0x1F5, "cwd": 0x1F6, "cdd": 0x1F7})
fc.RRR["shufb"] = 0xB
REGS = list(range(3, 10))


def reference(i, labels):
    if i[0] == "shufb":
        return f"c->gpr[{i[1]}] = spu_shufb(c->gpr[{i[2]}], c->gpr[{i[3]}], c->gpr[{i[4]}]);"
    if i[0] in ("cbd", "chd", "cwd", "cdd"):
        return f"c->gpr[{i[1]}] = spu_{i[0]}(c->gpr[{i[2]}], {i[3]});"
    return fc.reference(i, labels)


def block(rng, n, tag):
    out = []
    pick = lambda: rng.choice(REGS)
    for j in range(n):
        r = rng.random()
        if r < 0.3:
            op = rng.choice(["cbd", "chd", "cwd", "cdd", "cbx", "chx", "cwx", "cdx"])
            out.append((op, pick(), pick(), rng.randrange(128) if op[-1] == "d" else pick()))
        elif r < 0.65:
            out.append(("shufb", pick(), pick(), pick(), pick()))
        elif r < 0.75:
            out.append((rng.choice(["ori", "ai", "shlqbyi", "rotqbyi", "rotqbyi"]), pick(), pick(), rng.choice([0, 0, 1, 3, 4, 8, 12, 16, -16])))
        elif r < 0.82:
            out.append(("lqd", pick(), fc.BASE, rng.randrange(16) * 16))
        elif r < 0.88:
            out.append(("stqd", pick(), fc.BASE, rng.randrange(16) * 16))
        elif r < 0.94:
            out.append((rng.choice(["xor", "and", "or"]), pick(), pick(), pick()))
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
            cnt = fc.LOOP[b % 2]
            top = f"L{b}"
            body += [("il", cnt, rng.randint(1, 4)), ("label", top)] + block(rng, rng.randint(3, 14), top)
            body += [("ai", cnt, cnt, -1), ("brnz", cnt, top)]
        else:
            body += block(rng, rng.randint(3, 14), f"B{b}")
    return body + [("stop",)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--programs", type=int, default=60)
    ap.add_argument("--runs", type=int, default=200)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--cc")
    args = ap.parse_args()
    cc = fc.find_cc(args.cc)
    if not cc:
        print("SKIP: no C compiler")
        return 0
    rng = random.Random(args.seed)
    work = tempfile.mkdtemp(prefix="spuins_")
    harness = os.path.join(work, "harness.c")
    with open(harness, "w") as f:
        f.write(fc.HARNESS)
    failures = lowered = 0
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
                words.append(fc.encode(i, pc, labels))
                pc += 4
        pdir = os.path.join(work, f"p{p}")
        os.makedirs(pdir)
        with open(os.path.join(pdir, "reference.h"), "w") as f:
            f.write("static void reference_run(spu_context* c) {\n")
            f.write("\n".join(f"{i[1]}:;" if i[0] == "label" else "    " + reference(i, labels) for i in prog))
            f.write("\n}\n")
        code = os.path.join(pdir, "code.bin")
        with open(code, "wb") as f:
            f.write(b"".join(w.to_bytes(4, "big") for w in words))
        functions = os.path.join(pdir, "functions.json")
        with open(functions, "w") as f:
            json.dump([{"start": 0, "end": len(words) * 4}], f)
        out = os.path.join(pdir, "s")
        subprocess.run([sys.executable, os.path.join(fc.HERE, "spu_lifter.py"), code, "--base", "0", "--functions", functions,
                        "-o", out], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        with open(os.path.join(out, "spu_recomp.c")) as f:
            lowered += f.read().count("spu_shufb_ins(")
        exe = os.path.join(pdir, "s.exe")
        b = subprocess.run([cc, "-std=gnu17", "-O2", "-march=x86-64-v3", "-w", "-I", out, "-I", pdir,
                            "-I", os.path.join(fc.REPO, "include"), "-I", os.path.join(fc.REPO, "runtime"),
                            "-I", os.path.join(fc.REPO, "runtime", "spu"), os.path.join(out, "spu_recomp.c"), harness,
                            "-o", exe], capture_output=True, text=True)
        if b.returncode:
            raise RuntimeError(b.stderr[-3000:])
        r = subprocess.run([exe, str(args.runs)], capture_output=True, text=True, timeout=300)
        if r.returncode:
            failures += 1
            print(f"program {p} seed {args.seed}: rc={r.returncode}\n{r.stdout[-1500:]}")
    print(f"{args.programs} programs x {args.runs} inputs, {lowered} insert-pattern shufbs, {failures} mismatches")
    shutil.rmtree(work, ignore_errors=True)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
