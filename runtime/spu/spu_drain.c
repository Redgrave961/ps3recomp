/* SPU_DRAIN trampoline model -- runtime state and the per-transfer hooks. */
#include "spu_context.h"
#include "spu_lockstep.h"
#include <setjmp.h>
#include <stdlib.h>
#include <stdio.h>
#include "ps3emu/log_first.h"
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

/* See spu_context.h `irq_frame`. */
typedef struct spu_irq_frame {
    jmp_buf               env;
    struct spu_irq_frame* prev;
    uint32_t              depth;
    int                   image_id;
} spu_irq_frame;

/* Pending cross-function transfer target for this host thread's SPU context. */
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*) = 0;
SPU_THREAD_LOCAL uint32_t g_spu_pch[8];
SPU_THREAD_LOCAL unsigned g_spu_pch_n;

extern void spu_halt(spu_context*);

#if defined(_MSC_VER)
#define SPU_DRAIN_COLD __declspec(noinline)
#define SPU_DRAIN_INLINE static __forceinline
#else
#define SPU_DRAIN_COLD __attribute__((noinline, cold))
#define SPU_DRAIN_INLINE static inline __attribute__((always_inline))
#endif
static SPU_DRAIN_COLD void spu_task_launch_slow(spu_context* ctx);

/* Keep checkpoint order: stack reset can unwind before steps increment, and
 * guest calls can change PC and steps before the next hop. */
SPU_DRAIN_INLINE void spu_task_launch_check_inline(spu_context* ctx, void* fn)
{
    extern void spu_check_stack_reset(spu_context*, void (*)(spu_context*));
    if (spu_stack_reset_needed(ctx))
        spu_check_stack_reset(ctx, (void (*)(spu_context*))fn);
    if (!ctx->steps || (ctx->pc & SPU_LS_MASK)) {
        ctx->steps++;
        return;
    }
    spu_task_launch_slow(ctx);
}

/* SPU_DIRECT (spu_context.h) skips the per-step work of the drain loops, so
 * SPU_LOCKSTEP (the quantum tick) keeps it off. */
volatile int g_spu_direct = -1;
void spu_direct_init(void)
{
    if (g_spu_direct >= 0) return;
    const char* d = getenv("SPU_LOCKSTEP");
    int on = !(d && *d && strcmp(d, "0"));
    const char* why = "SPU_LOCKSTEP";
#ifndef SPU_DIRECT_TAILCALLS
    on = 0; why = "no musttail in this compiler";
#endif
    if (!on) fprintf(stderr, "[spu] direct tail calls off (%s)\n", why);
    g_spu_direct = on;
}

void spu_task_launch_check(spu_context* ctx, void* fn)
{
    spu_task_launch_check_inline(ctx, fn);
}

SPU_DRAIN_INLINE void spu_drain_hop_inline(spu_context* ctx, void (*fn)(spu_context*))
{
    /* Read the live gate each hop. -1 still takes the original initialization
     * path; a callee can enable lockstep before the next checkpoint. */
    if (g_yz_lockstep_on != 0) yz_lockstep_tick(ctx);
    spu_task_launch_check_inline(ctx, (void*)fn);
}

void spu_drain_hop(spu_context* ctx, void (*fn)(spu_context*))
{
    spu_drain_hop_inline(ctx, fn);
}

/* A SPURS job returning to LS 0 is finished -- its crt tail-jumps to the
 * resident job manager, and with the job loaded at 0 that lands on its own
 * entry. Planting a return address in r0 catches the jobs that get there
 * via `bi $r0`, but some branch to 0 DIRECTLY, which the lifter turns into
 * a plain trampoline that never reaches spu_indirect_branch. The drain sees
 * every step, so catch it here too. Step 0 is the real entry. */
static void spu_task_launch_slow(spu_context* ctx)
{
    ctx->steps++;
    if (ctx->image_id <= 0) return;
    static int _n = 0;
    if (PS3_LOG_FIRST(_n, 8))
        fprintf(stderr, "[spurs-job] img=%d branched to LS 0 -- job complete\n",
                ctx->image_id);
    spu_halt(ctx);
}


