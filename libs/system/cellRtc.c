#include "cellRtc.h"
#include "../../runtime/ppu/ppu_memory.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#ifdef _WIN32
#  include <windows.h>
#  include <sys/timeb.h>
#else
#  include <sys/time.h>
#endif

#define PS3_UNIX_EPOCH_DIFF  62135596800ULL

#define USEC_PER_SEC  1000000ULL

static u64 get_host_time_usec(void)
{
#ifdef _WIN32
    struct _timeb tb;
    _ftime_s(&tb);
    return (u64)tb.time * USEC_PER_SEC + (u64)tb.millitm * 1000ULL;
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (u64)tv.tv_sec * USEC_PER_SEC + (u64)tv.tv_usec;
#endif
}

static u64 time_t_to_tick(s64 t)
{
    return ((u64)t + PS3_UNIX_EPOCH_DIFF) * USEC_PER_SEC;
}

static s64 tick_to_time_t(u64 tick)
{
    return (s64)(tick / USEC_PER_SEC) - (s64)PS3_UNIX_EPOCH_DIFF;
}

static void tm_to_datetime(const struct tm* t, u32 usec, CellRtcDateTime* dt)
{
    dt->year        = (u16)(t->tm_year + 1900);
    dt->month       = (u16)(t->tm_mon + 1);
    dt->day         = (u16)t->tm_mday;
    dt->hour        = (u16)t->tm_hour;
    dt->minute      = (u16)t->tm_min;
    dt->second      = (u16)t->tm_sec;
    dt->microsecond = usec;
}

static struct tm datetime_to_tm(const CellRtcDateTime* dt)
{
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year  = dt->year - 1900;
    t.tm_mon   = dt->month - 1;
    t.tm_mday  = dt->day;
    t.tm_hour  = dt->hour;
    t.tm_min   = dt->minute;
    t.tm_sec   = dt->second;
    t.tm_isdst = -1;
    return t;
}

static int rtc_is_guest(const void* p)
{
    uintptr_t v = (uintptr_t)p;
    return v && v < 0x100000000ull;
}
static u64 rtc_tick_read(const CellRtcTick* p)
{
    return rtc_is_guest(p) ? vm_read64((uint32_t)(uintptr_t)p) : p->tick;
}
static void rtc_tick_write(CellRtcTick* p, u64 t)
{
    if (rtc_is_guest(p)) vm_write64((uint32_t)(uintptr_t)p, t);
    else                 p->tick = t;
}
static void rtc_dt_write(CellRtcDateTime* p, const CellRtcDateTime* v)
{
    if (rtc_is_guest(p)) {
        uint32_t ea = (uint32_t)(uintptr_t)p;
        vm_write16(ea + 0,  v->year);
        vm_write16(ea + 2,  v->month);
        vm_write16(ea + 4,  v->day);
        vm_write16(ea + 6,  v->hour);
        vm_write16(ea + 8,  v->minute);
        vm_write16(ea + 10, v->second);
        vm_write32(ea + 12, v->microsecond);
    } else {
        *p = *v;
    }
}
static void rtc_dt_read(const CellRtcDateTime* p, CellRtcDateTime* v)
{
    if (rtc_is_guest(p)) {
        uint32_t ea = (uint32_t)(uintptr_t)p;
        v->year        = vm_read16(ea + 0);
        v->month       = vm_read16(ea + 2);
        v->day         = vm_read16(ea + 4);
        v->hour        = vm_read16(ea + 6);
        v->minute      = vm_read16(ea + 8);
        v->second      = vm_read16(ea + 10);
        v->microsecond = vm_read32(ea + 12);
    } else {
        *v = *p;
    }
}

s32 cellRtcGetCurrentTick(CellRtcTick* pTick)
{
    if (!pTick)
        return CELL_EINVAL;

    u64 unix_usec = get_host_time_usec();
    rtc_tick_write(pTick, unix_usec + PS3_UNIX_EPOCH_DIFF * USEC_PER_SEC);

    return CELL_OK;
}

