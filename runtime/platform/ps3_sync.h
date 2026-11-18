#ifndef PS3RECOMP_PS3_SYNC_H
#define PS3RECOMP_PS3_SYNC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PS3_SYNC_FIFO      1u
#define PS3_SYNC_PRIORITY  2u

uint64_t ps3_sync_now_ns(void);
uint64_t ps3_sync_deadline_us(uint64_t timeout_us);
void     ps3_sleep_us(uint64_t us);

int  ps3_futex_wait(volatile uint32_t* addr, uint32_t expected, uint64_t deadline_ns);
void ps3_futex_wake_one(volatile uint32_t* addr);
void ps3_futex_wake_all(volatile uint32_t* addr);

int  ps3_brief_wait(volatile uint32_t* addr, uint32_t expected, uint64_t us);
void ps3_brief_wake(volatile uint32_t* addr);

typedef struct ps3_lock { volatile uint32_t v; } ps3_lock;

void ps3_lock_slow(ps3_lock* l, unsigned spin);

static inline void ps3_lock_acquire_spin(ps3_lock* l, unsigned spin)
{
    uint32_t z = 0;
    if (!__atomic_compare_exchange_n(&l->v, &z, 1u, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        ps3_lock_slow(l, spin);
}

static inline void ps3_lock_acquire(ps3_lock* l) { ps3_lock_acquire_spin(l, 0); }

static inline void ps3_lock_release(ps3_lock* l)
{
    if (__atomic_exchange_n(&l->v, 0u, __ATOMIC_RELEASE) == 2u)
        ps3_futex_wake_one(&l->v);
}

typedef struct ps3_waiter {
    struct ps3_waiter* next;
    uintptr_t          key;
    uint64_t           tid;
    int32_t            prio;
    volatile uint32_t  done;
    int64_t            result;
    uint64_t           arg[4];
} ps3_waiter;

typedef struct ps3_waitq { ps3_waiter* head; ps3_waiter* tail; } ps3_waitq;

void        ps3_waiter_init(ps3_waiter* w, uint64_t tid, uintptr_t key);
void        ps3_waitq_push(ps3_waitq* q, ps3_waiter* w);
int         ps3_waitq_remove(ps3_waitq* q, ps3_waiter* w);
ps3_waiter* ps3_waitq_pop(ps3_waitq* q, uint32_t protocol, uintptr_t key);
ps3_waiter* ps3_waitq_pop_tid(ps3_waitq* q, uintptr_t key, uint64_t tid);
int         ps3_waitq_has(const ps3_waitq* q, uintptr_t key);
void        ps3_waiter_wake(ps3_waiter* w, int64_t result);
int         ps3_waiter_sleep(ps3_waiter* w, uint64_t deadline_ns);

typedef struct ps3_park_bucket { ps3_lock lock; ps3_waitq q; } ps3_park_bucket;

ps3_park_bucket* ps3_park_bucket_of(uintptr_t key);
void             ps3_park_lock2(ps3_park_bucket* a, ps3_park_bucket* b);
void             ps3_park_unlock2(ps3_park_bucket* a, ps3_park_bucket* b);

#ifdef __cplusplus
}
#endif

#endif
