#include <stdlib.h>
#include "ps3emu/nid.h"
#include "lv2_syscall_table.h"
#include "lv2_lock_line.h"
#include "sys_ppu_thread.h"
#include "sys_mutex.h"
#include "sys_cond.h"
#include "sys_semaphore.h"
#include "sys_rwlock.h"
#include "sys_event.h"
#include "sys_timer.h"
#include "sys_memory.h"
#include "sys_vm.h"
#include "sys_fs.h"
extern void sys_raw_spu_init(lv2_syscall_table* tbl);
extern void spu_raw_note_image(uint32_t src_ea, uint32_t entry);

#define SPU_IMAGE_STAGE_BASE 0x0D800000u
#define SPU_IMAGE_STAGE_END  0x0E000000u
#define SPU_IMAGE_STAGE_MAX  0x00100000u
#include "ps3emu/spu_fallback.h"
#include "../spu/spu_lifted_thread.h"
#include "../spu/spu_context.h"
#include "../spu/spu_workload.h"

#include <stdio.h>
#include <string.h>

#include "../platform/win32_compat.h"
#include "ps3emu/log_first.h"
#include "ps3emu/thread_name.h"

extern uint8_t* vm_base;

static int64_t sys_tty_write(ppu_context* ctx)
{
    uint32_t ch     = (uint32_t)ctx->gpr[3];
    uint32_t buf_ea = (uint32_t)ctx->gpr[4];
    uint32_t len    = (uint32_t)ctx->gpr[5];
    uint32_t pwr_ea = (uint32_t)ctx->gpr[6];

    (void)ch;

    if (buf_ea && len > 0 && vm_base) {
        fwrite(vm_base + buf_ea, 1, len, stderr);
        fflush(stderr);
    }

    if (pwr_ea && vm_base) {
        uint32_t be_len = ((len >> 24) & 0xFF) | ((len >> 8) & 0xFF00) |
                          ((len << 8) & 0xFF0000) | ((len << 24) & 0xFF000000);
        memcpy(vm_base + pwr_ea, &be_len, 4);
    }

    return 0;
}

static int64_t sys_tty_read(ppu_context* ctx)
{
    uint32_t prd_ea = (uint32_t)ctx->gpr[6];

    if (prd_ea && vm_base)
        memset(vm_base + prd_ea, 0, 4);

    return 0;
}

#define SYS_TTY_READ   402
#define SYS_TTY_WRITE  403

#define SPU_GROUP_STATE_INITIALIZED  0
#define SPU_GROUP_STATE_READY        1
#define SPU_GROUP_STATE_RUNNING      2
#define SPU_GROUP_STATE_STOPPED      3

#define SPU_GROUP_CAUSE_GROUP_EXIT        0x0001u
#define SPU_GROUP_CAUSE_ALL_THREADS_EXIT  0x0002u
#define SPU_GROUP_CAUSE_TERMINATED        0x0004u

#define MAX_SPU_GROUPS   32
#define MAX_SPU_THREADS  (MAX_SPU_GROUPS * 8)

#ifdef _WIN32
#  include <windows.h>
typedef HANDLE spu_thread_handle_t;
typedef HANDLE spu_thread_event_t;
#else
#  include <pthread.h>
#  include <errno.h>
typedef pthread_t spu_thread_handle_t;
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    int             done;
} spu_thread_event_t;
#endif

typedef struct {
    int      in_use;
    uint32_t tid;
    uint32_t group_id;
    uint32_t index;
    int32_t  exit_status;
    uint32_t entry_point;
    uint32_t args_ea;
    uint32_t args_size;
    uint64_t args[4];
    uint32_t img_ea;
    struct spu_context* sctx;
    uint64_t spu_cfg;
    spu_thread_handle_t host_thread;
    spu_thread_event_t  finish_event;
    int                 running;
    spu_ppu_fallback_fn fb_handler;
    void*               fb_user;
    void*               live_ctx;
    volatile uint32_t   state_seq;
    uint32_t            pending_inmbox;
    uint8_t*            local_store;
    uint32_t            connected_queue;
    uint32_t            connect_spup;
    struct { uint32_t spup; uint32_t queue; } evt_bind[8];
    int                 evt_bind_n;
} spu_thread_t;

typedef struct {
    int      in_use;
    uint32_t id;
    int      state;
    uint32_t num_threads;
    uint32_t thread_indices[8];
    char     name[32];
    int32_t  exit_status;
    uint32_t cause;
    uint32_t event_queue_id;
    uint32_t user_event_ports[64];
} spu_group_t;

static spu_group_t  s_spu_groups[MAX_SPU_GROUPS];
static spu_thread_t s_spu_threads[MAX_SPU_THREADS];

static void spu_thread_signal(spu_thread_t* t)
{
    __atomic_add_fetch(&t->state_seq, 1u, __ATOMIC_RELEASE);
    ps3_futex_wake_all(&t->state_seq);
}
static uint32_t     s_spu_next_group_id  = 0x1000;
static uint32_t     s_spu_next_thread_id = 0x2000;

static spu_group_t* spu_find_group(uint32_t id)
{
    for (int i = 0; i < MAX_SPU_GROUPS; i++) {
        if (s_spu_groups[i].in_use && s_spu_groups[i].id == id)
            return &s_spu_groups[i];
    }
    return NULL;
}

static spu_group_t* spu_alloc_group(void)
{
    for (int i = 0; i < MAX_SPU_GROUPS; i++) {
        if (!s_spu_groups[i].in_use) {
            memset(&s_spu_groups[i], 0, sizeof(s_spu_groups[i]));
            s_spu_groups[i].in_use = 1;
            s_spu_groups[i].id     = s_spu_next_group_id++;
            s_spu_groups[i].state  = SPU_GROUP_STATE_INITIALIZED;
            s_spu_groups[i].exit_status = 0;
            s_spu_groups[i].cause  = SPU_GROUP_CAUSE_ALL_THREADS_EXIT;
            for (int k = 0; k < 8; k++)
                s_spu_groups[i].thread_indices[k] = MAX_SPU_THREADS;
            return &s_spu_groups[i];
        }
    }
    return NULL;
}

static spu_thread_t* spu_find_thread(uint32_t tid)
{
    for (int i = 0; i < MAX_SPU_THREADS; i++) {
        if (s_spu_threads[i].in_use && s_spu_threads[i].tid == tid)
            return &s_spu_threads[i];
    }
    return NULL;
}

static spu_thread_t* spu_alloc_thread(void)
{
    for (int i = 0; i < MAX_SPU_THREADS; i++) {
        if (!s_spu_threads[i].in_use) {
            memset(&s_spu_threads[i], 0, sizeof(s_spu_threads[i]));
            s_spu_threads[i].in_use = 1;
            s_spu_threads[i].tid    = s_spu_next_thread_id++;
            return &s_spu_threads[i];
        }
    }
    return NULL;
}

static void vm_write_be32(uint32_t guest_addr, uint32_t val)
{
    extern uint8_t* vm_base;
    if (!vm_base || !guest_addr) return;
    uint8_t* p = vm_base + guest_addr;
    p[0] = (uint8_t)(val >> 24);
    p[1] = (uint8_t)(val >> 16);
    p[2] = (uint8_t)(val >>  8);
    p[3] = (uint8_t)(val);
}

static uint32_t vm_read_be32(uint32_t guest_addr)
{
    extern uint8_t* vm_base;
    if (!vm_base || !guest_addr) return 0;
    const uint8_t* p = vm_base + guest_addr;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] <<  8) | (uint32_t)p[3];
}

static int64_t sys_ss_get_open_psid_handler(ppu_context* ctx)
{
    uint32_t ptr = (uint32_t)ctx->gpr[3];
    if (ptr) {
        for (uint32_t i = 0; i < 16; i += 4) vm_write_be32(ptr + i, 0);
    }
    ctx->gpr[3] = 0;
    return 0;
}

