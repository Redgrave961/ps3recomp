#ifndef PPU_RESV_H
#define PPU_RESV_H

#include "../memory/vm_page_map.h"

extern "C" uint8_t* vm_base;
extern "C" int  spu_coh_is_reserved(uint32_t addr);
extern "C" void spu_lockline_lock(uint32_t ea);
extern "C" void spu_lockline_unlock(uint32_t ea);
extern "C" void spu_coh_notify_write(uint32_t addr);
static inline void vm_coh_notify(uint32_t a)
{
    spu_lockline_lock(a);
    spu_coh_notify_write(a);
    spu_lockline_unlock(a);
}

extern "C" PPU_THREAD_LOCAL ppu_context* g_active_ctx;

static inline uint64_t* resv_line(uint32_t ea) { return &g_ppu_resv[(ea >> 7) & PPU_RESV_MASK]; }

static inline void ppu_resv_break(uint32_t ea)
{
    uint64_t* const line = resv_line(ea);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (!(__atomic_load_n(line, __ATOMIC_RELAXED) & PPU_RESV_LIVE)) return;
    const uint64_t old = __atomic_fetch_add(line, 2, __ATOMIC_ACQ_REL);
    ppu_context* self = g_active_ctx;
    if (self && (((uint32_t)self->reserve_addr ^ ea) & ~127u) == 0 && self->reserve_time == old)
        self->reserve_time = old + 2;
}

template <class T>
static inline int ppu_store_conditional(ppu_context* self, uint32_t a, T expected, T val)
{
    uint64_t* line = resv_line(a);
    uint64_t t = self->reserve_time;
    if ((t & 1) || !__atomic_compare_exchange_n(line, &t, t + 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 0;
    const int coh = spu_coh_is_reserved(a);
    if (coh) {
        spu_lockline_lock(a);
        if (__atomic_load_n(line, __ATOMIC_ACQUIRE) != t + 1) {
            spu_lockline_unlock(a);
            __atomic_fetch_sub(line, 1, __ATOMIC_RELEASE);
            return 0;
        }
    }
    T raw, swapped;
    if constexpr (sizeof(T) == 4) raw = __builtin_bswap32(expected), swapped = __builtin_bswap32(val);
    else raw = __builtin_bswap64(expected), swapped = __builtin_bswap64(val);
    const int ok = __atomic_compare_exchange_n((T*)(vm_base + a), &raw, swapped, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    if (coh) {
        if (ok) spu_coh_notify_write(a);
        spu_lockline_unlock(a);
    } else if (ok && spu_coh_is_reserved(a)) {
        vm_coh_notify(a);
    }
    __atomic_fetch_add(line, ok ? 1 : ~0ull, __ATOMIC_RELEASE);
    if (ok) vm_track_write(a, sizeof(T));
    return ok;
}

#endif
