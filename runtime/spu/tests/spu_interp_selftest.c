#include "../spu_interp.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

u128 spu_rdch(spu_context* c, uint32_t ch) { (void)c;(void)ch; u128 z; memset(&z,0,sizeof z); return z; }
void spu_wrch(spu_context* c, uint32_t ch, u128 v) { (void)c;(void)ch;(void)v; }
uint32_t spu_rchcnt(spu_context* c, uint32_t ch) { (void)c;(void)ch; return 1; }

spu_lifted_fn spu_lifted_lookup(const spu_context* c, uint32_t a) { (void)c;(void)a; return 0; }

static const unsigned char PROG[] = {
    0x40,0x80,0x02,0x82, 0x40,0x80,0x03,0x83, 0x18,0x00,0xc1,0x04,
    0x1c,0x19,0x02,0x04, 0x3f,0x80,0x02,0x05, 0x08,0x00,0xc1,0x06,
    0x24,0x00,0x00,0x84, 0x00,0x00,0x00,0x00,
};

int main(void) {
    static spu_context ctx;
    memset(&ctx, 0, sizeof ctx);
    spu_context_init(&ctx, 0);
    memcpy(ctx.ls, PROG, sizeof PROG);
    ctx.gpr[1]._u32[0] = 0x100;

    spu_interp_run(&ctx, 0);

    #define P(i) (ctx.gpr[i]._u32[0])
    printf("r2=%u r3=%u r4=%u r5=%u r6=%u stop=0x%X\n",
           P(2), P(3), P(4), P(5), P(6), ctx.stop_code);

    assert(P(2) == 5);
    assert(P(3) == 7);
    assert(P(4) == 112);
    assert(P(5) == 112);
    assert(P(6) == 2);
    assert(ctx.status == SPU_STATUS_STOPPED_BY_STOP);

    const unsigned char* q = &ctx.ls[0x100];
    unsigned w = ((unsigned)q[0]<<24)|((unsigned)q[1]<<16)|((unsigned)q[2]<<8)|q[3];
    assert(w == 112);

    static const unsigned char LOOP[] = {
        0x40,0x80,0x00,0x02, 0x40,0x80,0x02,0x83, 0x1c,0x00,0x41,0x02,
        0x78,0x00,0xc1,0x04, 0x20,0x7f,0xff,0x04, 0x00,0x00,0x00,0x00,
    };
    static spu_context lc;
    memset(&lc, 0, sizeof lc);
    spu_context_init(&lc, 0);
    memcpy(lc.ls, LOOP, sizeof LOOP);
    spu_dispatch(&lc, 0);
    printf("loop r2=%u stop=0x%X\n", lc.gpr[2]._u32[0], lc.stop_code);
    assert(lc.gpr[2]._u32[0] == 5);
    assert(lc.status == SPU_STATUS_STOPPED_BY_STOP);

    static spu_context dc;
    memset(&dc, 0, sizeof dc);
    spu_context_init(&dc, 0);
    memcpy(dc.ls, PROG, sizeof PROG);
    spu_interp_run_until(&dc, 0, 4);
    assert(dc.pc == 4 && dc.gpr[2]._u32[0] == 5 && dc.gpr[3]._u32[0] == 0);

    printf("spu_interp_selftest: PASS\n");
    return 0;
}
