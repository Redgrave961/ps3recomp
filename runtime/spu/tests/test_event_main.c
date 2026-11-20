#include "spu_recomp.h"
#include "spu_helpers.h"
#include "spu_dma.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef struct { uint64_t source, data1, data2, data3; } ev_t;
typedef struct { ev_t buf[16]; int head, tail, count, active; } ev_queue;

static int evq_push(ev_queue* q, uint64_t source, uint64_t d1, uint64_t d2, uint64_t d3) {
    if (!q->active || q->count == 16) return -1;
    q->buf[q->tail] = (ev_t){source, d1, d2, d3};
    q->tail = (q->tail + 1) % 16; q->count++;
    return 0;
}
static int evq_receive(ev_queue* q, ev_t* out) {
    if (!q->active || q->count == 0) return -1;
    *out = q->buf[q->head];
    q->head = (q->head + 1) % 16; q->count--;
    return 0;
}

#define EV_SOURCE_SPU_GROUP  0x0000000000000007ULL

static uint8_t  g_mem[1024];
uint8_t*        vm_base = g_mem;
static mfc_engine g_mfc;
static ev_queue g_spu_evq;

u128 spu_rdch(spu_context* ctx, uint32_t ch) { (void)ctx; (void)ch; return spu_zero(); }
uint32_t spu_rchcnt(spu_context* ctx, uint32_t ch) { (void)ctx; (void)ch; return 1; }

void spu_wrch(spu_context* ctx, uint32_t channel, u128 value) {
    if (channel == SPU_WrOutIntrMbox) {
        evq_push(&g_spu_evq, EV_SOURCE_SPU_GROUP, value._u32[0], 0, 0);
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

    memset(&g_spu_evq, 0, sizeof(g_spu_evq));
    g_spu_evq.active = 1;

    spu_func_00000000(&ctx);

    ev_t evt;
    int got = (evq_receive(&g_spu_evq, &evt) == 0);

    const uint32_t kExpect = 0x00001234u;
    uint32_t put_val = be32(&g_mem[0]);
    int put_ok   = (put_val == kExpect);
    int event_ok = (got && evt.source == EV_SOURCE_SPU_GROUP
                        && evt.data1 == kExpect);

    printf("  [DMA PUT ] SPU -> main_mem[0] = 0x%08X (expected 0x%08X)  %s\n",
           put_val, kExpect, put_ok ? "OK" : "FAIL");
    printf("  [EVENT   ] PPU received completion: got=%d source=0x%llX data1=0x%llX  %s\n",
           got, (unsigned long long)evt.source, (unsigned long long)evt.data1,
           event_ok ? "OK" : "FAIL");

    if (put_ok && event_ok)
        printf("  PASS: SPU completion delivered through the event-queue path to the PPU.\n");
    return (put_ok && event_ok) ? 0 : 1;
}
