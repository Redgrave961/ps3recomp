#include "sys_memory.h"
#include <stdio.h>
#include <string.h>
#include "../../include/ps3emu/log_first.h"

sys_mem_alloc_info       g_sys_mem_allocs[SYS_MEMORY_ALLOC_MAX];
sys_mem_container_info   g_sys_mem_containers[SYS_MEMORY_CONTAINER_MAX];
sys_mmapper_shared_info  g_sys_mmapper_shared[SYS_MMAPPER_SHARED_MAX];

uint32_t g_sys_mem_bump_ptr = 0;

#define SYS_MEMORY_CONTAINER_DEFAULT 0xFFFFFFFFu

#define SYS_MEM_ALLOC_BASE  0x10000000u
#define SYS_MEM_ALLOC_END   0x20000000u
#define SYS_MAPPER_BASE     0x40000000u
#define SYS_MAPPER_END      0xC0000000u
static uint32_t s_mapper_bump_ptr = SYS_MAPPER_BASE;
typedef struct mapper_reservation {
    uint32_t start, end, next;
} mapper_reservation;
static mapper_reservation s_mapper_reservations[SYS_MEMORY_ALLOC_MAX];


static uint32_t s_total_allocated = 0;
static int s_alloc_count = 0;

static int alloc_after(uint64_t addr)
{
    int lo = 0, hi = s_alloc_count;
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if ((uint64_t)g_sys_mem_allocs[mid].addr + g_sys_mem_allocs[mid].size > addr) hi = mid;
        else lo = mid + 1;
    }
    return lo;
}
static uint64_t (*s_image_query)(uint64_t addr, uint64_t end);

void sys_memory_set_image_query(uint64_t (*query)(uint64_t addr, uint64_t end))
{
    s_image_query = query;
}

#ifdef _WIN32
static SRWLOCK s_bump_lock = SRWLOCK_INIT;
static void bump_lock(void)   { AcquireSRWLockExclusive(&s_bump_lock); }
static void bump_unlock(void) { ReleaseSRWLockExclusive(&s_bump_lock); }
#else
#include <pthread.h>
static pthread_mutex_t s_bump_mtx = PTHREAD_MUTEX_INITIALIZER;
static void bump_lock(void)   { pthread_mutex_lock(&s_bump_mtx); }
static void bump_unlock(void) { pthread_mutex_unlock(&s_bump_mtx); }
#endif

static void write_be32(uint32_t addr, uint32_t val)
{
    uint32_t* p = (uint32_t*)vm_to_host(addr);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
    val = ((val >> 24) & 0xFF) | ((val >> 8) & 0xFF00) |
          ((val <<  8) & 0xFF0000) | ((val << 24) & 0xFF000000u);
#endif
    *p = val;
}

static uint32_t user_address(uint32_t size, uint32_t alignment)
{
    uint64_t candidate = SYS_MEM_ALLOC_BASE;
    for (;;) {
        candidate = (candidate + alignment - 1u) & ~(uint64_t)(alignment - 1u);
        if (candidate + size > SYS_MEM_ALLOC_END) return 0;
        uint64_t next = s_image_query ? s_image_query(candidate, candidate + size) : candidate;
        uint64_t injected = VM_HLE_INJECT_BASE;
        if (candidate < injected + VM_HLE_INJECT_SIZE && injected < candidate + size &&
            injected + VM_HLE_INJECT_SIZE > next)
            next = injected + VM_HLE_INJECT_SIZE;
        if (candidate < (uint64_t)VM_PPU_TLS_BASE + VM_PPU_TLS_SIZE &&
            VM_PPU_TLS_BASE < candidate + size && (uint64_t)VM_PPU_TLS_BASE + VM_PPU_TLS_SIZE > next)
            next = (uint64_t)VM_PPU_TLS_BASE + VM_PPU_TLS_SIZE;
        for (int i = alloc_after(candidate); i < s_alloc_count && g_sys_mem_allocs[i].addr < candidate + size; ++i) {
            const uint64_t end = (uint64_t)g_sys_mem_allocs[i].addr + g_sys_mem_allocs[i].size;
            if (end > next) next = end;
        }
        if (next == candidate) return (uint32_t)candidate;
        candidate = next;
    }
}