static int64_t sys_spu_initialize_handler(ppu_context* ctx)
{
    uint32_t nspu    = (uint32_t)ctx->gpr[3];
    uint32_t nrawspu = (uint32_t)ctx->gpr[4];
    fprintf(stderr, "[SPU] initialize(nspu=%u, nrawspu=%u)\n", nspu, nrawspu);
    fflush(stderr);
    ctx->gpr[3] = 0;
    return 0;
}

static int64_t sys_spu_thread_group_create_handler(ppu_context* ctx)
{
    extern uint8_t* vm_base;
    uint32_t out_ea   = (uint32_t)ctx->gpr[3];
    uint32_t num      = (uint32_t)ctx->gpr[4];
    int32_t  prio     = (int32_t)ctx->gpr[5];
    uint32_t attr_ea  = (uint32_t)ctx->gpr[6];
    fprintf(stderr, "[SPU] thread_group_create(num=%u prio=%d)\n", num, prio);

    spu_group_t* g = spu_alloc_group();
    if (!g) {
        fprintf(stderr, "[SPU] group_create: out of groups\n");
        fflush(stderr);
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_EAGAIN;
        return (int64_t)ctx->gpr[3];
    }
    if (num > 8) num = 8;
    g->num_threads = num;

    int32_t  gtype   = 0;
    uint32_t name_ea = 0;
    if (attr_ea) {
        uint32_t nsize = vm_read_be32(attr_ea + 0);
        name_ea        = vm_read_be32(attr_ea + 4);
        gtype          = (int32_t)vm_read_be32(attr_ea + 8);
        if (!nsize) name_ea = 0;
    }
    if (name_ea && vm_base) {
        const char* src = (const char*)(vm_base + name_ea);
        size_t i = 0;
        for (; i < sizeof(g->name) - 1 && src[i]; i++)
            g->name[i] = src[i];
        g->name[i] = 0;
    }

    vm_write_be32(out_ea, g->id);

    fprintf(stderr, "[SPU] group_create -> id=0x%X num=%u prio=%d type=0x%X name=%.31s\n",
            g->id, num, prio, gtype, g->name);
    fflush(stderr);
    ctx->gpr[3] = 0;
    return 0;
}

static int64_t sys_spu_thread_initialize_handler(ppu_context* ctx)
{
    uint32_t out_tid_ea = (uint32_t)ctx->gpr[3];
    uint32_t group_id   = (uint32_t)ctx->gpr[4];
    uint32_t thread_num = (uint32_t)ctx->gpr[5];
    uint32_t img_ea     = (uint32_t)ctx->gpr[6];
    uint32_t args_ea    = (uint32_t)ctx->gpr[8];

    spu_group_t* g = spu_find_group(group_id);
    if (!g) {
        fprintf(stderr, "[SPU] thread_init: group 0x%X not found\n", group_id);
        fflush(stderr);
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_ESRCH;
        return (int64_t)ctx->gpr[3];
    }
    spu_thread_t* t = spu_alloc_thread();
    if (!t) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_EAGAIN;
        return (int64_t)ctx->gpr[3];
    }
    t->group_id = group_id;
    t->index    = thread_num;
    if (img_ea) t->entry_point = vm_read_be32(img_ea + 4);
    t->img_ea    = img_ea;
    t->args_ea   = args_ea;
    t->args_size = 0;
    for (int a = 0; a < 4; a++) {
        uint64_t hi = args_ea ? vm_read_be32(args_ea + (uint32_t)a * 8)     : 0;
        uint64_t lo = args_ea ? vm_read_be32(args_ea + (uint32_t)a * 8 + 4) : 0;
        t->args[a] = (hi << 32) | lo;
    }

    if (thread_num < 8)
        g->thread_indices[thread_num] = (uint32_t)(t - s_spu_threads);

    vm_write_be32(out_tid_ea, t->tid);

    fprintf(stderr, "[SPU] thread_init group=0x%X index=%u img=0x%08X args=0x%08X -> tid=0x%X entry=0x%08X\n",
            group_id, thread_num, img_ea, args_ea, t->tid, t->entry_point);
    fflush(stderr);
    ctx->gpr[3] = 0;
    return 0;
}

static size_t spu_host_stack_bytes(void)
{
    return (size_t)16 * 1024 * 1024;
}

#ifndef _WIN32
static void spu_spawn_host_thread(spu_thread_handle_t* out,
                                  void* (*fn)(void*), void* arg)
{
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    int rc = EINVAL;
    if (pthread_attr_setstacksize(&attr, spu_host_stack_bytes()) == 0)
        rc = pthread_create(out, &attr, fn, arg);
    pthread_attr_destroy(&attr);
    if (rc != 0)
        pthread_create(out, NULL, fn, arg);
}
#endif

#ifdef _WIN32
static DWORD WINAPI spu_exec_thread_proc(LPVOID arg)
#else
static void* spu_exec_thread_proc(void* arg)
#endif
{
#ifdef _WIN32
    { ULONG g = 256 * 1024; SetThreadStackGuarantee(&g); }
#endif
    spu_thread_t* t = (spu_thread_t*)arg;
    { const spu_group_t* g = spu_find_group(t->group_id);
      ps3_thread_name("S%u.%u %s", t->group_id & 0xFFFFu, t->index, g && g->name[0] ? g->name : "group"); }
    spu_lifted_thread_result r;
    spu_lifted_thread_run(t->sctx, &r);
    t->exit_status = r.exit_status;
    if (r.group_exit) {
        spu_group_t* g = spu_find_group(t->group_id);
        if (g) {
            g->exit_status = r.group_status;
            g->cause = SPU_GROUP_CAUSE_GROUP_EXIT;
        }
    }
#ifdef _WIN32
    t->running = 0;
    spu_thread_signal(t);
    SetEvent(t->finish_event);
    return 0;
#else
    pthread_mutex_lock(&t->finish_event.mu);
    t->running = 0;
    t->finish_event.done = 1;
    pthread_cond_broadcast(&t->finish_event.cv);
    pthread_mutex_unlock(&t->finish_event.mu);
    spu_thread_signal(t);
    return NULL;
#endif
}

#ifdef _WIN32
static DWORD WINAPI spu_fallback_thread_proc(LPVOID arg)
#else
static void* spu_fallback_thread_proc(void* arg)
#endif
{
#ifdef _WIN32
    { ULONG g = 256 * 1024; SetThreadStackGuarantee(&g); }
#endif
    spu_thread_t* t = (spu_thread_t*)arg;
    { const spu_group_t* g = spu_find_group(t->group_id);
      ps3_thread_name("H%u.%u %s", t->group_id & 0xFFFFu, t->index, g && g->name[0] ? g->name : "group"); }
    int32_t rc = 0;
    if (t->fb_handler) {
        rc = t->fb_handler(t->tid, t->args_ea, t->args_size, t->fb_user);
    }
    t->exit_status = rc;
#ifdef _WIN32
    t->running = 0;
    spu_thread_signal(t);
    SetEvent(t->finish_event);
    return 0;
#else
    pthread_mutex_lock(&t->finish_event.mu);
    t->running = 0;
    t->finish_event.done = 1;
    pthread_cond_broadcast(&t->finish_event.cv);
    pthread_mutex_unlock(&t->finish_event.mu);
    spu_thread_signal(t);
    return NULL;
#endif
}

extern int32_t spu_registry_fallback(uint32_t, uint32_t, uint32_t, void*);

static size_t spu_thread_image(const spu_thread_t* t, uint32_t* src, uint64_t* fp, int* iid, int* registered)
{
    extern uint32_t ps3_spu_image_source_ea(uint32_t img_ea);
    *src = ps3_spu_image_source_ea(t->img_ea);
    *fp = 0;
    *registered = 0;
    const size_t isz = *src ? spu_elf_image_size(vm_base + *src, 1u << 20) : 0;
    if (isz) {
        *fp = spu_workload_fingerprint(vm_base + *src, isz);
        *registered = spu_workload_find_img(*fp, iid) != 0;
    }
    return isz;
}

