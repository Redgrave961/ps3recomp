#ifndef SPU_LOCKSTEP_H
#define SPU_LOCKSTEP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct spu_context;

extern volatile int g_yz_lockstep_on;

int yz_lockstep_enabled(void);

void yz_lockstep_register(struct spu_context* ctx);
void yz_lockstep_unregister(struct spu_context* ctx);

void yz_lockstep_tick(struct spu_context* ctx);

void yz_lockstep_block_begin(struct spu_context* ctx);
void yz_lockstep_block_end(struct spu_context* ctx);

#ifdef __cplusplus
}
#endif
#endif