static uint64_t read_be64(uint32_t addr)
{
    const uint8_t* p = (const uint8_t*)vm_to_host(addr);
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
    return v;
}

static void log_guest_chain(const ppu_context* ctx)
{
    if (!ctx) return;
    uint32_t sp = (uint32_t)ctx->gpr[1];
    fprintf(stderr, "[sys_memory]   guest chain: lr=0x%08X", (uint32_t)ctx->lr);
    for (int depth = 0; depth < 32; depth++) {
        uint32_t next = (uint32_t)read_be64(sp);
        if (next <= sp || next >= 0xF0000000u) break;
        fprintf(stderr, " <- 0x%08X", (uint32_t)read_be64(next + 16));
        sp = next;
    }
    fprintf(stderr, "\n");
}

static int64_t memory_allocate(const ppu_context* ctx, uint64_t requested, uint32_t flags, uint32_t cid, uint32_t addr_out)
{
    uint32_t alignment = (flags & SYS_MEMORY_PAGE_SIZE_1M) ? 0x100000u : 0x10000u;
    uint64_t rounded = (requested + alignment - 1u) & ~(uint64_t)(alignment - 1u);
    if (!requested || requested > UINT32_MAX || !rounded || rounded > UINT32_MAX)
        return (int64_t)(int32_t)CELL_EINVAL;
    uint32_t size = (uint32_t)rounded;
    bump_lock();
    if (size > SYS_MEM_USER_TOTAL - s_total_allocated) {
        bump_unlock();
        fprintf(stderr, "[sys_memory] allocate(size=0x%X) -> ENOMEM (0x%X of 0x%X in use)\n",
                size, s_total_allocated, SYS_MEM_USER_TOTAL);
        log_guest_chain(ctx);
        return (int64_t)(int32_t)CELL_ENOMEM;
    }
    sys_mem_container_info* ct = NULL;
    if (cid) {
        ct = &g_sys_mem_containers[cid - 1];
        if (size > ct->total_size - ct->used_size) {
            bump_unlock(); return (int64_t)(int32_t)CELL_ENOMEM;
        }
    }
    const int full = s_alloc_count == SYS_MEMORY_ALLOC_MAX;
    uint32_t addr = full ? 0 : user_address(size, alignment);
    if (!addr || vm_commit(addr, size) != CELL_OK) {
        bump_unlock();
        fprintf(stderr, "[sys_memory] allocate(size=0x%X) -> ENOMEM (%s)\n", size,
                full ? "allocation table full" : !addr ? "no address range" : "commit failed");
        log_guest_chain(ctx);
        return (int64_t)(int32_t)CELL_ENOMEM;
    }
    memset(vm_to_host(addr), 0, size);
    vm_track_write(addr, size);
    const int slot = alloc_after(addr);
    memmove(&g_sys_mem_allocs[slot + 1], &g_sys_mem_allocs[slot], (size_t)(s_alloc_count - slot) * sizeof g_sys_mem_allocs[0]);
    s_alloc_count++;
    sys_mem_alloc_info* a = &g_sys_mem_allocs[slot];
    a->addr = addr; a->size = size;
    a->container_id = (int32_t)cid; a->page_size = alignment;
    s_total_allocated += size;
    if (ct) ct->used_size += size;
    if (addr + size > g_sys_mem_bump_ptr) g_sys_mem_bump_ptr = addr + size;
    bump_unlock();
    if (addr_out) write_be32(addr_out, addr);
    return CELL_OK;
}

int64_t sys_memory_allocate(ppu_context* ctx)
{
    return memory_allocate(ctx, LV2_ARG_U64(ctx, 0), LV2_ARG_U32(ctx, 1), 0, LV2_ARG_PTR(ctx, 2));
}