static int spu_thread_image_id(const spu_thread_t* t)
{
    uint32_t src; uint64_t fp; int iid = 0, registered;
    if (!t->img_ea || !vm_base) return 0;
    return spu_thread_image(t, &src, &fp, &iid, &registered) && registered ? iid : 0;
}

static int64_t sys_spu_thread_group_start_handler(ppu_context* ctx)
{
    uint32_t id = (uint32_t)ctx->gpr[3];
    spu_group_t* g = spu_find_group(id);
    if (!g) { ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_ESRCH; return (int64_t)ctx->gpr[3]; }
    for (uint32_t i = 0; i < g->num_threads && i < 8; i++) {
        uint32_t idx = g->thread_indices[i];
        if (idx < MAX_SPU_THREADS && s_spu_threads[idx].in_use &&
            *(volatile int*)&s_spu_threads[idx].running && s_spu_threads[idx].sctx) {
            fprintf(stderr, "[SPU] group_start id=0x%X: thread 0x%X still running -> CELL_ESTAT\n",
                    id, s_spu_threads[idx].tid);
            ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_ESTAT;
            return (int64_t)ctx->gpr[3];
        }
    }
    g->state = SPU_GROUP_STATE_RUNNING;
    g->cause       = 0;
    g->exit_status = 0;

    int spawned = 0;
    int instant = 0;
    int nofb    = 0;
    for (uint32_t i = 0; i < g->num_threads && i < 8; i++) {
        uint32_t idx = g->thread_indices[i];
        if (idx >= MAX_SPU_THREADS) continue;
        spu_thread_t* t = &s_spu_threads[idx];
        if (!t->in_use) continue;

        if (spu_lifted_thread_available(t->entry_point)) {
            if (!t->sctx) t->sctx = (spu_context*)calloc(1, sizeof(spu_context));
            if (t->sctx) {
                spu_lifted_thread_desc d;
                d.tid      = t->tid;
                d.group_id = id;
                d.entry    = t->entry_point;
                d.img_ea   = t->img_ea;
                d.image_id = spu_thread_image_id(t);
                for (int a = 0; a < 4; a++) d.args[a] = t->args[a];
                spu_lifted_thread_setup(t->sctx, &d);
                t->running = 1;
#ifdef _WIN32
                if (!t->finish_event)
                    t->finish_event = CreateEventA(NULL, TRUE, FALSE, NULL);
                else
                    ResetEvent(t->finish_event);
                t->host_thread = CreateThread(NULL, spu_host_stack_bytes(),
                                              spu_exec_thread_proc, t,
                                              STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
#else
                pthread_mutex_init(&t->finish_event.mu, NULL);
                pthread_cond_init(&t->finish_event.cv, NULL);
                t->finish_event.done = 0;
                spu_spawn_host_thread(&t->host_thread, spu_exec_thread_proc, t);
#endif
                fprintf(stderr, "[SPU] group_start id=0x%X tid=0x%X entry=0x%08X "
                        "args=0x%08X -> LIFTED SPU execution (image %d, "
                        "r3=0x%08X%08X r4=0x%08X%08X r5=0x%08X%08X r6=0x%08X%08X)\n",
                        id, t->tid, t->entry_point, t->args_ea, t->sctx->image_id,
                        t->sctx->gpr[3]._u32[0], t->sctx->gpr[3]._u32[1],
                        t->sctx->gpr[4]._u32[0], t->sctx->gpr[4]._u32[1],
                        t->sctx->gpr[5]._u32[0], t->sctx->gpr[5]._u32[1],
                        t->sctx->gpr[6]._u32[0], t->sctx->gpr[6]._u32[1]);
                spawned++;
                continue;
            }
        }

        void* user = NULL;
        spu_ppu_fallback_fn fb = NULL;
        if (t->img_ea && vm_base) {
            extern int32_t spu_registry_fallback(uint32_t, uint32_t, uint32_t, void*);
            uint32_t src; uint64_t fp; int iid = 0, registered;
            const size_t isz = spu_thread_image(t, &src, &fp, &iid, &registered);
            if (isz) {
                if (registered) {
                    fb = spu_registry_fallback;
                    user = (void*)(uintptr_t)fp;
                    fprintf(stderr, "[SPU] thread tid=0x%X image @0x%08X (%u bytes) "
                            "matched lifted workload fp=0x%016llX image_id=%d\n",
                            t->tid, src, (unsigned)isz,
                            (unsigned long long)fp, iid);
                    { extern int spu_elf_load_to_ls(const uint8_t*, size_t,
                                                    uint8_t*, uint32_t*);
                      uint8_t* ls = spu_thread_get_local_store(t->tid);
                      uint32_t ls_entry = 0;
                      if (ls && spu_elf_load_to_ls(vm_base + src, isz, ls, &ls_entry))
                          fprintf(stderr, "[SPU] thread tid=0x%X local store loaded "
                                  "(entry 0x%05X)\n", t->tid, ls_entry);
                      else
                          fprintf(stderr, "[SPU] thread tid=0x%X LOCAL STORE LOAD FAILED "
                                  "-- worker will run against zeroes\n", t->tid);
                    }
                } else {
                    fprintf(stderr, "[SPU] thread tid=0x%X image @0x%08X (%u bytes) "
                            "fp=0x%016llX is NOT in the workload registry\n",
                            t->tid, src, (unsigned)isz, (unsigned long long)fp);
                }
            }
        }
        if (!fb) {
            t->exit_status = 0;
            t->running = 0;
            instant++; nofb++;
            continue;
        }
        t->fb_handler = fb;
        t->fb_user    = user;
        t->running    = 1;
#ifdef _WIN32
        if (!t->finish_event)
            t->finish_event = CreateEventA(NULL, TRUE, FALSE, NULL);
        else
            ResetEvent(t->finish_event);
        t->host_thread = CreateThread(NULL, spu_host_stack_bytes(),
                                      spu_fallback_thread_proc, t,
                                      STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
#else
        pthread_mutex_init(&t->finish_event.mu, NULL);
        pthread_cond_init(&t->finish_event.cv, NULL);
        t->finish_event.done = 0;
        spu_spawn_host_thread(&t->host_thread, spu_fallback_thread_proc, t);
#endif
        if (fb == spu_registry_fallback) {
            for (;;) {
                const uint32_t seq = __atomic_load_n(&t->state_seq, __ATOMIC_ACQUIRE);
                if (__atomic_load_n(&t->live_ctx, __ATOMIC_ACQUIRE) || !__atomic_load_n(&t->running, __ATOMIC_ACQUIRE)) break;
                ps3_futex_wait(&t->state_seq, seq, 0);
            }
            if (!t->live_ctx)
                fprintf(stderr, "[SPU] group_start tid=0x%X: worker never published "
                        "a context -- mailbox writes will fall back to re-runs\n",
                        t->tid);
        }
        fprintf(stderr, "[SPU] group_start id=0x%X tid=0x%X entry=0x%08X args=0x%08X -> spawned host thread\n",
                id, t->tid, t->entry_point, t->args_ea);
        spawned++;
    }

    if (spawned == 0) {
        g->state = SPU_GROUP_STATE_STOPPED;
        g->cause = SPU_GROUP_CAUSE_ALL_THREADS_EXIT;
        g->exit_status = 0;
        fprintf(stderr, "[SPU] group_start id=0x%X (%u thread(s), none spawned: %d ran synchronously, %d had no fallback)\n",
                id, g->num_threads, instant - nofb, nofb);
    } else {
        fprintf(stderr, "[SPU] group_start id=0x%X (%d host threads running, %d instant)\n",
                id, spawned, instant);
    }
    fflush(stderr);
    ctx->gpr[3] = 0;
    return 0;
}

static int64_t sys_spu_thread_group_join_handler(ppu_context* ctx)
{
    uint32_t id         = (uint32_t)ctx->gpr[3];
    uint32_t cause_ea   = (uint32_t)ctx->gpr[4];
    uint32_t status_ea  = (uint32_t)ctx->gpr[5];

    spu_group_t* g = spu_find_group(id);
    if (!g) {
        vm_write_be32(cause_ea,  SPU_GROUP_CAUSE_ALL_THREADS_EXIT);
        vm_write_be32(status_ea, 0);
        fprintf(stderr, "[SPU] group_join id=0x%X (unknown, faked ok)\n", id);
        fflush(stderr);
        ctx->gpr[3] = 0;
        return 0;
    }
    if (g->state == SPU_GROUP_STATE_INITIALIZED ||
        g->state == SPU_GROUP_STATE_READY) {
        g->state = SPU_GROUP_STATE_STOPPED;
    }

    if (g->state == SPU_GROUP_STATE_RUNNING) {
        int32_t worst = 0;
        for (int i = 0; i < 8 && i < (int)g->num_threads; i++) {
            uint32_t idx = g->thread_indices[i];
            if (idx >= MAX_SPU_THREADS) continue;
            spu_thread_t* t = &s_spu_threads[idx];
            if (!t->in_use) continue;
            if (t->running) {
#ifdef _WIN32
                if (t->finish_event)
                    WaitForSingleObject(t->finish_event, INFINITE);
                if (t->host_thread) {
                    CloseHandle(t->host_thread);
                    t->host_thread = NULL;
                }
#else
                pthread_mutex_lock(&t->finish_event.mu);
                while (!t->finish_event.done)
                    pthread_cond_wait(&t->finish_event.cv, &t->finish_event.mu);
                pthread_mutex_unlock(&t->finish_event.mu);
                pthread_join(t->host_thread, NULL);
                pthread_mutex_destroy(&t->finish_event.mu);
                pthread_cond_destroy(&t->finish_event.cv);
#endif
            }
            if (t->exit_status < worst) worst = t->exit_status;
        }
        if (g->cause != SPU_GROUP_CAUSE_GROUP_EXIT &&
            g->cause != SPU_GROUP_CAUSE_TERMINATED) {
            g->cause       = SPU_GROUP_CAUSE_ALL_THREADS_EXIT;
            g->exit_status = worst;
        }
        g->state       = SPU_GROUP_STATE_STOPPED;
    }

    if (g->event_queue_id) {
        sys_event_queue_push_by_id(g->event_queue_id,
                                   (uint64_t)g->id,
                                   (uint64_t)(int64_t)g->exit_status,
                                   (uint64_t)g->cause,
                                   0);
    }

    vm_write_be32(cause_ea,  g->cause);
    vm_write_be32(status_ea, (uint32_t)g->exit_status);

    fprintf(stderr, "[SPU] group_join id=0x%X cause=%u status=%d (event_queue=0x%X)\n",
            id, g->cause, g->exit_status, g->event_queue_id);
    fflush(stderr);
    ctx->gpr[3] = 0;
    return 0;
}

static int64_t sys_spu_thread_group_destroy_handler(ppu_context* ctx)
{
    uint32_t id = (uint32_t)ctx->gpr[3];
    spu_group_t* g = spu_find_group(id);
    if (g) {
        int busy = 0;
        const uint64_t deadline = ps3_sync_deadline_us(250000);
        for (int i = 0; i < 8 && i < (int)g->num_threads; i++) {
            uint32_t idx = g->thread_indices[i];
            if (idx >= MAX_SPU_THREADS) continue;
            spu_thread_t* st = &s_spu_threads[idx];
            if (!st->in_use || !(st->sctx || st->local_store)) continue;
            for (;;) {
                const uint32_t seq = __atomic_load_n(&st->state_seq, __ATOMIC_ACQUIRE);
                if (!__atomic_load_n(&st->running, __ATOMIC_ACQUIRE)) break;
                if (!ps3_futex_wait(&st->state_seq, seq, deadline)) {
                    busy = __atomic_load_n(&st->running, __ATOMIC_ACQUIRE) != 0;
                    break;
                }
            }
            if (busy) break;
        }
        if (busy) {
            fprintf(stderr, "[SPU] group_destroy id=0x%X: a thread is still running "
                            "-> CELL_EBUSY (nothing freed)\n", id);
            fflush(stderr);
            ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_EBUSY;
            return (int64_t)ctx->gpr[3];
        }
        for (int i = 0; i < 8 && i < (int)g->num_threads; i++) {
            uint32_t idx = g->thread_indices[i];
            if (idx < MAX_SPU_THREADS) {
                spu_thread_t* t = &s_spu_threads[idx];
                if (t->local_store) {
                    free(t->local_store);
                    t->local_store = NULL;
                }
                if (t->sctx) {
                    free(t->sctx);
                    t->sctx = NULL;
                }
                t->in_use = 0;
            }
        }
        g->in_use = 0;
    }
    fprintf(stderr, "[SPU] group_destroy id=0x%X  caller_lr=0x%08X cia=0x%08X r3..r6=%08X %08X %08X %08X\n",
            id, (uint32_t)ctx->lr, (uint32_t)ctx->cia,
            (uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4],
            (uint32_t)ctx->gpr[5], (uint32_t)ctx->gpr[6]);
    fflush(stderr);
    ctx->gpr[3] = 0;
    return 0;
}

static int64_t sys_spu_thread_group_terminate_handler(ppu_context* ctx)
{
    uint32_t id     = (uint32_t)ctx->gpr[3];
    int32_t  status = (int32_t)ctx->gpr[4];
    spu_group_t* g = spu_find_group(id);
    if (g) {
        g->state = SPU_GROUP_STATE_STOPPED;
        g->cause = SPU_GROUP_CAUSE_TERMINATED;
        g->exit_status = status;
    }
    fprintf(stderr, "[SPU] group_terminate id=0x%X status=%d\n", id, status);
    fflush(stderr);
    ctx->gpr[3] = 0;
    return 0;
}

static int64_t sys_spu_thread_get_exit_status_handler(ppu_context* ctx)
{
    uint32_t tid       = (uint32_t)ctx->gpr[3];
    uint32_t status_ea = (uint32_t)ctx->gpr[4];
    spu_thread_t* t = spu_find_thread(tid);
    if (!t) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010005;
        return (int64_t)ctx->gpr[3];
    }
    if (t->running) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_ESTAT;
        return (int64_t)ctx->gpr[3];
    }
    vm_write_be32(status_ea, (uint32_t)t->exit_status);
    ctx->gpr[3] = 0;
    return 0;
}

