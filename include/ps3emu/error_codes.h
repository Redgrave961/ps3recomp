#ifndef PS3EMU_ERROR_CODES_H
#define PS3EMU_ERROR_CODES_H

#include <stdint.h>

#define CELL_OK                         0x00000000

#define CELL_EAGAIN                     0x80010001
#define CELL_EINVAL                     0x80010002
#define CELL_ENOSYS                     0x80010003
#define CELL_ENOMEM                     0x80010004
#define CELL_ESRCH                      0x80010005
#define CELL_ENOENT                     0x80010006
#define CELL_ENOEXEC                    0x80010007
#define CELL_EDEADLK                    0x80010008
#define CELL_EPERM                      0x80010009
#define CELL_EBUSY                      0x8001000A
#define CELL_ETIMEDOUT                  0x8001000B
#define CELL_EABORT                     0x8001000C
#define CELL_EFAULT                     0x8001000D
#define CELL_ESTAT                      0x8001000F
#define CELL_EKRESOURCE                 0x80010011
#define CELL_EISDIR                     0x80010012
#define CELL_ECANCELED                  0x80010013
#define CELL_EEXIST                     0x80010014
#define CELL_EISCONN                    0x80010015
#define CELL_ENOTCONN                   0x80010016
#define CELL_EBADF                      0x8001002A
#define CELL_EIO                        0x8001002B
#define CELL_EMFILE                     0x8001002C
#define CELL_ENOTDIR                    0x8001002E
#define CELL_ENAMETOOLONG               0x80010034
#define CELL_ENOTEMPTY                  0x80010036

#define CELL_EMUTEX_UNLOCK_NOT_OWNED    0x80010122

#define CELL_ERROR_BASE_SYSMODULE       0x80012000
#define CELL_ERROR_BASE_AUDIO           0x80310700
#define CELL_ERROR_BASE_VIDEO           0x8002B100
#define CELL_ERROR_BASE_GCM             0x80210000
#define CELL_ERROR_BASE_PAD             0x80121100
#define CELL_ERROR_BASE_KB              0x80121200
#define CELL_ERROR_BASE_MOUSE           0x80121300
#define CELL_ERROR_BASE_SYSUTIL         0x8002B000
#define CELL_ERROR_BASE_SYSUTIL_SAVE    0x8002B400
#define CELL_ERROR_BASE_SYSUTIL_GAME    0x8002CB00
#define CELL_ERROR_BASE_PNG             0x80611200

#define CELL_SYSMODULE_ERROR_DUPLICATED     (CELL_ERROR_BASE_SYSMODULE | 0x01)
#define CELL_SYSMODULE_ERROR_UNKNOWN        (CELL_ERROR_BASE_SYSMODULE | 0x02)
#define CELL_SYSMODULE_ERROR_UNLOADED       (CELL_ERROR_BASE_SYSMODULE | 0x03)

#define CELL_SYSUTIL_ERROR_VALUE            (CELL_ERROR_BASE_SYSUTIL | 0x02)
#define CELL_SYSUTIL_ERROR_NUM              (CELL_ERROR_BASE_SYSUTIL | 0x04)

#define CELL_PAD_ERROR_FATAL                (CELL_ERROR_BASE_PAD | 0x01)
#define CELL_PAD_ERROR_INVALID_PARAMETER    (CELL_ERROR_BASE_PAD | 0x02)
#define CELL_PAD_ERROR_ALREADY_OPENED       (CELL_ERROR_BASE_PAD | 0x03)
#define CELL_PAD_ERROR_NOT_OPENED           (CELL_ERROR_BASE_PAD | 0x04)

#define CELL_AUDIO_ERROR_ALREADY_INIT       (CELL_ERROR_BASE_AUDIO | 0x01)
#define CELL_AUDIO_ERROR_AUDIOSYSTEM        (CELL_ERROR_BASE_AUDIO | 0x02)
#define CELL_AUDIO_ERROR_NOT_INIT           (CELL_ERROR_BASE_AUDIO | 0x03)
#define CELL_AUDIO_ERROR_PARAM              (CELL_ERROR_BASE_AUDIO | 0x04)
#define CELL_AUDIO_ERROR_PORT_FULL          (CELL_ERROR_BASE_AUDIO | 0x05)
#define CELL_AUDIO_ERROR_PORT_ALREADY_RUN   (CELL_ERROR_BASE_AUDIO | 0x06)
#define CELL_AUDIO_ERROR_PORT_NOT_OPEN      (CELL_ERROR_BASE_AUDIO | 0x07)
#define CELL_AUDIO_ERROR_PORT_NOT_RUN       (CELL_ERROR_BASE_AUDIO | 0x08)

#define CELL_GCM_ERROR_FAILURE              (CELL_ERROR_BASE_GCM | 0xFF)
#define CELL_GCM_ERROR_NO_IO_PAGE_TABLE     (CELL_ERROR_BASE_GCM | 0x01)
#define CELL_GCM_ERROR_INVALID_ENUM         (CELL_ERROR_BASE_GCM | 0x02)
#define CELL_GCM_ERROR_INVALID_VALUE        (CELL_ERROR_BASE_GCM | 0x03)
#define CELL_GCM_ERROR_INVALID_ALIGNMENT    (CELL_ERROR_BASE_GCM | 0x04)

#define CELL_IS_ERROR(code)   (((uint32_t)(code) & 0x80000000u) != 0)

#define CELL_RETURN_IF_ERROR(expr) \
    do {                                            \
        int32_t _rc = (int32_t)(expr);              \
        if (CELL_IS_ERROR(_rc)) return _rc;         \
    } while (0)

#define CELL_MODULE_ERROR(base, code)  ((int32_t)((base) | (code)))

#endif