int64_t sys_memory_allocate_from_container(ppu_context* ctx)
{
    uint32_t cid = LV2_ARG_U32(ctx, 1);
    if (cid == SYS_MEMORY_CONTAINER_DEFAULT)
        return memory_allocate(ctx, LV2_ARG_U64(ctx, 0), LV2_ARG_U32(ctx, 2), 0, LV2_ARG_PTR(ctx, 3));
    if (cid == 0 || cid > SYS_MEMORY_CONTAINER_MAX || !g_sys_mem_containers[cid - 1].active)
        return (int64_t)(int32_t)CELL_ESRCH;
    return memory_allocate(ctx, LV2_ARG_U64(ctx, 0), LV2_ARG_U32(ctx, 2), cid, LV2_ARG_PTR(ctx, 3));
}

int64_t sys_memory_free(ppu_context* ctx)
{
    uint32_t addr = LV2_ARG_U32(ctx, 0);
    bump_lock();
    const int i = alloc_after(addr);
    if (i < s_alloc_count && g_sys_mem_allocs[i].addr == addr) {
        s_total_allocated -= g_sys_mem_allocs[i].size;
        const int32_t cid = g_sys_mem_allocs[i].container_id;
        if (cid > 0 && cid <= SYS_MEMORY_CONTAINER_MAX && g_sys_mem_containers[cid - 1].active)
            g_sys_mem_containers[cid - 1].used_size -= g_sys_mem_allocs[i].size;
        s_alloc_count--;
        memmove(&g_sys_mem_allocs[i], &g_sys_mem_allocs[i + 1], (size_t)(s_alloc_count - i) * sizeof g_sys_mem_allocs[0]);
        bump_unlock(); return CELL_OK;
    }
    bump_unlock();
    return (int64_t)(int32_t)CELL_EINVAL;
}

int64_t sys_memory_get_user_memory_size(ppu_context* ctx)
{
    uint32_t out_addr = LV2_ARG_PTR(ctx, 0);

    fprintf(stderr, "[sys_memory] get_user_memory_size()\n");
    if (out_addr != 0) {
        uint32_t total = SYS_MEM_USER_TOTAL;
        uint32_t avail = total - s_total_allocated;
        write_be32(out_addr + 0, total);
        write_be32(out_addr + 4, avail);
    }

    return CELL_OK;
}

int64_t sys_memory_get_page_attribute(ppu_context* ctx)
{
    uint32_t addr     = LV2_ARG_U32(ctx, 0);
    uint32_t attr_out = LV2_ARG_PTR(ctx, 1);

    uint32_t page_size = 0x10000;
    bump_lock();
    const int i = alloc_after(addr);
    if (i < s_alloc_count && addr >= g_sys_mem_allocs[i].addr && g_sys_mem_allocs[i].page_size)
        page_size = g_sys_mem_allocs[i].page_size;
    bump_unlock();

    if (attr_out != 0) {
        write_be32(attr_out + 0x00, 0);
        write_be32(attr_out + 0x04, 0x00040000);
        write_be32(attr_out + 0x08, 0);
        write_be32(attr_out + 0x0C, 0x00000008);
        write_be32(attr_out + 0x10, page_size);
        write_be32(attr_out + 0x14, 0);
    }
    return CELL_OK;
}

int64_t sys_memory_container_create(ppu_context* ctx)
{
    uint32_t id_out_addr = LV2_ARG_PTR(ctx, 0);
    uint32_t size        = LV2_ARG_U32(ctx, 1);

    int slot = -1;
    for (int i = 0; i < SYS_MEMORY_CONTAINER_MAX; i++) {
        if (!g_sys_mem_containers[i].active) { slot = i; break; }
    }
    if (slot < 0)
        return (int64_t)(int32_t)CELL_EAGAIN;

    sys_mem_container_info* c = &g_sys_mem_containers[slot];
    c->active     = 1;
    c->total_size = size;
    c->used_size  = 0;

    uint32_t container_id = (uint32_t)(slot + 1);
    if (id_out_addr != 0) {
        write_be32(id_out_addr, container_id);
    }

    return CELL_OK;
}

