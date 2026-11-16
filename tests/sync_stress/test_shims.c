/*
 * Standalone definitions for services that the sync sources call through
 * optional title/runtime integration hooks. The stress suite tests the
 * primitives directly, without linking a generated title.
 */
#include "../../runtime/platform/win32_compat.h"   /* QueryPerformanceCounter on every host */

#include <stdint.h>

extern uint8_t* vm_base;

int64_t lv2_usec_deadline(uint64_t usec)
{
    LARGE_INTEGER now;
    LARGE_INTEGER freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    return now.QuadPart + (int64_t)((usec * (uint64_t)freq.QuadPart) / 1000000ull);
}

int lv2_deadline_passed(int64_t deadline)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart >= deadline;
}

void ppu_thread_note_block(uint64_t tid) { (void)tid; }   /* sys_ppu_thread.c: PS3_PRIO_START */

uint32_t vm_read32(uint64_t addr)
{
    const uint8_t* p = vm_base + (uint32_t)addr;
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}
