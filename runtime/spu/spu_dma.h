#ifndef SPU_DMA_H
#define SPU_DMA_H

#include "spu_context.h"
#include "../../include/ps3emu/log_first.h"
#include "spu_coherency.h"
#include "spu_coh_bitmap.h"
#include "../memory/vm_page_map.h"
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "../platform/win32_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

extern uint8_t* vm_base;
extern uint32_t ppu_hle_inject_base;
extern void (*g_rsx_window_write)(void);

static inline void spu_rsx_window_check(uint32_t ea, uint32_t size)
{
    if (__builtin_expect(size <= 4096u && ((ea & 4095u) + size <= 4096u), 1)) {
        const uint32_t p = ea >> VM_SLOW_PAGE_SHIFT;
        const unsigned char flags = __atomic_load_n(&ppu_slow_page[p], __ATOMIC_RELAXED);
        if (__builtin_expect((flags & (VM_PAGE_TRACKED | 2u)) == 0, 1)) {
            void (*notify)(void) = g_rsx_window_write;
            if (__builtin_expect(notify != 0 && ea < ppu_hle_inject_base + 0xB000u && (uint64_t)ea + size > ppu_hle_inject_base, 0))
                notify();
            return;
        }
        if (__builtin_expect(flags & VM_PAGE_TRACKED, 0))
            vm_track_dirty(p);
    } else {
        vm_track_write(ea, size);
    }
    void (*notify)(void) = g_rsx_window_write;
    if (__builtin_expect(notify != 0, 1)) {
        if (__builtin_expect((ea < ppu_hle_inject_base + 0xB000u && (uint64_t)ea + size > ppu_hle_inject_base) ||
                             vm_watched(ea, size), 0))
            notify();
    }
}

static inline const unsigned char* mfc_coh_bits(void)
{
    return __atomic_load_n(&g_spu_coh_bits, __ATOMIC_ACQUIRE);
}

static inline int mfc_line_reserved(const unsigned char* bits, uint32_t addr)
{
    const uint32_t line = addr >> SPU_COH_LINE_SHIFT;
    return bits && ((__atomic_load_n(&bits[line >> 3], __ATOMIC_RELAXED) >> (line & 7u)) & 1u);
}

static inline int mfc_span_reserved(const unsigned char* bits, uint32_t first, uint32_t last)
{
    if (!bits) return 0;
    if (__builtin_expect(first == last, 1)) return mfc_line_reserved(bits, first);
    const uint32_t l0 = first >> SPU_COH_LINE_SHIFT, l1 = last >> SPU_COH_LINE_SHIFT;
    const uint32_t b0 = l0 >> 3, b1 = l1 >> 3;
    const unsigned lo = 0xFFu << (l0 & 7u), hi = 0xFFu >> (7u - (l1 & 7u));
    if (b0 == b1) return (__atomic_load_n(&bits[b0], __ATOMIC_RELAXED) & lo & hi) != 0;
    if (__atomic_load_n(&bits[b0], __ATOMIC_RELAXED) & lo) return 1;
    uint32_t b = b0 + 1u;
    while (b + 8u <= b1) {
        uint64_t w;
        memcpy(&w, bits + b, 8);
        if (w) return 1;
        b += 8u;
    }
    for (; b < b1; b++)
        if (__atomic_load_n(&bits[b], __ATOMIC_RELAXED)) return 1;
    return (__atomic_load_n(&bits[b1], __ATOMIC_RELAXED) & hi) != 0;
}

static inline int mfc_ea_valid(uint64_t ea, uint32_t size)
{
    return ea >= 0x10000u && ea + size <= 0x100000000ull;
}

static inline int mfc_aligned(uint32_t lsa, uint64_t ea, uint32_t size)
{
    if (size >= 16) return size <= 0x4000u && !(size & 15u) && !((lsa | (uint32_t)ea) & 15u);
    return !(size & (size - 1u)) && !((lsa | (uint32_t)ea) & (size - 1u)) && !((lsa ^ (uint32_t)ea) & 15u);
}

typedef uint8_t mfc_v16 __attribute__((vector_size(16)));
typedef uint8_t mfc_v32 __attribute__((vector_size(32)));