int64_t sys_memory_container_destroy(ppu_context* ctx)
{
    uint32_t container_id = LV2_ARG_U32(ctx, 0);

    if (container_id == 0 || container_id > SYS_MEMORY_CONTAINER_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_mem_container_info* c = &g_sys_mem_containers[container_id - 1];
    if (!c->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    c->active = 0;
    return CELL_OK;
}

int64_t sys_memory_container_get_size(ppu_context* ctx)
{
    uint32_t out_addr     = LV2_ARG_PTR(ctx, 0);
    uint32_t container_id = LV2_ARG_U32(ctx, 1);

    if (container_id == SYS_MEMORY_CONTAINER_DEFAULT) {
        if (out_addr != 0) {
            bump_lock();
            const uint32_t avail = SYS_MEM_USER_TOTAL - s_total_allocated;
            bump_unlock();
            write_be32(out_addr + 0, SYS_MEM_USER_TOTAL);
            write_be32(out_addr + 4, avail);
        }
        return CELL_OK;
    }
    if (container_id == 0 || container_id > SYS_MEMORY_CONTAINER_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_mem_container_info* c = &g_sys_mem_containers[container_id - 1];
    if (!c->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    if (out_addr != 0) {
        write_be32(out_addr + 0, c->total_size);
        write_be32(out_addr + 4, c->total_size - c->used_size);
    }

    return CELL_OK;
}

static uint32_t mapper_address(uint64_t requested, uint64_t alignment)
{
    if (!requested || requested > UINT32_MAX || !alignment ||
        alignment > UINT32_MAX || (alignment & (alignment - 1u))) return 0;
    uint64_t addr = ((uint64_t)s_mapper_bump_ptr + alignment - 1u) & ~(alignment - 1u);
    uint64_t size = (requested + alignment - 1u) & ~(alignment - 1u);
    const uint64_t reserved[][2] = {
        { VM_HLE_HEAP_BASE, (uint64_t)VM_HLE_HEAP_BASE + VM_HLE_HEAP_SIZE },
        { VM_AUDIO_BASE, (uint64_t)VM_AUDIO_BASE + VM_AUDIO_SIZE },
        { VM_SYS_VM_BASE, (uint64_t)VM_SYS_VM_BASE + VM_SYS_VM_SIZE },
        { VM_HLE_INJECT_BASE, (uint64_t)VM_HLE_INJECT_BASE + VM_HLE_INJECT_SIZE }
    };
    for (;;) {
        uint64_t next = s_image_query ? s_image_query(addr, addr + size) : addr;
        for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); ++i)
            if (addr < reserved[i][1] && reserved[i][0] < addr + size &&
                reserved[i][1] > next) next = reserved[i][1];
        if (next == addr) break;
        addr = (next + alignment - 1u) & ~(alignment - 1u);
    }
    if (!size || addr + size > SYS_MAPPER_END || vm_commit((uint32_t)addr, (uint32_t)size) != CELL_OK)
        return 0;
    s_mapper_bump_ptr = (uint32_t)(addr + size);
    return (uint32_t)addr;
}

int64_t sys_mmapper_allocate_address(ppu_context* ctx)
{
    uint64_t size = LV2_ARG_U64(ctx, 0);
    uint64_t alignment = LV2_ARG_U64(ctx, 2);
    uint32_t out = LV2_ARG_PTR(ctx, 3);
    if (!alignment) alignment = 0x10000u;
    if (alignment > UINT32_MAX || (alignment & (alignment - 1u)) || !size || size > UINT32_MAX)
        return (int64_t)(int32_t)CELL_EINVAL;
    bump_lock();
    int slot = -1;
    for (int i = 0; i < SYS_MEMORY_ALLOC_MAX; ++i)
        if (!s_mapper_reservations[i].start) { slot = i; break; }
    uint32_t addr = slot < 0 ? 0 : mapper_address(size, alignment);
    if (addr) {
        mapper_reservation* r = &s_mapper_reservations[slot];
        r->start = r->next = addr; r->end = s_mapper_bump_ptr;
    }
    bump_unlock();
    if (!addr) {
        fprintf(stderr, "[sys_mmapper] allocate_address(size=0x%llX, align=0x%llX) -> ENOMEM\n",
                (unsigned long long)size, (unsigned long long)alignment);
        return (int64_t)(int32_t)CELL_ENOMEM;
    }
    { static int n = 0; if (PS3_LOG_FIRST(n, 256))
        fprintf(stderr, "[sys_mmapper] allocate_address(size=0x%llX, align=0x%llX) -> 0x%08X\n",
                (unsigned long long)size, (unsigned long long)alignment, addr); }
    if (out) write_be32(out, addr);
    return CELL_OK;
}

int64_t sys_mmapper_search_map_address(uint32_t start, uint32_t size, uint32_t* out)
{
    if (!out || !size) return (int64_t)(int32_t)CELL_EINVAL;
    bump_lock();
    mapper_reservation* found = NULL;
    for (int i = 0; i < SYS_MEMORY_ALLOC_MAX; ++i) {
        mapper_reservation* r = &s_mapper_reservations[i];
        if (r->start && ((!start && !found) || (start >= r->start && start < r->end))) {
            found = r; if (start) break;
        }
    }
    if (!found) {
        bump_unlock(); return (int64_t)(int32_t)CELL_EINVAL;
    }
    uint64_t addr = found->next;
    if (start > addr) addr = start;
    addr = (addr + 0xFFFFFu) & ~0xFFFFFull;
    uint64_t end = (addr + size + 0xFFFFFu) & ~0xFFFFFull;
    if (end > found->end || vm_commit((uint32_t)addr, size) != CELL_OK) {
        bump_unlock();
        fprintf(stderr, "[sys_mmapper] search_and_map(start=0x%08X, size=0x%X) -> ENOMEM (%s)\n", start, size,
                end > found->end ? "reservation exhausted" : "commit failed");
        return (int64_t)(int32_t)CELL_ENOMEM;
    }
    { static int n = 0; if (PS3_LOG_FIRST(n, 256))
        fprintf(stderr, "[sys_mmapper] search_and_map(start=0x%08X, size=0x%X) -> 0x%08X\n", start, size, (uint32_t)addr); }
    found->next = (uint32_t)end;
    *out = (uint32_t)addr;
    bump_unlock(); return CELL_OK;
}

int64_t sys_mmapper_free_address(ppu_context* ctx)
{
    (void)ctx;
    return CELL_OK;
}

int64_t sys_mmapper_allocate_shared_memory(ppu_context* ctx)
{
    uint64_t key      = LV2_ARG_U64(ctx, 0);
    uint32_t size     = LV2_ARG_U32(ctx, 1);
    uint32_t id_out   = LV2_ARG_PTR(ctx, 3);

    int slot = -1;
    for (int i = 0; i < SYS_MMAPPER_SHARED_MAX; i++) {
        if (!g_sys_mmapper_shared[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        fprintf(stderr, "[sys_mmapper] allocate_shared_memory(size=0x%X) -> EAGAIN (all %d slots in use)\n",
                size, SYS_MMAPPER_SHARED_MAX);
        return (int64_t)(int32_t)CELL_EAGAIN;
    }
    { static int n = 0; if (PS3_LOG_FIRST(n, 256))
        fprintf(stderr, "[sys_mmapper] allocate_shared_memory(key=0x%llX, size=0x%X) -> id %d\n",
                (unsigned long long)key, size, slot + 1); }

    sys_mmapper_shared_info* s = &g_sys_mmapper_shared[slot];
    s->active = 1;
    s->size   = VM_ALIGN_UP(size, VM_PAGE_SIZE);
    s->key    = key;
    s->addr   = 0;

    uint32_t shm_id = (uint32_t)(slot + 1);
    if (id_out != 0) {
        write_be32(id_out, shm_id);
    }

    return CELL_OK;
}

int64_t sys_mmapper_allocate_memory(ppu_context* ctx)
{
    uint32_t addr_out = LV2_ARG_PTR(ctx, 0);
    uint32_t size     = LV2_ARG_U32(ctx, 1);

    if (size == 0)
        return (int64_t)(int32_t)CELL_EINVAL;

    bump_lock();
    uint32_t addr = mapper_address(size, VM_PAGE_SIZE);
    bump_unlock();
    if (!addr) return (int64_t)(int32_t)CELL_ENOMEM;

    if (addr_out != 0)
        write_be32(addr_out, addr);

    { static int n = 0; if (PS3_LOG_FIRST(n, 4))
        fprintf(stderr, "[sys_memory] mmapper_allocate_memory size=0x%X -> 0x%08X\n",
                size, addr); }

    return CELL_OK;
}

int64_t sys_mmapper_allocate_memory_from_container(ppu_context* ctx)
{
    (void)ctx;
    return CELL_OK;
}

int64_t sys_mmapper_map_shared_memory(ppu_context* ctx)
{
    uint32_t addr   = LV2_ARG_U32(ctx, 0);
    uint32_t shm_id = LV2_ARG_U32(ctx, 1);

    if (shm_id == 0 || shm_id > SYS_MMAPPER_SHARED_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_mmapper_shared_info* s = &g_sys_mmapper_shared[shm_id - 1];
    if (!s->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    int32_t rc = vm_commit(addr, s->size);
    if (rc != CELL_OK) {
        fprintf(stderr, "[sys_mmapper] map_shared_memory(addr=0x%08X, id=%u) -> 0x%08X\n", addr, shm_id, (unsigned)rc);
        return (int64_t)(int32_t)rc;
    }
    { static int n = 0; if (PS3_LOG_FIRST(n, 256))
        fprintf(stderr, "[sys_mmapper] map_shared_memory(addr=0x%08X, id=%u, size=0x%X)\n", addr, shm_id, s->size); }

    s->addr = addr;
    memset(vm_to_host(addr), 0, s->size);
    vm_track_write(addr, s->size);

    return CELL_OK;
}

void sys_memory_init(lv2_syscall_table* tbl)
{
    bump_lock();
    memset(g_sys_mem_allocs,     0, sizeof(g_sys_mem_allocs));
    s_alloc_count = 0;
    memset(g_sys_mem_containers, 0, sizeof(g_sys_mem_containers));
    memset(g_sys_mmapper_shared, 0, sizeof(g_sys_mmapper_shared));
    g_sys_mem_bump_ptr = 0;
    s_total_allocated  = 0;
    s_mapper_bump_ptr = SYS_MAPPER_BASE;
    memset(s_mapper_reservations, 0, sizeof(s_mapper_reservations));
    bump_unlock();

    lv2_syscall_register(tbl, SYS_MEMORY_ALLOCATE,             sys_memory_allocate);
    lv2_syscall_register(tbl, SYS_MEMORY_FREE,                 sys_memory_free);
    lv2_syscall_register(tbl, SYS_MEMORY_GET_USER_MEMORY_SIZE, sys_memory_get_user_memory_size);
    lv2_syscall_register(tbl, SYS_MEMORY_GET_PAGE_ATTRIBUTE,   sys_memory_get_page_attribute);
    lv2_syscall_register(tbl, SYS_MEMORY_CONTAINER_CREATE,     sys_memory_container_create);
    lv2_syscall_register(tbl, SYS_MEMORY_CONTAINER_DESTROY,    sys_memory_container_destroy);
    lv2_syscall_register(tbl, SYS_MEMORY_CONTAINER_GET_SIZE,   sys_memory_container_get_size);
    lv2_syscall_register(tbl, SYS_MEMORY_ALLOCATE_FROM_CONTAINER, sys_memory_allocate_from_container);
    lv2_syscall_register(tbl, SYS_MMAPPER_ALLOCATE_ADDRESS,    sys_mmapper_allocate_address);
    lv2_syscall_register(tbl, SYS_MMAPPER_FREE_ADDRESS,        sys_mmapper_free_address);
    lv2_syscall_register(tbl, SYS_MMAPPER_ALLOCATE_SHARED_MEMORY, sys_mmapper_allocate_shared_memory);
    lv2_syscall_register(tbl, SYS_MMAPPER_MAP_SHARED_MEMORY,  sys_mmapper_map_shared_memory);
    lv2_syscall_register(tbl, 341, sys_mmapper_allocate_memory);
    lv2_syscall_register(tbl, 342, sys_mmapper_allocate_memory_from_container);
}
