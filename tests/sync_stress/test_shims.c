#include <stdint.h>

extern uint8_t* vm_base;

void ppu_thread_note_block(uint64_t tid) { (void)tid; }
int32_t ppu_thread_priority(uint64_t tid) { (void)tid; return 0; }
int ppu_thread_exists(uint64_t tid) { return tid != 0; }

uint32_t vm_read32(uint64_t addr)
{
    const uint8_t* p = vm_base + (uint32_t)addr;
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}
