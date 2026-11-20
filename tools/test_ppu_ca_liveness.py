#!/usr/bin/env python3
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
spec = importlib.util.spec_from_file_location("ppu_lifter", ROOT / "tools" / "ppu_lifter.py")
L = importlib.util.module_from_spec(spec)
sys.modules["ppu_lifter"] = L
spec.loader.exec_module(L)
D = sys.modules["ppu_disasm"]

BLR = 0x4E800020


def addic(d, a, v): return (12 << 26) | (d << 21) | (a << 16) | (v & 0xFFFF)
def subfic(d, a, v): return (8 << 26) | (d << 21) | (a << 16) | (v & 0xFFFF)
def addze(d, a): return (31 << 26) | (d << 21) | (a << 16) | (202 << 1)
def adde(d, a, b): return (31 << 26) | (d << 21) | (a << 16) | (b << 11) | (138 << 1)
def srawi(a, s, sh): return (31 << 26) | (s << 21) | (a << 16) | (sh << 11) | (824 << 1)
def bl(frm, to): return (18 << 26) | ((to - frm) & 0x3FFFFFC) | 1
def b(frm, to): return (18 << 26) | ((to - frm) & 0x3FFFFFC)


PROGRAMS = {
    "addic then addze": ([addic(3, 3, 5), addze(4, 4), BLR], 1),
    "srawi then addze": ([srawi(3, 3, 2), addze(3, 3), BLR], 1),
    "subfic then adde": ([subfic(5, 4, 0), adde(3, 5, 4), BLR], 1),
    "dead before return": ([addic(3, 3, -1), addic(5, 5, 1), BLR], 0),
    "dead after rewrite": ([addic(3, 3, 7), srawi(4, 4, 3), addze(5, 5), BLR], 1),
    "live across a call": ([addic(3, 3, 9), bl(0x1004, 0x2000), addze(4, 4), BLR], 1),
    "live into a tail call": ([addic(3, 3, 2), b(0x1004, 0x2000)], 1),
}


def lift(words):
    insns = D.disassemble_bytes(b"".join(struct.pack(">I", w) for w in words), 0x1000, True)
    return L.PPULifter().lift_function(insns, 0x1000, 0x1000 + 4 * len(words)).body_lines


pre = L.PPULifter()._preamble_lines()
SRA = pre[pre.index("static inline int64_t ppc_sraw(uint32_t* ca, int32_t rs, int n) {"):][:6]


def writes(lines):
    return sum("ctx->xer = (ctx->xer & ~(1u << 29))" in x for x in lines)


def test_lines():
    for name, (words, kept) in PROGRAMS.items():
        assert writes(L._elide_ca(lift(words))) == kept, (name, L._elide_ca(lift(words)))


def test_runs():
    cxx = shutil.which(os.environ.get("CXX", "c++")) or shutil.which("clang++")
    if cxx is None:
        if os.environ.get("CI"):
            raise SystemExit("FAIL: no C++ compiler found")
        print("skip test_runs: no C++ compiler")
        return
    funcs, calls = [], []
    for n, (name, (words, _)) in enumerate(PROGRAMS.items()):
        if any(w >> 26 == 18 for w in words):
            continue
        plain = lift(words)
        for tag, body in (("p", plain), ("e", L._elide_ca(plain))):
            funcs.append(f"static void {tag}{n}(ppu_context* ctx) {{\n" + "\n".join("    " + x.strip() for x in body) + "\n}")
        calls.append(f"    check(p{n}, e{n}, {n});")
    vals = [0, 1, 2, 7, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0xFFFFFFFFFFFFFFFF, 0x8000000000000000, 0x123456789]
    src = "\n".join([
        '#include "ppu_context.h"', "#include <stdio.h>", "#include <string.h>",
        *SRA,
        *funcs,
        "static const uint64_t V[] = {" + ",".join(f"0x{v:X}ull" for v in vals) + "};",
        "static int fails;",
        "static void check(void (*p)(ppu_context*), void (*e)(ppu_context*), int n) {",
        "    for (unsigned i = 0; i < sizeof V / sizeof *V; i++) for (unsigned j = 0; j < sizeof V / sizeof *V; j++) for (int ca = 0; ca < 2; ca++) {",
        "        ppu_context a; memset(&a, 0, sizeof a); a.gpr[3] = V[i]; a.gpr[4] = V[j]; a.gpr[5] = V[(i + j) % 10]; a.xer = ca << 29;",
        "        ppu_context c = a; p(&a); e(&c);",
        "        if (memcmp(a.gpr, c.gpr, sizeof a.gpr)) { if (fails++ < 10) printf(\"program %d inputs %u,%u,%d differ\\n\", n, i, j, ca); }",
        "    }",
        "}",
        "int main(void) {", *calls, '    printf("%d mismatches\\n", fails);', "    return fails != 0;", "}",
    ])
    with tempfile.TemporaryDirectory() as td:
        cpp = Path(td) / "ca.cpp"
        exe = Path(td) / ("ca.exe" if os.name == "nt" else "ca")
        cpp.write_text(src)
        r = subprocess.run([cxx, "-std=c++20", "-O2", "-w", "-I" + str(ROOT / "runtime" / "ppu"), "-o", str(exe), str(cpp)],
                           capture_output=True, text=True)
        assert r.returncode == 0, r.stdout + r.stderr
        out = subprocess.run([str(exe)], capture_output=True, text=True)
        assert out.returncode == 0, out.stdout


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print("ok", name)
