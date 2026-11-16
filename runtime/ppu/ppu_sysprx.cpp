/*
 * ps3recomp - sysPrxForUser CRT (boot-critical HLE)
 *
 * The first firmware functions a PS3 program calls at startup come from
 * sysPrxForUser (the libc/CRT bridge). Some need the full ppu_context (e.g.
 * sys_initialize_tls sets the thread pointer r13), so they register as
 * context-aware handlers (ps3_hle_register_ctx) rather than through the generic
 * integer-ABI table.
 *
 * NIDs are computed from the names (ps3_compute_nid), so this stays correct
 * without hand-written NID literals.
 */
#define PPU_NO_INLINE_LOADS   /* declares its own out-of-line vm_read* below */
#include "ppu_recomp.h"     /* ppu_context */
#include "ps3emu/nid.h"     /* ps3_compute_nid */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* win32_compat.h is <windows.h> on Windows (CRITICAL_SECTION for the real
 * lwmutex exclusion) and the POSIX shims elsewhere -- Sleep, DWORD, QPC. */
#include "../platform/win32_compat.h"
#include "../memory/vm.h"   /* VM_HLE_INJECT_BASE -- not platform-specific */
/* sys_timer.h: not included, its lv2 headers redefine ppu_context. */
extern "C" uint64_t ppu_timebase_now(void);
#define PS3_TIMEBASE_FREQ 79800000ULL
#include "ps3emu/log_first.h"

extern "C" uint8_t* vm_base;
extern "C" void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));
extern "C" void ps3_hle_register_fast(uint32_t nid, int (*fn)(ppu_context*));
extern "C" uint32_t vm_read32(uint64_t a);

static unsigned int hle_scratch_alloc(unsigned int size, unsigned int align)
{
    static unsigned int bump = 0x0F800000u;
    if (align < 16) align = 16;
    bump = (bump + align - 1) & ~(align - 1);
    unsigned int a = bump;
    bump += (size + 15u) & ~15u;
    return a;
}extern "C" uint64_t vm_read64(uint64_t a);
extern "C" void     vm_write32(uint64_t a, uint32_t v);
extern "C" void     vm_write64(uint64_t a, uint64_t v);

/* Simple bump allocator for TLS areas, in a free vm region below the stack. */
static uint32_t s_tls_next = 0x0E000000u;

/* sys_initialize_tls(u64 main_thread_id, u32 tls_seg_addr, u32 tls_seg_size,
 *                     u32 tls_mem_size) -- set up the main thread's TLS block
 * and point r13 (the PPC64 thread pointer) at it. TLS variables are accessed
 * at r13 - 0x7000 (the static TLS block bias). */
static void sys_initialize_tls(ppu_context* ctx)
{
    uint32_t seg_addr = (uint32_t)ctx->gpr[4];
    uint32_t seg_size = (uint32_t)ctx->gpr[5];
    uint32_t mem_size = (uint32_t)ctx->gpr[6];

    uint32_t block = s_tls_next;
    uint32_t total = ((mem_size + 0x7000u + 0x1000u) + 0xFFFu) & ~0xFFFu;
    s_tls_next += total;

    if (seg_addr && seg_size) memcpy(vm_base + block, vm_base + seg_addr, seg_size);
    if (mem_size > seg_size)  memset(vm_base + block + seg_size, 0, mem_size - seg_size);

    ctx->gpr[13] = block + 0x7000u;   /* thread pointer; TLS data at r13-0x7000 */
    ctx->gpr[3]  = 0;                  /* CELL_OK */
    fprintf(stderr, "[crt] sys_initialize_tls: block 0x%08X, r13=0x%08X (seg 0x%X+%u, mem %u)\n",
            block, (uint32_t)ctx->gpr[13], seg_addr, seg_size, mem_size);
}

/* sys_time_get_system_time() -> microseconds since boot, REAL time.
 * This was a fake counter advancing 1 ms PER CALL ("so callers see time
 * progress") -- so any guest clock built on it ran at call-rate, not
 * wall-time. LBP's Bink movie clock (sysGetSystemTime import 0x8461E528
 * lands here) paced the intro at ~1/10th speed: video decoded at <1 fps,
 * the audio preload threshold took forever to fill (movie stayed silent),
 * and every frontend animation timed off it crawled. */
static void sys_time_get_system_time(ppu_context* ctx)
{
    const uint64_t tb = ppu_timebase_now();
    ctx->gpr[3] = tb / PS3_TIMEBASE_FREQ * 1000000ull + tb % PS3_TIMEBASE_FREQ * 1000000ull / PS3_TIMEBASE_FREQ;
}

/* sys_process_is_stack(u32 addr) -> 1 if addr is in the stack region. We model
 * a single stack just below the TLS region; good enough for boot checks. */
static void sys_process_is_stack(ppu_context* ctx)
{
    uint32_t a = (uint32_t)ctx->gpr[3];
    ctx->gpr[3] = (a >= 0x0E000000u && a < 0x10000000u) ? 1 : 0;
}