static inline void mfc_copy(uint8_t* d, const uint8_t* s, uint32_t n)
{
    mfc_v16 a;
    mfc_v32 w, x, y, z;
    if (n - 16u > 112u) {
        if (n > 128u) {
            uint32_t i = 0;
            for (; i + 128u <= n; i += 128u) {
                memcpy(&w, s + i,      32); memcpy(&x, s + i + 32, 32);
                memcpy(&y, s + i + 64, 32); memcpy(&z, s + i + 96, 32);
                memcpy(d + i,      &w, 32); memcpy(d + i + 32, &x, 32);
                memcpy(d + i + 64, &y, 32); memcpy(d + i + 96, &z, 32);
            }
            if (i < n) {
                if (n - i > 64u) {
                    memcpy(&w, s + i, 32); memcpy(&x, s + i + 32, 32);
                    memcpy(&y, s + n - 64, 32); memcpy(&z, s + n - 32, 32);
                    memcpy(d + i, &w, 32); memcpy(d + i + 32, &x, 32);
                    memcpy(d + n - 64, &y, 32); memcpy(d + n - 32, &z, 32);
                } else if (n - i > 32u) {
                    memcpy(&w, s + i, 32); memcpy(&x, s + n - 32, 32);
                    memcpy(d + i, &w, 32); memcpy(d + n - 32, &x, 32);
                } else {
                    memcpy(&w, s + n - 32, 32);
                    memcpy(d + n - 32, &w, 32);
                }
            }
            return;
        }
        if (n == 8u) { uint64_t t; memcpy(&t, s, 8); memcpy(d, &t, 8); return; }
        if (n == 4u) { uint32_t t; memcpy(&t, s, 4); memcpy(d, &t, 4); return; }
        if (n == 2u) { uint16_t t; memcpy(&t, s, 2); memcpy(d, &t, 2); return; }
        if (n == 1u) { *d = *s; return; }
        memcpy(d, s, n);
        return;
    }
    if (n == 16u) { memcpy(&a, s, 16); memcpy(d, &a, 16); return; }
    if (n == 32u) { memcpy(&w, s, 32); memcpy(d, &w, 32); return; }
    if (n <= 64u) { memcpy(&w, s, 32); memcpy(&x, s + n - 32, 32); memcpy(d, &w, 32); memcpy(d + n - 32, &x, 32); return; }
    memcpy(&w, s, 32); memcpy(&x, s + 32, 32); memcpy(&y, s + n - 64, 32); memcpy(&z, s + n - 32, 32);
    memcpy(d, &w, 32); memcpy(d + 32, &x, 32); memcpy(d + n - 64, &y, 32); memcpy(d + n - 32, &z, 32);
}

static inline void mfc_fault(const spu_context* spu, const char* what, uint32_t cmd, uint32_t lsa, uint64_t ea, uint32_t size)
{
    fprintf(stderr, "\n[mfc] FATAL: %s: img=%d pc=0x%05X cmd=0x%X lsa=0x%05X ea=0x%08llX size=%u\n", what, spu->image_id,
            (uint32_t)spu->pc & SPU_LS_MASK, cmd, lsa, (unsigned long long)ea, size);
    fflush(stderr);
    _Exit(3);
}

#define MFC_QUEUE_DEPTH     16

#define MFC_MAX_DMA_SIZE    (16 * 1024)

typedef struct mfc_cmd {
    uint32_t lsa;
    uint64_t ea;
    uint32_t size;
    uint32_t tag;
    uint32_t cmd;

    int      active;
} mfc_cmd;

typedef struct mfc_list_element {
    uint32_t size_and_flags;
    uint32_t eal;
} mfc_list_element;

typedef struct mfc_engine {
    mfc_cmd     queue[MFC_QUEUE_DEPTH];
    uint32_t    queue_count;

    uint32_t    tag_completed;
} mfc_engine;

extern uint8_t* vm_base;

static inline void mfc_engine_init(mfc_engine* mfc)
{
    memset(mfc, 0, sizeof(*mfc));
    mfc->tag_completed = 0xFFFFFFFF;
}

static inline int mfc_is_get(uint32_t cmd)
{
    return (cmd & 0x40) != 0;
}

static inline int mfc_is_put(uint32_t cmd)
{
    return (cmd & 0x20) != 0 && !mfc_is_get(cmd);
}

static inline int mfc_is_list(uint32_t cmd)
{
    return (cmd & 0x04) != 0;
}

