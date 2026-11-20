#include "ps3_stdio.h"
#include "win32_compat.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#define PS3_STDIO_BUF_BYTES        (64u * 1024u)
#define PS3_STDIO_FLUSH_MS_DEFAULT 50u
#define PS3_STDIO_FLUSH_MS_MAX     10000u
#define PS3_STDIO_CRASH_WAIT_MS    1000u

static char s_out_buf[PS3_STDIO_BUF_BYTES];
static char s_err_buf[PS3_STDIO_BUF_BYTES];

static int      s_init_done;
static int      s_buffered;
static int      s_stop;
static unsigned s_flush_ms = PS3_STDIO_FLUSH_MS_DEFAULT;
static HANDLE   s_wake;
static long     s_flush_req;
static long     s_flush_done;

static LPTOP_LEVEL_EXCEPTION_FILTER s_prev_filter;
static void (*s_prev_abort)(int);

static DWORD WINAPI ps3_stdio_flusher(LPVOID arg)
{
    (void)arg;
    for (;;) {
        WaitForSingleObject(s_wake, s_flush_ms);
        if (__atomic_load_n(&s_stop, __ATOMIC_ACQUIRE)) return 0;
        long want = __atomic_load_n(&s_flush_req, __ATOMIC_ACQUIRE);
        fflush(stdout);
        fflush(stderr);
        __atomic_store_n(&s_flush_done, want, __ATOMIC_RELEASE);
    }
}

void ps3_stdio_flush(void)
{
    fflush(stdout);
    fflush(stderr);
}

void ps3_stdio_flush_crash(void)
{
    if (!__atomic_load_n(&s_buffered, __ATOMIC_ACQUIRE)) return;
    if (__atomic_load_n(&s_stop, __ATOMIC_ACQUIRE)) return;
    long want = __atomic_add_fetch(&s_flush_req, 1, __ATOMIC_ACQ_REL);
    SetEvent(s_wake);
    for (unsigned waited = 0; waited < PS3_STDIO_CRASH_WAIT_MS; waited += 5) {
        if (__atomic_load_n(&s_flush_done, __ATOMIC_ACQUIRE) >= want) return;
        Sleep(5);
    }
}

static void ps3_stdio_at_exit(void)
{
    __atomic_store_n(&s_stop, 1, __ATOMIC_RELEASE);
    ps3_stdio_flush();
}

static void ps3_stdio_on_abort(int sig)
{
    ps3_stdio_flush_crash();
    if (s_prev_abort && s_prev_abort != SIG_DFL && s_prev_abort != SIG_IGN)
        s_prev_abort(sig);
}

static LONG WINAPI ps3_stdio_on_crash(EXCEPTION_POINTERS* ep)
{
    ps3_stdio_flush_crash();
    LONG r = s_prev_filter ? s_prev_filter(ep) : EXCEPTION_CONTINUE_SEARCH;
    if (r != EXCEPTION_CONTINUE_EXECUTION) ps3_stdio_flush_crash();
    return r;
}

static unsigned ps3_stdio_env_ms(const char* name, unsigned def)
{
    const char* s = getenv(name);
    if (!s || !*s) return def;
    char* end = NULL;
    unsigned long v = strtoul(s, &end, 10);
    if (*end || v < 1 || v > PS3_STDIO_FLUSH_MS_MAX) {
        fprintf(stderr, "[stdio] %s=%s ignored (1..%u), using %u\n",
                name, s, PS3_STDIO_FLUSH_MS_MAX, def);
        return def;
    }
    return (unsigned)v;
}

void ps3_stdio_init(void)
{
    if (__atomic_exchange_n(&s_init_done, 1, __ATOMIC_ACQ_REL)) return;

    const char* unbuf = getenv("PS3_STDIO_UNBUFFERED");
    if (unbuf && unbuf[0] == '1') {
        setvbuf(stdout, NULL, _IONBF, 0);
        return;
    }

    s_flush_ms = ps3_stdio_env_ms("PS3_STDIO_FLUSH_MS", PS3_STDIO_FLUSH_MS_DEFAULT);
    s_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!s_wake) { setvbuf(stdout, NULL, _IONBF, 0); return; }

    fflush(stdout);
    fflush(stderr);
    setvbuf(stdout, s_out_buf, _IOFBF, sizeof s_out_buf);
    setvbuf(stderr, s_err_buf, _IOFBF, sizeof s_err_buf);

    HANDLE t = CreateThread(NULL, 0, ps3_stdio_flusher, NULL, 0, NULL);
    if (!t) {
        setvbuf(stdout, NULL, _IONBF, 0);
        setvbuf(stderr, NULL, _IONBF, 0);
        fprintf(stderr, "[stdio] flush thread failed to start; stdout/stderr unbuffered\n");
        return;
    }
    CloseHandle(t);
    __atomic_store_n(&s_buffered, 1, __ATOMIC_RELEASE);

    atexit(ps3_stdio_at_exit);
    void (*prev)(int) = signal(SIGABRT, ps3_stdio_on_abort);
    s_prev_abort = (prev == SIG_ERR) ? NULL : prev;
    s_prev_filter = SetUnhandledExceptionFilter(ps3_stdio_on_crash);

    fprintf(stderr, "[stdio] stdout/stderr buffered (%u KB each), flushed every %u ms"
                    " (PS3_STDIO_UNBUFFERED=1: unbuffered)\n",
            PS3_STDIO_BUF_BYTES / 1024u, s_flush_ms);
}
