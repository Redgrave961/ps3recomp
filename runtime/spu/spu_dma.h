/*
 * ps3recomp - SPU DMA engine (MFC)
 *
 * The Memory Flow Controller (MFC) handles DMA transfers between an SPU's
 * 256 KB local store and the main memory (EA space).  Each SPU has a 16-entry
 * MFC command queue.
 *
 * Supported operations:
 *   - DMA get/put  (local store <-> main memory)
 *   - DMA list commands (scatter/gather)
 *   - Tag group management and synchronization
 *   - Barrier and fence
 */

#ifndef SPU_DMA_H
#define SPU_DMA_H

#include "spu_context.h"
#include "../../include/ps3emu/log_first.h"
#include "spu_coherency.h"
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
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
    void (*notify)(void) = g_rsx_window_write;
    if (notify && ea < ppu_hle_inject_base + 0x10000u && (uint64_t)ea + size > ppu_hle_inject_base) notify();
}

/* Guard against a guest DMA whose effective address lands in reserved-but-
 * uncommitted guest memory (the 4 GB VM is MEM_RESERVE; only main mem / RSX /
 * SPU / lv2-heap / stack pages are committed). A garbage EA must be treated
 * as a failed DMA, NOT crash the host emulator with an access violation.
 * Queries the page state and demand-commits on first touch; the whole
 * [ea, ea+size) range must be committed and accessible.
 * Returns 1 if the range is safe to memcpy. */
static inline int mfc_ea_range_committed(uint64_t ea, uint32_t size)
{
    /* The VM reserves the full 32-bit guest space but only commits specific
     * regions (main mem, stack, RSX). A DMA to an uncommitted page must
     * commit it on demand rather than crash. A bitmap caches the per-64K-page
     * committed state so the steady-state cost is two bit tests; only a first
     * touch per page pays the VirtualQuery syscall. */
    uint32_t e = (uint32_t)ea;
    if (size == 0) return 0;
    if ((uint64_t)e + (uint64_t)size > 0x100000000ull) return 0;   /* past 4 GB */
    if (!vm_base) return 0;
    /* Committed-page bitmap as a self-healing CACHE of VirtualQuery. The
     * demand-commit fault handler seeds it, but regions the host commits
     * directly (never faulting) start unseeded. On a miss, fall back to ONE VirtualQuery and remember
     * the answer: first touch per 64K page pays the syscall, steady state is
     * two bit tests. (The all-syscall version was 94 CPU-s per 50 s run.) */
    { extern uint8_t g_vm_page_bitmap[65536 / 8];
      uint32_t last = e + size - 1;
      uint32_t pg[2] = { e >> 16, last >> 16 };
      for (int i = 0; i < (pg[0] == pg[1] ? 1 : 2); i++) {
          if ((g_vm_page_bitmap[pg[i] >> 3] >> (pg[i] & 7)) & 1) continue;
          MEMORY_BASIC_INFORMATION mbi;
          uint8_t* p = vm_base + ((uintptr_t)pg[i] << 16);
          if (VirtualQuery(p, &mbi, sizeof mbi) == 0) return 0;
          if (mbi.State != MEM_COMMIT) {
              if (!VirtualAlloc(p, 0x10000, MEM_COMMIT, PAGE_READWRITE)) return 0;
          } else if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) {
              return 0;
          }
          g_vm_page_bitmap[pg[i] >> 3] |= (uint8_t)(1u << (pg[i] & 7));
      }
    }
    return 1;
}

/* MFC queue depth */
#define MFC_QUEUE_DEPTH     16

/* Maximum DMA transfer size per element */
#define MFC_MAX_DMA_SIZE    (16 * 1024)

/* ---------------------------------------------------------------------------
 * MFC command entry
 * -----------------------------------------------------------------------*/
typedef struct mfc_cmd {
    uint32_t lsa;       /* Local store address */
    uint64_t ea;        /* Effective (main memory) address: EAH << 32 | EAL */
    uint32_t size;      /* Transfer size in bytes */
    uint32_t tag;       /* Tag group ID (0-31) */
    uint32_t cmd;       /* Command opcode (MFC_PUT_CMD, MFC_GET_CMD, etc.) */

    /* Status */
    int      active;    /* 1 = pending, 0 = completed/free */
} mfc_cmd;

