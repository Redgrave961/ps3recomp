/*
 * ps3recomp - PPU (PowerPC Processing Unit) execution context
 *
 * Models the full architectural state of a Cell PPU thread: 32 GPRs, 32 FPRs,
 * 32 VRs, condition register, link register, count register, XER, FPSCR, VSCR,
 * and the current instruction address (CIA).
 */

#ifndef PPU_CONTEXT_H
#define PPU_CONTEXT_H

#include "../../include/ps3emu/ps3types.h"
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Thread-local storage qualifier. Defined identically in the lifter's
 * HEADER_PREAMBLE (tools/ppu_lifter.py), which this header is already kept
 * in sync with -- code reaches one or the other depending on whether it
 * built against a generated ppu_recomp.h. The #ifndef makes including both
 * harmless. */
#ifndef PPU_THREAD_LOCAL
#  if defined(__cpp_constinit)
#    define PPU_TLS_CONSTINIT constinit
#  else
#    define PPU_TLS_CONSTINIT
#  endif
#  ifdef _MSC_VER
#    define PPU_THREAD_LOCAL PPU_TLS_CONSTINIT __declspec(thread)
#  else
#    define PPU_THREAD_LOCAL PPU_TLS_CONSTINIT __thread
#  endif
#endif

/* ---------------------------------------------------------------------------
 * Condition Register field -- 4 bits: LT, GT, EQ, SO
 * -----------------------------------------------------------------------*/
#define PPU_CR_LT  0x8
#define PPU_CR_GT  0x4
#define PPU_CR_EQ  0x2
#define PPU_CR_SO  0x1

/* XER bit positions */
#define PPU_XER_SO  (1u << 31)
#define PPU_XER_OV  (1u << 30)
#define PPU_XER_CA  (1u << 29)

/* VSCR bits */
#define PPU_VSCR_NJ  (1u << 16)  /* Non-Java mode */

/* ---------------------------------------------------------------------------
 * PPU thread context
 * -----------------------------------------------------------------------*/
typedef struct ppu_context {
    /* General-purpose registers (r0-r31) */
    uint64_t gpr[32];

    /* Floating-point registers (f0-f31) */
    double   fpr[32];

    /* Vector registers (vr0-vr31) for VMX/AltiVec
     * Each is 128 bits, aligned to 16 bytes. */
#ifdef _MSC_VER
    __declspec(align(16)) u128 vr[32];
#else
    u128     vr[32] __attribute__((aligned(16)));
#endif

    /* Condition register -- 8 x 4-bit fields packed into 32 bits.
     * CR0 is bits 31:28, CR1 is 27:24, ..., CR7 is 3:0. */
    uint32_t cr;

    /* Link register -- return address */
    uint64_t lr;

    /* Count register -- used by bdnz-style branches */
    uint64_t ctr;

    /* Fixed-point exception register */
    uint32_t xer;

    /* Floating-point status and control register */
    uint32_t fpscr;

    /* Vector status and control register */
    uint32_t vscr;

    /* Current instruction address (program counter) */
    uint64_t cia;

    /* Thread identification */
    uint64_t thread_id;

    /* Reserve address for lwarx/stwcx (load-linked / store-conditional) */
    uint64_t reserve_addr;
    uint64_t reserve_value;
    uint64_t reserve_time;

    /* VRSAVE (SPR 256): software-managed; kept so mfspr returns what mtspr wrote */
    uint32_t vrsave;

} ppu_context;

/* ---------------------------------------------------------------------------
 * Initialization
 * -----------------------------------------------------------------------*/
static inline void ppu_context_init(ppu_context* ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->vscr = PPU_VSCR_NJ;
    /* r1 (stack pointer) gets set during thread creation.
     * r2 (TOC) gets set from the ELF entry descriptor.
     * r13 (SDA base) gets set from the module loader. */
}

/* ---------------------------------------------------------------------------
 * Stack pointer management
 *
 * PS3/PPC64 ABI: r1 = stack pointer, stack grows downward,
 * 16-byte aligned, with a back-chain word at r1+0.
 * -----------------------------------------------------------------------*/
static inline void ppu_set_stack(ppu_context* ctx, uint64_t stack_top, uint64_t stack_size)
{
    /* Place SP at the top of the allocated region, aligned down to 16 bytes,
     * leaving room for the minimum stack frame (48 bytes on PPC64). */
    uint64_t sp = (stack_top + stack_size - 48) & ~(uint64_t)0xF;
    ctx->gpr[1] = sp;
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PPU_CONTEXT_H */
