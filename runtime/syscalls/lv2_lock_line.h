#ifndef LV2_LOCK_LINE_H
#define LV2_LOCK_LINE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int64_t lv2_spu_lock_line_classify(uint32_t addr, uint64_t flags);

#ifdef __cplusplus
}
#endif

#endif
