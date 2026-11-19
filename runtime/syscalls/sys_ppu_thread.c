/*
 * ps3recomp - PPU thread management syscalls (implementation)
 */

#include <setjmp.h>

/* sys_ppu_thread_exit never returns on hardware. Returning CELL_OK to the
 * guest lets it run on past the call: Twisted Metal has a thread that exits
 * from inside a loop and so called exit 1,435 times in one run, staying alive
 * and holding whatever it held. Arm a jump in the thread proc and unwind to it
 * instead. PS3_NO_THREAD_EXIT_UNWIND=1 restores the old behaviour. */
/* MinGW GCC ignores __declspec(thread) ("'thread' attribute directive
 * ignored"), which would make these two process-wide: MSVC only. */
#if defined(_MSC_VER)
#  define PPU_TLS __declspec(thread)
#else
#  define PPU_TLS __thread
#endif
static PPU_TLS jmp_buf s_exit_jmp;
static PPU_TLS int     s_exit_armed = 0;

#include <stddef.h>
#include "sys_ppu_thread.h"
#include "../platform/ps3_sync.h"
#include "../platform/win32_compat.h"   /* GetCurrentThreadId on POSIX */
#include "ps3emu/ps3_ftrace.h"           /* PS3_FTRACE: thread names for the report */
#include "ps3emu/ps3_tprof.h"            /* PS3_TPROF: in-process thread samples */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>   /* getenv (else return value truncated to int on x64) */
#include "ps3emu/log_first.h"
#include "ps3emu/thread_name.h"
#ifndef _WIN32
#include <errno.h>
#endif

/* Host stack reserved per guest thread, for pthread_attr_setstacksize. The
 * same 256 MB the Win32 branch hands _beginthreadex a few hundred lines
 * down; keep the two in step so a stack-depth bug reproduces on both. */
#define PPU_HOST_STACK_BYTES  (256u * 1024u * 1024u)

#ifdef _WIN32
#include <process.h>   /* _beginthreadex: CRT-aware thread creation (raw CreateThread
                        * leaves per-thread CRT state uninit -> buffered fread() silently
                        * returns 0 on those threads). */
#endif

/* ---------------------------------------------------------------------------
 * Globals
 * -----------------------------------------------------------------------*/
ppu_thread_info       g_ppu_threads[PPU_THREAD_MAX];
vm_stack_alloc        g_vm_stack_alloc;
ppu_thread_entry_fn   g_ppu_thread_entry_trampoline = NULL;

/* Simple mutex for thread table access */
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

/* Find a free slot. Returns index or -1. Must be called under lock.
 * The thread id is the slot + 1. A title that polls sys_ppu_thread_get_priority(id) until it returns ESRCH (to see a
 * worker finish) only gets its answer if the next thread does not take that slot, and so that id, at once. So the
 * search continues after the slot used last (next-fit): an id comes back only after the other slots were used. */
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

/* Find thread by ID. The ID is the index + 1 (0 is invalid). */
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

/* ---------------------------------------------------------------------------
 * Priority at thread start (PS3_PRIO_START=1, off by default)
 *
 * On lv2 a thread created with a higher priority (a lower number) than its creator preempts the creator and runs
 * until it blocks. SDK libraries rely on it: the SurMixer thread (prio 400, typically created by a prio 1000 thread)
 * creates the event queue whose key its creator passes to cellAudioSetNotifyEventQueue right after the create
 * returns, with no synchronisation of their own. Host threads run side by side, so the creator can read the key
 * before it is written, and the mixer is never notified. With PS3_PRIO_START=1 sys_ppu_thread_create waits until
 * such a thread reaches a blocking call (ppu_thread_note_block), for at most PRIO_START_TIMEOUT_MS so that a thread
 * which blocks somewhere not reported, or never blocks, cannot hold its creator up for long.
 *
 * Off by default: it changes thread interleaving for every title, and the regression gate's goldens are recorded
 * without it.
 * -----------------------------------------------------------------------*/
#define PPU_DEFAULT_PRIORITY   1000   /* what sys_ppu_thread_get_priority reports for a thread not in the table */
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

/* Priority the creator runs at. Must be called under the table lock. Main
 * registration now initializes a real priority; zero is the highest valid
 * priority and must not be treated as an unset/default sentinel. */
