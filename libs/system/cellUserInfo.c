#include "cellUserInfo.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../../runtime/ppu/ppu_memory.h"
#define BE32(v) ( (((uint32_t)(v) & 0x000000FFu) << 24) | (((uint32_t)(v) & 0x0000FF00u) << 8) | \
                  (((uint32_t)(v) & 0x00FF0000u) >> 8) | (((uint32_t)(v) & 0xFF000000u) >> 24) )

#define EMU_USER_ID     CELL_USERINFO_DEFAULT_USER_ID

s32 cellUserInfoGetList(u32* listNum, CellUserInfoUserList* list, u32* currentUser)
{
    printf("[cellUserInfo] GetList()\n");
    u32* listNum_h = GUEST_PTR(listNum, u32*);
    CellUserInfoUserList* list_h = GUEST_PTR(list, CellUserInfoUserList*);
    u32* currentUser_h = GUEST_PTR(currentUser, u32*);

    if (listNum_h)
        *listNum_h = BE32(1);

    if (list_h) {
        memset(list_h, 0, sizeof(CellUserInfoUserList));
        list_h->userId[0] = BE32(EMU_USER_ID);
    }

    if (currentUser_h)
        *currentUser_h = BE32(EMU_USER_ID);

    return CELL_OK;
}
