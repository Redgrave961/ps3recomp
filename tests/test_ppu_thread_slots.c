#include "../runtime/syscalls/sys_ppu_thread.c"
#include <assert.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;
int ps3_intr_is_handler(unsigned long long tid) { (void)tid; return 0; }
int ps3_intr_wait(unsigned long long tid) { (void)tid; return 0; }
PPU_THREAD_LOCAL ppu_context* g_active_ctx;

#define OUT_TID    (VM_MAIN_MEM_BASE + 0x100)
#define OUT_PRIO   (VM_MAIN_MEM_BASE + 0x200)
#define OUT_STATUS (VM_MAIN_MEM_BASE + 0x300)
#define BIG_STACK  0x01000000u

static void guest_entry(ppu_context* ctx) { ctx->gpr[3] = 0; }

static int g_fails;
#define EXPECT_EQ(got, want) do { long long g_ = (got), w_ = (want); if (g_ != w_) { g_fails++; \
    fprintf(stderr, "FAIL line %d: %s = %lld, want %lld\n", __LINE__, #got, g_, w_); } } while (0)

static int64_t create_sized(uint64_t flags, uint32_t stack_size, uint64_t* tid)
{
    ppu_context ctx; memset(&ctx, 0, sizeof(ctx));
    ctx.gpr[3] = OUT_TID;
    ctx.gpr[4] = 0x10000;
    ctx.gpr[5] = 0;
    ctx.gpr[6] = 1000;
    ctx.gpr[7] = stack_size;
    ctx.gpr[8] = flags;
    ctx.gpr[9] = 0;
    int64_t rc = sys_ppu_thread_create(&ctx);
    if (rc == 0 && tid) { uint64_t be; memcpy(&be, vm_base + OUT_TID, 8); *tid = __builtin_bswap64(be); }
    return rc;
}
static int64_t create(uint64_t flags, uint64_t* tid) { return create_sized(flags, 0x10000, tid); }
static int64_t join(uint64_t tid)
{
    ppu_context ctx; memset(&ctx, 0, sizeof(ctx));
    ctx.thread_id = 1;
    ctx.gpr[3] = tid; ctx.gpr[4] = OUT_STATUS;
    return sys_ppu_thread_join(&ctx);
}
static int used_slots(void)
{
    int n = 0;
    table_lock();
    for (int i = 1; i < PPU_THREAD_MAX; i++) if (g_ppu_threads[i].state != PPU_THREAD_STATE_FREE) n++;
    table_unlock();
    return n;
}
static uint32_t live_stacks(void) { table_lock(); uint32_t n = g_vm_stack_alloc.live; table_unlock(); return n; }
static uint32_t stack_of(uint64_t tid) { table_lock(); uint32_t a = g_ppu_threads[tid - 1].stack_addr; table_unlock(); return a; }
static void settle(void)
{
    for (int i = 0; i < 5000; i++) {
        int running = 0;
        table_lock();
        for (int k = 1; k < PPU_THREAD_MAX; k++) if (g_ppu_threads[k].state == PPU_THREAD_STATE_RUNNING) running++;
        table_unlock();
        if (!running) return;
        Sleep(1);
    }
}
static void wait_idle(void)
{
    for (int i = 0; i < 5000 && used_slots(); i++) Sleep(1);
}

int main(void)
{
    assert(vm_init() == CELL_OK);
    vm_stack_alloc_init(&g_vm_stack_alloc);
    g_ppu_thread_entry_trampoline = guest_entry;
    ppu_thread_register_main();

    {
        int32_t* out = (int32_t*)(vm_base + OUT_PRIO);
        ppu_context ctx; memset(&ctx, 0, sizeof(ctx));
        ctx.gpr[3] = 40; ctx.gpr[4] = OUT_PRIO;
        EXPECT_EQ(sys_ppu_thread_get_priority(&ctx), 0);
        EXPECT_EQ(__builtin_bswap32((uint32_t)*out), 1000);
        uint64_t a = 0, b = 0;
        EXPECT_EQ(create(0, &a), 0);
        wait_idle();
        ctx.gpr[3] = a;
        EXPECT_EQ(sys_ppu_thread_get_priority(&ctx), (int64_t)(int32_t)CELL_ESRCH);
        EXPECT_EQ(create(0, &b), 0);
        EXPECT_EQ(b != a, 1);
        wait_idle();
        uint64_t j = 0;
        EXPECT_EQ(create(1, &j), 0);
        settle();
        ctx.gpr[3] = j;
        EXPECT_EQ(sys_ppu_thread_get_priority(&ctx), 0);
        EXPECT_EQ(live_stacks(), 1);
        EXPECT_EQ(join(j), 0);
        EXPECT_EQ(live_stacks(), 0);
        EXPECT_EQ(sys_ppu_thread_get_priority(&ctx), (int64_t)(int32_t)CELL_ESRCH);
    }

    int ok = 0;
    for (int i = 0; i < 3 * PPU_THREAD_MAX; i++) {
        uint64_t tid = 0;
        int64_t rc = create(0, &tid);
        if (rc != 0) { EXPECT_EQ(rc, 0); fprintf(stderr, "  create #%d (tid so far %d) failed\n", i + 1, ok); break; }
        ok++;
        EXPECT_EQ(tid >= 2 && tid <= PPU_THREAD_MAX, 1);
        settle();
    }
    EXPECT_EQ(ok, 3 * PPU_THREAD_MAX);
    wait_idle();
    EXPECT_EQ(used_slots(), 0);
    EXPECT_EQ(live_stacks(), 0);

    for (int i = 0; i < PPU_THREAD_MAX - 1; i++) EXPECT_EQ(create(0, NULL), 0);
    settle();
    wait_idle();
    EXPECT_EQ(used_slots(), 0);
    EXPECT_EQ(live_stacks(), 0);
    EXPECT_EQ(g_vm_stack_alloc.free_count, 0);
    EXPECT_EQ(g_vm_stack_alloc.next_addr, VM_STACK_BASE);

    for (int i = 0; i < PPU_THREAD_MAX - 1; i++) EXPECT_EQ(create(1, NULL), 0);
    settle();
    EXPECT_EQ(used_slots(), PPU_THREAD_MAX - 1);
    EXPECT_EQ(live_stacks(), PPU_THREAD_MAX - 1);
    EXPECT_EQ(create(1, NULL), (int64_t)(int32_t)CELL_EAGAIN);
    EXPECT_EQ(create(0, NULL), (int64_t)(int32_t)CELL_EAGAIN);
    EXPECT_EQ(live_stacks(), PPU_THREAD_MAX - 1);

    {
        const uint32_t freed = VM_STACK_BASE + vm_host_page_size();
        uint64_t lowest = 0;
        for (uint64_t t = 2; t <= PPU_THREAD_MAX; t++) if (stack_of(t) == freed) lowest = t;
        EXPECT_EQ(lowest != 0, 1);
        EXPECT_EQ(join(lowest), 0);
        EXPECT_EQ(live_stacks(), PPU_THREAD_MAX - 2);
        EXPECT_EQ(g_vm_stack_alloc.free_count, 1);
        uint64_t tid = 0;
        EXPECT_EQ(create(1, &tid), 0);
        EXPECT_EQ(stack_of(tid), freed);
        EXPECT_EQ(g_vm_stack_alloc.free_count, 0);
    }

    settle();
    for (uint64_t tid = 2; tid <= PPU_THREAD_MAX; tid++) EXPECT_EQ(join(tid), 0);
    EXPECT_EQ(used_slots(), 0);
    EXPECT_EQ(live_stacks(), 0);
    EXPECT_EQ(g_vm_stack_alloc.free_count, 0);
    EXPECT_EQ(g_vm_stack_alloc.next_addr, VM_STACK_BASE);

    {
        const uint32_t fit = (uint32_t)((VM_CB_STACK_BASE - VM_STACK_BASE) / vm_stack_block_bytes(BIG_STACK));
        uint64_t first = 0, tid = 0;
        EXPECT_EQ(create_sized(1, BIG_STACK, &first), 0);
        for (uint32_t i = 1; i < fit; i++) EXPECT_EQ(create_sized(1, BIG_STACK, NULL), 0);
        EXPECT_EQ(live_stacks(), fit);
        const int slots = used_slots();
        EXPECT_EQ(create_sized(1, BIG_STACK, NULL), (int64_t)(int32_t)CELL_ENOMEM);
        EXPECT_EQ(used_slots(), slots);
        EXPECT_EQ(create(1, &tid), 0);
        EXPECT_EQ(join(tid), 0);
        const uint32_t freed = stack_of(first);
        settle();
        EXPECT_EQ(join(first), 0);
        EXPECT_EQ(create_sized(1, BIG_STACK, &tid), 0);
        EXPECT_EQ(stack_of(tid), freed);
        settle();
        for (uint64_t t = 2; t <= PPU_THREAD_MAX; t++)
            if (g_ppu_threads[t - 1].state != PPU_THREAD_STATE_FREE) EXPECT_EQ(join(t), 0);
        EXPECT_EQ(live_stacks(), 0);
        EXPECT_EQ(g_vm_stack_alloc.next_addr, VM_STACK_BASE);
    }

    vm_shutdown();
    if (g_fails) { fprintf(stderr, "%d check(s) failed\n", g_fails); return 1; }
    puts("PASS: a PPU thread's slot and stack come back when it ends (non-joinable) or is joined (joinable)");
    return 0;
}
