#ifndef SPU_COHERENCY_H
#define SPU_COHERENCY_H

#include "spu_context.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPU_EVENT_LR  0x400u

void spu_lockline_lock(uint32_t ea);
void spu_lockline_unlock(uint32_t ea);
void spu_lockline_lock_range(uint32_t ea, uint32_t size);
void spu_lockline_unlock_range(uint32_t ea, uint32_t size);

void spu_coh_reserve(spu_context* ctx, uint32_t ea);

int spu_coh_reserve_fast(spu_context* ctx, uint32_t ea, uint32_t* seq);
int spu_lockline_unchanged(uint32_t ea, uint32_t seq);

int spu_coh_is_reserved(uint32_t addr);

void spu_coh_notify_write(uint32_t ea);
void spu_coh_unregister(spu_context* ctx);
void spu_coh_forget_range(uintptr_t lo, uintptr_t hi);
void spu_coh_notify_write_except(uint32_t ea, const void* self);

extern unsigned long g_spu_lr_raise;

#ifdef __cplusplus
}
#endif

#endif