static int64_t sys_spu_thread_set_argument_handler(ppu_context* ctx)
{
    uint32_t tid    = (uint32_t)ctx->gpr[3];
    uint32_t arg_ea = (uint32_t)ctx->gpr[4];

    spu_thread_t* t = spu_find_thread(tid);
    if (t) {
        t->args_ea = arg_ea;
        for (int a = 0; a < 4; a++) {
            uint64_t hi = arg_ea ? vm_read_be32(arg_ea + (uint32_t)a * 8)     : 0;
            uint64_t lo = arg_ea ? vm_read_be32(arg_ea + (uint32_t)a * 8 + 4) : 0;
            t->args[a] = (hi << 32) | lo;
        }
    }

    fprintf(stderr, "[SPU] thread_set_argument tid=0x%X arg=0x%08X\n",
            tid, arg_ea);
    fflush(stderr);
    ctx->gpr[3] = 0;
    return 0;
}

static int64_t sys_spu_thread_write_spu_mb_handler(ppu_context* ctx)
{
    uint32_t tid = (uint32_t)ctx->gpr[3];
    uint32_t val = (uint32_t)ctx->gpr[4];

    spu_thread_t* t = spu_find_thread(tid);
    if (!t) {
        static int warned = 0;
        if (PS3_LOG_FIRST(warned, 8)) {
            fprintf(stderr, "[SPU] write_spu_mb: thread 0x%X not found -- "
                            "dropping the word and reporting CELL_OK, as the "
                            "unimplemented stub did\n", tid);
            fflush(stderr);
        }
        ctx->gpr[3] = CELL_OK;
        return CELL_OK;
    }

    if (t->live_ctx || t->sctx) {
        extern void spu_ch_wake(spu_context* c);
        spu_context* c = t->live_ctx ? (spu_context*)t->live_ctx : t->sctx;

        { static int _n = 0;
          if (PS3_LOG_FIRST(_n, 32))
              fprintf(stderr, "[SPU] write_spu_mb tid=0x%X val=0x%08X -> live worker\n",
                      tid, val); }
        spu_channel_write(&c->ch_in_mbox, val);
        spu_ch_wake(c);
        ctx->gpr[3] = 0;
        return 0;
    }

    t->pending_inmbox = val;
    fprintf(stderr, "[SPU] write_spu_mb tid=0x%X val=0x%08X%s\n", tid, val,
            t->fb_handler ? " -> re-running worker" : " (no fallback: queued only)");
    fflush(stderr);

    if (t->fb_handler)
        t->exit_status = t->fb_handler(t->tid, t->args_ea, t->args_size, t->fb_user);

    ctx->gpr[3] = 0;
    return 0;
}