static inline int mfc_do_transfer(spu_context* spu, uint32_t lsa, uint64_t ea,
                                   uint32_t size, uint32_t cmd)
{
    if ((cmd & 0xFFFFu) == MFC_SDCRZ_CMD) {
        if (!size || size > MFC_MAX_DMA_SIZE || (size & 127u) || (ea & 127u) || !mfc_ea_valid(ea, size))
            mfc_fault(spu, "SDCRZ range", cmd, 0, ea, size);
        if (spu->list_stall_mask & (1u << (spu->mfc_tag & 31u))) {
            fprintf(stderr, "[mfc] REJECTED SDCRZ behind stalled tag %u\n",
                    spu->mfc_tag & 31u);
            return -1;
        }
        for (uint32_t offset = 0; offset < size; offset += 128u) {
            const uint32_t line = (uint32_t)ea + offset;
            spu_lockline_lock(line);
            memset(vm_base + line, 0, 128u);
            spu_coh_notify_write_except(line, spu);
            spu_lockline_unlock(line);
        }
        vm_track_write((uint32_t)ea, size);
        return 0;
    }
    if (!size) return 0;
    if (!mfc_aligned(lsa, ea, size)) mfc_fault(spu, "DMA alignment", cmd, lsa, ea, size);
    if (!mfc_ea_valid(ea, size)) mfc_fault(spu, "DMA address", cmd, lsa, ea, size);
    lsa &= SPU_LS_MASK;
    if (lsa + size > SPU_LS_SIZE) mfc_fault(spu, "DMA local store range", cmd, lsa, ea, size);

    uint8_t* ls_ptr = &spu->ls[lsa];
    uint8_t* ea_ptr = vm_base + (uint32_t)ea;

    if (mfc_is_get(cmd)) {
        mfc_copy(ls_ptr, ea_ptr, size);
        { extern uint32_t g_spu_ovl_ea_lo, g_spu_ovl_ea_hi;
          extern int g_spu_ovl_has_sig;
          extern void spu_overlay_note_get(spu_context*, uint32_t, const uint8_t*, uint32_t);
          const uint32_t ea32 = (uint32_t)ea;
          if (__builtin_expect(
                  (spu->resident_code_mask && lsa < spu->resident_code_hi && lsa + size > spu->resident_code_lo) ||
                  (ea32 < __atomic_load_n(&g_spu_ovl_ea_hi, __ATOMIC_RELAXED) &&
                   (uint64_t)ea32 + size > (uint64_t)__atomic_load_n(&g_spu_ovl_ea_lo, __ATOMIC_RELAXED)) ||
                  (size >= 512u && __atomic_load_n(&g_spu_ovl_has_sig, __ATOMIC_RELAXED)), 0))
              spu_overlay_note_get(spu, ea32, (const uint8_t*)ls_ptr, size); }
    } else if (mfc_is_put(cmd)) {
        {
            uint32_t a0 = (uint32_t)ea & ~127u;
            uint32_t a1 = ((uint32_t)ea + size - 1u) & ~127u;
            const unsigned char* bits = mfc_coh_bits();
            const int span_reserved = mfc_span_reserved(bits, a0, a1);
            uint64_t locked_lines[3] = {0, 0, 0};
            if (span_reserved && a0 != a1 && ((a1 - a0) >> 7) < 64u * 3u) {
                const uint32_t ea32 = (uint32_t)ea;
                uint32_t i = 0;
                for (uint32_t a = a0; ; a += 128u, i++) {
                    const uint32_t lo = a > ea32 ? a - ea32 : 0u;
                    const uint32_t hi = a + 128u - ea32 < size ? a + 128u - ea32 : size;
                    if (mfc_line_reserved(bits, a)) locked_lines[i >> 6] |= 1ull << (i & 63u);
                    else mfc_copy(ea_ptr + lo, ls_ptr + lo, hi - lo);
                    if (a == a1) break;
                }
                __atomic_signal_fence(__ATOMIC_SEQ_CST);
                i = 0;
                for (uint32_t a = a0; ; a += 128u, i++) {
                    const uint32_t lo = a > ea32 ? a - ea32 : 0u;
                    const uint32_t hi = a + 128u - ea32 < size ? a + 128u - ea32 : size;
                    if ((locked_lines[i >> 6] >> (i & 63u)) & 1u) {
                        spu_lockline_lock(a);
                        mfc_copy(ea_ptr + lo, ls_ptr + lo, hi - lo);
                        spu_coh_notify_write_except(a, spu);
                        spu_lockline_unlock(a);
                    } else if (mfc_line_reserved(bits, a)) {
                        spu_lockline_lock(a);
                        spu_coh_notify_write_except(a, spu);
                        spu_lockline_unlock(a);
                    }
                    if (a == a1) break;
                }
            } else if (span_reserved) {
                spu_lockline_lock_range((uint32_t)ea, size);
                mfc_copy(ea_ptr, ls_ptr, size);
                for (uint32_t a = a0; ; a += 128u) {
                    if (mfc_line_reserved(bits, a)) spu_coh_notify_write_except(a, spu);
                    if (a == a1) break;
                }
                spu_lockline_unlock_range((uint32_t)ea, size);
            } else {
                mfc_copy(ea_ptr, ls_ptr, size);
                __atomic_signal_fence(__ATOMIC_SEQ_CST);
                bits = mfc_coh_bits();
                if (mfc_span_reserved(bits, a0, a1)) {
                    for (uint32_t a = a0; ; a += 128u) {
                        if (mfc_line_reserved(bits, a)) {
                            spu_lockline_lock(a);
                            spu_coh_notify_write_except(a, spu);
                            spu_lockline_unlock(a);
                        }
                        if (a == a1) break;
                    }
                }
            }
        }
        spu_rsx_window_check((uint32_t)ea, size);
    }

    return 0;
}

