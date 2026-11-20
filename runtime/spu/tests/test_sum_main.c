#include "spu_recomp.h"
#include "spu_helpers.h"
#include <stdio.h>
#include <stdint.h>

uint8_t* vm_base = 0;

static const uint32_t kInputs[] = { 10, 20, 30, 40 };
static const int      kNumInputs = (int)(sizeof(kInputs) / sizeof(kInputs[0]));
static int            g_in_idx = 0;
static uint32_t       g_out = 0;
static int            g_wrote = 0;

u128 spu_rdch(spu_context* ctx, uint32_t channel) {
    (void)ctx;
    uint32_t v = 0;
    if (channel == SPU_RdInMbox) {
        if (g_in_idx < kNumInputs) v = kInputs[g_in_idx++];
        else { fprintf(stderr, "FAIL: rdch underflow\n"); return spu_zero(); }
    }
    u128 r = spu_zero(); r._u32[0] = v; return r;
}
uint32_t spu_rchcnt(spu_context* ctx, uint32_t channel) {
    (void)ctx; (void)channel; return 1;
}
void spu_wrch(spu_context* ctx, uint32_t channel, u128 value) {
    (void)ctx;
    if (channel == SPU_WrOutMbox) { g_out = value._u32[0]; g_wrote = 1; }
}

void spu_indirect_branch(spu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "FAIL: unexpected spu_indirect_branch\n");
}
void spu_register_function(uint32_t addr, void (*fn)(spu_context*)) {
    (void)addr; (void)fn;
}

void spu_stop(spu_context* ctx) { (void)ctx; }
void spu_halt(spu_context* ctx) { (void)ctx; }

int main(void) {
    spu_context ctx;
    spu_context_init(&ctx, 0);
    spu_func_00000000(&ctx);

    uint32_t expected = 0;
    for (int i = 0; i < kNumInputs; i++) expected += kInputs[i];

    int ok = 1;
    if (!g_wrote)        { printf("FAIL: no wrch to outbound mailbox\n"); ok = 0; }
    if (g_in_idx != kNumInputs)
        { printf("FAIL: consumed %d/%d inbound values\n", g_in_idx, kNumInputs); ok = 0; }
    if (g_out != expected)
        { printf("FAIL: outbound = %u (expected %u)\n", g_out, expected); ok = 0; }

    if (ok) {
        printf("OK: consumed %d values, sum = %u (matches %u)\n",
               g_in_idx, g_out, expected);
    }
    return ok ? 0 : 1;
}