static int64_t sys_spu_thread_group_connect_event_handler(ppu_context* ctx)
{
    uint32_t group_id = (uint32_t)ctx->gpr[3];
    uint32_t queue_id = (uint32_t)ctx->gpr[4];
    spu_group_t* g = spu_find_group(group_id);
    if (!g) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010005;
        return (int64_t)ctx->gpr[3];
    }
    g->event_queue_id = queue_id;
    fprintf(stderr, "[SPU] group_connect_event group=0x%X queue=0x%X\n",
            group_id, queue_id);
    fflush(stderr);
    ctx->gpr[3] = 0;
    return 0;
}

static SRWLOCK s_spu_port_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE s_spu_port_bound = CONDITION_VARIABLE_INIT;
#define SPU_PORT_BIND_WAIT_MS 200u

static uint32_t spu_port_queue_locked(const spu_thread_t* t, uint32_t group_id, unsigned port)
{
    if (t) for (int i = 0; i < t->evt_bind_n; i++)
        if (t->evt_bind[i].spup == port) return t->evt_bind[i].queue;
    spu_group_t* g = group_id ? spu_find_group(group_id) : NULL;
    return g ? g->user_event_ports[port & 63] : 0;
}

static int64_t sys_spu_thread_group_connect_event_all_threads_handler(ppu_context* ctx)
{
    spu_group_t* g = spu_find_group((uint32_t)ctx->gpr[3]);
    uint64_t requested = ctx->gpr[5];
    uint32_t output = (uint32_t)ctx->gpr[6];
    uint32_t result = CELL_OK;
    if (!g) result = CELL_ESRCH;
    else if (!requested) result = CELL_EINVAL;
    else if (!output) result = CELL_EFAULT;
    else {
        result = CELL_EISCONN;
        AcquireSRWLockExclusive(&s_spu_port_lock);
        for (unsigned port = 0; port < 64; ++port) {
            if ((requested & (1ull << port)) && !g->user_event_ports[port]) {
                g->user_event_ports[port] = (uint32_t)ctx->gpr[4];
                vm_base[output] = (uint8_t)port;
                result = CELL_OK;
                WakeAllConditionVariable(&s_spu_port_bound);
                break;
            }
        }
        ReleaseSRWLockExclusive(&s_spu_port_lock);
    }
    ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)result;
    return (int64_t)(int32_t)result;
}

static int64_t sys_spu_thread_group_disconnect_event_all_threads_handler(ppu_context* ctx)
{
    const uint32_t port = (uint32_t)ctx->gpr[4];
    spu_group_t* g = port > 63u ? NULL : spu_find_group((uint32_t)ctx->gpr[3]);
    uint32_t result = CELL_OK;
    if (port > 63u) result = CELL_EINVAL;
    else if (!g) result = CELL_ESRCH;
    else {
        AcquireSRWLockExclusive(&s_spu_port_lock);
        g->user_event_ports[port] = 0;
        ReleaseSRWLockExclusive(&s_spu_port_lock);
    }
    ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)result;
    return (int64_t)(int32_t)result;
}
static int spu_deliver_user_event(spu_context* spu, uint32_t value)
{
    unsigned code = value >> 24;
    if (!spu->spu_group_id) {
        if (code < 128) { static int n; if (PS3_LOG_FIRST(n, 16))
            fprintf(stderr, "[spu-evt] SPURS user event on unbound port %u dropped (img=%d value=0x%08X)\n",
                    code & 63, spu->image_id, value); }
        return 0;
    }
    if (code == 128 || code == 192) {
        uint32_t result = CELL_EINVAL;
        if (spu->ch_out_mbox.count) {
            uint32_t flag_id = spu_channel_read(&spu->ch_out_mbox);
            uint32_t bit = value & 0xFFFFFFu;
            if (bit < 64) {
                ppu_context call = {0};
                call.gpr[3] = flag_id;
                call.gpr[4] = 1ull << bit;
                result = (uint32_t)sys_event_flag_set(&call);
            }
        }
        if (code == 128) spu_channel_write(&spu->ch_in_mbox, result);
        return 1;
    }
    if (code >= 128) return 0;
    uint32_t result = CELL_EINVAL;
    if (spu->ch_out_mbox.count) {
        uint32_t data = spu_channel_read(&spu->ch_out_mbox);
        unsigned port = code & 63;
        uint32_t queue = 0;
        {
            const ULONGLONG deadline = GetTickCount64() + SPU_PORT_BIND_WAIT_MS;
            AcquireSRWLockExclusive(&s_spu_port_lock);
            for (;;) {
                const spu_thread_t* bt = spu_find_thread(spu->spu_id);
                if ((queue = spu_port_queue_locked(bt, spu->spu_group_id, port)) != 0) break;
                const ULONGLONG now = GetTickCount64();
                if (!bt || now >= deadline) break;
                SleepConditionVariableSRW(&s_spu_port_bound, &s_spu_port_lock, (DWORD)(deadline - now), 0);
            }
            ReleaseSRWLockExclusive(&s_spu_port_lock);
        }
        result = CELL_ENOTCONN;
        if (queue) {
            int rc = sys_event_queue_push_by_id(queue, 0xFFFFFFFF53505501ull,
                spu->spu_id, ((uint64_t)port << 32) | (value & 0xFFFFFFu), data);
            result = rc == 0 ? CELL_OK : CELL_EBUSY;
        }
    }
    if (code < 64) spu_channel_write(&spu->ch_in_mbox, result);
    return 1;
}

static int64_t sys_spu_thread_group_disconnect_event_handler(ppu_context* ctx)
{
    uint32_t group_id = (uint32_t)ctx->gpr[3];
    spu_group_t* g = spu_find_group(group_id);
    if (g) g->event_queue_id = 0;
    fprintf(stderr, "[SPU] group_disconnect_event group=0x%X\n", group_id);
    fflush(stderr);
    ctx->gpr[3] = 0;
    return 0;
}

static int64_t sys_spu_thread_connect_event_handler(ppu_context* ctx)
{
    uint32_t tid = (uint32_t)ctx->gpr[3];
    uint32_t eq  = (uint32_t)ctx->gpr[4];
    uint32_t et  = (uint32_t)ctx->gpr[5];
    spu_thread_t* t = spu_find_thread(tid);
    uint32_t spup = (uint32_t)ctx->gpr[6];
    if (t) {
        if (!t->connected_queue) { t->connected_queue = eq; t->connect_spup = spup; }
        AcquireSRWLockExclusive(&s_spu_port_lock);
        int slot = -1;
        for (int i = 0; i < t->evt_bind_n; i++)
            if (t->evt_bind[i].spup == spup) { slot = i; break; }
        if (slot < 0 && t->evt_bind_n < 8) slot = t->evt_bind_n++;
        if (slot >= 0) { t->evt_bind[slot].spup = spup; t->evt_bind[slot].queue = eq; }
        WakeAllConditionVariable(&s_spu_port_bound);
        ReleaseSRWLockExclusive(&s_spu_port_lock);
    }
    fprintf(stderr, "[SPU] thread_connect_event tid=0x%X queue=0x%X et=0x%X spup=0x%X%s\n",
            tid, eq, et, (uint32_t)ctx->gpr[6], t ? "" : " (thread not found)");
    fflush(stderr);
    ctx->gpr[3] = t ? 0 : (uint64_t)(int64_t)(int32_t)CELL_ESRCH;
    return (int64_t)ctx->gpr[3];
}

