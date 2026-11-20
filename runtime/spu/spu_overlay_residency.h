#ifndef SPU_OVERLAY_RESIDENCY_H
#define SPU_OVERLAY_RESIDENCY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct spu_overlay_code_segment {
    uint32_t lsa, size;
    const uint8_t* bytes;
} spu_overlay_code_segment;

int spu_overlay_register_verified_region(uint32_t content_ea, uint32_t load_lsa,
    uint32_t span, int image_id, const spu_overlay_code_segment* code,
    unsigned count);

int spu_overlay_register_reloc_content(const uint8_t* code, uint32_t span, uint32_t canon,
    int image_id);

void spu_overlay_register_region_at(uint32_t content_ea, uint32_t lsa, uint32_t span,
    int image_id);

#ifdef __cplusplus
}
#endif

#endif
