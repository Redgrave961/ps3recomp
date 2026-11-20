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
#define SPUS    6
#define WKLS    3
#define PUBS    2
#define ROUNDS  3000

static int s_fail;
#define CHECK(cond, ...) do { if (!(cond)) { s_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const uint16_t s_run[SPUS] = { 0xE000, 0xE000, 0xC000, 0x8000, 0x8000, 0xA000 };
static const uint8_t s_mc[WKLS] = { 4, 2, 1 };

static spu_context* s_spu[SPUS];
static volatile LONG s_taken[WKLS], s_published[WKLS], s_bounce, s_round, s_done[PUBS], s_stop, s_over;

static uint32_t rnd(uint32_t* s)
{
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return *s;
}

static void busy_ns(uint64_t ns)
{
    const uint64_t end = ps3_sync_now_ns() + ns;
    while (ps3_sync_now_ns() < end) {}
}

static void line_add(uint32_t off, int delta)
{
    spu_lockline_lock(SPURS);
    vm_base[SPURS + off] = (uint8_t)(vm_base[SPURS + off] + delta);
    spu_coh_notify_write(SPURS);
    spu_lockline_unlock(SPURS);
}

static void finish(uint32_t w)
{
    spu_lockline_lock(SPURS);
    if (vm_base[SPURS + 0x20 + w] > s_mc[w] || !vm_base[SPURS + 0x20 + w]) InterlockedExchange(&s_over, 1);
    vm_base[SPURS + w]--;
    vm_base[SPURS + 0x20 + w]--;
    spu_coh_notify_write(SPURS);
    spu_lockline_unlock(SPURS);
}

static void idle_entry(spu_context* c) { ss_idle(c); }

static DWORD WINAPI spu_thread(LPVOID p)
{
    spu_context* c = (spu_context*)p;
    uint32_t seed = 0x9E3779B9u ^ c->spu_id;
    while (!s_stop) {
        p32(c->ls + 0x1DC, 0x20);
        memset(c->ls + 0x180, 0, 32);
        if (spu_run_with_halt(idle_entry, c)) break;
        uint32_t r[4];
        kernel_select(c, 1, r);
        if (r[0] == 0x20) {
            InterlockedIncrement(&s_bounce);
            continue;
        }
        busy_ns(rnd(&seed) % 3000u);
        kernel_select(c, 0, r);
        if (r[0] >= WKLS) {
            InterlockedIncrement(&s_bounce);
            continue;
        }
        InterlockedIncrement(&s_taken[r[0]]);
        busy_ns(rnd(&seed) % 20000u);
        finish(r[0]);
    }
    return 0;
}

static DWORD WINAPI pub_thread(LPVOID p)
{
    const int id = (int)(intptr_t)p;
    uint32_t seed = 0x2545F491u + 77u * (uint32_t)id;
    for (LONG round = 0; round < ROUNDS; round++) {
        while (s_round < round && !s_stop) {}
        if (s_stop) break;
        const uint32_t n = 1u + rnd(&seed) % 3u;
        for (uint32_t i = 0; i < n; i++) {
            busy_ns(rnd(&seed) % 15000u);
            if (rnd(&seed) % 4u == 0) line_add(0x7F, 1);
            const uint32_t w = rnd(&seed) % WKLS;
            InterlockedIncrement(&s_published[w]);
            line_add(w, 1);
        }
        InterlockedExchange(&s_done[id], round + 1);
    }
    return 0;
}

static spu_context* sleeper(uint32_t spu, uint16_t run, uint32_t line)
{
    spu_context* c = (spu_context*)calloc(1, sizeof(spu_context));
    spu_context_init(c, 0x3000 + spu);
    c->status = SPU_STATUS_RUNNING;
    c->image_id = SPURS_IMAGE;
    p64(c->ls + 0x1C0, line);
    p32(c->ls + 0x1C8, spu);
    p16(c->ls + 0x1EC, run);
    for (int w = 0; w < WKLS; w++) c->ls[0x1A0 + w] = 1;
    ss_idle_reserve(c, line);
    return c;
}

static void line_set(uint32_t line, uint32_t off, uint8_t v)
{
    spu_lockline_lock(line);
    vm_base[line + off] = v;
    spu_coh_notify_write(line);
    spu_lockline_unlock(line);
}

