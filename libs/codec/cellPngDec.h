#ifndef PS3RECOMP_CELL_PNGDEC_H
#define PS3RECOMP_CELL_PNGDEC_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CELL_PNGDEC_ERROR_HEADER        (CELL_ERROR_BASE_PNG | 0x01)
#define CELL_PNGDEC_ERROR_STREAM_FORMAT (CELL_ERROR_BASE_PNG | 0x02)
#define CELL_PNGDEC_ERROR_ARG           (CELL_ERROR_BASE_PNG | 0x03)
#define CELL_PNGDEC_ERROR_SEQ           (CELL_ERROR_BASE_PNG | 0x04)
#define CELL_PNGDEC_ERROR_BUSY          (CELL_ERROR_BASE_PNG | 0x05)
#define CELL_PNGDEC_ERROR_OPEN_FILE     (CELL_ERROR_BASE_PNG | 0x07)

#define CELL_PNGDEC_FILE        0
#define CELL_PNGDEC_BUFFER      1

#define CELL_PNGDEC_GRAYSCALE       1
#define CELL_PNGDEC_RGB             2
#define CELL_PNGDEC_GRAYSCALE_ALPHA 9
#define CELL_PNGDEC_RGBA            10

#define CELL_PNGDEC_COLOR_SPACE_GRAYSCALE       1
#define CELL_PNGDEC_COLOR_SPACE_RGB             2
#define CELL_PNGDEC_COLOR_SPACE_RGBA            10
#define CELL_PNGDEC_COLOR_SPACE_ARGB            20

#define CELL_PNGDEC_DEC_STATUS_FINISH   0
#define CELL_PNGDEC_DEC_STATUS_STOP     1

#define CELL_PNGDEC_MAX_PATH    1024

typedef u32 CellPngDecMainHandle;
typedef u32 CellPngDecSubHandle;

typedef struct CellPngDecSrc {
    u32         srcSelect;
    const char* fileName;
    u64         fileOffset;
    u32         fileSize;
    u32         streamPtr;
    u32         streamSize;
} CellPngDecSrc;

typedef struct CellPngDecInfo {
    u32  imageWidth;
    u32  imageHeight;
    u32  numComponents;
    u32  colorSpace;
    u32  bitDepth;
    u32  interlaceMethod;
    u32  chunkInformation;
} CellPngDecInfo;

typedef struct CellPngDecInParam {
    u32  commandPtr;
    u32  outputMode;
    u32  outputColorSpace;
    u32  outputBitDepth;
    u32  outputPackFlag;
    u32  outputAlphaSelect;
    u32  outputColorAlpha;
} CellPngDecInParam;

s32 cellPngDecDestroy(CellPngDecMainHandle mainHandle);

s32 cellPngDecOpen(CellPngDecMainHandle mainHandle,
                   u32 subHandle_addr, u32 src_addr, u32 openInfo_addr);

s32 cellPngDecReadHeader(CellPngDecMainHandle mainHandle,
                         CellPngDecSubHandle subHandle,
                         u32 info_addr);

s32 cellPngDecSetParameter(CellPngDecMainHandle mainHandle,
                           CellPngDecSubHandle subHandle,
                           u32 inParam_addr, u32 outParam_addr);

s32 cellPngDecDecodeData(CellPngDecMainHandle mainHandle,
                         CellPngDecSubHandle subHandle,
                         u32 data_addr, u32 dataCtrlParam_addr, u32 dataOutInfo_addr);

s32 cellPngDecClose(CellPngDecMainHandle mainHandle,
                    CellPngDecSubHandle subHandle);

#ifdef __cplusplus
}
#endif

#endif
