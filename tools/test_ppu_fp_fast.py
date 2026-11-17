#!/usr/bin/env python3
"""The single-check scalar FP helpers equal their reference definitions bit for bit.

ppu_fp_bin / ppu_fmadd_core test only the result for NaN and ppu_f2i32 /
ppu_f2i64 are one host conversion; each is compared against the
operand-by-operand reference (PowerISA NaN priority, saturating conversion
with nearbyint/trunc) over special and random values.
"""
from __future__ import annotations

import importlib.util
import os
import shutil
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

DRIVER = r"""
#include <stdio.h>
#include <stdlib.h>
static double ref_bin(double a, double b, double r)
{
    if (a != a) return ppu_fp_quiet(a);
    if (b != b) return ppu_fp_quiet(b);
    if (r != r) return ppu_fp_default_qnan();
    return r;
}
static double ref_fma(double a, double c, double b, int nb, int nr)
{
    if (a != a) return ppu_fp_quiet(a);
    if (b != b) return ppu_fp_quiet(b);
    if (c != c) return ppu_fp_quiet(c);
    double r = fma(a, c, nb ? -b : b);
    if (r != r) return ppu_fp_default_qnan();
    return nr ? -r : r;
}
static uint32_t ref_i32(double v, int rn)
{
    if (v != v) return 0x80000000u;
    double r = rn ? nearbyint(v) : trunc(v);
    if (r >= 2147483647.0) return 0x7FFFFFFFu;
    if (r <= -2147483648.0) return 0x80000000u;
    return (uint32_t)(int32_t)r;
}
static uint64_t ref_i64(double v, int rn)
{
    if (v != v) return 0x8000000000000000ULL;
    double r = rn ? nearbyint(v) : trunc(v);
    if (r >= 9223372036854775807.0) return 0x7FFFFFFFFFFFFFFFULL;
    if (r <= -9223372036854775808.0) return 0x8000000000000000ULL;
    return (uint64_t)(int64_t)r;
}
static uint64_t bits(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static double dbl(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }
int main(void)
{
    static const uint64_t special[] = { 0, 0x8000000000000000ull, 0x3FF0000000000000ull, 0xBFF0000000000000ull,
        0x7FF0000000000000ull, 0xFFF0000000000000ull, 0x7FF8000000000000ull, 0xFFF8000000000001ull, 0x7FF0000000000001ull,
        0x0000000000000001ull, 0x41DFFFFFFFC00000ull, 0x41DFFFFFFFE00000ull, 0x41E0000000000000ull, 0xC1E0000000000000ull,
        0xC1E0000000100000ull, 0xC1E0000000200000ull, 0x43E0000000000000ull, 0xC3E0000000000000ull, 0x43DFFFFFFFFFFFFFull,
        0x3FE0000000000000ull, 0x3FF8000000000000ull, 0x4004000000000000ull, 0xC004000000000000ull };
    const int ns = sizeof special / sizeof *special;
    double v[4096];
    int n = 0;
    for (int i = 0; i < ns; i++) v[n++] = dbl(special[i]);
    uint64_t s = 0x9E3779B97F4A7C15ull;
    while (n < 4096) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; v[n] = (n & 1) ? dbl(s) : (double)(int64_t)s / (double)(1 + (s >> 40)); n++; }
    long fails = 0;
    for (int i = 0; i < n; i++) {
        for (int rn = 0; rn < 2; rn++) {
            fails += ppu_f2i32(v[i], rn) != ref_i32(v[i], rn);
            fails += ppu_f2i64(v[i], rn) != ref_i64(v[i], rn);
        }
        for (int j = 0; j < 64; j++) {
            const double a = v[i], b = v[(i * 7 + j) % n], c = v[(i * 13 + j * 3) % n];
            fails += bits(ppu_fadd(a, b)) != bits(ref_bin(a, b, a + b));
            fails += bits(ppu_fsub(a, b)) != bits(ref_bin(a, b, a - b));
            fails += bits(ppu_fmul(a, b)) != bits(ref_bin(a, b, a * b));
            fails += bits(ppu_fdiv(a, b)) != bits(ref_bin(a, b, a / b));
            for (int m = 0; m < 4; m++) fails += bits(ppu_fmadd_core(a, c, b, m & 1, m >> 1)) != bits(ref_fma(a, c, b, m & 1, m >> 1));
        }
    }
    printf("%ld mismatches\n", fails);
    return fails != 0;
}
"""


def test_fp_fast():
    cxx = shutil.which(os.environ.get("CXX", "c++")) or shutil.which("clang++")
    if cxx is None:
        if os.environ.get("CI"):
            raise SystemExit("FAIL: no C++ compiler found")
        print("skip: no C++ compiler")
        return
    pre = L.SOURCE_PREAMBLE
    pre = pre[pre.index("#include <stdio.h>"):pre.index("/* FPSCR (Book I 4.2.2)")]
    with tempfile.TemporaryDirectory() as td:
        cpp = Path(td) / "fp.cpp"
        exe = Path(td) / ("fp.exe" if os.name == "nt" else "fp")
        cpp.write_text("#include <stdint.h>\n" + pre + DRIVER)
        for opt in ("-O0", "-O2"):
            r = subprocess.run([cxx, "-std=c++20", opt, "-w", "-march=x86-64-v3", "-o", str(exe), str(cpp)], capture_output=True, text=True)
            assert r.returncode == 0, r.stdout + r.stderr
            out = subprocess.run([str(exe)], capture_output=True, text=True)
            assert out.returncode == 0, opt + ": " + out.stdout


if __name__ == "__main__":
    test_fp_fast()
    print("ok test_fp_fast")
