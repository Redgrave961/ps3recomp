#ifndef PPU_MEMORY_H
#define PPU_MEMORY_H

#include "../../include/ps3emu/endian.h"
#include "ppu_context.h"
#include "../spu/spu_coh_bitmap.h"
#include "../memory/vm_page_map.h"

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

extern uint8_t* vm_base;

#ifndef GUEST_PTR
#define GUEST_PTR(p, T) ((T)((p) ? (void*)(vm_base + (uint32_t)(uintptr_t)(p)) : (void*)0))
#endif

static inline void* vm_translate(uint32_t addr)
{
    return (void*)(vm_base + addr);
}

static inline uint8_t* vm_ptr8(uint32_t addr)  { return (uint8_t*)vm_translate(addr); }

static inline uint8_t vm_read8(uint32_t addr)
{
    return *vm_ptr8(addr);
}

static inline uint16_t vm_read16(uint32_t addr)
{
    uint16_t raw;
    memcpy(&raw, vm_ptr8(addr), sizeof(raw));
    return ps3_bswap16(raw);
}

static inline uint32_t vm_read32(uint32_t addr)
{
    uint32_t raw;
    memcpy(&raw, vm_ptr8(addr), sizeof(raw));
    return ps3_bswap32(raw);
}

static inline uint64_t vm_read64(uint32_t addr)
{
    uint64_t raw;
    memcpy(&raw, vm_ptr8(addr), sizeof(raw));
    return ps3_bswap64(raw);
}

#ifdef __cplusplus
extern "C" {
#endif
int         spu_coh_is_reserved(uint32_t addr);
void        spu_lockline_lock(uint32_t ea);
void        spu_lockline_unlock(uint32_t ea);
void        spu_lockline_lock_range(uint32_t ea, uint32_t size);
void        spu_lockline_unlock_range(uint32_t ea, uint32_t size);
void        spu_coh_notify_write(uint32_t addr);
#ifdef __cplusplus
}
#endif

#define VM_WRITE_COH(addr, src, n)                                            \
    do {                                                                      \
        if (spu_coh_line_reserved((uint32_t)(addr))) {                        \
            spu_lockline_lock((uint32_t)(addr));                              \
            memcpy(vm_ptr8((uint32_t)(addr)), (src), (n));                    \
            spu_coh_notify_write((uint32_t)(addr));                           \
            spu_lockline_unlock((uint32_t)(addr));                            \
        } else {                                                              \
            memcpy(vm_ptr8((uint32_t)(addr)), (src), (n));                    \
        }                                                                     \
        vm_track_write((uint32_t)(addr), (n));                                \
    } while (0)

extern uint32_t g_ww_lo, g_ww_hi;
void ps3_ww_report_inline(uint32_t addr, uint64_t val, int width);

#define PS3_WW_CHECK(a, v, w)                                                 \
    do {                                                                      \
        if ((a) >= g_ww_lo && (a) < g_ww_hi)                                  \
            ps3_ww_report_inline((uint32_t)(a), (uint64_t)(v), (w));          \
    } while (0)

static inline void vm_write8(uint32_t addr, uint8_t val)
{
    PS3_WW_CHECK(addr, val, 1);
    VM_WRITE_COH(addr, &val, 1);
}

static inline void vm_write16(uint32_t addr, uint16_t val)
{
    PS3_WW_CHECK(addr, val, 2);
    uint16_t raw = ps3_bswap16(val);
    VM_WRITE_COH(addr, &raw, sizeof(raw));
}

static inline void vm_write32(uint32_t addr, uint32_t val)
{
    PS3_WW_CHECK(addr, val, 4);
    uint32_t raw = ps3_bswap32(val);
    VM_WRITE_COH(addr, &raw, sizeof(raw));
}

static inline void vm_write64(uint32_t addr, uint64_t val)
{
    PS3_WW_CHECK(addr, val, 8);
    uint64_t raw = ps3_bswap64(val);
    VM_WRITE_COH(addr, &raw, sizeof(raw));
}

#ifdef __cplusplus
}
#endif

#endif