/* DMA list element (used by PUTL/GETL) */
typedef struct mfc_list_element {
    /* In PS3 memory these are big-endian:
     *   bits  0-14 : reserved (notify/stall flags in upper halfword)
     *   bit   15   : stall-and-notify
     *   bits 16-31 : transfer size (low 16 bits)
     *   bits 32-63 : effective address low 32 bits
     */
    uint32_t size_and_flags;
    uint32_t eal;
} mfc_list_element;

/* ---------------------------------------------------------------------------
 * MFC DMA engine state
 * -----------------------------------------------------------------------*/
typedef struct mfc_engine {
    mfc_cmd     queue[MFC_QUEUE_DEPTH];
    uint32_t    queue_count;

    /* Per-tag completion tracking (bitmask: bit N = tag N completed) */
    uint32_t    tag_completed;
} mfc_engine;

/* External: pointer to host mapping of PS3 main memory.
 * Must be set by the VM manager before any DMA. */
extern uint8_t* vm_base;

/* ---------------------------------------------------------------------------
 * Initialization
 * -----------------------------------------------------------------------*/
static inline void mfc_engine_init(mfc_engine* mfc)
{
    memset(mfc, 0, sizeof(*mfc));
    mfc->tag_completed = 0xFFFFFFFF; /* all tags idle = completed */
}

/* ---------------------------------------------------------------------------
 * Core DMA transfer (synchronous for recompiled code)
 *
 * In real hardware this would be asynchronous.  For a static recompiler the
 * DMA is executed immediately; the tag status is updated after completion.
 * -----------------------------------------------------------------------*/

static inline int mfc_is_get(uint32_t cmd)
{
    return (cmd & 0x40) != 0; /* GET family has bit 6 set */
}

static inline int mfc_is_put(uint32_t cmd)
{
    return (cmd & 0x20) != 0 && !mfc_is_get(cmd);
}

static inline int mfc_is_list(uint32_t cmd)
{
    return (cmd & 0x04) != 0; /* list variants have bit 2 set */
}

static inline int mfc_is_barrier(uint32_t cmd)
{
    return (cmd & 0x01) != 0; /* barrier variants have bit 0 set */
}

static inline int mfc_is_fence(uint32_t cmd)
{
    return (cmd & 0x02) != 0; /* fence variants have bit 1 set */
}

/*
 * Execute a single DMA transfer between local store and main memory.
 * Returns 0 on success, -1 on error.
 */
