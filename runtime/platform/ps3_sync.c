#include "ps3_sync.h"

#include <stddef.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#    define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#  endif
#elif defined(__linux__)
#  include <errno.h>
#  include <linux/futex.h>
#  include <sys/syscall.h>
#  include <time.h>
#  include <unistd.h>
#else
#  include "win32_compat.h"
#  include <time.h>
#endif

extern int32_t ppu_thread_priority(uint64_t tid);

#if defined(_WIN32)
static uint64_t s_qpc_freq;

uint64_t ps3_sync_now_ns(void)
{
    if (__builtin_expect(!s_qpc_freq, 0)) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        s_qpc_freq = (uint64_t)f.QuadPart;
    }
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (uint64_t)(((unsigned __int128)(uint64_t)c.QuadPart * 1000000000ull) / s_qpc_freq);
}
#else
uint64_t ps3_sync_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#endif

uint64_t ps3_sync_deadline_us(uint64_t timeout_us)
{
    if (!timeout_us || timeout_us >= (1ull << 52)) return 0;
    return ps3_sync_now_ns() + timeout_us * 1000ull;
}

int ps3_futex_wait(volatile uint32_t* addr, uint32_t expected, uint64_t deadline_ns)
{
#if defined(_WIN32)
    for (;;) {
        DWORD ms = INFINITE;
        if (deadline_ns) {
            const uint64_t now = ps3_sync_now_ns();
            if (now >= deadline_ns) return 0;
            const uint64_t left = (deadline_ns - now + 999999ull) / 1000000ull;
            ms = left >= (uint64_t)INFINITE ? INFINITE - 1 : (DWORD)left;
        }
        if (WaitOnAddress(addr, &expected, sizeof expected, ms)) return 1;
        if (*addr != expected) return 1;
    }
#elif defined(__linux__)
    if (!deadline_ns) {
        syscall(SYS_futex, addr, FUTEX_WAIT_PRIVATE, expected, NULL, NULL, 0);
        return 1;
    }
    struct timespec ts;
    ts.tv_sec  = (time_t)(deadline_ns / 1000000000ull);
    ts.tv_nsec = (long)(deadline_ns % 1000000000ull);
    if (syscall(SYS_futex, addr, FUTEX_WAIT_BITSET_PRIVATE, expected, &ts, NULL,
                FUTEX_BITSET_MATCH_ANY) == -1 && errno == ETIMEDOUT)
        return 0;
    return 1;
#else
    for (;;) {
        DWORD ms = INFINITE;
        if (deadline_ns) {
            const uint64_t now = ps3_sync_now_ns();
            if (now >= deadline_ns) return 0;
            const uint64_t left = (deadline_ns - now + 999999ull) / 1000000ull;
            ms = left >= (uint64_t)INFINITE ? INFINITE - 1 : (DWORD)left;
        }
        if (WaitOnAddress(addr, &expected, sizeof expected, ms)) return 1;
        if (*addr != expected) return 1;
    }
#endif
}

void ps3_futex_wake_one(volatile uint32_t* addr)
{
#if defined(_WIN32)
    WakeByAddressSingle((PVOID)addr);
#elif defined(__linux__)
    syscall(SYS_futex, addr, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
#else
    WakeByAddressSingle((PVOID)addr);
#endif
}

void ps3_futex_wake_all(volatile uint32_t* addr)
{
#if defined(_WIN32)
    WakeByAddressAll((PVOID)addr);
#elif defined(__linux__)
    syscall(SYS_futex, addr, FUTEX_WAKE_PRIVATE, 0x7FFFFFFF, NULL, NULL, 0);
#else
    WakeByAddressAll((PVOID)addr);
#endif
}

#if defined(_WIN32)
static HANDLE armed_timer(uint64_t us)
{
    static _Thread_local HANDLE t_timer;
    if (!t_timer) t_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG)(us * 10ull);
    return t_timer && SetWaitableTimerEx(t_timer, &due, 0, NULL, NULL, NULL, 0) ? t_timer : NULL;
}
#endif

#define PS3_BRIEF_SLOTS 8u
#define PS3_BRIEF_CLAIMED ((uintptr_t)1)
#define PS3_BRIEF_SIGNALED ((uintptr_t)2)

static struct { volatile uintptr_t addr; void* volatile event; } s_brief[PS3_BRIEF_SLOTS];
static volatile uint32_t s_brief_waiters;

