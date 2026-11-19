#include "../spurs.c"
#include "../spu_overlay_residency.h"
#include "../../platform/win32_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;

#define VM_SIZE   0x1000000u
#define SPURS     0x100000u
#define TASKSET   0x200000u
#define CHAIN     0x300000u
#define LIST_A    0x310000u
#define LIST_B    0x311000u
#define DESC      0x320000u
#define BINARY    0x330000u
#define GUARD     0x340000u
#define JOBLIST   0x350000u
#define JOB_IMAGE 0x7000
#define JOB_CANON 0x4C00u

static spu_context* C;
static int s_fail;
static const char* s_case = "";

#define CHECK(cond, ...) do { if (!(cond)) { s_fail++; printf("FAIL [%s] %s:%d: ", s_case, __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint8_t* vm(uint32_t ea) { return vm_base + ea; }
static uint8_t* ls(uint32_t a) { return C->ls + a; }

static void fresh(uint32_t spu, uint32_t cur)
{
    spu_coh_unregister(C);
    spu_mfc_release(C);
    memset(vm_base, 0, VM_SIZE);
    spu_context_init(C, 0x2000);
    C->status = SPU_STATUS_RUNNING;
    C->image_id = SPURS_IMAGE;
    p64(ls(0x1C0), SPURS);
    p32(ls(0x1C8), spu);
    p32(ls(0x1CC), 0x1F);
    p32(ls(0x1D8), 0x20);
    p32(ls(0x1DC), cur);
    p32(vm(SPURS + 0x6C), 0xFFFFFFFFu);
    vm(SPURS)[0x77] = 0xFF;
}

