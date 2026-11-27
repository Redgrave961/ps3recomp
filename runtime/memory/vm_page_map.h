#ifndef VM_PAGE_MAP_H
#define VM_PAGE_MAP_H

#include <stdint.h>

#if defined(_WIN32)
#  define VM_PAGE_MAP_SHARED __declspec(selectany)
#else
#  define VM_PAGE_MAP_SHARED __attribute__((weak))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define VM_SLOW_PAGE_SHIFT 12
#define VM_PAGE_SPECIAL 1u
#define VM_PAGE_TRACKED 4u
#define VM_PAGE_COHERENT 8u

VM_PAGE_MAP_SHARED unsigned char ppu_slow_page[1u << (32 - VM_SLOW_PAGE_SHIFT)] = {0};
VM_PAGE_MAP_SHARED unsigned char vm_page_dirty[1u << (32 - VM_SLOW_PAGE_SHIFT)] = {0};
VM_PAGE_MAP_SHARED uint64_t vm_dirty_group[1u << (32 - VM_SLOW_PAGE_SHIFT - 12)] = {0};
VM_PAGE_MAP_SHARED uint32_t vm_watch_ea = 0;

static inline void vm_page_mark_dirty(uint32_t page)
{
    __atomic_store_n(&vm_page_dirty[page], 1, __ATOMIC_RELAXED);
    __atomic_fetch_or(&vm_dirty_group[page >> 12], 1ull << ((page >> 6) & 63u), __ATOMIC_RELEASE);
}

static inline void vm_track_dirty(uint32_t page)
{
    if (__atomic_fetch_and(&ppu_slow_page[page], (unsigned char)~VM_PAGE_TRACKED, __ATOMIC_ACQ_REL) & VM_PAGE_TRACKED)
        vm_page_mark_dirty(page);
}

static inline void vm_track_write(uint32_t ea, uint64_t len)
{
    if (!len) return;
    const uint64_t p0 = ea >> VM_SLOW_PAGE_SHIFT;
    const uint64_t p1 = ((uint64_t)ea + len - 1) >> VM_SLOW_PAGE_SHIFT;
    if (__builtin_expect(p0 == p1, 1)) {
        if (__atomic_load_n(&ppu_slow_page[p0], __ATOMIC_RELAXED) & VM_PAGE_TRACKED)
            vm_track_dirty((uint32_t)p0);
        return;
    }
    for (uint64_t p = p0; p <= p1; p++)
        if (__atomic_load_n(&ppu_slow_page[p], __ATOMIC_RELAXED) & VM_PAGE_TRACKED) vm_track_dirty((uint32_t)p);
}

static inline void vm_page_bits(uint32_t ea, uint64_t len, unsigned char bits)
{
    for (uint64_t p = ea >> VM_SLOW_PAGE_SHIFT; len && p <= ((uint64_t)ea + len - 1) >> VM_SLOW_PAGE_SHIFT; p++)
        __atomic_fetch_or(&ppu_slow_page[p], bits, __ATOMIC_RELEASE);
}

static inline void ppu_special_pages(uint32_t ea, uint64_t len) { vm_page_bits(ea, len, VM_PAGE_SPECIAL); }
static inline void ppu_slow_pages(uint32_t ea, uint64_t len) { vm_page_bits(ea, len, VM_PAGE_COHERENT); }

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
