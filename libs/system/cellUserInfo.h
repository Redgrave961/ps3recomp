#ifndef PS3RECOMP_CELL_USERINFO_H
#define PS3RECOMP_CELL_USERINFO_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CELL_USERINFO_USER_MAX              16

#define CELL_USERINFO_DEFAULT_USER_ID       1

typedef struct CellUserInfoUserList {
    u32 userId[CELL_USERINFO_USER_MAX];
} CellUserInfoUserList;

s32 cellUserInfoGetList(u32* listNum, CellUserInfoUserList* list, u32* currentUser);

#ifdef __cplusplus
}
#endif

#endif
