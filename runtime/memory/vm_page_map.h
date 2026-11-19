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
VM_PAGE_MAP_SHARED uint32_t vm_watch_ea = 0;

static inline void ppu_slow_pages(uint32_t ea, uint64_t len)
{
    for (uint64_t p = ea >> VM_SLOW_PAGE_SHIFT; len && p <= ((uint64_t)ea + len - 1) >> VM_SLOW_PAGE_SHIFT; p++)
        __atomic_fetch_or(&ppu_slow_page[p], 1, __ATOMIC_RELEASE);
}

static inline void vm_watch(uint32_t ea)
{
    __atomic_store_n(&vm_watch_ea, ea, __ATOMIC_RELAXED);
    __atomic_fetch_or(&ppu_slow_page[ea >> VM_SLOW_PAGE_SHIFT], 2, __ATOMIC_SEQ_CST);
}

static inline void vm_unwatch(uint32_t ea)
{
    __atomic_fetch_and(&ppu_slow_page[ea >> VM_SLOW_PAGE_SHIFT], (unsigned char)~2u, __ATOMIC_RELEASE);
    __atomic_store_n(&vm_watch_ea, 0, __ATOMIC_RELEASE);
}

static inline int vm_watched(uint32_t ea, uint32_t size)
{
    const uint32_t w = __atomic_load_n(&vm_watch_ea, __ATOMIC_ACQUIRE);
    return w && w - ea < size;
}

#ifdef __cplusplus
}
#endif

#endif
