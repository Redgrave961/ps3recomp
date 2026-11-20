#include "../cellNetCtl.c"

#include <assert.h>

uint8_t* vm_base;
uint32_t g_ww_lo, g_ww_hi;
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(uint32_t a) { (void)a; }
void spu_lockline_unlock(uint32_t a) { (void)a; }

static int s_unloaded;
void cellSysutilQueueEvent(int slot, u32 status, u32 param)
{
    (void)slot; (void)param;
    if (status == CELL_SYSUTIL_NET_CTL_NETSTART_UNLOADED) s_unloaded++;
}

static u32 be32_at(u32 ea)
{
    const uint8_t* p = vm_base + ea;
    return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}

int main(void)
{
    static uint8_t mem[0x10000];
    vm_base = mem;
    const u32 ea = 0x1000;

    const int online = getenv("PS3_NET_ONLINE") != NULL;
    mem[ea + 4] = 0xAA;
    assert(cellNetCtlNetStartDialogUnloadAsync((CellNetCtlNetStartDialogResult*)(uintptr_t)ea) == CELL_OK);
    if (online) assert(be32_at(ea + 4) == 0u);
    else        assert(be32_at(ea + 4) == 0x80130190u);
    assert(s_unloaded == CELL_SYSUTIL_MAX_CALLBACKS);
    printf("%s: result 0x%08X\n", online ? "PS3_NET_ONLINE=1" : "offline", be32_at(ea + 4));
    return 0;
}
