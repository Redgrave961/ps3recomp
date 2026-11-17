#!/usr/bin/env python3
"""Compare+branch fusion and the XER.SO proof.

A compare followed by a branch on one of its bits lifts to a direct C
condition; the CR field store stays whenever any path can still read the
field. Every fused function is compiled next to its unfused lift and both are
run over the same inputs, which must leave identical r3 and CR. The SO proof
must find every instruction that can set XER.SO and nothing else.
"""
from __future__ import annotations

import importlib.util
import itertools
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))


def _load(name, rel_path):
    spec = importlib.util.spec_from_file_location(name, ROOT / rel_path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


L = _load("ppu_lifter", "tools/ppu_lifter.py")
D = sys.modules["ppu_disasm"]


def decode(words, base=0x1000):
    return D.disassemble_bytes(b"".join(struct.pack(">I", w) for w in words), base, True)


def cmp_word(kind, bf, ra, rb_or_imm):
    l64 = kind in ("cmpd", "cmpld", "cmpdi", "cmpldi")
    if kind in ("cmpwi", "cmpdi"):
        return (11 << 26) | (bf << 23) | (l64 << 21) | (ra << 16) | (rb_or_imm & 0xFFFF)
    if kind in ("cmplwi", "cmpldi"):
        return (10 << 26) | (bf << 23) | (l64 << 21) | (ra << 16) | (rb_or_imm & 0xFFFF)
    if kind == "fcmpu":
        return (63 << 26) | (bf << 23) | (ra << 16) | (rb_or_imm << 11)
    xo = 32 if kind in ("cmplw", "cmpld") else 0
    return (31 << 26) | (bf << 23) | (l64 << 21) | (ra << 16) | (rb_or_imm << 11) | (xo << 1)


def bc_word(bo, bi, frm, to):
    return (16 << 26) | (bo << 21) | (bi << 16) | ((to - frm) & 0xFFFC)


def li(rd, v):
    return (14 << 26) | (rd << 21) | (v & 0xFFFF)


BLR = 0x4E800020
KINDS = ("cmpw", "cmplw", "cmpd", "cmpld", "cmpwi", "cmplwi", "cmpdi", "cmpldi", "fcmpu")


def program(kind, bf, bit, taken_if_set, dead):
    cmp = cmp_word(kind, bf, 4, 5) if kind[-1] != "i" else cmp_word(kind, bf, 4, -3 if "l" not in kind else 7)
    kill = cmp_word("cmpwi", bf, 0, 0)
    words = [cmp, 0, li(3, 0)] + ([kill] if dead else []) + [BLR]
    target = 0x1000 + 4 * len(words)
    words[1] = bc_word(12 if taken_if_set else 4, 4 * bf + bit, 0x1004, target)
    words += [li(3, 1)] + ([kill] if dead else []) + [BLR]
    return words


def lift(words, so_free):
    lifter = L.PPULifter()
    lifter.xer_so_free = so_free
    return lifter.lift_function(decode(words), 0x1000, 0x1000 + 4 * len(words)).body_lines


INTS = [0, 1, 2, 5, 7, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0xFFFFFFFD, 0x100000000,
        0xFFFFFFFF00000000, 0x7FFFFFFFFFFFFFFF, 0x8000000000000000, 0xFFFFFFFFFFFFFFFF]
FLOATS = [0.0, -0.0, 1.0, -1.0, 2.5, float("inf"), float("-inf"), float("nan"), 1e300]


def test_lines_and_liveness():
    for kind in KINDS:
        for bf, bit, taken, dead in itertools.product((0, 3, 7), range(4), (True, False), (True, False)):
            plain = lift(program(kind, bf, bit, taken, dead), True)
            fused = L._fuse_cr(plain)
            stores = lambda ls: sum("cr_val" in x for x in ls)
            assert not any(f"ctx->cr >> {31 - 4 * bf - bit})" in x for x in fused), (kind, fused)
            assert stores(fused) == stores(plain) - dead, (kind, bf, bit, dead, fused)
    assert all("xer" not in x for x in lift(program("cmpw", 0, 3, True, False), True))
    assert any("ctx->xer >> 31" in x for x in L._fuse_cr(lift(program("cmpw", 0, 3, True, True), False)))


def test_live_paths_keep_the_store():
    cmp = "{ int64_t a = (int32_t)ctx->gpr[4]; int64_t b = (int32_t)ctx->gpr[5]; uint32_t cr_val = (a < b) ? 8 : (a > b) ? 4 : 2; ctx->cr = (ctx->cr & ~(0xFu << 0)) | (cr_val << 0); }"
    br = "if (((ctx->cr >> 1) & 1)) goto loc_2;"
    kill = cmp.replace("gpr[4]", "gpr[6]")
    reread = "if (((ctx->cr >> 3) & 1)) goto loc_3;"
    cases = {
        "dead": ([cmp, br, kill, "return;", "loc_2:", kill, "return;", "loc_3:", "return;"], True),
        "read on fall-through": ([cmp, br, reread, "return;", "loc_2:", kill, "return;", "loc_3:", "return;"], False),
        "read at target": ([cmp, br, kill, "return;", "loc_2:", reread, "return;", "loc_3:", "return;"], False),
        "read through back edge": (["loc_1:", reread, kill, cmp, br, "goto loc_1;", "loc_2:", kill, "return;", "loc_3:", "return;"], False),
        "call": ([cmp, br, "ctx->lr = 0x1; func_00001000(ctx); DRAIN_TRAMPOLINE(ctx);", kill, "return;", "loc_2:", kill, "return;", "loc_3:", "return;"], False),
        "mfcr": ([cmp, br, kill, "return;", "loc_2:", "ctx->gpr[3] = ctx->cr;", kill, "return;", "loc_3:", "return;"], False),
        "conditional kill": ([cmp, br, "if (g_patch) { " + kill + " } else { ; }", reread, "return;", "loc_2:", kill, "return;", "loc_3:", "return;"], False),
        "falls off the end": ([cmp, br, kill, "return;", "loc_2:", "ctx->gpr[3] = 1;", "loc_3:"], False),
    }
    for name, (lines, dropped) in cases.items():
        out = [x.strip() for x in L._fuse_cr(["    " + x if not x.endswith(":") else x for x in lines])]
        assert (cmp not in out) == dropped, (name, out)
        assert "if ((int64_t)(int32_t)ctx->gpr[4] == (int64_t)(int32_t)ctx->gpr[5]) goto loc_2;" in out, (name, out)


def test_so_proof():
    add = (31 << 26) | (3 << 21) | (4 << 16) | (5 << 11) | (266 << 1)
    addo = add | (1 << 10)
    mullwo = (31 << 26) | (3 << 21) | (4 << 16) | (5 << 11) | (1 << 10) | (235 << 1)
    subfo_rc = (31 << 26) | (3 << 21) | (4 << 16) | (5 << 11) | (1 << 10) | (40 << 1) | 1
    mtxer = (31 << 26) | (3 << 21) | (1 << 16) | (467 << 1)
    mtctr = (31 << 26) | (3 << 21) | (9 << 16) | (467 << 1)
    mfxer = (31 << 26) | (3 << 21) | (1 << 16) | (339 << 1)
    mcrxr = (31 << 26) | (0 << 23) | (512 << 1)
    insns = decode([add, addo, mullwo, subfo_rc, mtxer, mtctr, mfxer, mcrxr])
    assert L.xer_so_writers(insns) == [0x1004, 0x1008, 0x100C, 0x1010]
    assert L.xer_so_writers(decode([add, mtctr, mfxer, mcrxr, li(3, 1), BLR])) == []
    stwcx_rc = (31 << 26) | (3 << 21) | (4 << 16) | (5 << 11) | (150 << 1) | 1
    for free in (True, False):
        lifter = L.PPULifter()
        lifter.xer_so_free = free
        body = lifter.lift_function(decode([add | 1, cmp_word("cmpw", 1, 3, 4), stwcx_rc, BLR]), 0x1000, 0x1010).body_lines
        assert any("ctx->xer >> 31" in x for x in body) != free, (free, body)


def _cxx():
    return shutil.which(os.environ.get("CXX", "c++")) or shutil.which("clang++") or shutil.which("g++")


def test_fused_runs_like_unfused():
    cxx = _cxx()
    if cxx is None:
        if os.environ.get("CI"):
            raise SystemExit("FAIL: no C++ compiler found")
        print("skip test_fused_runs_like_unfused: no C++ compiler")
        return
    funcs, calls = [], []
    for n, (kind, bf, bit, taken, dead, so_free, so) in enumerate(itertools.product(
            KINDS, (0, 6), range(4), (True, False), (True, False), (True, False), (0, 1))):
        if so_free and so:
            continue
        plain = lift(program(kind, bf, bit, taken, dead), so_free)
        for tag, body in (("p", plain), ("f", L._fuse_cr(plain))):
            funcs.append(f"static void {tag}{n}(ppu_context* ctx) {{\n" + "\n".join(
                ("    " + x.strip()) if not x.strip().endswith(":") else x.strip() for x in body) + "\n}")
        calls.append(f"    check(p{n}, f{n}, {int(kind == 'fcmpu')}, {so}, {n});")
    src = "\n".join([
        '#include "ppu_context.h"', "#include <stdio.h>", "#include <string.h>", "#include <math.h>",
        "#define DRAIN_TRAMPOLINE(c) ((void)0)",
        *funcs,
        "static const uint64_t I[] = {" + ",".join(f"0x{v:X}ull" for v in INTS) + "};",
        "static const double F[] = {" + ",".join("NAN" if v != v else "INFINITY" if v == float("inf")
                                                 else "-INFINITY" if v == float("-inf") else repr(v) for v in FLOATS) + "};",
        "static int fails;",
        "static void check(void (*p)(ppu_context*), void (*f)(ppu_context*), int fp, int so, int n) {",
        "    int na = fp ? (int)(sizeof F / sizeof *F) : (int)(sizeof I / sizeof *I);",
        "    for (int i = 0; i < na; i++) for (int j = 0; j < na; j++) {",
        "        ppu_context a, b; memset(&a, 0, sizeof a);",
        "        a.cr = 0x5A5A5A5Au; a.xer = so ? 0x80000000u : 0; a.gpr[3] = 9;",
        "        if (fp) { a.fpr[4] = F[i]; a.fpr[5] = F[j]; } else { a.gpr[4] = I[i]; a.gpr[5] = I[j]; }",
        "        b = a; p(&a); f(&b);",
        "        if (a.gpr[3] != b.gpr[3] || a.cr != b.cr) { if (fails++ < 20) printf(\"case %d inputs %d,%d: r3 %llu/%llu cr %08X/%08X\\n\", n, i, j,",
        "            (unsigned long long)a.gpr[3], (unsigned long long)b.gpr[3], a.cr, b.cr); }",
        "    }",
        "}",
        "int main(void) {", *calls, '    printf("%d mismatches\\n", fails);', "    return fails != 0;", "}",
    ])
    with tempfile.TemporaryDirectory() as td:
        cpp = Path(td) / "fuse.cpp"
        exe = Path(td) / ("fuse.exe" if os.name == "nt" else "fuse")
        cpp.write_text(src)
        r = subprocess.run([cxx, "-std=c++20", "-O2", "-w", "-I" + str(ROOT / "runtime" / "ppu"), "-o", str(exe), str(cpp)],
                           capture_output=True, text=True)
        assert r.returncode == 0, r.stdout + r.stderr
        out = subprocess.run([str(exe)], capture_output=True, text=True)
        assert out.returncode == 0, out.stdout
        print("  " + out.stdout.strip().splitlines()[-1] + f" over {len(calls)} programs")


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print("ok", name)
