#ifndef VM_PAGE_MAP_H
#define VM_PAGE_MAP_H

#include <stdint.h>

#if defined(_MSC_VER) || defined(__MINGW32__)
#  define VM_PAGE_MAP_SHARED __declspec(selectany)
#else
#  define VM_PAGE_MAP_SHARED __attribute__((weak))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define VM_SLOW_PAGE_SHIFT 12

VM_PAGE_MAP_SHARED unsigned char ppu_slow_page[1u << (32 - VM_SLOW_PAGE_SHIFT)] = {0};

static inline void ppu_slow_pages(uint32_t ea, uint64_t len)
{
    for (uint64_t p = ea >> VM_SLOW_PAGE_SHIFT; len && p <= ((uint64_t)ea + len - 1) >> VM_SLOW_PAGE_SHIFT; p++)
        __atomic_store_n(&ppu_slow_page[p], 1, __ATOMIC_RELEASE);
}

#ifdef __cplusplus
}
#endif

#endif
