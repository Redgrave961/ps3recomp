#ifndef SPU_INTERP_H
#define SPU_INTERP_H

#include "spu_context.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*spu_lifted_fn)(spu_context*);

spu_lifted_fn spu_lifted_lookup(const spu_context* ctx, uint32_t lsa);

uint32_t spu_interp_run(spu_context* ctx, uint32_t start_lsa);
uint32_t spu_interp_run_until(spu_context* ctx, uint32_t start_lsa, uint32_t stop_lsa);

void spu_dispatch(spu_context* ctx, uint32_t target);

#ifdef __cplusplus
}
#endif
#endif