static inline int mfc_run_list(spu_context* spu, uint32_t elem_lsa,
                               uint32_t num_left, uint32_t dest_lsa,
                               uint64_t ea_base, uint32_t base_cmd, uint32_t tag)
{
    const int is_get = mfc_is_get(base_cmd);
    extern uint32_t g_spu_ovl_ea_lo, g_spu_ovl_ea_hi;
    extern int g_spu_ovl_has_sig;
    extern void spu_overlay_note_get(spu_context*, uint32_t, const uint8_t*, uint32_t);
    const uint32_t ovl_hi = is_get ? __atomic_load_n(&g_spu_ovl_ea_hi, __ATOMIC_RELAXED) : 0u;
    const uint32_t ovl_lo = is_get ? __atomic_load_n(&g_spu_ovl_ea_lo, __ATOMIC_RELAXED) : 0u;
    const int ovl_sig = is_get ? __atomic_load_n(&g_spu_ovl_has_sig, __ATOMIC_RELAXED) : 0;
    const uint64_t ea_hi = ea_base & 0xFFFFFFFF00000000ull;

    for (uint32_t i = 0; i < num_left; i++) {
        uint32_t el = (elem_lsa + i * 8) & SPU_LS_MASK;
        uint32_t size_and_flags = spu_ls_read32(spu, el);
        uint32_t eal = spu_ls_read32(spu, el + 4);

        uint32_t xfer_size = size_and_flags & 0x7FFF;
        int stall_notify = (size_and_flags >> 31) & 1;

        uint64_t ea = ea_hi | eal;

        if (xfer_size) {
            uint32_t transfer_lsa = (dest_lsa & ~15u) | (eal & 15u);
            if (is_get) {
                if (__builtin_expect(!mfc_aligned(transfer_lsa, ea, xfer_size), 0))
                    mfc_fault(spu, "DMA alignment", base_cmd, transfer_lsa, ea, xfer_size);
                if (__builtin_expect(!mfc_ea_valid(ea, xfer_size), 0))
                    mfc_fault(spu, "DMA address", base_cmd, transfer_lsa, ea, xfer_size);
                const uint32_t lsa = transfer_lsa & SPU_LS_MASK;
                if (__builtin_expect(lsa + xfer_size > SPU_LS_SIZE, 0))
                    mfc_fault(spu, "DMA local store range", base_cmd, lsa, ea, xfer_size);
                uint8_t* ls_ptr = &spu->ls[lsa];
                mfc_copy(ls_ptr, vm_base + (uint32_t)ea, xfer_size);
                if (__builtin_expect(
                        (spu->resident_code_mask && lsa < spu->resident_code_hi && lsa + xfer_size > spu->resident_code_lo) ||
                        (eal < ovl_hi && (uint64_t)eal + xfer_size > (uint64_t)ovl_lo) ||
                        (xfer_size >= 512u && ovl_sig), 0))
                    spu_overlay_note_get(spu, eal, (const uint8_t*)ls_ptr, xfer_size);
            } else {
                int rc = mfc_do_transfer(spu, transfer_lsa, ea, xfer_size, base_cmd);
                if (rc != 0) return rc;
            }
            dest_lsa = (dest_lsa & ~15u) + ((xfer_size + 15u) & ~15u);
        }

        if (stall_notify) {
            uint32_t t = tag & 0x1F;
            spu->list_stall_elem_lsa[t]  = (el + 8) & SPU_LS_MASK;
            spu->list_stall_remaining[t] = num_left - i - 1;
            spu->list_stall_dest_lsa[t]  = dest_lsa;
            spu->list_stall_ea_base[t]   = ea_base;
            spu->list_stall_cmd[t]       = base_cmd;
            spu->list_stall_mask |= (1u << t);
            spu->list_stall_stat |= (1u << t);
            SPU_EVENT_SET(spu, 0x2u);
            spu_ch_wake(spu);
            { static int _n = 0; if (PS3_LOG_FIRST(_n, 16))
                fprintf(stderr, "[mfc-list] STALL img=%d tag=%u elem@0x%05X "
                        "left=%u dest=0x%05X (SN raised, parked mask=0x%X) "
                        "loadJobState@12A0=%02X%02X%02X%02X %02X%02X%02X%02X\n",
                        spu->image_id, t, el, num_left - i - 1, dest_lsa,
                        spu->list_stall_mask,
                        spu->ls[0x12A0], spu->ls[0x12A1],
                        spu->ls[0x12A2], spu->ls[0x12A3],
                        spu->ls[0x12A4], spu->ls[0x12A5],
                        spu->ls[0x12A6], spu->ls[0x12A7]); }
            return 0;
        }
    }
    return 0;
}

