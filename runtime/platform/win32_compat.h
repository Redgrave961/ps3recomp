#ifndef PS3RECOMP_WIN32_COMPAT_H
#define PS3RECOMP_WIN32_COMPAT_H

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else

#include <pthread.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sched.h>
#include <wchar.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#if defined(__linux__)
#include <linux/membarrier.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifndef FALSE
#  define FALSE 0
#endif
#ifndef TRUE
#  define TRUE 1
#endif
#ifndef INFINITE
#  define INFINITE 0xFFFFFFFFu
#endif

#ifndef WINAPI
#  define WINAPI
#endif
#ifndef CALLBACK
#  define CALLBACK
#endif
#ifndef APIENTRY
#  define APIENTRY
#endif

typedef int                BOOL;
typedef unsigned char      BYTE;
typedef char               CHAR;
typedef unsigned short     WORD;
#ifndef PS3_WIN32_TYPES_USHORT
#define PS3_WIN32_TYPES_USHORT
typedef unsigned short     USHORT;
#endif
typedef uint32_t           DWORD;
typedef uint32_t           ULONG;
typedef uint32_t           UINT;
typedef int32_t            LONG;
typedef long long          LONGLONG;
typedef long long          LONG64;
typedef unsigned long long ULONGLONG;
typedef unsigned long long ULONG64;
typedef unsigned long long DWORD64;
typedef size_t             SIZE_T;
typedef size_t             ULONG_PTR;
typedef size_t             UINT_PTR;
typedef size_t             DWORD_PTR;
typedef void               VOID;
typedef void*              PVOID;
typedef void*              LPVOID;
typedef const void*        LPCVOID;
typedef void*              HANDLE;
typedef char*              LPSTR;
typedef const char*        LPCSTR;
typedef const char*        PCSTR;
typedef wchar_t*           LPWSTR;
typedef const wchar_t*     LPCWSTR;
typedef const wchar_t*     PCWSTR;

typedef union {
    struct { DWORD LowPart; LONG HighPart; };
    struct { DWORD LowPart; LONG HighPart; } u;
    LONGLONG QuadPart;
} LARGE_INTEGER;

typedef union {
    struct { DWORD LowPart; DWORD HighPart; };
    struct { DWORD LowPart; DWORD HighPart; } u;
    ULONGLONG QuadPart;
} ULARGE_INTEGER;

typedef struct { DWORD dwLowDateTime; DWORD dwHighDateTime; } FILETIME;

typedef struct {
    DWORD     dwPageSize;
    LPVOID    lpMinimumApplicationAddress;
    LPVOID    lpMaximumApplicationAddress;
    DWORD_PTR dwActiveProcessorMask;
    DWORD     dwNumberOfProcessors;
    DWORD     dwProcessorType;
    DWORD     dwAllocationGranularity;
    WORD      wProcessorLevel;
    WORD      wProcessorRevision;
} SYSTEM_INFO;

#define INVALID_HANDLE_VALUE   ((HANDLE)(intptr_t)-1)

#ifndef MAX_PATH
#  ifdef PATH_MAX
#    define MAX_PATH PATH_MAX
#  else
#    define MAX_PATH 4096
#  endif
#endif

#define WAIT_OBJECT_0          0u
#define WAIT_TIMEOUT           258u
#define WAIT_FAILED            0xFFFFFFFFu
#define STILL_ACTIVE           259u

#define ERROR_SUCCESS          0u
#define ERROR_PATH_NOT_FOUND   3u
#define ERROR_ACCESS_DENIED    5u
#define ERROR_INVALID_HANDLE   6u
#define ERROR_NOT_ENOUGH_MEMORY 8u
#define ERROR_GEN_FAILURE      31u
#define ERROR_INVALID_PARAMETER 87u
#define ERROR_ALREADY_EXISTS   183u
#define ERROR_TIMEOUT          1460u
#define TIMERR_NOERROR         0u

#define CREATE_SUSPENDED       0x00000004u
#define CREATE_EVENT_MANUAL_RESET 0x00000001u
#define CREATE_EVENT_INITIAL_SET  0x00000002u
#define CONDITION_VARIABLE_LOCKMODE_SHARED 0x1u

#define THREAD_PRIORITY_IDLE          (-15)
#define THREAD_PRIORITY_LOWEST        (-2)
#define THREAD_PRIORITY_BELOW_NORMAL  (-1)
#define THREAD_PRIORITY_NORMAL        0
#define THREAD_PRIORITY_ABOVE_NORMAL  1
#define THREAD_PRIORITY_HIGHEST       2
#define THREAD_PRIORITY_TIME_CRITICAL 15

