#include "ppu_recomp.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern "C" {
void     ps3_hle_register(uint32_t nid, const char* name, void* handler);
void     ps3_hle_call(uint32_t nid, ppu_context* ctx);
uint32_t ps3_hle_count(void);
}

static uint64_t mock_combine(uint64_t a, uint64_t b, uint64_t c)
{
    return a * 1000 + b * 10 + c;
}

static int fails = 0;
#define CHECK(c, m) do { if (c) printf("[PASS] %s\n", m); else { printf("[FAIL] %s\n", m); fails++; } } while (0)

int main()
{
    ps3_hle_register(0x12345678u, "mock_combine", (void*)mock_combine);
    ps3_hle_register(0xAABBCCDDu, "mock_other",   (void*)mock_combine);
    CHECK(ps3_hle_count() == 2, "two HLE NIDs registered");

    ppu_context ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.gpr[3] = 7; ctx.gpr[4] = 8; ctx.gpr[5] = 9;
    ps3_hle_call(0x12345678u, &ctx);
    printf("ps3_hle_call(mock_combine, r3=7,r4=8,r5=9) -> r3 = %llu\n",
           (unsigned long long)ctx.gpr[3]);
    CHECK(ctx.gpr[3] == 7089, "ABI adapter marshalled r3..r5 and returned via r3");

    memset(&ctx, 0, sizeof(ctx));
    ctx.gpr[3] = 999;
    ps3_hle_call(0xDEADBEEFu, &ctx);
    CHECK(ctx.gpr[3] == 0, "unresolved NID returns 0 (no crash)");

    printf("\nResults: %s (%d failure(s))\n", fails ? "FAIL" : "OK", fails);
    return fails ? 1 : 0;
}