static inline int mfc_do_transfer(spu_context* spu, uint32_t lsa, uint64_t ea,
                                   uint32_t size, uint32_t cmd)
{
    /* Upper16 command-channel bits carry replacement/transfer class IDs. */
    if ((cmd & 0xFFFFu) == MFC_SDCRZ_CMD) {
        /* CBEA 1.02 section 7.7.3: SDCRZ writes zeros in EA space, without an
         * LSA operand. Storage-control alignment is implementation-specific
         * (table 7-6); currently support whole 128-byte blocks, up to 16 KiB.
         * Reject other forms visibly instead of applying ordinary LS DMA
         * validation or claiming an unsupported command completed. */
        if (!size || size > MFC_MAX_DMA_SIZE || (size & 127u) || (ea & 127u) ||
            (ea >> 32) || !mfc_ea_range_committed(ea, size)) {
            fprintf(stderr, "[mfc] REJECTED SDCRZ range: ea=0x%016llX size=%u\n",
                    (unsigned long long)ea, size);
            return -1;
        }
        /* Storage controls impose a barrier in their tag group. Ordinary
         * transfers finish synchronously, but a stalled list does not. This
         * engine cannot execute SDCRZ ahead of that still-pending transfer. */
        if (spu->list_stall_mask & (1u << (spu->mfc_tag & 31u))) {
            fprintf(stderr, "[mfc] REJECTED SDCRZ behind stalled tag %u\n",
                    spu->mfc_tag & 31u);
            return -1;
        }
        /* Serialize zeroing with reservation compare-and-commit. Notify each
         * touched line even for a zero-to-zero write: byte comparisons alone
         * cannot detect that a peer's reservation was lost. Issuer exclusion
         * follows the existing plain-MFC-write model; issuer reservation
         * behavior for storage controls is not separately modeled here. */
        spu_lockline_lock();
        memset(vm_base + (uint32_t)ea, 0, size);
        for (uint32_t offset = 0; offset < size; offset += 128u)
            spu_coh_notify_write_except((uint32_t)ea + offset, spu);
        spu_lockline_unlock();
        return 0;
    }
    /* Malformed transfers are REJECTED, as the hardware MFC does. The rules
     * are the ones the SDK's own dma.h asserts: a non-zero size, at most
     * 16 KB, a multiple of 16 once >= 16 bytes, and LSA/EA sharing 16-byte
     * alignment. Real hardware raises an MFC alignment exception and does
     * NOT move the data; performing it anyway lets one bad descriptor spray
     * the guest image. YDKJ issues exactly one such PUT -- 16 KB from a
     * 4-byte-aligned EA of 0x00542004, straight over the title's own data --
     * and every later call through the clobbered OPDs reports
     * "code 0x00000000 not registered". */
    {
          /* A PUT into the null page is the same class of accident: lv2 reserves
           * the low 64 KB and no title DMAs there on purpose, so an EA that
           * small means the descriptor field holding the real destination came
           * through as zero. GETs from low EAs stay allowed: they only read
           * garbage. */
          int malformed = (size == 0) || (size > 0x4000)
                       || (size >= 16 && (size & 15))
                       || (((lsa ^ (uint32_t)ea) & 15) != 0)
                       || (mfc_is_put(cmd) && (uint32_t)ea < 0x10000u);
          if (malformed) {
              static int _n = 0;
              if (PS3_LOG_FIRST(_n, 8))
                  fprintf(stderr, "[mfc] REJECTED malformed transfer: img=%d pc=0x%05X"
                                  " cmd=0x%X lsa=0x%05X ea=0x%08X size=%u\n",
                          spu->image_id, (uint32_t)spu->pc & SPU_LS_MASK, cmd,
                          lsa, (uint32_t)ea, size);
              return 0;
          } }
    /* Validate size */
    if (size == 0 || size > MFC_MAX_DMA_SIZE)
        return -1;

    /* Mask LSA to local store range */
    lsa &= SPU_LS_MASK;

    /* Reject transfers that would overrun the 256 KB local store buffer (a real
     * SPU MFC requires the LS range to be valid; an out-of-range lsa+size here
     * would corrupt host heap past spu->ls). */
    if ((uint64_t)lsa + (uint64_t)size > (uint64_t)SPU_LS_SIZE)
        return -1;

    /* Reject transfers whose main-memory range is not committed guest memory,
     * so a garbage EA (e.g. from an incomplete SPURS context) is a failed DMA
     * rather than a host segfault. */
    if (!mfc_ea_range_committed(ea, size)) {
        static int s_warned = 0;
        if (PS3_LOG_FIRST(s_warned, 32))
            fprintf(stderr, "[spu-dma] SKIP %s img=%d pc=0x%05X lsa=0x%05X ea=0x%08X "
                    "size=%u%s (EA not committed -- bad/garbage DMA target)\n",
                    mfc_is_get(cmd) ? "GET" : "PUT", spu->image_id,
                    (uint32_t)spu->pc & SPU_LS_MASK, lsa, (uint32_t)ea, size,
                    (size <= 8 && ((uint32_t)ea & (size - 1))) ? " MISALIGNED" : "");
        return -1;
    }

    uint8_t* ls_ptr = &spu->ls[lsa];
    uint8_t* ea_ptr = vm_base + (uint32_t)ea; /* PS3 uses 32-bit effective addresses for SPU DMA */

    if (mfc_is_get(cmd)) {
        /* GET: main memory -> local store */
        memcpy(ls_ptr, ea_ptr, size);
        /* Swappable-overlay tracking: a GET from a registered overlay source
         * (by EA or by content signature) marks that overlay's lifted
         * functions resident for this context. */
        { extern void spu_overlay_note_get(spu_context*, uint32_t, const uint8_t*, uint32_t);
          spu_overlay_note_get(spu, (uint32_t)ea, (const uint8_t*)ls_ptr, size); }
    } else if (mfc_is_put(cmd)) {
        /* PUT: local store -> main memory.
         *
         * A plain PUT is a store by another processor as far as every SPU
         * holding a reservation on the lines it covers is concerned: on
         * hardware those reservations are lost and their SPUs take
         * SPU_EVENT_LR. Unannounced, a peer's pending PUTLLC compares its
         * snapshot against a line this copy has already replaced and commits
         * over it, and a peer parked on RdEventStat sleeps through the write.
         *
         * So a reserved line's copy and its notify run as ONE critical section
         * under the lock-line lock -- the same contract the PPU store path and
         * the PUTLLC commit keep, and the only way the copy cannot land inside
         * a peer's compare-and-commit window. Only the reserved lines need it:
         * in a multi-line span the others are copied first, without the lock,
         * exactly as an unreserved span is, and the locked pass then copies the
         * lines it skipped and notifies every reserved one. A line reserved
         * after its unlocked copy is notified there but NOT copied again: a
         * peer may already have reserved and committed it on top of our bytes,
         * and a second copy would write that commit back over. A wide PUT that
         * touches one queue line no longer holds every other SPU's atomics off
         * for the whole copy. Spans
         * that touch no reserved line keep the lock-free copy: bulk asset PUTs
         * are large and hot, and the scan is a bitmap bit per 128 bytes against
         * a global that stays zero until an SPU reserves its first line.
         *
         * The unreserved path re-checks afterwards because a line can be
         * reserved between the scan and the end of the copy. That peer's
         * snapshot may be torn, so it gets its event and loses its reservation
         * rather than committing against a line it never saw whole. */
        {
            extern int  spu_coh_is_reserved(uint32_t);
            extern void spu_coh_notify_write(uint32_t);
            extern void spu_coh_notify_write_except(uint32_t, const void*);
            extern void spu_lockline_lock(void);
            extern void spu_lockline_unlock(void);
            uint32_t a0 = (uint32_t)ea & ~127u;
            uint32_t a1 = ((uint32_t)ea + size - 1u) & ~127u;
            int span_reserved = 0;
            for (uint32_t a = a0; ; a += 128u) {
                if (spu_coh_is_reserved(a)) { span_reserved = 1; break; }
                if (a == a1) break;
            }
            /* Lines the unlocked pass left for the locked one: a 16 KiB
             * transfer spans at most 129 lines. */
            uint64_t locked_lines[3] = {0, 0, 0};
            if (span_reserved && a0 != a1 && ((a1 - a0) >> 7) < 64u * 3u) {
                const uint32_t ea32 = (uint32_t)ea;
                uint32_t i = 0;
                for (uint32_t a = a0; ; a += 128u, i++) {
                    const uint32_t lo = a > ea32 ? a - ea32 : 0u;
                    const uint32_t hi = a + 128u - ea32 < size ? a + 128u - ea32 : size;
                    if (spu_coh_is_reserved(a)) locked_lines[i >> 6] |= 1ull << (i & 63u);
                    else memcpy(ea_ptr + lo, ls_ptr + lo, hi - lo);
                    if (a == a1) break;
                }
                spu_lockline_lock();
                i = 0;
                for (uint32_t a = a0; ; a += 128u, i++) {
                    const uint32_t lo = a > ea32 ? a - ea32 : 0u;
                    const uint32_t hi = a + 128u - ea32 < size ? a + 128u - ea32 : size;
                    if ((locked_lines[i >> 6] >> (i & 63u)) & 1u) {
                        memcpy(ea_ptr + lo, ls_ptr + lo, hi - lo);
                        spu_coh_notify_write_except(a, spu);
                    } else if (spu_coh_is_reserved(a)) {
                        spu_coh_notify_write_except(a, spu);   /* reserved after its copy */
                    }
                    if (a == a1) break;
                }
                spu_lockline_unlock();
            } else if (span_reserved) {
                spu_lockline_lock();
                memcpy(ea_ptr, ls_ptr, size);
                for (uint32_t a = a0; ; a += 128u) {
                    if (spu_coh_is_reserved(a)) spu_coh_notify_write_except(a, spu);
                    if (a == a1) break;
                }
                spu_lockline_unlock();
            } else {
                memcpy(ea_ptr, ls_ptr, size);
                for (uint32_t a = a0; ; a += 128u) {
                    if (spu_coh_is_reserved(a)) {
                        spu_lockline_lock();
                        spu_coh_notify_write_except(a, spu);
                        spu_lockline_unlock();
                    }
                    if (a == a1) break;
                }
            }
        }
        spu_rsx_window_check((uint32_t)ea, size);
    }

    return 0;
}