int ps3_brief_wait(volatile uint32_t* addr, uint32_t expected, uint64_t us)
{
    unsigned slot = 0;
    for (uintptr_t free_slot = 0; slot < PS3_BRIEF_SLOTS; slot++, free_slot = 0)
        if (__atomic_compare_exchange_n(&s_brief[slot].addr, &free_slot, PS3_BRIEF_CLAIMED, 0, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED))
            break;
    if (slot == PS3_BRIEF_SLOTS) {
        ps3_sleep_us(us);
        return 0;
    }
#if defined(_WIN32)
    static _Thread_local HANDLE t_event;
    if (!t_event) t_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    __atomic_store_n(&s_brief[slot].event, (void*)t_event, __ATOMIC_RELAXED);
#endif
    __atomic_add_fetch(&s_brief_waiters, 1u, __ATOMIC_SEQ_CST);
    __atomic_store_n(&s_brief[slot].addr, (uintptr_t)addr, __ATOMIC_SEQ_CST);
    int woken = 1;
#if defined(_WIN32)
    int consumed = 0;
    if (__atomic_load_n(addr, __ATOMIC_SEQ_CST) == expected) {
        HANDLE timer = t_event ? armed_timer(us) : NULL;
        if (timer) {
            HANDLE both[2] = { t_event, timer };
            consumed = woken = WaitForMultipleObjects(2, both, FALSE, INFINITE) == WAIT_OBJECT_0;
        } else {
            ps3_sleep_us(us);
            woken = 0;
        }
    }
    uintptr_t mine = (uintptr_t)addr;
    if (!consumed && !__atomic_compare_exchange_n(&s_brief[slot].addr, &mine, PS3_BRIEF_CLAIMED, 0, __ATOMIC_RELAXED,
                                                  __ATOMIC_RELAXED)) {
        WaitForSingleObject(t_event, INFINITE);
        woken = 1;
    }
#else
    if (__atomic_load_n(addr, __ATOMIC_SEQ_CST) == expected)
        woken = ps3_futex_wait(addr, expected, ps3_sync_now_ns() + us * 1000ull);
#endif
    __atomic_store_n(&s_brief[slot].addr, (uintptr_t)0, __ATOMIC_RELEASE);
    __atomic_sub_fetch(&s_brief_waiters, 1u, __ATOMIC_RELEASE);
    return woken;
}

void ps3_brief_wake(volatile uint32_t* addr)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (!__atomic_load_n(&s_brief_waiters, __ATOMIC_RELAXED)) return;
    for (unsigned slot = 0; slot < PS3_BRIEF_SLOTS; slot++) {
        uintptr_t want = (uintptr_t)addr;
        if (__atomic_load_n(&s_brief[slot].addr, __ATOMIC_RELAXED) != want) continue;
#if defined(_WIN32)
        if (__atomic_compare_exchange_n(&s_brief[slot].addr, &want, want | PS3_BRIEF_SIGNALED, 0, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED))
            SetEvent((HANDLE)__atomic_load_n(&s_brief[slot].event, __ATOMIC_RELAXED));
#else
        ps3_futex_wake_all(addr);
        return;
#endif
    }
}

void ps3_sleep_us(uint64_t us)
{
    if (!us) return;
    static _Thread_local uint64_t t_credit_ns;
    const uint64_t want = us * 1000ull;
    if (t_credit_ns >= want) {
        t_credit_ns -= want;
        return;
    }
    const uint64_t need = want - t_credit_ns;
    const uint64_t start = ps3_sync_now_ns();
#if defined(_WIN32)
    HANDLE timer = armed_timer((need + 999) / 1000);
    if (timer) WaitForSingleObject(timer, INFINITE);
    else Sleep((DWORD)((need + 999999) / 1000000));
#elif defined(__linux__)
    const uint64_t deadline = start + need;
    struct timespec ts;
    ts.tv_sec  = (time_t)(deadline / 1000000000ull);
    ts.tv_nsec = (long)(deadline % 1000000000ull);
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) != 0) {}
#else
    struct timespec ts = { (time_t)(need / 1000000000ull), (long)(need % 1000000000ull) };
    while (nanosleep(&ts, &ts) != 0) {}
#endif
    const uint64_t slept = ps3_sync_now_ns() - start;
    t_credit_ns = slept > need ? slept - need : 0;
}

