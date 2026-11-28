#include <setjmp.h>

static __thread jmp_buf s_exit_jmp;
static __thread int     s_exit_armed = 0;

#include <stddef.h>
#include "sys_ppu_thread.h"
#include "../platform/ps3_sync.h"
#include "../platform/win32_compat.h"
#include "ps3emu/ps3_ftrace.h"
#include "ps3emu/ps3_tprof.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "ps3emu/log_first.h"
#include "ps3emu/thread_name.h"

#define PPU_HOST_STACK_BYTES  (256u * 1024u * 1024u)

#ifdef _WIN32
#include <process.h>
#endif

ppu_thread_info       g_ppu_threads[PPU_THREAD_MAX];
vm_stack_alloc        g_vm_stack_alloc;
ppu_thread_entry_fn   g_ppu_thread_entry_trampoline = NULL;

#ifdef _WIN32
static CRITICAL_SECTION s_table_lock;
static int              s_table_lock_init = 0;
#else
static pthread_mutex_t  s_table_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

static void table_lock(void)
{
#ifdef _WIN32
    if (!s_table_lock_init) {
        InitializeCriticalSection(&s_table_lock);
        s_table_lock_init = 1;
    }
    EnterCriticalSection(&s_table_lock);
#else
    pthread_mutex_lock(&s_table_lock);
#endif
}

static void table_unlock(void)
{
#ifdef _WIN32
    LeaveCriticalSection(&s_table_lock);
#else
    pthread_mutex_unlock(&s_table_lock);
#endif
}

static int find_free_slot(void)
{
    static int s_next = 0;
    for (int n = 0; n < PPU_THREAD_MAX; n++) {
        int i = (s_next + n) % PPU_THREAD_MAX;
        if (g_ppu_threads[i].state == PPU_THREAD_STATE_FREE) {
            s_next = (i + 1) % PPU_THREAD_MAX;
            return i;
        }
    }
    return -1;
}

static ppu_thread_info* find_thread(uint64_t thread_id)
{
    if (thread_id == 0 || thread_id > PPU_THREAD_MAX) return NULL;
    ppu_thread_info* t = &g_ppu_threads[thread_id - 1];
    if (t->state == PPU_THREAD_STATE_FREE) return NULL;
    return t;
}

int32_t ppu_thread_priority(uint64_t tid)
{
    if (tid == 0 || tid > PPU_THREAD_MAX) return 1000;
    return __atomic_load_n(&g_ppu_threads[tid - 1].priority, __ATOMIC_RELAXED);
}

int ppu_thread_exists(uint64_t tid)
{
    if (tid == 0 || tid > PPU_THREAD_MAX) return 0;
    const int state = __atomic_load_n(&g_ppu_threads[tid - 1].state, __ATOMIC_ACQUIRE);
    return state != PPU_THREAD_STATE_FREE && state != PPU_THREAD_STATE_FINISHED;
}

void ppu_report_guest_lrs(void)
{
    for (uint64_t tid = 1; tid <= PPU_THREAD_MAX; tid++) {
        if (!ppu_thread_exists(tid)) continue;
        const ppu_context* c = &g_ppu_threads[tid - 1].ctx;
        fprintf(stderr, "  guest tid %-3u %-32s lr=0x%08X r3=0x%08X sp=0x%08X\n", (unsigned)tid, g_ppu_threads[tid - 1].name,
                (uint32_t)c->lr, (uint32_t)c->gpr[3], (uint32_t)c->gpr[1]);
    }
    fflush(stderr);
}

#define PPU_DEFAULT_PRIORITY   1000
#define PRIO_START_TIMEOUT_MS  200

static int prio_start_enabled(void)
{
    static int on = -1;
    if (on < 0) on = getenv("PS3_PRIO_START") ? 1 : 0;
    return on;
}

void ppu_thread_note_block(uint64_t tid)
{
    if (tid == 0 || tid > PPU_THREAD_MAX) return;
    int* blocked = &g_ppu_threads[tid - 1].blocked;
    if (!__atomic_load_n(blocked, __ATOMIC_RELAXED)) {
        __atomic_store_n(blocked, 1, __ATOMIC_RELEASE);
        ps3_futex_wake_all((volatile uint32_t*)blocked);
    }
}

static int32_t creator_priority(uint64_t tid)
{
    const ppu_thread_info* c = find_thread(tid);
    if (!c) return PPU_DEFAULT_PRIORITY;
    return c->priority;
}

