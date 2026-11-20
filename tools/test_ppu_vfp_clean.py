#!/usr/bin/env python3
"""VMX float inputs produced by an earlier float op in the same block skip the
denormal screen (_vfp_clean): the hint appears only on such inputs, and the
hinted code computes the same vector registers as the unhinted code for
denormal, NaN, infinite and ordinary inputs with VSCR[NJ] on and off."""
from __future__ import annotations

import importlib.util
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _load(name, rel):
    spec = importlib.util.spec_from_file_location(name, ROOT / rel)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


D = _load("ppu_disasm", "tools/ppu_disasm.py")
L = _load("ppu_lifter", "tools/ppu_lifter.py")


def vx(vd, va, vb, xo): return (4 << 26) | (vd << 21) | (va << 16) | (vb << 11) | xo
def va(vd, a, b, c, xo): return (4 << 26) | (vd << 21) | (a << 16) | (b << 11) | (c << 6) | xo


BLR = 0x4E800020
PROGRAM = [vx(4, 1, 2, 0x00A), va(5, 4, 4, 3, 0x2E), vx(6, 5, 1, 0x04A), vx(7, 0, 6, 0x10A), vx(8, 7, 5, 0x40A),
           va(9, 8, 5, 6, 0x2F), vx(6, 1, 1, 0x484), vx(10, 6, 9, 0x00A), va(11, 10, 2, 9, 0x2E), BLR]


def _body(clean):
    insns = D.disassemble_bytes(b"".join(struct.pack(">I", w) for w in PROGRAM), 0x1000, True)
    lines = L.PPULifter().lift_function(insns, 0x1000, 0x1000 + 4 * len(PROGRAM)).body_lines
    lines = L._vfp_clean(lines) if clean else lines
    return "\n".join(x for x in lines if "return" not in x)


def test_hints():
    body = _body(True)
    hinted = [ln.strip() for ln in body.split("\n") if ") | 0x" in ln]
    assert hinted == [
        "vmx_vmaddfp(&ctx->vr[5], &ctx->vr[4], &ctx->vr[4], &ctx->vr[3], (ctx->vscr & 0x00010000u) | 0x30000000u);",
        "vmx_vsubfp(&ctx->vr[6], &ctx->vr[5], &ctx->vr[1], (ctx->vscr & 0x00010000u) | 0x10000000u);",
        "vmx_vrefp(&ctx->vr[7], &ctx->vr[6], (ctx->vscr & 0x00010000u) | 0x20000000u);",
        "vmx_vnmsubfp(&ctx->vr[9], &ctx->vr[8], &ctx->vr[5], &ctx->vr[6], (ctx->vscr & 0x00010000u) | 0x70000000u);",
        "vmx_vaddfp(&ctx->vr[10], &ctx->vr[6], &ctx->vr[9], (ctx->vscr & 0x00010000u) | 0x20000000u);",
        "vmx_vmaddfp(&ctx->vr[11], &ctx->vr[10], &ctx->vr[2], &ctx->vr[9], (ctx->vscr & 0x00010000u) | 0x50000000u);",
    ], hinted


def test_same_results():
    cc = shutil.which(os.environ.get("CC", "cc")) or shutil.which("clang") or shutil.which("gcc")
    if cc is None:
        if os.environ.get("CI"):
            raise SystemExit("FAIL: no C compiler found")
        print("skip: no compiler")
        return
    rng = random.Random(7)
    special = [0, 0x80000000, 1, 0x80000001, 0x007FFFFF, 0x807FFFFF, 0x00800000, 0x00800001, 0x7F800000, 0xFF800000,
               0x7FC00000, 0x7F800001, 0xFFC12345, 0x3F800000, 0xBF800000, 0x1F800000, 0x5F800000, 0x00400000]

    def word():
        r = rng.randrange(4)
        return rng.choice(special) if r == 0 else rng.randrange(1 << 32) & (0x807FFFFF if r == 1 else 0xFFFFFFFF)

    inputs = [[word() for _ in range(12)] for _ in range(4000)]
    src = "\n".join([
        "#include <stdint.h>", "#include <string.h>", "#include <stdio.h>", "#include \"ppu_vmx.h\"",
        "typedef struct { uint8_t vr[32][16]; uint32_t vscr; } ctx_t;",
        f"static const uint32_t IN[{len(inputs)}][12] = {{" + ",".join("{" + ",".join(str(x) for x in t) + "}" for t in inputs) + "};",
        "static void run(ctx_t* ctx, int clean) {",
        "  if (clean) {", _body(True), "  } else {", _body(False), "  }", "}",
        "int main(void) { long bad = 0;",
        f"  for (int n = 0; n < {len(inputs)}; n++) for (int nj = 0; nj < 2; nj++) {{",
        "    ctx_t a, b; memset(&a, 0, sizeof a);",
        "    for (int r = 0; r < 3; r++) memcpy(a.vr[r + 1], &IN[n][4 * r], 16);",
        "    a.vscr = nj ? 0x00010000u : 0; b = a;",
        "    run(&a, 0); run(&b, 1);",
        "    if (memcmp(&a, &b, sizeof a)) bad++; }",
        "  printf(\"%ld\\n\", bad); return bad != 0; }",
    ])
    with tempfile.TemporaryDirectory() as td:
        cpath = Path(td) / "t.c"
        exe = Path(td) / ("t.exe" if os.name == "nt" else "t")
        cpath.write_text(src)
        for opt in ("-O0", "-O2"):
            r = subprocess.run([cc, "-std=c11", opt, "-march=x86-64-v3", "-ffp-contract=off", "-I" + str(ROOT / "runtime" / "ppu"),
                                "-o", str(exe), str(cpath)], capture_output=True, text=True)
            assert r.returncode == 0, r.stderr
            out = subprocess.run([str(exe)], capture_output=True, text=True)
            assert out.returncode == 0, opt + ": " + out.stdout
    print(f"  {len(inputs)} inputs x NJ on/off: hinted and unhinted code agree")


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print("ok", name)