/* ---------------------------------------------------------------------------
 * Lightweight mutex (sys_lwmutex) — sysPrxForUser.
 *
 * The CRT guards global/singleton initialization with lwmutexes. If create is
 * a no-op that never initializes the structure, the guarded init is skipped
 * and the protected registry is left with null function pointers (the early
 * boot then spins calling a null vtable entry). We model the structure for
 * real, and lock it for real (see "REAL mutual exclusion" below).
 *
 * sys_lwmutex_t (big-endian, 24 bytes):
 *   +0x00 owner (u32)   +0x04 waiter (u32)   +0x08 attribute (u32)
 *   +0x0C recursive_count (u32)   +0x10 sleep_queue (u32)   +0x14 pad
 * sys_lwmutex_attribute_t: +0x00 protocol  +0x04 recursive  +0x08 name[8]
 * -----------------------------------------------------------------------*/
#define LWM_OWNER  0x00
#define LWM_ATTR   0x08
#define LWM_RECUR  0x0C
/* Owner id = ctx->thread_id, which is nonzero for every thread since main
 * registers as id 1 (ppu_thread_register_main). The old 0->1 fallback made
 * main alias the FIRST CREATED thread: each passed the other's recursive
 * re-lock check, both "owned" the lock, and (LBP) main + bringup emitted GCM
 * concurrently -- fences vanished mid-ring. A zero id now means an
 * unregistered context (bug); stamp a sentinel that matches no real thread. */
#define LWM_SELF(ctx) ((uint32_t)(ctx)->thread_id ? (uint32_t)(ctx)->thread_id : 0x7FFFFFFEu)

struct LwmSlot;
static struct LwmSlot* lwm_slot(uint32_t addr);   /* fwd (defined below) */
static void lwm_release(struct LwmSlot* sl);
static void sys_lwmutex_create(ppu_context* ctx)
{
    uint32_t lwm  = (uint32_t)ctx->gpr[3];
    uint32_t attr = (uint32_t)ctx->gpr[4];
    uint32_t protocol = attr ? vm_read32(attr + 0) : 0;
    /* sys_lwmutex_t.attribute is protocol | recursive (the two words of
     * sys_lwmutex_attribute_t OR-ed together); the recursive word was dropped. */
    uint32_t recursive = attr ? vm_read32(attr + 4) : 0;
    vm_write32(lwm + 0x00, 0);          /* owner */
    vm_write32(lwm + 0x04, 0);          /* waiter */
    vm_write32(lwm + LWM_ATTR, protocol | recursive);
    vm_write32(lwm + LWM_RECUR, 0);     /* recursive_count */
    vm_write32(lwm + 0x10, 0);          /* sleep_queue */
    vm_write32(lwm + 0x14, 0);
    /* A recreate at a reused address must not inherit a locked slot (e.g. the
     * previous holder exited while holding): release it. Releasing a free
     * lock does nothing. */
    { struct LwmSlot* sl = lwm_slot(lwm); if (sl) lwm_release(sl); }
    ctx->gpr[3] = 0;
}
/* REAL mutual exclusion. The old no-op ("boot is single-threaded") corrupted
 * every lwmutex-protected structure once LBP spun up its worker/loader threads --
 * notably the dlmalloc mspace behind the game's big-allocator, whose tree then
 * fell apart and reported OOM on a tiny request with 100+ MB free.
 *
 * The same on every host: on POSIX the semaphore is win32_compat's. This was
 * Windows-only for a while, which left every lwmutex on Linux a bare owner
 * stamp -- Twisted Metal's FIOS layer then freed one block from two threads
 * and its dlmalloc free() aborted on a chunk already merged into top.
 *
 * Backed by a binary SEMAPHORE, not a CRITICAL_SECTION: a CS may only be
 * released by its owning thread, but guest code passes lwmutex ownership
 * between threads (LBP's job system) and threads exit while holding -- one
 * cross-thread unlock silently failed and the still-owned CS parked the next
 * locker forever (the Network-node hang). A semaphore releases from any
 * thread. Recursion is handled explicitly via the guest owner/recur fields
 * we stamp (only the holder ever writes owner=self, so the re-lock check is
 * race-free). Keyed by guest address in an open-addressed table. */
