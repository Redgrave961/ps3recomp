#ifndef PS3RECOMP_CELL_OSK_DIALOG_H
#define PS3RECOMP_CELL_OSK_DIALOG_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CELL_OSK_ERROR_NOT_INITIALIZED          0x8002B301
#define CELL_OSK_ERROR_DIALOG_NOT_LOADED        0x8002B304
#define CELL_OSK_ERROR_DIALOG_ALREADY_LOADED    0x8002B305

#define CELL_OSK_DIALOG_INPUT_FIELD_RESULT_OK           0

#define CELL_OSK_MAX_TEXT_LENGTH                 512

typedef u16 CellOskDialogChar;

typedef struct CellOskDialogInputFieldInfo {
    CellOskDialogChar*  message;
    CellOskDialogChar*  init_text;
    u32                 limit_length;
} CellOskDialogInputFieldInfo;

typedef struct CellOskDialogCallbackReturnParam {
    s32                 result;
    u32                 numCharsResultString;
    CellOskDialogChar*  pResultString;
} CellOskDialogCallbackReturnParam;

typedef struct CellOskDialogPoint {
    float x;
    float y;
} CellOskDialogPoint;

typedef struct CellOskDialogParam {
    u32                 allowOskPanelFlg;
    u32                 firstViewPanel;
    CellOskDialogPoint  controlPoint;
    s32                 prohibitFlgs;
} CellOskDialogParam;

s32 cellOskDialogLoadAsync(u32 container,
                           const CellOskDialogParam* dialogParam,
                           const CellOskDialogInputFieldInfo* inputFieldInfo);
s32 cellOskDialogUnloadAsync(CellOskDialogCallbackReturnParam* result);

s32 cellOskDialogAbort(void);

s32 cellOskDialogSetInitialKeyLayout(u32 layout);
s32 cellOskDialogSetLayoutMode(u32 mode);

#ifdef __cplusplus
}
#endif

#endif
