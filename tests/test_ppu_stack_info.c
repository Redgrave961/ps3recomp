#include "../runtime/syscalls/sys_ppu_thread.c"
#include <assert.h>
#ifndef _WIN32
#include <signal.h>
#endif

uint8_t* vm_base;
uint32_t ppu_vm_size;
int ps3_intr_is_handler(unsigned long long tid) { (void)tid; return 0; }
int ps3_intr_wait(unsigned long long tid) { (void)tid; return 0; }
PPU_THREAD_LOCAL ppu_context* g_active_ctx;

#define OUT_MAIN  (VM_MAIN_MEM_BASE + 0x100)
#define OUT_CHILD (VM_MAIN_MEM_BASE + 0x200)
#define OUT_TID   (VM_MAIN_MEM_BASE + 0x300)

static int g_fails;
#define EXPECT_EQ(got, want) do { long long g_ = (got), w_ = (want); if (g_ != w_) { g_fails++; \
    fprintf(stderr, "FAIL line %d: %s = 0x%llX, want 0x%llX\n", __LINE__, #got, g_, w_); } } while (0)

static uint32_t be32_at(uint32_t ea) { uint32_t v; memcpy(&v, vm_base + ea, 4); return __builtin_bswap32(v); }

#ifdef _WIN32
static int accessible(uint32_t ea)
{
    MEMORY_BASIC_INFORMATION mi;
    return VirtualQuery(vm_base + ea, &mi, sizeof mi) && mi.State == MEM_COMMIT && mi.Protect == PAGE_READWRITE;
}
#else
static sigjmp_buf s_probe;
static void probe_fault(int sig) { (void)sig; siglongjmp(s_probe, 1); }
static int accessible(uint32_t ea)
{
    struct sigaction sa, old_segv, old_bus;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = probe_fault;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &old_segv);
    sigaction(SIGBUS, &sa, &old_bus);
    volatile int ok = 0;
    if (!sigsetjmp(s_probe, 1)) {
        (void)*(volatile uint8_t*)(vm_base + ea);
        ok = 1;
    }
    sigaction(SIGSEGV, &old_segv, NULL);
    sigaction(SIGBUS, &old_bus, NULL);
    return ok;
}
#endif

static int all_zero(uint32_t ea, uint32_t size)
{
    for (uint32_t i = 0; i < size; i++) if (vm_base[ea + i]) return 0;
    return 1;
}

static void stack_info(ppu_context* ctx, uint32_t out, uint32_t* addr, uint32_t* size)
{
    memset(vm_base + out, 0xEE, 8);
    ctx->gpr[3] = out;
    EXPECT_EQ(sys_ppu_thread_get_stack_information(ctx), 0);
    *addr = be32_at(out);
    *size = be32_at(out + 4);
}

static volatile uint32_t g_child_addr, g_child_size;
static void guest_entry(ppu_context* ctx)
{
    uint32_t a, s;
    stack_info(ctx, OUT_CHILD, &a, &s);
    memset(vm_base + a, 0xA5, s);
    g_child_addr = a; g_child_size = s;
    ctx->gpr[3] = 0;
}

static uint32_t run_child(void)
{
    ppu_context ctx; memset(&ctx, 0, sizeof(ctx));
    ctx.gpr[3] = OUT_TID;
    ctx.gpr[4] = 0x10000;
    ctx.gpr[6] = 1000;
    ctx.gpr[7] = 0x10000;
    ctx.gpr[8] = 1;
    g_child_addr = 0; g_child_size = 0;
    EXPECT_EQ(sys_ppu_thread_create(&ctx), 0);
    uint64_t be; memcpy(&be, vm_base + OUT_TID, 8);
    ppu_context jctx; memset(&jctx, 0, sizeof(jctx));
    jctx.thread_id = 1;
    jctx.gpr[3] = __builtin_bswap64(be);
    EXPECT_EQ(sys_ppu_thread_join(&jctx), 0);
    EXPECT_EQ(g_child_size, 0x10000);
    return g_child_addr;
}