static void kernel_tests(void)
{
    uint32_t r[4], st;

    s_case = "select basic";
    fresh(0, 0x20);
    vm(SPURS)[0x00] = 1;
    vm(SPURS)[0x50] = 1;
    p16(ls(0x1EC), 0x8000);
    ls(0x1A0)[0] = 1;
    kernel_select(C, 0, r);
    CHECK(r[0] == 0 && r[1] == 1 && r[2] == 16 && r[3] == 16, "ret %X %X %X %X", r[0], r[1], r[2], r[3]);
    CHECK(vm(SPURS)[0x20] == 1, "contention %u", vm(SPURS)[0x20]);
    CHECK(ls(0x180)[0] == 1 && ls(0x190)[0] == 0, "loc %u pend %u", ls(0x180)[0], ls(0x190)[0]);
    CHECK(g32(ls(0x1DC)) == 0 && ls(0x1EB)[0] == 0, "cur %X idle %u", g32(ls(0x1DC)), ls(0x1EB)[0]);
    CHECK(!memcmp(ls(0x100), vm(SPURS), 128), "LS line-0 mirror differs from the committed line");

    s_case = "select priority";
    fresh(0, 0x20);
    for (int i = 1; i <= 2; i++) {
        vm(SPURS)[0x00 + i] = 1;
        vm(SPURS)[0x50 + i] = 1;
    }
    p16(ls(0x1EC), 0x6000);
    ls(0x1A0)[1] = 3;
    ls(0x1A0)[2] = 5;
    kernel_select(C, 0, r);
    CHECK(r[0] == 2 && r[1] == 1, "ret %X %X", r[0], r[1]);

    s_case = "select tie and unique id";
    fresh(0, 0x20);
    for (int i = 1; i <= 2; i++) {
        vm(SPURS)[0x00 + i] = 1;
        vm(SPURS)[0x50 + i] = 1;
        ls(0x1A0)[i] = 3;
    }
    p16(ls(0x1EC), 0x6000);
    kernel_select(C, 0, r);
    CHECK(r[0] == 1, "tie picks %X", r[0]);
    fresh(0, 0x20);
    for (int i = 1; i <= 2; i++) {
        vm(SPURS)[0x00 + i] = 1;
        vm(SPURS)[0x50 + i] = 1;
        ls(0x1A0)[i] = 3;
    }
    p16(ls(0x1EC), 0x6000);
    ls(0x220)[2] = 0x20;
    kernel_select(C, 0, r);
    CHECK(r[0] == 2, "unique id match picks %X", r[0]);

    s_case = "select contention saturated";
    fresh(0, 0x20);
    vm(SPURS)[0x00] = 1;
    vm(SPURS)[0x50] = 1;
    vm(SPURS)[0x20] = 1;
    p16(ls(0x1EC), 0x8000);
    ls(0x1A0)[0] = 1;
    kernel_select(C, 0, r);
    CHECK(r[0] == 0x20 && r[1] == 0 && r[2] == 0 && r[3] == 0, "ret %X %X %X %X", r[0], r[1], r[2], r[3]);
    CHECK(ls(0x1EB)[0] == 1 && g32(ls(0x1DC)) == 0x20 && vm(SPURS)[0x20] == 1, "idle %u cur %X cont %u", ls(0x1EB)[0], g32(ls(0x1DC)), vm(SPURS)[0x20]);

    s_case = "select raw ready count";
    fresh(0, 0x20);
    vm(SPURS)[0x00] = 10;
    vm(SPURS)[0x10] = 2;
    vm(SPURS)[0x20] = 9;
    vm(SPURS)[0x50] = 10;
    vm(SPURS)[0x01] = 1;
    vm(SPURS)[0x51] = 1;
    p16(ls(0x1EC), 0xC000);
    ls(0x1A0)[0] = 1;
    ls(0x1A0)[1] = 1;
    kernel_select(C, 0, r);
    CHECK(r[0] == 0 && r[1] == 1, "ret %X %X", r[0], r[1]);
    CHECK(vm(SPURS)[0x20] == 10, "contention %u", vm(SPURS)[0x20]);

    s_case = "poll other workload";
    fresh(0, 0);
    vm(SPURS)[0x00] = 1;
    vm(SPURS)[0x50] = 1;
    vm(SPURS)[0x20] = 1;
    ls(0x180)[0] = 1;
    vm(SPURS)[0x01] = 1;
    vm(SPURS)[0x51] = 1;
    p16(vm(SPURS + 0x70), 0x4000);
    p16(ls(0x1EC), 0xC000);
    ls(0x1A0)[0] = 1;
    ls(0x1A0)[1] = 9;
    CHECK(kernel_poll(C, &st) == 1 && st == 3, "poll %u", st);
    CHECK(vm(SPURS)[0x31] == 1 && vm(SPURS)[0x20] == 1, "pending %u contention %u", vm(SPURS)[0x31], vm(SPURS)[0x20]);
    CHECK(g16(vm(SPURS + 0x70)) == 0x4000, "signal %X", g16(vm(SPURS + 0x70)));
    CHECK(ls(0x190)[1] == 1 && ls(0x180)[0] == 1 && g32(ls(0x1DC)) == 0, "loc pend %u loc %u cur %X", ls(0x190)[1], ls(0x180)[0], g32(ls(0x1DC)));

    s_case = "poll same workload";
    fresh(0, 0);
    vm(SPURS)[0x00] = 1;
    vm(SPURS)[0x50] = 1;
    vm(SPURS)[0x20] = 1;
    ls(0x180)[0] = 1;
    p16(vm(SPURS + 0x70), 0x8000);
    p16(ls(0x1EC), 0x8000);
    ls(0x1A0)[0] = 1;
    CHECK(kernel_poll(C, &st) == 0 && st == 3, "poll %u", st);
    CHECK(g16(vm(SPURS + 0x70)) == 0 && ls(0x190)[0] == 0 && vm(SPURS)[0x30] == 0, "signal %X", g16(vm(SPURS + 0x70)));

    s_case = "system service message";
    fresh(0, 0x20);
    vm(SPURS)[0x72] = 1;
    kernel_select(C, 0, r);
    CHECK(r[0] == 0x20 && r[1] == 0 && vm(SPURS)[0x72] == 0 && ls(0x1EB)[0] == 0 && g32(ls(0x1DC)) == 0x20, "ret %X msg %u", r[0], vm(SPURS)[0x72]);
    fresh(0, 0);
    vm(SPURS)[0x72] = 1;
    CHECK(kernel_poll(C, &st) == 1 && vm(SPURS)[0x72] == 1 && g32(ls(0x1DC)) == 0, "message kept in poll mode %u", vm(SPURS)[0x72]);

    s_case = "flag receiver";
    fresh(0, 0x20);
    p32(vm(SPURS + 0x6C), 0);
    vm(SPURS)[0x77] = 3;
    vm(SPURS)[0x53] = 1;
    p16(ls(0x1EC), 0x1000);
    ls(0x1A0)[3] = 1;
    kernel_select(C, 0, r);
    CHECK(r[0] == 3 && r[1] == 4 && g32(vm(SPURS + 0x6C)) == 0xFFFFFFFFu, "ret %X %X flag %X", r[0], r[1], g32(vm(SPURS + 0x6C)));
}

