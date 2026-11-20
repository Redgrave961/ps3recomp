#include "sys_event.h"
#include "sys_ppu_thread.h"
#include "../memory/vm.h"
#include <string.h>

sys_event_queue_info g_sys_event_queues[SYS_EVENT_QUEUE_MAX];
sys_event_port_info  g_sys_event_ports[SYS_EVENT_PORT_MAX];
sys_event_flag_info  g_sys_event_flags[SYS_EVENT_FLAG_MAX];

static ps3_lock s_evt_table_lock;

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

static void write_be64(uint32_t addr, uint64_t val)
{
    write_be32(addr, (uint32_t)(val >> 32));
    write_be32(addr + 4, (uint32_t)val);
}

static uint32_t proto_of(uint32_t protocol)
{
    return protocol == 1 ? PS3_SYNC_FIFO : PS3_SYNC_PRIORITY;
}

static void wake_list(ps3_waiter* w)
{
    while (w) {
        ps3_waiter* next = w->next;
        ps3_waiter_wake(w, w->result);
        w = next;
    }
}

static sys_event_queue_info* queue_get(uint32_t id)
{
    if (id == 0 || id > SYS_EVENT_QUEUE_MAX) return NULL;
    sys_event_queue_info* q = &g_sys_event_queues[id - 1];
    return __atomic_load_n(&q->active, __ATOMIC_ACQUIRE) ? q : NULL;
}

