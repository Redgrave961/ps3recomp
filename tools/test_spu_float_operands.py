#!/usr/bin/env python3
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

PROGRAM = r"""
#include "spu_context.h"
#include "spu_helpers.h"
#include <stdio.h>

int main(void) {
    unsigned long long bad[3] = {0, 0, 0};
    for (uint64_t base = 0; base < 0x100000000ull; base += 4) {
        u128 a;
        for (int i = 0; i < 4; i++) a._u32[i] = (uint32_t)(base + i);
        const __m128i f = _mm_castps_si128(spu__xf(a)), k = spu__sse_key(spu__xl(a)), m = spu__sse_mag(spu__xl(a));
        uint32_t F[4], K[4], M[4];
        memcpy(F, &f, 16); memcpy(K, &k, 16); memcpy(M, &m, 16);
        for (int i = 0; i < 4; i++) {
            const uint32_t x = (uint32_t)(base + i);
            bad[0] += F[i] != ((x & 0x7F800000u) == 0x7F800000u ? (x & 0x80000000u) | 0x7F7FFFFFu : x);
            bad[1] += (int64_t)(int32_t)K[i] != spu__sf_key(x);
            bad[2] += (int64_t)M[i] != spu__sf_mag(x);
        }
    }
    printf("all 2^32 operands: extended-range clamp %llu, compare key %llu, compare magnitude %llu mismatches\n",
           bad[0], bad[1], bad[2]);
    return (bad[0] || bad[1] || bad[2]) != 0;
}
"""


def main():
    cc = next((shutil.which(c) for c in (os.environ.get("CC"), "clang", "gcc") if c and shutil.which(c)), None)
    if not cc:
        print("SKIP: no C compiler")
        return 0
    work = tempfile.mkdtemp(prefix="spufops_")
    try:
        src, exe = os.path.join(work, "t.c"), os.path.join(work, "t.exe")
        with open(src, "w") as f:
            f.write(PROGRAM)
        subprocess.run([cc, "-std=gnu17", "-O2", "-march=x86-64-v3", "-w", "-I", os.path.join(REPO, "include"),
                        "-I", os.path.join(REPO, "runtime"), "-I", os.path.join(REPO, "runtime", "spu"), src, "-o", exe],
                       check=True)
        r = subprocess.run([exe], capture_output=True, text=True, timeout=600)
        print(r.stdout.strip())
        return r.returncode
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