static int32_t creator_priority(uint64_t tid)
{
    const ppu_thread_info* c = find_thread(tid);
    if (!c) return PPU_DEFAULT_PRIORITY;
    return c->priority;
}

/* Called without the table lock, right after the creator made `t`. The acquire load pairs with the release store in
 * ppu_thread_note_block, so whatever `t` wrote before it blocked (the event queue key above) is visible here. */
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

/* ---------------------------------------------------------------------------
 * Host thread entry point
 * -----------------------------------------------------------------------*/
#ifdef _WIN32
static DWORD WINAPI ppu_host_thread_proc(LPVOID param)
#else
static void* ppu_host_thread_proc(void* param)
#endif
{
    ppu_thread_info* info = (ppu_thread_info*)param;
    ps3_thread_name("ppu %s", info->name);

#ifdef _WIN32
    /* Reserve stack for the STACK_OVERFLOW handler, same as main() does for the
     * main thread. Deep recompiled call chains DO overflow even a 256 MB stack
     * (a lifter bug that turns a tail call into recursion is unbounded), and
     * without the guarantee the handler faults while reporting -- the process
     * then dies silently with an access violation INSIDE the handler and the
     * backtrace that would name the recursing function is lost. */
    { ULONG g = 256 * 1024; SetThreadStackGuarantee(&g); }
#endif

    ps3_tprof_thread('P', (uint32_t)info->ctx.thread_id);

    fprintf(stderr, "[THREAD %llu] host thread started, entry=0x%08llX hosttid=%lu\n",
            (unsigned long long)info->ctx.thread_id,
            (unsigned long long)info->entry_addr, (unsigned long)GetCurrentThreadId());

    /* Invoke the recompiled entry point */
    if (g_ppu_thread_entry_trampoline) {
        s_exit_armed = 1;
        if (setjmp(s_exit_jmp) == 0)
            g_ppu_thread_entry_trampoline(&info->ctx);
        else
            fprintf(stderr, "[THREAD %llu] unwound out of sys_ppu_thread_exit\n",
                    (unsigned long long)info->ctx.thread_id);
        s_exit_armed = 0;
        fprintf(stderr, "[THREAD %llu] entry RETURNED (r3=0x%llX) -- thread finished\n",
                (unsigned long long)info->ctx.thread_id,
                (unsigned long long)info->ctx.gpr[3]);

        /* An established interrupt handler does not finish when its body
         * returns. lv2 re-enters such a thread at its ENTRY on the next
         * interrupt -- ps1_netemu's two SPU handlers both end in `blr` right
         * after their `sc 88` (eoi), so a return IS the end of one pass, not
         * the end of the thread. Wait for the next interrupt and run it again;
         * without this each handler ran once and the SPUs were never answered,
         * which is why SPUs 0-3 parked on SPU_RdInMbox and SPU 4 exited. */
        { extern int ps3_intr_is_handler(unsigned long long);
          extern int ps3_intr_wait(unsigned long long);
          if (ps3_intr_is_handler(info->ctx.thread_id)) {
              fprintf(stderr, "[intr] thread %llu is an interrupt handler -- \n",
                      (unsigned long long)info->ctx.thread_id);
              while (ps3_intr_wait(info->ctx.thread_id)) {
                  s_exit_armed = 1;
                  if (setjmp(s_exit_jmp) == 0)
                      g_ppu_thread_entry_trampoline(&info->ctx);
                  s_exit_armed = 0;
              }
          } }
    } else {
        fprintf(stderr, "[THREAD %llu] g_ppu_thread_entry_trampoline is NULL — thread is a no-op!\n",
                (unsigned long long)info->ctx.thread_id);
    }

    ps3_tprof_thread_exit();
    ppu_thread_note_block(info->ctx.thread_id);
    table_lock();
    info->exit_status = (int64_t)info->ctx.gpr[3];

    /* A thread nobody can join (created without SYS_PPU_THREAD_CREATE_JOINABLE, or detached)
     * frees its slot here, its last use of `info`. The sync objects go first, under the table lock: once the slot is
     * FREE a new sys_ppu_thread_create may reuse it and create its own. Joinable threads stay FINISHED for the join. */
    if (!info->joinable) {
#ifdef _WIN32
        if (info->finish_event) CloseHandle(info->finish_event);
        if (info->host_thread) CloseHandle(info->host_thread);
        info->finish_event = NULL;
        info->host_thread = NULL;
#else
        pthread_mutex_destroy(&info->finish_mutex);
        pthread_cond_destroy(&info->finish_cond);
#endif
        info->state = PPU_THREAD_STATE_FREE;
        table_unlock();
#ifdef _WIN32
        return 0;
#else
        return NULL;
#endif
    }
    info->state = PPU_THREAD_STATE_FINISHED;
    table_unlock();

    /* Signal anyone waiting for join */
#ifdef _WIN32
    SetEvent(info->finish_event);
    return 0;
#else
    pthread_mutex_lock(&info->finish_mutex);
    info->finished = 1;
    pthread_cond_signal(&info->finish_cond);
    pthread_mutex_unlock(&info->finish_mutex);
    return NULL;
#endif
}

