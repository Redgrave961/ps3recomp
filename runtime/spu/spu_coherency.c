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

extern uint8_t* vm_base;

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

#define SPU_COH_LINES       (1u << (32 - SPU_COH_LINE_SHIFT))
#define SPU_COH_BITMAP_SZ   (SPU_COH_LINES / 8)

static unsigned char s_coh_bitmap[SPU_COH_BITMAP_SZ];

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

#if defined(_MSC_VER)
void ppu_resv_break_line(uint32_t ea);
void ppu_resv_break_line_absent(uint32_t ea) { (void)ea; }
#pragma comment(linker, "/alternatename:ppu_resv_break_line=ppu_resv_break_line_absent")
#else
__attribute__((weak)) void ppu_resv_break_line(uint32_t ea) { (void)ea; }
#endif

static inline void spu_coh_writer_fence(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }


int spu_coh_reserve_fast(spu_context* ctx, uint32_t ea, uint32_t* seq)
{
    if (!ctx || !__atomic_load_n(&ctx->coh_listed, __ATOMIC_RELAXED) ||
        !spu_coh_is_reserved(ea))
        return 0;
    __atomic_store_n(&ctx->resv, (ea & ~127u) | 1u, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    *seq = __atomic_load_n(&g_stripes[spu_ll_index(ea)].seq, __ATOMIC_ACQUIRE);
    return !(*seq & 1u);
}

int (*g_spu_idle_wake)(const spu_context* ctx, const uint8_t* line);
uint32_t (*g_spu_idle_slots)(const uint8_t* line);

static uint32_t spu_coh_free_slots(uint32_t line, unsigned n)
{
    const uint32_t s = g_spu_idle_slots(vm_base + line);
    uint32_t k = 0;
    for (unsigned i = 0; i < n; i++) {
        const spu_context* c = spu_coh_holder(i);
        k += c && __atomic_load_n(&c->idle_claim, __ATOMIC_RELAXED) == (line | 1u);
    }
    return s > k ? s - k : 0u;
}

static void spu_coh_scan(uint32_t line, const void* self, int written)
{
    uint32_t slots = ~0u;
    const unsigned n = __atomic_load_n(&s_coh_n, __ATOMIC_ACQUIRE);
    for (unsigned i = 0; i < SPU_COH_PREFETCH; i++) spu_coh_prefetch_holder(i, n);
    for (unsigned i = 0; i < n; i++) {
        spu_coh_prefetch_holder(i + SPU_COH_PREFETCH, n);
        spu_context* c = spu_coh_holder(i);
        if (!c || (const void*)c == self) continue;
        uint32_t r = __atomic_load_n(&c->resv, __ATOMIC_RELAXED);
        if ((r & ~SPU_RESV_BY_CONTENT) != (line | 1u)) continue;
        int need = 2;
        if (r & SPU_RESV_BY_CONTENT) {
            need = g_spu_idle_wake ? g_spu_idle_wake(c, vm_base + line) : 2;
            if (!need) continue;
            if (!memcmp(vm_base + line, c->resv_line, MFC_ATOMIC_LINE)) continue;
            if (need == 1) {
                if (__atomic_load_n(&c->idle_claim, __ATOMIC_RELAXED) == (line | 1u)) continue;
                if (slots == ~0u) slots = spu_coh_free_slots(line, n);
                if (!slots) continue;
                slots--;
            }
        } else if (!written) {
            continue;
        }
        if (__atomic_compare_exchange_n(&c->resv, &r, line, 0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) {
            if (need == 1) __atomic_store_n(&c->idle_claim, line | 1u, __ATOMIC_RELAXED);
            SPU_EVENT_SET(c, SPU_EVENT_LR);
            g_spu_lr_raise++;
            spu_ll_defer_wake(c);
        }
    }
}

void spu_coh_notify_write_except(uint32_t ea, const void* self)
{
    spu_coh_writer_fence();
    ppu_resv_break_line(ea);
    spu_coh_scan(ea & ~127u, self, 1);
}

void spu_coh_recheck_idle(uint32_t ea, const void* self)
{
    spu_coh_scan(ea & ~127u, self, 0);
}

void spu_coh_notify_write(uint32_t ea)
{
    spu_coh_notify_write_except(ea, NULL);
}

#ifdef __cplusplus
}
#endif
