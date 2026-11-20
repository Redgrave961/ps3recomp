#ifndef PS3RECOMP_CELL_MSGDIALOG_H
#define PS3RECOMP_CELL_MSGDIALOG_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CELL_MSGDIALOG_TYPE_SE_TYPE_NORMAL         0x00000001
#define CELL_MSGDIALOG_TYPE_BUTTON_TYPE_NONE       0x00000000
#define CELL_MSGDIALOG_TYPE_BUTTON_TYPE_YESNO      0x00000010
#define CELL_MSGDIALOG_TYPE_BUTTON_TYPE_OK         0x00000020
#define CELL_MSGDIALOG_TYPE_PROGRESSBAR_SINGLE     0x00001000

#define CELL_MSGDIALOG_BUTTON_NONE     (-1)
#define CELL_MSGDIALOG_BUTTON_INVALID  0
#define CELL_MSGDIALOG_BUTTON_OK       1
#define CELL_MSGDIALOG_BUTTON_YES      1
#define CELL_MSGDIALOG_BUTTON_NO       2

#define CELL_MSGDIALOG_ERROR_PARAM         (s32)(CELL_ERROR_BASE_SYSUTIL | 0x01)
#define CELL_MSGDIALOG_ERROR_DIALOG_NOT_OPENED (s32)(CELL_ERROR_BASE_SYSUTIL | 0x10)

typedef u32 CellMsgDialogType;

typedef void (*CellMsgDialogCallback)(s32 buttonType, void* userdata);

s32 cellMsgDialogOpen2(CellMsgDialogType type, const char* msgString,
                        CellMsgDialogCallback callback, void* userdata,
                        void* extParam);

s32 cellMsgDialogClose(float delayMs);

s32 cellMsgDialogAbort(void);

s32 cellMsgDialogProgressBarInc(u32 progressBarIndex, u32 delta);

#ifdef __cplusplus
}
#endif

#endif
