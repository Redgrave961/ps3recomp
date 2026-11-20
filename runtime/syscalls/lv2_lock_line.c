#include "lv2_lock_line.h"

#include "sys_vm.h"

#include "../../include/ps3emu/error_codes.h"

int64_t lv2_spu_lock_line_classify(uint32_t addr, uint64_t flags)
{
    if (!flags || (flags & ~0x3ull)) {
        return (int64_t)(int32_t)CELL_EINVAL;
    }

    switch (addr >> 28) {
    case 0x0:
    case 0x1:
    case 0x2:
    case 0xC:
    case 0xE:
        return 0;

    case 0xF:
        if (flags & 0x1ull) {
            return (int64_t)(int32_t)CELL_EPERM;
        }
        return 0;

    case 0xD:
        return (int64_t)(int32_t)CELL_EPERM;

    default:
        if (addr >= SYS_VM_REGION_BASE && addr < SYS_VM_REGION_END) {
            int mapped = 0;
            for (int i = 0; i < SYS_VM_MAX; i++) {
                const sys_vm_map_info* m = &g_sys_vm_maps[i];
                if (!m->active) {
                    continue;
                }
                mapped = 1;
                if (addr >= m->addr && addr - m->addr < m->vsize) {
                    return (int64_t)(int32_t)CELL_EPERM;
                }
            }
            if (mapped) {
                return (int64_t)(int32_t)CELL_EINVAL;
            }
        }
        return (int64_t)(int32_t)CELL_EINVAL;
    }
}
