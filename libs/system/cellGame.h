#ifndef PS3RECOMP_CELL_GAME_H
#define PS3RECOMP_CELL_GAME_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CELL_GAME_PATH_MAX          128
#define CELL_GAME_DIRNAME_SIZE      32

#define CELL_GAME_GAMETYPE_DISC     1
#define CELL_GAME_GAMETYPE_HDD      2

#define CELL_GAME_ATTRIBUTE_PATCH           (1 << 0)

#define CELL_GAME_PARAMID_TITLE                   0
#define CELL_GAME_PARAMID_TITLE_TURKISH          21
#define CELL_GAME_PARAMID_TITLE_ID              100
#define CELL_GAME_PARAMID_VERSION               101
#define CELL_GAME_PARAMID_PS3_SYSTEM_VER        105
#define CELL_GAME_PARAMID_APP_VER               106

#define CELL_GAME_SIZEKB_NOTCALC    (-1)

#define CELL_GAME_RET_NONE          2

#define CELL_GAME_ERROR_PARAM          (s32)(CELL_ERROR_BASE_SYSUTIL_GAME | 0x07)

typedef struct CellGameContentSize {
    s32 hddFreeSizeKB;
    s32 sizeKB;
    s32 sysSizeKB;
} CellGameContentSize;

typedef struct CellGameSetInitParams {
    char title[128];
    char titleId[10];
    char reserved0[2];
    char version[6];
    char reserved1[66];
} CellGameSetInitParams;

void cellGame_init_from_paramsfo(const char* sfo_path);
void cellGame_init_boot(const char* vfs_root);
const char* cellGame_get_boot_path(void);

const char* cellGame_get_hdd_title_id(void);
const char* cellGame_get_title(void);

s32 cellGameBootCheck(u32* type, u32* attributes, CellGameContentSize* size,
                       char* dirName);

s32 cellGameContentPermit(char* contentInfoPath, char* usrdirPath);

s32 cellGameDataCheck(u32 type, const char* dirName, CellGameContentSize* size);

s32 cellGameGetParamString(s32 id, char* buf, u32 bufsize);

s32 cellGameCreateGameData(CellGameSetInitParams* init, char* tmp_contentInfoPath,
                            char* tmp_usrdirPath);

s32 cellGameContentErrorDialog(s32 type, s32 errNeedSizeKB, const char* dirName);

#ifdef __cplusplus
}
#endif

#endif
