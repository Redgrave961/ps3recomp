/*
 * ps3recomp - Kernel condition variables.
 */
#include "sys_cond.h"
#include "sys_ppu_thread.h"
#include "../memory/vm.h"
#include <string.h>

sys_cond_info g_sys_conds[SYS_COND_MAX];

static ps3_lock s_cond_table_lock;

static uint32_t cond_protocol(const sys_cond_info* c)
{
    return c->mutex->protocol == SYS_SYNC_FIFO ? PS3_SYNC_FIFO : PS3_SYNC_PRIORITY;
}

static sys_cond_info* cond_get(uint32_t id)
{
    if (!id || id > SYS_COND_MAX) return NULL;
    sys_cond_info* c = &g_sys_conds[id - 1];
    return __atomic_load_n(&c->active, __ATOMIC_ACQUIRE) ? c : NULL;
}

int64_t sys_cond_create(ppu_context* ctx)
{
    const uint32_t out  = LV2_ARG_PTR(ctx, 0);
    const uint32_t mid  = LV2_ARG_U32(ctx, 1);
    const uint32_t attr = LV2_ARG_PTR(ctx, 2);
    sys_mutex_info* m = lv2_mutex_get(mid);
    if (!m) return (int32_t)CELL_ESRCH;

    ps3_lock_acquire(&s_cond_table_lock);
    for (unsigned i = 0; i < SYS_COND_MAX; ++i) {
        sys_cond_info* c = &g_sys_conds[i];
        if (c->active) continue;
        memset((char*)c + sizeof c->lock, 0, sizeof *c - sizeof c->lock);
        c->mutex_id = mid;
        c->mutex = m;
        if (attr) memcpy(c->name, (uint8_t*)vm_to_host(attr) + 0x10, 8);
        ps3_lock_acquire(&m->lock);
        m->cond_count++;
        ps3_lock_release(&m->lock);
        __atomic_store_n(&c->active, 1, __ATOMIC_RELEASE);
        ps3_lock_release(&s_cond_table_lock);
        if (out) {
            uint8_t* p = vm_to_host(out);
            const uint32_t id = i + 1;
            p[0] = (uint8_t)(id >> 24); p[1] = (uint8_t)(id >> 16);
            p[2] = (uint8_t)(id >> 8);  p[3] = (uint8_t)id;
        }
        return CELL_OK;
    }
    ps3_lock_release(&s_cond_table_lock);
    return (int32_t)CELL_EAGAIN;
}

int64_t sys_cond_destroy(ppu_context* ctx)
{
    sys_cond_info* c = cond_get(LV2_ARG_U32(ctx, 0));
    if (!c) return (int32_t)CELL_ESRCH;
    ps3_lock_acquire(&s_cond_table_lock);
    ps3_lock_acquire(&c->lock);
    int32_t rc = CELL_OK;
    if (!c->active) rc = (int32_t)CELL_ESRCH;
    else if (c->q.head) rc = (int32_t)CELL_EBUSY;
    else {
        __atomic_store_n(&c->active, 0, __ATOMIC_RELEASE);
        ps3_lock_acquire(&c->mutex->lock);
        c->mutex->cond_count--;
        ps3_lock_release(&c->mutex->lock);
    }
    ps3_lock_release(&c->lock);
    ps3_lock_release(&s_cond_table_lock);
    return rc;
}

int64_t sys_cond_wait(ppu_context* ctx)
{
    sys_cond_info* c = cond_get(LV2_ARG_U32(ctx, 0));
    const uint64_t timeout = LV2_ARG_U64(ctx, 1);
    if (!c) return (int32_t)CELL_ESRCH;
    sys_mutex_info* m = c->mutex;
    const uint32_t self = (uint32_t)ctx->thread_id;
    if (lv2_mutex_owner(m) != self) return (int32_t)CELL_EPERM;

    ppu_thread_note_block(ctx->thread_id);
    ps3_waiter w;
    ps3_waiter_init(&w, ctx->thread_id, 0);
    w.arg[0] = m->lock_count;
    const uint64_t deadline = ps3_sync_deadline_us(timeout);

    ps3_lock_acquire(&c->lock);
    if (!c->active) {
        ps3_lock_release(&c->lock);
        return (int32_t)CELL_ESRCH;
    }
    ps3_waitq_push(&c->q, &w);
    ps3_lock_release(&c->lock);

    m->lock_count = 0;
    lv2_mutex_release(m, self);

    if (ps3_waiter_sleep(&w, deadline)) return CELL_OK;

    ps3_lock_acquire(&c->lock);
    ps3_lock_acquire(&m->lock);
    const int timed_out = ps3_waitq_remove(&c->q, &w);
    const int owned = timed_out && lv2_mutex_try_own(m, &w);
    ps3_lock_release(&m->lock);
    ps3_lock_release(&c->lock);
    if (!owned) ps3_waiter_sleep(&w, 0);
    return timed_out ? (int32_t)CELL_ETIMEDOUT : CELL_OK;
}

