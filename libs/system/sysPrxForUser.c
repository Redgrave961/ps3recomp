/*
 * ps3recomp - sysPrxForUser HLE implementation
 */

#include "sysPrxForUser.h"
#include <ps3emu/guest_call.h>
#include "../../runtime/ppu/ppu_memory.h"   /* vm_base, vm_write32: translate + byte-swap */
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

/* ---------------------------------------------------------------------------
 * Process management
 * -----------------------------------------------------------------------*/

/* The status the guest asked to exit with, published before the host exit()
 * runs. exit() gives its argument to _exit and to nothing else -- an atexit
 * handler cannot read it -- so without this the only place a guest's chosen
 * status is visible is the process's own exit code. That forces a harness to
 * exit with the guest's status rather than with its own verdict about the run,
 * and makes "the guest asked for 0" indistinguishable from "the harness fell
 * through and returned 0". */
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

/* ---------------------------------------------------------------------------
 * String/memory functions
 * -----------------------------------------------------------------------*/

/* libsre and other system PRXs reach these sysPrxForUser CRT shims through
 * ps3_hle_call, which forwards the raw GUEST effective address in the arg
 * registers. Translate guest EA -> host pointer (vm_base + 32-bit EA; guest
 * NULL stays NULL) before touching memory. The title itself inlines its own
 * memcpy/memset/str*, so this path first appears once the real libsre is
 * loaded -- without translation memset(guest_ea, ...) writes to a bare host
 * address and AVs (observed: _sys_memset write fault during SPURS init). */
extern u8* vm_base;
static inline void* yz_g2h(const void* g)
{
    u32 ea = (u32)(uintptr_t)g;
    return ea ? (void*)(vm_base + ea) : (void*)0;
}

/* Guest-aware printf core: the varargs reach us as raw 64-bit register values
 * (the generic HLE adapter forwards gpr3..10), so a %s argument is a GUEST
 * effective address -- passing it straight to the host vprintf derefs it as a
 * host pointer and AVs (observed crashing the libspurs assert printer on the
 * "handler.c" filename). Walk the (already host-translated) format and emit each
 * conversion via the host snprintf, but translate %s args through vm_base. */