static void claim_handoff(void)
{
    const uint32_t line = SPURS + 0x1000;
    p32(vm_base + line + 0x6C, 0xFFFFFFFFu);
    vm_base[line + 0x77] = 0xFF;
    vm_base[line + 0x76] = 2;
    vm_base[line + 0x73] = 3;
    vm_base[line + 0x50] = 1;
    vm_base[line + 0x52] = 1;
    spu_context* y = sleeper(0, 0x8000, line);
    spu_context* x = sleeper(1, 0xA000, line);
    line_set(line, 0x00, 1);
    CHECK(y->resv == line && y->idle_claim == (line | 1u), "first sleeper not claimed for the new unit: resv %X claim %X", y->resv, y->idle_claim);
    CHECK(x->resv == (line | 1u | SPU_RESV_BY_CONTENT), "second sleeper woken for a unit already claimed: resv %X", x->resv);
    line_set(line, 0x20, 1);
    line_set(line, 0x02, 1);
    CHECK(x->resv == (line | 1u | SPU_RESV_BY_CONTENT), "sleeper woken while a claim covers the free slot: resv %X", x->resv);
    ss_idle_reserve(y, line);
    CHECK(!y->idle_claim, "claim kept by a sleeper with no work");
    CHECK(x->resv == line && (x->event_status & SPU_EVENT_LR) && x->idle_claim == (line | 1u), "ended claim lost the wake of the only eligible sleeper: resv %X", x->resv);
}

static int drained(void)
{
    for (int w = 0; w < WKLS; w++)
        if (s_taken[w] != s_published[w] || vm_base[SPURS + w] || vm_base[SPURS + 0x20 + w] || vm_base[SPURS + 0x30 + w]) return 0;
    return 1;
}

int main(void)
{
    vm_base = (uint8_t*)calloc(1, VM_SIZE);
    ppu_vm_size = VM_SIZE;
    p32(vm_base + SPURS + 0x6C, 0xFFFFFFFFu);
    vm_base[SPURS + 0x77] = 0xFF;
    vm_base[SPURS + 0x76] = SPUS;
    for (int w = 0; w < WKLS; w++) vm_base[SPURS + 0x50 + w] = s_mc[w];
    spurs_register(0x30021480u, 0x30023680u, 0x3002A180u, 0x30026300u);
    claim_handoff();
    HANDLE t[SPUS + PUBS];
    for (int i = 0; i < SPUS; i++) {
        s_spu[i] = (spu_context*)calloc(1, sizeof(spu_context));
        spu_context_init(s_spu[i], 0x2000 + i);
        s_spu[i]->status = SPU_STATUS_RUNNING;
        s_spu[i]->image_id = SPURS_IMAGE;
        p64(s_spu[i]->ls + 0x1C0, SPURS);
        p32(s_spu[i]->ls + 0x1C8, (uint32_t)i);
        p32(s_spu[i]->ls + 0x1CC, 0x1F);
        p16(s_spu[i]->ls + 0x1EC, s_run[i]);
        for (int w = 0; w < WKLS; w++) s_spu[i]->ls[0x1A0 + w] = (uint8_t)(1 + w);
        t[i] = CreateThread(NULL, 0, spu_thread, s_spu[i], 0, NULL);
    }
    Sleep(100);
    const unsigned long raise0 = g_spu_lr_raise;
    for (int i = 0; i < PUBS; i++) t[SPUS + i] = CreateThread(NULL, 0, pub_thread, (LPVOID)(intptr_t)i, 0, NULL);
    for (LONG round = 0; round < ROUNDS && !s_fail; round++) {
        const uint64_t limit = ps3_sync_now_ns() + 5000000000ull;
        for (int i = 0; i < PUBS; i++)
            while (s_done[i] <= round && ps3_sync_now_ns() < limit) Sleep(0);
        while (!drained() && ps3_sync_now_ns() < limit) Sleep(0);
        CHECK(drained(), "round %ld: taken %ld/%ld %ld/%ld %ld/%ld ready %u %u %u contention %u %u %u pending %u %u %u (lost wake)", (long)round,
              (long)s_taken[0], (long)s_published[0], (long)s_taken[1], (long)s_published[1], (long)s_taken[2], (long)s_published[2],
              vm_base[SPURS], vm_base[SPURS + 1], vm_base[SPURS + 2], vm_base[SPURS + 0x20], vm_base[SPURS + 0x21], vm_base[SPURS + 0x22],
              vm_base[SPURS + 0x30], vm_base[SPURS + 0x31], vm_base[SPURS + 0x32]);
        InterlockedExchange(&s_round, round + 1);
    }
    CHECK(!s_over, "a workload ran above its max contention");
    s_stop = 1;
    for (int i = 0; i < SPUS; i++) {
        s_spu[i]->stop_request = 1;
        spu_ch_wake(s_spu[i]);
    }
    CHECK(WaitForMultipleObjects(SPUS + PUBS, t, TRUE, 5000) == WAIT_OBJECT_0, "threads did not stop");
    printf("spurs idle claims: %ld units, %lu reservation wakes, %ld bounces, %s\n",
           (long)(s_taken[0] + s_taken[1] + s_taken[2]), g_spu_lr_raise - raise0, (long)s_bounce, s_fail ? "FAIL" : "PASS");
    return s_fail ? 1 : 0;
}
