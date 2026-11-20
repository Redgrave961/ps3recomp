#include "spu_context.h"
#include "spu_lockstep.h"
#include <setjmp.h>
#include <stdlib.h>
#include <stdio.h>
#include "ps3emu/log_first.h"
#if defined(_WIN32)
#define SPU_SETJMP(env) setjmp(env)
#define SPU_LONGJMP(env, v) longjmp(env, v)
#else
#define SPU_SETJMP(env) _setjmp(env)
#define SPU_LONGJMP(env, v) _longjmp(env, v)
#endif

typedef struct spu_irq_frame {
    jmp_buf               env;
    struct spu_irq_frame* prev;
    uint32_t              depth;
    int                   image_id;
} spu_irq_frame;

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

SPU_DRAIN_INLINE void spu_task_launch_check_inline(spu_context* ctx)
{
    if (!ctx->steps || (ctx->pc & SPU_LS_MASK)) {
        ctx->steps++;
        return;
    }
    spu_task_launch_slow(ctx);
}

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

SPU_DRAIN_INLINE void spu_drain_hop_inline(spu_context* ctx)
{
    if (g_yz_lockstep_on != 0) yz_lockstep_tick(ctx);
    spu_task_launch_check_inline(ctx);
}

void spu_drain_hop(spu_context* ctx)
{
    spu_drain_hop_inline(ctx);
}

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


static void spu_irq_regs_save(spu_context* ctx)
{
    ctx->irq_saved = 1;
    ctx->irq_resume_pc = ctx->srr0;
    memcpy(ctx->irq_gpr, ctx->gpr, sizeof ctx->irq_gpr);
}

void spu_irq_regs_forget(spu_context* ctx)
{
    ctx->irq_saved = 0;
}

int spu_irq_regs_maybe_restore(spu_context* ctx)
{
    if (!ctx->irq_saved) return 0;
    if ((ctx->pc & SPU_LS_MASK) != (ctx->irq_resume_pc & SPU_LS_MASK) || !ctx->int_enable)
        return 0;
    memcpy(ctx->gpr, ctx->irq_gpr, sizeof ctx->irq_gpr);
    ctx->irq_saved = 0;
    spu_irq_frame* f = (spu_irq_frame*)ctx->irq_frame;
    if (f && ctx->host_depth <= f->depth)
        ctx->irq_frame = f->prev;
    if (f && ctx->host_depth > f->depth)
        SPU_LONGJMP(f->env, 1);
    if (!f && ctx->host_depth > 0) {
        extern void spu_restart_dispatch(spu_context*);
        spu_restart_dispatch(ctx);
    }
    return 1;
}

void (*spu_take_interrupt(spu_context* ctx,
                          void (*tf)(spu_context*)))(spu_context*)
{
    const uint8_t* v = ctx->ls;
    uint32_t w = ((uint32_t)v[0] << 24) | ((uint32_t)v[1] << 16) |
                 ((uint32_t)v[2] << 8) | v[3];
    uint32_t op9 = w >> 23;
    uint32_t target;
    if (op9 == 0x060) {
        target = ((w >> 7) & 0xFFFF) << 2;
    } else if (op9 == 0x064) {
        target = (((w >> 7) & 0xFFFF) << 2) & SPU_LS_MASK;
    } else {
        static int _w = 0;
        if (PS3_LOG_FIRST(_w, 4))
            fprintf(stderr, "[spu-int] pending (st=0x%X mask=0x%X) but LS0 word "
                    "0x%08X is no branch -- not taken\n",
                    ctx->event_status, ctx->event_mask, w);
        return tf;
    }
    ctx->srr0 = ctx->pc;
    ctx->int_enable = 0;
    spu_irq_regs_save(ctx);
    ctx->pc = target & SPU_LS_MASK;
    { static int _n = 0;
      if (PS3_LOG_FIRST(_n, 16))
          fprintf(stderr, "[spu-int] TAKEN img=%d events=0x%X&0x%X vector->0x%05X "
                  "(srr0=0x%05X) stallstat=0x%X parked=0x%X\n", ctx->image_id,
                  ctx->event_status, ctx->event_mask, ctx->pc, ctx->srr0,
                  ctx->list_stall_stat, ctx->list_stall_mask); }
    return spu_indirect_branch;
}

uint32_t g_spu_depth_guard_limit;
SPU_DRAIN_COLD void spu_depth_guard(spu_context* ctx)
{
    if (!g_spu_depth_guard_limit) g_spu_depth_guard_limit = 2000u;
    const uint32_t s_max = g_spu_depth_guard_limit;

    enum { RING = 24 };
    static uint32_t s_ring[RING];
    static uint32_t s_n;
    if (ctx->host_depth < s_max / 2) return;
    s_ring[s_n % RING] = ((uint32_t)ctx->pc & SPU_LS_MASK);
    s_n++;

    if (ctx->host_depth < s_max) return;
    static int s_reported = 0;
    if (s_reported < 1) {
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

typedef void (*spu_drain_fn)(spu_context*);
extern spu_drain_fn spu_lifted_lookup(const spu_context* ctx, uint32_t lsa);

void spu_drain_call(spu_context* ctx, uint32_t return_pc)
{
    if (__builtin_expect(g_spu_direct < 0, 0)) spu_direct_init();
    spu_depth_guard_check(ctx);
    const uint32_t outer_ret_pc = ctx->drain_ret_pc;
    spu_irq_frame f;
    f.prev = 0; f.depth = 0; f.image_id = 0;
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
            ctx->drain_ret_pc = return_pc & SPU_LS_MASK;
            spu_drain_hop_inline(ctx);
            if (ctx->int_enable && (ctx->event_status & ctx->event_mask)) {
                void (*vf)(spu_context*) = spu_take_interrupt(ctx, fn);
                if (!ctx->int_enable) {
                    if (ctx->irq_frame != &f) {
                        f.prev = (spu_irq_frame*)ctx->irq_frame;
                        f.depth = ctx->host_depth;
                        f.image_id = ctx->image_id;
                        ctx->irq_frame = &f;
                    }
                    if (SPU_SETJMP(f.env) == 0) {
                        extern void spu_jmp_no_unwind(void*);
                        spu_jmp_no_unwind(f.env);
                        vf(ctx);
                    } else {
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
