#ifndef PS3RECOMP_CELL_RTC_H
#define PS3RECOMP_CELL_RTC_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CellRtcTick {
    u64 tick;
} CellRtcTick;

typedef struct CellRtcDateTime {
    u16 year;
    u16 month;
    u16 day;
    u16 hour;
    u16 minute;
    u16 second;
    u32 microsecond;
} CellRtcDateTime;

s32 cellRtcGetCurrentTick(CellRtcTick* pTick);

s32 cellRtcGetCurrentClockLocalTime(CellRtcDateTime* pClock);

s32 cellRtcGetTime_t(const CellRtcTick* pTick, s64* pTime);

s32 cellRtcSetTime_t(CellRtcTick* pTick, s64 iTime);

s32 cellRtcConvertUtcToLocalTime(const CellRtcTick* pUtc, CellRtcTick* pLocalTime);

s32 cellRtcGetDayOfWeek(s32 year, s32 month, s32 day);

s32 cellRtcGetTick(const CellRtcDateTime* pDateTime, CellRtcTick* pTick);

#ifdef __cplusplus
}
#endif

#endif