#define LWM_HASH 65536u
static struct LwmSlot { volatile long addr; HANDLE sem; volatile long state; } g_lwm[LWM_HASH];
static volatile long g_lwm_tab_lock = 0;
static long long lwm_now_us(void){
    static LARGE_INTEGER freq={0}; if(!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    /* Seconds and remainder apart: on POSIX the frequency is 1e9, and the
     * counter times 1e6 overflows after a few hours of uptime. */
    return (long long)((c.QuadPart/freq.QuadPart)*1000000ll + (c.QuadPart%freq.QuadPart)*1000000ll/freq.QuadPart);
}
static struct LwmSlot* lwm_slot(uint32_t addr)
{
    if (!addr) return nullptr;
    uint32_t h = (addr * 2654435761u) & (LWM_HASH - 1);
    for (uint32_t i = 0; i < LWM_HASH; i++) {
        uint32_t idx = (h + i) & (LWM_HASH - 1);
        long cur = g_lwm[idx].addr;
        if ((uint32_t)cur == addr) return &g_lwm[idx];
        if (cur == 0) {
            while (_InterlockedExchange(&g_lwm_tab_lock, 1)) YieldProcessor();
            struct LwmSlot* r = nullptr;
            if (g_lwm[idx].addr == 0) {
                g_lwm[idx].sem = CreateSemaphoreA(NULL, 0, 1, NULL);   /* wake channel, empty */
                g_lwm[idx].state = 0;                                   /* free */
                g_lwm[idx].addr = (long)addr;   /* publish AFTER init (x86 TSO: readers see init) */
                r = &g_lwm[idx];
            } else if ((uint32_t)g_lwm[idx].addr == addr) {
                r = &g_lwm[idx];
            }
            _InterlockedExchange(&g_lwm_tab_lock, 0);
            if (r) return r;
            /* someone else claimed this slot for a different addr -> keep probing */
        }
    }
    return nullptr;   /* table full (raise LWM_HASH) */
}
/* The lock itself is the slot's state word: 0 free, 1 held, 2 held with a
 * thread waiting (or that was). Taking a free lock and releasing one nobody
 * waits for are one atomic operation each; the semaphore only wakes a waiter,
 * which then tries again. Every lock and unlock used to go through the
 * semaphore, two kernel calls a pair: God of War HD makes some 320,000 pairs
 * in one frame when a fight's next wave spawns, and that frame took 360 ms.
 * A waiter that gives up leaves the word at 2, so the next release posts a
 * wake nobody takes; the next waiter consumes it and waits again. */
static int lwm_try(struct LwmSlot* sl)
{
    return InterlockedCompareExchange(&sl->state, 1, 0) == 0;
}
/* 1 = taken, 0 = timed out. timeout_us 0 waits forever. */
static int lwm_acquire(struct LwmSlot* sl, uint64_t timeout_us)
{
    if (lwm_try(sl)) return 1;
    const long long deadline = timeout_us ? lwm_now_us() + (long long)timeout_us : 0;
    while (InterlockedExchange(&sl->state, 2) != 0) {
        DWORD ms = INFINITE;
        if (timeout_us) {
            const long long left = deadline - lwm_now_us();
            if (left <= 0) return 0;
            const unsigned long long m = ((unsigned long long)left + 999) / 1000;
            ms = m > 0xFFFFFFFEull ? 0xFFFFFFFEu : (DWORD)m;
        }
        WaitForSingleObject(sl->sem, ms);
    }
    return 1;
}
static void lwm_release(struct LwmSlot* sl)
{
    if (InterlockedExchange(&sl->state, 0) == 2) ReleaseSemaphore(sl->sem, 1, NULL);
}
/* syscalls/sys_ppu_thread.c: this thread is about to wait (PS3_PRIO_START). */
extern "C" void ppu_thread_note_block(uint64_t tid);
static void sys_lwmutex_lock(ppu_context* ctx)
{
    uint32_t lwm = (uint32_t)ctx->gpr[3];
    uint32_t self = LWM_SELF(ctx);
    /* lv2 ABI: r4 = timeout in microseconds, 0 = infinite. The real kernel
     * returns ETIMEDOUT (0x8001000B) when the wait expires; games rely on that
     * (e.g. LBP's resource loader locks with a 2s timeout in a retry loop so a
     * contended lock yields to other threads instead of hard-blocking). We had
     * been ignoring r4 and always waiting INFINITE, which defeats that pattern. */
    uint64_t timeout_us = ctx->gpr[4];
    struct LwmSlot* sl0 = lwm_slot(lwm);
    if (sl0) {
        /* Recursive re-lock by the current holder: bump the count, no wait.
         * Only the holder ever stamps owner=self, so this check is race-free. */
        if (vm_read32(lwm + LWM_OWNER) == self && vm_read32(lwm + LWM_RECUR) > 0) {
            vm_write32(lwm + LWM_RECUR, vm_read32(lwm + LWM_RECUR) + 1);
            ctx->gpr[3] = 0;
            return;
        }
        if (!lwm_try(sl0)) {
            /* Contended: log who we're stuck behind (owner stamped at acquire),
             * then block. */
            static long _bl = 0; long _b = ++_bl;
            if (_b <= 40) fprintf(stderr, "[LWM-BLOCK] tid=%llu lwm=0x%08X owner=%u recur=%u tmo=%lluus\n",
                (unsigned long long)ctx->thread_id, lwm, vm_read32(lwm + LWM_OWNER), vm_read32(lwm + LWM_RECUR),
                (unsigned long long)timeout_us);
            ppu_thread_note_block(ctx->thread_id);
            if (!lwm_acquire(sl0, timeout_us)) {   /* honor the timeout: ETIMEDOUT, no acquire */
                ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x8001000Bu;
                return;
            }
            if (_b <= 40) fprintf(stderr, "[LWM-GOT] tid=%llu lwm=0x%08X\n", (unsigned long long)ctx->thread_id, lwm);
        }
    }
    vm_write32(lwm + LWM_OWNER, self);
    vm_write32(lwm + LWM_RECUR, 1);
    ctx->gpr[3] = 0;   // CELL_OK
}
/* Fast forms for ps3_hle_call (see ps3_hle_register_fast): a lock that is
 * free or already ours, and any unlock. A lock that has to wait takes the
 * full handler. */
static int sys_lwmutex_lock_fast(ppu_context* ctx)
{
    const uint32_t lwm = (uint32_t)ctx->gpr[3];
    struct LwmSlot* sl = lwm_slot(lwm);
    if (!sl) return 0;
    const uint32_t self = LWM_SELF(ctx);
    if (vm_read32(lwm + LWM_OWNER) == self && vm_read32(lwm + LWM_RECUR) > 0)
        vm_write32(lwm + LWM_RECUR, vm_read32(lwm + LWM_RECUR) + 1);
    else if (lwm_try(sl)) {
        vm_write32(lwm + LWM_OWNER, self);
        vm_write32(lwm + LWM_RECUR, 1);
    } else
        return 0;
    ctx->gpr[3] = 0;
    return 1;
}
static void sys_lwmutex_unlock(ppu_context* ctx);
static int sys_lwmutex_unlock_fast(ppu_context* ctx)
{
    sys_lwmutex_unlock(ctx);
    return 1;
}
static void sys_lwmutex_trylock(ppu_context* ctx)
{
    uint32_t lwm = (uint32_t)ctx->gpr[3];
    uint32_t self = LWM_SELF(ctx);
    struct LwmSlot* sl0 = lwm_slot(lwm);
    if (sl0) {
        if (vm_read32(lwm + LWM_OWNER) == self && vm_read32(lwm + LWM_RECUR) > 0) {
            vm_write32(lwm + LWM_RECUR, vm_read32(lwm + LWM_RECUR) + 1);
            ctx->gpr[3] = 0;
            return;
        }
        /* Held by another thread: lv2 answers CELL_EBUSY (0x8001000A). 0x8001000B
         * is ETIMEDOUT, which only a lock with an expired timeout returns; titles
         * that poll with trylock and check for EBUSY treat it as a hard failure. */
        if (!lwm_try(sl0)) { ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x8001000Au; return; }
    }
    vm_write32(lwm + LWM_OWNER, self);
    vm_write32(lwm + LWM_RECUR, 1);
    ctx->gpr[3] = 0;
}
static void sys_lwmutex_unlock(ppu_context* ctx)
{
    uint32_t lwm = (uint32_t)ctx->gpr[3];
    uint32_t rc = vm_read32(lwm + LWM_RECUR);
    if (rc > 1) {                       /* recursive hold: count down, keep the lock */
        vm_write32(lwm + LWM_RECUR, rc - 1);
        ctx->gpr[3] = 0;
        return;
    }
    vm_write32(lwm + LWM_RECUR, 0);
    vm_write32(lwm + LWM_OWNER, 0);
    /* Any thread may release (unlike a CS) -- guest code hands lwmutex
     * ownership across threads. Unlocking a free mutex does nothing. */
    struct LwmSlot* sl0 = lwm_slot(lwm);
    if (sl0) lwm_release(sl0);
    ctx->gpr[3] = 0;
}

/* sys_lwcond (sysPrxForUser) — guest-side condition variable, paired with an
 * lwmutex. The CRT and (newly) libsre's cellSpurs create/wait/signal these. Like
 * sys_lwmutex above, model it directly in guest memory so the args stay GUEST
 * EAs (the generic adapter would pass them raw and the C sysPrxForUser impl
 * deref'd them as host pointers -> AV during cellSpurs init). A no-op wait is
 * adequate here: the CRT/SPURS paths that reach us use these for one-shot init
 * handshakes, not long-term blocking. sys_lwcond_t is 8 bytes: +0x00 u32
 * lwmutex EA, +0x04 u32 lwcond queue id. Titles read +0x00 themselves (an
 * lwcond whose lwmutex reads 0 is treated as invalid). */
static void sys_lwcond_create(ppu_context* ctx)
{
    static uint32_t s_lwcond_id = 0x4C000000u;
    uint32_t lwcond  = (uint32_t)ctx->gpr[3];
    uint32_t lwmutex = (uint32_t)ctx->gpr[4];
    vm_write32(lwcond + 0x00, lwmutex);
    vm_write32(lwcond + 0x04, ++s_lwcond_id);
    ctx->gpr[3] = 0;
}
static void sys_lwcond_destroy(ppu_context* ctx)    { ctx->gpr[3] = 0; }
static void sys_lwcond_signal(ppu_context* ctx)     { ctx->gpr[3] = 0; }
static void sys_lwcond_signal_all(ppu_context* ctx) { ctx->gpr[3] = 0; }
/* Now that the lwmutex is REAL, a no-op wait that keeps holding it deadlocks the
 * signaler. Release the paired lwmutex, wait briefly, reacquire (poll-style: the
 * guest's while(!predicate) loop re-checks; signalers stay no-ops). Handles the
 * common single (non-recursive) hold. */
static void sys_lwcond_wait(ppu_context* ctx)
{
    uint32_t lwcond  = (uint32_t)ctx->gpr[3];
    uint32_t lwmutex = vm_read32(lwcond + 0x00);
    struct LwmSlot* sl = lwm_slot(lwmutex);
    if (sl) {
        uint32_t own = vm_read32(lwmutex + LWM_OWNER);
        uint32_t rc  = vm_read32(lwmutex + LWM_RECUR);
        vm_write32(lwmutex + LWM_RECUR, 0);
        vm_write32(lwmutex + LWM_OWNER, 0);
        ppu_thread_note_block(ctx->thread_id);
        lwm_release(sl);
        Sleep(1);
        lwm_acquire(sl, 0);
        vm_write32(lwmutex + LWM_OWNER, own);
        vm_write32(lwmutex + LWM_RECUR, rc ? rc : 1);
    }
    ctx->gpr[3] = 0;
}

/* sys_ppu_thread_get_id(vm::ptr<u64> id) -> *id = calling thread's real id.
 * The old fixed "1" broke every am-I-the-designated-thread check in
 * multithreaded titles (LBP's job system routes work by thread identity, so
 * its queues were never serviced and network-init parked forever). */
static void sys_ppu_thread_get_id(ppu_context* ctx)
{
    uint32_t p = (uint32_t)ctx->gpr[3];
    /* Every registered thread has a nonzero id (main = 1 via
     * ppu_thread_register_main); 0 = unregistered scratch ctx, report the same
     * never-a-real-thread sentinel the lwmutex owner stamps use. */
    if (p) vm_write64(p, ctx->thread_id ? (uint64_t)ctx->thread_id : 0x7FFFFFFEull);
    ctx->gpr[3] = 0;
}

/* sys_mmapper_allocate_memory(u32 size, u64 flags, vm::ptr<u32> mem_id) ->
 * hand back a unique opaque id; the backing is the flat VM, so the later
 * search_and_map just needs a non-zero id to track. */
/* id -> size so sys_mmapper_search_and_map (lv2 337) can lay blocks out
 * without overlap. Ids are dense from 0x1000. */
static uint32_t s_mm_sizes[256];
static uint32_t s_mmapper_next_id = 0x1000;
extern "C" uint32_t ps3_mmapper_block_size(uint32_t mem_id)
{
    uint32_t i = mem_id - 0x1000u;
    return (i < 256) ? s_mm_sizes[i] : 0;
}

static uint32_t mmapper_new_id(uint32_t size)
{
    uint32_t id = s_mmapper_next_id++;
    if (id - 0x1000u < 256) s_mm_sizes[id - 0x1000u] = size;
    return id;
}

static void sys_mmapper_allocate_memory(ppu_context* ctx)
{
    uint32_t size       = (uint32_t)ctx->gpr[3];
    uint32_t mem_id_ptr = (uint32_t)ctx->gpr[5];
    uint32_t id         = mmapper_new_id(size);
    if (getenv("PS3_MEMTRACE"))
        fprintf(stderr, "[mmapper] allocate_memory(size=0x%X flags=0x%llX id_ptr=0x%X) -> id 0x%X\n",
                size, (unsigned long long)ctx->gpr[4], mem_id_ptr, id);
    if (mem_id_ptr) vm_write32(mem_id_ptr, id);
    ctx->gpr[3] = 0;
}
/* A handful of CRT helpers the early boot tends to hit; accept and continue. */
static void crt_ok(ppu_context* ctx) { ctx->gpr[3] = 0; }

/* Real preemptive thread create/exit live in the lv2 syscall layer
 * (syscalls/sys_ppu_thread.c) and spawn a host thread that runs the guest
 * entry through the recompiled code. The CRT also reaches them as
 * sysPrxForUser import NIDs (gen_hle_nids can't see them — they're not defined
 * in the sysPrxForUser lib), so bridge the NIDs to the same implementation.
 * Without this the CRT's thread/static-init runs through an uninitialised
 * object table and calls heap addresses as function pointers. */
extern "C" int64_t sys_ppu_thread_create(ppu_context* ctx);
extern "C" int64_t sys_ppu_thread_exit(ppu_context* ctx);
/* The ctx-aware dispatch (ppu_hle.cpp) does NOT propagate a handler return value
 * into gpr[3] -- each ctx handler must set gpr[3] itself. sys_ppu_thread_create
 * signals success by *returning* CELL_OK(0) (it never writes gpr[3]), so we must
 * store that return into gpr[3]. Otherwise gpr[3] is left as the incoming out-ptr
 * (&tid, nonzero) and the guest wrapper reads it as "create failed" -- e.g. LBP's
 * sub_52613C does `v6 = (ret==0); return v6 ? tid : 0`, so a nonzero ret makes it
 * hand back 0 and the caller's init (sub_C1484 / KdConvert) bails. */
static void hle_ppu_thread_create(ppu_context* ctx) { ctx->gpr[3] = sys_ppu_thread_create(ctx); }
static void hle_ppu_thread_exit(ppu_context* ctx)   { ctx->gpr[3] = sys_ppu_thread_exit(ctx); }


/* --- sys_net offline model ---------------------------------------------
 * LBP's net-services tick (sub_11A864) drains its UDP socket with
 * non-blocking recvfrom until it returns -1 (empty socket = EWOULDBLOCK on
 * real firmware). These NIDs were unresolved, and the unresolved default of
 * r3=0 reads as "received a 0-byte packet": the drain loop spins forever
 * while holding the net-manager lwmutex, wedging the whole boot at the
 * Network init node. Model the offline truth instead: no data, no sockets --
 * every receive/poll would-block. errno lives in a guest scratch cell since
 * _sys_net_errno_loc returns a POINTER the game dereferences. */
#define SYS_NET_EWOULDBLOCK_V 35
extern "C" void ps3_net_host_register(unsigned int (*guest_alloc)(unsigned int, unsigned int));
extern "C" void cellSaveData_register_ctx_handlers(void); /* libs/system/cellSaveData.c */
static uint32_t g_net_errno_ea = 0;
static void hle_net_errno_loc(ppu_context* ctx)
{
    if (!g_net_errno_ea) g_net_errno_ea = hle_scratch_alloc(4, 4);
    vm_write32(g_net_errno_ea, SYS_NET_EWOULDBLOCK_V);
    ctx->gpr[3] = g_net_errno_ea;
}
static void hle_net_wouldblock(ppu_context* ctx)
{
    if (g_net_errno_ea) vm_write32(g_net_errno_ea, SYS_NET_EWOULDBLOCK_V);
    ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)-1;
}
static void hle_net_zero(ppu_context* ctx) { ctx->gpr[3] = 0; }
/* select/poll: nothing is ever ready offline -- but a real select BLOCKS for
 * the caller's timeout before saying so. Returning instantly turned LBP's
 * 30Hz net pump (sub_3A5548: select(1, r/w/e sets, {0s, 33333us})) into a
 * 100%-CPU busy-spin that also dominated the guest-PC profiler, masquerading
 * as a boot hang. Honor the timeout and clear the fd sets (1024-bit each). */