static void idle_entry(spu_context* c) { ss_idle(c); }

static void idle_exit_test(void)
{
    s_case = "idle exit if no work";
    fresh(0, 0x20);
    vm(SPURS)[0x74] = 0x80;
    vm(SPURS)[0x76] = 1;
    vm(SPURS)[0xC8] = 1;
    const int halted = spu_run_with_halt(idle_entry, C);
    CHECK(halted && C->status == SPU_STATUS_STOPPED_BY_STOP && C->stop_code == 0x101, "halted %d status %X code %X", halted, C->status, C->stop_code);
    CHECK(C->ch_out_mbox.count == 1 && C->ch_out_mbox.value == 0, "out mbox %u", C->ch_out_mbox.count);
    CHECK(vm(SPURS)[0xBC] == 0 && vm(SPURS)[0xBD] == 0xFF && vm(SPURS)[0xC8] == 0 && vm(SPURS)[0x73] == 0, "barrier %X msg %X on %X idle %X",
          vm(SPURS)[0xBC], vm(SPURS)[0xBD], vm(SPURS)[0xC8], vm(SPURS)[0x73]);
    CHECK(C->event_mask == SPU_EVENT_LR, "event mask %X", C->event_mask);
}

static volatile LONG s_idle_done;

static DWORD WINAPI idle_thread(LPVOID p)
{
    (void)p;
    spu_run_with_halt(idle_entry, C);
    InterlockedExchange(&s_idle_done, 1);
    return 0;
}

static void idle_wake_test(void)
{
    s_case = "idle blocks until line 0 changes";
    fresh(0, 0x20);
    vm(SPURS)[0x76] = 2;
    vm(SPURS)[0x50] = 1;
    p16(ls(0x1EC), 0x8000);
    ls(0x1A0)[0] = 1;
    s_idle_done = 0;
    HANDLE t = CreateThread(NULL, 0, idle_thread, NULL, 0, NULL);
    Sleep(200);
    CHECK(!s_idle_done && vm(SPURS)[0x73] == 1, "returned %ld idle %X", s_idle_done, vm(SPURS)[0x73]);
    CHECK(C->status == SPU_STATUS_WAITING_CHANNEL, "status %X", C->status);
    spu_lockline_lock(SPURS);
    vm(SPURS)[0x00] = 1;
    spu_coh_notify_write(SPURS);
    spu_lockline_unlock(SPURS);
    CHECK(WaitForSingleObject(t, 5000) == WAIT_OBJECT_0, "idle SPU did not wake");
    CloseHandle(t);
    CHECK(s_idle_done && vm(SPURS)[0x73] == 0, "returned %ld idle %X", s_idle_done, vm(SPURS)[0x73]);
}

static void tset_put(uint32_t off, const uint32_t* tasks, int n)
{
    tset v = { 0, 0 };
    for (int i = 0; i < n; i++) v = ts_or(v, ts_bit(tasks[i]));
    ts_st(vm(TASKSET + off), v);
}

static int tset_has(uint32_t off, uint32_t t) { return ts_any(ts_and(ts_ld(vm(TASKSET + off)), ts_bit(t))); }
static int tset_empty(uint32_t off) { return !ts_any(ts_ld(vm(TASKSET + off))); }

static void ts_fresh(uint32_t cur, uint8_t ready_count)
{
    fresh(0, 3);
    p64(ls(0x27B8), TASKSET);
    p32(ls(0x27D4), cur);
    vm(SPURS)[0x03] = ready_count;
    vm(TASKSET)[0x72] = 0x80;
}

static void ts_halt_entry(spu_context* c) { ts_request(c, 3, 0, 0); }

