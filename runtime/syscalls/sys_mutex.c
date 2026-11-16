/*
 * ps3recomp - Kernel mutex syscalls (implementation)
 */

#include "sys_mutex.h"
#include "sys_ppu_thread.h"
#include "../memory/vm.h"
#include <string.h>

sys_mutex_info g_sys_mutexes[SYS_MUTEX_MAX];

static ps3_lock s_mtx_table_lock;

static uint32_t read_be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void write_be32(uint32_t addr, uint32_t val)
{
    uint8_t* p = (uint8_t*)vm_to_host(addr);
    p[0] = (uint8_t)(val >> 24); p[1] = (uint8_t)(val >> 16);
    p[2] = (uint8_t)(val >> 8);  p[3] = (uint8_t)val;
}

static uint32_t queue_protocol(const sys_mutex_info* m)
{
    return m->protocol == SYS_SYNC_FIFO ? PS3_SYNC_FIFO : PS3_SYNC_PRIORITY;
}

sys_mutex_info* lv2_mutex_get(uint32_t id)
{
    if (id == 0 || id > SYS_MUTEX_MAX) return NULL;
    sys_mutex_info* m = &g_sys_mutexes[id - 1];
    return __atomic_load_n(&m->active, __ATOMIC_ACQUIRE) ? m : NULL;
}