static void hle_net_select(ppu_context* ctx)
{
    uint32_t rd = (uint32_t)ctx->gpr[4], wr = (uint32_t)ctx->gpr[5];
    uint32_t ex = (uint32_t)ctx->gpr[6], tv = (uint32_t)ctx->gpr[7];
    uint64_t us = 10000;              /* NULL timeout = block forever: tick at 10ms instead */
    if (tv) us = vm_read64(tv) * 1000000ull + vm_read64(tv + 8);   /* {s64 sec, s64 usec} BE */
    if (us > 100000) us = 100000;     /* cap so shutdown stays responsive */
    if (us) Sleep((DWORD)((us + 999) / 1000));
    const uint32_t sets[3] = { rd, wr, ex };
    for (int s = 0; s < 3; s++)
        if (sets[s]) for (uint32_t i = 0; i < 128; i += 4) vm_write32(sets[s] + i, 0);
    ctx->gpr[3] = 0;                  /* 0 fds ready */
}
/* Distinct small fds: the unresolved default handed EVERY socket() call fd 0,
 * making all sockets alias one id in the game's tables. */
static void hle_net_socket(ppu_context* ctx) { static uint32_t s_fd = 3; ctx->gpr[3] = s_fd++; }
/* sendto: report the full length as sent (packets vanish into the void, matching
 * the RPCS3-offline oracle where broadcasts go out and nothing answers). */