s32 cellRtcGetCurrentClock(CellRtcDateTime* pClock, s32 iTimeZone)
{
    if (!pClock)
        return CELL_EINVAL;

    u64 host_usec = get_host_time_usec();
    host_usec += (u64)((s64)iTimeZone * 60 * (s64)USEC_PER_SEC);
    time_t t = (time_t)(host_usec / USEC_PER_SEC);
    u32 usec = (u32)(host_usec % USEC_PER_SEC);

    struct tm tm_val;
#ifdef _WIN32
    gmtime_s(&tm_val, &t);
#else
    gmtime_r(&t, &tm_val);
#endif
    CellRtcDateTime dt;
    tm_to_datetime(&tm_val, usec, &dt);
    rtc_dt_write(pClock, &dt);
    return CELL_OK;
}

s32 cellRtcGetCurrentClockLocalTime(CellRtcDateTime* pClock)
{
    if (!pClock)
        return CELL_EINVAL;

    u64 host_usec = get_host_time_usec();
    time_t t = (time_t)(host_usec / USEC_PER_SEC);
    u32 usec = (u32)(host_usec % USEC_PER_SEC);

    struct tm local_tm;
#ifdef _WIN32
    localtime_s(&local_tm, &t);
#else
    localtime_r(&t, &local_tm);
#endif

    CellRtcDateTime dt_l;
    tm_to_datetime(&local_tm, usec, &dt_l);
    rtc_dt_write(pClock, &dt_l);
    return CELL_OK;
}

s32 cellRtcGetTime_t(const CellRtcTick* pTick, s64* pTime)
{
    if (!pTick || !pTime)
        return CELL_EINVAL;

    {
        s64 t = tick_to_time_t(rtc_tick_read(pTick));
        if (rtc_is_guest(pTime)) vm_write64((uint32_t)(uintptr_t)pTime, (u64)t);
        else                     *pTime = t;
    }
    return CELL_OK;
}

s32 cellRtcSetTime_t(CellRtcTick* pTick, s64 iTime)
{
    if (!pTick)
        return CELL_EINVAL;

    rtc_tick_write(pTick, time_t_to_tick(iTime));
    return CELL_OK;
}

s32 cellRtcConvertUtcToLocalTime(const CellRtcTick* pUtc, CellRtcTick* pLocalTime)
{
    if (!pUtc || !pLocalTime)
        return CELL_EINVAL;

    time_t now = time(NULL);
    struct tm local_tm, utc_tm;
#ifdef _WIN32
    localtime_s(&local_tm, &now);
    gmtime_s(&utc_tm, &now);
#else
    localtime_r(&now, &local_tm);
    gmtime_r(&now, &utc_tm);
#endif

    time_t local_t = mktime(&local_tm);
    time_t utc_t   = mktime(&utc_tm);
    s64 offset_sec = (s64)(local_t - utc_t);

    rtc_tick_write(pLocalTime, rtc_tick_read(pUtc) + (u64)(offset_sec * (s64)USEC_PER_SEC));
    return CELL_OK;
}

s32 cellRtcGetDayOfWeek(s32 year, s32 month, s32 day)
{
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    int y = year;
    if (month < 3) y -= 1;
    return (y + y/4 - y/100 + y/400 + t[month - 1] + day) % 7;
}

s32 cellRtcGetTick(const CellRtcDateTime* pDateTime, CellRtcTick* pTick)
{
    if (!pDateTime || !pTick)
        return CELL_EINVAL;

    CellRtcDateTime dt_g;
    rtc_dt_read(pDateTime, &dt_g);
    struct tm t = datetime_to_tm(&dt_g);
#ifdef _WIN32
    time_t tt = _mkgmtime(&t);
#else
    time_t tt = timegm(&t);
#endif

    rtc_tick_write(pTick, time_t_to_tick((s64)tt) + dt_g.microsecond);
    return CELL_OK;
}
