#include "spu_recomp.h"
#include "spu_helpers.h"
#include "spu_dma.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static uint8_t g_mem[1024];
uint8_t* vm_base = g_mem;

static mfc_engine g_mfc;
static uint32_t   g_mbox = 0;
static int        g_mbox_wrote = 0;

u128 spu_rdch(spu_context* ctx, uint32_t ch) { (void)ctx; (void)ch; return spu_zero(); }
uint32_t spu_rchcnt(spu_context* ctx, uint32_t ch) { (void)ctx; (void)ch; return 1; }

void spu_wrch(spu_context* ctx, uint32_t channel, u128 value) {
    if (channel == SPU_WrOutMbox) {
        g_mbox = value._u32[0];
        g_mbox_wrote = 1;
        return;
    }
    mfc_channel_write(&g_mfc, ctx, channel, value._u32[0]);
}
void spu_indirect_branch(spu_context* ctx) {
    (void)ctx; fprintf(stderr, "FAIL: unexpected indirect branch\n");
}
void spu_register_function(uint32_t addr, void (*fn)(spu_context*)) { (void)addr; (void)fn; }

static uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

void spu_stop(spu_context* ctx) { (void)ctx; }
void spu_halt(spu_context* ctx) { (void)ctx; }

int main(void) {
    memset(g_mem, 0, sizeof(g_mem));

    spu_context ctx;
    spu_context_init(&ctx, 0);
    mfc_engine_init(&g_mfc);

    spu_func_00000000(&ctx);

    const uint32_t kExpect = 0x00001234u;
    uint32_t put_val = be32(&g_mem[0]);
    int put_ok  = (put_val == kExpect);
    int mbox_ok = (g_mbox_wrote && g_mbox == kExpect);

    printf("  [DMA PUT ] SPU -> main_mem[0] = 0x%08X (expected 0x%08X)  %s\n",
           put_val, kExpect, put_ok ? "OK" : "FAIL");
    printf("  [MAILBOX ] SPU -> PPU signal  = 0x%08X (wrote=%d)         %s\n",
           g_mbox, g_mbox_wrote, mbox_ok ? "OK" : "FAIL");

    if (put_ok && mbox_ok)
        printf("  PASS: SPU job produced shared-memory output AND signalled PPU completion.\n");
    return (put_ok && mbox_ok) ? 0 : 1;
}
