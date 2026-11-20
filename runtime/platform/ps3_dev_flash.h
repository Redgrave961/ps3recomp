#ifndef PS3RECOMP_PS3_DEV_FLASH_H
#define PS3RECOMP_PS3_DEV_FLASH_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void ps3_dev_flash_path(char* out, size_t cap, const char* roots, const char* rel);

#ifdef __cplusplus
}
#endif

#endif
