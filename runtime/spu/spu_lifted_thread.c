#include "spu_lifted_thread.h"
#include "spu_coherency.h"
#include "spu_context.h"
#include "ps3emu/ps3_tprof.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern uint8_t* vm_base;

int  spu_have_function(uint32_t addr);
int  spu_image_of_function(uint32_t addr);
void spu_indirect_branch(struct spu_context* ctx);
int  spu_run_with_halt(void (*entry)(struct spu_context*), struct spu_context* ctx);

static uint32_t rd_be32(uint32_t ea)
{
    if (!vm_base) return 0;
    const uint8_t* p = vm_base + ea;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static void spu_deploy_image(spu_context* ctx, uint32_t img_ea)
{
    if (!img_ea || !vm_base) return;
    uint32_t segs_ea = rd_be32(img_ea + 0x08);
    int32_t  nsegs   = (int32_t)rd_be32(img_ea + 0x0C);
    if (!segs_ea) return;

    for (int32_t i = 0; i < nsegs; i++) {
        uint32_t s    = segs_ea + (uint32_t)i * 0x18u;
        uint32_t type = rd_be32(s + 0x00);
        uint32_t ls   = rd_be32(s + 0x04) & SPU_LS_MASK;
        uint32_t size = rd_be32(s + 0x08);
        uint32_t src  = rd_be32(s + 0x10);
        if (!src) src = rd_be32(s + 0x14);
        if (size > SPU_LS_SIZE - ls) size = SPU_LS_SIZE - ls;
        if (type == 1) {
            if (src) memcpy(&ctx->ls[ls], vm_base + src, size);
        } else if (type == 2) {
            memset(&ctx->ls[ls], (int)(src & 0xFFu), size);
        }
    }
}

int spu_lifted_thread_available(uint32_t entry)
{
    return spu_have_function(entry & SPU_LS_MASK);
}

void spu_lifted_thread_setup(spu_context* ctx, const spu_lifted_thread_desc* d)
{
    if (!ctx || !d) return;

    spu_context_init(ctx, d->tid);
    ctx->spu_id       = d->tid;
    ctx->spu_group_id = d->group_id;
    ctx->pc           = d->entry & SPU_LS_MASK;

    { int img = d->image_id > 0 ? d->image_id : spu_image_of_function(ctx->pc);
      ctx->image_id = (img >= 0) ? img : 0; }

    if (d->img_ea) spu_deploy_image(ctx, d->img_ea);

    for (int a = 0; a < 4; a++) {
        ctx->gpr[3 + a]._u32[0] = (uint32_t)(d->args[a] >> 32);
        ctx->gpr[3 + a]._u32[1] = (uint32_t)d->args[a];
    }
}

void spu_lifted_thread_run(spu_context* ctx, spu_lifted_thread_result* out)
{
    spu_lifted_thread_result r;
    memset(&r, 0, sizeof(r));
    if (!ctx) { if (out) *out = r; return; }

    fprintf(stderr, "[SPU] tid=0x%X RUNNING lifted image entry=0x%05X (image %d)\n",
            ctx->spu_id, ctx->pc, ctx->image_id);
    fflush(stderr);

    ctx->status = SPU_STATUS_RUNNING;
    ps3_tprof_thread('S', ctx->spu_id);
    spu_run_with_halt(spu_indirect_branch, ctx);

    if (ctx->status == SPU_STATUS_STOPPED_BY_STOP &&
        ctx->stop_code == 0x102u && ctx->ch_out_mbox.count) {
        r.exit_status = (int32_t)ctx->ch_out_mbox.value;
        ctx->ch_out_mbox.count = 0;
    } else if (ctx->status == SPU_STATUS_STOPPED_BY_STOP &&
               ctx->stop_code == 0x101u && ctx->ch_out_mbox.count) {
        r.group_exit   = 1;
        r.group_status = (int32_t)ctx->ch_out_mbox.value;
        ctx->ch_out_mbox.count = 0;
    } else {
        r.faulted = 1;
    }

    fprintf(stderr, "[SPU] tid=0x%X stopped (status=0x%X pc=0x%05X code=0x%X) "
            "-> status=%d%s%s\n",
            ctx->spu_id, ctx->status, ctx->pc, ctx->stop_code, r.exit_status,
            r.group_exit ? " group-exit" : "", r.faulted ? " FAULT" : "");
    fflush(stderr);

    spu_coh_unregister(ctx);
    spu_mfc_release(ctx);
    ps3_tprof_thread_exit();

    if (out) *out = r;
}
