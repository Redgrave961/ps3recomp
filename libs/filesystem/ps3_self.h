#ifndef PS3RECOMP_PS3_SELF_H
#define PS3RECOMP_PS3_SELF_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { PS3_SELF_OK = 0, PS3_SELF_EFORMAT = -1, PS3_SELF_EKEY = -2, PS3_SELF_ELICENSE = -3 };

int ps3_self_decrypt(const uint8_t* self, size_t size, uint8_t** elf, size_t* elf_size);

#ifdef __cplusplus
}
#endif

#endif
