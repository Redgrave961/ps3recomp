#include "../runtime/syscalls/sys_ppu_thread.c"
#include <assert.h>
#include <time.h>

uint8_t* vm_base;
uint32_t ppu_vm_size = 0x10000;
int ps3_intr_is_handler(unsigned long long tid) { (void)tid; return 0; }
int ps3_intr_wait(unsigned long long tid) { (void)tid; return 0; }

static void guest_entry(ppu_context* ctx) { ctx->gpr[3] = 0; }

static int g_fails;
#define EXPECT_EQ(got, want) do { long long g_ = (got), w_ = (want); if (g_ != w_) { g_fails++; \
    fprintf(stderr, "FAIL line %d: %s = %lld, want %lld\n", __LINE__, #got, g_, w_); } } while (0)

static int64_t create(uint64_t flags, uint64_t* tid)
{
    ppu_context ctx; memset(&ctx, 0, sizeof(ctx));
    ctx.gpr[3] = 0x100;
    ctx.gpr[4] = 0x10000;
    ctx.gpr[5] = 0;
    ctx.gpr[6] = 1000;
    ctx.gpr[7] = 0x10000;
    ctx.gpr[8] = flags;
    ctx.gpr[9] = 0;
    int64_t rc = sys_ppu_thread_create(&ctx);
    if (rc == 0 && tid) { uint64_t be; memcpy(&be, vm_base + 0x100, 8); *tid = __builtin_bswap64(be); }
    return rc;
}
static int used_slots(void)
{
    int n = 0;
    table_lock();
    for (int i = 1; i < PPU_THREAD_MAX; i++) if (g_ppu_threads[i].state != PPU_THREAD_STATE_FREE) n++;
    table_unlock();
    return n;
}
static void settle(void)
{
    for (int i = 0; i < 5000; i++) {
        int running = 0;
        table_lock();
        for (int k = 1; k < PPU_THREAD_MAX; k++) if (g_ppu_threads[k].state == PPU_THREAD_STATE_RUNNING) running++;
        table_unlock();
        if (!running) return;
        struct timespec ts = { 0, 1000000 }; nanosleep(&ts, NULL);
    }
}

int main(void)
{
    vm_base = calloc(1, 0x10000); assert(vm_base);
    g_vm_stack_alloc.next_addr = 0x10000000u;
    g_vm_stack_alloc.region_end = 0xA0000000u;
    g_ppu_thread_entry_trampoline = guest_entry;
    ppu_thread_register_main();

    {
        int32_t* out = (int32_t*)(vm_base + 0x200);
        ppu_context ctx; memset(&ctx, 0, sizeof(ctx));
        ctx.gpr[3] = 40; ctx.gpr[4] = 0x200;
        EXPECT_EQ(sys_ppu_thread_get_priority(&ctx), 0);
        EXPECT_EQ(__builtin_bswap32((uint32_t)*out), 1000);
        uint64_t a = 0, b = 0;
        EXPECT_EQ(create(0, &a), 0);
        settle();
        for (int i = 0; i < 1000 && used_slots(); i++) { struct timespec ts = { 0, 1000000 }; nanosleep(&ts, NULL); }
        ctx.gpr[3] = a;
        EXPECT_EQ(sys_ppu_thread_get_priority(&ctx), (int64_t)(int32_t)CELL_ESRCH);
        EXPECT_EQ(create(0, &b), 0);
        EXPECT_EQ(b != a, 1);
        settle();
        for (int i = 0; i < 1000 && used_slots(); i++) { struct timespec ts = { 0, 1000000 }; nanosleep(&ts, NULL); }
        uint64_t j = 0;
        EXPECT_EQ(create(1, &j), 0);
        settle();
        ctx.gpr[3] = j;
        EXPECT_EQ(sys_ppu_thread_get_priority(&ctx), 0);
        ctx.gpr[4] = 0x300;
        EXPECT_EQ(sys_ppu_thread_join(&ctx), 0);
        ctx.gpr[4] = 0x200;
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
    for (int i = 0; i < 1000 && used_slots(); i++) { struct timespec ts = { 0, 1000000 }; nanosleep(&ts, NULL); }
    EXPECT_EQ(used_slots(), 0);

    for (int i = 0; i < PPU_THREAD_MAX - 1; i++) EXPECT_EQ(create(0, NULL), 0);
    settle();
    for (int i = 0; i < 1000 && used_slots(); i++) { struct timespec ts = { 0, 1000000 }; nanosleep(&ts, NULL); }
    EXPECT_EQ(used_slots(), 0);

    for (int i = 0; i < PPU_THREAD_MAX - 1; i++) EXPECT_EQ(create(1, NULL), 0);
    settle();
    EXPECT_EQ(used_slots(), PPU_THREAD_MAX - 1);
    EXPECT_EQ(create(1, NULL), (int64_t)(int32_t)CELL_EAGAIN);
    EXPECT_EQ(create(0, NULL), (int64_t)(int32_t)CELL_EAGAIN);

    {
        ppu_context ctx; memset(&ctx, 0, sizeof(ctx));
        ctx.gpr[3] = 5;
        EXPECT_EQ(sys_ppu_thread_join(&ctx), 0);
        EXPECT_EQ(create(1, NULL), 0);
    }

    free(vm_base);
    if (g_fails) { fprintf(stderr, "%d check(s) failed\n", g_fails); return 1; }
    puts("PASS: non-joinable PPU threads free their slot when they end; joinable ones keep it until joined");
    return 0;
}
