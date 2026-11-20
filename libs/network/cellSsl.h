#ifndef PS3RECOMP_CELL_SSL_H
#define PS3RECOMP_CELL_SSL_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CELL_SSL_ERROR_NOT_INITIALIZED       0x80720101
#define CELL_SSL_ERROR_ALREADY_INITIALIZED   0x80720102

s32 cellSslInit(void* pool, u32 poolSize);
s32 cellSslEnd(void);

s32 cellSslCertificateLoader(u64 flags, char* buffer, u32 size, u32* required);

#ifdef __cplusplus
}
#endif

#endif