static void allocator_cases(void)
{
    const uint32_t pg = vm_host_page_size();
    vm_stack_alloc sa;
    vm_stack_alloc_init(&sa);

    const uint32_t a = vm_stack_allocate(&sa, 0x10000);
    const uint32_t b = vm_stack_allocate(&sa, 0x10000);
    const uint32_t c = vm_stack_allocate(&sa, 0x10000);
    EXPECT_EQ(a, VM_STACK_BASE + pg);
    EXPECT_EQ(b, a + 0x10000 + pg);
    EXPECT_EQ(c, b + 0x10000 + pg);
    EXPECT_EQ(sa.live, 3);
    EXPECT_EQ(accessible(b - pg), 0);
    EXPECT_EQ(accessible(b), 1);

    memset(vm_base + b, 0x5A, 0x10000);
    vm_stack_free(&sa, b, 0x10000);
    EXPECT_EQ(sa.live, 2);
    EXPECT_EQ(sa.free_count, 1);
    EXPECT_EQ(sa.next_addr, c + 0x10000);
#ifdef _WIN32
    EXPECT_EQ(accessible(b), 0);
#endif

    const uint32_t d = vm_stack_allocate(&sa, 0x8000);
    EXPECT_EQ(d, b);
    EXPECT_EQ(sa.free_count, 1);
    EXPECT_EQ(all_zero(d, 0x8000), 1);
    EXPECT_EQ(accessible(d - pg), 0);
    const uint32_t e = vm_stack_allocate(&sa, 0x8000 - pg);
    EXPECT_EQ(e, d + 0x8000 + pg);
    EXPECT_EQ(sa.free_count, 0);
    EXPECT_EQ(sa.next_addr, c + 0x10000);
    EXPECT_EQ(all_zero(e, 0x8000 - pg), 1);

    vm_stack_free(&sa, d, 0x8000);
    vm_stack_free(&sa, a, 0x10000);
    EXPECT_EQ(sa.free_count, 1);
    EXPECT_EQ(sa.free_ranges[0].addr, VM_STACK_BASE);
    EXPECT_EQ(sa.free_ranges[0].size, 0x10000 + pg + 0x8000 + pg);
    vm_stack_free(&sa, c, 0x10000);
    EXPECT_EQ(sa.free_count, 1);
    EXPECT_EQ(sa.next_addr, e + 0x8000 - pg);
    vm_stack_free(&sa, e, 0x8000 - pg);
    EXPECT_EQ(sa.live, 0);
    EXPECT_EQ(sa.free_count, 0);
    EXPECT_EQ(sa.next_addr, VM_STACK_BASE);

    uint32_t blocks[VM_STACK_BLOCKS_MAX];
    for (uint32_t i = 0; i < VM_STACK_BLOCKS_MAX; i++) blocks[i] = vm_stack_allocate(&sa, pg);
    EXPECT_EQ(blocks[VM_STACK_BLOCKS_MAX - 1], VM_STACK_BASE + (2u * VM_STACK_BLOCKS_MAX - 1u) * pg);
    EXPECT_EQ(vm_stack_allocate(&sa, pg), 0);
    for (uint32_t i = 0; i < VM_STACK_BLOCKS_MAX; i += 2) vm_stack_free(&sa, blocks[i], pg);
    EXPECT_EQ(sa.free_count, VM_STACK_BLOCKS_MAX / 2);
    for (uint32_t i = 1; i < VM_STACK_BLOCKS_MAX; i += 2) vm_stack_free(&sa, blocks[i], pg);
    EXPECT_EQ(sa.live, 0);
    EXPECT_EQ(sa.free_count, 0);
    EXPECT_EQ(sa.next_addr, VM_STACK_BASE);

    const uint32_t whole = VM_CB_STACK_BASE - VM_STACK_BASE - pg;
    const uint32_t w = vm_stack_allocate(&sa, whole);
    EXPECT_EQ(w, VM_STACK_BASE + pg);
    EXPECT_EQ(sa.next_addr, VM_CB_STACK_BASE);
    EXPECT_EQ(vm_stack_allocate(&sa, pg), 0);
    EXPECT_EQ(sa.live, 1);
    vm_stack_free(&sa, w, whole);
    EXPECT_EQ(vm_stack_allocate(&sa, whole + 1), 0);
    EXPECT_EQ(sa.live, 0);
    EXPECT_EQ(sa.next_addr, VM_STACK_BASE);
}

int main(void)
{
    assert(vm_init() == CELL_OK);
    vm_stack_alloc_init(&g_vm_stack_alloc);
    g_ppu_thread_entry_trampoline = guest_entry;

    ppu_context main_ctx; memset(&main_ctx, 0, sizeof(main_ctx));
    main_ctx.thread_id = ppu_thread_register_main();
    EXPECT_EQ(main_ctx.thread_id, 1);
    uint32_t a, s;

    stack_info(&main_ctx, OUT_MAIN, &a, &s);
    EXPECT_EQ(a, VM_STACK_BASE);
    EXPECT_EQ(s, VM_PPU_STACK_SIZE);

    g_ppu_main_stack_addr = 0x0FE00000u;
    g_ppu_main_stack_size = 0x00100000u;
    stack_info(&main_ctx, OUT_MAIN, &a, &s);
    EXPECT_EQ(a, 0x0FE00000u);
    EXPECT_EQ(s, 0x00100000u);

    ppu_context stray; memset(&stray, 0, sizeof(stray));
    stray.thread_id = 0;
    stack_info(&stray, OUT_MAIN, &a, &s);
    EXPECT_EQ(a, 0x0FE00000u);
    EXPECT_EQ(s, 0x00100000u);

    {
        const uint32_t first = run_child();
        EXPECT_EQ(first, VM_STACK_BASE + vm_host_page_size());
        EXPECT_EQ(g_vm_stack_alloc.live, 0);
        EXPECT_EQ(g_vm_stack_alloc.next_addr, VM_STACK_BASE);
#ifdef _WIN32
        EXPECT_EQ(accessible(first), 0);
#endif
        const uint32_t again = run_child();
        EXPECT_EQ(again, first);
        EXPECT_EQ(g_vm_stack_alloc.live, 0);
    }

    allocator_cases();

    vm_shutdown();
    if (g_fails) { fprintf(stderr, "%d check(s) failed\n", g_fails); return 1; }
    puts("PASS: threads are told their own guarded stack, freed stacks are decommitted, coalesced and reused");
    return 0;
}