/*
 * Execute a DMA list command (scatter/gather).
 * The list resides in the SPU's local store at `lsa`.
 * Each list element describes a (size, EA) pair for a sub-transfer.
 */
/* Core list runner used by both the initial submit and the stall-ack resume.
 * Transfers elements starting at elem_lsa; on a stall-and-notify element
 * (bit 15 of the size halfword) it PARKS the remainder in the context, raises
 * SPU event 0x2 (MFC DMA list stall-and-notify) and returns -- the WWS
 * jobmanager's interrupt handler processes the arrived portion and writes
 * MFC_WrListStallAck to resume the rest (mfc_list_stall_ack below). */
static inline int mfc_run_list(spu_context* spu, uint32_t elem_lsa,
                               uint32_t num_left, uint32_t dest_lsa,
                               uint64_t ea_base, uint32_t base_cmd, uint32_t tag)
{
    for (uint32_t i = 0; i < num_left; i++) {
        uint32_t el = (elem_lsa + i * 8) & SPU_LS_MASK;
        uint32_t size_and_flags = spu_ls_read32(spu, el);
        uint32_t eal = spu_ls_read32(spu, el + 4);

        /* CBEA list element word0 = [S:1][reserved:16][size:15]: the stall-
         * and-notify bit is BIT 31 (the wwsjob sentinel element is exactly
         * 0x80000000 = S set, size 0 -- a pure notification). The old bit-15
         * check never saw it, so the event was never raised. */
        uint32_t xfer_size = size_and_flags & 0x7FFF; /* low 15 bits */
        int stall_notify = (size_and_flags >> 31) & 1;

        uint64_t ea = (ea_base & 0xFFFFFFFF00000000ull) | eal;

        if (xfer_size) {
            /* Each list element occupies whole LS quadwords, even for
             * 1/2/4/8-byte transfers. The EA supplies its byte offset within
             * the first quadword. Keep the rounded cursor for stall/resume. */
            uint32_t transfer_lsa = (dest_lsa & ~15u) | (eal & 15u);
            int rc = mfc_do_transfer(spu, transfer_lsa, ea, xfer_size, base_cmd);
            if (rc != 0) return rc;
            dest_lsa = (dest_lsa & ~15u) + ((xfer_size + 15u) & ~15u);
        }

        if (stall_notify) {
            uint32_t t = tag & 0x1F;
            spu->list_stall_elem_lsa[t]  = (el + 8) & SPU_LS_MASK;
            spu->list_stall_remaining[t] = num_left - i - 1;
            spu->list_stall_dest_lsa[t]  = dest_lsa;
            spu->list_stall_ea_base[t]   = ea_base;
            spu->list_stall_cmd[t]       = base_cmd;
            spu->list_stall_mask |= (1u << t);  /* this tag now has a parked list */
            spu->list_stall_stat |= (1u << t);  /* accumulate into the notify mask */
            /* Reservation invalidation can publish LR from another thread, and
             * WrEventAck clears bits from this one. SPU_EVENT_SET is an atomic
             * fetch-or (#492), so none of the three can read-modify-write a
             * freshly raised notification away -- which is what this needed the
             * lock-line lock for before. */
            SPU_EVENT_SET(spu, 0x2u);           /* SPU_EVENT_SN stall-and-notify */
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

/* Resume a parked list after MFC_WrListStallAck names its tag group. May park
 * again at the next stall element; on full drain the tag group completes. */
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
    spu->list_stall_mask &= ~(1u << t);          /* clears; re-set if it re-parks */
    { static int _n = 0; if (PS3_LOG_FIRST(_n, 16))
        fprintf(stderr, "[mfc-list] RESUME img=%d tag=%u elem@0x%05X left=%u\n",
                spu->image_id, t, spu->list_stall_elem_lsa[t],
                spu->list_stall_remaining[t]); }
    /* What the stall handler left in the remaining elements, and where they are
     * about to land. If the handler was supposed to fill in the claimed job's
     * EA and did not, the elements read back empty and the transfer is a no-op
     * -- which looks identical to "the pipeline never ran" from the PPU side. */
    { /* First 16 dumps per header call site, shared safely by its workers.
       * Saturate instead of wrapping the counter back into logging. */
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
    if (!(spu->list_stall_mask & (1u << t)))      /* ran to the end: tag done */
        mfc->tag_completed |= (1u << t);
    return rc;
}

static inline int mfc_do_list_transfer(spu_context* spu, uint32_t list_lsa,
                                        uint64_t ea_base, uint32_t list_size,
                                        uint32_t cmd)
{
    /* list_size is in bytes; each element is 8 bytes */
    uint32_t num_elements = list_size / 8;
    uint32_t base_cmd = cmd & ~0x04u; /* strip the 'list' bit to get base GET/PUT */
    return mfc_run_list(spu, list_lsa & SPU_LS_MASK, num_elements,
                        spu->mfc_lsa & SPU_LS_MASK, ea_base, base_cmd,
                        spu->mfc_tag);
}

/*
 * Submit and immediately execute an MFC command.
 * This is the main entry point called when the SPU writes to MFC_Cmd.
 */
static inline int mfc_submit(mfc_engine* mfc, spu_context* spu, uint32_t cmd)
{
    uint32_t lsa  = spu->mfc_lsa;
    uint64_t ea   = ((uint64_t)spu->mfc_eah << 32) | spu->mfc_eal;
    uint32_t size = spu->mfc_size;
    uint32_t tag  = spu->mfc_tag & 0x1F;
    int rc = 0;

    /* A job re-issuing the SAME transfer forever is wedged, not slow: nothing
     * about its state can change if the DMA is all it is doing. This used to
     * count only REJECTED transfers, so once the EA check started committing
     * pages on demand the counter never advanced and a spinning job simply hung
     * the chain walker instead. Count repeats whether or not the transfer
     * succeeds, and halt at a known pc rather than hanging silently.
     * The count alone is not enough: polling a value another agent writes is
     * also the same transfer over and over. God of War: Ascension's EDGE
     * geometry jobs wait for ring space by re-reading an RSX label until the
     * GPU catches up, and 256 reads take well under a millisecond, so a
     * count-only rule halted a healthy job and wedged the frame. Halt only once
     * the repeats have also lasted 30 seconds of wall
     * time. Below that, back off 1 ms per repeat rather than spinning. */
    /* One thread-local object: every Darwin TLV access is a call, and this
     * runs on every MFC command. */
    { static SPU_THREAD_LOCAL struct { uint32_t last_ea, last_lsa, rep; unsigned long long since; } t_rep;
#define s_last_ea  t_rep.last_ea
#define s_last_lsa t_rep.last_lsa
#define s_rep      t_rep.rep
#define s_since    t_rep.since
      const uint32_t s_limit = 256u; const unsigned long long s_secs = 30u, s_backoff_ms = 2u;
      extern unsigned long long ps3_ms_now(void);
      if ((uint32_t)ea == s_last_ea && lsa == s_last_lsa) {
          if (++s_rep >= s_limit) {
              unsigned long long now = ps3_ms_now();
              if (!s_since) s_since = now;
              if (now - s_since >= s_secs * 1000u) {
                  s_rep = 0; s_since = 0;   /* re-arm: the chain may run this job again */
                  extern void spu_halt(spu_context*);
                  fprintf(stderr, "[spu-dma] img=%d pc=0x%05X: %u identical "
                          "transfers to ea=0x%08X -- halting the SPU\n",
                          spu->image_id, (uint32_t)spu->pc & SPU_LS_MASK, s_limit,
                          (uint32_t)ea);
                  fflush(stderr);
                  spu_halt(spu);
              }
              /* A job re-reading a word another agent writes (an RSX label, a
               * ring-space counter, a peer job's completion flag) is bound by
               * that write, and a 1 ms sleep per poll lands it up to a
               * millisecond late -- on the frame's critical path when the
               * poller is the frame's longest job. Yield for the first
               * 2 ms of an episode, where a
               * healthy wait ends, and back off 1 ms per repeat after that. */
              else if (now - s_since < s_backoff_ms) SwitchToThread();
              else Sleep(1);
          }
      } else {
          s_last_ea = (uint32_t)ea; s_last_lsa = lsa; s_rep = 0; s_since = 0;
      }
#undef s_last_ea
#undef s_last_lsa
#undef s_rep
#undef s_since
    }

    /* Mark tag as in-progress */
    mfc->tag_completed &= ~(1u << tag);

    /* Handle sync/barrier commands */
    if (cmd == MFC_BARRIER_CMD || cmd == MFC_EIEIO_CMD || cmd == MFC_SYNC_CMD) {
        /* For synchronous emulation these are no-ops; all prior DMAs
         * have already completed. */
        mfc->tag_completed |= (1u << tag);
        return 0;
    }

    /* Execute the transfer.
     * List commands (GETL/PUTL...): per CBEA the LIST's local-store address is
     * carried in MFC_EAL (the PM writes EAL=list, LSA=dest) and each element
     * supplies the low-32 EA; only EAH carries through. Passing `lsa` as the
     * list address read list elements from the transfer DESTINATION. */
    if (mfc_is_list(cmd)) {
        /* List DMAs are rare, so
         * they are easy to miss entirely while diagnosing a pipeline that
         * never fills. Always report the issue: the DESTINATION lsa is the
         * value the whole transfer hangs on. */
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

    /* Mark tag as completed -- EXCEPT a list parked at a stall-and-notify
     * element: its tag group stays incomplete until MFC_WrListStallAck resumes
     * it to the end (mfc_list_stall_ack marks completion then). */
    if (!(spu->list_stall_mask & (1u << (tag & 0x1F))) &&
        ((cmd & 0xFFFFu) != MFC_SDCRZ_CMD || rc == 0))
        mfc->tag_completed |= (1u << tag);

    return rc;
}

/* ---------------------------------------------------------------------------
 * Tag group synchronization
 * -----------------------------------------------------------------------*/

/* Tag update types (values written to MFC_WrTagUpdate) */
#define MFC_TAG_UPDATE_IMMEDIATE  0  /* return status immediately */
#define MFC_TAG_UPDATE_ANY        1  /* wait for any tag in mask */
#define MFC_TAG_UPDATE_ALL        2  /* wait for all tags in mask */

/*
 * Query tag status.  Returns the bitmask of completed tags that match
 * the tag mask.  In synchronous mode, all submitted DMAs are already
 * complete, so this just returns the intersection.
 */
static inline uint32_t mfc_read_tag_status(const mfc_engine* mfc, uint32_t tag_mask)
{
    return mfc->tag_completed & tag_mask;
}

/*
 * Poll for tag completion.
 * update_type: 0 = immediate, 1 = any, 2 = all.
 * Returns the completed tag mask.  In synchronous mode, always succeeds.
 */
static inline uint32_t mfc_tag_wait(const mfc_engine* mfc, uint32_t tag_mask,
                                     uint32_t update_type)
{
    uint32_t completed = mfc->tag_completed & tag_mask;

    switch (update_type) {
    case MFC_TAG_UPDATE_IMMEDIATE:
        return completed;
    case MFC_TAG_UPDATE_ANY:
        /* In async mode we would block until any bit is set.
         * Synchronous: always returns immediately. */
        return completed;
    case MFC_TAG_UPDATE_ALL:
        /* Block until all bits in mask are set. */
        return completed;
    default:
        return completed;
    }
}

/* ---------------------------------------------------------------------------
 * Convenience: process an SPU channel write for MFC-related channels.
 * -----------------------------------------------------------------------*/
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
        /* Acknowledge a stalled list element's tag group: resume the parked
         * list from the element after the stall point (may stall again). */
        mfc_list_stall_ack(mfc, spu, value);
        break;
    default:
        break;
    }
}

static inline uint32_t mfc_channel_read(mfc_engine* mfc, spu_context* spu,
                                         uint32_t channel)
{
    switch (channel) {
    case MFC_RdTagStat:
        return spu->mfc_tag_status;
    case MFC_RdTagMask:
        return spu->mfc_tag_mask;
    case MFC_RdListStallStat: {
        /* Accumulated tag-group mask of newly-stalled lists. Reading returns
         * the mask and clears it (RPCS3 ch_stall_stat semantics); the interrupt
         * handler's IhcLoop then drains every set bit, acking each via ch26. */
        uint32_t m = spu->list_stall_stat;
        spu->list_stall_stat = 0;
        return m;
    }
    case MFC_RdAtomicStat:
        /* Result of the last atomic line op (GETLLAR/PUTLLC/PUTLLUC), set by
         * spu_mfc_atomic(): 0 = PUTLLC_SUCCESS, 1 = PUTLLC_FAILURE (line moved,
         * SPU must retry). Honoring this is what keeps the SPURS lock-free queue
         * consistent across concurrent SPU kernel threads. */
        return spu->atomic_stat;
    default:
        return 0;
    }
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SPU_DMA_H */
