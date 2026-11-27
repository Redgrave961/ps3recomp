#include "sysPrxForUser.h"
#include <ps3emu/guest_call.h>
#include "../../runtime/ppu/ppu_memory.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include "ps3emu/log_first.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#endif

#ifdef _WIN32
static SRWLOCK s_slot_lock = SRWLOCK_INIT;
static void slot_lock(void)   { AcquireSRWLockExclusive(&s_slot_lock); }
static void slot_unlock(void) { ReleaseSRWLockExclusive(&s_slot_lock); }
#else
static pthread_mutex_t s_slot_lock = PTHREAD_MUTEX_INITIALIZER;
static void slot_lock(void)   { pthread_mutex_lock(&s_slot_lock); }
static void slot_unlock(void) { pthread_mutex_unlock(&s_slot_lock); }
#endif

int g_sys_process_exit_called = 0;
s32 g_sys_process_exit_code   = 0;

void sys_process_exit(s32 exitcode)
{
    g_sys_process_exit_code   = exitcode;
    g_sys_process_exit_called = 1;
    printf("[sysPrxForUser] sys_process_exit(code=%d)\n", exitcode);
    exit(exitcode);
}

s32 sys_process_getpid(void)
{
    return 1001;
}

s32 sys_process_is_spu_lock_line_reservation_address(u32 addr, u64 flags)
{
    (void)addr; (void)flags;
    return CELL_OK;
}

static inline void* yz_g2h(const void* g)
{
    u32 ea = (u32)(uintptr_t)g;
    return ea ? (void*)(vm_base + ea) : (void*)0;
}

static int yz_format(char* out, size_t cap, const char* fmt, va_list ap)
{
    extern u8* vm_base;
    size_t p = 0;
    if (!out || !cap) return 0;
    if (!fmt) { out[0] = 0; return 0; }
    while (*fmt && p + 1 < cap) {
        if (*fmt != '%') { out[p++] = *fmt++; continue; }
        char spec[40]; int si = 0; spec[si++] = *fmt++;
        while (*fmt && strchr("-+ #0123456789.*lhLqjzt", *fmt) && si < 38) spec[si++] = *fmt++;
        char c = *fmt ? *fmt++ : 0; spec[si++] = c; spec[si] = 0;
        char tmp[1024]; int n = 0;
        if (c == '%') { out[p++] = '%'; continue; }
        else if (c == 's') {
            u32 g = (u32)va_arg(ap, unsigned long long);
            const char* hs = g ? (const char*)(vm_base + g) : "(null)";
            n = snprintf(tmp, sizeof tmp, spec, hs);
        } else if (c == 'c') {
            int v = (int)va_arg(ap, unsigned long long); n = snprintf(tmp, sizeof tmp, spec, v);
        } else if (c == 'p') {
            u32 v = (u32)va_arg(ap, unsigned long long); n = snprintf(tmp, sizeof tmp, "0x%08X", v);
        } else if (c == 'f' || c == 'F' || c == 'g' || c == 'G' || c == 'e' || c == 'E') {
            union { unsigned long long u; double d; } u; u.u = va_arg(ap, unsigned long long);
            n = snprintf(tmp, sizeof tmp, spec, u.d);
        } else if (c) {
            unsigned long long v = va_arg(ap, unsigned long long);
            if (strstr(spec, "ll") || strstr(spec, "q")) n = snprintf(tmp, sizeof tmp, spec, (long long)v);
            else n = snprintf(tmp, sizeof tmp, spec, (int)v);
        }
        for (int k = 0; k < n && p + 1 < cap; k++) out[p++] = tmp[k];
    }
    out[p] = 0;
    return (int)p;
}

s32 _sys_printf(const char* fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    char buf[2048];
    int ret = yz_format(buf, sizeof buf, (const char*)yz_g2h(fmt), ap);
    va_end(ap);
    printf("[PS3] %s", buf);
    return ret;
}

s32 _sys_trap_process(const char* msg)
{
    const char* m = (const char*)yz_g2h(msg);
    printf("[PS3] %s", m ? m : "(trap)\n");
    return CELL_OK;
}

s32 _sys_snprintf(char* buf, u32 size, const char* fmt, ...)
{
    if (!size) return 0;
    va_list ap; va_start(ap, fmt);
    int ret = yz_format((char*)yz_g2h(buf), size, (const char*)yz_g2h(fmt), ap);
    va_end(ap);
    return ret;
}

s32 _sys_strlen(const char* str)
{
    char* s = (char*)yz_g2h(str);
    if (!s) return 0;
    return (s32)strlen(s);
}

s32 _sys_strncpy(char* dst, const char* src, u32 size)
{
    char* d = (char*)yz_g2h(dst); const char* s = (const char*)yz_g2h(src);
    if (!d || !s) return CELL_EFAULT;
    strncpy(d, s, size);
    return CELL_OK;
}

