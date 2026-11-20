#include "sys_vm.h"
#include <stdio.h>
#include <string.h>

sys_vm_map_info g_sys_vm_maps[SYS_VM_MAX];

static uint32_t s_vm_bump_ptr = SYS_VM_REGION_BASE;

static void write_be32(uint32_t addr, uint32_t val)
{
    uint32_t* p = (uint32_t*)vm_to_host(addr);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
    val = ((val >> 24) & 0xFF) | ((val >> 8) & 0xFF00) |
          ((val <<  8) & 0xFF0000) | ((val << 24) & 0xFF000000u);
#endif
    *p = val;
}

static sys_vm_map_info* find_map(uint32_t addr)
{
    for (int i = 0; i < SYS_VM_MAX; i++) {
        if (g_sys_vm_maps[i].active && g_sys_vm_maps[i].addr == addr)
            return &g_sys_vm_maps[i];
    }
    return NULL;
}

int64_t sys_vm_memory_map(ppu_context* ctx)
{
    uint32_t vsize    = LV2_ARG_U32(ctx, 0);
    uint32_t psize    = LV2_ARG_U32(ctx, 1);
    uint32_t addr_out = LV2_ARG_PTR(ctx, 5);

    if (vsize == 0 || psize == 0 ||
        (vsize % SYS_VM_VSIZE_ALIGN) != 0 ||
        (psize % SYS_VM_PSIZE_ALIGN) != 0 ||
        vsize > (SYS_VM_REGION_END - SYS_VM_REGION_BASE))
        return (int64_t)(int32_t)CELL_EINVAL;

    if (s_vm_bump_ptr + vsize > SYS_VM_REGION_END)
        return (int64_t)(int32_t)CELL_ENOMEM;

    int slot = -1;
    for (int i = 0; i < SYS_VM_MAX; i++) {
        if (!g_sys_vm_maps[i].active) { slot = i; break; }
    }
    if (slot < 0)
        return (int64_t)(int32_t)CELL_ENOMEM;

    uint32_t base = s_vm_bump_ptr;
    if (vm_commit(base, vsize) != CELL_OK)
        return (int64_t)(int32_t)CELL_ENOMEM;
    s_vm_bump_ptr += vsize;

    sys_vm_map_info* m = &g_sys_vm_maps[slot];
    m->active = 1;
    m->addr   = base;
    m->vsize  = vsize;
    m->psize  = psize;

    if (addr_out != 0)
        write_be32(addr_out, base);

    fprintf(stderr, "[sys_vm] memory_map(vsize=0x%X, psize=0x%X) -> 0x%08X\n",
            vsize, psize, base);
    return CELL_OK;
}

int64_t sys_vm_unmap(ppu_context* ctx)
{
    uint32_t addr = LV2_ARG_U32(ctx, 0);

    sys_vm_map_info* m = find_map(addr);
    if (!m)
        return (int64_t)(int32_t)CELL_EINVAL;

    m->active = 0;

    fprintf(stderr, "[sys_vm] unmap(0x%08X)\n", addr);
    return CELL_OK;
}

int64_t sys_vm_append_memory(ppu_context* ctx)
{
    uint32_t addr = LV2_ARG_U32(ctx, 0);
    uint32_t size = LV2_ARG_U32(ctx, 1);

    sys_vm_map_info* m = find_map(addr);
    if (!m || size == 0 || (size % SYS_VM_PSIZE_ALIGN) != 0)
        return (int64_t)(int32_t)CELL_EINVAL;

    m->psize += size;
    return CELL_OK;
}

int64_t sys_vm_return_memory(ppu_context* ctx)
{
    uint32_t addr = LV2_ARG_U32(ctx, 0);
    uint32_t size = LV2_ARG_U32(ctx, 1);

    sys_vm_map_info* m = find_map(addr);
    if (!m || size == 0 || (size % SYS_VM_PSIZE_ALIGN) != 0 || size > m->psize)
        return (int64_t)(int32_t)CELL_EINVAL;

    m->psize -= size;
    return CELL_OK;
}

int64_t sys_vm_advisory_nop(ppu_context* ctx)
{
    (void)ctx;
    return CELL_OK;
}

void sys_vm_init(lv2_syscall_table* tbl)
{
    memset(g_sys_vm_maps, 0, sizeof(g_sys_vm_maps));
    s_vm_bump_ptr = SYS_VM_REGION_BASE;

    lv2_syscall_register(tbl, SYS_VM_MEMORY_MAP,           sys_vm_memory_map);
    lv2_syscall_register(tbl, SYS_VM_MEMORY_MAP_DIFFERENT, sys_vm_memory_map);
    lv2_syscall_register(tbl, SYS_VM_UNMAP,                sys_vm_unmap);
    lv2_syscall_register(tbl, SYS_VM_APPEND_MEMORY,        sys_vm_append_memory);
    lv2_syscall_register(tbl, SYS_VM_RETURN_MEMORY,        sys_vm_return_memory);
    lv2_syscall_register(tbl, SYS_VM_LOCK,                 sys_vm_advisory_nop);
    lv2_syscall_register(tbl, SYS_VM_UNLOCK,               sys_vm_advisory_nop);
    lv2_syscall_register(tbl, SYS_VM_TOUCH,                sys_vm_advisory_nop);
    lv2_syscall_register(tbl, SYS_VM_FLUSH,                sys_vm_advisory_nop);
    lv2_syscall_register(tbl, SYS_VM_INVALIDATE,           sys_vm_advisory_nop);
    lv2_syscall_register(tbl, SYS_VM_STORE,                sys_vm_advisory_nop);
    lv2_syscall_register(tbl, SYS_VM_SYNC,                 sys_vm_advisory_nop);
}
