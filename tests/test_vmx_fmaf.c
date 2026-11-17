#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include "../runtime/ppu/ppu_vmx.h"

static uint64_t st = 88172645463325252ull;
static uint32_t rnd(void) { st ^= st << 13; st ^= st >> 7; st ^= st << 17; return (uint32_t)st; }
static float f(uint32_t u) { float x; memcpy(&x, &u, 4); return x; }
static uint32_t U(float x) { uint32_t u; memcpy(&u, &x, 4); return u; }
static uint32_t fixed(float x) { return isnan(x) ? 0x7FC00000u : U(x); }

int main(int argc, char** argv)
{
    const long count = argc > 1 ? atol(argv[1]) : 20000000L;
    unsigned long long n = 0, bad = 0;
    for (long i = 0; i < count; i += 4) {
        uint8_t va[16], vb[16], vc[16], d1[16], d2[16];
        uint32_t as[4], bs[4], cs[4];
        for (int l = 0; l < 4; l++) {
            uint32_t a = rnd(), b = rnd(), c = rnd();
            if ((i + l) & 1) {
                a = (a & 0x80FFFFFF) | ((0x60 + (rnd() % 0x40)) << 23);
                b = (b & 0x80FFFFFF) | ((0x60 + (rnd() % 0x40)) << 23);
                c = (c & 0x80FFFFFF) | ((0x60 + (rnd() % 0x40)) << 23);
            }
            if (((i + l) & 7) == 2) c = U(-f(a) * f(b)) ^ (rnd() & 0xF);
            as[l] = a; bs[l] = b; cs[l] = c;
            vmx_sw(va, l, a); vmx_sw(vc, l, b); vmx_sw(vb, l, c);
        }
        vmx_vmaddfp(d1, va, vb, vc, 0);
        vmx_vnmsubfp(d2, va, vb, vc, 0);
        for (int l = 0; l < 4; l++) {
            const float x = f(as[l]), z = f(bs[l]), y = f(cs[l]);
            const uint32_t e1 = fixed(fmaf(x, z, y)), e2 = fixed(-fmaf(x, z, -y));
            n++;
            if (vmx_w(d1, l) != e1 || vmx_w(d2, l) != e2) {
                if (bad < 5) printf("diff %08x %08x %08x: %08x %08x vs %08x %08x\n", as[l], bs[l], cs[l],
                                    vmx_w(d1, l), vmx_w(d2, l), e1, e2);
                bad++;
            }
        }
    }
    printf("%llu lanes, %llu differ\n", n, bad);
    return bad ? 1 : 0;
}
