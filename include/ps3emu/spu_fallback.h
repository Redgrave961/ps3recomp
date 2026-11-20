#ifndef PS3EMU_SPU_FALLBACK_H
#define PS3EMU_SPU_FALLBACK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int32_t (*spu_ppu_fallback_fn)(uint32_t tid, uint32_t args_ea,
                                       uint32_t args_size, void* user);

uint8_t* spu_thread_get_local_store(uint32_t tid);

#ifdef __cplusplus
}
#endif

#endif
