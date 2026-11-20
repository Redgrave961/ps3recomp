#ifndef SPU_LIFTED_JOB_H
#define SPU_LIFTED_JOB_H

#include "spu_context.h"
#include "spu_interp.h"
#include "spu_coherency.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

void spu_mfc_release(spu_context* ctx);

typedef void (*spu_lifted_entry_fn)(spu_context*);
extern uint8_t* vm_base;

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
    ctx.image_id = image_id;
    if (opts) {
        ctx.spu_id              = opts->spu_id;
        ctx.spu_group_id        = opts->group_id;
        ctx.park_on_empty_inmbox = opts->park_on_empty;
        if (opts->inmbox_val) spu_channel_write(&ctx.ch_in_mbox, opts->inmbox_val);
        extern void spu_thread_publish_ctx(uint32_t tid, void* c);
        if (opts->spu_id) spu_thread_publish_ctx(opts->spu_id, &ctx);
    }
    ctx.gpr[1]._u32[0] = SPU_LS_SIZE - 0x10;
    if (local_store) memcpy(ctx.ls, local_store, SPU_LS_SIZE);
    if (opts && args_ea && vm_base) {
        for (int i = 0; i < 4; i++) {
            const uint8_t* p_ = vm_base + args_ea + i * 8;
            uint32_t hi = ((uint32_t)p_[0]<<24)|((uint32_t)p_[1]<<16)|((uint32_t)p_[2]<<8)|p_[3];
            uint32_t lo = ((uint32_t)p_[4]<<24)|((uint32_t)p_[5]<<16)|((uint32_t)p_[6]<<8)|p_[7];
            ctx.gpr[3 + i]._u32[0] = lo;
            ctx.gpr[3 + i]._u32[1] = hi;
        }
    } else {
        ctx.gpr[3]._u32[0] = args_ea;
    }
    { extern int spu_run_with_halt(void (*)(spu_context*), spu_context*);
      spu_run_with_halt(entry, &ctx); }

    if (opts && !spu_channel_has_data(&ctx.ch_out_intr_mbox) &&
        spu_channel_has_data(&ctx.ch_out_mbox)) {
        extern void (*g_spu_out_mbox_hook)(uint32_t, uint32_t, int, uint32_t);
        if (g_spu_out_mbox_hook)
            g_spu_out_mbox_hook(ctx.spu_group_id, ctx.spu_id, 1, ctx.ch_out_mbox.value);
    }
    if (opts && opts->spu_id) {
        extern void spu_thread_publish_ctx(uint32_t tid, void* c);
        spu_thread_publish_ctx(opts->spu_id, 0);
    }
    spu_mfc_release(&ctx);
    spu_coh_unregister(&ctx);
    spu_mfc_release(&ctx);
    if (local_store) memcpy(local_store, ctx.ls, SPU_LS_SIZE);
    return 0;
}

#endif