static int yz_format(char* out, size_t cap, const char* fmt, va_list ap)
{
    extern u8* vm_base;
    size_t p = 0;
    if (!fmt) { if (cap) out[0] = 0; return 0; }
    while (*fmt && p + 1 < cap) {
        if (*fmt != '%') { out[p++] = *fmt++; continue; }
        char spec[40]; int si = 0; spec[si++] = *fmt++;          /* '%' */
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

/* The abort half of the Cell SDK's internal assertion macro. Every one of its
 * 80 call sites in libsre is the same two-instruction pair: _sys_printf with
 * the "PS3 SDK INTERNAL ASSERTION FAILURE" format, then this with a short
 * message -- "Aborted.", or "The SPURS is aborted." when SPURS is the one
 * giving up. One argument, a guest pointer to that message.
 *
 * It is not noreturn. The compiler emits an ordinary return sequence after
 * each call, because on retail hardware with no debugger attached the trap is
 * taken, ignored, and the caller unwinds itself -- which is why a title whose
 * SPURS has aborted goes on to walk its own call chain, print it, and exit the
 * thread rather than stopping here.
 *
 * So printing the message and returning is the faithful behaviour, not a stub
 * standing in for something better. What it changes is that the abort is
 * legible: without it the call is an unresolved NID answering CELL_ENOSYS in a
 * trace, three lines after the assertion text that explains it, with nothing
 * connecting the two. */
s32 _sys_trap_process(const char* msg)
{
    const char* m = (const char*)yz_g2h(msg);
    printf("[PS3] %s", m ? m : "(trap)\n");
    return CELL_OK;
}

s32 _sys_snprintf(char* buf, u32 size, const char* fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int ret = yz_format((char*)yz_g2h(buf), size ? size : 1, (const char*)yz_g2h(fmt), ap);
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

/* Returns the destination GUEST pointer (callers such as cellSpurs use the
 * return value), not the host pointer. */
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
    if (d) memset(d, val, size);
    return dst;
}

void* _sys_memcpy(void* dst, const void* src, u32 size)
{
    void* d = yz_g2h(dst); const void* s = yz_g2h(src);
    if (d && s) memcpy(d, s, size);
    return dst;
}

s32 _sys_memcmp(const void* s1, const void* s2, u32 size)
{
    const void* a = yz_g2h(s1); const void* b = yz_g2h(s2);
    if (!a || !b) return 0;
    return memcmp(a, b, size);
}

/* sys_ppu_thread_once(once_ctrl, init)
 *
 * Runs `init` exactly once, the pthread_once of the lv2 user library. It was
 * not implemented at all, so it fell through to the unresolved-NID handler,
 * which returns CELL_OK -- success, without ever running the initialiser. Every
 * subsystem that brings itself up this way was therefore left uninitialised
 * while its caller was told the init had happened, and the failure only shows
 * up later as a state error from the first call that needs it.
 *
 * Tokyo Jungle reaches its SPURS bring-up through one of these; with the
 * initialiser skipped, the very next check returns CELL_ESTAT and the whole
 * bring-up -- including the allocation of the buffer its loader reads into --
 * is abandoned.
 *
 * `once_ctrl` is a guest word, zero before the first call (SYS_PPU_THREAD_ONCE_
 * INIT). `init` is a function DESCRIPTOR, so it goes through the guest caller.
 * ponytail: reuses the slot lock rather than a per-control one -- the call is
 * rare and only ever contended at start-up. */

s32 sys_ppu_thread_once(u32 once_ctrl_ea, u32 init_opd)
{
    if (!once_ctrl_ea)
        return (s32)CELL_EINVAL;

    slot_lock();
    const int first = (vm_read32(once_ctrl_ea) == 0);
    if (first)
        vm_write32(once_ctrl_ea, 1);
    slot_unlock();

    if (first && init_opd && g_ps3_guest_caller) {
        static int n = 0;
        if (PS3_LOG_FIRST(n, 8))
            printf("[sysPrxForUser] sys_ppu_thread_once: running init 0x%08X"
                   " (ctrl 0x%08X)\n", init_opd, once_ctrl_ea);
        g_ps3_guest_caller(init_opd, 0, 0, 0, 0, 0, 0, 0, 0);
    }
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

/* ---------------------------------------------------------------------------
 * PRX utilities
 * -----------------------------------------------------------------------*/

s32 sys_prx_exitspawn_with_level(void)
{
    printf("[sysPrxForUser] sys_prx_exitspawn_with_level() - no-op\n");
    return CELL_OK;
}

s32 sys_prx_get_module_id_by_name(const char* name, u64 flags, u32* id)
{
    (void)flags;
    /* name/id arrive as raw GUEST effective addresses (the generic HLE adapter
     * forwards gpr3.. untranslated). Translate before touching memory -- else
     * printf("%s", name) derefs a bare host address and AVs. This path first
     * appears once real libsre is loaded (its name string lives in the libsre
     * image at 0x3000xxxx). Same bug class as the _sys_memset fix above. */
    const char* hname = (const char*)yz_g2h(name);
    u32* hid = (u32*)yz_g2h(id);
    printf("[sysPrxForUser] sys_prx_get_module_id_by_name('%s')\n",
           hname ? hname : "(null)");

    /* A null id is not an error. The caller that only wants to know whether a
     * module is present passes one -- libsre's tuner probe does exactly that,
     * with r5 = 0 -- and answering CELL_EFAULT tells it the question was
     * malformed rather than that the module is absent. Report the lookup
     * result either way and write the id only if there is somewhere to put it.
     *
     * No module is loaded by name here, so the honest answer is that the name
     * is not known: CELL_PRX_ERROR_UNKNOWN_MODULE. This used to write a module
     * id of 0 and report CELL_OK, which is worse than it looks -- a caller asks
     * this question precisely to find out whether some optional module is
     * present, and success hands it an id that indexes nothing.
     *
     * libsre is the caller that shows the cost. _cellSpursIsLaunchedFromTuner
     * asks whether the SPURS profiler is loaded; told yes, it asserts on the
     * id, reports the title as launched from the tuner, and runs tuner and
     * trace setup that then fails with CELL_SPURS_CORE_ERROR_STAT -- and the
     * SPURS task workload never attaches, so a title waiting on its first
     * workload waits forever.
     *
     * A runtime that grows real load-by-name has a module table to answer
     * from, and this becomes a lookup miss rather than a constant. */
    if (hid) *hid = 0;
    return (s32)0x8001112E;   /* CELL_PRX_ERROR_UNKNOWN_MODULE */
}

/* ---------------------------------------------------------------------------
 * Process info
 * -----------------------------------------------------------------------*/

s32 sys_process_get_paramsfo(void* buf)
{
    /* PARAM.SFO data — return a minimal valid SFO with title ID */
    if (!buf) return CELL_EFAULT;
    memset(yz_g2h(buf), 0, 256);
    return CELL_OK;
}
