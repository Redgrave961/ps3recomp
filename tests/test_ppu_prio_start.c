#include "../runtime/syscalls/sys_ppu_thread.c"
#include <assert.h>
#include <time.h>

uint8_t* vm_base;
uint32_t ppu_vm_size = 0x10000;
int ps3_intr_is_handler(unsigned long long tid) { (void)tid; return 0; }
int ps3_intr_wait(unsigned long long tid) { (void)tid; return 0; }

enum { BODY_WRITE_THEN_BLOCK = 1, BODY_WRITE_THEN_RETURN, BODY_SPIN_UNTIL_RELEASED };

#define PUBLISH_DELAY_MS 50

static int g_published;
static int g_release;

static void sleep_ms(long ms) { struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&ts, NULL); }
static long long now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000; }

static void guest_entry(ppu_context* ctx)
{
    switch (ctx->gpr[3]) {
    case BODY_WRITE_THEN_BLOCK:
        sleep_ms(PUBLISH_DELAY_MS);
        __atomic_store_n(&g_published, 1, __ATOMIC_RELAXED);
        ppu_thread_note_block(ctx->thread_id);
        while (!__atomic_load_n(&g_release, __ATOMIC_ACQUIRE)) sleep_ms(1);
        break;
    case BODY_WRITE_THEN_RETURN:
        sleep_ms(PUBLISH_DELAY_MS);
        __atomic_store_n(&g_published, 1, __ATOMIC_RELAXED);
        break;
    case BODY_SPIN_UNTIL_RELEASED:
        while (!__atomic_load_n(&g_release, __ATOMIC_ACQUIRE)) sleep_ms(1);
        break;
    }
    ctx->gpr[3] = 0;
}

static int g_fails;
#define EXPECT(cond) do { if (!(cond)) { g_fails++; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

static long long create(int32_t priority, uint64_t body)
{
    ppu_context ctx; memset(&ctx, 0, sizeof(ctx));
    ctx.thread_id = 1;
    ctx.gpr[3] = 0x100;
    ctx.gpr[4] = 0x10000;
    ctx.gpr[5] = body;
    ctx.gpr[6] = (uint64_t)(int64_t)priority;
    ctx.gpr[7] = 0x10000;
    ctx.gpr[8] = 0;
    ctx.gpr[9] = 0;
    const long long t0 = now_ms();
    EXPECT(sys_ppu_thread_create(&ctx) == 0);
    return now_ms() - t0;
}

static void set_main_priority(int32_t priority)
{
    ppu_context ctx; memset(&ctx, 0, sizeof(ctx));
    ctx.gpr[3] = 1;
    ctx.gpr[4] = (uint64_t)(int64_t)priority;
    EXPECT(sys_ppu_thread_set_priority(&ctx) == 0);
}

static void finish_case(void)
{
    __atomic_store_n(&g_release, 1, __ATOMIC_RELEASE);
    for (int i = 0; i < 5000; i++) {
        int live = 0;
        table_lock();
        for (int k = 1; k < PPU_THREAD_MAX; k++) if (g_ppu_threads[k].state != PPU_THREAD_STATE_FREE) live++;
        table_unlock();
        if (!live) break;
        sleep_ms(1);
    }
    __atomic_store_n(&g_release, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_published, 0, __ATOMIC_RELAXED);
}

int main(void)
{
    const int on = getenv("PS3_PRIO_START") != NULL;
    vm_base = calloc(1, 0x10000); assert(vm_base);
    g_vm_stack_alloc.next_addr = 0x10000000u;
    g_vm_stack_alloc.region_end = 0xA0000000u;
    g_ppu_thread_entry_trampoline = guest_entry;
    ppu_thread_register_main();

    set_main_priority(1000);

    create(400, BODY_WRITE_THEN_BLOCK);
    EXPECT(__atomic_load_n(&g_published, __ATOMIC_RELAXED) == on);
    finish_case();

    create(1000, BODY_WRITE_THEN_BLOCK);
    EXPECT(__atomic_load_n(&g_published, __ATOMIC_RELAXED) == 0);
    finish_case();
    create(2000, BODY_WRITE_THEN_BLOCK);
    EXPECT(__atomic_load_n(&g_published, __ATOMIC_RELAXED) == 0);
    finish_case();

    {
        const long long ms = create(400, BODY_WRITE_THEN_RETURN);
        EXPECT(__atomic_load_n(&g_published, __ATOMIC_RELAXED) == on);
        EXPECT(ms < PRIO_START_TIMEOUT_MS - 20);
        finish_case();
    }

    {
        const long long ms = create(400, BODY_SPIN_UNTIL_RELEASED);
        if (on) EXPECT(ms >= PRIO_START_TIMEOUT_MS - 5 && ms < PRIO_START_TIMEOUT_MS + 300);
        else    EXPECT(ms < PRIO_START_TIMEOUT_MS / 2);
        finish_case();
    }

    set_main_priority(0);
    create(400, BODY_WRITE_THEN_BLOCK);
    EXPECT(__atomic_load_n(&g_published, __ATOMIC_RELAXED) == 0);
    finish_case();

    set_main_priority(300);
    create(400, BODY_WRITE_THEN_BLOCK);
    EXPECT(__atomic_load_n(&g_published, __ATOMIC_RELAXED) == 0);
    finish_case();
    create(200, BODY_WRITE_THEN_BLOCK);
    EXPECT(__atomic_load_n(&g_published, __ATOMIC_RELAXED) == on);
    finish_case();

    free(vm_base);
    if (g_fails) { fprintf(stderr, "%d check(s) failed (PS3_PRIO_START %s)\n", g_fails, on ? "on" : "off"); return 1; }
    printf("PASS: PS3_PRIO_START %s\n", on ? "on: a higher-priority thread runs until it blocks"
                                          : "off: sys_ppu_thread_create never waits");
    return 0;
}
