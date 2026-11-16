/* Clang-compiled musttail fast path for SPU indirect-branch dispatch.
 *
 * The full resolver (spu_indirect_branch, runtime/spu/spu_channels.c) lives in
 * the MSVC-built ps3recomp_runtime.lib, where __attribute__((musttail)) is
 * unavailable -- its `fn(ctx)` is a plain call, so a guest loop that iterates
 * through an indirect branch (the Bink decoder's per-command dispatch) leaks
 * one ~650-byte resolver frame per iteration and blows the thread stack a few
 * thousand iterations into the first really-decoding movie frame (observed as
 * a silent STATUS_GUARD_PAGE_VIOLATION death).
 *
 * This TU is compiled by clang inside the game build: on a registry hit the
 * dispatch is a GUARANTEED tail call -- zero stack growth. A lookup miss falls
 * back to the full resolver.
 */
#include "spu_context.h"
#include "spu_interp.h"

typedef spu_lifted_fn spu_dispatch_fn;
void spu_indirect_branch(spu_context* ctx);               /* full resolver in the lib */

void spu_indirect_branch_mt(spu_context* ctx)
{
    uint32_t pc = ctx->pc & SPU_LS_MASK;
    spu_dispatch_fn fn = spu_lifted_lookup(ctx, pc);
    if (fn) {
        ctx->pc = pc;
        __attribute__((musttail)) return fn(ctx);
    }
    __attribute__((musttail)) return spu_indirect_branch(ctx);
}
