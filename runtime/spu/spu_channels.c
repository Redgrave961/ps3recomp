#include "spu_dma.h"
#include "spu_coherency.h"
#include "spu_helpers.h"
#include "spu_lockstep.h"
#include "spu_interp.h"
#include "ps3emu/ps3_ftrace.h"
#include "spu_overlay_residency.h"
#include "../ps3_log.h"
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#if defined(_WIN32)
#define SPU_SETJMP(env) setjmp(env)
#define SPU_LONGJMP(env, v) longjmp(env, v)
#else
#define SPU_SETJMP(env) _setjmp(env)
#define SPU_LONGJMP(env, v) _longjmp(env, v)
#endif
#ifndef _WIN32
#include <sched.h>
#endif
#include <time.h>
#include "../platform/win32_compat.h"
static inline int spu_line_differs_128(const uint8_t* a, const uint8_t* b);
#include "../platform/win32_backtrace.h"
#include "ps3emu/log_first.h"

#ifdef __cplusplus
extern "C" {
#endif

static uint64_t spu_host_ns(void)
{
#ifdef _WIN32
    static LARGE_INTEGER s_freq;
    LARGE_INTEGER c;
    if (__builtin_expect(!s_freq.QuadPart, 0)) QueryPerformanceFrequency(&s_freq);
    QueryPerformanceCounter(&c);
    if (__builtin_expect(s_freq.QuadPart == 10000000LL, 1))
        return (uint64_t)c.QuadPart * 100ull;
    return (uint64_t)(((unsigned __int128)(uint64_t)c.QuadPart * 1000000000ull) / (uint64_t)s_freq.QuadPart);
#elif defined(__APPLE__)
    return clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

static __thread jmp_buf s_spu_halt_env;
static __thread int     s_spu_halt_armed = 0;

void spu_jmp_no_unwind(void* env)
{
#if defined(_WIN64)
    ((_JUMP_BUFFER*)env)->Frame = 0;
#else
    (void)env;
#endif
}

void spu_restart_dispatch(spu_context* ctx)
{
    g_spu_trampoline_fn = 0;
    if (s_spu_halt_armed) SPU_LONGJMP(s_spu_halt_env, 2);
    fprintf(stderr, "[spu] non-local return outside execution driver at 0x%05X\n", ctx->pc);
    ctx->status = SPU_STATUS_STOPPED_BY_HALT;
}

int (*g_spu_user_event_hook)(spu_context*, uint32_t) = NULL;
void (*g_spu_out_mbox_hook)(uint32_t group_id, uint32_t spu_id,
                            int is_intr, uint32_t value) = 0;

void (*g_rsx_window_write)(void) = 0;


void spu_halt(spu_context* ctx)
{
    { static int _n = 0;
      if (_n < 6 && ctx->status == SPU_STATUS_STOPPED_BY_HALT) { _n++;
        fprintf(stderr, "[spu-halt] img=%d pc=0x%05X regs:", ctx->image_id,
                ctx->pc & SPU_LS_MASK);
        for (int r = 3; r <= 16; r++)
            fprintf(stderr, " r%d=%08X", r, ctx->gpr[r]._u32[0]);
        fprintf(stderr, " r80=%08X r85=%08X\n",
                ctx->gpr[80]._u32[0], ctx->gpr[85]._u32[0]);
        fflush(stderr);
      } }
    if (s_spu_halt_armed) { s_spu_halt_armed = 0; SPU_LONGJMP(s_spu_halt_env, 1); }
}

void spu_stop_exit_check(spu_context* ctx)
{
    if (ctx->status == SPU_STATUS_STOPPED_BY_STOP &&
        (ctx->stop_code == 0x101u || ctx->stop_code == 0x102u) && s_spu_halt_armed) {
        s_spu_halt_armed = 0;
        SPU_LONGJMP(s_spu_halt_env, 1);
    }
}

void spu_stop(spu_context* ctx)
{
    ctx->status = SPU_STATUS_STOPPED_BY_STOP;
    spu_stop_exit_check(ctx);
}

static uint64_t spu_float_mode_enter(void)
{
#if defined(__aarch64__) && defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS)
    static int s_on = -1;
    int on = __atomic_load_n(&s_on, __ATOMIC_RELAXED);
    if (on < 0) {
        const char* e = getenv("SPU_FLOAT_RZ");
        on = e && e[0] == '1' && !e[1];
        __atomic_store_n(&s_on, on, __ATOMIC_RELAXED);
    }
    if (!on) return ~0ull;
    uint64_t v;
    __asm__ volatile("mrs %0, fpcr" : "=r"(v));
    if (v == SPU_FPCR_RZFZ) return ~0ull;
    __asm__ volatile("msr fpcr, %0" :: "r"((uint64_t)SPU_FPCR_RZFZ));
    return v;
#elif defined(SPU_MXCSR_RZ)
    const uint32_t v = _mm_getcsr();
    if ((v & ~0x3Fu) == SPU_MXCSR_RZ) return ~0ull;
    _mm_setcsr(SPU_MXCSR_RZ);
    return v;
#else
    return ~0ull;
#endif
}
static void spu_float_mode_leave(uint64_t outer)
{
#if defined(__aarch64__) && defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS)
    if (outer != ~0ull) __asm__ volatile("msr fpcr, %0" :: "r"(outer));
#elif defined(SPU_MXCSR_RZ)
    if (outer != ~0ull) _mm_setcsr((uint32_t)outer);
#else
    (void)outer;
#endif
}

static void spu_honour_stop_request(spu_context* ctx)
{
    if (!ctx->stop_request) return;
    ctx->status = SPU_STATUS_STOPPED;
    spu_halt(ctx);
}

int spu_run_with_halt(void (*entry)(spu_context*), spu_context* ctx)
{
    const uint64_t fpcr_outer = spu_float_mode_enter();
    int halted = 0;
    { extern void spu_irq_regs_forget(spu_context*); spu_irq_regs_forget(ctx); }
    ctx->steps = 0;
    if (g_spu_direct < 0) spu_direct_init();
    ctx->host_depth = 0;
    ctx->irq_frame = 0;
    ctx->drain_ret_pc = 0;
    jmp_buf outer_env; const int outer_armed = s_spu_halt_armed;
    if (outer_armed) memcpy(outer_env, s_spu_halt_env, sizeof(jmp_buf));
    s_spu_halt_armed = 1;
    g_spu_trampoline_fn = 0;
    if (__builtin_expect(g_yz_lockstep_on != 0, 0)) yz_lockstep_register(ctx);
    switch (SPU_SETJMP(s_spu_halt_env)) {
    case 1:
        halted = 1;
        g_spu_trampoline_fn = 0;
        ctx->irq_frame = 0;
        break;
    case 2:
        ctx->host_depth = 0;
        ctx->irq_frame = 0;
        g_spu_trampoline_fn = 0;
        spu_indirect_branch(ctx);
        SPU_DRAIN(ctx);
        break;
    default:
        spu_jmp_no_unwind(s_spu_halt_env);
        entry(ctx);
        SPU_DRAIN(ctx);
    }
    if (__builtin_expect(g_yz_lockstep_on != 0, 0)) yz_lockstep_unregister(ctx);
    s_spu_halt_armed = outer_armed;
    if (outer_armed) memcpy(s_spu_halt_env, outer_env, sizeof(jmp_buf));
    spu_float_mode_leave(fpcr_outer);
    return halted;
}


typedef struct spu_mfc_slot {
    spu_context* ctx;
    mfc_engine mfc;
    struct spu_mfc_slot* next;
} spu_mfc_slot;

static spu_mfc_slot* s_mfc_slots;
static SRWLOCK s_mfc_claim_lock = SRWLOCK_INIT;

static DWORD WINAPI spu_pcsample_thread(LPVOID p)
{
    const DWORD ms = (DWORD)(uintptr_t)p;
    for (;;) {
        Sleep(ms);
        int i = 0;
        AcquireSRWLockShared(&s_mfc_claim_lock);
        for (spu_mfc_slot* slot = s_mfc_slots; slot; slot = slot->next, i++) {
            spu_context* c = slot->ctx;
            if (c) fprintf(stderr, "[pcsample] slot=%d img=%d spu=0x%X pc=0x%05X lr=0x%05X"
                                   " r3=0x%08X r81=0x%08X r84=0x%X r13=0x%08X r14=0x%08X\n",
                           i, c->image_id, c->spu_id, (unsigned)(c->pc & SPU_LS_MASK),
                           (unsigned)(c->gpr[0]._u32[0] & SPU_LS_MASK), c->gpr[3]._u32[0],
                           c->gpr[81]._u32[0], c->gpr[84]._u32[0], c->gpr[13]._u32[0], c->gpr[14]._u32[0]);
        }
        ReleaseSRWLockShared(&s_mfc_claim_lock);
        fflush(stderr);
    }
}

static __attribute__((noinline, cold)) mfc_engine* mfc_claim(spu_context* ctx)
{
    { static volatile LONG checked = 0;
      if (!checked && !InterlockedExchange(&checked, 1)) {
          const char* e = getenv("SPU_PCSAMPLE");
          if (e) CreateThread(NULL, 0, spu_pcsample_thread, (LPVOID)(uintptr_t)(atoi(e) * 1000), 0, NULL);
      } }
    AcquireSRWLockShared(&s_mfc_claim_lock);
    for (spu_mfc_slot* slot = s_mfc_slots; slot; slot = slot->next) {
        if (slot->ctx == ctx) {
            ReleaseSRWLockShared(&s_mfc_claim_lock);
            return &slot->mfc;
        }
    }
    ReleaseSRWLockShared(&s_mfc_claim_lock);

    AcquireSRWLockExclusive(&s_mfc_claim_lock);
    for (spu_mfc_slot* slot = s_mfc_slots; slot; slot = slot->next) {
        if (slot->ctx == ctx) {
            ReleaseSRWLockExclusive(&s_mfc_claim_lock);
            return &slot->mfc;
        }
    }
    spu_mfc_slot* slot = (spu_mfc_slot*)malloc(sizeof(*slot));
    if (!slot) {
        fprintf(stderr, "[SPU] cannot allocate private MFC engine for context %p\n", (void*)ctx);
        fflush(stderr);
        abort();
    }
    slot->ctx = ctx;
    mfc_engine_init(&slot->mfc);
    slot->next = s_mfc_slots;
    s_mfc_slots = slot;
    ReleaseSRWLockExclusive(&s_mfc_claim_lock);
    return &slot->mfc;
}

static inline mfc_engine* mfc_for(spu_context* ctx)
{
    return ctx->mfc ? ctx->mfc : (ctx->mfc = mfc_claim(ctx));
}

void spu_mfc_release(spu_context* ctx)
{
    if (!ctx) return;
    ctx->mfc = NULL;
    AcquireSRWLockExclusive(&s_mfc_claim_lock);
    for (spu_mfc_slot** link = &s_mfc_slots; *link; link = &(*link)->next) {
        if ((*link)->ctx != ctx) continue;
        spu_mfc_slot* slot = *link;
        *link = slot->next;
        free(slot);
        break;
    }
    ReleaseSRWLockExclusive(&s_mfc_claim_lock);
}

void spu_mfc_forget_range(uintptr_t lo, uintptr_t hi)
{
    AcquireSRWLockExclusive(&s_mfc_claim_lock);
    for (spu_mfc_slot** link = &s_mfc_slots; *link;) {
        spu_mfc_slot* slot = *link;
        uintptr_t owner = (uintptr_t)slot->ctx;
        if (owner >= lo && owner < hi) {
            *link = slot->next;
            free(slot);
        } else {
            link = &slot->next;
        }
    }
    ReleaseSRWLockExclusive(&s_mfc_claim_lock);
}

extern uint8_t* vm_base;

#define SPU_JM2_KERNEL_BASE 0x15000u

static int spu_mfc_atomic(spu_context* ctx, uint32_t cmd)
{
    if (cmd != MFC_GETLLAR_CMD && cmd != MFC_PUTLLC_CMD &&
        cmd != MFC_PUTLLUC_CMD && cmd != MFC_PUTQLLUC_CMD)
        return 0;

    uint32_t ea  = ctx->mfc_eal & ~(uint32_t)(MFC_ATOMIC_LINE - 1);
    uint32_t lsa = ctx->mfc_lsa & SPU_LS_MASK & ~(uint32_t)(MFC_ATOMIC_LINE - 1);
    uint8_t* ls  = &ctx->ls[lsa];
    uint8_t* mem = vm_base + ea;


    if (!mfc_ea_valid(ea, MFC_ATOMIC_LINE)) mfc_fault(ctx, "atomic address", cmd, 0, ea, MFC_ATOMIC_LINE);

    switch (cmd) {
    case MFC_GETLLAR_CMD:
        if (__builtin_expect(g_yz_lockstep_on != 0, 0)) yz_lockstep_tick(ctx);
        uint32_t llar_seq;
        int llar_fast = spu_coh_reserve_fast(ctx, ea, &llar_seq);
        if (llar_fast) {
            memcpy(ls, mem, MFC_ATOMIC_LINE);
            llar_fast = spu_lockline_unchanged(ea, llar_seq);
        }
        if (!llar_fast) {
            spu_lockline_lock(ea);
            spu_coh_reserve(ctx, ea);
            memcpy(ls, mem, MFC_ATOMIC_LINE);
        }
        memcpy(ctx->resv_line, ls, MFC_ATOMIC_LINE);
        if (!llar_fast) __atomic_store_n(&ctx->resv, (ea & ~127u) | 1u, __ATOMIC_RELAXED);
        ctx->atomic_stat = 4;
        if (!llar_fast) spu_lockline_unlock(ea);
        return 1;

    case MFC_PUTLLC_CMD:
        if ((__atomic_load_n(&ctx->resv, __ATOMIC_RELAXED) & ~SPU_RESV_BY_CONTENT) != ((ea & ~127u) | 1u)) {
            __atomic_store_n(&ctx->resv, 0u, __ATOMIC_RELAXED);
            ctx->atomic_stat = 1;
            return 1;
        }
        spu_lockline_lock(ea);
        if ((__atomic_load_n(&ctx->resv, __ATOMIC_RELAXED) & ~SPU_RESV_BY_CONTENT) == ((ea & ~127u) | 1u) &&
            !spu_line_differs_128(mem, ctx->resv_line)) {
            memcpy(mem, ls, MFC_ATOMIC_LINE);
            __atomic_store_n(&ctx->resv, 0u, __ATOMIC_RELAXED);
            spu_coh_notify_write_except(ea, ctx);
            ctx->atomic_stat = 0;
        } else {
            __atomic_store_n(&ctx->resv, 0u, __ATOMIC_RELAXED);
            ctx->atomic_stat = 1;
        }
        spu_lockline_unlock(ea);
        if (ctx->atomic_stat == 0) {
            spu_rsx_window_check(ea, MFC_ATOMIC_LINE);
        }
        return 1;

    case MFC_PUTLLUC_CMD:
    case MFC_PUTQLLUC_CMD:
        spu_lockline_lock(ea);
        memcpy(mem, ls, MFC_ATOMIC_LINE);
        __atomic_store_n(&ctx->resv, 0u, __ATOMIC_RELAXED); ctx->atomic_stat = 2;
        spu_coh_notify_write_except(ea, ctx);
        spu_lockline_unlock(ea);
        spu_rsx_window_check(ea, MFC_ATOMIC_LINE);
        return 1;

    default:
        return 0;
    }
}

static int channel_is_mfc(uint32_t ch)
{
    switch (ch) {
    case MFC_WrMSSyncReq: case MFC_RdTagMask:  case MFC_LSA:
    case MFC_EAH:         case MFC_EAL:         case MFC_Size:
    case MFC_TagID:       case MFC_Cmd:         case MFC_WrTagMask:
    case MFC_WrTagUpdate: case MFC_RdTagStat:   case MFC_RdListStallStat:
    case MFC_WrListStallAck: case MFC_RdAtomicStat:
        return 1;
    default:
        return 0;
    }
}

static void spu_ch_park(spu_context* ctx, int (*ready)(void*), void* arg)
{
    uint32_t seq = __atomic_load_n(&ctx->ch_wake_seq, __ATOMIC_ACQUIRE);
    __atomic_store_n(&ctx->ch_sleeping, 2, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (ready(arg)) {
        __atomic_store_n(&ctx->ch_sleeping, 0, __ATOMIC_RELAXED);
        return;
    }
    for (int spin = 0; spin < 8; spin++) {
        const uint32_t cur = __atomic_load_n(&ctx->ch_wake_seq, __ATOMIC_ACQUIRE);
        if (cur != seq) {
            seq = cur;
            if (ready(arg)) {
                __atomic_store_n(&ctx->ch_sleeping, 0, __ATOMIC_RELAXED);
                return;
            }
        }
        YieldProcessor();
    }
    for (;;) {
        seq = __atomic_load_n(&ctx->ch_wake_seq, __ATOMIC_ACQUIRE);
        __atomic_store_n(&ctx->ch_sleeping, 1, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (ready(arg)) break;
        ps3_futex_wait(&ctx->ch_wake_seq, seq, 0);
    }
    __atomic_store_n(&ctx->ch_sleeping, 0, __ATOMIC_RELAXED);
}

static int spu_out_mbox_free(void* p)
{
    const spu_context* ctx = (const spu_context*)p;
    return ctx->ch_out_mbox.count < SPU_MBOX_DEPTH || ctx->stop_request;
}

void spu_wrch(spu_context* ctx, uint32_t channel, u128 value)
{
    uint32_t v = value._u32[0];

    switch (channel) {
    case MFC_LSA:       ctx->mfc_lsa = v;         return;
    case MFC_EAH:       ctx->mfc_eah = v;         return;
    case MFC_EAL:       ctx->mfc_eal = v;         return;
    case MFC_Size:      ctx->mfc_size = v;        return;
    case MFC_TagID:     ctx->mfc_tag = v & 0x1F;  return;
    case MFC_WrTagMask: ctx->mfc_tag_mask = v;    return;
    case MFC_Cmd:
        if (__builtin_expect((v & 0xFFu) >= 0xB0u, 0) && spu_mfc_atomic(ctx, v)) return;
        mfc_submit(mfc_for(ctx), ctx, v);
        return;
    case MFC_WrTagUpdate:
        ctx->mfc_tag_status = mfc_for(ctx)->tag_completed & ctx->mfc_tag_mask;
        if (v != 0 && (ctx->event_mask & 0x1u)) {
            SPU_EVENT_SET(ctx, 0x1u);
            spu_ch_wake(ctx);
        }
        return;
    default: break;
    }

    if (channel_is_mfc(channel)) {
        mfc_channel_write(mfc_for(ctx), ctx, channel, v);
        return;
    }

    switch (channel) {
    case SPU_WrOutMbox:
        if (ctx->ls != ctx->ls_store)
            while (ctx->ch_out_mbox.count >= SPU_MBOX_DEPTH && ctx->status != SPU_STATUS_STOPPED) {
                spu_honour_stop_request(ctx);
                spu_ch_park(ctx, spu_out_mbox_free, ctx);
            }
        spu_channel_write(&ctx->ch_out_mbox, v);
        break;
    case SPU_WrOutIntrMbox:
        if (g_spu_user_event_hook && g_spu_user_event_hook(ctx, v)) break;
        spu_channel_write(&ctx->ch_out_intr_mbox, v);
        if (g_spu_out_mbox_hook) g_spu_out_mbox_hook(ctx->spu_group_id, ctx->spu_id, 1, v);
        break;
    case SPU_WrDec:          ctx->decrementer = v;
                             ctx->dec_base_ns = spu_host_ns();              break;
    case SPU_WrEventMask:    ctx->event_mask = v;                           break;
    case SPU_WrEventAck:
        SPU_EVENT_CLEAR(ctx, v);
        if ((v & 0x2u) && ctx->list_stall_stat)
            SPU_EVENT_SET(ctx, 0x2u);
        break;
    case SPU_WrSRR0:         ctx->srr0 = v & 0x3FFFCu; break;
    default:
        break;
    }
}

static inline int spu_line_differs_128(const uint8_t* a, const uint8_t* b)
{
#ifdef __AVX2__
    const __m256i d0 = _mm256_xor_si256(_mm256_loadu_si256((const __m256i*)(a + 0)), _mm256_loadu_si256((const __m256i*)(b + 0)));
    const __m256i d1 = _mm256_xor_si256(_mm256_loadu_si256((const __m256i*)(a + 32)), _mm256_loadu_si256((const __m256i*)(b + 32)));
    const __m256i d2 = _mm256_xor_si256(_mm256_loadu_si256((const __m256i*)(a + 64)), _mm256_loadu_si256((const __m256i*)(b + 64)));
    const __m256i d3 = _mm256_xor_si256(_mm256_loadu_si256((const __m256i*)(a + 96)), _mm256_loadu_si256((const __m256i*)(b + 96)));
    const __m256i any = _mm256_or_si256(_mm256_or_si256(d0, d1), _mm256_or_si256(d2, d3));
    return !_mm256_testz_si256(any, any);
#else
    return memcmp(a, b, MFC_ATOMIC_LINE) != 0;
#endif
}

static void spu_resv_lost_poll(spu_context* ctx)
{
    uint32_t r = __atomic_load_n(&ctx->resv, __ATOMIC_RELAXED);
    if (!(ctx->event_mask & 0x400u) || !(r & 1u)) return;
    if (!vm_base || !(r & ~127u)) return;
    if (spu_line_differs_128(vm_base + (r & ~127u), ctx->resv_line) &&
        __atomic_compare_exchange_n(&ctx->resv, &r, r & ~1u, 0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED))
        SPU_EVENT_SET(ctx, 0x400u);
}

static int spu_ch_ready(spu_context* ctx, uint32_t channel)
{
    if (channel == SPU_RdEventStat) spu_resv_lost_poll(ctx);
    switch (channel) {
    case SPU_RdInMbox:      return ctx->rcv_evt_n != 0 || ctx->ch_in_mbox.count != 0;
    case SPU_RdSigNotify1:  return ctx->ch_sig_notify[0].count != 0;
    case SPU_RdSigNotify2:  return ctx->ch_sig_notify[1].count != 0;
    case SPU_RdEventStat:   return (ctx->event_status & ctx->event_mask) != 0;
    default:                return 1;
    }
}

typedef struct { spu_context* ctx; uint32_t channel; } spu_ch_wait_arg;

static int spu_ch_wait_ready(void* p)
{
    const spu_ch_wait_arg* a = (const spu_ch_wait_arg*)p;
    return spu_ch_ready(a->ctx, a->channel) || a->ctx->stop_request;
}

void spu_ch_wake(spu_context* ctx)
{
    if (!ctx) return;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    const int sl = __atomic_load_n(&ctx->ch_sleeping, __ATOMIC_RELAXED);
    if (!sl) return;
    __atomic_add_fetch(&ctx->ch_wake_seq, 1u, __ATOMIC_RELEASE);
    if (sl == 1) ps3_futex_wake_all(&ctx->ch_wake_seq);
}

static __thread uint64_t t_ft_busy0;

static void spu_ft_wait_span(spu_context* ctx, uint32_t channel, uint64_t t0)
{
    const uint64_t t1 = ps3_ft_now();
    if (t1 - t0 < ps3_ft_min_ns()) return;
    if (t_ft_busy0 && t0 > t_ft_busy0) {
        const uint8_t* w = ctx->ls + 0x1DC;
        ps3_ft_span(PS3_FT_SPU_BUSY, ctx->spu_id, (uint32_t)ctx->image_id,
                    ((uint32_t)w[0] << 24) | ((uint32_t)w[1] << 16) | ((uint32_t)w[2] << 8) | w[3],
                    t_ft_busy0, t0);
    }
    ps3_ft_span(PS3_FT_SPU_WAIT, ctx->spu_id, channel, ctx->pc & SPU_LS_MASK, t0, t1);
    t_ft_busy0 = t1;
}

static void spu_ch_wait(spu_context* ctx, uint32_t channel, const char* op)
{
    if (spu_ch_ready(ctx, channel)) return;
    const uint64_t ft_t0 = PS3_FT_ON() ? ps3_ft_now() : 0;

    if (ps3_log_verbose())
    { static unsigned long bn = 0; unsigned long n = ++bn;
      if (n <= 50 || (n % 512) == 0)
        fprintf(stderr, "[ch-block] spu=%X pc=0x%05X op=%s ch=%u evstat=0x%X evmask=0x%X\n",
                ctx->spu_id, ctx->pc & SPU_LS_MASK, op, channel,
                ctx->event_status, ctx->event_mask); }

    ctx->status = SPU_STATUS_WAITING_CHANNEL;
    if (__builtin_expect(g_yz_lockstep_on != 0, 0)) yz_lockstep_block_begin(ctx);
    spu_ch_wait_arg a = { ctx, channel };
    while (!spu_ch_ready(ctx, channel)) {
        if (ctx->stop_request) {
            if (__builtin_expect(g_yz_lockstep_on != 0, 0)) yz_lockstep_block_end(ctx);
            spu_honour_stop_request(ctx);
        }
        spu_ch_park(ctx, spu_ch_wait_ready, &a);
    }
    if (__builtin_expect(g_yz_lockstep_on != 0, 0)) yz_lockstep_block_end(ctx);
    if (ft_t0) spu_ft_wait_span(ctx, channel, ft_t0);
    ctx->status = SPU_STATUS_RUNNING;
}

u128 spu_rdch(spu_context* ctx, uint32_t channel)
{
    spu_honour_stop_request(ctx);
    if (channel == MFC_RdTagStat) return spu_make_preferred_u32(ctx->mfc_tag_status);
    if (channel == MFC_RdAtomicStat) return spu_make_preferred_u32(ctx->atomic_stat);
    if (channel == SPU_RdInMbox || channel == SPU_RdSigNotify1 || channel == SPU_RdSigNotify2)
        spu_ch_wait(ctx, channel, "rdch");
    else if (channel == SPU_RdEventStat && ctx->event_mask != 0)
        spu_ch_wait(ctx, channel, "rdch");

    uint32_t v = 0;

    if (channel_is_mfc(channel)) {
        v = mfc_channel_read(ctx, channel);
        return spu_make_preferred_u32(v);
    }

    switch (channel) {
    case SPU_RdInMbox:
        if (ctx->rcv_evt_n > 0) { v = ctx->rcv_evt[ctx->rcv_evt_i++]; ctx->rcv_evt_n--; }
        else v = spu_channel_read(&ctx->ch_in_mbox);
        break;
    case SPU_RdSigNotify1:  v = spu_channel_read(&ctx->ch_sig_notify[0]); break;
    case SPU_RdSigNotify2:  v = spu_channel_read(&ctx->ch_sig_notify[1]); break;
    case SPU_RdDec: {
        const uint64_t now = spu_host_ns();
        if (__builtin_expect(!ctx->dec_base_ns, 0)) {
            ctx->dec_base_ns = now;
            v = ctx->decrementer;
        } else {
            const uint64_t elapsed = now - ctx->dec_base_ns;
            const uint32_t ticks = (uint32_t)((elapsed / 5000ull) * 399ull + ((elapsed % 5000ull) * 399ull) / 5000ull);
            v = ctx->decrementer - ticks;
        }
        break;
    }
    case SPU_RdEventMask:   v = ctx->event_mask;                        break;
    case SPU_RdEventStat:   spu_resv_lost_poll(ctx); v = ctx->event_status & ctx->event_mask; break;
    case SPU_RdMachStat:    v = ctx->int_enable & 1u; break;
    case SPU_RdSRR0:        v = ctx->srr0;                              break;
    default:
        v = 0;
        break;
    }
    return spu_make_preferred_u32(v);
}

uint32_t spu_rchcnt(spu_context* ctx, uint32_t channel)
{
    spu_honour_stop_request(ctx);
    switch (channel) {
    case SPU_RdInMbox:
        if (ctx->park_on_empty_inmbox && ctx->rcv_evt_n == 0 &&
            ctx->ch_in_mbox.count == 0) {
            extern void spu_halt(spu_context*);
            spu_halt(ctx);
        }
        return (uint32_t)ctx->rcv_evt_n + ctx->ch_in_mbox.count;
    case SPU_WrOutMbox:
        return ctx->ch_out_mbox.count >= SPU_MBOX_DEPTH
             ? 0u : (uint32_t)(SPU_MBOX_DEPTH - ctx->ch_out_mbox.count);
    case SPU_WrOutIntrMbox:
        return ctx->ch_out_intr_mbox.count >= SPU_INTR_MBOX_DEPTH
             ? 0u : (uint32_t)(SPU_INTR_MBOX_DEPTH - ctx->ch_out_intr_mbox.count);
    case SPU_RdSigNotify1:   return ctx->ch_sig_notify[0].count;
    case SPU_RdSigNotify2:   return ctx->ch_sig_notify[1].count;
    case MFC_Cmd:            return MFC_QUEUE_DEPTH - mfc_for(ctx)->queue_count;
    case SPU_RdEventStat:
        spu_resv_lost_poll(ctx);
        return (ctx->event_status & ctx->event_mask) != 0;
    case MFC_RdTagStat:      return 1;
    case MFC_RdListStallStat: return ctx->list_stall_stat ? 1 : 0;
    default:                 return 1;
    }
}

typedef void (*spu_fn)(spu_context*);

typedef struct {
    uint32_t addr;
    spu_fn   fn;
    int      image_id;
} spu_reg_entry;

#define SPU_FN_REGISTRY_MAX 262144
static spu_reg_entry s_registry[SPU_FN_REGISTRY_MAX];
static uint32_t s_registry_count = 0;

#define SPU_FN_HASH_SIZE 131072
static uint32_t s_hash_head[SPU_FN_HASH_SIZE];
static uint32_t s_hash_tail[SPU_FN_HASH_SIZE];
static uint32_t s_hash_next[SPU_FN_REGISTRY_MAX];

static inline uint32_t spu_fn_hash(uint32_t addr)
{
    return ((addr >> 2) * 2654435761u) & (SPU_FN_HASH_SIZE - 1);
}

static int s_reg_image = 0;
void spu_begin_image(int image_id) { s_reg_image = image_id; }

void spu_register_function(uint32_t addr, spu_fn fn)
{
    if (s_registry_count >= SPU_FN_REGISTRY_MAX) {
        static int warned = 0;
        if (!warned) {
            warned = 1;
            fprintf(stderr,
                    "[spu] FATAL: SPU function registry full at %u entries "
                    "(SPU_FN_REGISTRY_MAX=%u) -- image %d function 0x%05X and all "
                    "later registrations DROPPED. Raise SPU_FN_REGISTRY_MAX.\n",
                    s_registry_count, (unsigned)SPU_FN_REGISTRY_MAX,
                    s_reg_image, addr);
            fflush(stderr);
        }
        return;
    }
    uint32_t i = s_registry_count;
    s_registry[i].addr = addr;
    s_registry[i].fn = fn;
    s_registry[i].image_id = s_reg_image;
    uint32_t h = spu_fn_hash(addr);
    s_hash_next[i] = 0;
    if (s_hash_head[h] == 0)
        s_hash_head[h] = i + 1;
    else
        s_hash_next[s_hash_tail[h] - 1] = i + 1;
    s_hash_tail[h] = i + 1;
    __atomic_store_n(&s_registry_count, i + 1, __ATOMIC_RELEASE);
}

static void spu_report_cross_image(uint32_t addr, int want, int got)
{
    enum { SEEN_MAX = 64 };
    static struct { uint32_t addr; int want, got; } seen[SEEN_MAX];
    static unsigned n_seen = 0;
    for (unsigned i = 0; i < n_seen; i++)
        if (seen[i].addr == addr && seen[i].want == want && seen[i].got == got)
            return;
    if (n_seen < SEEN_MAX) {
        seen[n_seen].addr = addr; seen[n_seen].want = want; seen[n_seen].got = got;
        n_seen++;
    }
    fprintf(stderr,
            "[spu] CROSS-IMAGE dispatch: LS 0x%05X requested by image %d, served "
            "by image %d's function (id-0 wildcard). If this job was loaded at a "
            "runtime-chosen base, it may be running the WRONG program -- see "
            "issue #59.\n", addr, want, got);
}

spu_fn spu_lookup(uint32_t addr, int image_id)
{
    for (uint32_t n = s_hash_head[spu_fn_hash(addr)]; n; n = s_hash_next[n - 1]) {
        const spu_reg_entry* e = &s_registry[n - 1];
        if (e->addr == addr &&
            (image_id == 0 || e->image_id == 0 || e->image_id == image_id)) {
            if (e->image_id != image_id)
                spu_report_cross_image(addr, image_id, e->image_id);
            return e->fn;
        }
    }
    return NULL;
}

typedef struct { uint32_t addr, gen; int image; spu_fn fn; } spu_lookup_memo;
#define SPU_LOOKUP_MEMO_SIZE 512
static __thread spu_lookup_memo t_lookup_memo[SPU_LOOKUP_MEMO_SIZE];
static spu_fn spu_lookup_memoized(uint32_t addr, int image_id)
{
    spu_lookup_memo* m = &t_lookup_memo[((addr >> 2) ^ ((uint32_t)image_id * 0x9E3779B1u)) &
                                        (SPU_LOOKUP_MEMO_SIZE - 1)];
    const uint32_t gen = s_registry_count + 1;
    if (m->gen != gen || m->addr != addr || m->image != image_id) {
        m->fn = spu_lookup(addr, image_id);
        m->addr = addr; m->image = image_id; m->gen = gen;
    }
    return m->fn;
}

int spu_image_of_function(uint32_t addr)
{
    for (uint32_t n = s_hash_head[spu_fn_hash(addr)]; n; n = s_hash_next[n - 1]) {
        const spu_reg_entry* e = &s_registry[n - 1];
        if (e->addr == addr) return e->image_id;
    }
    return -1;
}

int spu_have_function(uint32_t addr)
{
    return spu_image_of_function(addr) >= 0;
}

static int spu_overlay_resident_verified(spu_context* ctx, unsigned slot);
static int spu_overlay_resolve_at_lazy(spu_context* ctx, uint32_t lsa);
static spu_lifted_fn spu_lifted_lookup_full(const spu_context* ctx, uint32_t lsa, int* reloc)
{
    if (!ctx) return (spu_lifted_fn)spu_lookup_memoized(lsa, 0);
    if (ctx->resident_code_mask && lsa >= ctx->resident_code_lo && lsa < ctx->resident_code_hi)
        for (uint32_t m = ctx->resident_code_mask; m; m &= m - 1u) {
            const unsigned slot = (unsigned)__builtin_ctz(m);
            if (lsa >= ctx->resident_code[slot].lsa &&
                lsa - ctx->resident_code[slot].lsa < ctx->resident_code[slot].size) {
                if (ctx->resident_code[slot].verify_state &&
                    !spu_overlay_resident_verified((spu_context*)ctx, slot)) return NULL;
                const uint32_t canon = ctx->resident_code[slot].canon;
                if (!canon)
                    return (spu_lifted_fn)spu_lookup_memoized(lsa, ctx->resident_code[slot].image_id);
                spu_fn f = spu_lookup_memoized(lsa - ctx->resident_code[slot].lsa + canon,
                                               ctx->resident_code[slot].image_id);
                if (f) {
                    ((spu_context*)ctx)->reloc_delta = (int32_t)(ctx->resident_code[slot].lsa - canon);
                    if (reloc) *reloc = 1;
                }
                return (spu_lifted_fn)f;
            }
        }
    const int lazy_img = spu_overlay_resolve_at_lazy((spu_context*)ctx, lsa);
    if (lazy_img > 0)
        return (spu_lifted_fn)spu_lookup_memoized(lsa, lazy_img);
    spu_fn fn = NULL;
    if (ctx->resident_ovl) fn = spu_lookup_memoized(lsa, ctx->resident_ovl);
    if (!fn) fn = spu_lookup_memoized(lsa, ctx->image_id);
    return (spu_lifted_fn)fn;
}

typedef struct {
    spu_lifted_fn fn;
    uint32_t id, lsa, rgen, residency;
    int image, ovl;
    int reloc;
    int32_t reloc_delta;
} spu_dcache_ent;
#define SPU_DCACHE_ENTRIES 512u
static __thread spu_dcache_ent t_dcache[SPU_DCACHE_ENTRIES];
static uint32_t s_dcache_next_id;
static void spu_dcache_residency_changed(spu_context* ctx)
{
    ctx->dcache_residency++;
    uint32_t mask = 0, lo = SPU_LS_SIZE, hi = 0;
    for (unsigned s = 0; s < SPU_RESIDENT_CODE_SLOTS; ++s) {
        if (ctx->resident_code[s].image_id) {
            mask |= 1u << s;
            const uint32_t b = ctx->resident_code[s].lsa;
            const uint32_t e = b + ctx->resident_code[s].size;
            if (b < lo) lo = b;
            if (e > hi) hi = e;
        }
    }
    ctx->resident_code_mask = mask;
    ctx->resident_code_lo = mask ? lo : 0u;
    ctx->resident_code_hi = hi;
}
spu_lifted_fn spu_lifted_lookup(const spu_context* ctx, uint32_t lsa)
{
    if (!ctx) return spu_lifted_lookup_full(ctx, lsa, NULL);
    spu_context* c = (spu_context*)ctx;
    uint32_t id = c->dcache_id;
    if (!id) {
        do id = __atomic_add_fetch(&s_dcache_next_id, 1, __ATOMIC_RELAXED); while (!id);
        c->dcache_id = id;
    }
    const uint32_t rgen = __atomic_load_n(&s_registry_count, __ATOMIC_ACQUIRE);
    spu_dcache_ent* e = &t_dcache[((lsa >> 2) ^ (id * 0x9E3779B1u)) & (SPU_DCACHE_ENTRIES - 1)];
    if (e->id == id && e->lsa == lsa && e->rgen == rgen &&
        e->residency == c->dcache_residency && e->image == c->image_id &&
        e->ovl == c->resident_ovl) {
        if (e->reloc) c->reloc_delta = e->reloc_delta;
        return e->fn;
    }
    int reloc = 0;
    const spu_lifted_fn fn = spu_lifted_lookup_full(ctx, lsa, &reloc);
    e->fn = fn; e->id = id; e->lsa = lsa; e->rgen = rgen;
    e->residency = c->dcache_residency; e->image = c->image_id;
    e->ovl = c->resident_ovl;
    e->reloc = reloc; e->reloc_delta = c->reloc_delta;
    return fn;
}

typedef struct { uint32_t src_ea; int image_id; uint8_t sig[16]; int has_sig; uint32_t span;
                 uint32_t load_lsa; unsigned code_count; spu_overlay_code_segment* code;
                 uint32_t canon; uint32_t verified_ea; } spu_ovl_src;
#define SPU_OVL_SRC_MAX 128
static spu_ovl_src s_ovl_src[SPU_OVL_SRC_MAX];
static int s_ovl_src_count = 0;
static atomic_int s_ovl_has_sig;

#define SPU_OVL_EA_SET 512u
static uint32_t s_ovl_ea_set[SPU_OVL_EA_SET];
static uint8_t s_ovl_ea_used[SPU_OVL_EA_SET];
static int s_ovl_ea_count = 0;
static unsigned spu_ovl_ea_slot(uint32_t ea) { return (ea * 0x9E3779B1u) >> 23; }
static void spu_ovl_ea_add(uint32_t ea)
{
    for (unsigned i = spu_ovl_ea_slot(ea), n = 0; n < SPU_OVL_EA_SET; ++n, i = (i + 1) & (SPU_OVL_EA_SET - 1)) {
        if (__atomic_load_n(&s_ovl_ea_used[i], __ATOMIC_ACQUIRE)) {
            if (s_ovl_ea_set[i] == ea) return;
            continue;
        }
        s_ovl_ea_set[i] = ea;
        __atomic_store_n(&s_ovl_ea_used[i], 1, __ATOMIC_RELEASE);
        __atomic_store_n(&s_ovl_ea_count, s_ovl_ea_count + 1, __ATOMIC_RELEASE);
        return;
    }
}
static int spu_ovl_ea_known(uint32_t ea)
{
    for (unsigned i = spu_ovl_ea_slot(ea), n = 0; n < SPU_OVL_EA_SET; ++n, i = (i + 1) & (SPU_OVL_EA_SET - 1)) {
        if (!__atomic_load_n(&s_ovl_ea_used[i], __ATOMIC_ACQUIRE)) return 0;
        if (s_ovl_ea_set[i] == ea) return 1;
    }
    return 1;
}

#define SPU_OVL_SIG_SET 512u
static uint64_t s_ovl_sig_set[SPU_OVL_SIG_SET];
static uint8_t s_ovl_sig_used[SPU_OVL_SIG_SET];
uint64_t g_spu_ovl_ea_64k[1024];
uint64_t g_spu_ovl_sig_bloom[4];
static inline unsigned spu_ovl_sig_slot(uint64_t sig0)
{
    return (unsigned)(((uint32_t)(sig0 ^ (sig0 >> 32)) * 0x9E3779B1u) >> 23) & (SPU_OVL_SIG_SET - 1);
}
static void spu_ovl_sig_add(const uint8_t sig[16])
{
    uint64_t sig0;
    memcpy(&sig0, sig, 8);
    for (unsigned i = spu_ovl_sig_slot(sig0), n = 0; n < SPU_OVL_SIG_SET; ++n, i = (i + 1) & (SPU_OVL_SIG_SET - 1)) {
        if (__atomic_load_n(&s_ovl_sig_used[i], __ATOMIC_ACQUIRE)) {
            if (s_ovl_sig_set[i] == sig0) break;
            continue;
        }
        s_ovl_sig_set[i] = sig0;
        __atomic_store_n(&s_ovl_sig_used[i], 1, __ATOMIC_RELEASE);
        break;
    }
    const uint64_t h = sig0 * 0x9E3779B97F4A7C15ull;
    const unsigned b0 = (unsigned)(h >> 56);
    const unsigned b1 = (unsigned)((h >> 48) & 255u);
    __atomic_fetch_or(&g_spu_ovl_sig_bloom[b0 >> 6], 1ull << (b0 & 63u), __ATOMIC_RELEASE);
    __atomic_fetch_or(&g_spu_ovl_sig_bloom[b1 >> 6], 1ull << (b1 & 63u), __ATOMIC_RELEASE);
}
static inline int spu_ovl_sig_known(uint64_t sig0)
{
    for (unsigned i = spu_ovl_sig_slot(sig0), n = 0; n < SPU_OVL_SIG_SET; ++n, i = (i + 1) & (SPU_OVL_SIG_SET - 1)) {
        if (!__atomic_load_n(&s_ovl_sig_used[i], __ATOMIC_ACQUIRE)) return 0;
        if (s_ovl_sig_set[i] == sig0) return 1;
    }
    return 1;
}

uint32_t g_spu_ovl_ea_lo = 0xFFFFFFFFu;
uint32_t g_spu_ovl_ea_hi = 0u;
int g_spu_ovl_has_sig = 0;

static inline void spu_ovl_note_ea_range(uint32_t lo, uint32_t hi)
{
    if (lo < hi) {
        const uint32_t c0 = lo >> 16;
        const uint32_t c1 = (hi - 1u) >> 16;
        for (uint32_t c = c0; c <= c1; ++c)
            __atomic_fetch_or(&g_spu_ovl_ea_64k[c >> 6], 1ull << (c & 63u), __ATOMIC_RELEASE);
    }
    uint32_t cur = __atomic_load_n(&g_spu_ovl_ea_lo, __ATOMIC_RELAXED);
    while (lo < cur && !__atomic_compare_exchange_n(&g_spu_ovl_ea_lo, &cur, lo, 1, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {}
    cur = __atomic_load_n(&g_spu_ovl_ea_hi, __ATOMIC_RELAXED);
    while (hi > cur && !__atomic_compare_exchange_n(&g_spu_ovl_ea_hi, &cur, hi, 1, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {}
}

void spu_overlay_register_source(uint32_t content_ea, int image_id)
{
    if (s_ovl_src_count < SPU_OVL_SRC_MAX) {
        spu_ovl_note_ea_range(content_ea, content_ea + 1u);
        spu_ovl_ea_add(content_ea);
        s_ovl_src[s_ovl_src_count].src_ea = content_ea;
        s_ovl_src[s_ovl_src_count].image_id = image_id;
        s_ovl_src[s_ovl_src_count].has_sig = 0;
        s_ovl_src[s_ovl_src_count].canon = 0;
        __atomic_store_n(&s_ovl_src_count, s_ovl_src_count + 1, __ATOMIC_RELEASE);
    }
}

void spu_overlay_register_region(uint32_t content_ea, uint32_t span, int image_id)
{
    if (!span || span > SPU_LS_SIZE || s_ovl_src_count >= SPU_OVL_SRC_MAX) return;
    spu_ovl_note_ea_range(content_ea, content_ea + span);
    spu_overlay_register_source(content_ea, image_id);
    s_ovl_src[s_ovl_src_count - 1].span = span;
}

void spu_overlay_register_region_reloc(uint32_t content_ea, uint32_t span, uint32_t canon, int image_id)
{
    if (!canon || canon >= SPU_LS_SIZE || span > SPU_LS_SIZE - canon) return;
    const int n = s_ovl_src_count;
    spu_overlay_register_region(content_ea, span, image_id);
    if (s_ovl_src_count == n + 1) s_ovl_src[n].canon = canon;
}

enum { SPU_OVERLAY_PENDING = 1, SPU_OVERLAY_VERIFIED, SPU_OVERLAY_REJECTED };
#define SPU_OVERLAY_CODE_SEGMENTS 8u
int spu_overlay_register_verified_region(uint32_t content_ea, uint32_t load_lsa,
    uint32_t span, int image_id, const spu_overlay_code_segment* code, unsigned count)
{
    if (!span || load_lsa >= SPU_LS_SIZE || span > SPU_LS_SIZE - load_lsa || image_id <= 0 ||
        !code || !count || count > SPU_OVERLAY_CODE_SEGMENTS || s_ovl_src_count >= SPU_OVL_SRC_MAX)
        return 0;
    size_t total = 0;
    for (unsigned i = 0; i < count; ++i) {
        const spu_overlay_code_segment* c = &code[i];
        if (!c->bytes || !c->size || c->lsa < load_lsa || c->size > span ||
            c->lsa - load_lsa > span - c->size) return 0;
        total += c->size;
    }
    spu_overlay_code_segment* copy = (spu_overlay_code_segment*)malloc(count * sizeof *copy + total);
    if (!copy) return 0;
    uint8_t* bytes = (uint8_t*)(copy + count);
    for (unsigned i = 0; i < count; ++i) {
        copy[i] = code[i];
        memcpy(bytes, code[i].bytes, code[i].size);
        copy[i].bytes = bytes;
        bytes += code[i].size;
    }
    spu_overlay_register_region(content_ea, span, image_id);
    spu_ovl_src* o = &s_ovl_src[s_ovl_src_count - 1];
    o->load_lsa = load_lsa; o->code = copy; o->code_count = count;
    return 1;
}

#define SPU_OVL_AT_MAX 256
static struct { uint32_t ea, lsa, span; int image_id; uint64_t head8; } s_ovl_at[SPU_OVL_AT_MAX];
static int s_ovl_at_count;
#define SPU_OVL_AT_BLOCK_SHIFT 11
#define SPU_OVL_AT_WORDS (SPU_OVL_AT_MAX / 64)
static uint64_t s_ovl_at_blocks[SPU_LS_SIZE >> SPU_OVL_AT_BLOCK_SHIFT][SPU_OVL_AT_WORDS];
#define SPU_OVL_AT_DELTA_SET 512u
static uint32_t s_ovl_at_delta_set[SPU_OVL_AT_DELTA_SET];
static uint8_t s_ovl_at_delta_used[SPU_OVL_AT_DELTA_SET];
static inline unsigned spu_ovl_at_delta_slot(uint32_t d) { return (d * 0x9E3779B1u) >> 23; }
static void spu_ovl_at_delta_add(uint32_t d)
{
    for (unsigned i = spu_ovl_at_delta_slot(d), n = 0; n < SPU_OVL_AT_DELTA_SET; ++n, i = (i + 1) & (SPU_OVL_AT_DELTA_SET - 1)) {
        if (__atomic_load_n(&s_ovl_at_delta_used[i], __ATOMIC_ACQUIRE)) {
            if (s_ovl_at_delta_set[i] == d) return;
            continue;
        }
        s_ovl_at_delta_set[i] = d;
        __atomic_store_n(&s_ovl_at_delta_used[i], 1, __ATOMIC_RELEASE);
        return;
    }
}
static inline int spu_ovl_at_delta_known(uint32_t d)
{
    for (unsigned i = spu_ovl_at_delta_slot(d), n = 0; n < SPU_OVL_AT_DELTA_SET; ++n, i = (i + 1) & (SPU_OVL_AT_DELTA_SET - 1)) {
        if (!__atomic_load_n(&s_ovl_at_delta_used[i], __ATOMIC_ACQUIRE)) return 0;
        if (s_ovl_at_delta_set[i] == d) return 1;
    }
    return 1;
}
void spu_overlay_register_region_at(uint32_t content_ea, uint32_t lsa, uint32_t span, int image_id)
{
    if (!span || image_id <= 0 || lsa >= SPU_LS_SIZE || span > SPU_LS_SIZE - lsa) return;
    const int n = __atomic_load_n(&s_ovl_at_count, __ATOMIC_ACQUIRE);
    if (n >= SPU_OVL_AT_MAX) {
        fprintf(stderr, "[spu-ovl] region_at registry full, image %d not registered\n", image_id);
        return;
    }
    s_ovl_at[n].ea = content_ea; s_ovl_at[n].lsa = lsa;
    s_ovl_at[n].span = span; s_ovl_at[n].image_id = image_id;
    s_ovl_at[n].head8 = 0;
    spu_ovl_note_ea_range(content_ea, content_ea + span);
    spu_ovl_at_delta_add(content_ea - lsa);
    for (uint32_t b = lsa >> SPU_OVL_AT_BLOCK_SHIFT; b <= (lsa + span - 1u) >> SPU_OVL_AT_BLOCK_SHIFT; b++)
        __atomic_fetch_or(&s_ovl_at_blocks[b][n >> 6], 1ull << (n & 63), __ATOMIC_RELAXED);
    __atomic_store_n(&s_ovl_at_count, n + 1, __ATOMIC_RELEASE);
}

static inline int spu_bytes_differ(const uint8_t* a, const uint8_t* b, uint32_t n);

static inline int spu_eq16(const uint8_t* a, const uint8_t* b)
{
    uint64_t a0, a1, b0, b1;
    memcpy(&a0, a, 8); memcpy(&a1, a + 8, 8);
    memcpy(&b0, b, 8); memcpy(&b1, b + 8, 8);
    return a0 == b0 && a1 == b1;
}

static int spu_overlay_resident_verified(spu_context* ctx, unsigned slot)
{
    int* state = &ctx->resident_code[slot].verify_state;
    if (*state == SPU_OVERLAY_PENDING) {
        spu_ovl_src* o = &s_ovl_src[ctx->resident_code[slot].source_index];
        const uint32_t canon = ctx->resident_code[slot].canon;
        const uint32_t sea = ctx->resident_code[slot].source_ea;
        const int fast_ea = sea && sea == __atomic_load_n(&o->verified_ea, __ATOMIC_ACQUIRE);
        int match = 1;
        for (unsigned i = 0; match && i < o->code_count; ++i) {
            const uint32_t sz = o->code[i].size;
            const uint32_t at = canon ? ctx->resident_code[slot].lsa + (o->code[i].lsa - canon) : o->code[i].lsa;
            if (at > SPU_LS_SIZE - sz) {
                match = 0;
            } else if (fast_ea && sz >= 48) {
                const uint8_t* p = ctx->ls + at;
                const uint8_t* q = o->code[i].bytes;
                match = spu_eq16(p, q) && spu_eq16(p + (sz >> 1), q + (sz >> 1)) && spu_eq16(p + sz - 16, q + sz - 16);
            } else {
                match = !spu_bytes_differ(ctx->ls + at, o->code[i].bytes, sz);
            }
        }
        if (match && sea && !fast_ea) __atomic_store_n(&o->verified_ea, sea, __ATOMIC_RELEASE);
        *state = match ? SPU_OVERLAY_VERIFIED : SPU_OVERLAY_REJECTED;
    }
    return *state == SPU_OVERLAY_VERIFIED;
}

int spu_overlay_register_reloc_content(const uint8_t* code, uint32_t span, uint32_t canon, int image_id)
{
    if (!code || span < 16 || !canon || canon >= SPU_LS_SIZE || span > SPU_LS_SIZE - canon ||
        image_id <= 0 || s_ovl_src_count >= SPU_OVL_SRC_MAX)
        return 0;
    spu_overlay_code_segment* copy = (spu_overlay_code_segment*)malloc(sizeof *copy + span);
    if (!copy) return 0;
    uint8_t* bytes = (uint8_t*)(copy + 1);
    memcpy(bytes, code, span);
    copy->lsa = canon; copy->size = span; copy->bytes = bytes;
    spu_ovl_src* o = &s_ovl_src[s_ovl_src_count];
    memset(o, 0, sizeof *o);
    o->image_id = image_id;
    memcpy(o->sig, code, 16);
    o->has_sig = 1;
    o->span = span;
    o->canon = canon;
    o->load_lsa = canon;
    o->code = copy;
    o->code_count = 1;
    spu_ovl_sig_add(o->sig);
    __atomic_store_n(&g_spu_ovl_has_sig, 1, __ATOMIC_RELEASE);
    atomic_store_explicit(&s_ovl_has_sig, 1, memory_order_release);
    __atomic_store_n(&s_ovl_src_count, s_ovl_src_count + 1, __ATOMIC_RELEASE);
    return 1;
}

static int spu_overlay_resolve_reloc(spu_context* ctx, uint32_t pc)
{
    const int nsrc = __atomic_load_n(&s_ovl_src_count, __ATOMIC_ACQUIRE);
    uint32_t max_span = 0;
    int cand[SPU_OVL_SRC_MAX];
    uint64_t sig[SPU_OVL_SRC_MAX][2];
    int ncand = 0;
    for (int i = 0; i < nsrc; i++)
        if (s_ovl_src[i].canon && s_ovl_src[i].code) {
            if (s_ovl_src[i].span > max_span) max_span = s_ovl_src[i].span;
            memcpy(sig[ncand], s_ovl_src[i].sig, 16);
            cand[ncand++] = i;
        }
    if (!max_span) return 0;
    const uint32_t lowest = pc >= max_span ? pc - max_span + 1 : 0;
    for (uint32_t base = pc & ~15u; ; base -= 16) {
        uint64_t at[2];
        memcpy(at, ctx->ls + base, 16);
        for (int k = 0; k < ncand; k++) {
            if (at[0] != sig[k][0] || at[1] != sig[k][1]) continue;
            const int i = cand[k];
            const spu_ovl_src* o = &s_ovl_src[i];
            if (pc - base >= o->span || base > SPU_LS_SIZE - o->span ||
                spu_bytes_differ(ctx->ls + base, o->code[0].bytes, o->code[0].size))
                continue;
            int slot = -1;
            for (unsigned s = 0; s < SPU_RESIDENT_CODE_SLOTS && slot < 0; ++s)
                if (!ctx->resident_code[s].image_id ||
                    ctx->resident_code[s].image_id == o->image_id ||
                    ctx->resident_code[s].lsa == base)
                    slot = (int)s;
            if (slot < 0) {
                fprintf(stderr, "[spu-ovl] no free resident code span for image %d\n", o->image_id);
                return 0;
            }
            ctx->resident_code[slot].lsa = base;
            ctx->resident_code[slot].size = o->span;
            ctx->resident_code[slot].source_ea = 0;
            ctx->resident_code[slot].image_id = o->image_id;
            ctx->resident_code[slot].verify_state = SPU_OVERLAY_VERIFIED;
            ctx->resident_code[slot].source_index = i;
            ctx->resident_code[slot].canon = o->canon;
            spu_dcache_residency_changed(ctx);
            return 1;
        }
        if (base < 16 || base - 16 < lowest) break;
    }
    return 0;
}

void spu_overlay_register_sig(const uint8_t sig[16], int image_id)
{
    if (s_ovl_src_count < SPU_OVL_SRC_MAX) {
        s_ovl_src[s_ovl_src_count].src_ea = 0;
        s_ovl_src[s_ovl_src_count].image_id = image_id;
        memcpy(s_ovl_src[s_ovl_src_count].sig, sig, 16);
        s_ovl_src[s_ovl_src_count].has_sig = 1;
        spu_ovl_sig_add(sig);
        __atomic_store_n(&g_spu_ovl_has_sig, 1, __ATOMIC_RELEASE);
        atomic_store_explicit(&s_ovl_has_sig, 1, memory_order_release);
        __atomic_store_n(&s_ovl_src_count, s_ovl_src_count + 1, __ATOMIC_RELEASE);
    }
}

static inline int spu_bytes_differ(const uint8_t* a, const uint8_t* b, uint32_t n)
{
    if (n >= 8) {
        uint64_t x, y;
        memcpy(&x, a, 8);
        memcpy(&y, b, 8);
        if (x != y) return 1;
    }
#ifdef __AVX2__
    uint32_t i = 0;
    for (; i + 64 <= n; i += 64) {
        const __m256i d0 = _mm256_xor_si256(_mm256_loadu_si256((const __m256i*)(a + i)),
                                            _mm256_loadu_si256((const __m256i*)(b + i)));
        const __m256i d1 = _mm256_xor_si256(_mm256_loadu_si256((const __m256i*)(a + i + 32)),
                                            _mm256_loadu_si256((const __m256i*)(b + i + 32)));
        const __m256i any = _mm256_or_si256(d0, d1);
        if (!_mm256_testz_si256(any, any)) return 1;
    }
    if (i + 32 <= n) {
        const __m256i d = _mm256_xor_si256(_mm256_loadu_si256((const __m256i*)(a + i)),
                                           _mm256_loadu_si256((const __m256i*)(b + i)));
        if (!_mm256_testz_si256(d, d)) return 1;
        i += 32;
    }
    if (i < n && n >= 32) {
        const __m256i d = _mm256_xor_si256(_mm256_loadu_si256((const __m256i*)(a + n - 32)),
                                           _mm256_loadu_si256((const __m256i*)(b + n - 32)));
        return !_mm256_testz_si256(d, d);
    }
    for (; i + 8 <= n; i += 8) {
        uint64_t x, y;
        memcpy(&x, a + i, 8);
        memcpy(&y, b + i, 8);
        if (x != y) return 1;
    }
    for (; i < n; i++)
        if (a[i] != b[i]) return 1;
    return 0;
#else
    return memcmp(a, b, n) != 0;
#endif
}

static inline int spu_is_all_zero(const uint8_t* p, uint32_t n)
{
    uint32_t i = 0;
#ifdef __AVX2__
    for (; i + 32 <= n; i += 32) {
        const __m256i v = _mm256_loadu_si256((const __m256i*)(p + i));
        if (!_mm256_testz_si256(v, v)) return 0;
    }
#endif
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        if (w) return 0;
    }
    for (; i < n; i++)
        if (p[i]) return 0;
    return 1;
}

static int spu_overlay_resolve_at_lazy(spu_context* ctx, uint32_t lsa)
{
    const int nat = __atomic_load_n(&s_ovl_at_count, __ATOMIC_ACQUIRE);
    if (!nat || lsa >= SPU_LS_SIZE) return 0;
    const unsigned nwords = (unsigned)((nat + 63) >> 6);
    const uint32_t b = lsa >> SPU_OVL_AT_BLOCK_SHIFT;
    for (unsigned w = 0; w < nwords; w++) {
        uint64_t bits = __atomic_load_n(&s_ovl_at_blocks[b][w], __ATOMIC_RELAXED);
        for (; bits; bits &= bits - 1) {
            const int i = (int)(w * 64u + (unsigned)__builtin_ctzll(bits));
            if (i >= nat) break;
            const uint32_t rlo = s_ovl_at[i].lsa, span = s_ovl_at[i].span;
            if (lsa < rlo || lsa - rlo >= span) continue;
            const uint8_t* ls_p = ctx->ls + rlo;
            const uint8_t* orig = vm_base + s_ovl_at[i].ea;
            if (span >= 8) {
                uint64_t h = __atomic_load_n(&s_ovl_at[i].head8, __ATOMIC_RELAXED);
                if (!h) {
                    memcpy(&h, orig, 8);
                    if (h) __atomic_store_n(&s_ovl_at[i].head8, h, __ATOMIC_RELAXED);
                }
                if (h) {
                    uint64_t q0;
                    memcpy(&q0, ls_p, 8);
                    if (q0 != h) continue;
                }
            }
            const uint32_t check_n = span < 256u ? span : 256u;
            if (spu_bytes_differ(ls_p, orig, check_n)) continue;
            const uint32_t off = lsa - rlo;
            const uint32_t at_n = (span - off) < 64u ? (span - off) : 64u;
            if (spu_bytes_differ(ctx->ls + lsa, orig + off, at_n)) continue;
            if (spu_is_all_zero(ctx->ls + lsa, at_n)) continue;
            int free_slot = -1;
            for (unsigned slot = 0; slot < SPU_RESIDENT_CODE_SLOTS; ++slot) {
                if (!ctx->resident_code[slot].image_id || ctx->resident_code[slot].lsa == rlo) {
                    free_slot = (int)slot;
                    break;
                }
            }
            if (free_slot >= 0) {
                ctx->resident_code[free_slot].lsa = rlo;
                ctx->resident_code[free_slot].size = span;
                ctx->resident_code[free_slot].source_ea = s_ovl_at[i].ea;
                ctx->resident_code[free_slot].image_id = s_ovl_at[i].image_id;
                ctx->resident_code[free_slot].verify_state = 0;
                ctx->resident_code[free_slot].source_index = 0;
                ctx->resident_code[free_slot].canon = 0;
                spu_dcache_residency_changed(ctx);
            }
            return s_ovl_at[i].image_id;
        }
    }
    return 0;
}

void spu_overlay_note_get(spu_context* ctx, uint32_t ea, const uint8_t* ls, uint32_t size)
{
    uint32_t lsa = (uint32_t)(ls - ctx->ls);
    if (ctx->resident_code_mask && lsa < ctx->resident_code_hi && lsa + size > ctx->resident_code_lo) {
        int changed = 0;
        for (uint32_t m = ctx->resident_code_mask; m; m &= m - 1u) {
            const unsigned slot = (unsigned)__builtin_ctz(m);
            uint32_t base = ctx->resident_code[slot].lsa;
            uint32_t end = base + ctx->resident_code[slot].size;
            if (lsa < end && lsa + size > base) {
                if (lsa < base || ea != ctx->resident_code[slot].source_ea + (lsa - base)) {
                    if (!ctx->resident_code[slot].canon) {
                        const uint32_t lo = lsa > base ? lsa : base;
                        const uint32_t hi = (lsa + size) < end ? (lsa + size) : end;
                        const uint8_t* orig = vm_base + ctx->resident_code[slot].source_ea + (lo - base);
                        if (spu_is_all_zero(orig, hi - lo) || !spu_bytes_differ(ls + (lo - lsa), orig, hi - lo)) continue;
                    }
                    ctx->resident_code[slot].image_id = 0;
                    changed = 1;
                } else if (ctx->resident_code[slot].verify_state) {
                    ctx->resident_code[slot].verify_state = SPU_OVERLAY_PENDING;
                    changed = 1;
                }
            }
        }
        if (changed) spu_dcache_residency_changed(ctx);
    }
    const int nat = __atomic_load_n(&s_ovl_at_count, __ATOMIC_ACQUIRE);
    if (nat && size && spu_ovl_at_delta_known(ea - lsa)) {
        const unsigned nwords = (unsigned)((nat + 63) >> 6);
        uint64_t cover[SPU_OVL_AT_WORDS] = { 0 };
        const uint32_t last = (uint32_t)(SPU_LS_SIZE >> SPU_OVL_AT_BLOCK_SHIFT) - 1u;
        const uint32_t b1 = (lsa + size - 1u) >> SPU_OVL_AT_BLOCK_SHIFT;
        const uint32_t bend = b1 < last ? b1 : last;
        uint64_t any = 0;
        for (uint32_t b = lsa >> SPU_OVL_AT_BLOCK_SHIFT; b <= bend; b++)
            for (unsigned w = 0; w < nwords; w++) {
                const uint64_t v = __atomic_load_n(&s_ovl_at_blocks[b][w], __ATOMIC_RELAXED);
                cover[w] |= v;
                any |= v;
            }
        if (any) {
            const uint32_t delta = ea - lsa;
            for (unsigned w = 0; w < nwords; w++)
            for (uint64_t bits = cover[w]; bits; bits &= bits - 1) {
                const int i = (int)(w * 64u + (unsigned)__builtin_ctzll(bits));
                if (i >= nat) break;
                const uint32_t rlo = s_ovl_at[i].lsa, rhi = rlo + s_ovl_at[i].span;
                if (s_ovl_at[i].ea - rlo != delta) continue;
                const uint32_t lo = lsa > rlo ? lsa : rlo;
                const uint32_t hi = (lsa + size) < rhi ? (lsa + size) : rhi;
                if (lo >= hi || (hi - lo < 128 && hi - lo != size)) continue;
                int have = 0, free_slot = -1;
                for (unsigned slot = 0; slot < SPU_RESIDENT_CODE_SLOTS; ++slot) {
                    if (ctx->resident_code[slot].image_id == s_ovl_at[i].image_id &&
                        ctx->resident_code[slot].lsa == rlo) { have = 1; break; }
                    if (free_slot < 0 && (!ctx->resident_code[slot].image_id || ctx->resident_code[slot].lsa == rlo))
                        free_slot = (int)slot;
                }
                if (have) continue;
                if (spu_is_all_zero(ls + (lo - lsa), hi - lo)) continue;
                if (free_slot < 0) {
                    fprintf(stderr, "[spu-ovl] no free resident code span for image %d\n", s_ovl_at[i].image_id);
                    continue;
                }
                ctx->resident_code[free_slot].lsa = rlo;
                ctx->resident_code[free_slot].size = s_ovl_at[i].span;
                ctx->resident_code[free_slot].source_ea = s_ovl_at[i].ea;
                ctx->resident_code[free_slot].image_id = s_ovl_at[i].image_id;
                ctx->resident_code[free_slot].verify_state = 0;
                ctx->resident_code[free_slot].source_index = 0;
                ctx->resident_code[free_slot].canon = 0;
                spu_dcache_residency_changed(ctx);
            }
        }
    }
    int nsrc = __atomic_load_n(&s_ovl_src_count, __ATOMIC_ACQUIRE);
    if (!nsrc) return;
    int sig_ok = 0;
    uint64_t a0 = 0, a1 = 0;
    if (size >= 512 && atomic_load_explicit(&s_ovl_has_sig, memory_order_acquire)) {
        memcpy(&a0, ls, 8);
        sig_ok = spu_ovl_sig_known(a0);
        if (sig_ok) memcpy(&a1, ls + 8, 8);
    }
    const int ea_ok = __atomic_load_n(&s_ovl_ea_count, __ATOMIC_ACQUIRE) ? spu_ovl_ea_known(ea) : 0;
    if (!sig_ok && !ea_ok) return;
    for (int i = 0; i < nsrc; i++) {
        const spu_ovl_src* o = &s_ovl_src[i];
        int hit;
        if (o->has_sig) {
            if (sig_ok) {
                uint64_t b0, b1;
                memcpy(&b0, o->sig, 8); memcpy(&b1, o->sig + 8, 8);
                hit = (a0 == b0 && a1 == b1);
            } else {
                hit = 0;
            }
        } else {
            hit = ea_ok && (o->src_ea == ea);
        }
        if (hit) {
            if (o->span) {
                if (lsa + o->span > SPU_LS_SIZE) return;
                if (o->code && !o->canon && lsa != o->load_lsa) continue;
                for (unsigned slot = 0; slot < SPU_RESIDENT_CODE_SLOTS; ++slot) {
                    if (ctx->resident_code[slot].image_id && ctx->resident_code[slot].lsa != lsa &&
                        !(o->canon && ctx->resident_code[slot].image_id == o->image_id))
                        continue;
                    ctx->resident_code[slot].lsa = lsa;
                    ctx->resident_code[slot].size = o->span;
                    ctx->resident_code[slot].source_ea = ea;
                    ctx->resident_code[slot].image_id = o->image_id;
                    ctx->resident_code[slot].verify_state = o->code ? SPU_OVERLAY_PENDING : 0;
                    ctx->resident_code[slot].source_index = i;
                    ctx->resident_code[slot].canon = o->canon;
                    spu_dcache_residency_changed(ctx);
                    return;
                }
                fprintf(stderr, "[spu-ovl] no free resident code span for image %d\n", o->image_id);
                return;
            }
            if (ctx->resident_ovl != o->image_id) {
                ctx->resident_ovl = o->image_id;
                { static int _n = 0; if (PS3_LOG_FIRST(_n, 32))
                    fprintf(stderr, "[spu-ovl] img=%d streamed overlay src=0x%08X "
                            "-> LS 0x%05X size=%u -> resident ovl image %d%s\n",
                            ctx->image_id, ea, (uint32_t)(ls - ctx->ls), size,
                            ctx->resident_ovl, o->has_sig ? " (sig match)" : ""); }
            }
            return;
        }
    }
}

static int spu_smc_microstep(spu_context* ctx)
{
    uint32_t pc = ctx->pc & SPU_LS_MASK & ~3u;
    for (int steps = 0; steps < 4096; steps++) {
        {
            for (; steps < 4096; steps++, pc = (pc + 4) & SPU_LS_MASK) {
                uint32_t wf;
                memcpy(&wf, ctx->ls + pc, 4);
                wf = SPU_BSWAP32(wf);
                const uint32_t op9f = wf >> 23, op8f = wf >> 24;
                uint32_t a;
                if (op9f == 0x041 || op9f == 0x061)
                    a = ((wf >> 7) & 0xFFFF) << 2;
                else if (op8f == 0x24 || op8f == 0x34)
                    a = ctx->gpr[(wf >> 7) & 0x7F]._u32[0] + ((uint32_t)((int32_t)(wf << 8) >> 22) << 4);
                else
                    break;
                uint8_t* p = ctx->ls + (a & SPU_LS_MASK & ~15u);
                u128* r = &ctx->gpr[wf & 0x7F];
                if (op9f == 0x041 || op8f == 0x24) {
                    const uint32_t q[4] = { SPU_BSWAP32(r->_u32[0]), SPU_BSWAP32(r->_u32[1]),
                                            SPU_BSWAP32(r->_u32[2]), SPU_BSWAP32(r->_u32[3]) };
                    memcpy(p, q, 16);
                } else {
                    uint32_t q[4];
                    memcpy(q, p, 16);
                    for (int k = 0; k < 4; k++) r->_u32[k] = SPU_BSWAP32(q[k]);
                }
            }
            if (steps >= 4096) break;
        }
        uint32_t w = ((uint32_t)ctx->ls[pc] << 24) | ((uint32_t)ctx->ls[pc+1] << 16) |
                     ((uint32_t)ctx->ls[pc+2] << 8) | ctx->ls[pc+3];
        uint32_t op11 = w >> 21, op9 = w >> 23, op8 = w >> 24, op7 = w >> 25;
        uint32_t rt = w & 0x7F, ra = (w >> 7) & 0x7F, rb = (w >> 14) & 0x7F;
        int32_t  i10 = (int32_t)(w << 8) >> 22;
        int32_t  i16 = (int32_t)(int16_t)((w >> 7) & 0xFFFF);
        (void)rb;

        if (op11 == 0x1A8 || op11 == 0x1A9 || op11 == 0x1AA || op11 == 0x1AB) {
            if (op11 == 0x1AB &&
                (ctx->event_status & ctx->event_mask) == 0) { pc += 4; continue; }
            if (w & 0x40000) ctx->int_enable = 1;
            else if (w & 0x80000) ctx->int_enable = 0;
            uint32_t tgt = (op11 == 0x1AA) ? ctx->srr0
                                           : ctx->gpr[ra]._u32[0];
            if (op11 == 0x1A9 || op11 == 0x1AB)
                ctx->gpr[rt] = spu_splat_u32(pc + 4);
            ctx->pc = tgt & SPU_LS_MASK & ~3u;
            g_spu_trampoline_fn = spu_indirect_branch;
            return 1;
        }
        if (op11 >= 0x128 && op11 <= 0x12B) {
            uint32_t cv = ctx->gpr[rt]._u32[0];
            int taken;
            switch (op11) {
            case 0x128: taken = (cv == 0); break;
            case 0x129: taken = (cv != 0); break;
            case 0x12A: taken = ((cv & 0xFFFF) == 0); break;
            default:    taken = ((cv & 0xFFFF) != 0); break;
            }
            if (!taken) { pc += 4; continue; }
            if (w & 0x40000) ctx->int_enable = 1;
            else if (w & 0x80000) ctx->int_enable = 0;
            ctx->pc = ctx->gpr[ra]._u32[0] & SPU_LS_MASK & ~3u;
            g_spu_trampoline_fn = spu_indirect_branch;
            return 1;
        }
        if (op9 == 0x064 || op9 == 0x060 || op9 == 0x066 || op9 == 0x062) {
            uint32_t tgt = (op9 == 0x060 || op9 == 0x062)
                         ? ((uint32_t)i16 << 2)
                         : (pc + ((uint32_t)i16 << 2));
            if (op9 == 0x066 || op9 == 0x062)
                ctx->gpr[rt] = spu_splat_u32(pc + 4);
            ctx->pc = tgt & SPU_LS_MASK & ~3u;
            g_spu_trampoline_fn = spu_indirect_branch;
            return 1;
        }
        if (op9 == 0x040 || op9 == 0x042 || op9 == 0x044 || op9 == 0x046) {
            uint32_t cv = ctx->gpr[rt]._u32[0];
            int taken;
            switch (op9) {
            case 0x040: taken = (cv == 0); break;
            case 0x042: taken = (cv != 0); break;
            case 0x044: taken = ((cv & 0xFFFF) == 0); break;
            default:    taken = ((cv & 0xFFFF) != 0); break;
            }
            if (!taken) { pc += 4; continue; }
            ctx->pc = (pc + ((uint32_t)i16 << 2)) & SPU_LS_MASK & ~3u;
            g_spu_trampoline_fn = spu_indirect_branch;
            return 1;
        }
        if (op8 == 0x24) {
            uint32_t a = (ctx->gpr[ra]._u32[0] + ((uint32_t)i10 << 4)) & SPU_LS_MASK & ~15u;
            spu_ls_write128(ctx, a, ctx->gpr[rt]);
            pc += 4; continue;
        }
        if (op8 == 0x34) {
            uint32_t a = (ctx->gpr[ra]._u32[0] + ((uint32_t)i10 << 4)) & SPU_LS_MASK & ~15u;
            ctx->gpr[rt] = spu_ls_read128(ctx, a);
            pc += 4; continue;
        }
        if (op9 == 0x041 || op9 == 0x061) {
            uint32_t a = (((w >> 7) & 0xFFFF) << 2) & SPU_LS_MASK & ~15u;
            if (op9 == 0x041) spu_ls_write128(ctx, a, ctx->gpr[rt]);
            else              ctx->gpr[rt] = spu_ls_read128(ctx, a);
            pc += 4; continue;
        }
        if (op9 == 0x047 || op9 == 0x067) {
            uint32_t a = (pc + ((uint32_t)i16 << 2)) & SPU_LS_MASK & ~15u;
            if (op9 == 0x047) spu_ls_write128(ctx, a, ctx->gpr[rt]);
            else              ctx->gpr[rt] = spu_ls_read128(ctx, a);
            pc += 4; continue;
        }
        if (op8 == 0x44) {
            ctx->gpr[rt] = spu_xori(ctx->gpr[ra], i10);
            pc += 4; continue;
        }
        if (op8 == 0x1C) {
            u128 r = ctx->gpr[ra];
            for (int k = 0; k < 4; k++) r._u32[k] += (uint32_t)i10;
            ctx->gpr[rt] = r; pc += 4; continue;
        }
        if (op11 == 0x040 || op11 == 0x0C0) {
            u128 x = ctx->gpr[ra], y = ctx->gpr[rb], r;
            for (int k = 0; k < 4; k++)
                r._u32[k] = (op11 == 0x040) ? y._u32[k] - x._u32[k]
                                            : x._u32[k] + y._u32[k];
            ctx->gpr[rt] = r; pc += 4; continue;
        }
        if (op9 == 0x081) { ctx->gpr[rt] = spu_splat_u32((uint32_t)i16); pc += 4; continue; }
        if (op9 == 0x082) { ctx->gpr[rt] = spu_splat_u32(((w >> 7) & 0xFFFF) << 16); pc += 4; continue; }
        if (op9 == 0x0C1) { u128 r = ctx->gpr[rt];
            for (int k = 0; k < 4; k++) r._u32[k] |= (w >> 7) & 0xFFFF;
            ctx->gpr[rt] = r; pc += 4; continue; }
        if (op7 == 0x21)  { ctx->gpr[rt] = spu_splat_u32((w >> 7) & 0x3FFFF); pc += 4; continue; }
        if (op11 == 0x201 || op11 == 0x001) { pc += 4; continue; }
        if (op11 == 0x002 || op11 == 0x003) { pc += 4; continue; }
        if (op11 == 0x1AC || op7 == 0x08 || op7 == 0x09) { pc += 4; continue; }

        { static int _n = 0;
          if (PS3_LOG_FIRST(_n, 8))
              fprintf(stderr, "[spu-smc] microstep img=%d pc=0x%05X word 0x%08X not in the"
                      " microstep set -- handing to the interpreter (steps=%d)\n",
                      ctx->image_id, pc, w, steps); }
        ctx->pc = pc;
        const uint32_t return_pc = ctx->drain_ret_pc;
        uint32_t result = spu_interp_run_until(ctx, pc, return_pc);
        if (ctx->image_id >= 0 && (ctx->status & SPU_STATUS_RUNNING) &&
            (!return_pc || (ctx->pc & SPU_LS_MASK) != return_pc) &&
            spu_lifted_lookup(ctx, ctx->pc)) {
            g_spu_trampoline_fn = spu_indirect_branch;
            return 1;
        }
        return result ? 1 : 0;
    }
    { static int _n = 0; if (PS3_LOG_FIRST(_n, 4))
        fprintf(stderr, "[spu-smc] microstep img=%d runaway (4096 steps from 0x%05X)\n",
                ctx->image_id, ctx->pc & SPU_LS_MASK); }
    return 0;
}

void spu_indirect_branch(spu_context* ctx)
{
    ctx->pc &= SPU_LS_MASK;
    if (__builtin_expect(ctx->irq_saved, 0)) {
        extern int spu_irq_regs_maybe_restore(spu_context*);
        spu_irq_regs_maybe_restore(ctx);
    }
    spu_fn fn = spu_lifted_lookup(ctx, ctx->pc);
    if (!fn && spu_overlay_resolve_reloc(ctx, ctx->pc & SPU_LS_MASK))
        fn = spu_lifted_lookup(ctx, ctx->pc);
    if (!fn) {
        uint32_t p0 = ctx->pc & SPU_LS_MASK;
        if (p0 + 3 < SPU_LS_SIZE) {
            uint32_t w = ((uint32_t)ctx->ls[p0] << 24) | ((uint32_t)ctx->ls[p0+1] << 16) |
                         ((uint32_t)ctx->ls[p0+2] << 8) | ctx->ls[p0+3];
            if ((w >> 21) == 0u) {
                ctx->stop_code = w & 0x3FFFu;
                static int _n = 0;
                if (PS3_LOG_FIRST(_n, 8))
                    fprintf(stderr, "[spu] img=%d exit: synthesised stop 0x%04X at LS 0x%05X\n",
                            ctx->image_id, ctx->stop_code, p0);
                spu_halt(ctx);
                return;
            }
        }
    }

    if (!fn && ctx->pc >= SPU_JM2_KERNEL_BASE) {
        static uint32_t seen[16]; static int n_seen = 0;
        int known = 0;
        for (int i = 0; i < n_seen; i++) if (seen[i] == ctx->pc) { known = 1; break; }
        if (!known && n_seen < 16) {
            seen[n_seen++] = ctx->pc;
            fprintf(stderr, "[spu] img=%d branched into unlifted LS 0x%05X "
                    "(lr=0x%05X) -- ending the job\n",
                    ctx->image_id, ctx->pc, ctx->gpr[0]._u32[0] & SPU_LS_MASK);
            { fprintf(stderr, "      last dispatched PCs (oldest first):");
              for (unsigned q = 0; q < 8; q++) {
                  unsigned idx = (g_spu_pch_n + q) & 7u;
                  if (g_spu_pch_n > q || g_spu_pch[idx]) fprintf(stderr, " 0x%05X", g_spu_pch[idx]);
              }
              fprintf(stderr, "%c", 10); }
            { uint32_t a[2]; a[0] = ctx->pc & SPU_LS_MASK;
              a[1] = ctx->gpr[0]._u32[0] & SPU_LS_MASK;
              for (int k = 0; k < 2; k++) {
                  fprintf(stderr, "      LS[0x%05X]:", a[k]);
                  for (uint32_t o = 0; o < 32 && a[k] + o + 3 < SPU_LS_SIZE; o += 4)
                      fprintf(stderr, " %02X%02X%02X%02X",
                              ctx->ls[a[k]+o], ctx->ls[a[k]+o+1],
                              ctx->ls[a[k]+o+2], ctx->ls[a[k]+o+3]);
                  fprintf(stderr, "\n");
              } }
            fflush(stderr);
        }
        { uint32_t p = ctx->pc & SPU_LS_MASK;
          uint32_t w0 = ((uint32_t)ctx->ls[p] << 24) | ((uint32_t)ctx->ls[p+1] << 16) |
                        ((uint32_t)ctx->ls[p+2] << 8) | ctx->ls[p+3];
          if (w0) {
              static int _n = 0;
              if (PS3_LOG_FIRST(_n, 8))
                  fprintf(stderr, "[spu] img=%d interpreting unlifted LS 0x%05X "
                          "(runtime-loaded code)\n", ctx->image_id, p);
              spu_interp_run(ctx, p);
              spu_stop_exit_check(ctx);
              return;
          } }
        spu_halt(ctx);
        return;
    }
    if (fn) {
#if defined(__clang__)
        __attribute__((musttail)) return fn(ctx);
#else
        fn(ctx);
        return;
#endif
    }

    if (ctx->pc == 0 && ctx->image_id > 0) {
        static int _n = 0;
        if (PS3_LOG_FIRST(_n, 8))
            fprintf(stderr, "[spurs-job] img=%d returned to the job manager "
                    "(branch to LS 0) -- job complete\n", ctx->image_id);
        spu_halt(ctx);
        return;
    }
    if (spu_smc_microstep(ctx)) {
        spu_stop_exit_check(ctx);
        return;
    }
    { enum { BT0_MAX_IMG = 64, BT0_PER_IMG = 12 };
      static int _bt0[BT0_MAX_IMG];
      unsigned img = (ctx->image_id >= 0 && ctx->image_id < BT0_MAX_IMG)
                     ? (unsigned)ctx->image_id : 0;
      if (PS3_LOG_FIRST(_bt0[img], BT0_PER_IMG))
        fprintf(stderr, "[SPU] BRANCH-TO-0 unresolved pc=0x%05X image=%d lr=0x%05X\n",
                ctx->pc, ctx->image_id, ctx->gpr[0]._u32[0] & SPU_LS_MASK); }
    { static int _n=0;
      if (PS3_LOG_FIRST(_n, 2)) {
        fprintf(stderr, "[SPU] branch-to-0 lr=0x%05X r1=0x%05X\n",
                ctx->gpr[0]._u32[0] & SPU_LS_MASK, ctx->gpr[1]._u32[0] & SPU_LS_MASK);
        void* frames[24]; unsigned short fn = RtlCaptureStackBackTrace(0, 24, frames, NULL);
        char* base = (char*)GetModuleHandleA(NULL);
        fprintf(stderr, "[SPU] host bt RVAs:");
        for (unsigned short i = 0; i < fn; i++)
            fprintf(stderr, " 0x%zX", (size_t)((char*)frames[i] - base));
        fprintf(stderr, "\n");
        fprintf(stderr, "[SPU] image_id=%d  GPR dump (r0..r127, hi64:lo64 of each quadword, preferred slot = _u32[0]):\n", ctx->image_id);
        for (int g = 0; g < 128; g++) {
            fprintf(stderr, " r%-3d=%08X %08X %08X %08X", g,
                    ctx->gpr[g]._u32[0], ctx->gpr[g]._u32[1],
                    ctx->gpr[g]._u32[2], ctx->gpr[g]._u32[3]);
            if ((g & 1) == 1) fprintf(stderr, "\n");
        }
        fprintf(stderr, "\n");
    } }
    ctx->status = SPU_STATUS_STOPPED_BY_HALT;
}

static FILE* s_trace_fp = NULL;

void spu_trace_init(const char* path)
{
    if (!path || !*path) { s_trace_fp = stderr; return; }
    s_trace_fp = fopen(path, "w");
    if (!s_trace_fp) s_trace_fp = stderr;
}

static long long s_trace_left = -1;

void spu_trace_pc(spu_context* ctx, uint32_t pc)
{
    (void)ctx;
    if (s_trace_left < 0) {
        if (s_trace_left == -1) {
            const char* p = getenv("SPU_TRACE_FILE");
            if ((!p || !*p) && !getenv("SPU_TRACE")) { s_trace_left = 0; return; }
            if (p && *p && !s_trace_fp) spu_trace_init(p);
            const char* m = getenv("SPU_TRACE_MAX");
            s_trace_left = (m && *m) ? atoll(m) : 200000;
            if (s_trace_left == 0) s_trace_left = -2;
        }
    }
    if (!s_trace_fp) s_trace_fp = stderr;
    if (s_trace_left == 0) return;
    if (s_trace_left > 0 && --s_trace_left == 0) {
        fprintf(s_trace_fp, "-- SPU_TRACE_MAX reached; trace stopped --\n");
        fflush(s_trace_fp);
        return;
    }
    fprintf(s_trace_fp, "%05X\n", pc & SPU_LS_MASK);
}

void spu_trace_rt(spu_context* ctx, uint32_t rt)
{
    if (s_trace_left == 0) return;
    if (!s_trace_fp) s_trace_fp = stderr;
    u128 v = ctx->gpr[rt & 0x7F];
    fprintf(s_trace_fp, "  r%-3u %016llX %016llX\n",
            (unsigned)(rt & 0x7F),
            (unsigned long long)v._u64[0],
            (unsigned long long)v._u64[1]);
}

#ifdef __cplusplus
}
#endif