static void taskset_tests(void)
{
    static const uint32_t t012[] = { 0, 1, 2 };

    s_case = "select round robin";
    ts_fresh(0xFFFFFFFFu, 1);
    tset_put(0x10, t012, 3);
    tset_put(0x30, t012, 3);
    const uint32_t want[4] = { 1, 2, 0, 128 };
    for (int k = 0; k < 4; k++) {
        CHECK(ts_request(C, 5, 0x2C50, 0x2C60) == 0, "rc");
        CHECK(g32(ls(0x2C50)) == want[k] && g32(ls(0x2C60)) == 0, "pick %d: %u", k, g32(ls(0x2C50)));
        if (want[k] < 128) CHECK(tset_has(0x00, want[k]) && vm(TASKSET)[0x73] == want[k], "running/last %u", vm(TASKSET)[0x73]);
    }
    CHECK(vm(TASKSET)[0x73] == 0 && vm(SPURS)[0x03] == 1, "last %u ready count %u", vm(TASKSET)[0x73], vm(SPURS)[0x03]);

    s_case = "select resumes waiting task";
    ts_fresh(0xFFFFFFFFu, 1);
    static const uint32_t t9[] = { 9 };
    tset_put(0x10, t9, 1);
    tset_put(0x30, t9, 1);
    tset_put(0x50, t9, 1);
    ts_request(C, 5, 0x2C50, 0x2C60);
    CHECK(g32(ls(0x2C50)) == 9 && g32(ls(0x2C60)) == 1 && tset_empty(0x50) && tset_has(0x00, 9), "pick %u waiting %u", g32(ls(0x2C50)), g32(ls(0x2C60)));

    s_case = "select skips the workload flag waiter";
    ts_fresh(0xFFFFFFFFu, 2);
    static const uint32_t t23[] = { 2, 3 };
    tset_put(0x10, t23, 2);
    tset_put(0x30, t23, 2);
    vm(TASKSET)[0x72] = 2;
    vm(TASKSET)[0x73] = 1;
    ts_request(C, 5, 0x2C50, 0x2C60);
    CHECK(g32(ls(0x2C50)) == 3, "pick %u", g32(ls(0x2C50)));

    s_case = "pending becomes ready";
    ts_fresh(0xFFFFFFFFu, 0);
    static const uint32_t t5[] = { 5 };
    tset_put(0x20, t5, 1);
    tset_put(0x30, t5, 1);
    CHECK(ts_request(C, 3, 0, 0) == 1, "poll");
    CHECK(tset_has(0x10, 5) && tset_empty(0x20) && vm(SPURS)[0x03] == 1, "ready count %u", vm(SPURS)[0x03]);

    s_case = "signal wakes and is dropped";
    ts_fresh(0xFFFFFFFFu, 0);
    static const uint32_t t7[] = { 7 };
    tset_put(0x40, t7, 1);
    tset_put(0x30, t7, 1);
    ts_request(C, 3, 0, 0);
    CHECK(tset_has(0x10, 7) && tset_empty(0x40) && vm(SPURS)[0x03] == 1, "ready count %u", vm(SPURS)[0x03]);

    static const uint32_t t4[] = { 4 };
    s_case = "wait signal blocks";
    ts_fresh(4, 2);
    tset_put(0x00, t4, 1);
    tset_put(0x10, t4, 1);
    tset_put(0x30, t4, 1);
    CHECK(ts_request(C, 2, 0, 0) == 0, "rc");
    CHECK(tset_empty(0x00) && tset_empty(0x10) && tset_has(0x50, 4) && vm(SPURS)[0x03] == 1, "ready count %u", vm(SPURS)[0x03]);

    s_case = "wait signal consumes";
    ts_fresh(4, 2);
    tset_put(0x00, t4, 1);
    tset_put(0x10, t4, 1);
    tset_put(0x30, t4, 1);
    tset_put(0x40, t4, 1);
    CHECK(ts_request(C, 2, 0, 0) == 1, "rc");
    CHECK(tset_has(0x00, 4) && tset_has(0x10, 4) && tset_empty(0x40) && tset_empty(0x50) && vm(SPURS)[0x03] == 2, "ready count %u", vm(SPURS)[0x03]);

    s_case = "yield";
    ts_fresh(4, 2);
    tset_put(0x00, t4, 1);
    tset_put(0x10, t4, 1);
    tset_put(0x30, t4, 1);
    CHECK(ts_request(C, 1, 0, 0) == 0, "rc");
    CHECK(tset_empty(0x00) && tset_has(0x10, 4) && tset_has(0x50, 4) && vm(SPURS)[0x03] == 2, "ready count %u", vm(SPURS)[0x03]);

    s_case = "destroy";
    ts_fresh(4, 3);
    tset_put(0x00, t4, 1);
    tset_put(0x10, t4, 1);
    tset_put(0x30, t4, 1);
    tset_put(0x40, t4, 1);
    CHECK(ts_request(C, 0, 0, 0) == 0, "rc");
    CHECK(tset_empty(0x00) && tset_empty(0x10) && tset_empty(0x30) && tset_empty(0x40) && vm(SPURS)[0x03] == 2, "ready count %u", vm(SPURS)[0x03]);

    s_case = "poll signal";
    ts_fresh(4, 1);
    tset_put(0x10, t4, 1);
    tset_put(0x30, t4, 1);
    tset_put(0x40, t4, 1);
    CHECK(ts_request(C, -1, 0, 0) == 1 && tset_empty(0x40), "rc");
    CHECK(ts_request(C, -1, 0, 0) == 0, "second poll");

    s_case = "receive workload flag";
    ts_fresh(4, 1);
    vm(TASKSET)[0x72] = 0x80;
    CHECK(ts_request(C, 6, 0, 0) == 0 && vm(TASKSET)[0x72] == 0x81 && vm(SPURS)[0x03] == 1, "flag %X", vm(TASKSET)[0x72]);
    ts_fresh(4, 1);
    vm(TASKSET)[0x72] = 0x10;
    CHECK(ts_request(C, 6, 0, 0) == 1 && vm(TASKSET)[0x72] == 0x80 && vm(SPURS)[0x03] == 2, "flag %X count %u", vm(TASKSET)[0x72], vm(SPURS)[0x03]);

    s_case = "wait workload flag";
    ts_fresh(4, 1);
    vm(TASKSET)[0x72] = 0x81;
    CHECK(ts_request(C, 4, 0, 0) == 0 && vm(TASKSET)[0x72] == 0x80, "flag %X", vm(TASKSET)[0x72]);
    ts_fresh(4, 2);
    tset_put(0x00, t4, 1);
    tset_put(0x10, t4, 1);
    tset_put(0x30, t4, 1);
    vm(TASKSET)[0x72] = 0x80;
    CHECK(ts_request(C, 4, 0, 0) == 1 && vm(TASKSET)[0x72] == 4 && tset_empty(0x00) && tset_has(0x50, 4) && vm(SPURS)[0x03] == 1, "flag %X", vm(TASKSET)[0x72]);
    ts_fresh(4, 1);
    vm(TASKSET)[0x72] = 5;
    CHECK(ts_request(C, 4, 0, 0) == 0x8041090Au, "busy");

    s_case = "ready count saturates";
    ts_fresh(0xFFFFFFFFu, 255);
    tset_put(0x20, t5, 1);
    tset_put(0x30, t5, 1);
    ts_request(C, 3, 0, 0);
    CHECK(vm(SPURS)[0x03] == 255, "ready count %u", vm(SPURS)[0x03]);

    s_case = "invariant halt";
    ts_fresh(0xFFFFFFFFu, 1);
    static const uint32_t t1[] = { 1 };
    tset_put(0x00, t1, 1);
    const int halted = spu_run_with_halt(ts_halt_entry, C);
    CHECK(halted && C->status == SPU_STATUS_STOPPED_BY_HALT, "halted %d status %X", halted, C->status);
}

