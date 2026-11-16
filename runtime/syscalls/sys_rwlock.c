/*
 * ps3recomp - Reader-writer lock syscalls (implementation)
 */

#include "sys_rwlock.h"
#include "sys_ppu_thread.h"
#include "../memory/vm.h"
#include <string.h>

sys_rwlock_info g_sys_rwlocks[SYS_RWLOCK_MAX];

static ps3_lock s_rw_table_lock;

static void write_be32(uint32_t addr, uint32_t val)
{
    uint8_t* p = (uint8_t*)vm_to_host(addr);
    p[0] = (uint8_t)(val >> 24); p[1] = (uint8_t)(val >> 16);
    p[2] = (uint8_t)(val >> 8);  p[3] = (uint8_t)val;
}

static sys_rwlock_info* rw_get(uint32_t id)
{
    if (id == 0 || id > SYS_RWLOCK_MAX) return NULL;
    sys_rwlock_info* r = &g_sys_rwlocks[id - 1];
    return __atomic_load_n(&r->active, __ATOMIC_ACQUIRE) ? r : NULL;
}

static uint32_t rw_protocol(const sys_rwlock_info* r)
{
    return r->protocol == 1 ? PS3_SYNC_FIFO : PS3_SYNC_PRIORITY;
}

static int64_t rw_load(sys_rwlock_info* r) { return __atomic_load_n(&r->owner, __ATOMIC_ACQUIRE); }
static void rw_store(sys_rwlock_info* r, int64_t v) { __atomic_store_n(&r->owner, v, __ATOMIC_RELEASE); }

