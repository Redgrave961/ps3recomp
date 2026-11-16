/*
 * ps3recomp - SPU channel + indirect-branch runtime glue
 *
 * Implements the externs the SPU lifter (tools/spu_lifter.py) emits:
 *   - spu_rdch / spu_rchcnt / spu_wrch : SPU channel access. MFC channels are
 *     routed to the DMA engine (spu_dma.h); mailboxes, signal notification,
 *     events and the decrementer use the spu_context channel fields.
 *   - spu_indirect_branch : resolves ctx->pc to a lifted spu_func_* via a
 *     registry that generated code populates by calling spu_recomp_register().
 *
 * The MFC engine state is kept per spu_context here (spu_context.h does not
 * embed one), in a small lazily-populated registry.
 */

#include "spu_dma.h"
#include "spu_coherency.h" /* lock-line lock + PPU-write coherence bitmap */
#include "spu_helpers.h"   /* spu_splat_u32 / spu_ls_read128 (SMC microstep) */
#include "spu_lockstep.h"
#include "spu_interp.h"    /* spu_lifted_fn / spu_lifted_lookup -- defined below */
#include "ps3emu/ps3_ftrace.h"
#include "spu_overlay_residency.h"
#include "../ps3_log.h"
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
/* setjmp/longjmp also save and restore the signal mask on POSIX: one
 * sigprocmask system call per halt, stack reset or task resume. These frames
 * never cross a signal handler, so the mask is the same at both ends. */
#if defined(_WIN32)
#define SPU_SETJMP(env) setjmp(env)
#define SPU_LONGJMP(env, v) longjmp(env, v)
#else
#define SPU_SETJMP(env) _setjmp(env)
#define SPU_LONGJMP(env, v) _longjmp(env, v)
#endif
#ifndef _WIN32
#include <sched.h>   /* sched_yield */
#endif
#include <time.h>
#include "../platform/win32_compat.h"
#include "ps3emu/log_first.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The SPU decrementer ticks at the PS3 timebase, 79.8 MHz -- the same clock
 * sys_time_get_timebase_frequency reports to the PPU. Titles calibrate real
 * delays against it, so the rate has to be right, not merely non-zero. */
#define SPU_DECREMENTER_HZ  79800000ull

static uint64_t spu_host_ns(void)
{
#ifdef _WIN32
    static LARGE_INTEGER s_freq;
    LARGE_INTEGER c;
    if (!s_freq.QuadPart) QueryPerformanceFrequency(&s_freq);
    QueryPerformanceCounter(&c);
    /* Split the divide to keep the multiply from overflowing: QPC counters are
     * large enough that (count * 1e9) wraps a u64 within hours of uptime. */
    return (uint64_t)(c.QuadPart / s_freq.QuadPart) * 1000000000ull
         + (uint64_t)(c.QuadPart % s_freq.QuadPart) * 1000000000ull
           / (uint64_t)s_freq.QuadPart;
#elif defined(__APPLE__)
    /* SPU_RdDec reads this on every decrementer read. Darwin builds
     * CLOCK_MONOTONIC from gettimeofday and the boot time, at microsecond
     * resolution; CLOCK_MONOTONIC_RAW is the mach continuous-time counter in
     * nanoseconds, at about half the cost. Both are monotonic, count across
     * sleep and start from an arbitrary origin, and the decrementer only ever
     * subtracts two readings of this one function. */
    return clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

/* ---------------------------------------------------------------------------
 * Clean SPU job abort (longjmp). The `br .` halt idiom and other terminal
 * spins can't be escaped by setting a status flag (lifted code never checks
 * it), so spu_halt() longjmps back to spu_run_with_halt() in the dispatcher.
 * -----------------------------------------------------------------------*/
#if defined(_MSC_VER)
#  define SPU_TLS __declspec(thread)
#else
#  define SPU_TLS __thread
#endif

static SPU_TLS jmp_buf s_spu_halt_env;
static SPU_TLS int     s_spu_halt_armed = 0;

/* Call right after a setjmp returned 0. On 64-bit Windows longjmp unwinds every
 * host frame between it and its setjmp (RtlUnwindEx, a function-table lookup
 * and a virtual unwind per frame), several microseconds per jump; with the
 * buffer's Frame zeroed it restores the registers directly. These jumps --
 * a halt back to spu_run_with_halt, an iret back to the drain loop that took
 * the interrupt -- only cross C frames of this runtime and of lifted code,
 * which have no destructors or __finally blocks to run. */
void spu_jmp_no_unwind(void* env)
{
#if defined(_WIN64) && defined(_MSC_VER)
    ((_JUMP_BUFFER*)env)->Frame = 0;
#else
    (void)env;
#endif
}

/* A non-local return replaces the old host call chain without changing
 * any architectural registers, local store, or pending guest work. */
void spu_restart_dispatch(spu_context* ctx)
{
    g_spu_trampoline_fn = 0;
    if (s_spu_halt_armed) SPU_LONGJMP(s_spu_halt_env, 2);
    fprintf(stderr, "[spu] non-local return outside execution driver at 0x%05X\n", ctx->pc);
    ctx->status = SPU_STATUS_STOPPED_BY_HALT;
}

/* SPU->PPU outbound-mailbox delivery hook. The SPU writing WrOutMbox /
 * WrOutIntrMbox must wake PPU code blocked on the SPURS event queue bound to
 * the SPU thread group (e.g. cellSpursInitialize). lv2_register.c installs a
 * handler that maps spu_group_id -> connected event queue and pushes an event.
 * NULL until installed (plain SPU jobs with no PPU listener stay a no-op). */
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

/* A lifted `stop` sets status and RETURNS to its caller -- for a SPURS
 * policy this is stop-and-signal: it returns up into the kernel/policy
 * service loop, which continues (i.e. the SPU "resumes" past the stop). */
void spu_stop(spu_context* ctx)
{
    ctx->status = SPU_STATUS_STOPPED_BY_STOP;
    spu_stop_exit_check(ctx);
}

/* SPU_FLOAT_RZ=1: SPU code runs with FPCR at round toward zero with flush-to-zero, so the
 * single-precision helpers take their native form (spu_helpers.h, spu__nv_rz). The mode is set
 * on entry to spu_run_with_halt and the caller's FPCR is put back on the way out: a run can
 * happen synchronously on a PPU thread (a workload dispatched from the PPU caller), whose
 * lifted floating point must keep round to nearest with denormals. Double precision restores
 * the default around itself inside a run. arm64 only; elsewhere the exact forms always run.
 * Returns the FPCR to restore, or ~0 when nothing was changed. */
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
    if (v == SPU_FPCR_RZFZ) return ~0ull;   /* nested run: the outer one restores */
    __asm__ volatile("msr fpcr, %0" :: "r"((uint64_t)SPU_FPCR_RZFZ));
    return v;
#else
    return ~0ull;
#endif
}
static void spu_float_mode_leave(uint64_t outer)
{
#if defined(__aarch64__) && defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS)
    if (outer != ~0ull) __asm__ volatile("msr fpcr, %0" :: "r"(outer));
#else
    (void)outer;
#endif
}

/* An external stop (raw SPU SPU_RunCntl = 0) halts the SPU at its next
 * channel operation. The SPU itself never sees the request; the host thread
 * that runs it leaves through the spu_run_with_halt landing pad, which is what
 * the stopping side waits for. */
static void spu_honour_stop_request(spu_context* ctx)
{
    if (!ctx->stop_request) return;
    ctx->status = SPU_STATUS_STOPPED;
    spu_halt(ctx);
}

/* Run a lifted SPU entry with a halt landing pad. Returns 1 if the job halted
 * (via spu_halt), 0 if it returned normally. */
int spu_run_with_halt(void (*entry)(spu_context*), spu_context* ctx)
{
    const uint64_t fpcr_outer = spu_float_mode_enter();   /* not modified after setjmp */
    int halted = 0;
    /* Also discard any saved register file keyed to this context pointer:
     * contexts are stack locals, so the address recurs and a stale save from
     * an earlier run would be restored over this job's arguments. */
    { extern void spu_irq_regs_forget(spu_context*); spu_irq_regs_forget(ctx); }
    ctx->steps = 0;   /* fresh run: step 0 is the entry */
    if (g_spu_direct < 0) spu_direct_init();
    /* The host stack is empty here by definition, so no lifted caller frame
     * is live. A persistent context (a SPURS policy module re-entered every
     * scheduling quantum) otherwise carries the depth its last quantum
     * halted at: two frames per quantum for an idle job manager, and the
     * recursion guard halted the SPU at 2000 after a few minutes of idling. */
    ctx->host_depth = 0;
    ctx->irq_frame = 0;
    ctx->drain_ret_pc = 0;
    /* Nest-safe: a job run synchronously from inside another SPU's execution
     * re-enters here on the same host thread. The halt target is per-thread,
     * so save the outer one and put it back on the way out -- otherwise the
     * outer run's next halt longjmps into this (by then dead) frame, and a
     * stack-local job context skips its coherency unregister (GH3: crashes in
     * spu_coh_notify_write on a freed stack). */
    jmp_buf outer_env; const int outer_armed = s_spu_halt_armed;
    if (outer_armed) memcpy(outer_env, s_spu_halt_env, sizeof(jmp_buf));
    s_spu_halt_armed = 1;
    g_spu_trampoline_fn = 0;                        /* no stale transfer pending */
    /* Lockstep gate (env SPU_LOCKSTEP, default off): join the round-robin
     * ring and BLOCK until this ctx holds the run token, so only one lifted SPU
     * executes at a time. No-op when unarmed. The thread-local halt env above
     * makes a token pause/resume mid-run safe. */
    yz_lockstep_register(ctx);
    switch (SPU_SETJMP(s_spu_halt_env)) {
    case 1:
        halted = 1;
        g_spu_trampoline_fn = 0;
        ctx->irq_frame = 0;
        break;
    case 2:
        /* A one-way guest stack reset invalidates every lifted host caller.
         * Re-enter its target with the same guest state on the driver stack. */
        ctx->host_depth = 0;
        ctx->irq_frame = 0;
        g_spu_trampoline_fn = 0;
        spu_indirect_branch(ctx);
        SPU_DRAIN(ctx);
        break;
    default:
        spu_jmp_no_unwind(s_spu_halt_env);
        /* SPU_DRAIN trampoline model: the top-level entry runs until its first
         * cross-function tail transfer, which sets g_spu_trampoline_fn and
         * returns; the drain loop re-enters each queued target until the SPU
         * halts (stop -> longjmp) or the trampoline empties. Nested brsl/bisl
         * calls drain inside their own call brackets (see the lifter). */
        entry(ctx);
        SPU_DRAIN(ctx);
    }
    yz_lockstep_unregister(ctx);   /* leave the ring; hand the token onward */
    s_spu_halt_armed = outer_armed;
    if (outer_armed) memcpy(s_spu_halt_env, outer_env, sizeof(jmp_buf));
    spu_float_mode_leave(fpcr_outer);
    return halted;
}

