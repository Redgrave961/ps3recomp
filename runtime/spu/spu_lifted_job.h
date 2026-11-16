/* spu_lifted_job.h — run a lifted SPU thread image on a fresh context.
 *
 * The bridge between the lv2 SPU-thread-group layer (runtime/syscalls/lv2_register.c,
 * which runs SPU threads as registered PPU-fallbacks) and the lifted-execution layer
 * (a lifted spu_func from spu_lifter): this helper sets up the context, runs the
 * lifted entry, and bridges the local store.
 */
#ifndef SPU_LIFTED_JOB_H
#define SPU_LIFTED_JOB_H

#include "spu_context.h"
#include "spu_interp.h"
#include "spu_coherency.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

void spu_mfc_release(spu_context* ctx);   /* spu_channels.c */

typedef void (*spu_lifted_entry_fn)(spu_context*);
/* Guest RAM base, for decoding a raw SPU thread's argument block. */
extern uint8_t* vm_base;

/* Optional per-run setup for a RAW SPU THREAD (sys_spu_thread_*). Pass NULL
 * for a plain job.
 *
 * A raw SPU thread is a persistent worker: it inits, hands the PPU a ready
 * handshake, then idles on its inbound mailbox waiting for commands. Three
 * things it needs that a fire-and-forget job does not:
 *
 *   inmbox_val    the command the PPU just wrote with
 *                 sys_spu_thread_write_spu_mb, pre-loaded so the worker's
 *                 `rdch SPU_RdInMbox` returns it.
 *   park_on_empty park (halt) instead of spinning when the inbox is empty, so
 *                 the run ENDS at the idle point and the host thread is free.
 *   spu_id        without it, a WrOutMbox the worker issues cannot be matched
 *                 back to an lv2 SPU thread, so the completion event is dropped
 *                 and the PPU waits in sys_event_queue_receive forever.
 */
typedef struct spu_run_opts {
    uint32_t inmbox_val;
    int      park_on_empty;
    uint32_t spu_id;
    uint32_t group_id;
} spu_run_opts;

static inline int32_t spu_run_lifted_job_abi(spu_lifted_entry_fn entry,
                                             uint8_t* local_store,
                                             uint32_t args_ea,
                                             int image_id,
                                             const spu_run_opts* opts)
{
    if (!entry) return -1;
    spu_context ctx;
    spu_context_init(&ctx, 0);
    ctx.image_id = image_id;     /* select this image's indirect-branch table */
    if (opts) {
        ctx.spu_id              = opts->spu_id;
        ctx.spu_group_id        = opts->group_id;
        ctx.park_on_empty_inmbox = opts->park_on_empty;
        if (opts->inmbox_val) spu_channel_write(&ctx.ch_in_mbox, opts->inmbox_val);
        /* Publish the LIVE context for the duration of the run so the PPU side
         * (sys_spu_thread_write_spu_mb) can poke this worker's mailbox and wake
         * it where it stands, instead of restarting it from its entry.
         *
         * This is what makes a persistent worker actually persistent: blocking
         * inside rdch keeps the host thread's C stack alive, so the SPU's
         * register state survives the wait. Park-and-restart cannot do that --
         * local store persists but registers do not, so a re-run re-executes
         * init and consumes the next command as though it were a startup
         * parameter. */
        extern void spu_thread_publish_ctx(uint32_t tid, void* c);
        if (opts->spu_id) spu_thread_publish_ctx(opts->spu_id, &ctx);
    }
    /* Initialize the SPU stack pointer to the top of local store (the SPU ABI
     * expects r1 = top-16, 16-byte aligned, with a NULL back-chain). Without
     * this it is 0 from spu_context_init, so the first `r1 -= frame` wraps
     * negative -> garbage stack -> null function pointers -> branch to LS 0. */
    ctx.gpr[1]._u32[0] = SPU_LS_SIZE - 0x10;   /* 0x3FFF0 for a 256KB LS */
    if (local_store) memcpy(ctx.ls, local_store, SPU_LS_SIZE);  /* job's LS in */
    /* RAW SPU THREAD (opts set): sys_spu_thread_argument is FOUR u64s
     * (arg1..arg4) passed in r3..r6 -- not the address of that block. Each
     * is a 64-bit EA the SPU reads as {word0 = EA-low, word1 = EA-high},
     * so the guest u64's low half goes in word0 and the high half in word1
     * (plain big-endian doubleword order would put the EA in word1 and trip
     * the EA-high==0 assert in spu_dma.h on any real pointer argument).
     *
     * Handing the raw path `args_ea` itself instead made the worker read a
     * pointer-to-its-arguments where it expected argument one: MultiStream's
     * mixer ran ~12 hops of CRT, touched no channels at all, and returned
     * (branch to LS 0) without ever reaching its service loop. */
    if (opts && args_ea && vm_base) {
        for (int i = 0; i < 4; i++) {
            const uint8_t* p_ = vm_base + args_ea + i * 8;
            uint32_t hi = ((uint32_t)p_[0]<<24)|((uint32_t)p_[1]<<16)|((uint32_t)p_[2]<<8)|p_[3];
            uint32_t lo = ((uint32_t)p_[4]<<24)|((uint32_t)p_[5]<<16)|((uint32_t)p_[6]<<8)|p_[7];
            ctx.gpr[3 + i]._u32[0] = lo;
            ctx.gpr[3 + i]._u32[1] = hi;
        }
    } else {
        ctx.gpr[3]._u32[0] = args_ea;                       /* simple-job arg -> r3 */
    }
    { extern int spu_run_with_halt(void (*)(spu_context*), spu_context*);
      spu_run_with_halt(entry, &ctx); }               /* run with halt pad   */

    /* Raw-thread completion signal. Hardware raises a PPU event only from
     * WrOutIntrMbox; the plain mailbox is PPU-polled. A worker that reports
     * completion on the PLAIN mailbox would otherwise never wake the PPU
     * blocked in sys_event_queue_receive. One event per RUN -- one event per
     * WRITE oversupplies the queue. Jobs (opts == NULL) are left alone. */
    if (opts && !spu_channel_has_data(&ctx.ch_out_intr_mbox) &&
        spu_channel_has_data(&ctx.ch_out_mbox)) {
        extern void (*g_spu_out_mbox_hook)(uint32_t, uint32_t, int, uint32_t);
        if (g_spu_out_mbox_hook)
            g_spu_out_mbox_hook(ctx.spu_group_id, ctx.spu_id, 1, ctx.ch_out_mbox.value);
    }
    if (opts && opts->spu_id) {
        extern void spu_thread_publish_ctx(uint32_t tid, void* c);
        spu_thread_publish_ctx(opts->spu_id, 0);   /* run over: ctx is a stack local */
    }
    /* ctx is a stack local that may have taken a lock-line reservation; left
     * in the reserver set it is walked by the next PPU store to that line.
     * GH3's Havok task returning is what first hit it. */
    spu_mfc_release(&ctx);
    spu_coh_unregister(&ctx);
    spu_mfc_release(&ctx);   /* or its MFC slot leaks and later SPUs share one engine */
    if (local_store) memcpy(local_store, ctx.ls, SPU_LS_SIZE);  /* LS back out */
    return 0;
}

#endif /* SPU_LIFTED_JOB_H */
