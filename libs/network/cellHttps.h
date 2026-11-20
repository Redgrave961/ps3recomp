#ifndef PS3RECOMP_CELL_HTTPS_H
#define PS3RECOMP_CELL_HTTPS_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CELL_HTTPS_ERROR_NOT_INITIALIZED       0x80711101
#define CELL_HTTPS_ERROR_ALREADY_INITIALIZED   0x80711102

typedef struct CellHttpsData {
    char* ptr;
    u32   size;
} CellHttpsData;

s32 cellHttpsInit(u32 caCertNum, const CellHttpsData* caList);
s32 cellHttpsEnd(void);

#ifdef __cplusplus
}
#endif

#endif