/* ===========================================================================
 * One private MFC engine per live virtual context. Persistent policy contexts
 * outnumber physical SPUs even when their scheduling quanta run sequentially.
 * Registry nodes have stable addresses and survive a quantum, including stalled
 * lists and tag state. The registry lock protects lookup/publication/removal;
 * the context's single execution owner protects its engine after lookup.
 * Release is permitted only after that owner has stopped using the context.
 * ===========================================================================*/
typedef struct spu_mfc_slot {
    spu_context* ctx;
    mfc_engine mfc;
    struct spu_mfc_slot* next;
} spu_mfc_slot;

static spu_mfc_slot* s_mfc_slots;
static SRWLOCK s_mfc_claim_lock = SRWLOCK_INIT;

/* SPU_PCSAMPLE=<secs>: every <secs>, print image and pc of every context
 * holding an MFC slot -- i.e. every live SPU that has done DMA. Answers
 * "which SPU task never returned, and where is it" without a debugger. */
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

static mfc_engine* mfc_for(spu_context* ctx)
{
    /* Looked up once. mfc_for runs on every MFC channel access, and with
     * SPU_PCSAMPLE unset `started` never became 1, so every access paid a
     * getenv: a tenth of all samples in a Twisted Metal profile. */
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

    /* A concurrent miss may publish while the read lock is released. Recheck
     * under the writer lock before creating this context's private engine. */
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
        abort(); /* Shared fallback state would silently corrupt DMA completion. */
    }
    slot->ctx = ctx;
    mfc_engine_init(&slot->mfc);
    slot->next = s_mfc_slots;
    s_mfc_slots = slot;
    ReleaseSRWLockExclusive(&s_mfc_claim_lock);
    return &slot->mfc;
}