static uint32_t s_log[16], s_tag[16], s_desc[16], s_sbuf[16], s_sp[16];
static uint64_t s_eajd[16];
static int s_nlog;

static void test_job(spu_context* c)
{
    const uint32_t d = c->gpr[4]._u32[0];
    if (s_nlog < 16) {
        s_log[s_nlog] = g32(c->ls + d + 0x38);
        s_tag[s_nlog] = g32(c->ls + 0x4964);
        s_eajd[s_nlog] = g64(c->ls + 0x4968);
        s_desc[s_nlog] = d;
        s_sbuf[s_nlog] = g32(c->ls + 0x4960);
        s_sp[s_nlog] = c->gpr[1]._u32[0];
        CHECK(c->gpr[3]._u32[0] == 0x4940 && c->gpr[3]._u32[3] == 0x4940 && c->gpr[0]._u32[0] == 0x2570, "job registers");
        s_nlog++;
    }
    SPU_RET(c);
}

static void put_desc(uint32_t i, uint32_t id)
{
    uint8_t* d = vm(DESC + 0x40 * i);
    p64(d, BINARY);
    p16(d + 8, 512 / 16);
    p32(d + 0x38, id);
}

static uint64_t s_chain_list;

static void chain_fresh(uint64_t pc)
{
    fresh(0, 0);
    vm(SPURS)[0x50] = 1;
    vm(SPURS)[0x20] = 1;
    ls(0x180)[0] = 1;
    p16(ls(0x1EC), 0x8000);
    ls(0x1A0)[0] = 1;
    uint8_t* l = vm(CHAIN);
    p64(l, pc);
    l[0x24] = 1;
    l[0x2A] = 1;
    l[0x2B] = 2;
    p16(l + 0x70, 64);
    p16(l + 0x72, 4);
    uint8_t* b = vm(BINARY);
    memcpy(b, "TESTJOB-BINARY!!", 16);
    p32(b + 0x10, 0x44012850u);
    for (uint32_t i = 0; i < 8; i++) put_desc(i, 100 + i);
    s_nlog = 0;
    s_chain_list = pc;
}