void ps3_lock_slow(ps3_lock* l, unsigned spin)
{
    for (unsigned i = 0; i < spin; i++) {
        ps3_pause();
        if (__atomic_load_n(&l->v, __ATOMIC_RELAXED) == 0) {
            uint32_t z = 0;
            if (__atomic_compare_exchange_n(&l->v, &z, 1u, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
                return;
        }
    }
    while (__atomic_exchange_n(&l->v, 2u, __ATOMIC_ACQUIRE) != 0)
        ps3_futex_wait(&l->v, 2u, 0);
}

void ps3_waiter_init(ps3_waiter* w, uint64_t tid, uintptr_t key)
{
    w->next = NULL;
    w->key = key;
    w->tid = tid;
    w->prio = ppu_thread_priority(tid);
    w->done = 0;
    w->result = 0;
    w->arg[0] = w->arg[1] = w->arg[2] = w->arg[3] = 0;
}

void ps3_waitq_push(ps3_waitq* q, ps3_waiter* w)
{
    w->next = NULL;
    if (q->tail) q->tail->next = w; else q->head = w;
    q->tail = w;
}

static void waitq_unlink(ps3_waitq* q, ps3_waiter* prev, ps3_waiter* w)
{
    if (prev) prev->next = w->next; else q->head = w->next;
    if (q->tail == w) q->tail = prev;
    w->next = NULL;
}

int ps3_waitq_remove(ps3_waitq* q, ps3_waiter* w)
{
    for (ps3_waiter *p = NULL, *it = q->head; it; p = it, it = it->next) {
        if (it == w) { waitq_unlink(q, p, it); return 1; }
    }
    return 0;
}

ps3_waiter* ps3_waitq_pop(ps3_waitq* q, uint32_t protocol, uintptr_t key)
{
    ps3_waiter *best = NULL, *best_prev = NULL;
    for (ps3_waiter *p = NULL, *it = q->head; it; p = it, it = it->next) {
        if (key && it->key != key) continue;
        if (!best || (protocol == PS3_SYNC_PRIORITY && it->prio < best->prio)) {
            best = it; best_prev = p;
            if (protocol != PS3_SYNC_PRIORITY) break;
        }
    }
    if (best) waitq_unlink(q, best_prev, best);
    return best;
}

ps3_waiter* ps3_waitq_pop_tid(ps3_waitq* q, uintptr_t key, uint64_t tid)
{
    for (ps3_waiter *p = NULL, *it = q->head; it; p = it, it = it->next) {
        if ((!key || it->key == key) && it->tid == tid) { waitq_unlink(q, p, it); return it; }
    }
    return NULL;
}

int ps3_waitq_has(const ps3_waitq* q, uintptr_t key)
{
    for (const ps3_waiter* it = q->head; it; it = it->next)
        if (!key || it->key == key) return 1;
    return 0;
}

#define PS3_WAITER_PARKED 2u

void ps3_waiter_wake(ps3_waiter* w, int64_t result)
{
    w->result = result;
    if (__atomic_exchange_n(&w->done, 1u, __ATOMIC_ACQ_REL) == PS3_WAITER_PARKED) ps3_futex_wake_one(&w->done);
}

static int waiter_done(void* w)
{
    return __atomic_load_n(&((ps3_waiter*)w)->done, __ATOMIC_ACQUIRE) == 1u;
}

int ps3_waiter_sleep(ps3_waiter* w, uint64_t deadline_ns)
{
    if (waiter_done(w)) return 1;
    uint32_t state = 0;
    __atomic_compare_exchange_n(&w->done, &state, PS3_WAITER_PARKED, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    int woken = 1;
    while (!waiter_done(w)) {
        if (!ps3_futex_wait(&w->done, PS3_WAITER_PARKED, deadline_ns)) {
            woken = waiter_done(w);
            break;
        }
    }
    return woken;
}

#define PS3_PARK_BUCKETS 256u

typedef struct { ps3_park_bucket b; char pad[64 - sizeof(ps3_park_bucket) % 64]; } park_slot;
static park_slot s_park[PS3_PARK_BUCKETS];

ps3_park_bucket* ps3_park_bucket_of(uintptr_t key)
{
    uint64_t x = (uint64_t)key * 0x9E3779B97F4A7C15ull;
    return &s_park[(x >> 56) & (PS3_PARK_BUCKETS - 1)].b;
}

void ps3_park_lock2(ps3_park_bucket* a, ps3_park_bucket* b)
{
    if (a == b) { ps3_lock_acquire(&a->lock); return; }
    if (a > b) { ps3_park_bucket* t = a; a = b; b = t; }
    ps3_lock_acquire(&a->lock);
    ps3_lock_acquire(&b->lock);
}

void ps3_park_unlock2(ps3_park_bucket* a, ps3_park_bucket* b)
{
    ps3_lock_release(&a->lock);
    if (a != b) ps3_lock_release(&b->lock);
}
