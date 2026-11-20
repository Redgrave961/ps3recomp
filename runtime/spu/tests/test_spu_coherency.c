#include "../spu_coherency.c"

#include "../../ppu/ppu_memory.h"

#include <stdio.h>
#include <string.h>

static uint8_t g_guest_mem[1u << 20];
uint8_t* vm_base = g_guest_mem;

uint32_t g_ww_lo = 0xFFFFFFFFu, g_ww_hi = 0;
void ps3_ww_report_inline(uint32_t addr, uint64_t val, int width)
{
    (void)addr; (void)val; (void)width;
}

static int g_wakes;
void spu_ch_wake(spu_context* ctx) { (void)ctx; g_wakes++; }

static int g_pass = 0;
static int g_fail = 0;
static const char* g_current = "(none)";

#define TEST(name) g_current = (name)

#define CHECK(cond) do {                                          \
    if (cond) { g_pass++; }                                       \
    else {                                                        \
        g_fail++;                                                 \
        fprintf(stderr, "FAIL: %s\n  at %s:%d: %s\n",             \
                g_current, __FILE__, __LINE__, #cond);            \
    }                                                             \
} while (0)

#define CHECK_EQ_U32(actual, expected) do {                       \
    uint32_t _a = (uint32_t)(actual), _e = (uint32_t)(expected);  \
    if (_a == _e) { g_pass++; }                                   \
    else {                                                        \
        g_fail++;                                                 \
        fprintf(stderr, "FAIL: %s\n  at %s:%d: %s\n"              \
                        "  actual   0x%08X\n  expected 0x%08X\n", \
                g_current, __FILE__, __LINE__, #actual, _a, _e);  \
    }                                                             \
} while (0)

#define LINE_A   0x00040000u
#define LINE_B   0x00040180u
#define LINE_C   0x00040300u

static spu_context g_spu_a;
static spu_context g_spu_b;

static void spu_getllar(spu_context* ctx, uint32_t ea)
{
    ea &= ~127u;
    spu_lockline_lock(ea);
    spu_coh_reserve(ctx, ea);
    ctx->resv = (ea & ~127u) | 1u;
    spu_lockline_unlock(ea);
}

static void spu_reset(spu_context* ctx, uint32_t spu_id)
{
    spu_context_init(ctx, 0);
    ctx->spu_id = spu_id;
}

static void test_bitmap(void)
{
    spu_reset(&g_spu_a, 0);

    TEST("a line nobody reserved is not marked");
    CHECK(spu_coh_is_reserved(LINE_A) == 0);
    CHECK(spu_coh_is_reserved(LINE_A + 64) == 0);

    spu_getllar(&g_spu_a, LINE_A);

    TEST("GETLLAR marks the whole 128-byte line, not just the word");
    CHECK(spu_coh_is_reserved(LINE_A) != 0);
    CHECK(spu_coh_is_reserved(LINE_A + 4) != 0);
    CHECK(spu_coh_is_reserved(LINE_A + 127) != 0);

    TEST("the neighbouring lines stay unmarked");
    CHECK(spu_coh_is_reserved(LINE_A - 1) == 0);
    CHECK(spu_coh_is_reserved(LINE_A + 128) == 0);
    CHECK(spu_coh_is_reserved(LINE_B) == 0);
}

static void test_ppu_store_raises_lr(void)
{
    spu_reset(&g_spu_a, 0);
    g_wakes = 0;
    unsigned long raised_before = g_spu_lr_raise;

    spu_getllar(&g_spu_a, LINE_A);

    TEST("a PPU store into the reserved line raises SPU_EVENT_LR");
    vm_write32(LINE_A + 8, 0xDEADBEEFu);
    CHECK_EQ_U32(g_spu_a.event_status & SPU_EVENT_LR, SPU_EVENT_LR);

    TEST("...and drops the reservation, so the SPU's PUTLLC must fail");
    CHECK((g_spu_a.resv & 1u) == 0);

    TEST("...and wakes the host thread parked on the event channel");
    CHECK(g_wakes == 1);
    CHECK(g_spu_lr_raise == raised_before + 1);

    TEST("...and the store itself still lands, big-endian");
    CHECK_EQ_U32(vm_read32(LINE_A + 8), 0xDEADBEEFu);

    TEST("the line stays marked after the notify (reservations recur)");
    CHECK(spu_coh_is_reserved(LINE_A) != 0);

    TEST("a second store with no live reservation raises nothing more");
    vm_write32(LINE_A + 8, 0x11111111u);
    CHECK(g_wakes == 1);
    CHECK(g_spu_lr_raise == raised_before + 1);

    TEST("the SPU acks the event and the bit clears");
    g_spu_a.event_status &= ~SPU_EVENT_LR;
    CHECK_EQ_U32(g_spu_a.event_status, 0);

    TEST("re-reserving and storing again raises it a second time");
    spu_getllar(&g_spu_a, LINE_A);
    vm_write32(LINE_A + 8, 0x22222222u);
    CHECK_EQ_U32(g_spu_a.event_status & SPU_EVENT_LR, SPU_EVENT_LR);
    CHECK(g_wakes == 2);
}