static void chain_entry(spu_context* c) { jc_entry(c, CHAIN); }

static void run_chain(void)
{
    const int halted = spu_run_with_halt(chain_entry, C);
    CHECK(!halted, "job chain policy module halted (status %X)", C->status);
}

static void cmds(uint32_t ea, const uint64_t* v, int n)
{
    for (int i = 0; i < n; i++) p64(vm(ea + 8 * i), v[i]);
}

static int log_is(const uint32_t* want, int n)
{
    if (s_nlog != n) return 0;
    for (int i = 0; i < n; i++)
        if (s_log[i] != want[i]) return 0;
    return 1;
}

#define JOB(i) ((uint64_t)(DESC + 0x40 * (i)))
#define CMD_RESET(t) ((uint64_t)(t) | 1)
#define CMD_NEXT(t) ((uint64_t)(t) | 3)
#define CMD_CALL(t) ((uint64_t)(t) | 4)
#define CMD_JOBLIST(t) ((uint64_t)(t) | 6)
#define CMD_GUARD(t) ((uint64_t)(t) | 0x0F)
#define CMD_SYNC 2ull
#define CMD_LWSYNC 0x12ull
#define CMD_SYNC_LABEL 0x0Aull
#define CMD_FLUSH 5ull
#define CMD_LABEL 0x17ull
#define CMD_RET 0x77ull
#define CMD_END 0x7Full

