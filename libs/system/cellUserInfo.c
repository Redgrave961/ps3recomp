/*
 * ps3recomp - cellUserInfo HLE implementation
 *
 * Reports a single default user (ID=00000001, name="User").
 */

#include "cellUserInfo.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../../runtime/ppu/ppu_memory.h"   /* vm_base (guest mem) */
/* HLE args arrive as guest effective addresses; translate before deref. */
/* The guest is big-endian; u32 fields we write into guest structs must be stored
 * byte-swapped or the guest reads them mangled (e.g. a count of 1 read as
 * 0x01000000 = 16.7M, blowing up the caller's enumeration loop). */
#define BE32(v) ( (((uint32_t)(v) & 0x000000FFu) << 24) | (((uint32_t)(v) & 0x0000FF00u) << 8) | \
                  (((uint32_t)(v) & 0x00FF0000u) >> 8) | (((uint32_t)(v) & 0xFF000000u) >> 24) )

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

/* The emulated PS3 has one user */
#define EMU_USER_ID     CELL_USERINFO_DEFAULT_USER_ID

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

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