/* --- SPU interrupt dispatch (drain-loop hook) ------------------------------
 * Called when int_enable && (event_status & event_mask): architectural SPU
 * interrupt. srr0 <- the interrupted continuation (ctx->pc already holds the
 * pending transfer target), interrupts disable, and control vectors through
 * the guest-planted branch instruction at LS 0. If LS 0 holds no br/bra, the
 * interrupt is NOT taken -- we log once and resume normal flow rather than
 * jump into zeros. */

/* ---- Interrupt register preservation --------------------------------------
 * Hardware contract: an SPU interrupt handler preserves every register. Enforce
 * it host-side: snapshot the full GPR file when the interrupt is taken, restore
 * it when the handler irets (detected in spu_indirect_branch: pc == saved srr0
 * with interrupts re-enabled -- during the handler int_enable stays 0, so the
 * first enabled dispatch at srr0 IS the irete). State the handler legitimately
 * publishes lives in LS/channels and is untouched. Per-ctx slots; no nesting
 * (interrupts stay disabled until iret). */
static void spu_irq_regs_save(spu_context* ctx)
{
    ctx->irq_saved = 1;
    ctx->irq_resume_pc = ctx->srr0;
    memcpy(ctx->irq_gpr, ctx->gpr, sizeof ctx->irq_gpr);
}

/* Drop any saved register file belonging to this context. The save slots are
 * keyed by RAW POINTER, and an spu_context is typically a stack local -- so a
 * later, unrelated run lands on the same address and inherits the earlier
 * context's registers wholesale. A fresh run must never do that. */
void spu_irq_regs_forget(spu_context* ctx)
{
    ctx->irq_saved = 0;
}

/* Called from spu_indirect_branch on every dispatch. Restores + clears when
 * the iret lands. Returns 1 if a restore happened. */
int spu_irq_regs_maybe_restore(spu_context* ctx)
{
    if (!ctx->irq_saved) return 0;
    if ((ctx->pc & SPU_LS_MASK) != (ctx->irq_resume_pc & SPU_LS_MASK) || !ctx->int_enable)
        return 0;
    memcpy(ctx->gpr, ctx->irq_gpr, sizeof ctx->irq_gpr);
    ctx->irq_saved = 0;
    /* The iret completed below the frame that took the interrupt:
     * abandon the handler's host frames and resume there. */
    spu_irq_frame* f = (spu_irq_frame*)ctx->irq_frame;
    if (f && ctx->host_depth <= f->depth)
        ctx->irq_frame = f->prev;      /* returned to its loop the ordinary way */
    if (f && ctx->host_depth > f->depth)
        SPU_LONGJMP(f->env, 1);
    if (!f && ctx->host_depth > 0) {
        /* Taken by the top-level driver loop (depth 0), which
         * keeps no frame: the driver's own restart re-enters at
         * srr0 with the host stack empty, which is that frame. */
        extern void spu_restart_dispatch(spu_context*);
        spu_restart_dispatch(ctx);
    }
    return 1;
}

void (*spu_take_interrupt(spu_context* ctx,
                          void (*tf)(spu_context*)))(spu_context*)
{
    const uint8_t* v = ctx->ls;      /* vector word at LS 0, big-endian */
    uint32_t w = ((uint32_t)v[0] << 24) | ((uint32_t)v[1] << 16) |
                 ((uint32_t)v[2] << 8) | v[3];
    uint32_t op9 = w >> 23;          /* top 9 opcode bits */
    uint32_t target;
    if (op9 == 0x060) {              /* bra i16 (absolute) */
        target = ((w >> 7) & 0xFFFF) << 2;
    } else if (op9 == 0x064) {       /* br i16 (relative to LS 0) */
        target = (((w >> 7) & 0xFFFF) << 2) & SPU_LS_MASK;
    } else {
        static int _w = 0;
        if (PS3_LOG_FIRST(_w, 4))
            fprintf(stderr, "[spu-int] pending (st=0x%X mask=0x%X) but LS0 word "
                    "0x%08X is no branch -- not taken\n",
                    ctx->event_status, ctx->event_mask, w);
        return tf;
    }
    ctx->srr0 = ctx->pc;             /* resume point for iret */
    ctx->int_enable = 0;
    spu_irq_regs_save(ctx);          /* hardware contract: handler preserves regs */
    ctx->pc = target & SPU_LS_MASK;
    { static int _n = 0;
      if (PS3_LOG_FIRST(_n, 16))
          fprintf(stderr, "[spu-int] TAKEN img=%d events=0x%X&0x%X vector->0x%05X "
                  "(srr0=0x%05X) stallstat=0x%X parked=0x%X\n", ctx->image_id,
                  ctx->event_status, ctx->event_mask, ctx->pc, ctx->srr0,
                  ctx->list_stall_stat, ctx->list_stall_mask); }
    return spu_indirect_branch;
}