static void hand_to_mutex(sys_mutex_info* m, ps3_waiter* w, ps3_waiter** wake)
{
    if (lv2_mutex_try_own(m, w)) {
        w->next = *wake;
        *wake = w;
    }
}

static void wake_list(ps3_waiter* w)
{
    while (w) {
        ps3_waiter* next = w->next;
        ps3_waiter_wake(w, CELL_OK);
        w = next;
    }
}

int64_t sys_cond_signal(ppu_context* ctx)
{
    sys_cond_info* c = cond_get(LV2_ARG_U32(ctx, 0));
    if (!c) return (int32_t)CELL_ESRCH;
    ps3_waiter* wake = NULL;
    ps3_lock_acquire(&c->lock);
    ps3_waiter* w = ps3_waitq_pop(&c->q, cond_protocol(c), 0);
    if (w) {
        ps3_lock_acquire(&c->mutex->lock);
        hand_to_mutex(c->mutex, w, &wake);
        ps3_lock_release(&c->mutex->lock);
    }
    ps3_lock_release(&c->lock);
    wake_list(wake);
    return CELL_OK;
}

int64_t sys_cond_signal_all(ppu_context* ctx)
{
    sys_cond_info* c = cond_get(LV2_ARG_U32(ctx, 0));
    if (!c) return (int32_t)CELL_ESRCH;
    ps3_waiter* wake = NULL;
    ps3_lock_acquire(&c->lock);
    if (c->q.head) {
        ps3_lock_acquire(&c->mutex->lock);
        for (ps3_waiter* w; (w = ps3_waitq_pop(&c->q, PS3_SYNC_PRIORITY, 0)) != NULL;)
            hand_to_mutex(c->mutex, w, &wake);
        ps3_lock_release(&c->mutex->lock);
    }
    ps3_lock_release(&c->lock);
    wake_list(wake);
    return CELL_OK;
}

int64_t sys_cond_signal_to(ppu_context* ctx)
{
    sys_cond_info* c = cond_get(LV2_ARG_U32(ctx, 0));
    const uint32_t tid = LV2_ARG_U32(ctx, 1);
    if (!c || !ppu_thread_exists(tid)) return (int32_t)CELL_ESRCH;
    ps3_waiter* wake = NULL;
    ps3_lock_acquire(&c->lock);
    ps3_waiter* w = ps3_waitq_pop_tid(&c->q, 0, tid);
    if (w) {
        ps3_lock_acquire(&c->mutex->lock);
        hand_to_mutex(c->mutex, w, &wake);
        ps3_lock_release(&c->mutex->lock);
    }
    ps3_lock_release(&c->lock);
    wake_list(wake);
    return w ? CELL_OK : (int32_t)CELL_EPERM;
}

void sys_cond_init(lv2_syscall_table* tbl)
{
    lv2_syscall_register(tbl, SYS_COND_CREATE, sys_cond_create);
    lv2_syscall_register(tbl, SYS_COND_DESTROY, sys_cond_destroy);
    lv2_syscall_register(tbl, SYS_COND_WAIT, sys_cond_wait);
    lv2_syscall_register(tbl, SYS_COND_SIGNAL, sys_cond_signal);
    lv2_syscall_register(tbl, SYS_COND_SIGNAL_ALL, sys_cond_signal_all);
    lv2_syscall_register(tbl, SYS_COND_SIGNAL_TO, sys_cond_signal_to);
}