static void hle_net_sendto(ppu_context* ctx) { ctx->gpr[3] = (uint32_t)ctx->gpr[5]; }

/* _sys_spu_image_import (sysPrxForUser NID 0xEBE5F72F) -- the user-space wrapper
 * libsre calls during cellSpursInitialize to load the SPURS SPU kernel into the
 * sys_spu_image struct. Previously UNRESOLVED -> returned 0 -> libsre proceeded
 * with an EMPTY image (entry=0) -> the 5 SPURS SPU threads ran nothing -> PPU
 * busy-waits on an SPU completion that never comes -> 0 GCM draws
 * (prx/spu_kernel/README.txt step 1). Parses the SPU ELF at r4 into the image
 * struct at r3.
 *
 * The ELF-parse (segment/entry layout below) is lifted directly from sagemono's
 * sys_spu_image_import SYSCALL handler in PR #57 (fix/spu-image-import); this is
 * the sysPrxForUser LIBRARY counterpart libsre actually calls. Credit: sagemono
 * (PR #57) for the import implementation + SPU-image syscall-number fix.
 * Self-verifying: no-op unless r4 points to an ELF (so a wrong NID guess is safe).
 * sys_spu_image { u32 type; u32 entry; sys_spu_segment* segs; int nsegs; }
 * sys_spu_segment{ int type; u32 ls_start; int size; u64 src_pa }  (0x18, src@0x10) */
