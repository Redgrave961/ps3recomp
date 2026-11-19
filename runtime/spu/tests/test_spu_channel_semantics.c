#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t g_guest_mem[1u << 20];
uint8_t* vm_base = g_guest_mem;
uint32_t ppu_vm_size = 0;
void spu_thread_publish_ctx(uint32_t tid, void* ctx) { (void)tid; (void)ctx; }

#include "../spu_channels.c"
#include "../../platform/ps3_sync.h"

static int g_pass, g_fail;
#define CHECK(cond) do {                                                    \
    if (cond) g_pass++;                                                     \
    else { g_fail++; fprintf(stderr, "FAIL at %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

static spu_context g_spu;

static void test_wake_reaches_a_published_sleeper(void)
{
    static spu_context c;
    spu_context_init(&c, 3);

    const uint32_t idle = __atomic_load_n(&c.ch_wake_seq, __ATOMIC_SEQ_CST);
    spu_channel_write(&c.ch_in_mbox, 0x11111111u);
    spu_ch_wake(&c);
    CHECK(__atomic_load_n(&c.ch_wake_seq, __ATOMIC_SEQ_CST) == idle);
    (void)spu_channel_read(&c.ch_in_mbox);

    const uint32_t snap = __atomic_load_n(&c.ch_wake_seq, __ATOMIC_ACQUIRE);
    __atomic_store_n(&c.ch_sleeping, 1, __ATOMIC_SEQ_CST);
    CHECK(!spu_ch_ready(&c, SPU_RdInMbox));
    spu_channel_write(&c.ch_in_mbox, 0x5A5A5A5Au);
    spu_ch_wake(&c);
    CHECK(__atomic_load_n(&c.ch_wake_seq, __ATOMIC_SEQ_CST) != snap);
    CHECK(spu_ch_ready(&c, SPU_RdInMbox));
    __atomic_store_n(&c.ch_sleeping, 0, __ATOMIC_SEQ_CST);
    CHECK(spu_channel_read(&c.ch_in_mbox) == 0x5A5A5A5Au);
}

static spu_context g_hs_spu;
static volatile LONG g_hs_ready, g_hs_got, g_hs_bad;
static int g_hs_total, g_hs_late;
static uint64_t g_hs_stamp, g_hs_slowest;

static DWORD WINAPI handoff_reader(LPVOID arg)
{
    (void)arg;
    for (int i = 0; i < g_hs_total; i++) {
        InterlockedExchange(&g_hs_ready, i + 1);
        const uint32_t v = spu_rdch(&g_hs_spu, SPU_RdInMbox)._u32[0];
        const uint64_t dt = ps3_sync_now_ns() - __atomic_load_n(&g_hs_stamp, __ATOMIC_SEQ_CST);
        if (v != 0xC0DE0000u + (uint32_t)i) InterlockedExchange(&g_hs_bad, 1);
        if (dt > 5000000u) g_hs_late++;
        if (dt > g_hs_slowest) g_hs_slowest = dt;
        InterlockedExchange(&g_hs_got, i + 1);
    }
    return 0;
}

static void test_blocking_read_handoff(int n)
{
    spu_context_init(&g_hs_spu, 1);
    g_hs_spu.status = SPU_STATUS_RUNNING;
    g_hs_total = n;
    g_hs_ready = g_hs_got = g_hs_bad = 0;
    g_hs_late = 0;
    g_hs_slowest = 0;
    HANDLE t = CreateThread(NULL, 0, handoff_reader, NULL, 0, NULL);
    for (int i = 0; i < n; i++) {
        while (InterlockedCompareExchange(&g_hs_ready, 0, 0) != i + 1) Sleep(0);
        for (volatile int k = 0; k < (i * 7) % 300; k++) { }
        __atomic_store_n(&g_hs_stamp, ps3_sync_now_ns(), __ATOMIC_SEQ_CST);
        spu_channel_write(&g_hs_spu.ch_in_mbox, 0xC0DE0000u + (uint32_t)i);
        spu_ch_wake(&g_hs_spu);
        while (InterlockedCompareExchange(&g_hs_got, 0, 0) != i + 1) Sleep(0);
    }
    CHECK(WaitForSingleObject(t, 30000) == WAIT_OBJECT_0);
    CloseHandle(t);
    CHECK(!g_hs_bad);
    printf("%d blocking mailbox handoffs: %d slower than 5 ms, slowest %.3f ms\n",
           n, g_hs_late, (double)g_hs_slowest / 1e6);
}

static uint32_t rdch(uint32_t ch) { return spu_rdch(&g_spu, ch)._u32[0]; }
static void wrch(uint32_t ch, uint32_t v) { spu_wrch(&g_spu, ch, spu_splat_u32(v)); }

static void test_event_status_is_pending_and_enabled(void)
{
    g_spu.event_status = 0;
    wrch(SPU_WrEventMask, 0x0002u);
    SPU_EVENT_SET(&g_spu, 0x0002u | 0x0010u);
    CHECK(rdch(SPU_RdEventStat) == 0x0002u);

    wrch(SPU_WrEventMask, 0x0010u | 0x0002u);
    CHECK(rdch(SPU_RdEventStat) == (0x0002u | 0x0010u));

    wrch(SPU_WrEventMask, 0x0004u);
    g_spu.event_status = 0x0002u;
    CHECK(spu_rchcnt(&g_spu, SPU_RdEventStat) == 0);
    wrch(SPU_WrEventMask, 0);
    CHECK(rdch(SPU_RdEventStat) == 0);
    g_spu.event_status = 0;
}

static void test_srr0_is_a_word_local_store_address(void)
{
    wrch(SPU_WrSRR0, 0x12345678u);
    CHECK(rdch(SPU_RdSRR0) == (0x12345678u & 0x3FFFCu));
    wrch(SPU_WrSRR0, 0xFFFFFFFFu);
    CHECK(rdch(SPU_RdSRR0) == 0x3FFFCu);
    wrch(SPU_WrSRR0, 0x2040u);
    CHECK(rdch(SPU_RdSRR0) == 0x2040u);
}

static void atomic_cmd(spu_context* c, uint32_t ea, uint32_t lsa, uint32_t cmd)
{
    c->mfc_eah = 0; c->mfc_eal = ea; c->mfc_lsa = lsa;
    (void)spu_mfc_atomic(c, cmd);
}

static void test_atomic_commands_use_an_aligned_lsa(void)
{
    static spu_context c;
    const uint32_t ea = 0x40000;
    uint8_t line[128], other[128];
    for (int i = 0; i < 128; i++) { line[i] = (uint8_t)(0x10 + i); other[i] = (uint8_t)(0xA0 ^ i); }
    spu_context_init(&c, 2);
    c.image_id = -1;

    memcpy(g_guest_mem + ea, line, 128);
    memset(c.ls, 0xEE, SPU_LS_SIZE);
    atomic_cmd(&c, ea, 0x2000 + 0x4C, MFC_GETLLAR_CMD);
    CHECK(memcmp(c.ls + 0x2000, line, 128) == 0);
    CHECK(c.ls[0x1FFF] == 0xEE && c.ls[0x2080] == 0xEE);
    CHECK((c.resv & 1u));

    memcpy(c.ls + 0x2000, other, 128);
    atomic_cmd(&c, ea, 0x2000 + 0x7F, MFC_PUTLLC_CMD);
    CHECK(c.atomic_stat == 0);
    CHECK(memcmp(g_guest_mem + ea, other, 128) == 0);

    memcpy(c.ls + 0x3000, line, 128);
    atomic_cmd(&c, ea + 0x80, 0x3000 + 0x01, MFC_PUTLLUC_CMD);
    CHECK(memcmp(g_guest_mem + ea + 0x80, line, 128) == 0);

    {
        const size_t after = offsetof(spu_context, ls_store) + SPU_LS_SIZE;
        uint8_t tail[128], tail_before[128];
        memcpy(tail_before, (uint8_t*)&c + after, sizeof tail_before);
        memcpy(g_guest_mem + ea, line, 128);
        atomic_cmd(&c, ea, SPU_LS_SIZE - 16, MFC_GETLLAR_CMD);
        memcpy(tail, (uint8_t*)&c + after, sizeof tail);
        CHECK(memcmp(tail, tail_before, sizeof tail) == 0);
        CHECK(memcmp(c.ls + SPU_LS_SIZE - 128, line, 128) == 0);
    }
}

int main(int argc, char** argv)
{
    const int handoffs = argc > 1 ? atoi(argv[1]) : 2000;

    spu_context_init(&g_spu, 0);
    g_spu.image_id = -1;

    test_wake_reaches_a_published_sleeper();
    test_blocking_read_handoff(handoffs);
    test_event_status_is_pending_and_enabled();
    test_srr0_is_a_word_local_store_address();
    test_atomic_commands_use_an_aligned_lsa();

    printf("SPU channel semantics: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
