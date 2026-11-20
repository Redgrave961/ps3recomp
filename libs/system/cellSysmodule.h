#ifndef PS3RECOMP_CELL_SYSMODULE_H
#define PS3RECOMP_CELL_SYSMODULE_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CELL_SYSMODULE_NET                  0x0000
#define CELL_SYSMODULE_HTTP                 0x0001
#define CELL_SYSMODULE_HTTP_UTIL            0x0002
#define CELL_SYSMODULE_SSL                  0x0003
#define CELL_SYSMODULE_HTTPS                0x0004
#define CELL_SYSMODULE_VDEC                 0x0005
#define CELL_SYSMODULE_ADEC                 0x0006
#define CELL_SYSMODULE_DMUX                 0x0007
#define CELL_SYSMODULE_VPOST                0x0008
#define CELL_SYSMODULE_RTC                  0x0009
#define CELL_SYSMODULE_SPURS                0x000A
#define CELL_SYSMODULE_OVIS                 0x000B
#define CELL_SYSMODULE_SHEAP                0x000C
#define CELL_SYSMODULE_SYNC                 0x000D
#define CELL_SYSMODULE_FS                   0x000E
#define CELL_SYSMODULE_JPGDEC               0x000F
#define CELL_SYSMODULE_GCM_SYS              0x0010
#define CELL_SYSMODULE_AUDIO                0x0011
#define CELL_SYSMODULE_PAMF                 0x0012
#define CELL_SYSMODULE_ATRAC3PLUS           0x0013
#define CELL_SYSMODULE_NETCTL               0x0014
#define CELL_SYSMODULE_SYSUTIL              0x0015
#define CELL_SYSMODULE_SYSUTIL_NP           0x0016
#define CELL_SYSMODULE_IO                   0x0017
#define CELL_SYSMODULE_PNGDEC               0x0018
#define CELL_SYSMODULE_FONT                 0x0019
#define CELL_SYSMODULE_FONTFT               0x001A
#define CELL_SYSMODULE_FREETYPE             0x001B
#define CELL_SYSMODULE_USBD                 0x001C
#define CELL_SYSMODULE_SAIL                 0x001D
#define CELL_SYSMODULE_L10N                 0x001E
#define CELL_SYSMODULE_RESC                 0x001F
#define CELL_SYSMODULE_MIC                  0x0022
#define CELL_SYSMODULE_CAMERA               0x0023
#define CELL_SYSMODULE_LV2DBG               0x002E
#define CELL_SYSMODULE_USBPSPCM             0x0030
#define CELL_SYSMODULE_AVCONF_EXT           0x0031
#define CELL_SYSMODULE_SYSUTIL_USERINFO     0x0032
#define CELL_SYSMODULE_SYSUTIL_SAVEDATA     0x0033
#define CELL_SYSMODULE_SYSUTIL_GAME_EXEC    0x0037
#define CELL_SYSMODULE_SYSUTIL_NP2          0x0038
#define CELL_SYSMODULE_SYSUTIL_GAME         0x003E
#define CELL_SYSMODULE_FIBER                0x0043
#define CELL_SYSMODULE_SYNC2                0x0055
#define CELL_SYSMODULE_GEM                  0x005A
#define CELL_SYSMODULE_SYSUTIL_NP_TROPHY    0xF035

#define CELL_SYSMODULE_MAX_ID       0x0100

s32 cellSysmoduleInitialize(void);
s32 cellSysmoduleLoadModule(u16 id);

s32 cellSysmoduleUnloadModule(u16 id);

#ifdef __cplusplus
}
#endif

#endif