static void test_no_false_raise(void)
{
    spu_reset(&g_spu_a, 0);
    g_wakes = 0;

    spu_getllar(&g_spu_a, LINE_A);

    TEST("a store to a DIFFERENT line leaves the reservation alone");
    vm_write32(LINE_B + 8, 0x33333333u);
    CHECK_EQ_U32(g_spu_a.event_status, 0);
    CHECK((g_spu_a.resv & 1u) == 1);
    CHECK(g_wakes == 0);

    TEST("...even when that line is itself reserved by another SPU");
    spu_reset(&g_spu_b, 1);
    spu_getllar(&g_spu_b, LINE_B);
    vm_write32(LINE_B + 8, 0x44444444u);
    CHECK_EQ_U32(g_spu_b.event_status & SPU_EVENT_LR, SPU_EVENT_LR);
    CHECK_EQ_U32(g_spu_a.event_status, 0);
    CHECK((g_spu_a.resv & 1u) == 1);

    TEST("an SPU that holds NO reservation is never notified");
    spu_reset(&g_spu_b, 1);
    g_wakes = 0;
    vm_write32(LINE_B + 8, 0x55555555u);
    CHECK_EQ_U32(g_spu_b.event_status, 0);
    CHECK(g_wakes == 0);
}

static void test_all_store_widths(void)
{
    static const char* names[4] = { "vm_write8", "vm_write16",
                                    "vm_write32", "vm_write64" };
    for (int w = 0; w < 4; w++) {
        spu_reset(&g_spu_a, 0);
        spu_getllar(&g_spu_a, LINE_C);

        TEST(names[w]);
        switch (w) {
        case 0: vm_write8 (LINE_C + 16, 0xA5u);                    break;
        case 1: vm_write16(LINE_C + 16, 0xA5A5u);                  break;
        case 2: vm_write32(LINE_C + 16, 0xA5A5A5A5u);              break;
        case 3: vm_write64(LINE_C + 16, 0xA5A5A5A5A5A5A5A5ull);    break;
        }
        CHECK_EQ_U32(g_spu_a.event_status & SPU_EVENT_LR, SPU_EVENT_LR);
        CHECK((g_spu_a.resv & 1u) == 0);
    }
}

static void test_many_contexts(void)
{
    enum { N = 20 };
    spu_context* c[N];
    for (unsigned i = 0; i < N; ++i) {
        c[i] = calloc(1, sizeof(*c[i]));
        CHECK(c[i] != NULL);
        spu_reset(c[i], i);
        spu_getllar(c[i], LINE_A);
    }
    TEST("same-value writes invalidate every registered context, beyond eight");
    g_wakes = 0;
    vm_write32(LINE_A, vm_read32(LINE_A));
    for (unsigned i = 0; i < N; ++i) {
        CHECK((c[i]->resv & 1u) == 0);
        CHECK(c[i]->event_status & SPU_EVENT_LR);
    }
    CHECK(g_wakes == N);
    TEST("ABA writes also invalidate every context even though bytes match again");
    for (unsigned i = 0; i < N; ++i) {
        c[i]->event_status = 0;
        spu_getllar(c[i], LINE_B);
    }
    uint32_t old = vm_read32(LINE_B);
    g_wakes = 0;
    vm_write32(LINE_B, old ^ 1u);
    vm_write32(LINE_B, old);
    for (unsigned i = 0; i < N; ++i) {
        CHECK((c[i]->resv & 1u) == 0);
        CHECK(c[i]->event_status & SPU_EVENT_LR);
    }
    CHECK(g_wakes == N);
    TEST("unregister and same-address reuse preserve only current holders");
    for (unsigned i = 0; i < N; ++i) {
        c[i]->event_status = 0;
        spu_getllar(c[i], LINE_C);
        if (i & 1) spu_coh_unregister(c[i]);
    }
    g_wakes = 0;
    vm_write32(LINE_C, vm_read32(LINE_C));
    for (unsigned i = 0; i < N; ++i)
        CHECK(!!(c[i]->event_status & SPU_EVENT_LR) == !(i & 1));
    CHECK(g_wakes == N / 2);
    for (unsigned i = 0; i < N; ++i) {
        c[i]->event_status = 0;
        spu_getllar(c[i], LINE_B);
    }
    g_wakes = 0;
    vm_write32(LINE_B, vm_read32(LINE_B));
    CHECK(g_wakes == N);
    for (unsigned i = 0; i < N; ++i) {
        spu_coh_unregister(c[i]);
        spu_coh_unregister(c[i]);
        free(c[i]);
    }
    g_wakes = 0;
    vm_write32(LINE_B, vm_read32(LINE_B));
    CHECK(g_wakes == 0);
    TEST("range cleanup removes adjacent entries without dropping other holders");
    spu_context* range = calloc(3, sizeof(*range));
    CHECK(range != NULL);
    for (unsigned i = 0; i < 3; ++i) {
        spu_reset(&range[i], i);
        spu_getllar(&range[i], LINE_A);
    }
    spu_reset(&g_spu_a, 0);
    spu_getllar(&g_spu_a, LINE_A);
    spu_coh_forget_range((uintptr_t)range, (uintptr_t)(range + 3));
    free(range);
    g_wakes = 0;
    vm_write32(LINE_A, vm_read32(LINE_A));
    CHECK(g_wakes == 1 && !(g_spu_a.resv & 1u));
    spu_coh_unregister(&g_spu_a);
}

int main(void)
{
    test_bitmap();
    test_ppu_store_raises_lr();
    test_no_false_raise();
    test_all_store_widths();
    spu_coh_unregister(&g_spu_a);
    spu_coh_unregister(&g_spu_b);
    test_many_contexts();

    printf("\nSPU lock-line coherence tests: %d passed, %d failed\n",
           g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
