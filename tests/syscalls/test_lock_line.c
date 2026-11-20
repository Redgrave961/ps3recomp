#include "lv2_lock_line.h"
#include "sys_vm.h"
#include "ps3emu/error_codes.h"

#include <stdio.h>

sys_vm_map_info g_sys_vm_maps[SYS_VM_MAX];

static int g_fail = 0;

static void expect(const char* what, uint32_t addr, uint64_t flags, int64_t want)
{
    const int64_t got = lv2_spu_lock_line_classify(addr, flags);
    if (got != want) {
        printf("FAIL %s: classify(0x%08X, flags=0x%llX) = %lld, want %lld\n",
               what, addr, (unsigned long long)flags,
               (long long)got, (long long)want);
        g_fail++;
    }
}

int main(void)
{
    const int64_t OK     = 0;
    const int64_t E_PERM  = (int64_t)(int32_t)CELL_EPERM;
    const int64_t E_INVAL = (int64_t)(int32_t)CELL_EINVAL;

    expect("main low",      0x00010000u, 0x2, OK);
    expect("main top",      0x0FFFFFFFu, 0x2, OK);
    expect("segment 1 low", 0x10000000u, 0x2, OK);
    expect("segment 1 mid", 0x11000000u, 0x2, OK);
    expect("segment 1 top", 0x1FFFFFFFu, 0x2, OK);
    expect("user 64K low",  0x20000000u, 0x2, OK);
    expect("user 64K top",  0x2FFFFFFFu, 0x2, OK);
    expect("rsx local",     0xC0000000u, 0x2, OK);
    expect("rawspu mmio",   0xE0000000u, 0x2, OK);

    expect("segment 1 rawspu", 0x11000000u, 0x1, OK);

    expect("priv spu rawspu", 0xF0000000u, 0x1, E_PERM);
    expect("priv spu thr",    0xF0000000u, 0x2, OK);

    expect("ppu stack thr",   0xD0000000u, 0x2, E_PERM);
    expect("ppu stack rawspu", 0xD0000000u, 0x1, E_PERM);

    g_sys_vm_maps[0].active = 1;
    g_sys_vm_maps[0].addr   = 0xA0000000u;
    g_sys_vm_maps[0].vsize  = 0x02000000u;
    g_sys_vm_maps[0].psize  = 0x00010000u;
    expect("sys_vm start",  0xA0000000u, 0x2, E_PERM);
    expect("sys_vm end",    0xA1FFFFFFu, 0x2, E_PERM);
    expect("sys_vm hole",   0xA2000000u, 0x2, E_INVAL);
    g_sys_vm_maps[0].active = 0;
    expect("sys_vm empty",  0xA0000000u, 0x2, E_INVAL);

    expect("unmapped 0x3", 0x30000000u, 0x2, E_INVAL);
    expect("unmapped 0x7", 0x70000000u, 0x2, E_INVAL);
    expect("unmapped 0xB", 0xB0000000u, 0x2, E_INVAL);

    expect("flags zero",  0x11000000u, 0x0, E_INVAL);
    expect("flags both",  0x11000000u, 0x3, OK);
    expect("flags bit 2", 0x11000000u, 0x4, E_INVAL);
    expect("flags high",  0x11000000u, 0x80010009ull, E_INVAL);

    if (g_fail) {
        printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    printf("test_lock_line: all checks passed\n");
    return 0;
}