static inline int mfc_list_stall_ack(struct mfc_engine* mfc, spu_context* spu,
                                     uint32_t tag)
{
    uint32_t t = tag & 0x1F;
    if (!(spu->list_stall_mask & (1u << t))) {
        static int _n = 0;
        if (PS3_LOG_FIRST(_n, 8))
            fprintf(stderr, "[mfc-list] StallAck tag=%u but no parked list "
                    "(parked mask=0x%X)\n", t, spu->list_stall_mask);
        return 0;
    }
    spu->list_stall_mask &= ~(1u << t);
    { static int _n = 0; if (PS3_LOG_FIRST(_n, 16))
        fprintf(stderr, "[mfc-list] RESUME img=%d tag=%u elem@0x%05X left=%u\n",
                spu->image_id, t, spu->list_stall_elem_lsa[t],
                spu->list_stall_remaining[t]); }
    {
      static volatile LONG _n = 0;
      LONG _seen = InterlockedCompareExchange(&_n, 0, 0);
      int _emit = 0;
      while (_seen < 16) {
          LONG _previous = InterlockedCompareExchange(&_n, _seen + 1, _seen);
          if (_previous == _seen) { _emit = 1; break; }
          _seen = _previous;
      }
      if (_emit) {
        uint32_t _e = spu->list_stall_elem_lsa[t] & SPU_LS_MASK;
        fprintf(stderr, "[mfc-list] RESUME-ELEMS dest=0x%05X elems\n0x%05X:",
                spu->list_stall_dest_lsa[t], _e);
        for (uint32_t _o = 0; _o < 16 && (_e + _o) + 3 < SPU_LS_SIZE; _o += 4)
            fprintf(stderr, " %02X%02X%02X%02X", spu->ls[_e+_o], spu->ls[_e+_o+1],
                    spu->ls[_e+_o+2], spu->ls[_e+_o+3]);
        fprintf(stderr, "\n");
      }
    }
    int rc = mfc_run_list(spu, spu->list_stall_elem_lsa[t], spu->list_stall_remaining[t],
                          spu->list_stall_dest_lsa[t], spu->list_stall_ea_base[t],
                          spu->list_stall_cmd[t], t);
    if (!(spu->list_stall_mask & (1u << t)))
        mfc->tag_completed |= (1u << t);
    return rc;
}