s32 _sys_strcat(char* dst, const char* src)
{
    char* d = (char*)yz_g2h(dst); const char* s = (const char*)yz_g2h(src);
    if (!d || !s) return CELL_EFAULT;
    strcat(d, s);
    return CELL_OK;
}

char* _sys_strcpy(char* dst, const char* src)
{
    char* d = (char*)yz_g2h(dst); const char* s = (const char*)yz_g2h(src);
    if (d && s) strcpy(d, s);
    return dst;
}

char* _sys_strncat(char* dst, const char* src, u32 size)
{
    char* d = (char*)yz_g2h(dst); const char* s = (const char*)yz_g2h(src);
    if (d && s) strncat(d, s, size);
    return dst;
}

s32 _sys_strncmp(const char* s1, const char* s2, u32 size)
{
    const char* a = (const char*)yz_g2h(s1); const char* b = (const char*)yz_g2h(s2);
    if (!a || !b) return -1;
    return strncmp(a, b, size);
}

void* _sys_memset(void* dst, s32 val, u32 size)
{
    void* d = yz_g2h(dst);
    if (d) { memset(d, val, size); vm_track_write((uint32_t)(uintptr_t)dst, size); }
    return dst;
}

void* _sys_memcpy(void* dst, const void* src, u32 size)
{
    void* d = yz_g2h(dst); const void* s = yz_g2h(src);
    if (d && s) { memcpy(d, s, size); vm_track_write((uint32_t)(uintptr_t)dst, size); }
    return dst;
}

s32 _sys_memcmp(const void* s1, const void* s2, u32 size)
{
    const void* a = yz_g2h(s1); const void* b = yz_g2h(s2);
    if (!a || !b) return 0;
    return memcmp(a, b, size);
}

s32 sys_ppu_thread_once(u32 once_ctrl_ea, u32 init_opd)
{
    if (!once_ctrl_ea)
        return (s32)CELL_EINVAL;

    for (;;) {
        slot_lock();
        const u32 st = vm_read32(once_ctrl_ea);
        if (st == 1u) {
            slot_unlock();
            return CELL_OK;
        }
        if (st == 0u) {
            vm_write32(once_ctrl_ea, 2u);
            slot_unlock();
            break;
        }
        slot_unlock();
#ifdef _WIN32
        SwitchToThread();
#else
        sched_yield();
#endif
    }

    if (init_opd && g_ps3_guest_caller) {
        static int n = 0;
        if (PS3_LOG_FIRST(n, 8))
            printf("[sysPrxForUser] sys_ppu_thread_once: running init 0x%08X"
                   " (ctrl 0x%08X)\n", init_opd, once_ctrl_ea);
        g_ps3_guest_caller(init_opd, 0, 0, 0, 0, 0, 0, 0, 0);
    }
    slot_lock();
    vm_write32(once_ctrl_ea, 1u);
    slot_unlock();
    return CELL_OK;
}

static volatile u32 s_spu_printf_cb[4];

static s32 spu_printf_callback(unsigned which, u32 arg)
{
    const u32 opd = s_spu_printf_cb[which];
    if (!opd)
        return (s32)CELL_ESTAT;
    uint64_t r3 = 0;
    if (!ppu_guest_call_ct_checked(vm_read32(opd), vm_read32(opd + 4), arg, 0, 0, 0, 0, 0, 0, 0, &r3))
        return (s32)CELL_ESTAT;
    return (s32)(u32)r3;
}

s32 _sys_spu_printf_initialize(u32 agcb, u32 dgcb, u32 atcb, u32 dtcb)
{
    s_spu_printf_cb[0] = agcb;
    s_spu_printf_cb[1] = dgcb;
    s_spu_printf_cb[2] = atcb;
    s_spu_printf_cb[3] = dtcb;
    return CELL_OK;
}

s32 _sys_spu_printf_finalize(void)
{
    for (unsigned i = 0; i < 4; i++)
        s_spu_printf_cb[i] = 0;
    return CELL_OK;
}

s32 _sys_spu_printf_attach_group(u32 group)   { return spu_printf_callback(0, group); }
s32 _sys_spu_printf_detach_group(u32 group)   { return spu_printf_callback(1, group); }

s32 sys_prx_exitspawn_with_level(void)
{
    printf("[sysPrxForUser] sys_prx_exitspawn_with_level() - no-op\n");
    return CELL_OK;
}

s32 sys_prx_get_module_id_by_name(const char* name, u64 flags, u32* id)
{
    (void)flags;
    const char* hname = (const char*)yz_g2h(name);
    u32* hid = (u32*)yz_g2h(id);
    printf("[sysPrxForUser] sys_prx_get_module_id_by_name('%s')\n",
           hname ? hname : "(null)");

    if (hid) *hid = 0;
    return (s32)0x8001112E;
}

s32 sys_process_get_paramsfo(void* buf)
{
    if (!buf) return CELL_EFAULT;
    memset(yz_g2h(buf), 0, 256);
    return CELL_OK;
}