/* img_ea -> the EA of the SPU ELF it was parsed from. Small and fixed: a title
 * has a handful of images, and a repeat import of the same descriptor replaces
 * its entry. */
#define SPU_IMG_SRC_MAX 32
static struct { uint32_t img, src; } s_spu_img_src[SPU_IMG_SRC_MAX];

extern "C" void ps3_spu_image_record(uint32_t img_ea, uint32_t src_ea)
{
    if (!img_ea || !src_ea) return;
    for (int i = 0; i < SPU_IMG_SRC_MAX; i++)
        if (s_spu_img_src[i].img == img_ea || s_spu_img_src[i].img == 0) {
            s_spu_img_src[i].img = img_ea; s_spu_img_src[i].src = src_ea; return;
        }
}

extern "C" uint32_t ps3_spu_image_source_ea(uint32_t img_ea)
{
    for (int i = 0; i < SPU_IMG_SRC_MAX; i++)
        if (s_spu_img_src[i].img == img_ea) return s_spu_img_src[i].src;
    return 0;
}

extern "C" void spu_raw_note_image(uint32_t src_ea, uint32_t entry);  /* runtime/spu/spu_raw.c */

static void hle_sys_spu_image_import(ppu_context* ctx)
{
    uint32_t img_ea = (uint32_t)ctx->gpr[3];
    uint32_t src_ea = (uint32_t)ctx->gpr[4];
    fprintf(stderr, "[HLE] _sys_spu_image_import(img=0x%08X src=0x%08X r5=0x%08X r6=0x%08X)\n",
            img_ea, src_ea, (uint32_t)ctx->gpr[5], (uint32_t)ctx->gpr[6]);
    if (!img_ea || !src_ea || !vm_base) { ctx->gpr[3] = 0; return; }
    const uint8_t* e = vm_base + src_ea;
    if (!(e[0]==0x7F && e[1]=='E' && e[2]=='L' && e[3]=='F')) {
        fprintf(stderr, "[HLE] _sys_spu_image_import: src not an ELF -> no-op\n");
        fflush(stderr); ctx->gpr[3] = 0; return;
    }
    uint16_t machine = (uint16_t)((e[0x12] << 8) | e[0x13]);   /* 23 = SPU */
    uint32_t entry   = vm_read32(src_ea + 0x18);
    uint32_t phoff   = vm_read32(src_ea + 0x1C);
    uint16_t phentsz = (uint16_t)((e[0x2A] << 8) | e[0x2B]); if (!phentsz) phentsz = 0x20;
    uint16_t phnum   = (uint16_t)((e[0x2C] << 8) | e[0x2D]);
    static uint32_t s_seg_bump = 0x0D000000u;
    uint32_t segs_ea = s_seg_bump; int nsegs = 0;
    for (uint16_t i = 0; i < phnum && nsegs < 32; i++) {
        uint32_t ph = phoff + (uint32_t)i * phentsz;
        if (vm_read32(src_ea + ph + 0x00) != 1) continue;      /* PT_LOAD */
        uint32_t p_off = vm_read32(src_ea + ph + 0x04);
        uint32_t p_va  = vm_read32(src_ea + ph + 0x08);
        uint32_t p_fsz = vm_read32(src_ea + ph + 0x10);
        uint32_t p_msz = vm_read32(src_ea + ph + 0x14);
        uint32_t seg = segs_ea + (uint32_t)nsegs * 0x18;       /* COPY */
        vm_write32(seg + 0x00, 1); vm_write32(seg + 0x04, p_va);
        vm_write32(seg + 0x08, p_fsz);
        /* src is the u64 at +0x10; its low word, which every 32-bit reader
         * takes, is +0x14 -- but LBP's FMOD mixer reads the u32 at +0x10 as
         * the DMA source (it DMA'd its DSP overlay from EA 0 and jumped into
         * zeroed LS), so write both, as the lv2 syscall path already does. */
        vm_write32(seg + 0x10, src_ea + p_off);
        vm_write32(seg + 0x14, src_ea + p_off); nsegs++;
        if (p_msz > p_fsz && nsegs < 32) {                     /* BSS tail -> FILL 0 */
            seg = segs_ea + (uint32_t)nsegs * 0x18;
            vm_write32(seg + 0x00, 2); vm_write32(seg + 0x04, p_va + p_fsz);
            vm_write32(seg + 0x08, p_msz - p_fsz);
            vm_write32(seg + 0x10, 0); vm_write32(seg + 0x14, 0); nsegs++;
        }
    }
    s_seg_bump += (uint32_t)nsegs * 0x18;
    if (s_seg_bump >= 0x0E000000u) s_seg_bump = 0x0D000000u;
    /* Remember which ELF this descriptor was parsed from. sys_spu_thread_group_
     * start only gets the DESCRIPTOR, but the workload registry is keyed by a
     * fingerprint of the ELF's own bytes -- without this the raw SPU path has no
     * way to ask whether the image it is about to run was lifted. */
    ps3_spu_image_record(img_ea, src_ea);
    /* ...and ask that question now, while the ELF is still identified. A raw SPU
     * is started by an MMIO store to its run-control register, with no syscall in
     * between, so this import is the last point at which the image can be matched
     * to a lifted entry (runtime/spu/spu_raw.c). No-op for a SPU-thread image. */
    spu_raw_note_image(src_ea, entry);
    vm_write32(img_ea + 0x00, 0);                              /* type = USER */
    vm_write32(img_ea + 0x04, entry);
    vm_write32(img_ea + 0x08, nsegs ? segs_ea : 0);
    vm_write32(img_ea + 0x0C, (uint32_t)nsegs);
    fprintf(stderr, "[HLE] _sys_spu_image_import -> entry=0x%05X nsegs=%d machine=%u (SPU=23)\n",
            entry, nsegs, machine);
    fflush(stderr);
    ctx->gpr[3] = 0;
}