#define MEM_COMMIT                    0x00001000u
#define MEM_RESERVE                   0x00002000u
#define MEM_DECOMMIT                  0x00004000u
#define MEM_RELEASE                   0x00008000u
#define PAGE_NOACCESS                 0x01u
#define PAGE_READONLY                 0x02u
#define PAGE_READWRITE                0x04u
#define PAGE_WRITECOPY                0x08u
#define PAGE_EXECUTE                  0x10u
#define PAGE_EXECUTE_READ             0x20u
#define PAGE_EXECUTE_READWRITE        0x40u
#define PAGE_EXECUTE_WRITECOPY        0x80u

DWORD GetLastError(void);
void  SetLastError(DWORD err);

static inline void* ps3_lazy_obj(void** slot, size_t sz,
                                 void (*init)(void*))
{
    void* cur = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
    if (cur) return cur;
    void* fresh = calloc(1, sz);
    if (!fresh) return NULL;
    init(fresh);
    void* expected = NULL;
    if (__atomic_compare_exchange_n(slot, &expected, fresh, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return fresh;
    free(fresh);
    return expected;
}

static inline struct timespec ps3__deadline_ms(DWORD ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec  += (time_t)(ms / 1000u);
    ts.tv_nsec += (long)(ms % 1000u) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    return ts;
}

#define PS3__ILK_DEFINE(SUF, T) \
static inline T ps3__ilk_inc_##SUF(volatile T* t)            { return __atomic_add_fetch(t, 1, __ATOMIC_SEQ_CST); } \
static inline T ps3__ilk_dec_##SUF(volatile T* t)            { return __atomic_sub_fetch(t, 1, __ATOMIC_SEQ_CST); } \
static inline T ps3__ilk_xchg_##SUF(volatile T* t, T v)      { return __atomic_exchange_n(t, v, __ATOMIC_SEQ_CST); } \
static inline T ps3__ilk_xadd_##SUF(volatile T* t, T v)      { return __atomic_fetch_add(t, v, __ATOMIC_SEQ_CST); } \
static inline T ps3__ilk_add_##SUF(volatile T* t, T v)       { return __atomic_add_fetch(t, v, __ATOMIC_SEQ_CST); } \
static inline T ps3__ilk_and_##SUF(volatile T* t, T v)       { return __atomic_fetch_and(t, v, __ATOMIC_SEQ_CST); } \
static inline T ps3__ilk_or_##SUF(volatile T* t, T v)        { return __atomic_fetch_or(t, v, __ATOMIC_SEQ_CST); } \
static inline T ps3__ilk_xor_##SUF(volatile T* t, T v)       { return __atomic_fetch_xor(t, v, __ATOMIC_SEQ_CST); } \
static inline T ps3__ilk_cas_##SUF(volatile T* t, T x, T c)  \
{ __atomic_compare_exchange_n(t, &c, x, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return c; }

PS3__ILK_DEFINE(i, int)
PS3__ILK_DEFINE(l, long)
PS3__ILK_DEFINE(ll, long long)
#undef PS3__ILK_DEFINE

#ifdef __cplusplus
extern "C++" {
#define PS3__ILK_OVERLOADS(NAME, OP) \
static inline int       NAME(volatile int* t)       { return ps3__ilk_##OP##_i(t); } \
static inline long      NAME(volatile long* t)      { return ps3__ilk_##OP##_l(t); } \
static inline long long NAME(volatile long long* t) { return ps3__ilk_##OP##_ll(t); }
#define PS3__ILK_OVERLOADS2(NAME, OP) \
static inline int       NAME(volatile int* t, int v)             { return ps3__ilk_##OP##_i(t, v); } \
static inline long      NAME(volatile long* t, long v)           { return ps3__ilk_##OP##_l(t, v); } \
static inline long long NAME(volatile long long* t, long long v) { return ps3__ilk_##OP##_ll(t, v); }
#define PS3__ILK_OVERLOADS3(NAME) \
static inline int       NAME(volatile int* t, int x, int c)                        { return ps3__ilk_cas_i(t, x, c); } \
static inline long      NAME(volatile long* t, long x, long c)                     { return ps3__ilk_cas_l(t, x, c); } \
static inline long long NAME(volatile long long* t, long long x, long long c)      { return ps3__ilk_cas_ll(t, x, c); }
PS3__ILK_OVERLOADS(InterlockedIncrement, inc)
PS3__ILK_OVERLOADS(InterlockedDecrement, dec)
PS3__ILK_OVERLOADS2(InterlockedExchange, xchg)
PS3__ILK_OVERLOADS2(InterlockedExchangeAdd, xadd)
PS3__ILK_OVERLOADS2(InterlockedAdd, add)
PS3__ILK_OVERLOADS2(InterlockedAnd, and)
PS3__ILK_OVERLOADS2(InterlockedOr, or)
PS3__ILK_OVERLOADS2(InterlockedXor, xor)
PS3__ILK_OVERLOADS3(InterlockedCompareExchange)
static inline long      InterlockedIncrement64(volatile long* t)                { return ps3__ilk_inc_l(t); }
static inline long long InterlockedIncrement64(volatile long long* t)           { return ps3__ilk_inc_ll(t); }
static inline long      InterlockedDecrement64(volatile long* t)                { return ps3__ilk_dec_l(t); }
static inline long long InterlockedDecrement64(volatile long long* t)           { return ps3__ilk_dec_ll(t); }
static inline long      InterlockedExchange64(volatile long* t, long v)         { return ps3__ilk_xchg_l(t, v); }
static inline long long InterlockedExchange64(volatile long long* t, long long v){ return ps3__ilk_xchg_ll(t, v); }
static inline long      InterlockedExchangeAdd64(volatile long* t, long v)      { return ps3__ilk_xadd_l(t, v); }
static inline long long InterlockedExchangeAdd64(volatile long long* t, long long v){ return ps3__ilk_xadd_ll(t, v); }
static inline long      InterlockedAdd64(volatile long* t, long v)              { return ps3__ilk_add_l(t, v); }
static inline long long InterlockedAdd64(volatile long long* t, long long v)    { return ps3__ilk_add_ll(t, v); }
static inline long      InterlockedAnd64(volatile long* t, long v)              { return ps3__ilk_and_l(t, v); }
static inline long long InterlockedAnd64(volatile long long* t, long long v)    { return ps3__ilk_and_ll(t, v); }
static inline long      InterlockedOr64(volatile long* t, long v)               { return ps3__ilk_or_l(t, v); }
static inline long long InterlockedOr64(volatile long long* t, long long v)     { return ps3__ilk_or_ll(t, v); }
static inline long      InterlockedXor64(volatile long* t, long v)              { return ps3__ilk_xor_l(t, v); }
static inline long long InterlockedXor64(volatile long long* t, long long v)    { return ps3__ilk_xor_ll(t, v); }
static inline long      InterlockedCompareExchange64(volatile long* t, long x, long c) { return ps3__ilk_cas_l(t, x, c); }
static inline long long InterlockedCompareExchange64(volatile long long* t, long long x, long long c) { return ps3__ilk_cas_ll(t, x, c); }
#undef PS3__ILK_OVERLOADS
#undef PS3__ILK_OVERLOADS2
#undef PS3__ILK_OVERLOADS3
}
#else
#define PS3__ILK_PICK(p, OP) _Generic((p), \
    volatile int*:       ps3__ilk_##OP##_i,  int*:       ps3__ilk_##OP##_i,  \
    volatile long*:      ps3__ilk_##OP##_l,  long*:      ps3__ilk_##OP##_l,  \
    volatile long long*: ps3__ilk_##OP##_ll, long long*: ps3__ilk_##OP##_ll)
#define PS3__ILK_PICK64(p, OP) _Generic((p), \
    volatile long*:      ps3__ilk_##OP##_l,  long*:      ps3__ilk_##OP##_l,  \
    volatile long long*: ps3__ilk_##OP##_ll, long long*: ps3__ilk_##OP##_ll)
#define InterlockedIncrement(p)            PS3__ILK_PICK((p), inc)((p))
#define InterlockedDecrement(p)            PS3__ILK_PICK((p), dec)((p))
#define InterlockedExchange(p, v)          PS3__ILK_PICK((p), xchg)((p), (v))
#define InterlockedExchangeAdd(p, v)       PS3__ILK_PICK((p), xadd)((p), (v))
#define InterlockedAdd(p, v)               PS3__ILK_PICK((p), add)((p), (v))
#define InterlockedAnd(p, v)               PS3__ILK_PICK((p), and)((p), (v))
#define InterlockedOr(p, v)                PS3__ILK_PICK((p), or)((p), (v))
#define InterlockedXor(p, v)               PS3__ILK_PICK((p), xor)((p), (v))
#define InterlockedCompareExchange(p, x, c) PS3__ILK_PICK((p), cas)((p), (x), (c))
#define InterlockedIncrement64(p)          PS3__ILK_PICK64((p), inc)((p))
#define InterlockedDecrement64(p)          PS3__ILK_PICK64((p), dec)((p))
#define InterlockedExchange64(p, v)        PS3__ILK_PICK64((p), xchg)((p), (v))
#define InterlockedExchangeAdd64(p, v)     PS3__ILK_PICK64((p), xadd)((p), (v))
#define InterlockedAdd64(p, v)             PS3__ILK_PICK64((p), add)((p), (v))
#define InterlockedAnd64(p, v)             PS3__ILK_PICK64((p), and)((p), (v))
#define InterlockedOr64(p, v)              PS3__ILK_PICK64((p), or)((p), (v))
#define InterlockedXor64(p, v)             PS3__ILK_PICK64((p), xor)((p), (v))
#define InterlockedCompareExchange64(p, x, c) PS3__ILK_PICK64((p), cas)((p), (x), (c))
#endif

static inline PVOID InterlockedExchangePointer(PVOID volatile* t, PVOID v) { return __atomic_exchange_n(t, v, __ATOMIC_SEQ_CST); }
static inline PVOID InterlockedCompareExchangePointer(PVOID volatile* t, PVOID exchange, PVOID comparand)
{
    __atomic_compare_exchange_n(t, &comparand, exchange, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return comparand;
}

#define _InterlockedExchange           InterlockedExchange
#define _InterlockedAnd                InterlockedAnd
#define _InterlockedOr                 InterlockedOr
#define _InterlockedCompareExchange    InterlockedCompareExchange

static inline LONG   ReadAcquire(const volatile LONG* p)          { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static inline void   WriteRelease(volatile LONG* p, LONG v)       { __atomic_store_n(p, v, __ATOMIC_RELEASE); }

static inline void MemoryBarrier(void)     { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
static inline void _ReadWriteBarrier(void) { __asm__ __volatile__("" ::: "memory"); }
#if defined(__linux__)
static inline void FlushProcessWriteBuffers(void)
{
    static int registered;
    if (!__atomic_load_n(&registered, __ATOMIC_ACQUIRE)) {
        if (syscall(__NR_membarrier, MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED, 0, 0)) abort();
        __atomic_store_n(&registered, 1, __ATOMIC_RELEASE);
    }
    if (syscall(__NR_membarrier, MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0, 0)) abort();
}
#endif

static inline void YieldProcessor(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield" ::: "memory");
#endif
}

typedef void*  SRWLOCK;
#define SRWLOCK_INIT NULL

static inline void ps3__rwl_init(void* p) { pthread_rwlock_init((pthread_rwlock_t*)p, NULL); }
static inline pthread_rwlock_t* ps3_srw(SRWLOCK* l)
{ return (pthread_rwlock_t*)ps3_lazy_obj((void**)l, sizeof(pthread_rwlock_t), ps3__rwl_init); }

static inline void InitializeSRWLock(SRWLOCK* l)          { *l = NULL; }
static inline void AcquireSRWLockExclusive(SRWLOCK* l)    { pthread_rwlock_wrlock(ps3_srw(l)); }
static inline void ReleaseSRWLockExclusive(SRWLOCK* l)    { pthread_rwlock_unlock(ps3_srw(l)); }
static inline BOOL TryAcquireSRWLockExclusive(SRWLOCK* l) { return pthread_rwlock_trywrlock(ps3_srw(l)) == 0; }
static inline void AcquireSRWLockShared(SRWLOCK* l)       { pthread_rwlock_rdlock(ps3_srw(l)); }
static inline void ReleaseSRWLockShared(SRWLOCK* l)       { pthread_rwlock_unlock(ps3_srw(l)); }
static inline BOOL TryAcquireSRWLockShared(SRWLOCK* l)    { return pthread_rwlock_tryrdlock(ps3_srw(l)) == 0; }

typedef struct {
    pthread_mutex_t m;
    int             initialised;
} CRITICAL_SECTION;

static inline void InitializeCriticalSection(CRITICAL_SECTION* cs)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&cs->m, &a);
    pthread_mutexattr_destroy(&a);
    cs->initialised = 1;
}
static inline void EnterCriticalSection(CRITICAL_SECTION* cs)    { pthread_mutex_lock(&cs->m); }
static inline void LeaveCriticalSection(CRITICAL_SECTION* cs)    { pthread_mutex_unlock(&cs->m); }
static inline BOOL TryEnterCriticalSection(CRITICAL_SECTION* cs) { return pthread_mutex_trylock(&cs->m) == 0; }
static inline void DeleteCriticalSection(CRITICAL_SECTION* cs)
{ if (cs->initialised) { pthread_mutex_destroy(&cs->m); cs->initialised = 0; } }

typedef void*  CONDITION_VARIABLE;
#define CONDITION_VARIABLE_INIT NULL

typedef struct { pthread_mutex_t m; pthread_cond_t c; unsigned gen; } ps3_condvar;

static inline void ps3__cv_init(void* p)
{
    ps3_condvar* cv = (ps3_condvar*)p;
    pthread_mutex_init(&cv->m, NULL);
    pthread_cond_init(&cv->c, NULL);
    cv->gen = 0;
}
static inline ps3_condvar* ps3_cv(CONDITION_VARIABLE* c)
{ return (ps3_condvar*)ps3_lazy_obj((void**)c, sizeof(ps3_condvar), ps3__cv_init); }

static inline void InitializeConditionVariable(CONDITION_VARIABLE* c) { *c = NULL; }
static inline void WakeConditionVariable(CONDITION_VARIABLE* c)
{
    ps3_condvar* cv = ps3_cv(c);
    if (!cv) return;
    pthread_mutex_lock(&cv->m);
    cv->gen++;
    pthread_cond_signal(&cv->c);
    pthread_mutex_unlock(&cv->m);
}
static inline void WakeAllConditionVariable(CONDITION_VARIABLE* c)
{
    ps3_condvar* cv = ps3_cv(c);
    if (!cv) return;
    pthread_mutex_lock(&cv->m);
    cv->gen++;
    pthread_cond_broadcast(&cv->c);
    pthread_mutex_unlock(&cv->m);
}

static inline BOOL ps3__cv_sleep(ps3_condvar* cv, DWORD ms,
                                 void (*unlock)(void*), void (*relock)(void*), void* lock)
{
    if (!cv) return FALSE;
    pthread_mutex_lock(&cv->m);
    unsigned g = cv->gen;
    unlock(lock);
    int rc = 0;
    if (ms == INFINITE) {
        while (cv->gen == g && rc == 0) rc = pthread_cond_wait(&cv->c, &cv->m);
    } else {
        struct timespec ts = ps3__deadline_ms(ms);
        while (cv->gen == g && rc == 0) rc = pthread_cond_timedwait(&cv->c, &cv->m, &ts);
    }
    BOOL woke = (cv->gen != g);
    pthread_mutex_unlock(&cv->m);
    relock(lock);
    if (!woke) SetLastError(ERROR_TIMEOUT);
    return woke;
}
static inline void ps3__srw_unlock_x(void* l) { ReleaseSRWLockExclusive((SRWLOCK*)l); }
static inline void ps3__srw_lock_x  (void* l) { AcquireSRWLockExclusive((SRWLOCK*)l); }
static inline void ps3__srw_unlock_s(void* l) { ReleaseSRWLockShared((SRWLOCK*)l); }
static inline void ps3__srw_lock_s  (void* l) { AcquireSRWLockShared((SRWLOCK*)l); }
static inline void ps3__cs_unlock   (void* l) { LeaveCriticalSection((CRITICAL_SECTION*)l); }
static inline void ps3__cs_lock     (void* l) { EnterCriticalSection((CRITICAL_SECTION*)l); }

static inline BOOL SleepConditionVariableSRW(CONDITION_VARIABLE* c, SRWLOCK* l, DWORD ms, ULONG flags)
{
    if (flags & CONDITION_VARIABLE_LOCKMODE_SHARED)
        return ps3__cv_sleep(ps3_cv(c), ms, ps3__srw_unlock_s, ps3__srw_lock_s, l);
    return ps3__cv_sleep(ps3_cv(c), ms, ps3__srw_unlock_x, ps3__srw_lock_x, l);
}
static inline BOOL SleepConditionVariableCS(CONDITION_VARIABLE* c, CRITICAL_SECTION* cs, DWORD ms)
{
    return ps3__cv_sleep(ps3_cv(c), ms, ps3__cs_unlock, ps3__cs_lock, cs);
}

typedef union { PVOID Ptr; } INIT_ONCE;
typedef INIT_ONCE* PINIT_ONCE;
#define INIT_ONCE_STATIC_INIT { NULL }

typedef BOOL (WINAPI *PINIT_ONCE_FN)(PINIT_ONCE once, PVOID parameter, PVOID* context);

enum { PS3_ONCE_IDLE = 0, PS3_ONCE_RUNNING = 1, PS3_ONCE_DONE = 2 };

typedef struct {
    pthread_mutex_t m;
    pthread_cond_t  c;
    int             state;
    PVOID           context;
} ps3_init_once;

static inline void ps3__once_init(void* p)
{
    ps3_init_once* o = (ps3_init_once*)p;
    pthread_mutex_init(&o->m, NULL);
    pthread_cond_init(&o->c, NULL);
    o->state   = PS3_ONCE_IDLE;
    o->context = NULL;
}
static inline ps3_init_once* ps3_once(INIT_ONCE* once)
{ return (ps3_init_once*)ps3_lazy_obj((void**)&once->Ptr, sizeof(ps3_init_once), ps3__once_init); }

static inline void InitOnceInitialize(PINIT_ONCE once) { once->Ptr = NULL; }

static inline BOOL InitOnceExecuteOnce(PINIT_ONCE once, PINIT_ONCE_FN fn,
                                       PVOID parameter, PVOID* context)
{
    ps3_init_once* o = (once && fn) ? ps3_once(once) : NULL;
    if (!o) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }

    pthread_mutex_lock(&o->m);
    while (o->state == PS3_ONCE_RUNNING) pthread_cond_wait(&o->c, &o->m);
    if (o->state == PS3_ONCE_DONE) {
        PVOID done = o->context;
        pthread_mutex_unlock(&o->m);
        if (context) *context = done;
        return TRUE;
    }
    o->state = PS3_ONCE_RUNNING;
    pthread_mutex_unlock(&o->m);

    PVOID produced = NULL;
    BOOL  ok = fn(once, parameter, &produced);

    pthread_mutex_lock(&o->m);
    o->state = ok ? PS3_ONCE_DONE : PS3_ONCE_IDLE;
    if (ok) o->context = produced;
    pthread_cond_broadcast(&o->c);
    pthread_mutex_unlock(&o->m);

    if (!ok) return FALSE;
    if (context) *context = produced;
    return TRUE;
}

static inline unsigned long long ps3__mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
}
static inline ULONGLONG GetTickCount64(void) { return ps3__mono_ns() / 1000000ull; }
static inline DWORD     GetTickCount(void)   { return (DWORD)(ps3__mono_ns() / 1000000ull); }
static inline BOOL QueryPerformanceFrequency(LARGE_INTEGER* f) { f->QuadPart = 1000000000LL; return TRUE; }
static inline BOOL QueryPerformanceCounter  (LARGE_INTEGER* c) { c->QuadPart = (LONGLONG)ps3__mono_ns(); return TRUE; }

static inline void GetSystemTimeAsFileTime(FILETIME* ft)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    unsigned long long t = (unsigned long long)ts.tv_sec * 10000000ull
                         + (unsigned long long)ts.tv_nsec / 100ull
                         + 116444736000000000ull;
    ft->dwLowDateTime  = (DWORD)t;
    ft->dwHighDateTime = (DWORD)(t >> 32);
}

static inline void Sleep(DWORD ms)
{
    if (ms == 0) { sched_yield(); return; }
    struct timespec d = { (time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L };
    nanosleep(&d, NULL);
}
static inline BOOL  SwitchToThread(void)              { sched_yield(); return TRUE; }
static inline UINT  timeBeginPeriod(UINT period)      { (void)period; return TIMERR_NOERROR; }
static inline UINT  timeEndPeriod(UINT period)        { (void)period; return TIMERR_NOERROR; }

static inline DWORD GetCurrentThreadId(void)
{
#if defined(__APPLE__)
    uint64_t tid = 0;
    pthread_threadid_np(NULL, &tid);
    return (DWORD)tid;
#elif defined(__linux__)
    return (DWORD)syscall(SYS_gettid);
#else
    return (DWORD)(uintptr_t)pthread_self();
#endif
}
static inline DWORD  GetCurrentProcessId(void) { return (DWORD)getpid(); }
static inline HANDLE GetCurrentProcess(void)   { return (HANDLE)(intptr_t)-1; }
static inline HANDLE GetCurrentThread(void)    { return (HANDLE)(intptr_t)-2; }

typedef DWORD (*PS3_THREAD_FN)(LPVOID);
typedef PS3_THREAD_FN LPTHREAD_START_ROUTINE;

HANDLE CreateEventA(void* sa, BOOL manual_reset, BOOL initial_state, LPCSTR name);
HANDLE CreateEventW(void* sa, BOOL manual_reset, BOOL initial_state, LPCWSTR name);
HANDLE CreateEventExA(void* sa, LPCSTR name, DWORD flags, DWORD access);
BOOL   SetEvent(HANDLE h);
BOOL   ResetEvent(HANDLE h);

HANDLE CreateSemaphoreA(void* sa, LONG initial, LONG maximum, LPCSTR name);
BOOL   ReleaseSemaphore(HANDLE h, LONG count, LONG* previous);

HANDLE CreateWaitableTimerA(void* sa, BOOL manual_reset, LPCSTR name);
BOOL   SetWaitableTimer(HANDLE h, const LARGE_INTEGER* due, LONG period_ms,
                        void* completion, void* arg, BOOL resume);
BOOL   CancelWaitableTimer(HANDLE h);

HANDLE CreateThread(void* sa, SIZE_T stack, PS3_THREAD_FN fn, LPVOID arg,
                    DWORD flags, DWORD* out_tid);
uintptr_t _beginthreadex(void* sa, unsigned stack, unsigned (*fn)(void*),
                         void* arg, unsigned flags, unsigned* out_tid);
void   ExitThread(DWORD code);
BOOL   GetExitCodeThread(HANDLE h, DWORD* code);
DWORD  ResumeThread(HANDLE h);
DWORD  SuspendThread(HANDLE h);

#define INVALID_FILE_ATTRIBUTES  ((DWORD)0xFFFFFFFFu)
#define FILE_ATTRIBUTE_DIRECTORY 0x00000010u
#define FILE_ATTRIBUTE_NORMAL    0x00000080u
DWORD  GetFileAttributesA(const char* path);
BOOL   SetThreadPriority(HANDLE h, int priority);
DWORD_PTR SetThreadAffinityMask(HANDLE h, DWORD_PTR mask);
LONG   SetThreadDescription(HANDLE h, PCWSTR description);

DWORD  WaitForSingleObject(HANDLE h, DWORD ms);
DWORD  WaitForMultipleObjects(DWORD count, const HANDLE* handles, BOOL wait_all, DWORD ms);
BOOL   CloseHandle(HANDLE h);

#define CONTEXT_CONTROL         0x00000001u
#define CONTEXT_INTEGER         0x00000002u
#define CONTEXT_SEGMENTS        0x00000004u
#define CONTEXT_FLOATING_POINT  0x00000008u
#define CONTEXT_DEBUG_REGISTERS 0x00000010u
#define CONTEXT_FULL            (CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_SEGMENTS)

typedef struct {
    DWORD     ContextFlags;
#if defined(__x86_64__)
    DWORD     EFlags;
    ULONGLONG Rax, Rbx, Rcx, Rdx, Rsi, Rdi;
    ULONGLONG R8, R9, R10, R11, R12, R13, R14, R15;
    union { ULONGLONG Pc; ULONGLONG Rip; };
    union { ULONGLONG Sp; ULONGLONG Rsp; };
    union { ULONGLONG Fp; ULONGLONG Rbp; };
    ULONGLONG Lr;
#elif defined(__aarch64__)
    DWORD     Cpsr;
    ULONGLONG X[29];
    ULONGLONG Pc;
    ULONGLONG Sp;
    union { ULONGLONG Fp; ULONGLONG X29; };
    union { ULONGLONG Lr; ULONGLONG X30; };
#else
    DWORD     ps3_status;
    ULONGLONG Pc, Sp, Fp, Lr;
#endif
} CONTEXT;
typedef CONTEXT* PCONTEXT;

#define THREAD_ALL_ACCESS           0x001FFFFFu

#define DUPLICATE_CLOSE_SOURCE      0x00000001u
#define DUPLICATE_SAME_ACCESS       0x00000002u

BOOL   GetThreadContext(HANDLE h, CONTEXT* ctx);
BOOL   SetThreadContext(HANDLE h, const CONTEXT* ctx);

HANDLE OpenThread(DWORD access, BOOL inherit, DWORD tid);

BOOL   DuplicateHandle(HANDLE src_process, HANDLE src, HANDLE dst_process,
                       HANDLE* out, DWORD access, BOOL inherit, DWORD options);

BOOL   GetThreadTimes(HANDLE h, FILETIME* creation, FILETIME* exit,
                      FILETIME* kernel, FILETIME* user);

#define TH32CS_SNAPTHREAD  0x00000004u

typedef struct {
    DWORD dwSize;
    DWORD cntUsage;
    DWORD th32ThreadID;
    DWORD th32OwnerProcessID;
    LONG  tpBasePri;
    LONG  tpDeltaPri;
    DWORD dwFlags;
} THREADENTRY32;

HANDLE CreateToolhelp32Snapshot(DWORD flags, DWORD pid);
BOOL   Thread32First(HANDLE snapshot, THREADENTRY32* entry);
BOOL   Thread32Next(HANDLE snapshot, THREADENTRY32* entry);

BOOL   WaitOnAddress(volatile VOID* address, PVOID compare, SIZE_T size, DWORD ms);
void   WakeByAddressSingle(PVOID address);
void   WakeByAddressAll(PVOID address);

LPVOID VirtualAlloc(LPVOID address, SIZE_T size, DWORD type, DWORD protect);
BOOL   VirtualFree(LPVOID address, SIZE_T size, DWORD type);
BOOL   VirtualProtect(LPVOID address, SIZE_T size, DWORD protect, DWORD* old);
void   GetSystemInfo(SYSTEM_INFO* si);

#define MEM_FREE                      0x00010000u
#define MEM_PRIVATE                   0x00020000u

typedef struct {
    PVOID  BaseAddress;
    PVOID  AllocationBase;
    DWORD  AllocationProtect;
    SIZE_T RegionSize;
    DWORD  State;
    DWORD  Protect;
    DWORD  Type;
} MEMORY_BASIC_INFORMATION;
typedef MEMORY_BASIC_INFORMATION* PMEMORY_BASIC_INFORMATION;

SIZE_T VirtualQuery(LPCVOID address, PMEMORY_BASIC_INFORMATION mbi, SIZE_T length);
BOOL   IsBadReadPtr(LPCVOID p, UINT_PTR length);
BOOL   IsBadWritePtr(LPVOID p, UINT_PTR length);
BOOL   IsBadCodePtr(LPCVOID p);

#define EXCEPTION_MAXIMUM_PARAMETERS    15

#define EXCEPTION_ACCESS_VIOLATION      0xC0000005u
#define EXCEPTION_IN_PAGE_ERROR         0xC0000006u
#define EXCEPTION_ILLEGAL_INSTRUCTION   0xC000001Du
#define EXCEPTION_PRIV_INSTRUCTION      0xC0000096u
#define EXCEPTION_DATATYPE_MISALIGNMENT 0x80000002u
#define EXCEPTION_SINGLE_STEP           0x80000004u
#define EXCEPTION_ARRAY_BOUNDS_EXCEEDED 0xC000008Cu
#define EXCEPTION_FLT_DIVIDE_BY_ZERO    0xC000008Eu
#define EXCEPTION_FLT_INEXACT_RESULT    0xC000008Fu
#define EXCEPTION_FLT_INVALID_OPERATION 0xC0000090u
#define EXCEPTION_FLT_OVERFLOW          0xC0000091u
#define EXCEPTION_FLT_UNDERFLOW         0xC0000093u
#define EXCEPTION_INT_DIVIDE_BY_ZERO    0xC0000094u
#define EXCEPTION_INT_OVERFLOW          0xC0000095u

#define EXCEPTION_CONTINUE_EXECUTION    (-1)
#define EXCEPTION_CONTINUE_SEARCH       0
#define EXCEPTION_EXECUTE_HANDLER       1


typedef struct _EXCEPTION_RECORD {
    DWORD                    ExceptionCode;
    DWORD                    ExceptionFlags;
    struct _EXCEPTION_RECORD* ExceptionRecord;
    PVOID                    ExceptionAddress;
    DWORD                    NumberParameters;
    ULONG_PTR                ExceptionInformation[EXCEPTION_MAXIMUM_PARAMETERS];
} EXCEPTION_RECORD;
typedef EXCEPTION_RECORD* PEXCEPTION_RECORD;

typedef struct {
    PEXCEPTION_RECORD ExceptionRecord;
    PCONTEXT          ContextRecord;
} EXCEPTION_POINTERS;

typedef LONG (WINAPI *PVECTORED_EXCEPTION_HANDLER)(EXCEPTION_POINTERS*);
typedef LONG (WINAPI *LPTOP_LEVEL_EXCEPTION_FILTER)(EXCEPTION_POINTERS*);

PVOID AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER handler);
ULONG RemoveVectoredExceptionHandler(PVOID handle);
LPTOP_LEVEL_EXCEPTION_FILTER SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER filter);

static inline DWORD GetEnvironmentVariableA(LPCSTR name, LPSTR buf, DWORD size)
{
    const char* v = name ? getenv(name) : NULL;
    if (!v) return 0;
    size_t n = strlen(v);
    if (!buf || size == 0 || n + 1 > size) return (DWORD)(n + 1);
    memcpy(buf, v, n + 1);
    return (DWORD)n;
}

static inline BOOL CreateDirectoryA(LPCSTR path, void* security_attributes)
{
    (void)security_attributes;
    if (!path) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (mkdir(path, 0777) == 0) return TRUE;
    switch (errno) {
    case EEXIST:  SetLastError(ERROR_ALREADY_EXISTS); break;
    case ENOENT:
    case ENOTDIR: SetLastError(ERROR_PATH_NOT_FOUND); break;
    case EACCES:
    case EPERM:
    case EROFS:   SetLastError(ERROR_ACCESS_DENIED);  break;
    default:      SetLastError(ERROR_GEN_FAILURE);    break;
    }
    return FALSE;
}

typedef enum {
    ProcessMemoryPriority,
    ProcessMemoryExhaustionInfo,
    ProcessAppMemoryInfo,
    ProcessInPrivateInfo,
    ProcessPowerThrottling,
    ProcessReservedValue1,
    ProcessTelemetryCoverageInfo,
    ProcessProtectionLevelInfo,
    ProcessLeapSecondInfo,
    ProcessMachineTypeInfo,
    ProcessInformationClassMax
} PROCESS_INFORMATION_CLASS;

#define PROCESS_POWER_THROTTLING_CURRENT_VERSION         1u
#define PROCESS_POWER_THROTTLING_EXECUTION_SPEED         0x1u

typedef struct {
    ULONG Version;
    ULONG ControlMask;
    ULONG StateMask;
} PROCESS_POWER_THROTTLING_STATE;

static inline BOOL SetProcessInformation(HANDLE process, PROCESS_INFORMATION_CLASS cls,
                                         LPVOID info, DWORD size)
{ (void)process; (void)cls; (void)info; (void)size; return TRUE; }

#ifdef __cplusplus
}
#endif

#endif
#endif
