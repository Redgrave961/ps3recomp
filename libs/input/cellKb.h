#ifndef PS3RECOMP_CELL_KB_H
#define PS3RECOMP_CELL_KB_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CELL_KB_ERROR_INVALID_PARAMETER     (CELL_ERROR_BASE_KB | 0x02)
#define CELL_KB_ERROR_ALREADY_INITIALIZED   (CELL_ERROR_BASE_KB | 0x03)
#define CELL_KB_ERROR_UNINITIALIZED         (CELL_ERROR_BASE_KB | 0x04)

#define CELL_KB_MAX_KEYBOARDS       2
#define CELL_KB_MAX_KEYCODES        62

#define CELL_KB_RMODE_INPUTCHAR     0
#define CELL_KB_RMODE_PACKET        1

#define CELL_KB_CODETYPE_RAW        0
#define CELL_KB_CODETYPE_ASCII      1

#define CELL_KB_STATUS_CONNECTED        1

typedef struct CellKbInfo {
    u32 max_connect;
    u32 now_connect;
    u32 info;
    u32 status[CELL_KB_MAX_KEYBOARDS];
} CellKbInfo;

s32 cellKbInit(u32 max_connect);
s32 cellKbEnd(void);
s32 cellKbGetInfo(CellKbInfo* info);
s32 cellKbSetReadMode(u32 port_no, u32 rmode);
s32 cellKbSetCodeType(u32 port_no, u32 codetype);

#ifdef __cplusplus
}
#endif

#endif
