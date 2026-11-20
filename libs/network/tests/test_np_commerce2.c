#include "../sceNpCommerce2.c"

#include <assert.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;
uint32_t g_ww_lo = 0, g_ww_hi = 0;
int  spu_coh_is_reserved(uint32_t addr) { (void)addr; return 0; }
void spu_lockline_lock(uint32_t a) { (void)a; }
void spu_lockline_unlock(uint32_t a) { (void)a; }
void spu_coh_notify_write(uint32_t addr) { (void)addr; }
void ps3_ww_report_inline(uint32_t addr, uint64_t val, int width) { (void)addr; (void)val; (void)width; }

typedef int32_t (*create_ctx5_t)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

#define NPID     0x1000u
#define HANDLER  0x2000u
#define CTX_ID   0x3000u

int main(void)
{
    static uint8_t mem[0x10000];
    vm_base = mem;
    ppu_vm_size = sizeof mem;

    create_ctx5_t create = (create_ctx5_t)(void*)sceNpCommerce2CreateCtx;
    sceNpCommerce2Init();

    memset(mem + HANDLER, 0xAA, 16);
    memset(mem + CTX_ID, 0xBB, 4);

    assert(create(2, NPID, HANDLER, 0, CTX_ID) == CELL_OK);
    for (int i = 0; i < 16; i++)
        assert(mem[HANDLER + i] == 0xAA);
    assert(vm_read32(CTX_ID) == 0);

    assert(create(2, NPID, HANDLER, 0, CTX_ID) == CELL_OK);
    assert(vm_read32(CTX_ID) == 1);
    for (int i = 0; i < 16; i++)
        assert(mem[HANDLER + i] == 0xAA);

    assert(create(2, NPID, HANDLER, 0, 0) == (int32_t)SCE_NP_COMMERCE2_ERROR_INVALID_ARGUMENT);
    for (int i = 0; i < 16; i++)
        assert(mem[HANDLER + i] == 0xAA);

    printf("test_np_commerce2: all passed\n");
    return 0;
}