void spu_mfc_release(spu_context* ctx)
{
    if (!ctx) return;
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

/* Exit safety net for contexts unwound off a departing thread's stack.
 * Pointer comparisons suffice: the abandoned context need not be readable. */
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

/* ===========================================================================
 * Atomic reservation (GETLLAR / PUTLLC / PUTLLUC) -- real lock-line semantics
 *
 * Multiple SPU kernel threads (the SPURS workload runtime runs several SPUs on
 * one shared lock-free queue) issue GETLLAR/PUTLLC on the SAME 128-byte lines.
 * Without honoring the reservation, two SPUs both "claim" the same slot and the
 * queue corrupts (observed: the 2nd claim returns garbage [1,1,1,1] -> the SPU
 * traps). PUTLLC must FAIL when the line changed since GETLLAR. We implement the
 * compare-and-swap under one global lock across all SPU host threads.
 * ===========================================================================*/
extern uint8_t* vm_base;

/* The spinlock guarding all atomic line ops now lives in spu_coherency.c as
 * spu_lockline_lock/unlock. It used to be a file-static here, which serialized
 * SPU against SPU but left the PPU free to store into a line in the middle of
 * a PUTLLC's compare-and-commit -- the update the SPU was about to make would
 * be written over, and the SPU would never learn the line had changed. Same
 * lock, same critical sections, now shared with the PPU store paths. */

/* Lowest LS address treated as "not job code". Jobs load at 0 and
 * their stacks top out well below this; a branch at or above it with no
 * lifted code is a runaway, not a service call. */
#define SPU_JM2_KERNEL_BASE 0x15000u

/* Returns 1 if `cmd` is an atomic line op and was handled here, else 0. */
static int spu_mfc_atomic(spu_context* ctx, uint32_t cmd)
{
    /* Classify FIRST. Every MFC_Cmd write lands here, and only the switch at
     * the bottom used to filter -- so the uncommitted-EA guard below reported
     * ORDINARY DMA as `[spu-atomic]`. A plain put (0x20) to a garbage EA read
     * as an atomic spin, which is a genuinely misleading place to start
     * debugging from. Non-atomic commands belong to the DMA engine. */
    if (cmd != MFC_GETLLAR_CMD && cmd != MFC_PUTLLC_CMD &&
        cmd != MFC_PUTLLUC_CMD && cmd != MFC_PUTQLLUC_CMD)
        return 0;

    uint32_t ea  = ctx->mfc_eal & ~(uint32_t)(MFC_ATOMIC_LINE - 1);
    /* The line moves to and from a 128-byte aligned local-store address: the
     * low seven bits of MFC_LSA are ignored, as those of the EA are. Used
     * unmasked, an unaligned LSA put the line at the wrong place, and one in
     * the last 128 bytes of local store carried the copy past the end of the
     * store and over the context behind it. */
    uint32_t lsa = ctx->mfc_lsa & SPU_LS_MASK & ~(uint32_t)(MFC_ATOMIC_LINE - 1);
    uint8_t* ls  = &ctx->ls[lsa];
    uint8_t* mem = vm_base + ea;


    /* Guard atomic line ops against an uncommitted/garbage EA (e.g. a SPURS
     * policy computing a lock-line address from an incomplete instance context).
     * Same rationale as the DMA EA guard: a bad guest atomic must not segfault
     * the host. GETLLAR returns a zeroed line (no reservation); PUTLLC fails. */
    if (!mfc_ea_valid(ea, MFC_ATOMIC_LINE)) mfc_fault(ctx, "atomic address", cmd, 0, ea, MFC_ATOMIC_LINE);

    switch (cmd) {
    case MFC_GETLLAR_CMD:
        /* Lockstep tick: a guest GETLLAR..PUTLLC poll loop is intra-function
         * (all gotos), so it never crosses the SPU_DRAIN tick site -- without a
         * tick here a polling SPU would hold the run token forever while the
         * peer that must WRITE the line waits for it (canersaka ticks the
         * GETLLAR fast+slow paths for exactly this reason). */
        yz_lockstep_tick(ctx);
        /* A registered holder re-reading an already marked line -- every
         * policy-module poll -- publishes its reservation and copies without
         * the lock; any write the copy misses ends that reservation (see
         * spu_coh_reserve_fast). Anything else takes the lock as before. */
        const int llar_fast = spu_coh_reserve_fast(ctx, ea);
        if (!llar_fast) {
            spu_lockline_lock();
            /* Tell the PPU store paths this line is live, so a store into it goes
             * through the lock and raises SPU_EVENT_LR here instead of landing
             * unannounced. Nothing else about this transaction changes. */
            spu_coh_reserve(ctx, ea);
        }

        /* ONE read of guest memory, then the snapshot from that copy.
         *
         * This used to memcpy from `mem` twice -- once to the local store, once
         * to resv_line -- and a PPU store landing BETWEEN the two reads gave the
         * SPU a stale line and a fresh snapshot. That is a permanent deadlock,
         * and it is the one this port spent a session chasing: the SPU compares
         * its (stale) copy, finds produced == consumed, and sleeps on
         * MFC_LLR_LOST_EVENT; spu_resv_lost_poll then compares memory against a
         * snapshot that ALREADY has the new value, finds no difference, and
         * never raises the event. Measured at a freeze: mirror 0x9E, snapshot
         * 0x9F, memory 0x9F, and the SPU polling rchcnt 1.6 billion times.
         *
         * Hardware cannot produce that state: GETLLAR is a single atomic
         * 128-byte read, so the reserved data and the reservation come from the
         * same instant. Copying the snapshot from `ls` restores that property,
         * and a store that lands after the read now leaves BOTH stale -- which
         * is what makes the lost-reservation event fire. */
        memcpy(ls, mem, MFC_ATOMIC_LINE);              /* line -> local store */
        /* Snapshot from `ls`, NOT a second read of `mem`. Hardware GETLLAR is a
         * single atomic 128-byte read, so the reserved data and the reservation
         * come from the same instant. Reading guest memory twice lets a PPU store
         * land between them, leaving the SPU a STALE line and a FRESH snapshot --
         * and then both halves of the wait fail: the SPU compares its stale copy,
         * sees produced == consumed and sleeps on MFC_LLR_LOST_EVENT, while the
         * reservation poll compares memory against a snapshot that already holds
         * the new value, finds no difference, and never raises the event. Nobody
         * wakes anybody. Snapshotting from `ls` makes a later store leave BOTH
         * stale, which is exactly what makes the lost-reservation event fire. */
        memcpy(ctx->resv_line, ls, MFC_ATOMIC_LINE);   /* snapshot for compare */
        /* MFC_RdAtomicStat after a GETLLAR reads 4 (GETLLAR complete), not 0:
         * Havok's SPU allocator (GH3 collide task, LS 0xA66C) re-issues the
         * GETLLAR until that bit is set, and spun forever on 0. PUTLLUC is 2. */
        if (!llar_fast) { ctx->resv_ea = ea; ctx->resv_valid = 1; }   /* the fast path published it already */
        ctx->atomic_stat = 4;
        if (!llar_fast) spu_lockline_unlock();
        return 1;

    case MFC_PUTLLC_CMD:
        spu_lockline_lock();
        if (ctx->resv_valid && ctx->resv_ea == ea &&
            memcmp(mem, ctx->resv_line, MFC_ATOMIC_LINE) == 0) {
            memcpy(mem, ls, MFC_ATOMIC_LINE);          /* commit local store */
            /* A committing PUTLLC is a line write like any other, so every
             * PEER reservation on it is lost and its SPU takes SPU_EVENT_LR.
             * Silent, this is the same lost update the PPU half was added to
             * close, with an SPU on the writing side: a peer is never told to
             * re-read, so it goes on polling a line it believes it still owns,
             * and a peer parked on RdEventStat sleeps through the commit it
             * was waiting for.
             *
             * Our own reservation is dropped FIRST so the notify does not
             * raise a self-LR: hardware CONSUMES the reservation on a
             * successful PUTLLC, it does not report it lost. Already under the
             * lock-line lock, which is what spu_coh_notify_write expects. */
            ctx->resv_valid = 0;
            spu_coh_notify_write(ea);
            ctx->atomic_stat = 0;                      /* PUTLLC_SUCCESS */
        } else {
            ctx->atomic_stat = 1;                      /* PUTLLC_FAILURE -> retry */
        }
        ctx->resv_valid = 0;                           /* reservation consumed */
        spu_lockline_unlock();
        if (ctx->atomic_stat == 0) {
            spu_rsx_window_check(ea, MFC_ATOMIC_LINE);
        }
        return 1;

    case MFC_PUTLLUC_CMD:
    case MFC_PUTQLLUC_CMD:
        spu_lockline_lock();
        memcpy(mem, ls, MFC_ATOMIC_LINE);              /* unconditional store */
        /* Unconditional, so it invalidates EVERY reservation on the line --
         * this SPU's included, which is why the notify runs before the
         * bookkeeping below rather than after it. A peer left holding a
         * reservation here would commit a PUTLLC against a snapshot this store
         * has already overwritten. */
        spu_coh_notify_write(ea);
        ctx->resv_valid = 0; ctx->atomic_stat = 2;   /* PUTLLUC complete */
        spu_lockline_unlock();
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

/* ===========================================================================
 * Channel write
 * ===========================================================================*/
void spu_wrch(spu_context* ctx, uint32_t channel, u128 value)
{
    uint32_t v = value._u32[0];  /* channel writes use the preferred slot */

    /* The MFC parameter channels only latch a value in the context (see
     * mfc_channel_write); the command write is what reaches the engine. Store
     * them here and skip mfc_for for five or six of the eight or nine writes
     * of every DMA. */
    switch (channel) {
    case MFC_LSA:       ctx->mfc_lsa = v;         return;
    case MFC_EAH:       ctx->mfc_eah = v;         return;
    case MFC_EAL:       ctx->mfc_eal = v;         return;
    case MFC_Size:      ctx->mfc_size = v;        return;
    case MFC_TagID:     ctx->mfc_tag = v & 0x1F;  return;
    case MFC_WrTagMask: ctx->mfc_tag_mask = v;    return;
    default: break;
    }

    if (channel_is_mfc(channel)) {
        /* Atomic line ops (GETLLAR/PUTLLC/...) need real reservation semantics,
         * not the plain GET/PUT the DMA engine would do. */
        if (channel == MFC_Cmd && spu_mfc_atomic(ctx, v))
            return;
        mfc_channel_write(mfc_for(ctx), ctx, channel, v);
        /* MFC tag-status-update EVENT producer (channel-stall milestone). Our
         * DMA completes synchronously, so the instant the program arms tag
         * notification (MFC_WrTagUpdate with a non-zero mode = any/all), the
         * tags in MFC_WrTagMask are already complete -- pend the tag event
         * (bit 0, MFC_TAG_STATUS_UPDATE_EVENT) when the SPU has it enabled, and
         * wake any host thread blocked in rdch SPU_RdEventStat. This is the
         * long-missing producer for event_status (previously only ever cleared
         * via WrEventAck -> RdEventStat waits could never complete). */
        if (channel == MFC_WrTagUpdate && v != 0 && (ctx->event_mask & 0x1u)) {
            SPU_EVENT_SET(ctx, 0x1u);
            spu_ch_wake(ctx);
        }
        return;
    }

    switch (channel) {
    case SPU_WrOutMbox:
        /* A raw SPU's outbound mailbox is one entry deep and wrch stalls while
         * it is full. Buffering instead let two replies queue up, and a PPU
         * that tests bit 0 of the count -- Tornado Outbreak's boot handshake,
         * func_000D5A10 -- read count 2 as empty and spun forever (an
         * intermittent boot hang). Raw SPUs only: their reader is the PPU
         * polling the MMIO window, whereas SPURS-thread mailboxes are drained
         * by paths that do not wake a stalled writer. A raw SPU's local store
         * is the guest window, not its own buffer. */
        if (ctx->ls != ctx->ls_store)
            while (ctx->ch_out_mbox.count >= SPU_MBOX_DEPTH && ctx->status != SPU_STATUS_STOPPED) {
                spu_honour_stop_request(ctx);
                AcquireSRWLockExclusive((SRWLOCK*)&ctx->ch_wait_lock);
                if (ctx->ch_out_mbox.count >= SPU_MBOX_DEPTH && !ctx->stop_request)
                    SleepConditionVariableSRW((CONDITION_VARIABLE*)&ctx->ch_wait_cv,
                                              (SRWLOCK*)&ctx->ch_wait_lock, INFINITE, 0);
                ReleaseSRWLockExclusive((SRWLOCK*)&ctx->ch_wait_lock);
            }
        spu_channel_write(&ctx->ch_out_mbox, v);
        /* Plain mailbox data is consumed by the following interrupt request. */
        break;
    case SPU_WrOutIntrMbox:
        if (g_spu_user_event_hook && g_spu_user_event_hook(ctx, v)) break;
        spu_channel_write(&ctx->ch_out_intr_mbox, v);
        if (g_spu_out_mbox_hook) g_spu_out_mbox_hook(ctx->spu_group_id, ctx->spu_id, 1, v);
        break;
    case SPU_WrDec:          ctx->decrementer = v;
                             ctx->dec_base_ns = spu_host_ns();              break;
    case SPU_WrEventMask:    ctx->event_mask = v;                           break; /* WrEventMask */
    case SPU_WrEventAck:
        /* Under the lock-line lock, because every producer of the LR bit sets
         * it under that lock: the PPU coherent store, and a peer SPU's PUTLLC,
         * PUTLLUC or plain PUT. A bare read-modify-write here can read
         * event_status, have a concurrent |= SPU_EVENT_LR land in between, and
         * write the stale value back. The edge is then gone, and an SPU that
         * has just acknowledged the events it read goes back to sleep believing
         * it still holds a reservation it has already lost, which is the parked
         * SPURS kernel this whole mechanism exists to wake. */
        spu_lockline_lock();
        SPU_EVENT_CLEAR(ctx, v);
        /* A stall-and-notify nobody has read yet stays pending. We run a
         * resumed list inline, inside the MFC_WrListStallAck that resumes it,
         * so its next stall can land BEFORE the handler acknowledges the event
         * it was taken for; on hardware the MFC gets there later, after the
         * ack. Acking then cleared that new edge while the list stayed parked
         * (list_stall_stat set, Sn clear). */
        if ((v & 0x2u) && ctx->list_stall_stat)
            SPU_EVENT_SET(ctx, 0x2u);
        spu_lockline_unlock();
        break;
    case SPU_WrSRR0:         ctx->srr0 = v & 0x3FFFCu;   /* a word-aligned LS address */ break;
    default:
        /* Unknown / unhandled channel write -- ignore (matches a no-op SPU). */
        break;
    }
}

/* ===========================================================================
 * Channel-stall contract (faithful-adopt, from canersaka's fork).
 *
 * A blocking spu_rdch on an empty read channel parks the SPU host thread on a
 * per-SPU CV until a producer (mailbox/signal write, event raise) calls
 * spu_ch_wake -- never fabricating a value. Under SPU_LOCKSTEP the wait
 * releases the run token first (block_begin) so a peer SPU that must post the
 * awaited data is never blocked on this ctx. The producers (mailbox/signal writes, the MFC
 * tag-status and lock-line-lost event raises) all wake, and a read that returns
 * instead of stalling is not what the SPU does -- the SPURS kernel idles in
 * `rdch SPU_RdEventStat` waiting for its lock line to change, and returning at
 * once turns every idle kernel into a getllar busy-loop through the lock-line
 * lock that starves the SPU and PPU threads doing work.
 * ===========================================================================*/
static void spu_resv_lost_poll(spu_context* ctx)
{
    if (!(ctx->event_mask & 0x400u) || !ctx->resv_valid) return;
    if (!vm_base || ctx->resv_ea == 0) return;
    if (memcmp(vm_base + ctx->resv_ea, ctx->resv_line, 128) != 0) {
        ctx->resv_valid = 0;
        SPU_EVENT_SET(ctx, 0x400u);
    }
}

static int spu_ch_ready(spu_context* ctx, uint32_t channel)
{
    if (channel == SPU_RdEventStat) spu_resv_lost_poll(ctx);
    switch (channel) {
    case SPU_RdInMbox:      return ctx->rcv_evt_n != 0 || ctx->ch_in_mbox.count != 0;
    case SPU_RdSigNotify1:  return ctx->ch_sig_notify[0].count != 0;
    case SPU_RdSigNotify2:  return ctx->ch_sig_notify[1].count != 0;
    case SPU_RdEventStat:   return (ctx->event_status & ctx->event_mask) != 0;
    default:                return 1;   /* non-blocking channels: always ready */
    }
}

/* Signal the per-SPU wait CV so a blocked spu_rdch re-checks its predicate.
 * MUST be called by every producer AFTER it makes a read predicate true. */
void spu_ch_wake(spu_context* ctx)
{
    if (!ctx) return;
    /* Pass through the waiter's lock first. spu_ch_wait tests its predicate and
     * goes to sleep under ch_wait_lock, but a producer makes the predicate true
     * outside it. A wake issued between the waiter's test and its sleep found
     * nobody on the condition variable and was lost (on POSIX the shim's
     * generation count is read when the sleep begins, so it misses it too);
     * the read then completed only at the 10 ms re-poll. Taking the lock orders
     * this wake after the waiter is either asleep or has not yet tested.
     * Uncontended it is one lock/unlock pair. No caller holds ch_wait_lock, and
     * the waiter takes no other lock while it does. */
    AcquireSRWLockExclusive((SRWLOCK*)&ctx->ch_wait_lock);
    ReleaseSRWLockExclusive((SRWLOCK*)&ctx->ch_wait_lock);
    WakeAllConditionVariable((CONDITION_VARIABLE*)&ctx->ch_wait_cv);
}

/* PS3_FTRACE: when this SPU thread last left a blocking wait. The time between
 * two waits is a busy span, tagged with the lifted image and the SPURS workload
 * id the kernel keeps at LS 0x1DC (SpursKernelContext.wklCurrentId) when it
 * goes idle. Waits shorter than PS3_FTRACE_MIN_US do not split a busy span. */
static SPU_TLS uint64_t t_ft_busy0;

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

/* Block the calling SPU host thread until `channel` is readable. */
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
    yz_lockstep_block_begin(ctx);          /* release the run token before the OS wait */
    while (!spu_ch_ready(ctx, channel)) {
        if (ctx->stop_request) {
            yz_lockstep_block_end(ctx);
            spu_honour_stop_request(ctx);
        }
        AcquireSRWLockExclusive((SRWLOCK*)&ctx->ch_wait_lock);
        if (!spu_ch_ready(ctx, channel) && !ctx->stop_request)
            SleepConditionVariableSRW((CONDITION_VARIABLE*)&ctx->ch_wait_cv,
                                      (SRWLOCK*)&ctx->ch_wait_lock, INFINITE, 0);
        ReleaseSRWLockExclusive((SRWLOCK*)&ctx->ch_wait_lock);
    }
    yz_lockstep_block_end(ctx);            /* rejoin the rotation, reacquire the token */
    if (ft_t0) spu_ft_wait_span(ctx, channel, ft_t0);
    ctx->status = SPU_STATUS_RUNNING;
}

/* ===========================================================================
 * Channel read (returns value in the preferred word slot)
 * ===========================================================================*/
u128 spu_rdch(spu_context* ctx, uint32_t channel)
{
    spu_honour_stop_request(ctx);
    /* Block (never fabricate) on an empty producer-fed read channel.
     * RdEventStat now has a producer (the MFC tag-status event
     * raise above), but only block it when the SPU has actually enabled events
     * (event_mask != 0) -- a masked-off read must return 0 immediately, not
     * park forever. RdInMbox/RdSigNotify park on their PPU producers. */
    if (channel == SPU_RdInMbox || channel == SPU_RdSigNotify1 || channel == SPU_RdSigNotify2)
        spu_ch_wait(ctx, channel, "rdch");
    else if (channel == SPU_RdEventStat && ctx->event_mask != 0)
        spu_ch_wait(ctx, channel, "rdch");

    uint32_t v = 0;

    if (channel_is_mfc(channel)) {
        v = mfc_channel_read(mfc_for(ctx), ctx, channel);
        return spu_make_preferred_u32(v);
    }

    switch (channel) {
    case SPU_RdInMbox:
        if (ctx->rcv_evt_n > 0) { v = ctx->rcv_evt[ctx->rcv_evt_i++]; ctx->rcv_evt_n--; }
        else v = spu_channel_read(&ctx->ch_in_mbox);
        break;
    case SPU_RdSigNotify1:  v = spu_channel_read(&ctx->ch_sig_notify[0]); break;
    case SPU_RdSigNotify2:  v = spu_channel_read(&ctx->ch_sig_notify[1]); break;
    /* The decrementer must actually DECREMENT. Returning the latched WrDec
     * value verbatim makes every `rdch $ch8` deadline poll spin forever: the
     * elapsed-time term is always zero, so the deadline never passes. LBP's
     * wwsjob SPURS policy module hangs on exactly that at LS 0x2D68:
     *
     *     2d6c:  lqa   $2, 0x1530     ; deadline
     *     2d74:  rdch  $5, $ch8       ; now  <- always read 0
     *     2d78:  sf    $6, $2, $5     ; now - deadline
     *     2d80:  cgti  $5, $6, 0
     *     2d88:  binz  $2, $0         ; leave once it goes positive
     *
     * It never leaves, never reaches its first DMA, and blocks the workload.
     * Wraparound on subtract is deliberate -- it is what hardware does, and the
     * `cgti > 0` test above is written to tolerate it. */
    case SPU_RdDec: {
        /* Never armed by a WrDec: stamp the base on first read rather than
         * measuring from epoch 0, which would subtract the host's entire
         * uptime and hand back a wild value to code that has every right to
         * read the decrementer before writing it. */
        if (!ctx->dec_base_ns) ctx->dec_base_ns = spu_host_ns();
        uint64_t elapsed = spu_host_ns() - ctx->dec_base_ns;
        /* Split seconds from the remainder before scaling: elapsed_ns * 79.8M
         * overflows 64 bits 231 s after the base stamp, and a context that
         * never writes its decrementer (the SPURS kernel's) keeps the stamp
         * from boot, so every timed loop on that SPU saw the counter jump
         * forward by ~16 s of ticks at 231 s, 462 s, 693 s... */
        const uint64_t secs = elapsed / 1000000000ull, rem = elapsed % 1000000000ull;
        uint32_t ticks   = (uint32_t)(secs * SPU_DECREMENTER_HZ +
                                      (rem * SPU_DECREMENTER_HZ) / 1000000000ull);
        v = ctx->decrementer - ticks;
        break;
    }
    case SPU_RdEventMask:   v = ctx->event_mask;                        break;
    /* The events that are pending AND enabled. A pending event the SPU has
     * not unmasked (an Lr left by a lost reservation, an Sn) must not show up
     * in a read that is waiting on a different set; this is the condition
     * spu_ch_ready and rchcnt already use. */
    case SPU_RdEventStat:   spu_resv_lost_poll(ctx); v = ctx->event_status & ctx->event_mask; break;
    /* CBEA 9.8: numeric bit 0 is interrupt-enable, independent of run status.
     * This runtime models nonisolated SPUs, so the isolation bit stays clear. */
    case SPU_RdMachStat:    v = ctx->int_enable & 1u; break;
    case SPU_RdSRR0:        v = ctx->srr0;                              break;
    default:
        v = 0;
        break;
    }
    return spu_make_preferred_u32(v);
}

/* ===========================================================================
 * Channel count (rchcnt) -- how many entries can be read/written right now
 * ===========================================================================*/
uint32_t spu_rchcnt(spu_context* ctx, uint32_t channel)
{
    spu_honour_stop_request(ctx);
    switch (channel) {
    case SPU_RdInMbox:
        /* Synchronous persistent-worker park: after its handshake the SPU polls
         * the inbound mailbox for PPU commands; if empty and parking is armed,
         * halt (longjmp out of spu_run_with_halt) instead of spinning forever. */
        /* A pending sys_spu_thread_receive_event reply counts as readable
         * mailbox words: the worker checks rchcnt to decide whether its reply
         * has arrived, and parking on an "empty" inbox that actually holds the
         * reply strands it. */
        if (ctx->park_on_empty_inmbox && ctx->rcv_evt_n == 0 &&
            ctx->ch_in_mbox.count == 0) {
            extern void spu_halt(spu_context*);
            spu_halt(ctx);
        }
        return (uint32_t)ctx->rcv_evt_n + ctx->ch_in_mbox.count;           /* readable */
    /* Free slots, CLAMPED. The queue behind each channel is SPU_CHANNEL_CAP
     * deep -- deeper than the hardware depth reported here -- so `count` can
     * exceed the depth and a bare subtraction underflows, telling the SPU it
     * has four billion free slots. That silently converts a bounded mailbox
     * into an unbounded one for any title that polls before writing, which is
     * every SPU-heavy one. */
    case SPU_WrOutMbox:
        return ctx->ch_out_mbox.count >= SPU_MBOX_DEPTH
             ? 0u : (uint32_t)(SPU_MBOX_DEPTH - ctx->ch_out_mbox.count);
    case SPU_WrOutIntrMbox:
        return ctx->ch_out_intr_mbox.count >= SPU_INTR_MBOX_DEPTH
             ? 0u : (uint32_t)(SPU_INTR_MBOX_DEPTH - ctx->ch_out_intr_mbox.count);
    case SPU_RdSigNotify1:   return ctx->ch_sig_notify[0].count;
    case SPU_RdSigNotify2:   return ctx->ch_sig_notify[1].count;
    case MFC_Cmd:            return MFC_QUEUE_DEPTH - mfc_for(ctx)->queue_count;
    /* An enabled event pending, or 0 -- NOT the `default: 1` this used to fall
     * through to. The SPU idiom is `rchcnt SPU_RdEventStat; brnz -> rdch`: it
     * asks whether an event is pending and only commits to the BLOCKING read if
     * the answer is yes. Answering 1 unconditionally lured it into a read that
     * then parked forever, because rdch correctly blocks while
     * (event_status & event_mask) == 0 -- rchcnt and rdch disagreed.
     *
     * ps1_netemu's audio SPU does exactly this at LS 0x8934 and parked at
     * 0x0A5E8 waiting for MFC_LLR_LOST_EVENT on a line nothing was ever going
     * to touch; on hardware it would simply have fallen through to 0x893C and
     * kept working. Same condition as spu_ch_ready's case, including the
     * lost-reservation poll, so the two now agree by construction. */
    case SPU_RdEventStat:
        spu_resv_lost_poll(ctx);
        return (ctx->event_status & ctx->event_mask) != 0;
    case MFC_RdTagStat:      return 1;  /* synchronous: status always ready */
    /* Stall-and-notify status is a single-value channel: count is 1 while a
     * newly-stalled tag is pending to be read, else 0. The wwsjob interrupt
     * handler gates on this (rchcnt $ch25; brz IhcExit) -- returning the old
     * default of 1 made it enter on spurious interrupts with an empty stat,
     * running IhcLoop on a zero mask (clz(0) -> bogus tagId/ack). */
    case MFC_RdListStallStat: return ctx->list_stall_stat ? 1 : 0;
    default:                 return 1;  /* default: channel ready */
    }
}

/* ===========================================================================
 * Indirect-branch dispatch + function registry
 * ===========================================================================*/
typedef void (*spu_fn)(spu_context*);

typedef struct {
    uint32_t addr;
    spu_fn   fn;
    int      image_id;   /* which recompiled image this function belongs to */
} spu_reg_entry;

/* One entry per lifted SPU function across EVERY registered image, and a game
 * that lifts its whole SPU workload set has a lot of them: Yakuza: Dead Souls
 * registers ~170k (cri_audio alone is ~35k, gs_task ~9k, the job binaries ~11k).
 * At 65536 the registry silently truncated every image registered past the cap
 * -- and the SPURS job-chain policy, the job binaries and the Edge geometry
 * task (gs_task) register LAST, so their functions were dropped wholesale and
 * every indirect branch into them fell through to a branch-to-0. Size it for
 * the real workload; the overflow is now loud (see spu_register_function). */
#define SPU_FN_REGISTRY_MAX 262144
static spu_reg_entry s_registry[SPU_FN_REGISTRY_MAX];
static uint32_t s_registry_count = 0;

/* Accessor for the sampling profiler in ppu_loader.cpp. Its address->function
 * map is built from the PPU table alone, which leaves the SPU-lifted bodies as
 * unowned gaps -- and an [entry, next_entry) extent then charges every SPU
 * sample to whichever PPU function happens to precede it. That produced a
 * confident and completely wrong "99% of guest time in one PPU function".
 * Handing the SPU host pointers over lets the map cover both, so the gaps are
 * real entries and the extents mean something. */
uint32_t spu_registry_size(void) { return s_registry_count; }

int spu_registry_entry(uint32_t i, void** host, uint32_t* ls_addr)
{
    if (i >= s_registry_count) return 0;
    if (host)    *host    = (void*)s_registry[i].fn;
    if (ls_addr) *ls_addr = s_registry[i].addr;
    return 1;
}

/* Hash index over the registry. spu_lookup runs on EVERY guest indirect
 * branch -- with the Bink decoder live that is millions of dispatches per
 * second, and the old linear scan (~950 binkspu entries walked per branch)
 * dominated movie playback. Buckets chain in REGISTRATION ORDER (tail
 * append) so the first-registered-match-wins semantics of the linear scan
 * are preserved exactly; the image-id wildcard rules stay in the bucket
 * walk, which is 1-3 entries (same LS addr across overlapping images).
 * Registration is startup-single-threaded; lookups treat the index as
 * read-only. Chain links store index+1 so zero-init means "empty". */
#define SPU_FN_HASH_SIZE 131072   /* power of two, ~= MAX/2 -> load factor ~2 */
static uint32_t s_hash_head[SPU_FN_HASH_SIZE];
static uint32_t s_hash_tail[SPU_FN_HASH_SIZE];
static uint32_t s_hash_next[SPU_FN_REGISTRY_MAX];

static inline uint32_t spu_fn_hash(uint32_t addr)
{
    return ((addr >> 2) * 2654435761u) & (SPU_FN_HASH_SIZE - 1);
}

/* Image currently being registered. SPURS images (kernel/policy/job) overlap in
 * LS, so each registers under a distinct id via spu_begin_image() before calling
 * its (prefixed) spu_recomp_register(). Single-image callers leave it 0. */
static int s_reg_image = 0;
void spu_begin_image(int image_id) { s_reg_image = image_id; }

void spu_register_function(uint32_t addr, spu_fn fn)
{
    if (s_registry_count >= SPU_FN_REGISTRY_MAX) {
        /* Silent truncation here dropped whole late-registered SPU images and
         * cost a multi-round hunt (the branch-to-0 looked like a lifter/overlay
         * bug). Never again: say so, once, loudly, with the number to raise the
         * cap to. */
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
    /* Published after the entry is linked: the dispatch cache keys its entries
     * by this count, so a lookup that sees the new count also finds the entry. */
    __atomic_store_n(&s_registry_count, i + 1, __ATOMIC_RELEASE);
}

/* Report a CROSS-IMAGE match: the registry served a function some other image
 * registered at this address, because one side's image_id was the 0 wildcard.
 *
 * This is a real hazard, reported with evidence by @canersaka (#59): SPURS job
 * chains load job binaries into descriptor-assigned LS slots, so the same binary
 * can sit at different addresses on different rounds. If the lift was fixed-base
 * and the exact match misses, the wildcard can serve ANOTHER image's function at
 * that address -- the job then runs the wrong program end to end, returns
 * cleanly, and the only symptom is something three layers downstream that never
 * happens. Their words: one log line would have saved a day.
 *
 * Logging, not refusing. Refusing the wildcard outright was measured and does
 * NOT survive contact: a dormant task image can register spans overlapping the
 * resident kernel's low LS range, so a legitimate service-to-kernel yield gets
 * falsely rejected. Telling resident from dormant needs residency tracking the
 * registry does not have. So this reports and keeps going.
 *
 * Deduped per (addr, from, to): spu_lookup runs millions of times a second, and
 * a substitution that repeats every dispatch would bury the log it is meant to
 * make readable. */
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

spu_fn spu_lookup(uint32_t addr, int image_id)   /* exported: clang-built fast-path dispatch (spu_dispatch_mt.c) needs it */
{
    /* Match the context's active image; image_id 0 (context or entry) matches
     * any, for back-compat with single-image contexts. */
    for (uint32_t n = s_hash_head[spu_fn_hash(addr)]; n; n = s_hash_next[n - 1]) {
        const spu_reg_entry* e = &s_registry[n - 1];
        if (e->addr == addr &&
            (image_id == 0 || e->image_id == 0 || e->image_id == image_id)) {
            /* One compare on the return path; the report itself is rare. */
            if (e->image_id != image_id)
                spu_report_cross_image(addr, image_id, e->image_id);
            return e->fn;
        }
    }
    return NULL;
}

/* Indirect dispatch resolves the same (address, image) pairs on every guest
 * indirect branch. Each resolution walks the address's hash chain -- one entry
 * per image registered there, which reaches dozens where streamed job modules
 * share one code buffer. Memoize the result per host thread. Registration only
 * appends, so the registry count is a complete generation: any new function
 * invalidates every cached entry, including cached misses. The generation is
 * read before the walk, so a registration that races the walk leaves the entry
 * stale and the next lookup redoes it. */
typedef struct { uint32_t addr, gen; int image; spu_fn fn; } spu_lookup_memo;
#define SPU_LOOKUP_MEMO_SIZE 512   /* power of two */
static SPU_TLS spu_lookup_memo t_lookup_memo[SPU_LOOKUP_MEMO_SIZE];
static spu_fn spu_lookup_memoized(uint32_t addr, int image_id)
{
    spu_lookup_memo* m = &t_lookup_memo[((addr >> 2) ^ ((uint32_t)image_id * 0x9E3779B1u)) &
                                        (SPU_LOOKUP_MEMO_SIZE - 1)];
    const uint32_t gen = s_registry_count + 1;   /* +1: zero-init never matches */
    if (m->gen != gen || m->addr != addr || m->image != image_id) {
        m->fn = spu_lookup(addr, image_id);
        m->addr = addr; m->image = image_id; m->gen = gen;
    }
    return m->fn;
}

/* Which image registered a function at this LS address, or -1 if none did.
 *
 * Deliberately NOT spu_lookup(addr, 0): that call means "dispatch here in a
 * context with no image", so it reports a cross-image substitution for every
 * hit in a real image. This is a question about the registry, not a dispatch,
 * and it runs once per SPU thread start. First match wins, in registration
 * order, so it agrees with which function spu_lookup would actually serve. */
int spu_image_of_function(uint32_t addr)
{
    for (uint32_t n = s_hash_head[spu_fn_hash(addr)]; n; n = s_hash_next[n - 1]) {
        const spu_reg_entry* e = &s_registry[n - 1];
        if (e->addr == addr) return e->image_id;
    }
    return -1;
}

/* Does a lifted function exist at this LS address (any image)? Lets the lv2
 * layer decide between real SPU execution and the PPU-fallback paths. */
int spu_have_function(uint32_t addr)
{
    return spu_image_of_function(addr) >= 0;
}

/* Shared by indirect dispatch and interpreter rejoin. A bounded resident
 * code owner is authoritative even if it has no lift at this LSA: falling
 * through would execute stale code registered under another image. Task and
 * unbounded overlay misses retain their existing primary-image fallback. */
static int spu_overlay_resident_verified(spu_context* ctx, unsigned slot);
static spu_lifted_fn spu_lifted_lookup_full(const spu_context* ctx, uint32_t lsa, int* reloc)
{
    /* Every probe below goes through #436's per-thread memo rather than
     * spu_lookup() directly: the resolver makes up to three of them per
     * resolution and the first two usually miss, which is exactly the case a
     * generation-keyed memo answers without walking the hash chain again. */
    if (!ctx) return (spu_lifted_fn)spu_lookup_memoized(lsa, 0);
    for (unsigned slot = 0; slot < SPU_RESIDENT_CODE_SLOTS; ++slot)
        if (ctx->resident_code[slot].image_id &&
            lsa >= ctx->resident_code[slot].lsa &&
            lsa - ctx->resident_code[slot].lsa < ctx->resident_code[slot].size) {
            /* A region registered with its code bytes owns its span even when
             * unverified: other images' functions there would be stale. */
            if (ctx->resident_code[slot].verify_state &&
                !spu_overlay_resident_verified((spu_context*)ctx, slot)) return NULL;
            const uint32_t canon = ctx->resident_code[slot].canon;
            if (!canon)
                return (spu_lifted_fn)spu_lookup_memoized(lsa, ctx->resident_code[slot].image_id);
            /* Relocatable code (spu_overlay_register_region_reloc): its lift sits at
             * canon + offset. The caller runs the function next, so set its delta now. */
            spu_fn f = spu_lookup_memoized(lsa - ctx->resident_code[slot].lsa + canon,
                                           ctx->resident_code[slot].image_id);
            if (f) {
                ((spu_context*)ctx)->reloc_delta = (int32_t)(ctx->resident_code[slot].lsa - canon);
                if (reloc) *reloc = 1;
            }
            return (spu_lifted_fn)f;
        }
    spu_fn fn = NULL;
    if (ctx->resident_ovl) fn = spu_lookup_memoized(lsa, ctx->resident_ovl);
    if (!fn) fn = spu_lookup_memoized(lsa, ctx->image_id);
    return (spu_lifted_fn)fn;
}

/* ---- Dispatch cache ------------------------------------------------------
 * Indirect dispatch and interpreter rejoin resolve the same few targets
 * millions of times a second, and each resolution re-derives ownership: the
 * resident code slots (verifying a pending load), then the overlay and
 * primary registries, each a hash-chain walk. The answer changes only when one
 * of its inputs does, so remember it per host thread, keyed by everything the
 * resolver reads:
 *  - the target and the context's image and resident overlay;
 *  - the registry, by its append-only count (any registration drops every
 *    entry);
 *  - the context's resident code slots, by dcache_residency, which
 *    spu_overlay_note_get (their only runtime writer) bumps on every change;
 *  - the context's lifetime, by dcache_id: a context re-initialised at the
 *    same address starts with id 0 and takes a fresh one.
 * A verified slot is not re-compared, as without the cache (verify once per
 * load); a store by SPU code into a verified span is not tracked, the same as
 * there. Misses (NULL) are not cached, so a rejected or unlifted target is
 * resolved every time, as before. */
typedef struct {
    spu_lifted_fn fn;
    uint32_t id, lsa, rgen, residency;
    int image, ovl;
    /* A relocatable resolution also sets ctx->reloc_delta, which a hit must
     * repeat: residency fixes the placement, so the delta is part of the answer. */
    int reloc;
    int32_t reloc_delta;
} spu_dcache_ent;
#define SPU_DCACHE_ENTRIES 512u
static SPU_TLS spu_dcache_ent t_dcache[SPU_DCACHE_ENTRIES];
static uint32_t s_dcache_next_id;
static void spu_dcache_residency_changed(spu_context* ctx) { ctx->dcache_residency++; }
spu_lifted_fn spu_lifted_lookup(const spu_context* ctx, uint32_t lsa)
{
    if (!ctx) return spu_lifted_lookup_full(ctx, lsa, NULL);
    spu_context* c = (spu_context*)ctx;
    uint32_t id = c->dcache_id;
    if (!id) {
        do id = __atomic_add_fetch(&s_dcache_next_id, 1, __ATOMIC_RELAXED); while (!id);
        c->dcache_id = id;
    }
    /* Read before the resolver walks the registry: an entry is never keyed by
     * a count newer than the registry it saw. */
    const uint32_t rgen = __atomic_load_n(&s_registry_count, __ATOMIC_ACQUIRE);
    spu_dcache_ent* e = &t_dcache[((lsa >> 2) ^ (id * 0x9E3779B1u)) & (SPU_DCACHE_ENTRIES - 1)];
    if (e->fn && e->id == id && e->lsa == lsa && e->rgen == rgen &&
        e->residency == c->dcache_residency && e->image == c->image_id &&
        e->ovl == c->resident_ovl) {
        if (e->reloc) c->reloc_delta = e->reloc_delta;
        return e->fn;
    }
    int reloc = 0;
    const spu_lifted_fn fn = spu_lifted_lookup_full(ctx, lsa, &reloc);
    if (fn) {
        /* The resolver may have verified a pending slot; the entry records the
         * residency it resolved against, read after it. */
        e->fn = fn; e->id = id; e->lsa = lsa; e->rgen = rgen;
        e->residency = c->dcache_residency; e->image = c->image_id;
        e->ovl = c->resident_ovl;
        e->reloc = reloc; e->reloc_delta = c->reloc_delta;
    }
    return fn;
}

/* ---- Swappable code overlays (see spu_context.resident_ovl) --------------
 * A title registers each runtime-streamed overlay's SOURCE content EA with
 * the image id its lifted functions were registered under. The MFC GET path
 * marks that overlay resident in the streaming context; dispatch retries a
 * primary-image miss against the resident overlay's registry. */
typedef struct { uint32_t src_ea; int image_id; uint8_t sig[16]; int has_sig; uint32_t span;
                 uint32_t load_lsa; unsigned code_count; spu_overlay_code_segment* code;
                 uint32_t canon; } spu_ovl_src;
/* 6 FMOD codec/DSP overlays + up to 89 WWS job-code modules (all stream into
 * the same job code buffer at LS 0x4000, dispatched by content signature). */
#define SPU_OVL_SRC_MAX 128
static spu_ovl_src s_ovl_src[SPU_OVL_SRC_MAX];
static int s_ovl_src_count = 0;
/* Monotonic: published before any signature entry becomes visible through
 * s_ovl_src_count. A reader that acquired that count cannot miss the flag.
 * This lets EA-only registries reject unrelated GETs of every size. */
static atomic_int s_ovl_has_sig;

/* Every MFC GET is checked against the registered sources, and a job that
 * streams its data in small pieces issues thousands of GETs a frame against
 * up to SPU_OVL_SRC_MAX sources. A GET under 512 bytes can only match a
 * source by EA (signature entries need >= 512 bytes), so keep every
 * registered source EA in an insert-only open-addressed set and skip the
 * scan for a small GET whose EA was never registered.
 *
 * The set is a superset of the EA-keyed entries the scan can see: it has
 * four times as many slots as there can be sources (so an insert always
 * finds a slot, and a probe always reaches an empty one), nothing is ever
 * removed, and an EA is inserted before the source count that publishes its
 * entry is release-stored. spu_overlay_note_get acquire-loads the count
 * before probing, so any entry the scan would visit is already in the set. */
#define SPU_OVL_EA_SET 512u
static uint32_t s_ovl_ea_set[SPU_OVL_EA_SET];
static uint8_t s_ovl_ea_used[SPU_OVL_EA_SET];
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
        return;
    }
}
static int spu_ovl_ea_known(uint32_t ea)
{
    for (unsigned i = spu_ovl_ea_slot(ea), n = 0; n < SPU_OVL_EA_SET; ++n, i = (i + 1) & (SPU_OVL_EA_SET - 1)) {
        if (!__atomic_load_n(&s_ovl_ea_used[i], __ATOMIC_ACQUIRE)) return 0;
        if (s_ovl_ea_set[i] == ea) return 1;
    }
    return 1;   /* full (cannot happen): never filter */
}

void spu_overlay_register_source(uint32_t content_ea, int image_id)
{
    if (s_ovl_src_count < SPU_OVL_SRC_MAX) {
        spu_ovl_ea_add(content_ea);
        s_ovl_src[s_ovl_src_count].src_ea = content_ea;
        s_ovl_src[s_ovl_src_count].image_id = image_id;
        s_ovl_src[s_ovl_src_count].has_sig = 0;
        s_ovl_src[s_ovl_src_count].canon = 0;
        __atomic_store_n(&s_ovl_src_count, s_ovl_src_count + 1, __ATOMIC_RELEASE);
    }
}

/* Register a bounded code image that may coexist with other streamed images.
 * Its functions must be translated at the local-store addresses used by the title. */
void spu_overlay_register_region(uint32_t content_ea, uint32_t span, int image_id)
{
    if (!span || span > SPU_LS_SIZE || s_ovl_src_count >= SPU_OVL_SRC_MAX) return;
    spu_overlay_register_source(content_ea, image_id);
    s_ovl_src[s_ovl_src_count - 1].span = span;
}

/* And for position-independent code the title copies to a different LS address at each
 * load (GoW:A's BatchJobList jobs place four EBOOT blobs at a new 16-byte-aligned address
 * every time): lifted ONCE at a canonical base with spu_lifter.py --reloc. A GET of the
 * content at any address makes that copy resident; dispatch then looks the function up at
 * canon + offset and sets ctx->reloc_delta, which the lifted code adds to every address
 * it derives from its own pc. */
void spu_overlay_register_region_reloc(uint32_t content_ea, uint32_t span, uint32_t canon, int image_id)
{
    if (!canon || canon >= SPU_LS_SIZE || span > SPU_LS_SIZE - canon) return;
    const int n = s_ovl_src_count;
    spu_overlay_register_region(content_ea, span, image_id);
    if (s_ovl_src_count == n + 1) s_ovl_src[n].canon = canon;
}

/* Residency proves which load happened, not what the bytes are: the source EA
 * could hold different content than was lifted. Compare the lifted segments
 * once per load, at the first dispatch into the span, instead of at every
 * transfer. Any later GET into the span resets the slot to pending (contiguous
 * continuation) or clears it (anything else), see spu_overlay_note_get. */
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
static struct { uint32_t ea, lsa, span; int image_id; } s_ovl_at[SPU_OVL_AT_MAX];
static int s_ovl_at_count;
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
    __atomic_store_n(&s_ovl_at_count, n + 1, __ATOMIC_RELEASE);
}

