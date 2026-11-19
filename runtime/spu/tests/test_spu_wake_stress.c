#include "../spu_dma.h"
#include "../spu_coherency.h"
#include "../../platform/ps3_sync.h"
#include "../../platform/win32_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

u128 spu_rdch(spu_context* ctx, uint32_t channel);
void spu_wrch(spu_context* ctx, uint32_t channel, u128 value);

uint8_t* vm_base;
uint32_t ppu_vm_size;
void spu_thread_publish_ctx(uint32_t tid, void* c) { (void)tid; (void)c; }

#define VM_SIZE 0x100000u
#define LINE    0x40000u

static int s_fail;
#define CHECK(cond, ...) do { if (!(cond)) { s_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t s_rng = 0x12345678u;
static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static void busy_ns(uint64_t ns)
{
    const uint64_t end = ps3_sync_now_ns() + ns;
    while (ps3_sync_now_ns() < end) ps3_pause();
}

static void pace(void)
{
    const uint32_t r = rnd() % 8u;
    if (r < 3u) return;
    if (r < 6u) busy_ns(rnd() % 3000u);
    else busy_ns(20000u + rnd() % 80000u);
}

static int join(HANDLE t, DWORD ms)
{
    const int ok = WaitForSingleObject(t, ms) == WAIT_OBJECT_0;
    if (ok) CloseHandle(t);
    return ok;
}

#define MBOX_N 60000

static spu_context* s_mbox;
static volatile LONG s_mbox_got;
static volatile LONG s_mbox_bad;

static DWORD WINAPI mbox_reader(LPVOID p)
{
    (void)p;
    for (int i = 0; i < MBOX_N; i++) {
        const uint32_t v = spu_rdch(s_mbox, SPU_RdInMbox)._u32[0];
        if (v != 0xC0DE0000u + (uint32_t)i) InterlockedExchange(&s_mbox_bad, 1);
        InterlockedExchange(&s_mbox_got, i + 1);
    }
    return 0;
}

static void mailbox_handoff_stress(void)
{
    s_mbox = (spu_context*)calloc(1, sizeof(spu_context));
    spu_context_init(s_mbox, 1);
    s_mbox->status = SPU_STATUS_RUNNING;
    s_mbox_got = 0;
    s_mbox_bad = 0;
    HANDLE t = CreateThread(NULL, 0, mbox_reader, NULL, 0, NULL);
    for (int i = 0; i < MBOX_N; i++) {
        pace();
        spu_channel_write(&s_mbox->ch_in_mbox, 0xC0DE0000u + (uint32_t)i);
        spu_ch_wake(s_mbox);
        if (rnd() & 1u) {
            const uint64_t limit = ps3_sync_now_ns() + 10000000000ull;
            while (InterlockedCompareExchange(&s_mbox_got, 0, 0) != i + 1 && ps3_sync_now_ns() < limit) ps3_pause();
        }
    }
    CHECK(join(t, 30000), "mailbox reader stranded at %ld of %d", (long)s_mbox_got, MBOX_N);
    CHECK(!s_mbox_bad, "mailbox words out of order");
}

static void content_reserve(spu_context* c, uint32_t ea)
{
    spu_lockline_lock(ea);
    spu_coh_reserve(c, ea);
    memcpy(c->ls + 0x100, vm_base + ea, MFC_ATOMIC_LINE);
    memcpy(c->resv_line, c->ls + 0x100, MFC_ATOMIC_LINE);
    __atomic_store_n(&c->resv, ea | 1u | SPU_RESV_BY_CONTENT, __ATOMIC_RELAXED);
    spu_lockline_unlock(ea);
}

static void line_store(uint32_t ea, uint32_t off, uint32_t v)
{
    spu_lockline_lock(ea);
    memcpy(vm_base + ea + off, &v, 4);
    spu_coh_notify_write(ea);
    spu_lockline_unlock(ea);
}

static void content_reservation_semantics(void)
{
    spu_context* c = (spu_context*)calloc(1, sizeof(spu_context));
    spu_context_init(c, 2);
    uint32_t v = 7;
    memcpy(vm_base + LINE + 8, &v, 4);
    content_reserve(c, LINE);
    line_store(LINE, 8, 7);
    CHECK(c->resv == (LINE | 1u | SPU_RESV_BY_CONTENT) && !(c->event_status & SPU_EVENT_LR), "identical store ended a content reservation");
    line_store(LINE, 8, 8);
    CHECK(!(c->resv & 1u) && (c->event_status & SPU_EVENT_LR), "changed store kept a content reservation");

    c->event_status = 0;
    c->resv = LINE | 1u;
    memcpy(c->resv_line, vm_base + LINE, MFC_ATOMIC_LINE);
    line_store(LINE, 8, 8);
    CHECK(!(c->resv & 1u) && (c->event_status & SPU_EVENT_LR), "identical store kept an ordinary reservation");

    c->event_status = 0;
    content_reserve(c, LINE);
    c->mfc_eah = 0;
    c->mfc_eal = LINE;
    c->mfc_lsa = 0x100;
    c->ls[0x100 + 8] = 0x55;
    line_store(LINE, 8, 8);
    spu_wrch(c, MFC_Cmd, spu_make_preferred_u32(MFC_PUTLLC_CMD));
    CHECK(c->atomic_stat == 0 && vm_base[LINE + 8] == 0x55 && c->resv == 0, "content PUTLLC after an identical store failed");

    content_reserve(c, LINE);
    line_store(LINE, 12, 1);
    spu_wrch(c, MFC_Cmd, spu_make_preferred_u32(MFC_PUTLLC_CMD));
    CHECK(c->atomic_stat == 1 && c->resv == 0 && vm_base[LINE + 12] == 1, "content PUTLLC after a changed store succeeded");
    spu_coh_unregister(c);
    free(c);
}