/* See the declaration in spu_context.h. */
uint32_t g_spu_depth_guard_limit;
SPU_DRAIN_COLD void spu_depth_guard(spu_context* ctx)
{
    if (!g_spu_depth_guard_limit) {
        const char* e = getenv("SPU_HOST_DEPTH_MAX");
        g_spu_depth_guard_limit = e ? (uint32_t)strtoul(e, 0, 0) : 2000u;
    }
    const uint32_t s_max = g_spu_depth_guard_limit;

    /* Ring of recent drain sites. When the depth trips, the useful question is
     * not "where are we" but "what cycle got us here" -- one pc names a point,
     * a ring names the loop. */
    enum { RING = 24 };
    static uint32_t s_ring[RING];
    static uint32_t s_n;
    /* Record only in the upper half of the budget: a runaway passes through it
     * on its way to the limit, and ordinary transfers from every SPU thread no
     * longer write this shared line. */
    if (ctx->host_depth < s_max / 2) return;
    s_ring[s_n % RING] = ((uint32_t)ctx->pc & SPU_LS_MASK);
    s_n++;

    if (ctx->host_depth < s_max) return;
    static int s_reported = 0;
    if (s_reported < 1) {   /* one report: five SPU threads all trip together */
        s_reported++;
        fprintf(stderr, "[spu-depth] img=%d pc=0x%05X lr=0x%05X host_depth=%u"
                        " -- lifted call recursion, halting the SPU\n",
                ctx->image_id, (uint32_t)ctx->pc & SPU_LS_MASK,
                ctx->gpr[0]._u32[0] & SPU_LS_MASK, ctx->host_depth);
        fprintf(stderr, "[spu-depth] recent drain sites (oldest first):");
        uint32_t start = (s_n > RING) ? (s_n - RING) : 0;
        for (uint32_t k = start; k < s_n; k++)
            fprintf(stderr, " %05X", s_ring[k % RING]);
        fprintf(stderr, "\n");
        fflush(stderr);
    }
    { extern void spu_halt(spu_context*); spu_halt(ctx); }
}

/* A return may use any register, not only r0. Running that branch target
 * inside this drain executes the caller's continuation twice (once here,
 * once when its real host frame resumes), including its stack adjustment.
 *
 * One kind of "wrong" pc is safe to run here, and must be: a lifted ENTRY
 * that has no host frame of its own. Every WWS job module starts with
 *
 *     ila $r0, entry+0xAC ; a $r0, $r0, $r126 ; br body
 *
 * so the body ends `bi $r0` at a point the entry function tail-jumped to and
 * whose host frame is therefore gone: the continuation exists only as the
 * lifted function seeded at that address. Unwinding instead (the restart
 * below) leaves the manager unable to resume at ITS return point, which is a
 * plain r0 drain with no entry, and the job ends on a garbage r0. So when the
 * callee comes back on a lifted entry, run it in this drain until the pc
 * reaches return_pc; only a pc with no lifted code behind it is unwound. */
typedef void (*spu_drain_fn)(spu_context*);
extern spu_drain_fn spu_lifted_lookup(const spu_context* ctx, uint32_t lsa);