extern int sys_event_queue_push_by_id(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t);
static void spu_out_mbox_deliver(uint32_t group_id, uint32_t spu_id,
                                      int is_intr, uint32_t value)
{
    uint32_t q = 0;
    spu_thread_t* t = spu_find_thread(spu_id);
    if (t) {
        uint32_t port = (value >> 24) & 0xFF;
        AcquireSRWLockShared(&s_spu_port_lock);
        q = spu_port_queue_locked(t, 0, port);
        ReleaseSRWLockShared(&s_spu_port_lock);
    }
    if (!q && t && t->connected_queue) q = t->connected_queue;
    if (!q) { spu_group_t* g = spu_find_group(group_id); if (g) q = g->event_queue_id; }
    if (!q) return;
    sys_event_queue_push_by_id(q,
        ((uint64_t)spu_id << 32) | (is_intr ? 0x2u : 0x1u),
        (uint64_t)value, 0, 0);
    { static int s_w = 0; if (PS3_LOG_FIRST(s_w, 32))
        fprintf(stderr, "[SPU->PPU] mbox deliver spu=0x%X intr=%d val=0x%08X -> q=%u\n",
                spu_id, is_intr, value, q); }
}

#define SPU_LS_SIZE  (256 * 1024)
static uint8_t* spu_thread_get_or_alloc_ls(spu_thread_t* t)
{
    if (!t) return NULL;
    if (t->sctx) return t->sctx->ls;
    if (!t->local_store) {
        t->local_store = (uint8_t*)calloc(1, SPU_LS_SIZE);
    }
    return t->local_store;
}

static int64_t sys_spu_thread_write_ls_handler(ppu_context* ctx)
{
    uint32_t tid       = (uint32_t)ctx->gpr[3];
    uint32_t ls_offset = (uint32_t)ctx->gpr[4];
    uint64_t value     = (uint64_t)ctx->gpr[5];
    uint32_t type      = (uint32_t)ctx->gpr[6];
    spu_thread_t* t = spu_find_thread(tid);
    if (!t) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010005;
        return (int64_t)ctx->gpr[3];
    }
    if (ls_offset + type > SPU_LS_SIZE) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010002;
        return (int64_t)ctx->gpr[3];
    }
    uint8_t* ls = spu_thread_get_or_alloc_ls(t);
    if (!ls) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010004;
        return (int64_t)ctx->gpr[3];
    }
    switch (type) {
    case 1: ls[ls_offset] = (uint8_t)value; break;
    case 2: ls[ls_offset+0] = (uint8_t)(value >> 8);
            ls[ls_offset+1] = (uint8_t)value; break;
    case 4: for (int i = 0; i < 4; i++)
                ls[ls_offset+i] = (uint8_t)(value >> ((3-i)*8));
            break;
    case 8: for (int i = 0; i < 8; i++)
                ls[ls_offset+i] = (uint8_t)(value >> ((7-i)*8));
            break;
    default:
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010002;
        return (int64_t)ctx->gpr[3];
    }
    ctx->gpr[3] = 0;
    return 0;
}

static int64_t sys_spu_thread_read_ls_handler(ppu_context* ctx)
{
    extern uint8_t* vm_base;
    uint32_t tid       = (uint32_t)ctx->gpr[3];
    uint32_t ls_offset = (uint32_t)ctx->gpr[4];
    uint32_t value_ea  = (uint32_t)ctx->gpr[5];
    uint32_t type      = (uint32_t)ctx->gpr[6];
    spu_thread_t* t = spu_find_thread(tid);
    if (!t || !value_ea || !vm_base) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010005;
        return (int64_t)ctx->gpr[3];
    }
    if (ls_offset + type > SPU_LS_SIZE) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010002;
        return (int64_t)ctx->gpr[3];
    }
    uint8_t* ls = spu_thread_get_or_alloc_ls(t);
    if (!ls) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010004;
        return (int64_t)ctx->gpr[3];
    }
    uint64_t value = 0;
    switch (type) {
    case 1: value = ls[ls_offset]; break;
    case 2: value = ((uint64_t)ls[ls_offset] << 8) | ls[ls_offset+1]; break;
    case 4:
        for (int i = 0; i < 4; i++)
            value = (value << 8) | ls[ls_offset+i];
        break;
    case 8:
        for (int i = 0; i < 8; i++)
            value = (value << 8) | ls[ls_offset+i];
        break;
    default:
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010002;
        return (int64_t)ctx->gpr[3];
    }
    uint8_t* p = vm_base + value_ea;
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(value >> ((7-i)*8));
    ctx->gpr[3] = 0;
    return 0;
}

uint8_t* spu_thread_get_local_store(uint32_t tid)
{
    return spu_thread_get_or_alloc_ls(spu_find_thread(tid));
}

uint32_t spu_thread_get_group_id(uint32_t tid)
{
    spu_thread_t* t = spu_find_thread(tid);
    return t ? t->group_id : 0;
}

void spu_thread_publish_ctx(uint32_t tid, void* c)
{
    spu_thread_t* t = spu_find_thread(tid);
    if (t) {
        t->live_ctx = c;
        spu_thread_signal(t);
    }
}

uint32_t spu_thread_take_pending_inmbox(uint32_t tid)
{
    spu_thread_t* t = spu_find_thread(tid);
    if (!t) return 0;
    uint32_t v = t->pending_inmbox;
    t->pending_inmbox = 0;
    return v;
}

static uint16_t vm_read_be16(uint32_t a)
{
    extern uint8_t* vm_base;
    if (!vm_base || !a) return 0;
    const uint8_t* p = vm_base + a;
    return (uint16_t)((p[0] << 8) | p[1]);
}