static void wait_first_block(const ppu_thread_info* t, uint64_t tid)
{
    const uint64_t deadline = ps3_sync_deadline_us(PRIO_START_TIMEOUT_MS * 1000ull);
    while (!__atomic_load_n(&t->blocked, __ATOMIC_ACQUIRE)) {
        if (!ps3_futex_wait((volatile uint32_t*)&t->blocked, 0u, deadline)) {
            fprintf(stderr, "[SYS] PS3_PRIO_START: tid=%llu \"%s\" (prio %d) did not block within %d ms; its creator goes on\n",
                    (unsigned long long)tid, t->name, t->priority, PRIO_START_TIMEOUT_MS);
            return;
        }
    }
}

static void release_slot(ppu_thread_info* t)
{
    vm_stack_free(&g_vm_stack_alloc, t->stack_addr, t->stack_size);
    t->state = PPU_THREAD_STATE_FREE;
}

static int run_guest_entry(ppu_thread_info* info)
{
    s_exit_armed = 1;
    if (setjmp(s_exit_jmp) != 0) return 1;
    g_ppu_thread_entry_trampoline(&info->ctx);
    s_exit_armed = 0;
    info->exit_status = (int64_t)info->ctx.gpr[3];
    return 0;
}

#ifdef _WIN32
static DWORD WINAPI ppu_host_thread_proc(LPVOID param)
#else
static void* ppu_host_thread_proc(void* param)
#endif
{
    ppu_thread_info* info = (ppu_thread_info*)param;
    ps3_thread_name("P%u %s", (unsigned)info->ctx.thread_id, info->name);

#ifdef _WIN32
    { ULONG g = 256 * 1024; SetThreadStackGuarantee(&g); }
#else
    if (info->priority < 256) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#endif

    ps3_tprof_thread('P', (uint32_t)info->ctx.thread_id);

    fprintf(stderr, "[THREAD %llu] host thread started, entry=0x%08llX hosttid=%lu\n",
            (unsigned long long)info->ctx.thread_id,
            (unsigned long long)info->entry_addr, (unsigned long)GetCurrentThreadId());

    if (g_ppu_thread_entry_trampoline) {
        if (run_guest_entry(info))
            fprintf(stderr, "[THREAD %llu] unwound out of sys_ppu_thread_exit\n",
                    (unsigned long long)info->ctx.thread_id);
        fprintf(stderr, "[THREAD %llu] entry RETURNED (r3=0x%llX) -- thread finished\n",
                (unsigned long long)info->ctx.thread_id,
                (unsigned long long)info->ctx.gpr[3]);

        { extern int ps3_intr_is_handler(unsigned long long);
          extern int ps3_intr_wait(unsigned long long);
          if (ps3_intr_is_handler(info->ctx.thread_id)) {
              fprintf(stderr, "[intr] thread %llu is an interrupt handler -- \n",
                      (unsigned long long)info->ctx.thread_id);
              while (ps3_intr_wait(info->ctx.thread_id))
                  run_guest_entry(info);
          } }
    } else {
        fprintf(stderr, "[THREAD %llu] g_ppu_thread_entry_trampoline is NULL — thread is a no-op!\n",
                (unsigned long long)info->ctx.thread_id);
    }

    ps3_tprof_thread_exit();
    ppu_thread_note_block(info->ctx.thread_id);
    table_lock();
    if (info->joinable) {
        __atomic_store_n(&info->state, PPU_THREAD_STATE_FINISHED, __ATOMIC_RELEASE);
        ps3_futex_wake_all((volatile uint32_t*)&info->state);
    } else {
        release_slot(info);
    }
    table_unlock();
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

#ifdef _WIN32
static int spawn_host_thread(ppu_thread_info* t)
{
    HANDLE h = (HANDLE)_beginthreadex(NULL, PPU_HOST_STACK_BYTES,
                                      (unsigned (__stdcall*)(void*))ppu_host_thread_proc, t,
                                      STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!h) return 0;
    if (t->priority < 256)
        SetThreadPriority(h, THREAD_PRIORITY_ABOVE_NORMAL);
    CloseHandle(h);
    return 1;
}
#else
static int start_detached_host_thread(ppu_thread_info* t, size_t stack_bytes)
{
    pthread_attr_t attr;
    pthread_t host;
    pthread_attr_init(&attr);
    int rc = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (rc == 0 && stack_bytes) rc = pthread_attr_setstacksize(&attr, stack_bytes);
    if (rc == 0) rc = pthread_create(&host, &attr, ppu_host_thread_proc, t);
    pthread_attr_destroy(&attr);
    return rc;
}

static int spawn_host_thread(ppu_thread_info* t)
{
    const int rc = start_detached_host_thread(t, PPU_HOST_STACK_BYTES);
    if (rc == 0) return 1;
    fprintf(stderr, "[SYS] tid=%llu: no %zu MB host stack (%d), "
                    "falling back to the default\n",
            (unsigned long long)t->ctx.thread_id,
            (size_t)(PPU_HOST_STACK_BYTES / (1024 * 1024)), rc);
    return start_detached_host_thread(t, 0) == 0;
}
#endif

uint64_t ppu_thread_register_main(void)
{
    table_lock();
    ppu_thread_info* t = &g_ppu_threads[0];
    if (t->state == PPU_THREAD_STATE_FREE) {
        memset(t, 0, sizeof(*t));
        t->ctx.thread_id = 1;
        t->state    = PPU_THREAD_STATE_RUNNING;
        t->joinable = 0;
        t->priority = 1001;
        strncpy(t->name, "main", sizeof(t->name) - 1);
        ps3_ft_name(1, "main");
        ps3_tprof_thread('P', 1);
        ps3_thread_name("P1 main");
    }
    table_unlock();
    return 1;
}

ppu_context* ppu_thread_adopt(const char* name, uint32_t stack_size, int32_t priority)
{
    table_lock();
    const int slot = find_free_slot();
    const uint32_t stack_addr = slot < 0 ? 0 : vm_stack_allocate(&g_vm_stack_alloc, stack_size);
    if (!stack_addr) {
        table_unlock();
        return NULL;
    }
    ppu_thread_info* t = &g_ppu_threads[slot];
    memset(t, 0, sizeof(*t));
    ppu_context_init(&t->ctx);
    ppu_set_stack(&t->ctx, (uint64_t)stack_addr, (uint64_t)stack_size);
    t->ctx.thread_id = (uint64_t)(slot + 1);
    t->state = PPU_THREAD_STATE_RUNNING;
    t->priority = priority;
    t->stack_addr = stack_addr;
    t->stack_size = stack_size;
    strncpy(t->name, name, sizeof(t->name) - 1);
    table_unlock();
    ps3_ft_name((uint32_t)slot + 1u, t->name);
    ps3_thread_name("P%u %s", (unsigned)slot + 1u, t->name);
    ps3_tprof_thread('P', (uint32_t)slot + 1u);
    { extern PPU_THREAD_LOCAL ppu_context* g_active_ctx; g_active_ctx = &t->ctx; }
    return &t->ctx;
}
uint64_t ppu_thread_register_main_priority(int32_t priority)
{
    uint64_t id = ppu_thread_register_main();
    table_lock();
    g_ppu_threads[0].priority = (priority >= 0 && priority <= 3071) ? priority : 1001;
    table_unlock();
    return id;
}

void ppu_thread_set_main_stack(uint32_t stack_base, uint32_t stack_size)
{
    table_lock();
    ppu_thread_info* t = &g_ppu_threads[0];
    if (t->state != PPU_THREAD_STATE_FREE) {
        t->stack_addr = stack_base;
        t->stack_size = stack_size;
    }
    table_unlock();
}

int64_t sys_ppu_thread_create(ppu_context* ctx)
{
    uint32_t tid_out_addr = LV2_ARG_PTR(ctx, 0);
    uint64_t entry        = LV2_ARG_U64(ctx, 1);
    uint64_t arg          = LV2_ARG_U64(ctx, 2);
    int32_t  priority     = LV2_ARG_S32(ctx, 3);
    uint32_t stack_size   = LV2_ARG_U32(ctx, 4);
    uint64_t flags        = LV2_ARG_U64(ctx, 5);
    uint32_t name_addr    = LV2_ARG_PTR(ctx, 6);

    if (stack_size == 0) stack_size = VM_PPU_STACK_SIZE;
    if (stack_size < 0x4000) stack_size = 0x4000;

    table_lock();

    int slot = find_free_slot();
    if (slot < 0) {
        table_unlock();
        return (int64_t)(int32_t)CELL_EAGAIN;
    }

    ppu_thread_info* t = &g_ppu_threads[slot];
    memset(t, 0, sizeof(*t));

    uint32_t stack_addr = vm_stack_allocate(&g_vm_stack_alloc, stack_size);
    if (stack_addr == 0) {
        table_unlock();
        return (int64_t)(int32_t)CELL_ENOMEM;
    }

    ppu_context_init(&t->ctx);
    t->ctx.cia = entry;
    t->ctx.gpr[3] = arg;
    ppu_set_stack(&t->ctx, (uint64_t)stack_addr, (uint64_t)stack_size);
    t->ctx.gpr[2] = ctx->gpr[2];

    uint64_t thread_id = (uint64_t)(slot + 1);
    t->ctx.thread_id = thread_id;

    t->state      = PPU_THREAD_STATE_RUNNING;
    t->priority   = priority;
    t->joinable   = (flags & 1u) ? 1 : 0;
    t->stack_addr = stack_addr;
    t->stack_size = stack_size;
    t->entry_addr = entry;

    if (name_addr != 0) {
        const char* name = (const char*)vm_to_host(name_addr);
        strncpy(t->name, name, sizeof(t->name) - 1);
        t->name[sizeof(t->name) - 1] = '\0';
        ps3_ft_name((uint32_t)(t - g_ppu_threads) + 1u, t->name);
    }

    if (tid_out_addr != 0) {
        uint64_t* out = (uint64_t*)vm_to_host(tid_out_addr);
        uint64_t be_id = thread_id;
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
        be_id = ((be_id >> 56) & 0xFF) |
                ((be_id >> 40) & 0xFF00) |
                ((be_id >> 24) & 0xFF0000) |
                ((be_id >>  8) & 0xFF000000ULL) |
                ((be_id <<  8) & 0xFF00000000ULL) |
                ((be_id << 24) & 0xFF0000000000ULL) |
                ((be_id << 40) & 0xFF000000000000ULL) |
                ((be_id << 56) & 0xFF00000000000000ULL);
#endif
        *out = be_id;
    }

    fprintf(stderr, "[SYS] sys_ppu_thread_create tid=%llu name=\"%s\" entry=0x%08llX arg=0x%llX stack=0x%X prio=%d flags=0x%llX\n",
            (unsigned long long)thread_id, t->name, (unsigned long long)entry, (unsigned long long)arg,
            stack_size, priority, (unsigned long long)flags);
    if (!spawn_host_thread(t)) {
        release_slot(t);
        table_unlock();
        return (int64_t)(int32_t)CELL_EAGAIN;
    }

    const int preempts = prio_start_enabled() && priority < creator_priority(ctx->thread_id);
    table_unlock();
    if (preempts) wait_first_block(t, thread_id);
    return CELL_OK;
}

int64_t sys_ppu_thread_exit(ppu_context* ctx)
{
    uint64_t status = LV2_ARG_U64(ctx, 0);
    uint64_t tid = ctx->thread_id;
    fprintf(stderr, "[SYS] sys_ppu_thread_exit(tid=%llu status=%llu)\n",
            (unsigned long long)tid, (unsigned long long)status);

    table_lock();
    ppu_thread_info* t = find_thread(tid);
    if (t) {
        t->exit_status = (int64_t)status;
        if (t == &g_ppu_threads[0])
            t->state = PPU_THREAD_STATE_FINISHED;
    }
    table_unlock();

    if (s_exit_armed) { s_exit_armed = 0; longjmp(s_exit_jmp, 1); }
    return CELL_OK;
}

int64_t sys_ppu_thread_join(ppu_context* ctx)
{
    uint64_t tid          = LV2_ARG_U64(ctx, 0);
    uint32_t status_addr  = LV2_ARG_PTR(ctx, 1);
    { static int n=0; if(PS3_LOG_FIRST(n, 30)) fprintf(stderr,"[WAIT] ppu_thread_join(tid=%llu)\n", (unsigned long long)tid); }
    ppu_thread_note_block(ctx->thread_id);

    if (tid == ctx->thread_id)
        return (int64_t)(int32_t)CELL_EDEADLK;

    table_lock();
    ppu_thread_info* t = find_thread(tid);
    if (!t) {
        table_unlock();
        return (int64_t)(int32_t)CELL_ESRCH;
    }
    if (!t->joinable || t->joining) {
        table_unlock();
        return (int64_t)(int32_t)CELL_EINVAL;
    }
    t->joining = 1;
    for (int state; (state = t->state) != PPU_THREAD_STATE_FINISHED;) {
        table_unlock();
        ps3_futex_wait((volatile uint32_t*)&t->state, (uint32_t)state, 0);
        table_lock();
    }
    const int64_t es = t->exit_status;
    release_slot(t);
    table_unlock();

    if (status_addr != 0) {
        int64_t* out = (int64_t*)vm_to_host(status_addr);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
        uint64_t u = (uint64_t)es;
        u = ((u >> 56) & 0xFF) |
            ((u >> 40) & 0xFF00) |
            ((u >> 24) & 0xFF0000) |
            ((u >>  8) & 0xFF000000ULL) |
            ((u <<  8) & 0xFF00000000ULL) |
            ((u << 24) & 0xFF0000000000ULL) |
            ((u << 40) & 0xFF000000000000ULL) |
            ((u << 56) & 0xFF00000000000000ULL);
        *out = (int64_t)u;
#else
        *out = es;
#endif
    }

    return CELL_OK;
}

int64_t sys_ppu_thread_detach(ppu_context* ctx)
{
    uint64_t tid = LV2_ARG_U64(ctx, 0);

    table_lock();
    ppu_thread_info* t = find_thread(tid);
    if (!t) {
        table_unlock();
        return (int64_t)(int32_t)CELL_ESRCH;
    }

    if (!t->joinable) {
        table_unlock();
        return (int64_t)(int32_t)CELL_EINVAL;
    }
    if (t->joining) {
        table_unlock();
        return (int64_t)(int32_t)CELL_EBUSY;
    }

    if (t->state == PPU_THREAD_STATE_FINISHED) {
        release_slot(t);
    } else {
        t->state = PPU_THREAD_STATE_DETACHED;
        t->joinable = 0;
    }

    table_unlock();
    return CELL_OK;
}

int64_t sys_ppu_thread_yield(ppu_context* ctx)
{
    (void)ctx;
#ifdef _WIN32
    SwitchToThread();
#else
    sched_yield();
#endif
    return CELL_OK;
}

int64_t sys_ppu_thread_get_priority(ppu_context* ctx)
{
    uint64_t tid       = LV2_ARG_U64(ctx, 0);
    uint32_t prio_addr = LV2_ARG_PTR(ctx, 1);

    table_lock();
    ppu_thread_info* t = find_thread(tid);
    if (!t && tid >= 1 && tid <= PPU_THREAD_MAX && g_ppu_threads[tid - 1].entry_addr != 0) {
        table_unlock();
        return (int64_t)(int32_t)CELL_ESRCH;
    }
    if (!t) {
        table_unlock();
        if (prio_addr != 0) {
            int32_t prio = PPU_DEFAULT_PRIORITY;
            int32_t* out = (int32_t*)vm_to_host(prio_addr);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
            uint32_t u = (uint32_t)prio;
            u = ((u >> 24) & 0xFF) | ((u >> 8) & 0xFF00) |
                ((u <<  8) & 0xFF0000) | ((u << 24) & 0xFF000000u);
            *out = (int32_t)u;
#else
            *out = prio;
#endif
        }
        return CELL_OK;
    }

    if (prio_addr != 0) {
        int32_t prio = t->priority;
        int32_t* out = (int32_t*)vm_to_host(prio_addr);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
        uint32_t u = (uint32_t)prio;
        u = ((u >> 24) & 0xFF) | ((u >> 8) & 0xFF00) |
            ((u <<  8) & 0xFF0000) | ((u << 24) & 0xFF000000u);
        *out = (int32_t)u;
#else
        *out = prio;
#endif
    }

    table_unlock();
    return CELL_OK;
}

int64_t sys_ppu_thread_set_priority(ppu_context* ctx)
{
    uint64_t tid      = LV2_ARG_U64(ctx, 0);
    int32_t  priority = LV2_ARG_S32(ctx, 1);

    table_lock();
    ppu_thread_info* t = find_thread(tid);
    if (!t) {
        table_unlock();
        return (int64_t)(int32_t)CELL_ESRCH;
    }

    t->priority = priority;
    table_unlock();
    return CELL_OK;
}

int64_t sys_ppu_thread_rename(ppu_context* ctx)
{
    uint64_t tid       = LV2_ARG_U64(ctx, 0);
    uint32_t name_addr = LV2_ARG_PTR(ctx, 1);

    table_lock();
    ppu_thread_info* t = find_thread(tid);
    if (!t) {
        table_unlock();
        return (int64_t)(int32_t)CELL_ESRCH;
    }

    if (name_addr != 0) {
        const char* name = (const char*)vm_to_host(name_addr);
        strncpy(t->name, name, sizeof(t->name) - 1);
        t->name[sizeof(t->name) - 1] = '\0';
        ps3_ft_name((uint32_t)(t - g_ppu_threads) + 1u, t->name);
    }

    table_unlock();
    return CELL_OK;
}

int64_t sys_ppu_thread_get_join_state(ppu_context* ctx)
{
    uint32_t out_addr = LV2_ARG_PTR(ctx, 0);
    uint64_t tid = ctx->thread_id;

    table_lock();
    ppu_thread_info* t = find_thread(tid);
    int32_t joinable = t ? t->joinable : 0;
    table_unlock();

    if (out_addr != 0) {
        int32_t* out = (int32_t*)vm_to_host(out_addr);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
        uint32_t u = (uint32_t)joinable;
        u = ((u >> 24) & 0xFF) | ((u >> 8) & 0xFF00) |
            ((u <<  8) & 0xFF0000) | ((u << 24) & 0xFF000000u);
        *out = (int32_t)u;
#else
        *out = joinable;
#endif
    }

    return CELL_OK;
}

uint32_t g_ppu_main_stack_addr = 0;
uint32_t g_ppu_main_stack_size = 0;

static void main_stack_info(uint32_t* addr, uint32_t* size)
{
    if (g_ppu_main_stack_size) {
        *addr = g_ppu_main_stack_addr;
        *size = g_ppu_main_stack_size;
    } else {
        *addr = VM_STACK_BASE;
        *size = VM_PPU_STACK_SIZE;
    }
}

int64_t sys_ppu_thread_get_stack_information(ppu_context* ctx)
{
    uint32_t out_addr = LV2_ARG_PTR(ctx, 0);
    uint64_t tid = ctx->thread_id;
    uint32_t sa, ss;

    table_lock();
    ppu_thread_info* t = find_thread(tid);
    if (t && t->stack_size) {
        sa = t->stack_addr;
        ss = t->stack_size;
    } else {
        main_stack_info(&sa, &ss);
    }
    table_unlock();

    if (out_addr != 0) {
        uint32_t* out = (uint32_t*)vm_to_host(out_addr);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
        sa = ((sa >> 24) & 0xFF) | ((sa >> 8) & 0xFF00) |
             ((sa <<  8) & 0xFF0000) | ((sa << 24) & 0xFF000000u);
        ss = ((ss >> 24) & 0xFF) | ((ss >> 8) & 0xFF00) |
             ((ss <<  8) & 0xFF0000) | ((ss << 24) & 0xFF000000u);
#endif
        out[0] = sa;
        out[1] = ss;
    }
    return CELL_OK;
}

void sys_ppu_thread_init(lv2_syscall_table* tbl)
{
    vm_stack_alloc_init(&g_vm_stack_alloc);

    memset(g_ppu_threads, 0, sizeof(g_ppu_threads));

#ifdef _WIN32
    if (!s_table_lock_init) {
        InitializeCriticalSection(&s_table_lock);
        s_table_lock_init = 1;
    }
#endif

    lv2_syscall_register(tbl, SYS_PPU_THREAD_CREATE,              sys_ppu_thread_create);
    lv2_syscall_register(tbl, SYS_PPU_THREAD_EXIT,                sys_ppu_thread_exit);
    lv2_syscall_register(tbl, SYS_PPU_THREAD_YIELD,               sys_ppu_thread_yield);
    lv2_syscall_register(tbl, SYS_PPU_THREAD_JOIN,                sys_ppu_thread_join);
    lv2_syscall_register(tbl, SYS_PPU_THREAD_DETACH,              sys_ppu_thread_detach);
    lv2_syscall_register(tbl, SYS_PPU_THREAD_GET_JOIN_STATE,      sys_ppu_thread_get_join_state);
    lv2_syscall_register(tbl, SYS_PPU_THREAD_SET_PRIORITY,        sys_ppu_thread_set_priority);
    lv2_syscall_register(tbl, SYS_PPU_THREAD_GET_PRIORITY,        sys_ppu_thread_get_priority);
    lv2_syscall_register(tbl, SYS_PPU_THREAD_GET_STACK_INFORMATION, sys_ppu_thread_get_stack_information);
    lv2_syscall_register(tbl, SYS_PPU_THREAD_RENAME,              sys_ppu_thread_rename);
}