void spu_drain_call(spu_context* ctx, uint32_t return_pc)
{
    if (__builtin_expect(g_spu_direct < 0, 0)) spu_direct_init();
    spu_depth_guard_check(ctx);
    /* On the way out, the enclosing drain's return pc is the innermost one
     * again. It used to be 0 until that drain's next step re-set it, and with
     * SPU_DIRECT the code after a call can tail-call on for a long while
     * before it takes a step. */
    const uint32_t outer_ret_pc = ctx->drain_ret_pc;
    /* The frame an interrupt taken in THIS loop returns to. It stays armed
     * after the handler's first host function returns -- the handler keeps
     * running as trampolines of this loop -- until its iret fires or the
     * loop exits, whichever first. */
    spu_irq_frame f;
    f.prev = 0; f.depth = 0; f.image_id = 0;
    /* The pending-transfer slot is thread-local, and on Darwin every access
     * to a thread-local is a call to the TLV resolver. The slot's address is
     * fixed for the life of this host thread, and a drain never leaves its
     * thread, so resolve it once per drain instead of once per access. */
    void (**const tramp)(spu_context*) = &g_spu_trampoline_fn;

#define SPU_DRAIN_POP_IRQ() do { if (ctx->irq_frame == &f) ctx->irq_frame = f.prev; ctx->drain_ret_pc = outer_ret_pc; } while (0)
    for (;;) {
        while (*tramp) {
            if (*tramp == spu_indirect_branch &&
                (ctx->pc & SPU_LS_MASK) == (return_pc & SPU_LS_MASK)) {
                *tramp = 0;
                SPU_DRAIN_POP_IRQ();
                return;
            }
            void (*fn)(spu_context*) = *tramp;
            *tramp = 0;
            ctx->drain_ret_pc = return_pc & SPU_LS_MASK;   /* re-set: a nested drain changed it */
            spu_drain_hop_inline(ctx, fn);
            if (ctx->int_enable && (ctx->event_status & ctx->event_mask)) {
                void (*vf)(spu_context*) = spu_take_interrupt(ctx, fn);
                if (!ctx->int_enable) {   /* taken (a take clears the enable; a deferral leaves it) */
                    /* Interrupt taken here: this loop is the frame the iret
                     * comes back to, however deep the handler leaves from. */
                    if (ctx->irq_frame != &f) {
                        f.prev = (spu_irq_frame*)ctx->irq_frame;
                        f.depth = ctx->host_depth;
                        f.image_id = ctx->image_id;
                        ctx->irq_frame = &f;
                    }
                    if (SPU_SETJMP(f.env) == 0) {
                        extern void spu_jmp_no_unwind(void*);   /* spu_channels.c */
                        spu_jmp_no_unwind(f.env);
                        vf(ctx);
                    } else {
                        /* iret fired deeper: registers restored, pc = srr0 */
                        ctx->host_depth = f.depth;
                        ctx->image_id = f.image_id;
                        *tramp = spu_indirect_branch;
                        SPU_DRAIN_POP_IRQ();
                    }
                    continue;
                }
            }
            fn(ctx);
        }
        if ((ctx->pc & SPU_LS_MASK) == (return_pc & SPU_LS_MASK)) {
            SPU_DRAIN_POP_IRQ();
            return;
        }
        {
            uint32_t pc = ctx->pc & SPU_LS_MASK;
            spu_drain_fn fn = spu_lifted_lookup(ctx, pc);
            if (fn) {
                { static int _n = 0; if (PS3_LOG_FIRST(_n, 8))
                    fprintf(stderr, "[spu] drain-resume return_pc=0x%05X at lifted entry 0x%05X img=%d depth=%d ovl=%d\n",
                            (unsigned)(return_pc & SPU_LS_MASK), pc, ctx->image_id, ctx->host_depth,
                            (int)ctx->resident_ovl); }
                *tramp = spu_indirect_branch;
                continue;
            }
        }
        {
            extern void spu_restart_dispatch(spu_context*);
            SPU_DRAIN_POP_IRQ();
            spu_restart_dispatch(ctx);
            return;
        }
    }
#undef SPU_DRAIN_POP_IRQ
}
