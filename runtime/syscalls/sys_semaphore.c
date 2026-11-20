#include "sys_semaphore.h"
#include "sys_ppu_thread.h"
#include "../memory/vm.h"
#include <string.h>

sys_semaphore_info g_sys_semaphores[SYS_SEMAPHORE_MAX];

static ps3_lock s_sem_table_lock;

static void write_be32(uint32_t addr, uint32_t val)
{
    uint8_t* p = (uint8_t*)vm_to_host(addr);
    p[0] = (uint8_t)(val >> 24); p[1] = (uint8_t)(val >> 16);
    p[2] = (uint8_t)(val >> 8);  p[3] = (uint8_t)val;
}

static sys_semaphore_info* sem_get(uint32_t id)
{
    if (id == 0 || id > SYS_SEMAPHORE_MAX) return NULL;
    sys_semaphore_info* s = &g_sys_semaphores[id - 1];
    return __atomic_load_n(&s->active, __ATOMIC_ACQUIRE) ? s : NULL;
}

int64_t sys_semaphore_create(ppu_context* ctx)
{
    uint32_t id_out_addr = LV2_ARG_PTR(ctx, 0);
    uint32_t attr_addr   = LV2_ARG_PTR(ctx, 1);
    int32_t  initial     = LV2_ARG_S32(ctx, 2);
    int32_t  max_val     = LV2_ARG_S32(ctx, 3);

    if (max_val <= 0 || initial < 0 || initial > max_val)
        return (int64_t)(int32_t)CELL_EINVAL;

    ps3_lock_acquire(&s_sem_table_lock);
    int slot = -1;
    for (int i = 0; i < SYS_SEMAPHORE_MAX; i++) {
        if (!g_sys_semaphores[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        ps3_lock_release(&s_sem_table_lock);
        return (int64_t)(int32_t)CELL_EAGAIN;
    }

    sys_semaphore_info* s = &g_sys_semaphores[slot];
    memset((char*)s + sizeof s->lock, 0, sizeof *s - sizeof s->lock);
    s->value     = initial;
    s->max_value = max_val;
    if (attr_addr != 0) {
        const uint8_t* a = (const uint8_t*)vm_to_host(attr_addr);
        s->protocol = ((uint32_t)a[0] << 24) | ((uint32_t)a[1] << 16) | ((uint32_t)a[2] << 8) | a[3];
        memcpy(s->name, a + 0x18, 8);
    }
    __atomic_store_n(&s->active, 1, __ATOMIC_RELEASE);
    ps3_lock_release(&s_sem_table_lock);

    if (id_out_addr != 0) write_be32(id_out_addr, (uint32_t)(slot + 1));
    return CELL_OK;
}

int64_t sys_semaphore_destroy(ppu_context* ctx)
{
    sys_semaphore_info* s = sem_get(LV2_ARG_U32(ctx, 0));
    if (!s) return (int64_t)(int32_t)CELL_ESRCH;

    ps3_lock_acquire(&s_sem_table_lock);
    ps3_lock_acquire(&s->lock);
    int64_t rc = CELL_OK;
    if (!s->active) rc = (int64_t)(int32_t)CELL_ESRCH;
    else if (s->q.head) rc = (int64_t)(int32_t)CELL_EBUSY;
    else __atomic_store_n(&s->active, 0, __ATOMIC_RELEASE);
    ps3_lock_release(&s->lock);
    ps3_lock_release(&s_sem_table_lock);
    return rc;
}

static int sem_try_take(sys_semaphore_info* s)
{
    int32_t v = __atomic_load_n(&s->value, __ATOMIC_RELAXED);
    while (v > 0) {
        if (__atomic_compare_exchange_n(&s->value, &v, v - 1, 1, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return 1;
    }
    return 0;
}

int64_t sys_semaphore_wait(ppu_context* ctx)
{
    sys_semaphore_info* s = sem_get(LV2_ARG_U32(ctx, 0));
    const uint64_t timeout_us = LV2_ARG_U64(ctx, 1);
    if (!s) return (int64_t)(int32_t)CELL_ESRCH;
    if (sem_try_take(s)) return CELL_OK;

    ppu_thread_note_block(ctx->thread_id);
    ps3_waiter w;
    ps3_waiter_init(&w, ctx->thread_id, 0);
    const uint64_t deadline = ps3_sync_deadline_us(timeout_us);

    ps3_lock_acquire(&s->lock);
    if (!s->active) {
        ps3_lock_release(&s->lock);
        return (int64_t)(int32_t)CELL_ESRCH;
    }
    if (__atomic_fetch_sub(&s->value, 1, __ATOMIC_ACQUIRE) > 0) {
        ps3_lock_release(&s->lock);
        return CELL_OK;
    }
    ps3_waitq_push(&s->q, &w);
    ps3_lock_release(&s->lock);

    if (ps3_waiter_sleep(&w, deadline)) return CELL_OK;
    ps3_lock_acquire(&s->lock);
    const int removed = ps3_waitq_remove(&s->q, &w);
    if (removed) __atomic_add_fetch(&s->value, 1, __ATOMIC_RELAXED);
    ps3_lock_release(&s->lock);
    if (removed) return (int64_t)(int32_t)CELL_ETIMEDOUT;
    ps3_waiter_sleep(&w, 0);
    return CELL_OK;
}

int64_t sys_semaphore_trywait(ppu_context* ctx)
{
    sys_semaphore_info* s = sem_get(LV2_ARG_U32(ctx, 0));
    if (!s) return (int64_t)(int32_t)CELL_ESRCH;
    return sem_try_take(s) ? CELL_OK : (int64_t)(int32_t)CELL_EBUSY;
}

int64_t sys_semaphore_post(ppu_context* ctx)
{
    sys_semaphore_info* s = sem_get(LV2_ARG_U32(ctx, 0));
    const int32_t count = LV2_ARG_S32(ctx, 1);
    if (!s) return (int64_t)(int32_t)CELL_ESRCH;
    if (count <= 0) return (int64_t)(int32_t)CELL_EINVAL;

    int32_t v = __atomic_load_n(&s->value, __ATOMIC_RELAXED);
    while (v >= 0 && count <= s->max_value - v) {
        if (__atomic_compare_exchange_n(&s->value, &v, v + count, 1, __ATOMIC_RELEASE, __ATOMIC_RELAXED))
            return CELL_OK;
    }

    ps3_waiter* wake = NULL;
    ps3_lock_acquire(&s->lock);
    v = __atomic_load_n(&s->value, __ATOMIC_RELAXED);
    do {
        if ((uint32_t)count > (uint32_t)s->max_value - (uint32_t)v) {
            ps3_lock_release(&s->lock);
            return (int64_t)(int32_t)CELL_EBUSY;
        }
    } while (!__atomic_compare_exchange_n(&s->value, &v, v + count, 1, __ATOMIC_RELEASE, __ATOMIC_RELAXED));
    int32_t n = v < 0 ? (-v < count ? -v : count) : 0;
    const uint32_t proto = s->protocol == 1 ? PS3_SYNC_FIFO : PS3_SYNC_PRIORITY;
    while (n-- > 0) {
        ps3_waiter* w = ps3_waitq_pop(&s->q, proto, 0);
        if (!w) break;
        w->next = wake;
        wake = w;
    }
    ps3_lock_release(&s->lock);
    while (wake) {
        ps3_waiter* next = wake->next;
        ps3_waiter_wake(wake, CELL_OK);
        wake = next;
    }
    return CELL_OK;
}

int64_t sys_semaphore_get_value(ppu_context* ctx)
{
    sys_semaphore_info* s = sem_get(LV2_ARG_U32(ctx, 0));
    const uint32_t out_addr = LV2_ARG_PTR(ctx, 1);
    if (!s) return (int64_t)(int32_t)CELL_ESRCH;
    if (!out_addr) return (int64_t)(int32_t)CELL_EFAULT;
    const int32_t v = __atomic_load_n(&s->value, __ATOMIC_RELAXED);
    write_be32(out_addr, (uint32_t)(v > 0 ? v : 0));
    return CELL_OK;
}

void sys_semaphore_init(lv2_syscall_table* tbl)
{
    lv2_syscall_register(tbl, SYS_SEMAPHORE_CREATE,    sys_semaphore_create);
    lv2_syscall_register(tbl, SYS_SEMAPHORE_DESTROY,   sys_semaphore_destroy);
    lv2_syscall_register(tbl, SYS_SEMAPHORE_WAIT,      sys_semaphore_wait);
    lv2_syscall_register(tbl, SYS_SEMAPHORE_TRYWAIT,   sys_semaphore_trywait);
    lv2_syscall_register(tbl, SYS_SEMAPHORE_POST,      sys_semaphore_post);
    lv2_syscall_register(tbl, SYS_SEMAPHORE_GET_VALUE, sys_semaphore_get_value);
}