static int64_t sys_spu_image_import_handler(ppu_context* ctx)
{
    extern uint8_t* vm_base;
    uint32_t img_ea = (uint32_t)ctx->gpr[3];
    uint32_t src_ea = (uint32_t)ctx->gpr[4];
    uint32_t itype  = (uint32_t)ctx->gpr[5];
    (void)itype;

    if (!img_ea || !src_ea || !vm_base) {
        if (img_ea && vm_base) memset(vm_base + img_ea, 0, 16);
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_EFAULT;
        return (int64_t)ctx->gpr[3];
    }

    const uint8_t* e = vm_base + src_ea;
    if (!(e[0] == 0x7F && e[1] == 'E' && e[2] == 'L' && e[3] == 'F')) {
        memset(vm_base + img_ea, 0, 16);
        fprintf(stderr, "[SPU] image_import img=0x%08X src=0x%08X -- not an ELF\n", img_ea, src_ea);
        fflush(stderr);
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_ENOEXEC;
        return (int64_t)ctx->gpr[3];
    }

    uint32_t entry   = vm_read_be32(src_ea + 0x18);
    spu_raw_note_image(src_ea, entry);
    uint32_t phoff   = vm_read_be32(src_ea + 0x1C);
    uint16_t phentsz = vm_read_be16(src_ea + 0x2A);
    uint16_t phnum   = vm_read_be16(src_ea + 0x2C);
    if (phentsz == 0) phentsz = 0x20;

    static uint32_t s_spu_seg_bump = 0x0D000000u;
    uint32_t segs_ea = s_spu_seg_bump;
    int nsegs = 0;

    for (uint16_t i = 0; i < phnum && nsegs < 32; i++) {
        uint32_t ph = phoff + (uint32_t)i * phentsz;
        if (vm_read_be32(src_ea + ph + 0x00) != 1) continue;
        uint32_t p_off = vm_read_be32(src_ea + ph + 0x04);
        uint32_t p_va  = vm_read_be32(src_ea + ph + 0x08);
        uint32_t p_fsz = vm_read_be32(src_ea + ph + 0x10);
        uint32_t p_msz = vm_read_be32(src_ea + ph + 0x14);

        uint32_t seg = segs_ea + (uint32_t)nsegs * 0x18;
        vm_write_be32(seg + 0x00, 1);
        vm_write_be32(seg + 0x04, p_va);
        vm_write_be32(seg + 0x08, p_fsz);
        vm_write_be32(seg + 0x10, src_ea + p_off);
        vm_write_be32(seg + 0x14, src_ea + p_off);
        nsegs++;

        if (p_msz > p_fsz && nsegs < 32) {
            seg = segs_ea + (uint32_t)nsegs * 0x18;
            vm_write_be32(seg + 0x00, 2);
            vm_write_be32(seg + 0x04, p_va + p_fsz);
            vm_write_be32(seg + 0x08, p_msz - p_fsz);
            vm_write_be32(seg + 0x10, 0);
            vm_write_be32(seg + 0x14, 0);
            nsegs++;
        }
    }
    s_spu_seg_bump += (uint32_t)nsegs * 0x18;
    if (s_spu_seg_bump >= 0x0E000000u) s_spu_seg_bump = 0x0D000000u;

    vm_write_be32(img_ea + 0x00, 0);
    vm_write_be32(img_ea + 0x04, entry);
    vm_write_be32(img_ea + 0x08, nsegs ? segs_ea : 0);
    vm_write_be32(img_ea + 0x0C, (uint32_t)nsegs);

    fprintf(stderr, "[SPU] image_import img=0x%08X src=0x%08X -> entry=0x%05X nsegs=%d\n",
            img_ea, src_ea, entry, nsegs);
    fflush(stderr);
    ctx->gpr[3] = 0;
    return 0;
}

static int64_t sys_spu_image_open_handler(ppu_context* ctx)
{
    extern uint8_t* vm_base;
    uint32_t img_ea  = (uint32_t)ctx->gpr[3];
    uint32_t path_ea = (uint32_t)ctx->gpr[4];

    if (img_ea && vm_base) {
        memset(vm_base + img_ea, 0, 16);
        vm_write_be32(img_ea + 0, 1);
    }

    if (!path_ea || !vm_base) {
        fprintf(stderr, "[SPU] image_open img=0x%08X path=NULL — empty image\n", img_ea);
        fflush(stderr);
        ctx->gpr[3] = 0;
        return 0;
    }

    const char* ps3_path = (const char*)(vm_base + path_ea);
    char host_path[SYS_FS_HOST_PATH_MAX];
    const int translated = sys_fs_translate_path(ps3_path, host_path, sizeof(host_path));

    ps3_vfile* f = translated ? ps3_vfile_open(host_path, "rb") : NULL;
    if (!f) {
        fprintf(stderr, "[SPU] image_open img=0x%08X path='%s' (host: %s) — open failed\n",
                img_ea, ps3_path, host_path);
        fflush(stderr);
        ctx->gpr[3] = 0;
        return 0;
    }

    ps3_vstat fst;
    ps3_vfile_fstat(f, &fst);
    long fsz = (long)fst.size;
    if (fsz <= 0 || fsz > (long)SPU_IMAGE_STAGE_MAX) {
        ps3_vfile_close(f);
        fprintf(stderr, "[SPU] image_open img=0x%08X path='%s' — implausible size %ld\n",
                img_ea, ps3_path, fsz);
        fflush(stderr);
        ctx->gpr[3] = 0;
        return 0;
    }

    static uint32_t s_spu_img_bump = SPU_IMAGE_STAGE_BASE;
    if (s_spu_img_bump + (uint32_t)fsz > SPU_IMAGE_STAGE_END)
        s_spu_img_bump = SPU_IMAGE_STAGE_BASE;
    uint32_t staged_ea = s_spu_img_bump;

    uint8_t* buf = (uint8_t*)malloc((size_t)fsz);
    size_t got = buf ? ps3_vfile_read(buf, (size_t)fsz, f) : 0;
    ps3_vfile_close(f);
    if (got != (size_t)fsz) {
        fprintf(stderr, "[SPU] image_open img=0x%08X path='%s' — short read "
                        "(%zu of %ld)\n", img_ea, ps3_path, got, fsz);
        fflush(stderr);
        free(buf);
        ctx->gpr[3] = 0;
        return 0;
    }
    memcpy(vm_base + staged_ea, buf, (size_t)fsz);
    free(buf);
    s_spu_img_bump += ((uint32_t)fsz + 0x7F) & ~0x7Fu;

    fprintf(stderr, "[SPU] image_open img=0x%08X path='%s' staged %ld bytes at "
                    "0x%08X\n", img_ea, ps3_path, fsz, staged_ea);
    fflush(stderr);

    uint64_t saved_r4 = ctx->gpr[4], saved_r5 = ctx->gpr[5];
    ctx->gpr[4] = staged_ea;
    ctx->gpr[5] = 1;
    int64_t rc = sys_spu_image_import_handler(ctx);
    ctx->gpr[4] = saved_r4;
    ctx->gpr[5] = saved_r5;

    return rc;
}

extern void sys_process_exit(int32_t exitcode);
extern int32_t sys_process_getpid(void);

static int64_t sys_process_exit_handler(ppu_context* ctx)
{
    sys_process_exit((int32_t)ctx->gpr[3]);
    return 0;
}

static int64_t sys_process_getpid_handler(ppu_context* ctx)
{
    ctx->gpr[3] = (uint64_t)(uint32_t)sys_process_getpid();
    return (int64_t)(int32_t)ctx->gpr[3];
}

static int64_t sys_process_get_sdk_version_handler(ppu_context* ctx)
{
    uint32_t version_ea = (uint32_t)ctx->gpr[4];
    if (!version_ea) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_EFAULT;
        return (int64_t)(int32_t)CELL_EFAULT;
    }
    vm_write_be32(version_ea, g_ps3_sdk_version);
    ctx->gpr[3] = 0;
    return 0;
}

static int64_t sys_process_is_spu_lock_line_reservation_address(ppu_context* ctx)
{
    const int64_t rc = lv2_spu_lock_line_classify((uint32_t)ctx->gpr[3], ctx->gpr[4]);

    ctx->gpr[3] = (uint64_t)rc;
    return rc;
}

static int64_t sys_spu_thread_write_snr_handler(ppu_context* ctx)
{
    uint32_t tid = (uint32_t)ctx->gpr[3];
    uint32_t num = (uint32_t)ctx->gpr[4];
    uint32_t val = (uint32_t)ctx->gpr[5];
    if (num > 1) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010002;
        return (int64_t)ctx->gpr[3];
    }
    spu_thread_t* t = spu_find_thread(tid);
    if (!t) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010005;
        return (int64_t)ctx->gpr[3];
    }
    if (t->sctx) {
        spu_channel* ch = &t->sctx->ch_sig_notify[num];
        int or_mode = (t->spu_cfg >> num) & 1;
        if (or_mode && ch->count)
            ch->value |= val;
        else
            spu_channel_write(ch, val);
        extern void spu_ch_wake(spu_context*);
        spu_ch_wake(t->sctx);
        { static int n = 0; if (n < 12) { n++;
            fprintf(stderr, "[SPU] write_snr tid=0x%X snr%u <- 0x%08X (%s)\n",
                    tid, num + 1, val, or_mode ? "OR" : "overwrite");
            fflush(stderr); } }
    }
    ctx->gpr[3] = 0;
    return 0;
}

