#ifndef SPU_COH_BITMAP_H
#define SPU_COH_BITMAP_H

#include <stdint.h>

#if defined(_WIN32)
#  define SPU_COH_SHARED __declspec(selectany)
#else
#  define SPU_COH_SHARED __attribute__((weak))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SPU_COH_LINE_SHIFT  7

SPU_COH_SHARED const unsigned char* g_spu_coh_bits = 0;

static inline int spu_coh_line_reserved(uint32_t addr)
{
    const unsigned char* bits = g_spu_coh_bits;
    if (!bits) return 0;
    const uint32_t line = addr >> SPU_COH_LINE_SHIFT;
    return (bits[line >> 3] >> (line & 7)) & 1;
}

#ifdef __cplusplus
}
#endif

#endif