static void jobchain_tests(void)
{
    spu_begin_image(JOB_IMAGE);
    spu_register_function(JOB_CANON, test_job);
    spu_begin_image(0);
    uint8_t code[512] = { 0 };
    memcpy(code, "TESTJOB-BINARY!!", 16);
    p32(code + 0x10, 0x44012850u);
    spu_overlay_register_reloc_content(code, sizeof code, JOB_CANON, JOB_IMAGE);

    s_case = "jobs then end";
    chain_fresh(LIST_A);
    {
        const uint64_t v[] = { JOB(0), JOB(1), JOB(2), CMD_END };
        cmds(LIST_A, v, 4);
    }
    run_chain();
    {
        const uint32_t want[] = { 100, 101, 102 };
        CHECK(log_is(want, 3), "ran %d jobs", s_nlog);
        const uint32_t descs[] = { 0x3F700, 0x3F600, 0x3F500 };
        for (int i = 0; i < 3 && i < s_nlog; i++) {
            CHECK(s_eajd[i] == JOB(i), "job %d descriptor EA %llX", i, (unsigned long long)s_eajd[i]);
            CHECK(s_tag[i] == (i & 1 ? 2u : 1u), "job %d tag %u", i, s_tag[i]);
            CHECK(s_desc[i] == descs[i], "job %d descriptor LS %X", i, s_desc[i]);
            CHECK(s_sbuf[i] == 0x5000 && s_sp[i] == 0x6FD0, "job %d scratch %X sp %X", i, s_sbuf[i], s_sp[i]);
        }
    }
    CHECK(g64(vm(CHAIN)) == LIST_A + 24, "pc %llX", (unsigned long long)g64(vm(CHAIN)));
    CHECK(vm(SPURS)[0x00] == 0 && vm(CHAIN)[0x2E] == 0 && vm(CHAIN)[0x29] == 0 && vm(CHAIN)[0x25] == 0, "ready count %u cached %u lock %u busy %X",
          vm(SPURS)[0x00], vm(CHAIN)[0x2E], vm(CHAIN)[0x29], vm(CHAIN)[0x25]);
    CHECK(g32(ls(0x6FD0)) == 0x6FF0 && g32(ls(0x6FD4)) == 0x2020 && g32(ls(0x6FDC)) == 0x20, "stack frame %X %X", g32(ls(0x6FD0)), g32(ls(0x6FD4)));

    s_case = "next";
    chain_fresh(LIST_A);
    {
        const uint64_t a[] = { CMD_NEXT(LIST_B) }, b[] = { JOB(0), CMD_END };
        cmds(LIST_A, a, 1);
        cmds(LIST_B, b, 2);
    }
    run_chain();
    {
        const uint32_t want[] = { 100 };
        CHECK(log_is(want, 1) && g64(vm(CHAIN)) == LIST_B + 8, "ran %d pc %llX", s_nlog, (unsigned long long)g64(vm(CHAIN)));
    }

    s_case = "call and return";
    chain_fresh(LIST_A);
    {
        const uint64_t a[] = { CMD_CALL(LIST_B), JOB(1), CMD_END }, b[] = { JOB(0), CMD_RET };
        cmds(LIST_A, a, 3);
        cmds(LIST_B, b, 2);
    }
    run_chain();
    {
        const uint32_t want[] = { 100, 101 };
        CHECK(log_is(want, 2), "ran %d", s_nlog);
        CHECK(g64(vm(CHAIN + 8)) == 0 && g64(vm(CHAIN + 0x10)) == 0 && g64(vm(CHAIN + 0x18)) == 0 && vm(CHAIN)[0x20] == 0, "link stack not empty");
        CHECK(g64(vm(CHAIN)) == LIST_A + 16, "pc %llX", (unsigned long long)g64(vm(CHAIN)));
    }

    s_case = "sync";
    chain_fresh(LIST_A);
    {
        const uint64_t a[] = { JOB(0), CMD_SYNC, JOB(1), CMD_LWSYNC, JOB(2), CMD_END };
        cmds(LIST_A, a, 6);
    }
    run_chain();
    {
        const uint32_t want[] = { 100, 101, 102 };
        CHECK(log_is(want, 3) && g64(vm(CHAIN)) == LIST_A + 40, "ran %d pc %llX", s_nlog, (unsigned long long)g64(vm(CHAIN)));
    }

    s_case = "label";
    chain_fresh(LIST_A);
    {
        const uint64_t a[] = { JOB(0), CMD_LABEL, JOB(1), CMD_SYNC_LABEL, JOB(2), CMD_END };
        cmds(LIST_A, a, 6);
    }
    run_chain();
    {
        const uint32_t want[] = { 100, 101, 102 };
        CHECK(log_is(want, 3) && vm(CHAIN)[0x26] == 0 && vm(CHAIN)[0x27] == 0, "ran %d labels %X %X", s_nlog, vm(CHAIN)[0x26], vm(CHAIN)[0x27]);
    }

    s_case = "guard passes with auto reset";
    chain_fresh(LIST_A);
    p32(vm(GUARD + 0x04), 5);
    p32(vm(GUARD + 0x10), 2);
    p32(vm(GUARD + 0x20), 1);
    {
        const uint64_t a[] = { CMD_GUARD(GUARD), JOB(0), CMD_END };
        cmds(LIST_A, a, 3);
    }
    run_chain();
    {
        const uint32_t want[] = { 100 };
        CHECK(log_is(want, 1), "ran %d", s_nlog);
        CHECK(g32(vm(GUARD)) == 5 && g32(vm(GUARD + 0x30)) == 0 && !(vm(CHAIN)[0x2C] & 8), "ncount0 %u flag %u", g32(vm(GUARD)), g32(vm(GUARD + 0x30)));
    }

    s_case = "guard blocks";
    chain_fresh(LIST_A);
    p32(vm(GUARD), 1);
    {
        const uint64_t a[] = { CMD_GUARD(GUARD), JOB(0), CMD_END };
        cmds(LIST_A, a, 3);
    }
    run_chain();
    CHECK(s_nlog == 0 && g32(vm(GUARD + 0x30)) == 1 && (vm(CHAIN)[0x2C] & 8) && g64(vm(CHAIN)) == LIST_A && vm(SPURS)[0x00] == 0,
          "ran %d flag %u blocked %X", s_nlog, g32(vm(GUARD + 0x30)), vm(CHAIN)[0x2C]);

    s_case = "urgent command";
    chain_fresh(LIST_A);
    p64(vm(CHAIN + 0x30), JOB(5));
    {
        const uint64_t a[] = { JOB(0), CMD_END };
        cmds(LIST_A, a, 2);
    }
    run_chain();
    {
        const uint32_t want[] = { 105, 100 };
        CHECK(log_is(want, 2) && g64(vm(CHAIN + 0x30)) == 0, "ran %d first %u", s_nlog, s_nlog ? s_log[0] : 0);
    }

    s_case = "job list";
    chain_fresh(LIST_A);
    p32(vm(JOBLIST), 3);
    p32(vm(JOBLIST + 4), 64);
    p64(vm(JOBLIST + 8), DESC);
    {
        const uint64_t a[] = { CMD_JOBLIST(JOBLIST), CMD_END };
        cmds(LIST_A, a, 2);
    }
    run_chain();
    {
        const uint32_t want[] = { 100, 101, 102 };
        CHECK(log_is(want, 3), "ran %d", s_nlog);
        CHECK(g32(vm(CHAIN + 0x50)) == 0 && g64(vm(CHAIN + 0x58)) == DESC + 3 * 64, "job list state %u %llX", g32(vm(CHAIN + 0x50)), (unsigned long long)g64(vm(CHAIN + 0x58)));
    }

    s_case = "flush";
    chain_fresh(LIST_A);
    {
        const uint64_t a[] = { JOB(0), CMD_FLUSH, JOB(1), CMD_END };
        cmds(LIST_A, a, 4);
    }
    run_chain();
    {
        const uint32_t want[] = { 100, 101 };
        CHECK(log_is(want, 2) && vm(CHAIN)[0x21] == 0xFE, "ran %d flush mask %X", s_nlog, vm(CHAIN)[0x21]);
    }

    s_case = "reset pc";
    chain_fresh(LIST_A);
    p64(vm(CHAIN + 0x18), 0x1234);
    {
        const uint64_t a[] = { CMD_RESET(LIST_B) }, b[] = { JOB(0), CMD_END };
        cmds(LIST_A, a, 1);
        cmds(LIST_B, b, 2);
    }
    run_chain();
    {
        const uint32_t want[] = { 100 };
        CHECK(log_is(want, 1) && g64(vm(CHAIN + 0x18)) == 0, "ran %d", s_nlog);
    }
}

void spu_thread_publish_ctx(uint32_t tid, void* c) { (void)tid; (void)c; }

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    vm_base = (uint8_t*)calloc(1, VM_SIZE);
    ppu_vm_size = VM_SIZE;
    C = (spu_context*)calloc(1, sizeof(spu_context));
    spurs_register(0x30021480u, 0x30023680u, 0x3002A180u, 0x30026300u);
    if (!strcmp(argv[1], "kernel")) kernel_tests();
    else if (!strcmp(argv[1], "idle")) {
        idle_exit_test();
        idle_wake_test();
    } else if (!strcmp(argv[1], "taskset")) taskset_tests();
    else if (!strcmp(argv[1], "jobchain")) jobchain_tests();
    else return 2;
    printf("%s: %s\n", argv[1], s_fail ? "FAIL" : "PASS");
    return s_fail ? 1 : 0;
}