int lv2_mutex_try_own(sys_mutex_info* m, ps3_waiter* w)
{
    uint32_t cur = __atomic_load_n(&m->owner, __ATOMIC_RELAXED);
    for (;;) {
        if (cur == 0) {
            if (__atomic_compare_exchange_n(&m->owner, &cur, (uint32_t)w->tid, 0,
                                            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
                m->lock_count = (uint32_t)w->arg[0];
                return 1;
            }
        } else if ((cur & SYS_MUTEX_WAITERS) ||
                   __atomic_compare_exchange_n(&m->owner, &cur, cur | SYS_MUTEX_WAITERS, 0,
                                               __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
            ps3_waitq_push(&m->q, w);
            return 0;
        }
    }
}

void lv2_mutex_release(sys_mutex_info* m, uint32_t self)
{
    uint32_t expected = self;
    if (__atomic_compare_exchange_n(&m->owner, &expected, 0u, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED))
        return;
    ps3_lock_acquire(&m->lock);
    ps3_waiter* w = ps3_waitq_pop(&m->q, queue_protocol(m), 0);
    if (w) {
        m->lock_count = (uint32_t)w->arg[0];
        __atomic_store_n(&m->owner, (uint32_t)w->tid | (m->q.head ? SYS_MUTEX_WAITERS : 0u), __ATOMIC_RELEASE);
    } else {
        __atomic_store_n(&m->owner, 0u, __ATOMIC_RELEASE);
    }
    ps3_lock_release(&m->lock);
    if (w) ps3_waiter_wake(w, CELL_OK);
}

int64_t lv2_mutex_sleep(sys_mutex_info* m, ps3_waiter* w, uint64_t deadline_ns)
{
    if (ps3_waiter_sleep(w, deadline_ns)) return CELL_OK;
    ps3_lock_acquire(&m->lock);
    if (ps3_waitq_remove(&m->q, w)) {
        if (!m->q.head) __atomic_and_fetch(&m->owner, ~SYS_MUTEX_WAITERS, __ATOMIC_RELAXED);
        ps3_lock_release(&m->lock);
        return (int64_t)(int32_t)CELL_ETIMEDOUT;
    }
    ps3_lock_release(&m->lock);
    ps3_waiter_sleep(w, 0);
    return CELL_OK;
}

int64_t sys_mutex_create(ppu_context* ctx)
{
    uint32_t id_out_addr = LV2_ARG_PTR(ctx, 0);
    uint32_t attr_addr   = LV2_ARG_PTR(ctx, 1);

    if (!id_out_addr || !attr_addr)
        return (int64_t)(int32_t)CELL_EFAULT;

    const uint8_t* attr = (const uint8_t*)vm_to_host(attr_addr);
    const uint32_t protocol  = read_be32(attr + 0);
    const uint32_t recursive = read_be32(attr + 4);
    if (protocol != SYS_SYNC_FIFO && protocol != SYS_SYNC_PRIORITY && protocol != SYS_SYNC_PRIORITY_INHERIT)
        return (int64_t)(int32_t)CELL_EINVAL;
    if (recursive != SYS_SYNC_RECURSIVE && recursive != SYS_SYNC_NOT_RECURSIVE)
        return (int64_t)(int32_t)CELL_EINVAL;

    ps3_lock_acquire(&s_mtx_table_lock);
    int slot = -1;
    for (int i = 0; i < SYS_MUTEX_MAX; i++) {
        if (!g_sys_mutexes[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        ps3_lock_release(&s_mtx_table_lock);
        return (int64_t)(int32_t)CELL_EAGAIN;
    }

    sys_mutex_info* m = &g_sys_mutexes[slot];
    memset((char*)m + sizeof m->lock, 0, sizeof *m - sizeof m->lock);
    m->protocol  = protocol;
    m->recursive = recursive == SYS_SYNC_RECURSIVE;
    memcpy(m->name, attr + 0x20, 8);
    __atomic_store_n(&m->active, 1, __ATOMIC_RELEASE);
    ps3_lock_release(&s_mtx_table_lock);

    write_be32(id_out_addr, (uint32_t)(slot + 1));
    return CELL_OK;
}

int64_t sys_mutex_destroy(ppu_context* ctx)
{
    sys_mutex_info* m = lv2_mutex_get(LV2_ARG_U32(ctx, 0));
    if (!m) return (int64_t)(int32_t)CELL_ESRCH;

    ps3_lock_acquire(&s_mtx_table_lock);
    ps3_lock_acquire(&m->lock);
    int64_t rc = CELL_OK;
    if (!m->active) rc = (int64_t)(int32_t)CELL_ESRCH;
    else if (__atomic_load_n(&m->owner, __ATOMIC_ACQUIRE)) rc = (int64_t)(int32_t)CELL_EBUSY;
    else if (m->cond_count) rc = (int64_t)(int32_t)CELL_EPERM;
    else __atomic_store_n(&m->active, 0, __ATOMIC_RELEASE);
    ps3_lock_release(&m->lock);
    ps3_lock_release(&s_mtx_table_lock);
    return rc;
}

static int64_t relock_owned(sys_mutex_info* m)
{
    if (!m->recursive) return (int64_t)(int32_t)CELL_EDEADLK;
    if (m->lock_count == 0xFFFFFFFFu) return (int64_t)(int32_t)CELL_EKRESOURCE;
    m->lock_count++;
    return CELL_OK;
}

int64_t sys_mutex_lock(ppu_context* ctx)
{
    sys_mutex_info* m = lv2_mutex_get(LV2_ARG_U32(ctx, 0));
    const uint64_t timeout_us = LV2_ARG_U64(ctx, 1);
    if (!m) return (int64_t)(int32_t)CELL_ESRCH;

    const uint32_t self = (uint32_t)ctx->thread_id;
    uint32_t cur = 0;
    if (__atomic_compare_exchange_n(&m->owner, &cur, self, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        m->lock_count = 1;
        return CELL_OK;
    }
    if ((cur & ~SYS_MUTEX_WAITERS) == self) return relock_owned(m);

    ppu_thread_note_block(ctx->thread_id);
    ps3_waiter w;
    ps3_waiter_init(&w, ctx->thread_id, 0);
    w.arg[0] = 1;
    const uint64_t deadline = ps3_sync_deadline_us(timeout_us);

    ps3_lock_acquire(&m->lock);
    if (!m->active) {
        ps3_lock_release(&m->lock);
        return (int64_t)(int32_t)CELL_ESRCH;
    }
    const int owned = lv2_mutex_try_own(m, &w);
    ps3_lock_release(&m->lock);
    if (owned) return CELL_OK;
    return lv2_mutex_sleep(m, &w, deadline);
}

int64_t sys_mutex_trylock(ppu_context* ctx)
{
    sys_mutex_info* m = lv2_mutex_get(LV2_ARG_U32(ctx, 0));
    if (!m) return (int64_t)(int32_t)CELL_ESRCH;

    const uint32_t self = (uint32_t)ctx->thread_id;
    uint32_t cur = 0;
    if (__atomic_compare_exchange_n(&m->owner, &cur, self, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        m->lock_count = 1;
        return CELL_OK;
    }
    if ((cur & ~SYS_MUTEX_WAITERS) == self) return relock_owned(m);
    return (int64_t)(int32_t)CELL_EBUSY;
}

int64_t sys_mutex_unlock(ppu_context* ctx)
{
    sys_mutex_info* m = lv2_mutex_get(LV2_ARG_U32(ctx, 0));
    if (!m) return (int64_t)(int32_t)CELL_ESRCH;

    const uint32_t self = (uint32_t)ctx->thread_id;
    if (lv2_mutex_owner(m) != self) return (int64_t)(int32_t)CELL_EPERM;
    if (m->lock_count > 1) {
        m->lock_count--;
        return CELL_OK;
    }
    m->lock_count = 0;
    lv2_mutex_release(m, self);
    return CELL_OK;
}

void sys_mutex_init(lv2_syscall_table* tbl)
{
    lv2_syscall_register(tbl, SYS_MUTEX_CREATE,  sys_mutex_create);
    lv2_syscall_register(tbl, SYS_MUTEX_DESTROY, sys_mutex_destroy);
    lv2_syscall_register(tbl, SYS_MUTEX_LOCK,    sys_mutex_lock);
    lv2_syscall_register(tbl, SYS_MUTEX_TRYLOCK, sys_mutex_trylock);
    lv2_syscall_register(tbl, SYS_MUTEX_UNLOCK,  sys_mutex_unlock);
}
