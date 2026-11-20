#include <stdint.h>
int64_t lv2_spu_lock_line_classify(uint32_t a, uint64_t f) { (void)a; (void)f; return 0; }
#include "../../syscalls/lv2_register.c"
#include <assert.h>
#include <stdio.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;
#ifndef _WIN32
void SetLastError(DWORD e) { (void)e; }
#endif
uint32_t spurs_port_queue(uint32_t port) { (void)port; return 0; }
int64_t sys_event_flag_set(ppu_context* c) { (void)c; return 0; }
size_t spu_elf_image_size(const uint8_t* image, size_t max_avail) { (void)image; (void)max_avail; return 0; }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static uint32_t rd32(uint32_t a) { const uint8_t* p = vm_base + a; return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static uint32_t sc(int64_t (*h)(ppu_context*), uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    ppu_context ctx; memset(&ctx, 0, sizeof ctx);
    ctx.gpr[3] = a; ctx.gpr[4] = b; ctx.gpr[5] = c; ctx.gpr[6] = d;
    ctx.gpr[3] = (uint64_t)h(&ctx);
    return (uint32_t)ctx.gpr[3];
}

static spu_thread_t* make_thread(spu_group_t** gout)
{
    spu_group_t* g = spu_alloc_group(); assert(g);
    spu_thread_t* t = spu_alloc_thread(); assert(t);
    t->group_id = g->id;
    g->num_threads = 1;
    g->thread_indices[0] = (uint32_t)(t - s_spu_threads);
    t->sctx = (struct spu_context*)calloc(1, sizeof(spu_context)); assert(t->sctx);
    t->local_store = (uint8_t*)calloc(1, 64); assert(t->local_store);
    if (gout) *gout = g;
    return t;
}

int main(void)
{
    vm_base = calloc(1, 1u << 20); assert(vm_base);
    ppu_vm_size = 1u << 20;

    {
        spu_group_t* g = spu_alloc_group(); assert(g);
        int ok = 1;
        for (int k = 0; k < 8; k++) ok &= g->thread_indices[k] == MAX_SPU_THREADS;
        CHECK(ok, "a fresh group's thread slots are all invalid");
        g->in_use = 0;
    }

    CHECK(sc(sys_spu_thread_group_start_handler, 0xDEAD, 0, 0, 0) == (uint32_t)CELL_ESRCH,
          "group_start of an unknown group is ESRCH");
    CHECK(sc(sys_spu_thread_get_exit_status_handler, 0xDEAD, 0x100, 0, 0) == (uint32_t)CELL_ESRCH,
          "get_exit_status of an unknown thread is ESRCH");
    CHECK(sc(sys_spu_thread_connect_event_handler, 0xDEAD, 1, 1, 0) == (uint32_t)CELL_ESRCH,
          "connect_event on an unknown thread is ESRCH");
    CHECK(sc(sys_spu_thread_write_ls_handler, 0xDEAD, 0, 0, 4) == (uint32_t)CELL_ESRCH,
          "write_ls on an unknown thread is ESRCH");
    CHECK(sc(sys_spu_thread_get_spu_cfg_handler, 0xDEAD, 0x100, 0, 0) == (uint32_t)CELL_ESRCH,
          "get_spu_cfg on an unknown thread is ESRCH");
    CHECK(sc(sys_spu_image_import_handler, 0x2000, 0, 0, 0) == (uint32_t)CELL_EFAULT,
          "image_import with a null source is EFAULT");
    memset(vm_base + 0x3000, 0, 16);
    CHECK(sc(sys_spu_image_import_handler, 0x2000, 0x3000, 0, 0) == (uint32_t)CELL_ENOEXEC,
          "image_import of something that is not an ELF is ENOEXEC");

    spu_group_t* g = NULL;
    spu_thread_t* t = make_thread(&g);
    t->running = 1;
    CHECK(sc(sys_spu_thread_get_exit_status_handler, t->tid, 0x100, 0, 0) == (uint32_t)CELL_ESTAT,
          "get_exit_status of a running thread is ESTAT (got 0x%08X)",
          sc(sys_spu_thread_get_exit_status_handler, t->tid, 0x100, 0, 0));
    CHECK(sc(sys_spu_thread_group_start_handler, g->id, 0, 0, 0) == (uint32_t)CELL_ESTAT,
          "group_start with a running thread is ESTAT");

    t->spu_cfg = 3;
    memset(vm_base + 0x400, 0xEE, 16);
    CHECK(sc(sys_spu_thread_get_spu_cfg_handler, t->tid, 0x400, 0, 0) == 0, "get_spu_cfg succeeds");
    CHECK(rd32(0x400) == 0 && rd32(0x404) == 3, "get_spu_cfg stores 3 through the pointer (0x%08X 0x%08X)",
          rd32(0x400), rd32(0x404));
    CHECK(vm_base[0x408] == 0xEE, "and nothing past the u64");

    uint8_t* ls = t->local_store; struct spu_context* sctx = t->sctx;
    CHECK(sc(sys_spu_thread_group_destroy_handler, g->id, 0, 0, 0) == (uint32_t)CELL_EBUSY,
          "group_destroy with a running thread is EBUSY");
    CHECK(t->in_use && g->in_use && t->local_store == ls && t->sctx == sctx,
          "and the thread, its context and its local store are untouched");

    t->running = 0;
    CHECK(sc(sys_spu_thread_group_destroy_handler, g->id, 0, 0, 0) == 0, "group_destroy of a stopped group succeeds");
    CHECK(!t->in_use && !g->in_use && !t->local_store && !t->sctx, "and releases the thread and the group");

    printf("[spu_group_lifecycle] %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    free(vm_base);
    return fails ? 1 : 0;
}
