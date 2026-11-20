#ifndef PS3RECOMP_CELL_MOUSE_H
#define PS3RECOMP_CELL_MOUSE_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CELL_MOUSE_ERROR_INVALID_PARAMETER  (CELL_ERROR_BASE_MOUSE | 0x02)
#define CELL_MOUSE_ERROR_ALREADY_INITIALIZED (CELL_ERROR_BASE_MOUSE | 0x03)
#define CELL_MOUSE_ERROR_UNINITIALIZED      (CELL_ERROR_BASE_MOUSE | 0x04)
#define CELL_MOUSE_ERROR_NO_DEVICE          (CELL_ERROR_BASE_MOUSE | 0x05)

#define CELL_MOUSE_MAX_MICE             2
#define CELL_MOUSE_MAX_DATA_LIST_NUM    8

#define CELL_MOUSE_STATUS_CONNECTED     1

typedef struct CellMouseData {
    u8  update;
    u8  buttons;
    s8  x_axis;
    s8  y_axis;
    s8  wheel;
    s8  tilt;
} CellMouseData;

typedef struct CellMouseDataList {
    u32          list_num;
    CellMouseData list[CELL_MOUSE_MAX_DATA_LIST_NUM];
} CellMouseDataList;

s32 cellMouseInit(u32 max_connect);
s32 cellMouseGetDataList(u32 port_no, CellMouseDataList* data);
s32 cellMouseGetInfo(u32 info_ea);

#ifdef __cplusplus
}
#endif

#endif
