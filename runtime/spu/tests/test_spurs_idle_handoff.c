#include "../spurs.c"
#include "../../platform/win32_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;
void spu_thread_publish_ctx(uint32_t tid, void* c) { (void)tid; (void)c; }

#define VM_SIZE 0x200000u
#define SPURS   0x100000u
#define SPUS    4
#define ROUNDS  4000

static int s_fail;
#define CHECK(cond, ...) do { if (!(cond)) { s_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static spu_context* s_spu[SPUS];
static volatile LONG s_taken, s_stop, s_bad;

static uint32_t s_rng = 0x2545F491u;
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
    while (ps3_sync_now_ns() < end) {}
}

static void line_update(int ready_delta, int contention_delta, uint32_t max_contention)
{
    spu_lockline_lock(SPURS);
    vm_base[SPURS + 0x00] = (uint8_t)(vm_base[SPURS + 0x00] + ready_delta);
    vm_base[SPURS + 0x20] = (uint8_t)(vm_base[SPURS + 0x20] + contention_delta);
    if (max_contention) vm_base[SPURS + 0x50] = (uint8_t)max_contention;
    spu_coh_notify_write(SPURS);
    spu_lockline_unlock(SPURS);
}

static void idle_entry(spu_context* c) { ss_idle(c); }

static DWORD WINAPI spu_thread(LPVOID p)
{
    spu_context* c = (spu_context*)p;
    while (!s_stop) {
        p32(c->ls + 0x1DC, 0x20);
        memset(c->ls + 0x180, 0, 32);
        if (spu_run_with_halt(idle_entry, c)) break;
        uint32_t r[4];
        kernel_select(c, 0, r);
        if (r[0] != 0) continue;
        InterlockedIncrement(&s_taken);
        busy_ns(rnd() % 20000u);
        if (vm_base[SPURS + 0x20] == 0) InterlockedExchange(&s_bad, 1);
        line_update(-1, -1, 0);
    }
    return 0;
}

int main(void)
{
    vm_base = (uint8_t*)calloc(1, VM_SIZE);
    ppu_vm_size = VM_SIZE;
    p32(vm_base + SPURS + 0x6C, 0xFFFFFFFFu);
    vm_base[SPURS + 0x77] = 0xFF;
    vm_base[SPURS + 0x76] = SPUS;
    spurs_register(0x30021480u, 0x30023680u, 0x3002A180u, 0x30026300u);
    HANDLE t[SPUS];
    for (int i = 0; i < SPUS; i++) {
        s_spu[i] = (spu_context*)calloc(1, sizeof(spu_context));
        spu_context_init(s_spu[i], 0x2000 + i);
        s_spu[i]->status = SPU_STATUS_RUNNING;
        s_spu[i]->image_id = SPURS_IMAGE;
        p64(s_spu[i]->ls + 0x1C0, SPURS);
        p32(s_spu[i]->ls + 0x1C8, (uint32_t)i);
        p32(s_spu[i]->ls + 0x1CC, 0x1F);
        p16(s_spu[i]->ls + 0x1EC, 0x8000);
        s_spu[i]->ls[0x1A0] = 1;
        t[i] = CreateThread(NULL, 0, spu_thread, s_spu[i], 0, NULL);
    }
    Sleep(100);
    for (int round = 0; round < ROUNDS && !s_fail; round++) {
        const uint32_t k = 1u + rnd() % SPUS;
        const LONG before = s_taken;
        if (rnd() & 1u) busy_ns(rnd() % 50000u);
        line_update((int)k, 0, k);
        const uint64_t limit = ps3_sync_now_ns() + 5000000000ull;
        while (s_taken - before < (LONG)k && ps3_sync_now_ns() < limit) Sleep(0);
        CHECK(s_taken - before >= (LONG)k, "round %d: %ld of %u published units taken (lost wake)", round, (long)(s_taken - before), k);
        while ((vm_base[SPURS + 0x00] || vm_base[SPURS + 0x20]) && ps3_sync_now_ns() < limit) Sleep(0);
        CHECK(!vm_base[SPURS + 0x00] && !vm_base[SPURS + 0x20], "round %d: ready %u contention %u left", round, vm_base[SPURS], vm_base[SPURS + 0x20]);
    }
    CHECK(!s_bad, "a taker saw zero contention");
    s_stop = 1;
    for (int i = 0; i < SPUS; i++) {
        s_spu[i]->stop_request = 1;
        spu_ch_wake(s_spu[i]);
    }
    CHECK(WaitForMultipleObjects(SPUS, t, TRUE, 5000) == WAIT_OBJECT_0, "idle SPUs did not stop");
    printf("spurs idle handoff: %ld units taken, %s\n", (long)s_taken, s_fail ? "FAIL" : "PASS");
    return s_fail ? 1 : 0;
}
