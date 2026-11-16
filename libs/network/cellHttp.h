/*
 * ps3recomp - cellHttp HLE
 */

#ifndef PS3RECOMP_CELL_HTTP_H
#define PS3RECOMP_CELL_HTTP_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CELL_HTTP_ERROR_NOT_INITIALIZED         0x80710101
#define CELL_HTTP_ERROR_ALREADY_INITIALIZED     0x80710102
#define CELL_HTTP_ERROR_UNKNOWN                 0x807101FF

s32 cellHttpInit(void* pool, u32 poolSize);
s32 cellHttpEnd(void);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_HTTP_H */
