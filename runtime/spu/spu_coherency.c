/*
 * ps3recomp - PPU <-> SPU lock-line coherence
 *
 * See spu_coherency.h for what this is for. The state here is deliberately
 * plain: a bit per 128-byte line, a small table of the SPU contexts that have
 * ever reserved one, and the lock-line spinlock. Everything that WRITES it runs
 * with the lock-line lock held (the SPU's GETLLAR, the PPU's coherent store),
 * so the read-modify-write on a shared bitmap byte is serialized. Only
 * spu_coh_is_reserved reads without the lock, which is the entire point: it is
 * on every PPU store. The one other unlocked path is a GETLLAR that re-reserves
 * a marked line (spu_coh_reserve_fast); its ordering argument is there.
 */

#include <stdlib.h>
#include <string.h>
#include "spu_coherency.h"
#include "spu_coh_bitmap.h"
#include "../memory/vm_page_map.h"
#include "../platform/win32_compat.h"

#include <stdint.h>
#include <stdio.h>
#include "ps3emu/log_first.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "../platform/ps3_sync.h"

#define SPU_LL_STRIPES 256u
#define SPU_LL_SPIN 256u

typedef struct {
    ps3_lock lock;
    uint32_t seq;
} __attribute__((aligned(64))) spu_ll_stripe;

static spu_ll_stripe g_stripes[SPU_LL_STRIPES];

static inline unsigned spu_ll_index(uint32_t ea) { return (ea >> SPU_COH_LINE_SHIFT) & (SPU_LL_STRIPES - 1u); }