static int spu_overlay_resident_verified(spu_context* ctx, unsigned slot)
{
    int* state = &ctx->resident_code[slot].verify_state;
    if (*state == SPU_OVERLAY_PENDING) {
        const spu_ovl_src* o = &s_ovl_src[ctx->resident_code[slot].source_index];
        const uint32_t canon = ctx->resident_code[slot].canon;
        int match = 1;
        for (unsigned i = 0; match && i < o->code_count; ++i) {
            const uint32_t at = canon ? ctx->resident_code[slot].lsa + (o->code[i].lsa - canon) : o->code[i].lsa;
            match = at <= SPU_LS_SIZE - o->code[i].size &&
                    !memcmp(ctx->ls + at, o->code[i].bytes, o->code[i].size);
        }
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
    atomic_store_explicit(&s_ovl_has_sig, 1, memory_order_release);
    __atomic_store_n(&s_ovl_src_count, s_ovl_src_count + 1, __ATOMIC_RELEASE);
    return 1;
}

static int spu_overlay_resolve_reloc(spu_context* ctx, uint32_t pc)
{
    const int nsrc = __atomic_load_n(&s_ovl_src_count, __ATOMIC_ACQUIRE);
    uint32_t max_span = 0;
    for (int i = 0; i < nsrc; i++)
        if (s_ovl_src[i].canon && s_ovl_src[i].code && s_ovl_src[i].span > max_span)
            max_span = s_ovl_src[i].span;
    if (!max_span) return 0;
    const uint32_t lowest = pc >= max_span ? pc - max_span + 1 : 0;
    for (uint32_t base = pc & ~15u; ; base -= 16) {
        for (int i = 0; i < nsrc; i++) {
            const spu_ovl_src* o = &s_ovl_src[i];
            if (!o->canon || !o->code || pc - base >= o->span || base > SPU_LS_SIZE - o->span ||
                memcmp(ctx->ls + base, o->sig, 16) ||
                memcmp(ctx->ls + base, o->code[0].bytes, o->code[0].size))
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

/* Register one-way runtime entries that replace the guest call stack.
 * Matching the resolved function, not just its LS address, avoids affecting
 * unrelated overlays using the same address. Entries have one registration
 * writer and are immutable after publication. */
static struct { uint32_t entry; int image_id; } s_stack_reset[16];
unsigned g_spu_stack_reset_count;
/* Nonzero at (entry >> 2) & 1023 of every registered entry, so SPU_DIRECT
 * (spu_context.h) can tell with one load that a transfer needs the drain's
 * stack-reset check. Collisions only cost a trampoline step. */
uint8_t g_spu_stack_reset_hint[1024];
void spu_register_stack_reset_entry(uint32_t entry, int image_id)
{
    /* Registrations have one writer; publish a complete new slot before a
     * reader observes its count, including registration during a live run.
     * The hint byte is written before that release store for the same reason:
     * a reader that sees the new count must also see the hint. */
    unsigned n = __atomic_load_n(&g_spu_stack_reset_count, __ATOMIC_ACQUIRE);
    if (n < 16) {
        s_stack_reset[n].entry = entry & SPU_LS_MASK;
        s_stack_reset[n].image_id = image_id;
        g_spu_stack_reset_hint[((entry & SPU_LS_MASK) >> 2) & 1023u] = 1;
        __atomic_store_n(&g_spu_stack_reset_count, n + 1, __ATOMIC_RELEASE);
    }
}

#if defined(_MSC_VER)
static __declspec(noinline) void spu_check_stack_reset_scan(spu_context*, void (*)(spu_context*));
#else
static __attribute__((noinline, cold)) void spu_check_stack_reset_scan(spu_context*, void (*)(spu_context*));
#endif
/* Also called before direct trampoline transfers: generated calls can enter
 * a stack-switching routine without going through spu_indirect_branch. */
void spu_check_stack_reset(spu_context* ctx, void (*fn)(spu_context*))
{
    /* Runs on every drain transfer: test the registry before the
     * thread-local halt state, which is a function call on Darwin, and keep
     * the scan out of line so the common return needs no frame. */
    if (!spu_stack_reset_needed(ctx)) return;
    spu_check_stack_reset_scan(ctx, fn);
}

static void spu_check_stack_reset_scan(spu_context* ctx, void (*fn)(spu_context*))
{
    if (!s_spu_halt_armed) return;
    const unsigned count = __atomic_load_n(&g_spu_stack_reset_count, __ATOMIC_ACQUIRE);
    for (unsigned i = 0; i < count; ++i)
        if (ctx->pc == s_stack_reset[i].entry &&
            fn == spu_lookup(ctx->pc, s_stack_reset[i].image_id)) {
            g_spu_trampoline_fn = 0;
            SPU_LONGJMP(s_spu_halt_env, 2);
        }
}


/* Content-signature variant: FMOD COPIES codec overlays to the heap before
 * streaming them into the swap slot, so the source EA is unknowable ahead of
 * time -- match the first 16 bytes of the streamed content instead. */
void spu_overlay_register_sig(const uint8_t sig[16], int image_id)
{
    if (s_ovl_src_count < SPU_OVL_SRC_MAX) {
        s_ovl_src[s_ovl_src_count].src_ea = 0;
        s_ovl_src[s_ovl_src_count].image_id = image_id;
        memcpy(s_ovl_src[s_ovl_src_count].sig, sig, 16);
        s_ovl_src[s_ovl_src_count].has_sig = 1;
        atomic_store_explicit(&s_ovl_has_sig, 1, memory_order_release);
        __atomic_store_n(&s_ovl_src_count, s_ovl_src_count + 1, __ATOMIC_RELEASE);
    }
}

/* Called from the MFC GET path after the copy: ls points at the JUST-COPIED
 * bytes. EA match first (exact, cheap), then content signature for sizeable
 * chunks (overlay bodies are >= 0x500 bytes). */

void spu_overlay_note_get(spu_context* ctx, uint32_t ea, const uint8_t* ls, uint32_t size)
{
    uint32_t lsa = (uint32_t)(ls - ctx->ls);
    for (unsigned slot = 0; slot < SPU_RESIDENT_CODE_SLOTS; ++slot) {
        if (!ctx->resident_code[slot].image_id) continue;
        uint32_t base = ctx->resident_code[slot].lsa;
        uint32_t end = base + ctx->resident_code[slot].size;
        if (lsa < end && lsa + size > base) {
            if (lsa < base || ea != ctx->resident_code[slot].source_ea + (lsa - base)) {
                if (!ctx->resident_code[slot].canon) {
                    const uint32_t lo = lsa > base ? lsa : base;
                    const uint32_t hi = (lsa + size) < end ? (lsa + size) : end;
                    const uint8_t* orig = vm_base + ctx->resident_code[slot].source_ea + (lo - base);
                    uint32_t k = 0;
                    while (k < hi - lo && !orig[k]) k++;
                    if (k == hi - lo || !memcmp(ls + (lo - lsa), orig, hi - lo)) continue;
                }
                ctx->resident_code[slot].image_id = 0;
                spu_dcache_residency_changed(ctx);
            } else if (ctx->resident_code[slot].verify_state) {
                ctx->resident_code[slot].verify_state = SPU_OVERLAY_PENDING;
                spu_dcache_residency_changed(ctx);
            }
        }
    }
    const int nat = __atomic_load_n(&s_ovl_at_count, __ATOMIC_ACQUIRE);
    for (int i = 0; i < nat; i++) {
        const uint32_t rlo = s_ovl_at[i].lsa, rhi = rlo + s_ovl_at[i].span;
        const uint32_t lo = lsa > rlo ? lsa : rlo;
        const uint32_t hi = (lsa + size) < rhi ? (lsa + size) : rhi;
        if (lo >= hi || (hi - lo < 128 && hi - lo != size)) continue;
        const uint8_t* q = ls + (lo - lsa);
        uint32_t k = 0;
        while (k < hi - lo && !q[k]) k++;
        if (k == hi - lo || memcmp(q, vm_base + s_ovl_at[i].ea + (lo - rlo), hi - lo)) continue;
        int have = 0, free_slot = -1;
        for (unsigned slot = 0; slot < SPU_RESIDENT_CODE_SLOTS; ++slot) {
            if (ctx->resident_code[slot].image_id == s_ovl_at[i].image_id &&
                ctx->resident_code[slot].lsa == rlo) have = 1;
            if (free_slot < 0 && (!ctx->resident_code[slot].image_id || ctx->resident_code[slot].lsa == rlo))
                free_slot = (int)slot;
        }
        if (have) continue;
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
    int nsrc = __atomic_load_n(&s_ovl_src_count, __ATOMIC_ACQUIRE);
    /* Invalidate destination code/residency above even for an unrelated EA.
     * Acquire count before the signature flag: if this snapshot contains a
     * signature, its earlier flag publication is visible. A racing future
     * signature can only force an unnecessary scan of the older snapshot. */
    if ((size < 512 || !atomic_load_explicit(&s_ovl_has_sig, memory_order_acquire)) &&
        !spu_ovl_ea_known(ea)) return;
    for (int i = 0; i < nsrc; i++) {
        const spu_ovl_src* o = &s_ovl_src[i];
        int hit = o->has_sig ? (size >= 512 && memcmp(ls, o->sig, 16) == 0)
                             : (o->src_ea == ea);
        if (hit) {
            if (o->span) {
                if (lsa + o->span > SPU_LS_SIZE) return;
                /* Verified code was lifted for one load address. */
                if (o->code && !o->canon && lsa != o->load_lsa) continue;
                for (unsigned slot = 0; slot < SPU_RESIDENT_CODE_SLOTS; ++slot) {
                    /* A relocatable image takes over its own previous placement's slot: each load
                     * lands somewhere new, and stale copies would fill the slots. */
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

/* ---------------------------------------------------------------------------
 * Micro-interpreter for RUNTIME-GENERATED stub code (SMC).
 *
 * The WWS jobmanager's interrupt handler GENERATES a register save/restore
 * stub above the static image (LS 0x3FEC0: a chain of stqd/lqd plus a `bi`
 * terminator, written at interrupt entry) and calls it. No lift can exist for
 * bytes that only come into being at runtime, so when the indirect dispatch
 * finds no registered function we interpret the LIVE LS bytes directly for
 * the small instruction set such stubs use, and exit back into lifted code at
 * the first branch (trampoline re-dispatch). Returns 1 if it executed to a
 * branch, 0 on an unknown opcode (caller falls through to BRANCH-TO-0). */
static int spu_smc_microstep(spu_context* ctx)
{
    uint32_t pc = ctx->pc & SPU_LS_MASK & ~3u;
    /* Straight runs of quadword loads and stores (lqa/stqa/lqd/stqd) go through
     * a tight loop: no decode chain, no per-access probe. The WWS handler's
     * generated save and restore stubs are 76 of them and a `bi`, run on every
     * stall-and-notify interrupt -- ~40k stubs a second on God of War:
     * Ascension's serial EDGE chain, ~7% of its time at ~25 ns an instruction
     * the generic way. */
    for (int steps = 0; steps < 4096; steps++) {
        {
            for (; steps < 4096; steps++, pc = (pc + 4) & SPU_LS_MASK) {
                uint32_t wf;
                memcpy(&wf, ctx->ls + pc, 4);
                wf = SPU_BSWAP32(wf);
                const uint32_t op9f = wf >> 23, op8f = wf >> 24;
                uint32_t a;
                if (op9f == 0x041 || op9f == 0x061)                   /* stqa / lqa: LSA = I16 << 2 */
                    a = ((wf >> 7) & 0xFFFF) << 2;
                else if (op8f == 0x24 || op8f == 0x34)                /* stqd / lqd */
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
        int32_t  i10 = (int32_t)(w << 8) >> 22;               /* bits 14-23 sext */
        int32_t  i16 = (int32_t)(int16_t)((w >> 7) & 0xFFFF);
        (void)rb;

        if (op11 == 0x1A8 || op11 == 0x1A9 || op11 == 0x1AA || op11 == 0x1AB) {
            /* bi / bisl / iret / bisled (+E/D interrupt bits) */
            if (op11 == 0x1AB &&                       /* bisled: only on event */
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
            /* biz / binz / bihz / bihnz rt,ra (+E/D bits, taken-only) */
            uint32_t cv = ctx->gpr[rt]._u32[0];
            int taken;
            switch (op11) {
            case 0x128: taken = (cv == 0); break;                     /* biz  */
            case 0x129: taken = (cv != 0); break;                     /* binz */
            case 0x12A: taken = ((cv & 0xFFFF) == 0); break;          /* bihz */
            default:    taken = ((cv & 0xFFFF) != 0); break;          /* bihnz*/
            }
            if (!taken) { pc += 4; continue; }
            if (w & 0x40000) ctx->int_enable = 1;
            else if (w & 0x80000) ctx->int_enable = 0;
            ctx->pc = ctx->gpr[ra]._u32[0] & SPU_LS_MASK & ~3u;
            g_spu_trampoline_fn = spu_indirect_branch;
            return 1;
        }
        if (op9 == 0x064 || op9 == 0x060 || op9 == 0x066 || op9 == 0x062) {
            /* br / bra / brsl / brasl */
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
            /* brz / brnz / brhz / brhnz rt,label (relative) */
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
        if (op8 == 0x24) {                                    /* stqd rt,i10(ra) */
            uint32_t a = (ctx->gpr[ra]._u32[0] + ((uint32_t)i10 << 4)) & SPU_LS_MASK & ~15u;
            spu_ls_write128(ctx, a, ctx->gpr[rt]);
            pc += 4; continue;
        }
        if (op8 == 0x34) {                                    /* lqd rt,i10(ra) */
            uint32_t a = (ctx->gpr[ra]._u32[0] + ((uint32_t)i10 << 4)) & SPU_LS_MASK & ~15u;
            ctx->gpr[rt] = spu_ls_read128(ctx, a);
            pc += 4; continue;
        }
        if (op9 == 0x041 || op9 == 0x061) {                   /* stqa / lqa (abs) */
            /* LSA = I16 << 2 (SPU ISA, RI16 form), as in spu_interp.c and
             * the lifter. */
            uint32_t a = (((w >> 7) & 0xFFFF) << 2) & SPU_LS_MASK & ~15u;
            if (op9 == 0x041) spu_ls_write128(ctx, a, ctx->gpr[rt]);
            else              ctx->gpr[rt] = spu_ls_read128(ctx, a);
            pc += 4; continue;
        }
        if (op9 == 0x047 || op9 == 0x067) {                   /* stqr / lqr (rel) */
            uint32_t a = (pc + ((uint32_t)i16 << 2)) & SPU_LS_MASK & ~15u;
            if (op9 == 0x047) spu_ls_write128(ctx, a, ctx->gpr[rt]);
            else              ctx->gpr[rt] = spu_ls_read128(ctx, a);
            pc += 4; continue;
        }
        if (op8 == 0x44) {                                    /* xori rt,ra,i10 */
            /* Runtime-generated job stubs toggle their own instruction words.
             * Use the signed RI10 immediate, just like statically lifted xori. */
            ctx->gpr[rt] = spu_xori(ctx->gpr[ra], i10);
            pc += 4; continue;
        }
        if (op8 == 0x1C) {                                    /* ai rt,ra,i10 */
            u128 r = ctx->gpr[ra];
            for (int k = 0; k < 4; k++) r._u32[k] += (uint32_t)i10;
            ctx->gpr[rt] = r; pc += 4; continue;
        }
        if (op11 == 0x040 || op11 == 0x0C0) {                 /* sf / a (word) */
            u128 x = ctx->gpr[ra], y = ctx->gpr[rb], r;
            for (int k = 0; k < 4; k++)
                r._u32[k] = (op11 == 0x040) ? y._u32[k] - x._u32[k]
                                            : x._u32[k] + y._u32[k];
            ctx->gpr[rt] = r; pc += 4; continue;
        }
        if (op9 == 0x081) { ctx->gpr[rt] = spu_splat_u32((uint32_t)i16); pc += 4; continue; }  /* il  */
        if (op9 == 0x082) { ctx->gpr[rt] = spu_splat_u32(((w >> 7) & 0xFFFF) << 16); pc += 4; continue; } /* ilhu */
        if (op9 == 0x0C1) { u128 r = ctx->gpr[rt];            /* iohl */
            for (int k = 0; k < 4; k++) r._u32[k] |= (w >> 7) & 0xFFFF;
            ctx->gpr[rt] = r; pc += 4; continue; }
        if (op7 == 0x21)  { ctx->gpr[rt] = spu_splat_u32((w >> 7) & 0x3FFFF); pc += 4; continue; } /* ila */
        if (op11 == 0x201 || op11 == 0x001) { pc += 4; continue; }  /* nop/lnop */
        if (op11 == 0x002 || op11 == 0x003) { pc += 4; continue; }  /* sync/dsync */
        /* Branch hints, all no-ops for execution: hbr is the 11-bit-opcode RR
         * form (0x1AC), hbra and hbrr are the 7-bit-opcode RI18 form (0x08,
         * 0x09) -- the same op7 group as ila (0x21) above, NOT op9. Testing
         * op9 here never matched either (hbrr 0x12033296 has op9 0x24), so a
         * runtime-generated stub carrying a branch hint decoded to UNKNOWN,
         * the microstep bailed, and the SPU fell into branch-to-0. */
        /* sagemono reached the same fix independently in #166, from a different
         * witness (hbrr 0x1200048C rather than 0x12033296). Same line, same
         * diagnosis, arrived at separately -- which is about as good as
         * corroboration gets for a decode bug. */
        if (op11 == 0x1AC || op7 == 0x08 || op7 == 0x09) { pc += 4; continue; } /* hbr/hbra/hbrr */

        /* Hand off to the real interpreter rather than giving up.
         *
         * This microstepper knows a dozen opcodes -- enough for the
         * runtime-generated stubs it was written for -- and spu_interp.c is a
         * full, selftested SPU interpreter that was already sitting here
         * unused on this path. Bailing on the first ordinary instruction is
         * strictly worse than running it.
         *
         * It matters for overlay code: a SPURS job policy module DMAs its job
         * bodies into local store and branches to them, so there is no lifted
         * body to find and the fallback IS the execution path. Guitar Hero
         * III`s job kernel dies here on an `ilh` at LS 0x6638 -- a perfectly
         * ordinary instruction the interpreter handles -- and with it the
         * decompression job it was running never completes. */
        { static int _n = 0;
          if (PS3_LOG_FIRST(_n, 8))
              fprintf(stderr, "[spu-smc] microstep img=%d pc=0x%05X word 0x%08X not in the"
                      " microstep set -- handing to the interpreter (steps=%d)\n",
                      ctx->image_id, pc, w, steps); }
        ctx->pc = pc;
        /* Stop at the drain's return point too: it sits mid-function, so it is
         * no lifted entry, and a job's return would otherwise run on through
         * the caller's code (GH3's song-freeze; see drain_ret_pc). */
        const uint32_t return_pc = ctx->drain_ret_pc;
        uint32_t result = spu_interp_run_until(ctx, pc, return_pc);
        /* A live interpreter rejoin is a transfer, not a host return. Nested
         * drains can repair an empty trampoline, but the top-level driver
         * otherwise ends the run before executing the lifted target.
         * An already-owning drain must retain its own host continuation;
         * stopped states and diagnostic exits keep their existing behavior. */
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
    /* Real SPU bi/bisl mask the target to the 256 KB local store; the high bits
     * of a computed pointer (e.g. a packed handle like 0x7a028803) are ignored.
     * Without this, any indirect branch through such a value fails the lookup
     * and falls into branch-to-0. All lifted funcs live below SPU_LS_SIZE, so
     * masking is a no-op for already-valid targets. */
    ctx->pc &= SPU_LS_MASK;
    /* Interrupt-return register restore (see spu_drain.c spu_irq_regs_save):
     * enforce the preserve-all-registers hardware contract here -- the first
     * interrupts-enabled dispatch at the saved srr0 is the irete. */
    { extern int spu_irq_regs_maybe_restore(spu_context*);
      spu_irq_regs_maybe_restore(ctx); }
    /* Use the same ownership order as the interpreter's rejoin probe. */
    spu_fn fn = spu_lifted_lookup(ctx, ctx->pc);
    if (!fn && spu_overlay_resolve_reloc(ctx, ctx->pc & SPU_LS_MASK))
        fn = spu_lifted_lookup(ctx, ctx->pc);
    /* Branch into high LS with no lifted code there.
     *
     * This was added believing the jobs called a resident SPURS job-manager
     * kernel at 0x16100 / 0x18160 / 0x1A100. That was WRONG. Those "absolute
     * branches" were the disassembler decoding ASCII as instructions: the
     * bytes behind one of them are 30 2C 20 43, i.e. "0, C" from the string
     * "ch0, F:0:400, 100, Channel 0 level, %". Every job embeds the same
     * parameter-description text, which is why the same phantom target
     * appeared in 9 of 12 unrelated images and looked like a shared ABI.
     *
     * The guard is kept because branching into unlifted high local store is
     * still an error worth stopping at rather than executing whatever is
     * there -- but it is a backstop, not a kernel interface. */
    /* SPU exit: the CRT does not branch to a `stop` in the image -- it BUILDS one
     * in memory and jumps to it. ps1_netemu's GPU core ends with
     *
     *     ori $r80,$r3,0 / andi $r80,$r80,255 / iohl $r80,0x2000
     *     stqd $r80,0x10($r1) / sync / ai $r3,$r1,16 / bi $r3
     *
     * i.e. `stop (0x2000 | status)` assembled onto the stack and executed there.
     * That target is a stack address with no lifted code, so the unlifted-branch
     * guard below called a clean exit "branched into unlifted LS 0x3FFB0 -- ending
     * the job" and reported stop_code 0, which reads as a runaway and sent a long
     * chase after phantom memory corruption. Decode the target word instead: a
     * `stop` is opcode 0 in the top 11 bits, so this is unambiguous. */
    if (!fn) {
        uint32_t p0 = ctx->pc & SPU_LS_MASK;
        if (p0 + 3 < SPU_LS_SIZE) {
            uint32_t w = ((uint32_t)ctx->ls[p0] << 24) | ((uint32_t)ctx->ls[p0+1] << 16) |
                         ((uint32_t)ctx->ls[p0+2] << 8) | ctx->ls[p0+3];
            if ((w >> 21) == 0u) {                 /* stop / stopd */
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
            /* Is there real code at the target, or is the pc garbage? Eight
             * words at the target and at the return address separate "the lift
             * missed a function" from "this branch should never have happened". */
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
        /* If real code is present at the target, run it through the interpreter
         * instead of ending the job: the SPU executes whatever local store holds.
         * No lift can exist for code written at runtime -- You Don't Know Jack's
         * FMOD mixer relocates a DSP plugin into local store and calls it -- and
         * the interpreter rejoins the compiled path as soon as it reaches a
         * lifted address. A zero word (stop 0, never written) keeps the loud
         * stop. */
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
        spu_check_stack_reset(ctx, fn);
        /* MUSTTAIL: a guest loop that iterates through an indirect branch (the
         * Bink decoder's per-command dispatch does) must not grow the host
         * stack -- a plain call here leaked a resolver+callee frame per
         * iteration and blew the thread stack ~4k iterations into the first
         * really-decoding movie frame (silent 0x80000001 death). */
#if defined(__clang__)
        __attribute__((musttail)) return fn(ctx);
#else
        fn(ctx);
        return;
#endif
    }

    /* A SPURS *job* that branches back to LS 0 has RETURNED TO THE JOB MANAGER.
     * That is how a jm2 job signals completion: its crt tail-jumps to the
     * resident manager, which on hardware loads the next job and sets up r3/r4
     * afresh. We load the job at LS 0 and have no manager, so the jump lands on
     * the job's OWN entry and it runs a second lap -- with dead registers.
     *
     * Tokyo Jungle showed exactly that: lap one is correct at every step and
     * does the real work, then the crt jumps to 0 and lap two re-reads its
     * parameters through a now-zero context pointer (hence the reads of LS 0x20
     * and the DMA addresses that were really its own opcodes) and spins forever.
     * The work was already finished before the jump; the second lap is noise.
     *
     * The initial entry is called directly, not through here, so any arrival at
     * LS 0 in this path is a re-entry. Treat it as job end. */
    if (ctx->pc == 0 && ctx->image_id > 0) {
        static int _n = 0;
        if (PS3_LOG_FIRST(_n, 8))
            fprintf(stderr, "[spurs-job] img=%d returned to the job manager "
                    "(branch to LS 0) -- job complete\n", ctx->image_id);
        spu_halt(ctx);
        return;
    }
    /* No lifted function at this PC: it may be RUNTIME-GENERATED code (the
     * WWS jobmanager writes a save/restore stub above its static
     * image and calls it). Interpret the live LS bytes; on success the next
     * branch re-enters lifted code via the trampoline. */
    if (spu_smc_microstep(ctx)) {
        spu_stop_exit_check(ctx);
        return;
    }
    /* Cap the unresolved-branch log PER IMAGE: a global cap let one noisy
     * image (the FMOD mixer's overlay calls) exhaust it and silently hide
     * every other image's misses -- LBP's loading jobs skipped their command
     * handlers for a whole session without a single log line. */
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
#ifdef _WIN32
        void* frames[24]; unsigned short fn = RtlCaptureStackBackTrace(0, 24, frames, NULL);
        char* base = (char*)GetModuleHandleA(NULL);
        fprintf(stderr, "[SPU] host bt RVAs:");
        for (unsigned short i = 0; i < fn; i++)
            fprintf(stderr, " 0x%zX", (size_t)((char*)frames[i] - base));
        fprintf(stderr, "\n");
#endif
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

/* ===========================================================================
 * Execution trace (for §3 validation: diff vs RPCS3 SPU interpreter)
 *
 * When the lifter is invoked with --trace, every emitted instruction is
 * surrounded by spu_trace_pc(ctx, PC) before execution and spu_trace_rt(
 * ctx, RT) after, for instructions whose destination is the rt slot. The
 * output is one line per event:
 *
 *     <PC-5hex>                          - PC about to execute
 *       r<rt> <hi-64hex> <lo-64hex>      - register written, post-state
 *
 * Direct to stderr by default; call spu_trace_init(path) once at startup
 * to redirect to a file. The format is intentionally minimal and stable
 * so a small converter can line it up against an RPCS3.log SPU trace.
 * ===========================================================================*/
static FILE* s_trace_fp = NULL;

void spu_trace_init(const char* path)
{
    if (!path || !*path) { s_trace_fp = stderr; return; }
    s_trace_fp = fopen(path, "w");
    if (!s_trace_fp) s_trace_fp = stderr;
}

/* Bounded by design: a --trace lift emits one call PER INSTRUCTION, so an image
 * that spins (a persistent SPURS policy module polling its job queue never
 * returns) would otherwise write stderr until the disk fills. The cap keeps the
 * useful part -- the entry path plus enough revolutions to show what the loop
 * tests -- and the tail IS the loop. SPU_TRACE_MAX=0 restores unbounded.
 * SPU_TRACE_FILE redirects off stderr so the trace does not interleave with the
 * boot log. */
/* Trace output is OPT-IN at runtime: a --trace lift bakes the calls into the
 * generated C forever, so a trace-lifted image would otherwise spew the
 * per-instruction log into every boot. Enable with SPU_TRACE=1 (stderr) or
 * SPU_TRACE_FILE=<path>. */
static long long s_trace_left = -1;     /* -1 uninit, -2 unbounded, 0 off */

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
            if (s_trace_left == 0) s_trace_left = -2;   /* -2 = unbounded */
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
    if (s_trace_left == 0) return;         /* tracing off/stopped (see _pc) */
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