static inline int mfc_do_list_transfer(spu_context* spu, uint32_t list_lsa,
                                        uint64_t ea_base, uint32_t list_size,
                                        uint32_t cmd)
{
    uint32_t num_elements = list_size / 8;
    uint32_t base_cmd = cmd & ~0x04u;
    return mfc_run_list(spu, list_lsa & SPU_LS_MASK, num_elements,
                        spu->mfc_lsa & SPU_LS_MASK, ea_base, base_cmd,
                        spu->mfc_tag);
}

static inline int mfc_submit(mfc_engine* mfc, spu_context* spu, uint32_t cmd)
{
    uint32_t lsa  = spu->mfc_lsa;
    uint64_t ea   = ((uint64_t)spu->mfc_eah << 32) | spu->mfc_eal;
    uint32_t size = spu->mfc_size;
    uint32_t tag  = spu->mfc_tag & 0x1F;
    int rc = 0;

    mfc->tag_completed &= ~(1u << tag);

    if (cmd == MFC_BARRIER_CMD || cmd == MFC_EIEIO_CMD || cmd == MFC_SYNC_CMD) {
        mfc->tag_completed |= (1u << tag);
        return 0;
    }

    if (mfc_is_list(cmd)) {
        { static int _l = 0; if (PS3_LOG_FIRST(_l, 32))
            fprintf(stderr, "[mfc-list] ISSUE img=%d cmd=0x%02X dest_lsa=0x%05X "
                    "list\n0x%05X size=0x%X (%u elems) tag=%u\n",
                    spu->image_id, cmd, lsa, (uint32_t)ea & SPU_LS_MASK,
                    size, size / 8, tag); }
        rc = mfc_do_list_transfer(spu, (uint32_t)ea & SPU_LS_MASK,
                                  ea & 0xFFFFFFFF00000000ull, size, cmd);
    } else {
        rc = mfc_do_transfer(spu, lsa, ea, size, cmd);
    }

    if (!(spu->list_stall_mask & (1u << (tag & 0x1F))) &&
        ((cmd & 0xFFFFu) != MFC_SDCRZ_CMD || rc == 0))
        mfc->tag_completed |= (1u << tag);

    return rc;
}

#define MFC_TAG_UPDATE_IMMEDIATE  0
#define MFC_TAG_UPDATE_ANY        1
#define MFC_TAG_UPDATE_ALL        2

static inline uint32_t mfc_tag_wait(const mfc_engine* mfc, uint32_t tag_mask,
                                     uint32_t update_type)
{
    uint32_t completed = mfc->tag_completed & tag_mask;

    switch (update_type) {
    case MFC_TAG_UPDATE_IMMEDIATE:
        return completed;
    case MFC_TAG_UPDATE_ANY:
        return completed;
    case MFC_TAG_UPDATE_ALL:
        return completed;
    default:
        return completed;
    }
}

static inline void mfc_channel_write(mfc_engine* mfc, spu_context* spu,
                                      uint32_t channel, uint32_t value)
{
    switch (channel) {
    case MFC_LSA:
        spu->mfc_lsa = value;
        break;
    case MFC_EAH:
        spu->mfc_eah = value;
        break;
    case MFC_EAL:
        spu->mfc_eal = value;
        break;
    case MFC_Size:
        spu->mfc_size = value;
        break;
    case MFC_TagID:
        spu->mfc_tag = value & 0x1F;
        break;
    case MFC_Cmd:
        mfc_submit(mfc, spu, value);
        break;
    case MFC_WrTagMask:
        spu->mfc_tag_mask = value;
        break;
    case MFC_WrTagUpdate:
        spu->mfc_tag_status = mfc_tag_wait(mfc, spu->mfc_tag_mask, value);
        break;
    case MFC_WrListStallAck:
        mfc_list_stall_ack(mfc, spu, value);
        break;
    default:
        break;
    }
}

static inline uint32_t mfc_channel_read(spu_context* spu,
                                         uint32_t channel)
{
    switch (channel) {
    case MFC_RdTagStat:
        return spu->mfc_tag_status;
    case MFC_RdTagMask:
        return spu->mfc_tag_mask;
    case MFC_RdListStallStat: {
        uint32_t m = spu->list_stall_stat;
        spu->list_stall_stat = 0;
        return m;
    }
    case MFC_RdAtomicStat:
        return spu->atomic_stat;
    default:
        return 0;
    }
}

#ifdef __cplusplus
}
#endif

#endif
