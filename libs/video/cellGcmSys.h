#ifndef PS3RECOMP_CELL_GCM_SYS_H
#define PS3RECOMP_CELL_GCM_SYS_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CELL_GCM_DISPLAY_VSYNC              2

#define CELL_GCM_FLIP_STATUS_WAITING        1

#define CELL_GCM_LOCATION_LOCAL             0
#define CELL_GCM_LOCATION_MAIN              1

#define CELL_GCM_MAX_DISPLAY_BUFFER_NUM     8
#define CELL_GCM_MAX_TILE_COUNT             15
#define CELL_GCM_MAX_ZCULL_COUNT            8
#define CELL_GCM_MAX_LOCAL_REPORT_COUNT     2048
#define CELL_GCM_MAX_MAIN_REPORT_COUNT      (1024 * 1024)

s32 _cellGcmInitBody(u32 context, u32 cmdSize, u32 ioSize, u32 ioAddress);
s32 cellGcmSetDisplayBuffer(u8 id, u32 offset, u32 pitch, u32 width, u32 height);
u32 cellGcmGetLabelAddress(u8 index);

#ifdef __cplusplus
}
#endif

#endif