int64_t sys_event_queue_create(ppu_context* ctx)
{
    uint32_t id_out_addr = LV2_ARG_PTR(ctx, 0);
    uint32_t attr_addr   = LV2_ARG_PTR(ctx, 1);
    uint64_t key         = LV2_ARG_U64(ctx, 2);
    int32_t  size        = LV2_ARG_S32(ctx, 3);

    if (size <= 0 || size > SYS_EVENT_QUEUE_BUF_MAX)
        size = SYS_EVENT_QUEUE_BUF_MAX;

    ps3_lock_acquire(&s_evt_table_lock);
    int slot = -1;
    for (int i = 0; i < SYS_EVENT_QUEUE_MAX; i++) {
        if (!g_sys_event_queues[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        ps3_lock_release(&s_evt_table_lock);
        return (int64_t)(int32_t)CELL_EAGAIN;
    }

    sys_event_queue_info* q = &g_sys_event_queues[slot];
    memset((char*)q + sizeof q->lock, 0, sizeof *q - sizeof q->lock);
    q->key      = key;
    q->capacity = size;
    q->type     = SYS_PPU_QUEUE;
    if (attr_addr != 0) {
        const uint8_t* a = (const uint8_t*)vm_to_host(attr_addr);
        q->protocol = read_be32(a);
        q->type     = (int32_t)read_be32(a + 4);
        memcpy(q->name, a + 8, 8);
    }
    __atomic_store_n(&q->active, 1, __ATOMIC_RELEASE);
    ps3_lock_release(&s_evt_table_lock);

    if (id_out_addr != 0) write_be32(id_out_addr, (uint32_t)(slot + 1));
    return CELL_OK;
}

int64_t sys_event_queue_destroy(ppu_context* ctx)
{
    const uint32_t queue_id = LV2_ARG_U32(ctx, 0);
    const int32_t  mode     = LV2_ARG_S32(ctx, 1);
    sys_event_queue_info* q = queue_get(queue_id);
    if (!q) return (int64_t)(int32_t)CELL_ESRCH;
    if (mode != 0 && mode != SYS_EVENT_QUEUE_DESTROY_FORCE)
        return (int64_t)(int32_t)CELL_EINVAL;

    ps3_waiter* wake = NULL;
    ps3_lock_acquire(&s_evt_table_lock);
    ps3_lock_acquire(&q->lock);
    if (!q->active) {
        ps3_lock_release(&q->lock);
        ps3_lock_release(&s_evt_table_lock);
        return (int64_t)(int32_t)CELL_ESRCH;
    }
    if (q->q.head && mode != SYS_EVENT_QUEUE_DESTROY_FORCE) {
        ps3_lock_release(&q->lock);
        ps3_lock_release(&s_evt_table_lock);
        return (int64_t)(int32_t)CELL_EBUSY;
    }
    for (ps3_waiter* w; (w = ps3_waitq_pop(&q->q, PS3_SYNC_FIFO, 0)) != NULL;) {
        w->result = (int64_t)(int32_t)CELL_ECANCELED;
        w->next = wake;
        wake = w;
    }
    __atomic_store_n(&q->active, 0, __ATOMIC_RELEASE);
    ps3_lock_release(&q->lock);
    for (int i = 0; i < SYS_EVENT_PORT_MAX; i++)
        if (g_sys_event_ports[i].active && g_sys_event_ports[i].connected_queue == (int32_t)queue_id)
            g_sys_event_ports[i].connected_queue = 0;
    ps3_lock_release(&s_evt_table_lock);
    wake_list(wake);
    return CELL_OK;
}

int64_t sys_event_queue_receive(ppu_context* ctx)
{
    sys_event_queue_info* q = queue_get(LV2_ARG_U32(ctx, 0));
    const uint32_t event_addr = LV2_ARG_PTR(ctx, 1);
    const uint64_t timeout_us = LV2_ARG_U64(ctx, 2);
    if (!q) return (int64_t)(int32_t)CELL_ESRCH;
    if (q->type != SYS_PPU_QUEUE) return (int64_t)(int32_t)CELL_EINVAL;

    ps3_waiter w;
    ps3_waiter_init(&w, ctx->thread_id, 0);
    int64_t rc = CELL_OK;

    ps3_lock_acquire(&q->lock);
    if (!q->active) {
        ps3_lock_release(&q->lock);
        return (int64_t)(int32_t)CELL_ESRCH;
    }
    if (q->count) {
        const sys_event_t* e = &q->buffer[q->head];
        w.arg[0] = e->source; w.arg[1] = e->data1; w.arg[2] = e->data2; w.arg[3] = e->data3;
        q->head = (q->head + 1) % q->capacity;
        q->count--;
        ps3_lock_release(&q->lock);
    } else {
        ppu_thread_note_block(ctx->thread_id);
        const uint64_t deadline = ps3_sync_deadline_us(timeout_us);
        ps3_waitq_push(&q->q, &w);
        ps3_lock_release(&q->lock);
        if (!ps3_waiter_sleep(&w, deadline)) {
            ps3_lock_acquire(&q->lock);
            const int removed = ps3_waitq_remove(&q->q, &w);
            ps3_lock_release(&q->lock);
            if (removed) return (int64_t)(int32_t)CELL_ETIMEDOUT;
            ps3_waiter_sleep(&w, 0);
        }
        rc = w.result;
        if (rc != CELL_OK) return rc;
    }

    ctx->gpr[4] = w.arg[0];
    ctx->gpr[5] = w.arg[1];
    ctx->gpr[6] = w.arg[2];
    ctx->gpr[7] = w.arg[3];
    if (event_addr != 0) {
        write_be64(event_addr,      w.arg[0]);
        write_be64(event_addr + 8,  w.arg[1]);
        write_be64(event_addr + 16, w.arg[2]);
        write_be64(event_addr + 24, w.arg[3]);
    }
    return CELL_OK;
}

int64_t sys_event_queue_tryreceive(ppu_context* ctx)
{
    sys_event_queue_info* q = queue_get(LV2_ARG_U32(ctx, 0));
    const uint32_t event_addr = LV2_ARG_PTR(ctx, 1);
    int32_t        max_count  = LV2_ARG_S32(ctx, 2);
    const uint32_t count_addr = LV2_ARG_PTR(ctx, 3);
    if (!q) return (int64_t)(int32_t)CELL_ESRCH;
    if (max_count <= 0) max_count = 1;

    int32_t received = 0;
    ps3_lock_acquire(&q->lock);
    while (received < max_count && q->count > 0) {
        const sys_event_t* e = &q->buffer[q->head];
        if (event_addr != 0) {
            const uint32_t o = event_addr + (uint32_t)received * 32u;
            write_be64(o,      e->source);
            write_be64(o + 8,  e->data1);
            write_be64(o + 16, e->data2);
            write_be64(o + 24, e->data3);
        }
        q->head = (q->head + 1) % q->capacity;
        q->count--;
        received++;
    }
    ps3_lock_release(&q->lock);

    if (count_addr != 0) write_be32(count_addr, (uint32_t)received);
    return CELL_OK;
}

int64_t sys_event_queue_drain(ppu_context* ctx)
{
    sys_event_queue_info* q = queue_get(LV2_ARG_U32(ctx, 0));
    if (!q) return (int64_t)(int32_t)CELL_ESRCH;
    ps3_lock_acquire(&q->lock);
    q->head = q->tail = q->count = 0;
    ps3_lock_release(&q->lock);
    return CELL_OK;
}

static int event_queue_push(sys_event_queue_info* q, const sys_event_t* evt)
{
    ps3_lock_acquire(&q->lock);
    if (!q->active) {
        ps3_lock_release(&q->lock);
        return -2;
    }
    ps3_waiter* w = ps3_waitq_pop(&q->q, proto_of(q->protocol), 0);
    if (w) {
        ps3_lock_release(&q->lock);
        w->arg[0] = evt->source; w->arg[1] = evt->data1;
        w->arg[2] = evt->data2;  w->arg[3] = evt->data3;
        ps3_waiter_wake(w, CELL_OK);
        return 0;
    }
    if (q->count >= q->capacity) {
        ps3_lock_release(&q->lock);
        return -1;
    }
    q->buffer[q->tail] = *evt;
    q->tail = (q->tail + 1) % q->capacity;
    q->count++;
    ps3_lock_release(&q->lock);
    return 0;
}

int sys_event_queue_push_by_id(uint32_t queue_id,
                               uint64_t source, uint64_t data1,
                               uint64_t data2,  uint64_t data3)
{
    sys_event_queue_info* q = queue_get(queue_id);
    if (!q) return -1;
    const sys_event_t evt = { source, data1, data2, data3 };
    return event_queue_push(q, &evt) == 0 ? 0 : -1;
}

int64_t sys_event_port_create(ppu_context* ctx)
{
    uint32_t id_out_addr = LV2_ARG_PTR(ctx, 0);
    int32_t  port_type   = LV2_ARG_S32(ctx, 1);
    uint64_t name        = LV2_ARG_U64(ctx, 2);

    ps3_lock_acquire(&s_evt_table_lock);
    int slot = -1;
    for (int i = 0; i < SYS_EVENT_PORT_MAX; i++) {
        if (!g_sys_event_ports[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        ps3_lock_release(&s_evt_table_lock);
        return (int64_t)(int32_t)CELL_EAGAIN;
    }
    sys_event_port_info* p = &g_sys_event_ports[slot];
    p->active = 1;
    p->type   = port_type;
    p->name   = name;
    p->connected_queue = 0;
    ps3_lock_release(&s_evt_table_lock);

    if (id_out_addr != 0) write_be32(id_out_addr, (uint32_t)(slot + 1));
    return CELL_OK;
}

static sys_event_port_info* port_get(uint32_t id)
{
    if (id == 0 || id > SYS_EVENT_PORT_MAX) return NULL;
    sys_event_port_info* p = &g_sys_event_ports[id - 1];
    return p->active ? p : NULL;
}

int64_t sys_event_port_destroy(ppu_context* ctx)
{
    ps3_lock_acquire(&s_evt_table_lock);
    sys_event_port_info* p = port_get(LV2_ARG_U32(ctx, 0));
    int64_t rc = CELL_OK;
    if (!p) rc = (int64_t)(int32_t)CELL_ESRCH;
    else if (p->connected_queue) rc = (int64_t)(int32_t)CELL_EISCONN;
    else p->active = 0;
    ps3_lock_release(&s_evt_table_lock);
    return rc;
}

int64_t sys_event_port_connect_local(ppu_context* ctx)
{
    const uint32_t queue_id = LV2_ARG_U32(ctx, 1);
    ps3_lock_acquire(&s_evt_table_lock);
    sys_event_port_info* p = port_get(LV2_ARG_U32(ctx, 0));
    int64_t rc = CELL_OK;
    if (!p || !queue_get(queue_id)) rc = (int64_t)(int32_t)CELL_ESRCH;
    else if (p->type != SYS_EVENT_PORT_LOCAL) rc = (int64_t)(int32_t)CELL_EINVAL;
    else if (p->connected_queue) rc = (int64_t)(int32_t)CELL_EISCONN;
    else p->connected_queue = (int32_t)queue_id;
    ps3_lock_release(&s_evt_table_lock);
    return rc;
}

int64_t sys_event_port_disconnect(ppu_context* ctx)
{
    ps3_lock_acquire(&s_evt_table_lock);
    sys_event_port_info* p = port_get(LV2_ARG_U32(ctx, 0));
    int64_t rc = CELL_OK;
    if (!p) rc = (int64_t)(int32_t)CELL_ESRCH;
    else if (!p->connected_queue) rc = (int64_t)(int32_t)CELL_ENOTCONN;
    else p->connected_queue = 0;
    ps3_lock_release(&s_evt_table_lock);
    return rc;
}

int64_t sys_event_port_send(ppu_context* ctx)
{
    sys_event_port_info* p = port_get(LV2_ARG_U32(ctx, 0));
    if (!p) return (int64_t)(int32_t)CELL_ESRCH;
    sys_event_queue_info* q = queue_get((uint32_t)p->connected_queue);
    if (!q) return (int64_t)(int32_t)CELL_ENOTCONN;
    const sys_event_t evt = { p->name, LV2_ARG_U64(ctx, 1), LV2_ARG_U64(ctx, 2), LV2_ARG_U64(ctx, 3) };
    const int rc = event_queue_push(q, &evt);
    if (rc == -2) return (int64_t)(int32_t)CELL_ENOTCONN;
    if (rc == -1) return (int64_t)(int32_t)CELL_EBUSY;
    return CELL_OK;
}

static sys_event_flag_info* flag_get(uint32_t id)
{
    if (id == 0 || id > SYS_EVENT_FLAG_MAX) return NULL;
    sys_event_flag_info* f = &g_sys_event_flags[id - 1];
    return __atomic_load_n(&f->active, __ATOMIC_ACQUIRE) ? f : NULL;
}

static int flag_mode_valid(uint32_t mode)
{
    const uint32_t wait = mode & 0xF, clear = mode & ~0xFu;
    return (wait == SYS_EVENT_FLAG_WAIT_AND || wait == SYS_EVENT_FLAG_WAIT_OR) &&
           (clear == 0 || clear == SYS_EVENT_FLAG_WAIT_CLEAR || clear == SYS_EVENT_FLAG_WAIT_CLEAR_ALL);
}

static int flag_take(uint64_t* pattern, uint64_t bits, uint32_t mode, uint64_t* result)
{
    const int hit = (mode & 0xF) == SYS_EVENT_FLAG_WAIT_AND ? (*pattern & bits) == bits
                                                           : (*pattern & bits) != 0;
    if (!hit) return 0;
    *result = *pattern;
    if (mode & SYS_EVENT_FLAG_WAIT_CLEAR) *pattern &= ~bits;
    else if (mode & SYS_EVENT_FLAG_WAIT_CLEAR_ALL) *pattern = 0;
    return 1;
}

int64_t sys_event_flag_create(ppu_context* ctx)
{
    uint32_t id_out_addr  = LV2_ARG_PTR(ctx, 0);
    uint32_t attr_addr    = LV2_ARG_PTR(ctx, 1);
    uint64_t init_pattern = LV2_ARG_U64(ctx, 2);

    if (!id_out_addr || !attr_addr) return (int64_t)(int32_t)CELL_EFAULT;
    const uint8_t* a = (const uint8_t*)vm_to_host(attr_addr);
    const uint32_t protocol = read_be32(a), type = read_be32(a + 4);
    if (protocol != 1 && protocol != 2) return (int64_t)(int32_t)CELL_EINVAL;
    if (type != SYS_SYNC_WAITER_SINGLE && type != SYS_SYNC_WAITER_MULTIPLE)
        return (int64_t)(int32_t)CELL_EINVAL;

    ps3_lock_acquire(&s_evt_table_lock);
    int slot = -1;
    for (int i = 0; i < SYS_EVENT_FLAG_MAX; i++) {
        if (!g_sys_event_flags[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        ps3_lock_release(&s_evt_table_lock);
        return (int64_t)(int32_t)CELL_EAGAIN;
    }
    sys_event_flag_info* f = &g_sys_event_flags[slot];
    memset((char*)f + sizeof f->lock, 0, sizeof *f - sizeof f->lock);
    f->protocol = protocol;
    f->type     = type;
    f->pattern  = init_pattern;
    memcpy(f->name, a + 8, 8);
    __atomic_store_n(&f->active, 1, __ATOMIC_RELEASE);
    ps3_lock_release(&s_evt_table_lock);

    write_be32(id_out_addr, (uint32_t)(slot + 1));
    return CELL_OK;
}

int64_t sys_event_flag_destroy(ppu_context* ctx)
{
    sys_event_flag_info* f = flag_get(LV2_ARG_U32(ctx, 0));
    if (!f) return (int64_t)(int32_t)CELL_ESRCH;
    ps3_lock_acquire(&s_evt_table_lock);
    ps3_lock_acquire(&f->lock);
    int64_t rc = CELL_OK;
    if (!f->active) rc = (int64_t)(int32_t)CELL_ESRCH;
    else if (f->q.head) rc = (int64_t)(int32_t)CELL_EBUSY;
    else __atomic_store_n(&f->active, 0, __ATOMIC_RELEASE);
    ps3_lock_release(&f->lock);
    ps3_lock_release(&s_evt_table_lock);
    return rc;
}

int64_t sys_event_flag_wait(ppu_context* ctx)
{
    sys_event_flag_info* f = flag_get(LV2_ARG_U32(ctx, 0));
    const uint64_t bits        = LV2_ARG_U64(ctx, 1);
    const uint32_t mode        = LV2_ARG_U32(ctx, 2);
    const uint32_t result_addr = LV2_ARG_PTR(ctx, 3);
    const uint64_t timeout_us  = LV2_ARG_U64(ctx, 4);

    uint64_t result = 0;
    int64_t rc;
    if (!flag_mode_valid(mode)) {
        rc = (int64_t)(int32_t)CELL_EINVAL;
    } else if (!f) {
        rc = (int64_t)(int32_t)CELL_ESRCH;
    } else {
        ps3_waiter w;
        ps3_waiter_init(&w, ctx->thread_id, 0);
        w.arg[0] = bits;
        w.arg[1] = mode;
        ps3_lock_acquire(&f->lock);
        uint64_t pat = f->pattern;
        if (flag_take(&pat, bits, mode, &result)) {
            f->pattern = pat;
            ps3_lock_release(&f->lock);
            rc = CELL_OK;
        } else if (f->type == SYS_SYNC_WAITER_SINGLE && f->q.head) {
            ps3_lock_release(&f->lock);
            rc = (int64_t)(int32_t)CELL_EPERM;
        } else {
            ppu_thread_note_block(ctx->thread_id);
            const uint64_t deadline = ps3_sync_deadline_us(timeout_us);
            ps3_waitq_push(&f->q, &w);
            ps3_lock_release(&f->lock);
            rc = CELL_OK;
            if (!ps3_waiter_sleep(&w, deadline)) {
                ps3_lock_acquire(&f->lock);
                if (ps3_waitq_remove(&f->q, &w)) {
                    rc = (int64_t)(int32_t)CELL_ETIMEDOUT;
                    w.arg[2] = f->pattern;
                }
                ps3_lock_release(&f->lock);
                if (rc == CELL_OK) ps3_waiter_sleep(&w, 0);
            }
            if (rc == CELL_OK) rc = w.result;
            result = w.arg[2];
        }
    }
    if (result_addr) write_be64(result_addr, result);
    return rc;
}

int64_t sys_event_flag_trywait(ppu_context* ctx)
{
    sys_event_flag_info* f = flag_get(LV2_ARG_U32(ctx, 0));
    const uint64_t bits        = LV2_ARG_U64(ctx, 1);
    const uint32_t mode        = LV2_ARG_U32(ctx, 2);
    const uint32_t result_addr = LV2_ARG_PTR(ctx, 3);

    if (!flag_mode_valid(mode)) return (int64_t)(int32_t)CELL_EINVAL;
    if (!f) return (int64_t)(int32_t)CELL_ESRCH;

    uint64_t result = 0;
    ps3_lock_acquire(&f->lock);
    uint64_t pat = f->pattern;
    const int hit = flag_take(&pat, bits, mode, &result);
    f->pattern = pat;
    ps3_lock_release(&f->lock);
    if (!hit) return (int64_t)(int32_t)CELL_EBUSY;
    if (result_addr) write_be64(result_addr, result);
    return CELL_OK;
}

int64_t sys_event_flag_set(ppu_context* ctx)
{
    sys_event_flag_info* f = flag_get(LV2_ARG_U32(ctx, 0));
    const uint64_t bits = LV2_ARG_U64(ctx, 1);
    if (!f) return (int64_t)(int32_t)CELL_ESRCH;

    ps3_waiter* wake = NULL;
    ps3_lock_acquire(&f->lock);
    uint64_t pat = f->pattern | bits;
    const uint32_t proto = proto_of(f->protocol);
    for (ps3_waiter* it = f->q.head; it; it = it->next) it->arg[3] = 0;
    for (;;) {
        ps3_waiter* best = NULL;
        for (ps3_waiter* it = f->q.head; it; it = it->next) {
            if (it->arg[3]) continue;
            if (!best || (proto == PS3_SYNC_PRIORITY && it->prio < best->prio)) best = it;
            if (proto != PS3_SYNC_PRIORITY) break;
        }
        if (!best) break;
        best->arg[3] = 1;
        if (flag_take(&pat, best->arg[0], (uint32_t)best->arg[1], &best->arg[2])) {
            ps3_waitq_remove(&f->q, best);
            best->result = CELL_OK;
            best->next = wake;
            wake = best;
        }
        if (proto != PS3_SYNC_PRIORITY && !f->q.head) break;
    }
    f->pattern = pat;
    ps3_lock_release(&f->lock);
    wake_list(wake);
    return CELL_OK;
}

int64_t sys_event_flag_clear(ppu_context* ctx)
{
    sys_event_flag_info* f = flag_get(LV2_ARG_U32(ctx, 0));
    if (!f) return (int64_t)(int32_t)CELL_ESRCH;
    ps3_lock_acquire(&f->lock);
    f->pattern &= LV2_ARG_U64(ctx, 1);
    ps3_lock_release(&f->lock);
    return CELL_OK;
}

int64_t sys_event_flag_cancel(ppu_context* ctx)
{
    sys_event_flag_info* f = flag_get(LV2_ARG_U32(ctx, 0));
    const uint32_t num_addr = LV2_ARG_PTR(ctx, 1);
    if (num_addr) write_be32(num_addr, 0);
    if (!f) return (int64_t)(int32_t)CELL_ESRCH;

    ps3_waiter* wake = NULL;
    uint32_t n = 0;
    ps3_lock_acquire(&f->lock);
    for (ps3_waiter* w; (w = ps3_waitq_pop(&f->q, PS3_SYNC_FIFO, 0)) != NULL; n++) {
        w->result = (int64_t)(int32_t)CELL_ECANCELED;
        w->arg[2] = f->pattern;
        w->next = wake;
        wake = w;
    }
    ps3_lock_release(&f->lock);
    wake_list(wake);
    if (num_addr) write_be32(num_addr, n);
    return CELL_OK;
}

int64_t sys_event_flag_get(ppu_context* ctx)
{
    sys_event_flag_info* f = flag_get(LV2_ARG_U32(ctx, 0));
    const uint32_t out_addr = LV2_ARG_PTR(ctx, 1);
    if (!f) {
        if (out_addr) write_be64(out_addr, 0);
        return (int64_t)(int32_t)CELL_ESRCH;
    }
    if (!out_addr) return (int64_t)(int32_t)CELL_EFAULT;
    ps3_lock_acquire(&f->lock);
    const uint64_t pat = f->pattern;
    ps3_lock_release(&f->lock);
    write_be64(out_addr, pat);
    return CELL_OK;
}

void sys_event_init(lv2_syscall_table* tbl)
{
    lv2_syscall_register(tbl, SYS_EVENT_QUEUE_CREATE,      sys_event_queue_create);
    lv2_syscall_register(tbl, SYS_EVENT_QUEUE_DESTROY,     sys_event_queue_destroy);
    lv2_syscall_register(tbl, SYS_EVENT_QUEUE_RECEIVE,     sys_event_queue_receive);
    lv2_syscall_register(tbl, SYS_EVENT_QUEUE_TRYRECEIVE,  sys_event_queue_tryreceive);
    lv2_syscall_register(tbl, SYS_EVENT_QUEUE_DRAIN,       sys_event_queue_drain);

    lv2_syscall_register(tbl, SYS_EVENT_PORT_CREATE,        sys_event_port_create);
    lv2_syscall_register(tbl, SYS_EVENT_PORT_DESTROY,       sys_event_port_destroy);
    lv2_syscall_register(tbl, SYS_EVENT_PORT_CONNECT_LOCAL, sys_event_port_connect_local);
    lv2_syscall_register(tbl, SYS_EVENT_PORT_DISCONNECT,    sys_event_port_disconnect);
    lv2_syscall_register(tbl, SYS_EVENT_PORT_SEND,          sys_event_port_send);

    lv2_syscall_register(tbl, SYS_EVENT_FLAG_CREATE,   sys_event_flag_create);
    lv2_syscall_register(tbl, SYS_EVENT_FLAG_DESTROY,  sys_event_flag_destroy);
    lv2_syscall_register(tbl, SYS_EVENT_FLAG_WAIT,     sys_event_flag_wait);
    lv2_syscall_register(tbl, SYS_EVENT_FLAG_TRYWAIT,  sys_event_flag_trywait);
    lv2_syscall_register(tbl, SYS_EVENT_FLAG_SET,      sys_event_flag_set);
    lv2_syscall_register(tbl, SYS_EVENT_FLAG_CLEAR,    sys_event_flag_clear);
    lv2_syscall_register(tbl, SYS_EVENT_FLAG_CANCEL,   sys_event_flag_cancel);
    lv2_syscall_register(tbl, SYS_EVENT_FLAG_GET,      sys_event_flag_get);
}
