/*
 * ps3recomp - PPU memory access for recompiled code
 *
 * All PS3 memory is big-endian.  These functions translate a 32-bit PS3
 * virtual address to a host pointer via vm_base, then perform a load or
 * store with the appropriate byte swap.
 */

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

/* ---------------------------------------------------------------------------
 * Base pointer -- set by the VM manager at startup.
 *
 * Host pointer to the beginning of the PS3 32-bit address space mapping.
 * All guest address translation is:  host_ptr = vm_base + guest_addr
 * -----------------------------------------------------------------------*/
extern uint8_t* vm_base;

/* ---------------------------------------------------------------------------
 * Address translation
 * -----------------------------------------------------------------------*/
/* Translate a guest pointer PARAMETER to a host pointer, preserving NULL.
 *
 * HLE entry points receive pointer arguments as raw 32-bit GUEST addresses
 * (ppu_hle.cpp passes the PPC register through untouched), so dereferencing
 * one directly hits whatever host address happens to share that number. That
 * is the most common bug in libs/ -- cellFsRead read an entire sound bank to
 * an untranslated address this way. Fifteen files had each pasted their own
 * copy of this macro; this is the one definition.
 *
 * NOTE this gives you a host pointer to BIG-ENDIAN memory. It is right for
 * byte buffers and for structs of u8, but a u32/u64/float field still needs
 * the vm_read and vm_write accessors below (or an explicit swap) -- the cast
 * alone does not fix endianness.
 */
#ifndef GUEST_PTR
#define GUEST_PTR(p, T) ((T)((p) ? (void*)(vm_base + (uint32_t)(uintptr_t)(p)) : (void*)0))
#endif

static inline void* vm_translate(uint32_t addr)
{
    return (void*)(vm_base + addr);
}

static inline uint8_t* vm_ptr8(uint32_t addr)  { return (uint8_t*)vm_translate(addr); }

/* ---------------------------------------------------------------------------
 * Loads -- read from big-endian guest memory, return host-endian value.
 * -----------------------------------------------------------------------*/
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

/* ---------------------------------------------------------------------------
 * Stores -- write host-endian value into big-endian guest memory.
 * -----------------------------------------------------------------------*/
#ifdef __cplusplus
extern "C" {
#endif
/* PPU <-> SPU lock-line coherence (runtime/spu/spu_coherency.c). A PPU store to
 * a 128-byte line an SPU has reserved with GETLLAR has to serialize through the
 * SPU lock-line lock -- otherwise it can land inside a PUTLLC's compare and be
 * written straight back over -- and has to raise SPU_EVENT_LR, which is how an
 * SPURS idle service parked in `rdch SPU_RdEventStat` learns work was posted.
 * Declared here rather than pulling spu_coherency.h into every HLE module.
 *
 * The stores test the bitmap inline (spu_coh_bitmap.h): a pointer that stays
 * NULL until an SPU reserves its first line, so the ordinary store pays one
 * predictable branch. */
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

/* ---------------------------------------------------------------------------
 * Write-watch hook for the INLINE writers.
 *
 * PPU_WWATCH (ppu_loader.cpp) logs stores to a watched address along with the guest
 * function responsible. Recompiled guest code calls the extern vm_write* family
 * there, so it is covered -- but everything in libs/ uses the inline family
 * below, and those stores were invisible to it. That makes "nothing writes this
 * address" a claim the watch cannot actually support, which is exactly the sort
 * of thing worth not being wrong about: an HLE writing a zero looks identical
 * to nobody writing at all.
 *
 * Cost is two compares against a pair that stay 0 unless PPU_WWATCH is set, on the
 * HLE path only -- guest stores never reach here.
 * -----------------------------------------------------------------------*/
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
} /* extern "C" */
#endif

#endif /* PPU_MEMORY_H */