static int64_t sys_spu_thread_set_spu_cfg_handler(ppu_context* ctx)
{
    uint32_t tid = (uint32_t)ctx->gpr[3];
    uint64_t val = (uint64_t)ctx->gpr[4];
    if (val & ~3ull) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010002;
        return (int64_t)ctx->gpr[3];
    }
    spu_thread_t* t = spu_find_thread(tid);
    if (!t) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010005;
        return (int64_t)ctx->gpr[3];
    }
    t->spu_cfg = (uint32_t)val;
    ctx->gpr[3] = 0;
    return 0;
}

static int64_t sys_spu_thread_get_spu_cfg_handler(ppu_context* ctx)
{
    uint32_t tid = (uint32_t)ctx->gpr[3];
    spu_thread_t* t = spu_find_thread(tid);
    if (!t) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010005;
        return (int64_t)ctx->gpr[3];
    }
    { uint32_t out_ea = (uint32_t)ctx->gpr[4];
      if (out_ea) { vm_write_be32(out_ea, 0); vm_write_be32(out_ea + 4, (uint32_t)t->spu_cfg); } }
    ctx->gpr[4] = (uint64_t)t->spu_cfg;
    ctx->gpr[3] = 0;
    return 0;
}

extern void vm_write64(uint64_t a, uint64_t v);



static int64_t sys_spu_thread_stub(ppu_context* ctx)
{
    (void)ctx;
    ctx->gpr[3] = 0;
    return 0;
}



static int64_t sys_usbd_receive_event_handler(ppu_context* ctx)
{
    uint32_t a1 = (uint32_t)ctx->gpr[4];
    uint32_t a2 = (uint32_t)ctx->gpr[5];
    uint32_t a3 = (uint32_t)ctx->gpr[6];
    if (a1) { vm_write_be32(a1, 0); vm_write_be32(a1 + 4, 0); }
    if (a2) { vm_write_be32(a2, 0); vm_write_be32(a2 + 4, 0); }
    if (a3) { vm_write_be32(a3, 0); vm_write_be32(a3 + 4, 0); }
    static volatile uint32_t s_no_usb_event;
    for (;;) ps3_futex_wait(&s_no_usb_event, 0u, 0);
    return CELL_OK;
}

void lv2_register_all_syscalls(lv2_syscall_table* tbl)
{
    lv2_syscall_table_init(tbl);

    lv2_syscall_register(tbl, SYS_PROCESS_GETPID, sys_process_getpid_handler);
    lv2_syscall_register(tbl, SYS_PROCESS_EXIT3,  sys_process_exit_handler);
    lv2_syscall_register(tbl, SYS_PROCESS_EXIT,   sys_process_exit_handler);
    lv2_syscall_register(tbl, SYS_PROCESS_GET_SDK_VERSION, sys_process_get_sdk_version_handler);
    lv2_syscall_register(tbl, SYS_PROCESS_IS_SPU_LOCK_LINE_RESERVATION_ADDRESS,
                         sys_process_is_spu_lock_line_reservation_address);

    sys_ppu_thread_init(tbl);

    sys_mutex_init(tbl);
    sys_cond_init(tbl);
    sys_semaphore_init(tbl);
    sys_rwlock_init(tbl);

    sys_timer_init(tbl);

    sys_event_init(tbl);

    sys_memory_init(tbl);
    sys_vm_init(tbl);

    sys_fs_init(tbl);

    sys_raw_spu_init(tbl);

    lv2_syscall_register(tbl, SYS_TTY_READ,  sys_tty_read);
    lv2_syscall_register(tbl, SYS_TTY_WRITE, sys_tty_write);
    lv2_syscall_register(tbl, 988, sys_tty_write);

    lv2_syscall_register(tbl, 872, sys_ss_get_open_psid_handler);

    lv2_syscall_register(tbl, 25, sys_process_get_sdk_version_handler);

    lv2_syscall_register(tbl, 540, sys_usbd_receive_event_handler);

    lv2_syscall_register(tbl, 169,                            sys_spu_thread_stub);
    lv2_syscall_register(tbl, SYS_SPU_INITIALIZE,             sys_spu_initialize_handler);
    lv2_syscall_register(tbl, SYS_SPU_IMAGE_OPEN,             sys_spu_image_open_handler);
    lv2_syscall_register(tbl, SYS_SPU_IMAGE_IMPORT,           sys_spu_image_import_handler);
    lv2_syscall_register(tbl, SYS_SPU_IMAGE_CLOSE,            sys_spu_thread_stub);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_GROUP_CREATE,    sys_spu_thread_group_create_handler);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_GROUP_DESTROY,   sys_spu_thread_group_destroy_handler);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_GROUP_START,     sys_spu_thread_group_start_handler);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_GROUP_SUSPEND,   sys_spu_thread_stub);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_GROUP_RESUME,    sys_spu_thread_stub);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_GROUP_YIELD,     sys_spu_thread_stub);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_GROUP_TERMINATE, sys_spu_thread_group_terminate_handler);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_GROUP_JOIN,      sys_spu_thread_group_join_handler);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_INITIALIZE,      sys_spu_thread_initialize_handler);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_SET_ARGUMENT,    sys_spu_thread_set_argument_handler);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_GET_EXIT_STATUS, sys_spu_thread_get_exit_status_handler);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_CONNECT_EVENT,   sys_spu_thread_connect_event_handler);
    { extern void (*g_spu_out_mbox_hook)(uint32_t,uint32_t,int,uint32_t);
      g_spu_out_mbox_hook = spu_out_mbox_deliver; }
    { extern int (*g_spu_user_event_hook)(spu_context*, uint32_t);
      g_spu_user_event_hook = spu_deliver_user_event; }
    lv2_syscall_register(tbl, SYS_SPU_THREAD_DISCONNECT_EVENT,sys_spu_thread_stub);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_GROUP_CONNECT_EVENT, sys_spu_thread_group_connect_event_handler);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_GROUP_DISCONNECT_EVENT, sys_spu_thread_group_disconnect_event_handler);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_WRITE_LS,        sys_spu_thread_write_ls_handler);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_READ_LS,         sys_spu_thread_read_ls_handler);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_WRITE_SNR,       sys_spu_thread_write_snr_handler);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_SET_SPU_CFG,    sys_spu_thread_set_spu_cfg_handler);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_GET_SPU_CFG,    sys_spu_thread_get_spu_cfg_handler);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_WRITE_SPU_MB,  sys_spu_thread_write_spu_mb_handler);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_BIND_QUEUE,      sys_spu_thread_stub);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_UNBIND_QUEUE,    sys_spu_thread_stub);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_GROUP_CONNECT_EVENT_ALL_THREADS, sys_spu_thread_group_connect_event_all_threads_handler);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_GROUP_DISCONNECT_EVENT_ALL_THREADS, sys_spu_thread_group_disconnect_event_all_threads_handler);
}

lv2_syscall_table g_lv2_syscalls;

extern void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));

#define LV2_HLE_BRIDGE(fn_name, handler)                                       static void fn_name(ppu_context* ctx) { (void)handler(ctx); }

LV2_HLE_BRIDGE(hle_sys_spu_image_import, sys_spu_image_import_handler)
LV2_HLE_BRIDGE(hle_sys_spu_image_open,   sys_spu_image_open_handler)

void lv2_init_syscalls(void)
{
    lv2_register_all_syscalls(&g_lv2_syscalls);

    ps3_hle_register_ctx(ps3_compute_nid("sys_spu_image_import"),
                         "sys_spu_image_import", hle_sys_spu_image_import);
    ps3_hle_register_ctx(ps3_compute_nid("sys_spu_image_open"),
                         "sys_spu_image_open",   hle_sys_spu_image_open);
}

int lv2_try_syscall(ppu_context* ctx)
{
    uint32_t num = (uint32_t)ctx->gpr[11];
    if (num >= LV2_SYSCALL_MAX)
        return 0;
    lv2_syscall_fn h = g_lv2_syscalls.handlers[num];
    if (!h || h == lv2_syscall_unimplemented)
        return 0;
    ctx->gpr[3] = (uint64_t)h(ctx);
    return 1;
}
