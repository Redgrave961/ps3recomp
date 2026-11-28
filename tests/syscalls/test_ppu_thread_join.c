#include "../../runtime/syscalls/sys_ppu_thread.c"
#include <assert.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;
uint32_t vm_read32(uint64_t a)
{
    const uint8_t* p = vm_base + (uint32_t)a;
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
int ps3_intr_is_handler(unsigned long long tid) { (void)tid; return 0; }
int ps3_intr_wait(unsigned long long tid) { (void)tid; return 0; }
PPU_THREAD_LOCAL ppu_context* g_active_ctx;

#define OUT_TID    (VM_MAIN_MEM_BASE + 0x100)
#define OUT_STATUS (VM_MAIN_MEM_BASE + 0x200)

enum { ARG_RETURN = 0, ARG_EXIT = 1, ARG_GATED = 2 };
static volatile int s_gate;

static void guest_entry(ppu_context* ctx)
{
    switch ((int)ctx->gpr[3]) {
    case ARG_EXIT:
        ctx->gpr[3] = 9;
        sys_ppu_thread_exit(ctx);
        break;
    case ARG_GATED:
        while (!s_gate) Sleep(1);
        ctx->gpr[3] = 3;
        break;
    default:
        ctx->gpr[3] = 5;
        break;
    }
}

static int g_fails;
#define EXPECT_EQ(got, want) do { long long g_ = (long long)(got), w_ = (long long)(want); if (g_ != w_) { g_fails++; \
    fprintf(stderr, "FAIL line %d: %s = 0x%llX, want 0x%llX\n", __LINE__, #got, g_, w_); } } while (0)

static int64_t create(uint64_t flags, uint64_t arg, uint64_t* tid)
{
    ppu_context ctx; memset(&ctx, 0, sizeof(ctx));
    ctx.gpr[3] = OUT_TID;
    ctx.gpr[4] = 0x10000;
    ctx.gpr[5] = arg;
    ctx.gpr[6] = 1000;
    ctx.gpr[7] = 0x10000;
    ctx.gpr[8] = flags;
    ctx.gpr[9] = 0;
    int64_t rc = sys_ppu_thread_create(&ctx);
    if (rc == 0 && tid) { uint64_t be; memcpy(&be, vm_base + OUT_TID, 8); *tid = __builtin_bswap64(be); }
    return rc;
}
static int64_t join_as(uint64_t caller, uint64_t tid, int64_t* status)
{
    ppu_context ctx; memset(&ctx, 0, sizeof(ctx));
    ctx.thread_id = caller;
    ctx.gpr[3] = tid; ctx.gpr[4] = OUT_STATUS;
    int64_t rc = sys_ppu_thread_join(&ctx);
    if (rc == 0 && status) { uint64_t be; memcpy(&be, vm_base + OUT_STATUS, 8); *status = (int64_t)__builtin_bswap64(be); }
    return rc;
}
static int64_t detach(uint64_t tid)
{
    ppu_context ctx; memset(&ctx, 0, sizeof(ctx));
    ctx.gpr[3] = tid;
    return sys_ppu_thread_detach(&ctx);
}
static int slot_state(uint64_t tid) { table_lock(); int s = g_ppu_threads[tid - 1].state; table_unlock(); return s; }
static int slot_joining(uint64_t tid) { table_lock(); int j = g_ppu_threads[tid - 1].joining; table_unlock(); return j; }
static int used_slots(void)
{
    int n = 0;
    table_lock();
    for (int i = 1; i < PPU_THREAD_MAX; i++) if (g_ppu_threads[i].state != PPU_THREAD_STATE_FREE) n++;
    table_unlock();
    return n;
}
static uint32_t live_stacks(void) { table_lock(); uint32_t n = g_vm_stack_alloc.live; table_unlock(); return n; }
static void wait_state(uint64_t tid, int want)
{
    for (int i = 0; i < 5000 && slot_state(tid) != want; i++) Sleep(1);
}
static void wait_idle(void)
{
    for (int i = 0; i < 5000 && used_slots(); i++) Sleep(1);
}
static void expect_stacks_released(int line)
{
    table_lock();
    const vm_stack_alloc sa = g_vm_stack_alloc;
    table_unlock();
    if (sa.live || sa.free_count || sa.next_addr != VM_STACK_BASE) {
        g_fails++;
        fprintf(stderr, "FAIL line %d: stack allocator live=%u free_count=%u next=0x%08X, want 0/0/0x%08X\n",
                line, sa.live, sa.free_count, sa.next_addr, VM_STACK_BASE);
    }
}

typedef struct { uint64_t caller, tid; int64_t rc, status; } bgjoin_t;
static DWORD WINAPI bgjoin_main(LPVOID p)
{
    bgjoin_t* b = (bgjoin_t*)p;
    b->rc = join_as(b->caller, b->tid, &b->status);
    return 0;
}

int main(void)
{
    assert(vm_init() == CELL_OK);
    vm_stack_alloc_init(&g_vm_stack_alloc);
    g_ppu_thread_entry_trampoline = guest_entry;
    ppu_thread_register_main();

    for (int i = 0; i < 200; i++) {
        uint64_t tid = 0; int64_t status = 0;
        EXPECT_EQ(create(1, (i & 1) ? ARG_EXIT : ARG_RETURN, &tid), 0);
        EXPECT_EQ(join_as(1, tid, &status), 0);
        EXPECT_EQ(status, (i & 1) ? 9 : 5);
        EXPECT_EQ(slot_state(tid), PPU_THREAD_STATE_FREE);
    }
    EXPECT_EQ(used_slots(), 0);
    expect_stacks_released(__LINE__);

    for (int i = 0; i < 100; i++) {
        uint64_t tid = 0;
        EXPECT_EQ(create(1, ARG_EXIT, &tid), 0);
        wait_state(tid, PPU_THREAD_STATE_FINISHED);
        EXPECT_EQ(live_stacks(), 1);
        EXPECT_EQ(detach(tid), 0);
        EXPECT_EQ(slot_state(tid), PPU_THREAD_STATE_FREE);
        EXPECT_EQ(detach(tid), (int64_t)(int32_t)CELL_ESRCH);
    }
    EXPECT_EQ(used_slots(), 0);
    expect_stacks_released(__LINE__);

    {
        uint64_t tid = 0;
        s_gate = 0;
        EXPECT_EQ(create(0, ARG_GATED, &tid), 0);
        EXPECT_EQ(detach(tid), (int64_t)(int32_t)CELL_EINVAL);
        EXPECT_EQ(join_as(1, tid, NULL), (int64_t)(int32_t)CELL_EINVAL);
        s_gate = 1;
        wait_idle();
        EXPECT_EQ(slot_state(tid), PPU_THREAD_STATE_FREE);
        expect_stacks_released(__LINE__);
    }

    {
        uint64_t tid = 0;
        s_gate = 0;
        EXPECT_EQ(create(1, ARG_GATED, &tid), 0);
        EXPECT_EQ(detach(tid), 0);
        EXPECT_EQ(slot_state(tid), PPU_THREAD_STATE_DETACHED);
        EXPECT_EQ(detach(tid), (int64_t)(int32_t)CELL_EINVAL);
        EXPECT_EQ(join_as(1, tid, NULL), (int64_t)(int32_t)CELL_EINVAL);
        EXPECT_EQ(live_stacks(), 1);
        s_gate = 1;
        wait_idle();
        EXPECT_EQ(slot_state(tid), PPU_THREAD_STATE_FREE);
        EXPECT_EQ(join_as(1, tid, NULL), (int64_t)(int32_t)CELL_ESRCH);
        expect_stacks_released(__LINE__);
    }

    {
        uint64_t tid = 0; int64_t status = 0;
        s_gate = 0;
        EXPECT_EQ(create(1, ARG_GATED, &tid), 0);
        EXPECT_EQ(join_as(tid, tid, &status), (int64_t)(int32_t)CELL_EDEADLK);
        bgjoin_t b = { 1, tid, -1, 0 };
        HANDLE h = CreateThread(NULL, 0, bgjoin_main, &b, 0, NULL);
        for (int i = 0; i < 5000 && !slot_joining(tid); i++) Sleep(1);
        EXPECT_EQ(slot_joining(tid), 1);
        EXPECT_EQ(join_as(1, tid, &status), (int64_t)(int32_t)CELL_EINVAL);
        EXPECT_EQ(detach(tid), (int64_t)(int32_t)CELL_EBUSY);
        EXPECT_EQ(slot_state(tid), PPU_THREAD_STATE_RUNNING);
        s_gate = 1;
        WaitForSingleObject(h, 20000); CloseHandle(h);
        EXPECT_EQ(b.rc, 0);
        EXPECT_EQ(b.status, 3);
        EXPECT_EQ(used_slots(), 0);
        EXPECT_EQ(join_as(1, tid, &status), (int64_t)(int32_t)CELL_ESRCH);
        expect_stacks_released(__LINE__);
    }

    vm_shutdown();
    printf("ppu thread join: %s\n", g_fails ? "FAIL" : "PASS");
    return g_fails ? 1 : 0;
}
