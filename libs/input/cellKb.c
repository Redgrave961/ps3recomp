/*
 * ps3recomp - cellKb HLE implementation
 *
 * Keyboard input. Tracks key state from host keyboard events and provides
 * data in PS3 CellKbData format.
 *
 * The host input layer (SDL3 event loop, etc.) should call cellKb_keyEvent()
 * whenever a key is pressed or released.
 */

#include "cellKb.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* HLE args are GUEST effective addresses; guest structs are BIG-ENDIAN. Write
 * through the vm accessors (cellPad.c idiom) -- dereferencing the "pointer"
 * as host memory faults (LBP: cellKbGetInfo memset at guest-stack EA
 * 0x0FEFF814 crashed the boot right after the loading phase). */
extern void vm_write8 (unsigned long long a, unsigned char  v);
extern void vm_write16(unsigned long long a, unsigned short v);
extern void vm_write32(unsigned long long a, unsigned int   v);

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

#define KB_MAX_TRACKED_KEYS 256

typedef struct {
    int  connected;
    u32  rmode;         /* CELL_KB_RMODE_INPUTCHAR or CELL_KB_RMODE_PACKET */
    u32  codetype;      /* CELL_KB_CODETYPE_RAW or CELL_KB_CODETYPE_ASCII */
    u32  mkey;          /* Current modifier flags */
    u32  led;           /* Current LED flags */
    u8   key_state[KB_MAX_TRACKED_KEYS]; /* 1 = pressed */
    /* Ring buffer for key events (PACKET mode) */
    u16  event_buf[CELL_KB_MAX_KEYCODES];
    s32  event_count;
} KbPortState;

static int         s_kb_initialized = 0;
static u32         s_kb_max_connect = 0;
static KbPortState s_kb_ports[CELL_KB_MAX_KEYBOARDS];

/* ---------------------------------------------------------------------------
 * Host key event injection
 * -----------------------------------------------------------------------*/

/* ---------------------------------------------------------------------------
 * Keycode conversion: RAW (USB HID) to ASCII
 * -----------------------------------------------------------------------*/

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellKbInit(u32 max_connect)
{
    printf("[cellKb] Init(max_connect=%u)\n", max_connect);

    if (s_kb_initialized)
        return CELL_KB_ERROR_ALREADY_INITIALIZED;

    if (max_connect == 0 || max_connect > CELL_KB_MAX_KEYBOARDS)
        return CELL_KB_ERROR_INVALID_PARAMETER;

    s_kb_initialized = 1;
    s_kb_max_connect = max_connect;
    memset(s_kb_ports, 0, sizeof(s_kb_ports));

    /* Assume keyboard 0 is always connected on the host */
    s_kb_ports[0].connected = 1;
    s_kb_ports[0].rmode = CELL_KB_RMODE_INPUTCHAR;
    s_kb_ports[0].codetype = CELL_KB_CODETYPE_ASCII;

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

/* Guest CellKbInfo layout (big-endian): +0x00 max_connect u32, +0x04
 * now_connect u32, +0x08 info u32, +0x0C status[] u8 PER KEYBOARD (bytes,
 * not u32s -- SDK/RPCS3 layout). */
s32 cellKbGetInfo(CellKbInfo* info)
{
    unsigned long long ea = (unsigned int)(uintptr_t)info;

    if (!s_kb_initialized)
        return CELL_KB_ERROR_UNINITIALIZED;

    if (!info)
        return CELL_KB_ERROR_INVALID_PARAMETER;

    vm_write32(ea + 0x00, s_kb_max_connect);
    vm_write32(ea + 0x08, 0);                      /* system info flags */

    u32 connected = 0;
    for (u32 i = 0; i < s_kb_max_connect; i++) {
        int on = s_kb_ports[i].connected;
        vm_write8(ea + 0x0C + i, on ? CELL_KB_STATUS_CONNECTED : 0);
        if (on) connected++;
    }
    vm_write32(ea + 0x04, connected);              /* now_connect */

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

    s_kb_ports[port_no].rmode = rmode;
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

    s_kb_ports[port_no].codetype = codetype;
    return CELL_OK;
}