#define EVT_N 40000

static spu_context* s_evt;
static volatile LONG s_evt_seen;

static DWORD WINAPI event_waiter(LPVOID p)
{
    (void)p;
    s_evt->event_mask = SPU_EVENT_LR;
    uint32_t last = 0;
    while (last < EVT_N) {
        content_reserve(s_evt, LINE);
        uint32_t now;
        memcpy(&now, s_evt->ls + 0x100, 4);
        if (now == last) {
            spu_rdch(s_evt, SPU_RdEventStat);
            spu_wrch(s_evt, SPU_WrEventAck, spu_make_preferred_u32(SPU_EVENT_LR));
            continue;
        }
        last = now;
        InterlockedExchange(&s_evt_seen, (LONG)now);
    }
    return 0;
}

static void reservation_wake_stress(void)
{
    memset(vm_base + LINE, 0, MFC_ATOMIC_LINE);
    s_evt = (spu_context*)calloc(1, sizeof(spu_context));
    spu_context_init(s_evt, 3);
    s_evt->status = SPU_STATUS_RUNNING;
    s_evt_seen = 0;
    HANDLE t = CreateThread(NULL, 0, event_waiter, NULL, 0, NULL);
    for (uint32_t i = 1; i <= EVT_N; i++) {
        pace();
        if (rnd() & 1u) line_store(LINE, 4, rnd());
        line_store(LINE, 0, i);
        if (rnd() & 1u) {
            const uint64_t limit = ps3_sync_now_ns() + 10000000000ull;
            while ((uint32_t)InterlockedCompareExchange(&s_evt_seen, 0, 0) != i && ps3_sync_now_ns() < limit) ps3_pause();
        }
    }
    CHECK(join(t, 30000), "reservation waiter stranded at %ld of %d", (long)s_evt_seen, EVT_N);
    spu_coh_unregister(s_evt);
}

#define WAITER_N 60000

static ps3_lock s_wq_lock;
static ps3_waitq s_wq;
static volatile LONG s_wq_done, s_wq_bad, s_wq_timeouts;

static DWORD WINAPI waiter_sleeper(LPVOID p)
{
    (void)p;
    for (int i = 0; i < WAITER_N; i++) {
        ps3_waiter w;
        ps3_waiter_init(&w, 1, 0);
        ps3_lock_acquire(&s_wq_lock);
        ps3_waitq_push(&s_wq, &w);
        ps3_lock_release(&s_wq_lock);
        const int timed = (i % 5) == 4;
        if (!ps3_waiter_sleep(&w, timed ? ps3_sync_now_ns() + 30000u : 0)) {
            ps3_lock_acquire(&s_wq_lock);
            const int removed = ps3_waitq_remove(&s_wq, &w);
            ps3_lock_release(&s_wq_lock);
            if (removed) {
                InterlockedIncrement(&s_wq_timeouts);
                continue;
            }
            ps3_waiter_sleep(&w, 0);
        }
        if (w.result != 0x1234 + i % 7) InterlockedExchange(&s_wq_bad, 1);
        InterlockedIncrement(&s_wq_done);
    }
    return 0;
}

static DWORD WINAPI waiter_waker(LPVOID p)
{
    (void)p;
    while (InterlockedCompareExchange(&s_wq_done, 0, 0) + InterlockedCompareExchange(&s_wq_timeouts, 0, 0) < WAITER_N) {
        pace();
        ps3_lock_acquire(&s_wq_lock);
        ps3_waiter* w = ps3_waitq_pop(&s_wq, PS3_SYNC_FIFO, 0);
        ps3_lock_release(&s_wq_lock);
        if (w) ps3_waiter_wake(w, 0x1234 + (InterlockedCompareExchange(&s_wq_done, 0, 0) + InterlockedCompareExchange(&s_wq_timeouts, 0, 0)) % 7);
    }
    return 0;
}

static void waiter_handoff_stress(void)
{
    HANDLE a = CreateThread(NULL, 0, waiter_sleeper, NULL, 0, NULL);
    HANDLE b = CreateThread(NULL, 0, waiter_waker, NULL, 0, NULL);
    CHECK(join(a, 60000), "waiter stranded at %ld (+%ld timeouts) of %d", (long)s_wq_done, (long)s_wq_timeouts, WAITER_N);
    CHECK(join(b, 60000), "waker did not finish");
    CHECK(!s_wq_bad, "waiter got another wake's result");
}

int main(void)
{
    vm_base = (uint8_t*)calloc(1, VM_SIZE);
    ppu_vm_size = VM_SIZE;
    content_reservation_semantics();
    mailbox_handoff_stress();
    reservation_wake_stress();
    waiter_handoff_stress();
    printf("spu wake stress: %s\n", s_fail ? "FAIL" : "PASS");
    return s_fail ? 1 : 0;
}
