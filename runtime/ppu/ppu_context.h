#ifndef PPU_CONTEXT_H
#define PPU_CONTEXT_H

#include "../../include/ps3emu/ps3types.h"
#include "ppu_tls.h"
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PPU_VSCR_NJ  (1u << 16)

typedef struct ppu_context {
    uint64_t gpr[32];

    double   fpr[32];

#ifdef _MSC_VER
    __declspec(align(16)) u128 vr[32];
#else
    u128     vr[32] __attribute__((aligned(16)));
#endif

    uint32_t cr;

    uint64_t lr;

    uint64_t ctr;

    uint32_t xer;

    uint32_t fpscr;

    uint32_t vscr;

    uint64_t cia;

    uint64_t thread_id;

    uint64_t reserve_addr;
    uint64_t reserve_value;
    uint64_t reserve_time;

    uint32_t vrsave;

} ppu_context;

static inline void ppu_context_init(ppu_context* ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->vscr = PPU_VSCR_NJ;
}

static inline void ppu_set_stack(ppu_context* ctx, uint64_t stack_top, uint64_t stack_size)
{
    uint64_t sp = (stack_top + stack_size - 48) & ~(uint64_t)0xF;
    ctx->gpr[1] = sp;
}

#ifdef __cplusplus
}
#endif

#endif