/* ---------------------------------------------------------------------------
 * Main-thread registration. The main guest thread used to run with
 * thread_id 0 -- an id every owner-tracking primitive treated specially:
 * sys_lwmutex mapped 0 to owner id 1 (COLLIDING with the first created
 * thread, so main and that thread mutually satisfied each other's recursive
 * re-lock check and both "owned" the lock -- LBP: main + "bringup" emitted
 * GCM concurrently and fences vanished), and sys_mutex uses owner_tid==0 as
 * its FREE marker (a mutex held by main read as free). Claim slot 0 (id 1)
 * for main before any sys_ppu_thread_create so every thread has a unique
 * nonzero id and the special cases die.
 * -----------------------------------------------------------------------*/
uint64_t ppu_thread_register_main(void)
{
    table_lock();
    ppu_thread_info* t = &g_ppu_threads[0];
    if (t->state == PPU_THREAD_STATE_FREE) {
        memset(t, 0, sizeof(*t));
        t->ctx.thread_id = 1;
        t->state    = PPU_THREAD_STATE_RUNNING;
        t->joinable = 0;                    /* nobody joins the main thread */
        t->priority = 1001;                 /* default PS3 process priority */
        strncpy(t->name, "main", sizeof(t->name) - 1);
        ps3_ft_name(1, "main");
        ps3_tprof_thread('P', 1);
        ps3_thread_name("ppu main");      /* registered from the boot thread itself */
#ifdef _WIN32
        t->finish_event = CreateEventA(NULL, TRUE, FALSE, NULL);
#else
        pthread_mutex_init(&t->finish_mutex, NULL);
        pthread_cond_init(&t->finish_cond, NULL);
#endif
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
#ifdef _WIN32
    t->host_tid = GetCurrentThreadId();
    t->finish_event = CreateEventA(NULL, TRUE, FALSE, NULL);
#else
    t->host_thread = pthread_self();
    pthread_mutex_init(&t->finish_mutex, NULL);
    pthread_cond_init(&t->finish_cond, NULL);
#endif
    table_unlock();
    ps3_ft_name((uint32_t)slot + 1u, t->name);
    ps3_thread_name("ppu %s", t->name);
    ps3_tprof_thread('P', (uint32_t)slot + 1u);
    { extern PPU_THREAD_LOCAL ppu_context* g_active_ctx; g_active_ctx = &t->ctx; }
    return &t->ctx;
}
/* The ELF entry path may follow a host bootstrap which already claimed slot
 * zero. Apply its process priority in either case, before guest dispatch. The
 * legacy no-argument registration API remains available for other harnesses. */
uint64_t ppu_thread_register_main_priority(int32_t priority)
{
    uint64_t id = ppu_thread_register_main();
    table_lock();
    g_ppu_threads[0].priority = (priority >= 0 && priority <= 3071) ? priority : 1001;
    table_unlock();
    return id;
}

/* Fill in the main thread's stack fields after the boot harness has chosen
 * where the stack lives. register_main() memset()s the whole struct, so
 * without this sys_ppu_thread_get_stack_information(49) finds tid 1 in the
 * table with stack_addr/stack_size == 0 and reports a {0,0} stack -- a guest
 * that dumps its own stack (RPCS3's dump_stack test) then calls
 * cellFsWrite(fd, NULL, 0) and produces an empty file while the real kernel
 * reports the actual region. RPCS3's main stack: base 0xD0010000, 1 MB. */
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

/* ---------------------------------------------------------------------------
 * sys_ppu_thread_create
 *
 * r3 = pointer to receive thread ID (u64*)
 * r4 = entry point address
 * r5 = argument (passed in new thread's r3)
 * r6 = priority (s32)
 * r7 = stack size
 * r8 = flags
 * r9 = thread name pointer
 * -----------------------------------------------------------------------*/

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
    if (stack_size < 0x4000) stack_size = 0x4000; /* 16 KB minimum */

    table_lock();

    int slot = find_free_slot();
    if (slot < 0) {
        table_unlock();
        return (int64_t)(int32_t)CELL_EAGAIN;
    }

    ppu_thread_info* t = &g_ppu_threads[slot];
    memset(t, 0, sizeof(*t));

    /* Allocate guest stack */
    uint32_t stack_addr = vm_stack_allocate(&g_vm_stack_alloc, stack_size);
    if (stack_addr == 0) {
        table_unlock();
        return (int64_t)(int32_t)CELL_ENOMEM;
    }

    /* Set up the PPU context for the new thread */
    ppu_context_init(&t->ctx);
    t->ctx.cia = entry;
    t->ctx.gpr[3] = arg;
    ppu_set_stack(&t->ctx, (uint64_t)stack_addr, (uint64_t)stack_size);
    /* Copy TOC from creating thread */
    t->ctx.gpr[2] = ctx->gpr[2];

    uint64_t thread_id = (uint64_t)(slot + 1);
    t->ctx.thread_id = thread_id;

    t->state      = PPU_THREAD_STATE_RUNNING;
    t->priority   = priority;
    /* SYS_PPU_THREAD_CREATE_JOINABLE is bit 0 of the flags. If every thread were joinable, a thread created without
     * the flag (nobody will ever join it) would stay FINISHED after its exit and keep its slot, and a title that
     * starts a short-lived worker per task would see CELL_EAGAIN once 63 of them had run. */
    t->joinable   = (flags & 1u) ? 1 : 0;
    t->stack_addr = stack_addr;
    t->stack_size = stack_size;
    t->entry_addr = entry;

    /* Copy thread name if provided */
    if (name_addr != 0) {
        const char* name = (const char*)vm_to_host(name_addr);
        strncpy(t->name, name, sizeof(t->name) - 1);
        t->name[sizeof(t->name) - 1] = '\0';
        ps3_ft_name((uint32_t)(t - g_ppu_threads) + 1u, t->name);
    }

    /* Create synchronization for join */
#ifdef _WIN32
    t->finish_event = CreateEventA(NULL, TRUE, FALSE, NULL);
#else
    pthread_mutex_init(&t->finish_mutex, NULL);
    pthread_cond_init(&t->finish_cond, NULL);
    t->finished = 0;
#endif

    /* Write thread ID to output pointer */
    if (tid_out_addr != 0) {
        uint64_t* out = (uint64_t*)vm_to_host(tid_out_addr);
        /* Store as big-endian u64 */
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
    /* Create the host thread. Give it a large RESERVED stack: each recompiled
     * guest call is a real host call, so deep guest call chains nest deeply on
     * the host stack and overflow the 1 MB default. Reserve 256 MB (committed
     * lazily by the OS via STACK_SIZE_PARAM_IS_A_RESERVATION). */
#ifdef _WIN32
    t->host_thread = (HANDLE)_beginthreadex(NULL, 256u * 1024 * 1024,
                                  (unsigned (__stdcall*)(void*))ppu_host_thread_proc, t,
                                  STACK_SIZE_PARAM_IS_A_RESERVATION, (unsigned*)&t->host_tid);
    if (t->host_thread == NULL) {
        t->state = PPU_THREAD_STATE_FREE;
        CloseHandle(t->finish_event);
        table_unlock();
        return (int64_t)(int32_t)CELL_EAGAIN;
    }
    /* The guest's top priorities (0 is highest, 3071 lowest) are its audio and
     * I/O pollers. At normal host priority they wake late under a busy frame:
     * The Simpsons Arcade Game's music thread (prio 0) polls the audio read
     * index every 2.5 ms and misses a 5.3 ms block whenever it is delayed, and
     * each miss is an audible gap. A mild boost, not TIME_CRITICAL: these
     * threads sleep between polls.
     * ponytail: two host levels only; map the full range if a title needs it. */
    if (priority < 256)
        SetThreadPriority(t->host_thread, THREAD_PRIORITY_ABOVE_NORMAL);
#else
    /* Same reservation, for the same reason. This is the HOST stack the
     * recompiled C frames run on, not the guest stack (allocated above out of
     * guest VM), and the default is nowhere near enough: 512 KB on Darwin,
     * where a recompiled call chain that spills a whole ppu_context per frame
     * overflows in a few hundred frames. Like the Win32 branch it is a
     * reservation, not a commitment -- the pages are mapped lazily. */
    int rc;
    {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        if (pthread_attr_setstacksize(&attr, PPU_HOST_STACK_BYTES) == 0) {
            rc = pthread_create(&t->host_thread, &attr, ppu_host_thread_proc, t);
        } else {
            rc = EINVAL;
        }
        pthread_attr_destroy(&attr);
        /* A host that will not hand out that much address space (a strict
         * overcommit policy, a low RLIMIT_AS) gets the thread anyway on the
         * default stack. A shallow guest thread runs fine there, and a deep
         * one crashing beats not starting at all. */
        if (rc != 0) {
            fprintf(stderr, "[SYS] tid=%llu: no %zu MB host stack (%d), "
                            "falling back to the default\n",
                    (unsigned long long)thread_id,
                    (size_t)(PPU_HOST_STACK_BYTES / (1024 * 1024)), rc);
            rc = pthread_create(&t->host_thread, NULL, ppu_host_thread_proc, t);
        }
    }
    if (rc != 0) {
        t->state = PPU_THREAD_STATE_FREE;
        pthread_mutex_destroy(&t->finish_mutex);
        pthread_cond_destroy(&t->finish_cond);
        table_unlock();
        return (int64_t)(int32_t)CELL_EAGAIN;
    }
    if (!t->joinable) pthread_detach(t->host_thread);   /* nobody pthread_joins it */
#endif

    const int preempts = prio_start_enabled() && priority < creator_priority(ctx->thread_id);
    table_unlock();
    if (preempts) wait_first_block(t, thread_id);
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_ppu_thread_exit
 *
 * r3 = exit status
 * -----------------------------------------------------------------------*/
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
        /* A non-joinable host thread frees its slot in ppu_host_thread_proc, right after the
         * unwind below (freeing it here let a new thread take the slot while this one still ran its epilogue).
         * The main thread (slot 0) has no such epilogue and is marked FINISHED as before. */
        if (t->joinable || t == &g_ppu_threads[0])
            t->state = PPU_THREAD_STATE_FINISHED;

#ifdef _WIN32
        SetEvent(t->finish_event);
#else
        pthread_mutex_lock(&t->finish_mutex);
        t->finished = 1;
        pthread_cond_signal(&t->finish_cond);
        pthread_mutex_unlock(&t->finish_mutex);
#endif
    }
    table_unlock();

    /* Hardware never returns from this. Unwind to the thread proc so the guest
     * cannot keep running past its own exit. */
    {
        static int allow = -1;
        if (allow < 0) allow = getenv("PS3_NO_THREAD_EXIT_UNWIND") ? 0 : 1;
        if (allow && s_exit_armed) { s_exit_armed = 0; longjmp(s_exit_jmp, 1); }
    }
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_ppu_thread_join
 *
 * r3 = thread_id
 * r4 = pointer to receive exit status (s64*)
 * -----------------------------------------------------------------------*/
int64_t sys_ppu_thread_join(ppu_context* ctx)
{
    uint64_t tid          = LV2_ARG_U64(ctx, 0);
    uint32_t status_addr  = LV2_ARG_PTR(ctx, 1);
    { static int n=0; if(PS3_LOG_FIRST(n, 30)) fprintf(stderr,"[WAIT] ppu_thread_join(tid=%llu)\n", (unsigned long long)tid); }
    ppu_thread_note_block(ctx->thread_id);   /* PS3_PRIO_START */

    /* Joining yourself is EDEADLK (it used to wait on its own finish event
     * for good). */
    if (tid == ctx->thread_id)
        return (int64_t)(int32_t)CELL_EDEADLK;

    table_lock();
    ppu_thread_info* t = find_thread(tid);
    if (!t) {
        table_unlock();
        return (int64_t)(int32_t)CELL_ESRCH;
    }
    /* A thread has one joiner: a second one gets EINVAL instead of both
     * waiting, both closing the same handles and both freeing the slot. */
    if (!t->joinable || t->joining || t->state == PPU_THREAD_STATE_DETACHED) {
        table_unlock();
        return (int64_t)(int32_t)CELL_EINVAL;
    }
    t->joining = 1;
    table_unlock();

    /* Wait for completion */
#ifdef _WIN32
    WaitForSingleObject(t->finish_event, INFINITE);
    /* sys_ppu_thread_exit signals finish_event from INSIDE the thread, which
     * then still unwinds through ppu_host_thread_proc's epilogue, touching the
     * slot (it stamps the state and signals the event again). Freeing the slot
     * and closing the handles before that finished let the epilogue write to
     * a closed handle and put a slot this join had freed back to FINISHED, so
     * it never came back. Wait for the host thread itself first, as
     * pthread_join does below. Bounded, so that a thread kept running past its
     * exit (PS3_NO_THREAD_EXIT_UNWIND=1) cannot hang the joiner. */
    if (t->host_thread && WaitForSingleObject(t->host_thread, 5000) == WAIT_TIMEOUT) {
        static int s_warned = 0;
        if (s_warned++ < 4)
            fprintf(stderr, "[SYS] ppu_thread_join(tid=%llu): host thread still running 5 s "
                            "after exit -- freeing the slot anyway\n", (unsigned long long)tid);
    }
#else
    pthread_mutex_lock(&t->finish_mutex);
    while (!t->finished) {
        pthread_cond_wait(&t->finish_cond, &t->finish_mutex);
    }
    pthread_mutex_unlock(&t->finish_mutex);
#endif

    /* Write exit status */
    if (status_addr != 0) {
        int64_t es = t->exit_status;
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

    /* Clean up */
#ifdef _WIN32
    table_lock();
    CloseHandle(t->host_thread);
    CloseHandle(t->finish_event);
    t->host_thread = NULL;
    t->finish_event = NULL;
#else
    /* Reap the host thread with the table lock RELEASED.
     *
     * pthread_join blocks until the thread procedure returns, and that
     * procedure's epilogue takes the table lock to stamp its own state. The
     * joiner is woken well before that epilogue runs, because a guest thread
     * normally ends at sys_ppu_thread_exit, which signals `finished` from
     * INSIDE the thread body and then longjmps back out to the epilogue. So a
     * joiner that holds the lock across the join is waiting for a thread that
     * is waiting for the lock -- and the whole title stops with no message.
     *
     * Windows never saw it: its half of this block is CloseHandle, which
     * records nothing about whether the thread has finished and never waits.
     * The two halves have to be read as one thing, and only one of them was.
     * Take the lock again afterwards for the teardown, which is what actually
     * needs it: nothing can claim this slot in between, because it is still
     * FINISHED rather than FREE until the line below. */
    pthread_join(t->host_thread, NULL);
    table_lock();
    pthread_mutex_destroy(&t->finish_mutex);
    pthread_cond_destroy(&t->finish_cond);
#endif
    t->state = PPU_THREAD_STATE_FREE;
    table_unlock();

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_ppu_thread_detach
 *
 * r3 = thread_id
 * -----------------------------------------------------------------------*/
int64_t sys_ppu_thread_detach(ppu_context* ctx)
{
    uint64_t tid = LV2_ARG_U64(ctx, 0);

    table_lock();
    ppu_thread_info* t = find_thread(tid);
    if (!t) {
        table_unlock();
        return (int64_t)(int32_t)CELL_ESRCH;
    }

    if (t->state == PPU_THREAD_STATE_FINISHED) {
        /* Already finished, free it */
#ifdef _WIN32
        /* FINISHED is set by sys_ppu_thread_exit while the host thread still has
         * its epilogue to run (it takes the table lock): let it end before the
         * slot is freed, with the lock released while waiting, as in join. */
        HANDLE h = t->host_thread;
        t->joining = 1;                 /* nobody joins it meanwhile */
        table_unlock();
        if (h) WaitForSingleObject(h, 5000);
        table_lock();
        if (t->state != PPU_THREAD_STATE_FINISHED || t->host_thread != h) {
            table_unlock();             /* freed or reused in the meantime */
            return CELL_OK;
        }
        CloseHandle(t->host_thread);
        CloseHandle(t->finish_event);
        t->host_thread  = NULL;
        t->finish_event = NULL;
#else
        pthread_detach(t->host_thread);
        pthread_mutex_destroy(&t->finish_mutex);
        pthread_cond_destroy(&t->finish_cond);
#endif
        t->state = PPU_THREAD_STATE_FREE;
    } else {
        t->state = PPU_THREAD_STATE_DETACHED;
        t->joinable = 0;
#ifndef _WIN32
        pthread_detach(t->host_thread);
#endif
    }

    table_unlock();
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_ppu_thread_yield
 * -----------------------------------------------------------------------*/
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

/* ---------------------------------------------------------------------------
 * sys_ppu_thread_get_priority
 *
 * r3 = thread_id
 * r4 = pointer to receive priority (s32*)
 * -----------------------------------------------------------------------*/
int64_t sys_ppu_thread_get_priority(ppu_context* ctx)
{
    uint64_t tid       = LV2_ARG_U64(ctx, 0);
    uint32_t prio_addr = LV2_ARG_PTR(ctx, 1);

    table_lock();
    ppu_thread_info* t = find_thread(tid);
    if (!t && tid >= 1 && tid <= PPU_THREAD_MAX && g_ppu_threads[tid - 1].entry_addr != 0) {
        /* A thread created by sys_ppu_thread_create that has exited and been freed (the slot keeps its entry address):
         * it is gone (ESRCH). */
        table_unlock();
        return (int64_t)(int32_t)CELL_ESRCH;
    }
    if (!t) {
        /* The main thread (and any thread we didn't spawn via sys_ppu_thread_create)
         * isn't in our table. Returning ESRCH here is fatal for engines that query
         * their own priority at startup (PhyreEngine PApplication::PlatformInit ->
         * "Error initializing PSSG"). Report a sane default priority + success. */
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

/* ---------------------------------------------------------------------------
 * sys_ppu_thread_set_priority
 *
 * r3 = thread_id
 * r4 = priority
 * -----------------------------------------------------------------------*/
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

/* ---------------------------------------------------------------------------
 * sys_ppu_thread_rename
 *
 * r3 = thread_id
 * r4 = name pointer
 * -----------------------------------------------------------------------*/
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

/* ---------------------------------------------------------------------------
 * sys_ppu_thread_get_join_state
 *
 * r3 = pointer to receive join state (s32*)
 * -----------------------------------------------------------------------*/
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

/* ---------------------------------------------------------------------------
 * sys_ppu_thread_get_stack_information
 *
 * r3 = pointer to receive stack info struct
 *   struct { u32 addr; u32 size; }
 * -----------------------------------------------------------------------*/
/* The main thread's stack, for a host that runs it on one of its own (the
 * template's STACK_TOP, say). ppu_run takes only a stack top, so the host sets
 * these before it; a size of 0 keeps the default below. */
uint32_t g_ppu_main_stack_addr = 0;
uint32_t g_ppu_main_stack_size = 0;

/* What the main thread is told. The default names the first allocation in the
 * thread stack region (VM_STACK_BASE), which is where a host that gives main an
 * lv2-style stack puts it. */
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
    /* The main thread is in the table (ppu_thread_register_main, slot 0) but
     * has no recorded stack. Reporting its 0 + 0 sent a title that looks for
     * data at (addr + size - 0x1000) to 0xFFFFF000. A context with no table
     * entry at all gets the same main-thread answer, as before. */
    if (t && t->stack_size) {
        sa = t->stack_addr;
        ss = t->stack_size;
    } else {
        main_stack_info(&sa, &ss);
    }
    table_unlock();

    static int s_log = -1;
    if (s_log < 0) s_log = getenv("PPU_STACKINFO_LOG") ? 1 : 0;
    if (s_log)
        fprintf(stderr, "[ppu_thread] get_stack_information tid=%llu -> 0x%08X+0x%X\n",
                (unsigned long long)tid, sa, ss);

    if (out_addr != 0) {
        uint32_t* out = (uint32_t*)vm_to_host(out_addr);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
        /* byteswap both fields */
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

/* ---------------------------------------------------------------------------
 * Registration
 * -----------------------------------------------------------------------*/
void sys_ppu_thread_init(lv2_syscall_table* tbl)
{
    /* Initialize stack allocator */
    vm_stack_alloc_init(&g_vm_stack_alloc);

    /* Clear thread table */
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
