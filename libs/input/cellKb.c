#include "cellKb.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

extern void vm_write8 (unsigned long long a, unsigned char  v);
extern void vm_write32(unsigned long long a, unsigned int   v);

static int s_kb_initialized = 0;
static u32 s_kb_max_connect = 0;
static int s_kb_connected[CELL_KB_MAX_KEYBOARDS];

s32 cellKbInit(u32 max_connect)
{
    printf("[cellKb] Init(max_connect=%u)\n", max_connect);

    if (s_kb_initialized)
        return CELL_KB_ERROR_ALREADY_INITIALIZED;

    if (max_connect == 0 || max_connect > CELL_KB_MAX_KEYBOARDS)
        return CELL_KB_ERROR_INVALID_PARAMETER;

    s_kb_initialized = 1;
    s_kb_max_connect = max_connect;
    memset(s_kb_connected, 0, sizeof(s_kb_connected));

    s_kb_connected[0] = 1;

    return CELL_OK;
}

s32 cellKbEnd(void)
{
    printf("[cellKb] End()\n");

    if (!s_kb_initialized)
        return CELL_KB_ERROR_UNINITIALIZED;

    s_kb_initialized = 0;
    return CELL_OK;
}

s32 cellKbGetInfo(CellKbInfo* info)
{
    unsigned long long ea = (unsigned int)(uintptr_t)info;

    if (!s_kb_initialized)
        return CELL_KB_ERROR_UNINITIALIZED;

    if (!info)
        return CELL_KB_ERROR_INVALID_PARAMETER;

    vm_write32(ea + 0x00, s_kb_max_connect);
    vm_write32(ea + 0x08, 0);

    u32 connected = 0;
    for (u32 i = 0; i < s_kb_max_connect; i++) {
        int on = s_kb_connected[i];
        vm_write8(ea + 0x0C + i, on ? CELL_KB_STATUS_CONNECTED : 0);
        if (on) connected++;
    }
    vm_write32(ea + 0x04, connected);

    return CELL_OK;
}

s32 cellKbSetReadMode(u32 port_no, u32 rmode)
{
    printf("[cellKb] SetReadMode(port=%u, rmode=%u)\n", port_no, rmode);

    if (!s_kb_initialized)
        return CELL_KB_ERROR_UNINITIALIZED;

    if (port_no >= CELL_KB_MAX_KEYBOARDS)
        return CELL_KB_ERROR_INVALID_PARAMETER;

    if (rmode != CELL_KB_RMODE_INPUTCHAR && rmode != CELL_KB_RMODE_PACKET)
        return CELL_KB_ERROR_INVALID_PARAMETER;

    return CELL_OK;
}

s32 cellKbSetCodeType(u32 port_no, u32 codetype)
{
    printf("[cellKb] SetCodeType(port=%u, codetype=%u)\n", port_no, codetype);

    if (!s_kb_initialized)
        return CELL_KB_ERROR_UNINITIALIZED;

    if (port_no >= CELL_KB_MAX_KEYBOARDS)
        return CELL_KB_ERROR_INVALID_PARAMETER;

    if (codetype != CELL_KB_CODETYPE_RAW && codetype != CELL_KB_CODETYPE_ASCII)
        return CELL_KB_ERROR_INVALID_PARAMETER;

    return CELL_OK;
}