static int rw_cas(sys_rwlock_info* r, int64_t* expected, int64_t desired)
{
    return __atomic_compare_exchange_n(&r->owner, expected, desired, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

static ps3_waiter* take_all_readers(sys_rwlock_info* r, int64_t* count)
{
    ps3_waiter* list = NULL;
    *count = 0;
    for (ps3_waiter* w; (w = ps3_waitq_pop(&r->rq, PS3_SYNC_FIFO, 0)) != NULL;) {
        w->next = list;
        list = w;
        (*count)++;
    }
    return list;
}

static void wake_list(ps3_waiter* w)
{
    while (w) {
        ps3_waiter* next = w->next;
        ps3_waiter_wake(w, CELL_OK);
        w = next;
    }
}

int64_t sys_rwlock_create(ppu_context* ctx)
{
    uint32_t id_out_addr = LV2_ARG_PTR(ctx, 0);
    uint32_t attr_addr   = LV2_ARG_PTR(ctx, 1);

    ps3_lock_acquire(&s_rw_table_lock);
    int slot = -1;
    for (int i = 0; i < SYS_RWLOCK_MAX; i++) {
        if (!g_sys_rwlocks[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        ps3_lock_release(&s_rw_table_lock);
        return (int64_t)(int32_t)CELL_EAGAIN;
    }

    sys_rwlock_info* r = &g_sys_rwlocks[slot];
    memset((char*)r + sizeof r->lock, 0, sizeof *r - sizeof r->lock);
    if (attr_addr != 0) {
        const uint8_t* a = (const uint8_t*)vm_to_host(attr_addr);
        r->protocol = ((uint32_t)a[0] << 24) | ((uint32_t)a[1] << 16) | ((uint32_t)a[2] << 8) | a[3];
        memcpy(r->name, a + 0x18, 8);
    }
    __atomic_store_n(&r->active, 1, __ATOMIC_RELEASE);
    ps3_lock_release(&s_rw_table_lock);

    if (id_out_addr != 0) write_be32(id_out_addr, (uint32_t)(slot + 1));
    return CELL_OK;
}

int64_t sys_rwlock_destroy(ppu_context* ctx)
{
    sys_rwlock_info* r = rw_get(LV2_ARG_U32(ctx, 0));
    if (!r) return (int64_t)(int32_t)CELL_ESRCH;

    ps3_lock_acquire(&s_rw_table_lock);
    ps3_lock_acquire(&r->lock);
    int64_t rc = CELL_OK;
    if (!r->active) rc = (int64_t)(int32_t)CELL_ESRCH;
    else if (rw_load(r)) rc = (int64_t)(int32_t)CELL_EBUSY;
    else __atomic_store_n(&r->active, 0, __ATOMIC_RELEASE);
    ps3_lock_release(&r->lock);
    ps3_lock_release(&s_rw_table_lock);
    return rc;
}

static int64_t rw_sleep(sys_rwlock_info* r, ps3_waiter* w, ps3_waitq* q, uint64_t deadline)
{
    if (ps3_waiter_sleep(w, deadline)) return CELL_OK;
    ps3_waiter* wake = NULL;
    ps3_lock_acquire(&r->lock);
    const int removed = ps3_waitq_remove(q, w);
    if (removed) {
        const int64_t v = rw_load(r);
        if (q == &r->wq && r->rq.head && !r->wq.head && v < 0) {
            int64_t n;
            wake = take_all_readers(r, &n);
            rw_store(r, (v - 2 * n) & ~(int64_t)1);
        } else if (!r->rq.head && !r->wq.head) {
            rw_store(r, v & ~(int64_t)1);
        }
    }
    ps3_lock_release(&r->lock);
    wake_list(wake);
    if (removed) return (int64_t)(int32_t)CELL_ETIMEDOUT;
    ps3_waiter_sleep(w, 0);
    return CELL_OK;
}

static int rw_try_read(sys_rwlock_info* r)
{
    int64_t v = rw_load(r);
    while (v <= 0 && !(v & 1)) {
        if (rw_cas(r, &v, v - 2)) return 1;
    }
    return 0;
}

int64_t sys_rwlock_rlock(ppu_context* ctx)
{
    sys_rwlock_info* r = rw_get(LV2_ARG_U32(ctx, 0));
    const uint64_t timeout = LV2_ARG_U64(ctx, 1);
    if (!r) return (int64_t)(int32_t)CELL_ESRCH;
    if (rw_try_read(r)) return CELL_OK;

    ppu_thread_note_block(ctx->thread_id);
    ps3_waiter w;
    ps3_waiter_init(&w, ctx->thread_id, 0);
    const uint64_t deadline = ps3_sync_deadline_us(timeout);

    ps3_lock_acquire(&r->lock);
    int64_t v = rw_load(r);
    for (;;) {
        if (v <= 0 && !(v & 1)) {
            if (rw_cas(r, &v, v - 2)) { ps3_lock_release(&r->lock); return CELL_OK; }
        } else if (rw_cas(r, &v, v | 1)) {
            break;
        }
    }
    ps3_waitq_push(&r->rq, &w);
    ps3_lock_release(&r->lock);
    return rw_sleep(r, &w, &r->rq, deadline);
}

int64_t sys_rwlock_tryrlock(ppu_context* ctx)
{
    sys_rwlock_info* r = rw_get(LV2_ARG_U32(ctx, 0));
    if (!r) return (int64_t)(int32_t)CELL_ESRCH;
    return rw_try_read(r) ? CELL_OK : (int64_t)(int32_t)CELL_EBUSY;
}

static void rw_hand_off(sys_rwlock_info* r, ps3_waiter** wake)
{
    ps3_waiter* w = ps3_waitq_pop(&r->wq, rw_protocol(r), 0);
    if (w) {
        rw_store(r, (int64_t)(w->tid << 1) | ((r->wq.head || r->rq.head) ? 1 : 0));
        w->next = NULL;
        *wake = w;
    } else if (r->rq.head) {
        int64_t n;
        *wake = take_all_readers(r, &n);
        rw_store(r, -2 * n);
    } else {
        rw_store(r, 0);
    }
}

int64_t sys_rwlock_runlock(ppu_context* ctx)
{
    sys_rwlock_info* r = rw_get(LV2_ARG_U32(ctx, 0));
    if (!r) return (int64_t)(int32_t)CELL_ESRCH;

    int64_t v = rw_load(r);
    while (v < 0 && !(v & 1)) {
        if (rw_cas(r, &v, v + 2)) return CELL_OK;
    }

    ps3_waiter* wake = NULL;
    ps3_lock_acquire(&r->lock);
    v = rw_load(r);
    for (;;) {
        if (v >= 0) { ps3_lock_release(&r->lock); return (int64_t)(int32_t)CELL_EPERM; }
        if (v == -1 || rw_cas(r, &v, v + 2)) break;
    }
    if (v == -1) rw_hand_off(r, &wake);
    ps3_lock_release(&r->lock);
    wake_list(wake);
    return CELL_OK;
}

int64_t sys_rwlock_wlock(ppu_context* ctx)
{
    sys_rwlock_info* r = rw_get(LV2_ARG_U32(ctx, 0));
    const uint64_t timeout = LV2_ARG_U64(ctx, 1);
    if (!r) return (int64_t)(int32_t)CELL_ESRCH;

    const int64_t self = (int64_t)(ctx->thread_id << 1);
    int64_t v = 0;
    if (rw_cas(r, &v, self)) return CELL_OK;
    if (v > 0 && (v >> 1) == (int64_t)ctx->thread_id) return (int64_t)(int32_t)CELL_EDEADLK;

    ppu_thread_note_block(ctx->thread_id);
    ps3_waiter w;
    ps3_waiter_init(&w, ctx->thread_id, 0);
    const uint64_t deadline = ps3_sync_deadline_us(timeout);

    ps3_lock_acquire(&r->lock);
    v = rw_load(r);
    for (;;) {
        if (v == 0) {
            if (rw_cas(r, &v, self)) { ps3_lock_release(&r->lock); return CELL_OK; }
        } else if (rw_cas(r, &v, v | 1)) {
            break;
        }
    }
    ps3_waitq_push(&r->wq, &w);
    ps3_lock_release(&r->lock);
    return rw_sleep(r, &w, &r->wq, deadline);
}

int64_t sys_rwlock_trywlock(ppu_context* ctx)
{
    sys_rwlock_info* r = rw_get(LV2_ARG_U32(ctx, 0));
    if (!r) return (int64_t)(int32_t)CELL_ESRCH;
    int64_t v = 0;
    if (rw_cas(r, &v, (int64_t)(ctx->thread_id << 1))) return CELL_OK;
    if (v > 0 && (v >> 1) == (int64_t)ctx->thread_id) return (int64_t)(int32_t)CELL_EDEADLK;
    return (int64_t)(int32_t)CELL_EBUSY;
}

int64_t sys_rwlock_wunlock(ppu_context* ctx)
{
    sys_rwlock_info* r = rw_get(LV2_ARG_U32(ctx, 0));
    if (!r) return (int64_t)(int32_t)CELL_ESRCH;

    const int64_t self = (int64_t)(ctx->thread_id << 1);
    int64_t v = rw_load(r);
    if (v <= 0 || (v >> 1) != (int64_t)ctx->thread_id) return (int64_t)(int32_t)CELL_EPERM;
    if (v == self && rw_cas(r, &v, 0)) return CELL_OK;

    ps3_waiter* wake = NULL;
    ps3_lock_acquire(&r->lock);
    rw_hand_off(r, &wake);
    ps3_lock_release(&r->lock);
    wake_list(wake);
    return CELL_OK;
}

void sys_rwlock_init(lv2_syscall_table* tbl)
{
    lv2_syscall_register(tbl, SYS_RWLOCK_CREATE,   sys_rwlock_create);
    lv2_syscall_register(tbl, SYS_RWLOCK_DESTROY,  sys_rwlock_destroy);
    lv2_syscall_register(tbl, SYS_RWLOCK_RLOCK,    sys_rwlock_rlock);
    lv2_syscall_register(tbl, SYS_RWLOCK_TRYRLOCK, sys_rwlock_tryrlock);
    lv2_syscall_register(tbl, SYS_RWLOCK_RUNLOCK,  sys_rwlock_runlock);
    lv2_syscall_register(tbl, SYS_RWLOCK_WLOCK,    sys_rwlock_wlock);
    lv2_syscall_register(tbl, SYS_RWLOCK_TRYWLOCK, sys_rwlock_trywlock);
    lv2_syscall_register(tbl, SYS_RWLOCK_WUNLOCK,  sys_rwlock_wunlock);
}
