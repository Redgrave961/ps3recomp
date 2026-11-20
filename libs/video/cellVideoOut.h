#ifndef PS3RECOMP_CELL_VIDEOOUT_H
#define PS3RECOMP_CELL_VIDEOOUT_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CELL_VIDEO_OUT_PRIMARY      0

#define CELL_VIDEO_OUT_RESOLUTION_1080       1
#define CELL_VIDEO_OUT_RESOLUTION_720        2
#define CELL_VIDEO_OUT_RESOLUTION_480        4
#define CELL_VIDEO_OUT_RESOLUTION_576        5
#define CELL_VIDEO_OUT_RESOLUTION_1600x1080  10
#define CELL_VIDEO_OUT_RESOLUTION_1440x1080  11
#define CELL_VIDEO_OUT_RESOLUTION_1280x1080  12
#define CELL_VIDEO_OUT_RESOLUTION_960x1080   13

#define CELL_VIDEO_OUT_SCAN_MODE_PROGRESSIVE 1

#define CELL_VIDEO_OUT_ASPECT_AUTO   0
#define CELL_VIDEO_OUT_ASPECT_16_9   2

#define CELL_VIDEO_OUT_PORT_HDMI     0x01
#define CELL_VIDEO_OUT_DEVICE_STATE_AVAILABLE  1
#define CELL_VIDEO_OUT_DISPLAY_CONVERSION_NONE 0
#define CELL_VIDEO_OUT_REFRESH_RATE_59_94HZ    0x0001
#define CELL_VIDEO_OUT_REFRESH_RATE_60HZ       0x0004

#define CELL_VIDEO_OUT_COLOR_SPACE_RGB   0x01

#define CELL_VIDEO_OUT_BUFFER_COLOR_FORMAT_X8R8G8B8  1

#define CELL_VIDEO_OUT_ERROR_ILLEGAL_PARAMETER (s32)(CELL_ERROR_BASE_VIDEO | 0x03)
#define CELL_VIDEO_OUT_ERROR_UNSUPPORTED_VIDEO_OUT (s32)(CELL_ERROR_BASE_VIDEO | 0x06)

typedef struct CellVideoOutResolution {
    u16 width;
    u16 height;
} CellVideoOutResolution;

typedef struct CellVideoOutConfiguration {
    u8  resolutionId;
    u8  format;
    u8  aspect;
    u8  reserved[9];
    u32 pitch;
} CellVideoOutConfiguration;

typedef struct CellVideoOutDisplayMode {
    u8  resolutionId;
    u8  scanMode;
    u8  conversion;
    u8  aspect;
    u8  reserved[2];
    u16 refreshRates;
} CellVideoOutDisplayMode;

typedef struct CellVideoOutColorInfo {
    u16 redX, redY, greenX, greenY, blueX, blueY, whiteX, whiteY;
    u32 gamma;
} CellVideoOutColorInfo;

typedef struct CellVideoOutKSVList {
    u8  ksv[32 * 5];
    u8  reserved[4];
    u32 count;
} CellVideoOutKSVList;

typedef struct CellVideoOutDeviceInfo {
    u8  portType;
    u8  colorSpace;
    u16 latency;
    u8  availableModeCount;
    u8  state;
    u8  rgbOutputRange;
    u8  reserved[5];
    CellVideoOutColorInfo colorInfo;
    CellVideoOutDisplayMode availableModes[32];
    CellVideoOutKSVList ksvList;
} CellVideoOutDeviceInfo;

_Static_assert(sizeof(CellVideoOutColorInfo) == 20, "CellVideoOutColorInfo layout");
_Static_assert(sizeof(CellVideoOutDeviceInfo) == 456, "CellVideoOutDeviceInfo layout");

s32 cellVideoOutGetResolution(u32 resolutionId, CellVideoOutResolution* resolution);

s32 cellVideoOutConfigure(u32 videoOut, CellVideoOutConfiguration* config,
                            void* option, u32 waitForEvent);

s32 cellVideoOutGetDeviceInfo(u32 videoOut, u32 deviceIndex,
                               CellVideoOutDeviceInfo* info);

s32 cellVideoOutGetNumberOfDevice(u32 videoOut);

#ifdef __cplusplus
}
#endif

#endif
