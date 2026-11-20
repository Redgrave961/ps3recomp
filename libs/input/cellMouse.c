#include "cellMouse.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

extern void vm_write8 (unsigned long long a, unsigned char  v);
extern void vm_write32(unsigned long long a, unsigned int   v);

static int            s_mouse_initialized = 0;
static u32            s_mouse_max_connect = 0;

s32 cellMouseInit(u32 max_connect)
{
    printf("[cellMouse] Init(max_connect=%u)\n", max_connect);

    if (s_mouse_initialized)
        return CELL_MOUSE_ERROR_ALREADY_INITIALIZED;

    if (max_connect == 0 || max_connect > CELL_MOUSE_MAX_MICE)
        return CELL_MOUSE_ERROR_INVALID_PARAMETER;

    s_mouse_initialized = 1;
    s_mouse_max_connect = max_connect;

    return CELL_OK;
}

s32 cellMouseGetDataList(u32 port_no, CellMouseDataList* data)
{
    unsigned long long ea = (unsigned int)(uintptr_t)data;

    if (!s_mouse_initialized)
        return CELL_MOUSE_ERROR_UNINITIALIZED;

    if (port_no >= s_mouse_max_connect || !data)
        return CELL_MOUSE_ERROR_INVALID_PARAMETER;

    vm_write32(ea + 0x00, 0);
    return CELL_MOUSE_ERROR_NO_DEVICE;
}

#define MOUSE_INFO_SIZE      0x287u

s32 cellMouseGetInfo(u32 info_ea)
{
    if (!s_mouse_initialized)
        return CELL_MOUSE_ERROR_UNINITIALIZED;

    if (!info_ea)
        return CELL_MOUSE_ERROR_INVALID_PARAMETER;

    for (u32 i = 0; i < MOUSE_INFO_SIZE; i++)
        vm_write8((unsigned long long)info_ea + i, 0);

    vm_write32((unsigned long long)info_ea + 0, s_mouse_max_connect);

    return CELL_OK;
}