extern "C" void ppu_sysprx_register(void)
{
    ps3_hle_register_ctx(0xEBE5F72Fu, "_sys_spu_image_import", hle_sys_spu_image_import);

    /* PS3_NET_ONLINE: real host sockets (libs/network/sysNet.c). Registered
     * first because the first registration of a NID wins, so these shadow the
     * offline model below; without the variable nothing changes. */
    if (getenv("PS3_NET_ONLINE")) ps3_net_host_register(hle_scratch_alloc);
    /* cellSaveDataUserAutoLoad/UserAutoSave take nine arguments; the ninth
     * (userdata) is on the guest stack, so they need the full context too.
     * Registered here so no project has to remember the call. */
    cellSaveData_register_ctx_handlers();
    /* sys_net offline model (NIDs from PSL1GHT libnet exports). Covers every
     * sys_net NID LBP imports so none fall to the unresolved-NID default. */
    ps3_hle_register_ctx(0x6005CDE1u, "_sys_net_errno_loc",     hle_net_errno_loc);
    ps3_hle_register_ctx(0x1F953B9Fu, "sys_net_bnet_recvfrom",  hle_net_wouldblock);
    ps3_hle_register_ctx(0xFBA04F37u, "sys_net_bnet_recv",      hle_net_wouldblock);
    ps3_hle_register_ctx(0x3F09E20Au, "sys_net_bnet_select",    hle_net_select);
    ps3_hle_register_ctx(0x139A9E9Bu, "netInitializeNetworkEx", hle_net_zero);   /* lib init ok */
    ps3_hle_register_ctx(0x9C056962u, "netSocket",              hle_net_socket);
    ps3_hle_register_ctx(0xB0A59804u, "netBind",                hle_net_zero);
    ps3_hle_register_ctx(0x88F03575u, "netSetSockOpt",          hle_net_zero);
    ps3_hle_register_ctx(0x9647570Bu, "netSendTo",              hle_net_sendto);
    ps3_hle_register_ctx(0x6DB6E8CDu, "netClose",               hle_net_zero);
    ps3_hle_register_ctx(0x71F4C717u, "netGetHostByName",       hle_net_zero);   /* NULL: DNS down */
    ps3_hle_register_ctx(0xB68D5625u, "netFinalizeNetwork",     hle_net_zero);
    ps3_hle_register_ctx(ps3_compute_nid("sys_initialize_tls"),       "sys_initialize_tls",       sys_initialize_tls);
    ps3_hle_register_ctx(ps3_compute_nid("sys_time_get_system_time"), "sys_time_get_system_time", sys_time_get_system_time);
    ps3_hle_register_ctx(ps3_compute_nid("sys_process_is_stack"),     "sys_process_is_stack",     sys_process_is_stack);
    /* Atexit registration: nothing to do at boot, just succeed. */
    ps3_hle_register_ctx(ps3_compute_nid("_sys_process_atexitspawn"), "_sys_process_atexitspawn", crt_ok);
    ps3_hle_register_ctx(ps3_compute_nid("_sys_process_at_Exitspawn"),"_sys_process_at_Exitspawn",crt_ok);

    /* Lightweight mutex family (guards global/singleton init in the CRT). */
    ps3_hle_register_ctx(ps3_compute_nid("sys_lwmutex_create"),  "sys_lwmutex_create",  sys_lwmutex_create);
    ps3_hle_register_ctx(ps3_compute_nid("sys_lwmutex_destroy"), "sys_lwmutex_destroy", crt_ok);
    ps3_hle_register_ctx(ps3_compute_nid("sys_lwmutex_lock"),    "sys_lwmutex_lock",    sys_lwmutex_lock);
    ps3_hle_register_ctx(ps3_compute_nid("sys_lwmutex_unlock"),  "sys_lwmutex_unlock",  sys_lwmutex_unlock);
    ps3_hle_register_ctx(ps3_compute_nid("sys_lwmutex_trylock"), "sys_lwmutex_trylock", sys_lwmutex_trylock);
    ps3_hle_register_fast(ps3_compute_nid("sys_lwmutex_lock"),   sys_lwmutex_lock_fast);
    ps3_hle_register_fast(ps3_compute_nid("sys_lwmutex_unlock"), sys_lwmutex_unlock_fast);

    ps3_hle_register_ctx(ps3_compute_nid("sys_lwcond_create"),     "sys_lwcond_create",     sys_lwcond_create);
    ps3_hle_register_ctx(ps3_compute_nid("sys_lwcond_destroy"),    "sys_lwcond_destroy",    sys_lwcond_destroy);
    ps3_hle_register_ctx(ps3_compute_nid("sys_lwcond_signal"),     "sys_lwcond_signal",     sys_lwcond_signal);
    ps3_hle_register_ctx(ps3_compute_nid("sys_lwcond_signal_all"), "sys_lwcond_signal_all", sys_lwcond_signal_all);
    ps3_hle_register_ctx(ps3_compute_nid("sys_lwcond_wait"),       "sys_lwcond_wait",       sys_lwcond_wait);

    /* Thread id + memory manager (high-frequency boot imports). The flat VM
     * means map/unmap/free are no-ops: the memory already exists everywhere. */
    ps3_hle_register_ctx(ps3_compute_nid("sys_ppu_thread_get_id"),      "sys_ppu_thread_get_id",      sys_ppu_thread_get_id);
    ps3_hle_register_ctx(ps3_compute_nid("sys_ppu_thread_create"),      "sys_ppu_thread_create",      hle_ppu_thread_create);
    ps3_hle_register_ctx(ps3_compute_nid("sys_ppu_thread_exit"),        "sys_ppu_thread_exit",        hle_ppu_thread_exit);
    ps3_hle_register_ctx(ps3_compute_nid("sys_mmapper_allocate_memory"), "sys_mmapper_allocate_memory", sys_mmapper_allocate_memory);
    ps3_hle_register_ctx(ps3_compute_nid("sys_mmapper_map_memory"),     "sys_mmapper_map_memory",     crt_ok);
    ps3_hle_register_ctx(ps3_compute_nid("sys_mmapper_unmap_memory"),   "sys_mmapper_unmap_memory",   crt_ok);
    ps3_hle_register_ctx(ps3_compute_nid("sys_mmapper_free_memory"),    "sys_mmapper_free_memory",    crt_ok);
}