static void spu_ll_acquire(spu_ll_stripe* s)
{
    ps3_lock_acquire_spin(&s->lock, SPU_LL_SPIN);
    __atomic_store_n(&s->seq, s->seq + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
}

static void spu_ll_release(spu_ll_stripe* s)
{
    __atomic_store_n(&s->seq, s->seq + 1, __ATOMIC_RELEASE);
    ps3_lock_release(&s->lock);
}

static SPU_THREAD_LOCAL spu_context** t_wake;
static SPU_THREAD_LOCAL unsigned t_wake_n, t_wake_cap;
static volatile uint32_t s_waking;

static void spu_ll_defer_wake(spu_context* c)
{
    if (t_wake_n == t_wake_cap) {
        const unsigned cap = t_wake_cap ? t_wake_cap * 2u : 16u;
        spu_context** grown = (spu_context**)realloc(t_wake, cap * sizeof(*grown));
        if (!grown) {
            fprintf(stderr, "[spu-coh] cannot queue reservation wake\n");
            fflush(stderr);
            abort();
        }
        t_wake = grown;
        t_wake_cap = cap;
    }
    t_wake[t_wake_n++] = c;
}

static int spu_ll_wake_begin(void)
{
    if (!t_wake_n) return 0;
    __atomic_add_fetch(&s_waking, 1u, __ATOMIC_RELAXED);
    return 1;
}

static void spu_ll_wake_end(int pending)
{
    if (!pending) return;
    for (unsigned i = 0; i < t_wake_n; i++) spu_ch_wake(t_wake[i]);
    t_wake_n = 0;
    __atomic_sub_fetch(&s_waking, 1u, __ATOMIC_RELEASE);
}

void spu_lockline_lock(uint32_t ea) { spu_ll_acquire(&g_stripes[spu_ll_index(ea)]); }
void spu_lockline_unlock(uint32_t ea)
{
    const int pending = spu_ll_wake_begin();
    spu_ll_release(&g_stripes[spu_ll_index(ea)]);
    spu_ll_wake_end(pending);
}

static void spu_ll_range_set(uint32_t ea, uint32_t size, uint64_t set[SPU_LL_STRIPES / 64u])
{
    for (unsigned w = 0; w < SPU_LL_STRIPES / 64u; w++) set[w] = 0;
    const uint32_t first = ea >> SPU_COH_LINE_SHIFT;
    const uint32_t lines = size ? ((uint32_t)(ea + (size - 1u)) >> SPU_COH_LINE_SHIFT) - first + 1u : 1u;
    if (lines >= SPU_LL_STRIPES) {
        for (unsigned w = 0; w < SPU_LL_STRIPES / 64u; w++) set[w] = ~0ull;
        return;
    }
    for (uint32_t i = 0; i < lines; i++) {
        const unsigned s = (first + i) & (SPU_LL_STRIPES - 1u);
        set[s >> 6] |= 1ull << (s & 63u);
    }
}

void spu_lockline_lock_range(uint32_t ea, uint32_t size)
{
    uint64_t set[SPU_LL_STRIPES / 64u];
    spu_ll_range_set(ea, size, set);
    for (unsigned s = 0; s < SPU_LL_STRIPES; s++)
        if ((set[s >> 6] >> (s & 63u)) & 1u) spu_ll_acquire(&g_stripes[s]);
}

void spu_lockline_unlock_range(uint32_t ea, uint32_t size)
{
    uint64_t set[SPU_LL_STRIPES / 64u];
    spu_ll_range_set(ea, size, set);
    const int pending = spu_ll_wake_begin();
    for (unsigned s = SPU_LL_STRIPES; s-- > 0;)
        if ((set[s >> 6] >> (s & 63u)) & 1u) spu_ll_release(&g_stripes[s]);
    spu_ll_wake_end(pending);
}

int spu_lockline_unchanged(uint32_t ea, uint32_t seq)
{
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return __atomic_load_n(&g_stripes[spu_ll_index(ea)].seq, __ATOMIC_RELAXED) == seq;
}

static void spu_ll_quiesce(void)
{
    for (unsigned s = 0; s < SPU_LL_STRIPES; s++) {
        ps3_lock_acquire(&g_stripes[s].lock);
        ps3_lock_release(&g_stripes[s].lock);
    }
    while (__atomic_load_n(&s_waking, __ATOMIC_ACQUIRE)) SwitchToThread();
}

/* ===========================================================================
 * The reserved-line bitmap
 *
 * One bit per 128-byte line over the whole 32-bit guest address space: 4 MiB of
 * zero-initialized BSS, of which only the pages around addresses an SPU has
 * actually reserved are ever touched. The reference implementation scopes its
 * bitmap to the one window a single title's SPURS structures live in; this
 * runtime hands out guest memory from several disjoint regions (the ELF image,
 * the sys_memory window at 0x40000000, the raw-SPU local-store windows), and a
 * line outside a hardcoded window would be silently non-coherent, which is the
 * failure this whole file exists to remove.
 * ===========================================================================*/
#define SPU_COH_LINES       (1u << (32 - SPU_COH_LINE_SHIFT))   /* 2^25 lines */
#define SPU_COH_BITMAP_SZ   (SPU_COH_LINES / 8)                 /* 4 MiB */

static unsigned char s_coh_bitmap[SPU_COH_BITMAP_SZ];

/* g_spu_coh_bits (spu_coh_bitmap.h) stays NULL until the first GETLLAR
 * anywhere. That keeps the PPU fast path off the bitmap entirely for a title
 * that runs no SPU code, and keeps every existing test that stores through
 * vm_write* paying one compare against a hot global. */

/* Every live virtual context may retain a reservation between scheduling
 * quanta; physical SPU count is not a bound on this set. Entries are removed
 * before their context's lifetime ends.
 *
 * A contiguous array, not a linked list: every notifier walks all of it with
 * the line held, and a list's scattered nodes were a cache miss per holder on
 * top of the one on each holder's resv, which the walk now prefetches a
 * few entries ahead. */
#define SPU_COH_BLOCK  256u
#define SPU_COH_BLOCKS 256u
static spu_context** s_coh_blocks[SPU_COH_BLOCKS];
static unsigned s_coh_n;
static ps3_lock s_coh_reg;
#define SPU_COH_PREFETCH 4

static inline spu_context** spu_coh_slot(unsigned i) { return &s_coh_blocks[i / SPU_COH_BLOCK][i % SPU_COH_BLOCK]; }
static inline spu_context* spu_coh_holder(unsigned i) { return __atomic_load_n(spu_coh_slot(i), __ATOMIC_ACQUIRE); }
static inline void spu_coh_prefetch_holder(unsigned i, unsigned n)
{
    if (i < n) {
        spu_context* c = spu_coh_holder(i);
        if (c) __builtin_prefetch(&c->resv);
    }
}

unsigned long g_spu_lr_raise = 0;

static void spu_coh_register(spu_context* ctx)
{
    ps3_lock_acquire(&s_coh_reg);
    unsigned i = 0;
    while (i < s_coh_n && spu_coh_holder(i)) i++;
    if (i == s_coh_n && (i / SPU_COH_BLOCK >= SPU_COH_BLOCKS || !s_coh_blocks[i / SPU_COH_BLOCK])) {
        spu_context** block = i / SPU_COH_BLOCK < SPU_COH_BLOCKS
                                  ? (spu_context**)malloc(SPU_COH_BLOCK * sizeof(*block)) : NULL;
        if (!block) {
            fprintf(stderr, "[spu-coh] cannot register reservation holder %p\n", (void*)ctx);
            fflush(stderr);
            abort();
        }
        memset(block, 0, SPU_COH_BLOCK * sizeof(*block));
        __atomic_store_n(&s_coh_blocks[i / SPU_COH_BLOCK], block, __ATOMIC_RELEASE);
    }
    __atomic_store_n(spu_coh_slot(i), ctx, __ATOMIC_RELEASE);
    if (i == s_coh_n) __atomic_store_n(&s_coh_n, i + 1, __ATOMIC_RELEASE);
    __atomic_store_n(&ctx->coh_listed, 1, __ATOMIC_RELAXED);
    ps3_lock_release(&s_coh_reg);
}

void spu_coh_reserve(spu_context* ctx, uint32_t ea)
{
    uint32_t line = ea >> SPU_COH_LINE_SHIFT;
    const unsigned char bit = (unsigned char)(1u << (line & 7));
    if (!(__atomic_load_n(&s_coh_bitmap[line >> 3], __ATOMIC_RELAXED) & bit)) {
        ppu_slow_pages(line << SPU_COH_LINE_SHIFT, 1u << SPU_COH_LINE_SHIFT);
        __atomic_fetch_or(&s_coh_bitmap[line >> 3], bit, __ATOMIC_RELEASE);
        __atomic_store_n(&g_spu_coh_bits, (const unsigned char*)s_coh_bitmap, __ATOMIC_RELEASE);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        FlushProcessWriteBuffers();
    }

    if (ctx && !__atomic_load_n(&ctx->coh_listed, __ATOMIC_RELAXED)) spu_coh_register(ctx);
}

/* Drop a context from the reserving set.
 *
 * There was no way to do this, and the contexts that most need it are
 * TRANSIENT: spu_run_lifted_job_abi runs each job on a STACK-LOCAL
 * spu_context, which registered itself here on its first reservation and then
 * returned. The entry outlived the object, so spu_coh_notify_write walked a
 * dangling pointer -- reading resv, writing event_status and calling
 * spu_ch_wake on reclaimed stack. Undefined behaviour, and observable: Guitar
 * Hero III showed THREE live-looking contexts for one dispatch, stealing each
 * other`s reservations, with every PUTLLC failing "no reservation". */
void spu_coh_unregister(spu_context* ctx)
{
    if (!ctx) return;
    const unsigned n = __atomic_load_n(&s_coh_n, __ATOMIC_ACQUIRE);
    unsigned listed = 0;
    while (listed < n && spu_coh_holder(listed) != ctx) listed++;
    if (listed == n) {
        __atomic_store_n(&ctx->coh_listed, 0, __ATOMIC_RELAXED);
        return;
    }
    ps3_lock_acquire(&s_coh_reg);
    for (unsigned i = 0; i < s_coh_n; i++)
        if (spu_coh_holder(i) == ctx) __atomic_store_n(spu_coh_slot(i), NULL, __ATOMIC_SEQ_CST);
    __atomic_store_n(&ctx->coh_listed, 0, __ATOMIC_RELAXED);
    ps3_lock_release(&s_coh_reg);
    spu_ll_quiesce();
}

/* Drop every registered context that lies in [lo, hi) -- a host thread's
 * stack, called as that thread exits. A stack-local context still registered
 * then has leaked past its unregister; the next walker would read an unmapped
 * stack (GH3 at the end of a Havok task: spu_coh_notify_write faulting).
 * ponytail: safety net that also names the leak; find the path that skips
 * spu_coh_unregister if it keeps firing. */
void spu_coh_forget_range(uintptr_t lo, uintptr_t hi)
{
    int dropped = 0;
    ps3_lock_acquire(&s_coh_reg);
    for (unsigned i = 0; i < s_coh_n; i++) {
        spu_context* c = spu_coh_holder(i);
        if (!c || (uintptr_t)c < lo || (uintptr_t)c >= hi) continue;
        static int _n = 0;
        if (PS3_LOG_FIRST(_n, 16))
            fprintf(stderr, "[spu-coh] leaked ctx %p (img=%d) dropped at thread exit\n", (void*)c, c->image_id);
        __atomic_store_n(spu_coh_slot(i), NULL, __ATOMIC_SEQ_CST);
        dropped = 1;
    }
    ps3_lock_release(&s_coh_reg);
    if (dropped) spu_ll_quiesce();
}

int spu_coh_is_reserved(uint32_t addr)
{
    return spu_coh_line_reserved(addr);
}

/* A DMA PUT is issued BY an SPU, and hardware does not take that SPU's own
 * reservation away for its own MFC write -- the MFC is part of the same SPE.
 * Our notify had no way to say "everyone but me", so an SPU that reserved a
 * line and then DMA'd a buffer overlapping it killed its own reservation and
 * its next PUTLLC could never succeed. Guitar Hero III's job policy module
 * livelocks exactly there: 16 million atomic ops on one line, every PUTLLC
 * failing for "no reservation", 600k of them self-inflicted.
 *
 * The PUTLLC path already guards this case by dropping its own reservation
 * before notifying; this gives the DMA path the same ability. */
/* ppu_loader.cpp, when linked. The loader is compiled per game, so the host,
 * the fixtures and any build without it have no PPU reservations to break.
 * POSIX: a weak no-op definition the loader's overrides (a weak undefined
 * reference is not enough: ld64 only lets those stay unresolved for dylib
 * symbols). link.exe/lld-link: the linker's own default. */
#if defined(_MSC_VER)
void ppu_resv_break_line(uint32_t ea);
void ppu_resv_break_line_absent(uint32_t ea) { (void)ea; }
#pragma comment(linker, "/alternatename:ppu_resv_break_line=ppu_resv_break_line_absent")
#else
__attribute__((weak)) void ppu_resv_break_line(uint32_t ea) { (void)ea; }
#endif

/* ===========================================================================
 * GETLLAR without the lock-line lock
 *
 * Every SPURS policy module polls its workload lines with GETLLAR, so taking
 * the one global lock for each poll serialized every SPU on a 128-byte copy.
 * A poll is almost always a context that is already in s_coh_ctxs
 * re-reading a line that is already marked, and for that case the lock buys
 * nothing the reservation itself does not:
 *
 *   reader (spu_coh_reserve_fast + caller)    writer (any notifier caller)
 *     resv = line | 1                           store the new line data
 *     seq_cst fence                             spu_coh_writer_fence()
 *     copy the line                             load resv
 *                                               if it is line | 1:
 *                                                 CAS it to line, then LR
 *
 * This is the store-buffering (Dekker) shape: with a seq_cst fence between the
 * store and the load on both sides, the two fences are ordered, so at least
 * one side sees the other. Either the reader's copy already holds the write
 * (an ordinary GETLLAR after the store), or the writer sees the reservation
 * and ends it. A writer that loads a stale resv is one whose fence
 * came first, so the copy already holds its data; at worst it ends the new
 * reservation spuriously.
 *
 * Two preconditions keep every writer of the line on that right-hand side:
 *  - the line is already marked. Marks are never cleared, so every store path
 *    that checks spu_coh_is_reserved takes the notifier for this line.
 *  - ctx is already in s_coh_ctxs (coh_listed). The entry was added under the
 *    lock by an earlier GETLLAR of this same thread, so any writer whose
 *    critical section could overlap this read walks past it.
 * Anything else takes the lock and spu_coh_reserve as before.
 *
 * Writers still hold the lock-line lock, so they stay serialized with each
 * other and with PUTLLC's compare-and-commit. resv is one word, so the writer's
 * compare-and-swap ends exactly the reservation it saw: one the owner has since
 * moved to another line is left alone and gets no event. Everything else about a
 * reservation is still only touched by the owning thread or under the lock.
 * ===========================================================================*/
static inline void spu_coh_writer_fence(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }


int spu_coh_reserve_fast(spu_context* ctx, uint32_t ea, uint32_t* seq)
{
    if (!ctx || !__atomic_load_n(&ctx->coh_listed, __ATOMIC_RELAXED) ||
        !spu_coh_is_reserved(ea))
        return 0;
    /* Retire any older reservation before retargeting it. The fence is what
     * the argument above rests on. */
    __atomic_store_n(&ctx->resv, (ea & ~127u) | 1u, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    *seq = __atomic_load_n(&g_stripes[spu_ll_index(ea)].seq, __ATOMIC_ACQUIRE);
    return !(*seq & 1u);
}

void spu_coh_notify_write_except(uint32_t ea, const void* self)
{
    const uint32_t line = ea & ~127u;
    spu_coh_writer_fence();
    ppu_resv_break_line(ea);
    const unsigned n = __atomic_load_n(&s_coh_n, __ATOMIC_ACQUIRE);
    for (unsigned i = 0; i < SPU_COH_PREFETCH; i++) spu_coh_prefetch_holder(i, n);
    for (unsigned i = 0; i < n; i++) {
        spu_coh_prefetch_holder(i + SPU_COH_PREFETCH, n);
        spu_context* c = spu_coh_holder(i);
        if (!c || (const void*)c == self) continue;
        uint32_t r = line | 1u;
        if (__atomic_load_n(&c->resv, __ATOMIC_RELAXED) == r &&
            __atomic_compare_exchange_n(&c->resv, &r, line, 0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) {
            SPU_EVENT_SET(c, SPU_EVENT_LR);
            g_spu_lr_raise++;
            spu_ll_defer_wake(c);
        }
    }
}
void spu_coh_notify_write(uint32_t ea)
{
    spu_coh_notify_write_except(ea, NULL);
}

#ifdef __cplusplus
}
#endif
