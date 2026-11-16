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
#include "../platform/win32_compat.h"
#include "../platform/ps3_sync.h"
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

#define LWM_OWNER  0x00
#define LWM_ATTR   0x08
#define LWM_RECUR  0x0C
#define LWM_QUEUE  0x10
#define LWM_FREE   0x00000000u
#define LWM_DEAD   0xFFFFFFFEu
#define LWM_SELF(ctx) ((uint32_t)(ctx)->thread_id ? (uint32_t)(ctx)->thread_id : 0x7FFFFFFEu)

#define CELL_OK_V        0u
#define CELL_EINVAL_V    0x80010002u
#define CELL_ESRCH_V     0x80010005u
#define CELL_EBUSY_V     0x8001000Au
#define CELL_ETIMEDOUT_V 0x8001000Bu
#define CELL_EDEADLK_V   0x80010008u
#define CELL_EPERM_V     0x80010009u
#define CELL_EKRESOURCE_V 0x80010011u

extern "C" void ppu_thread_note_block(uint64_t tid);

static inline uint64_t err(uint32_t e) { return (uint64_t)(int64_t)(int32_t)e; }

static inline volatile uint64_t* lwm_var(uint32_t lwm) { return (volatile uint64_t*)(vm_base + lwm); }
static inline uint32_t lwm_owner(uint64_t v)  { return __builtin_bswap32((uint32_t)v); }
static inline uint32_t lwm_waiter(uint64_t v) { return __builtin_bswap32((uint32_t)(v >> 32)); }
static inline uint64_t lwm_pack(uint32_t owner, uint32_t waiter)
{
    return (uint64_t)__builtin_bswap32(owner) | ((uint64_t)__builtin_bswap32(waiter) << 32);
}
static inline uint64_t lwm_load(uint32_t lwm) { return __atomic_load_n(lwm_var(lwm), __ATOMIC_ACQUIRE); }
static inline bool lwm_cas(uint32_t lwm, uint64_t* expected, uint64_t desired)
{
    return __atomic_compare_exchange_n(lwm_var(lwm), expected, desired, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}
static inline uint32_t lwm_protocol(uint32_t lwm)
{
    return (vm_read32(lwm + LWM_ATTR) & 0xFu) == 2u ? PS3_SYNC_PRIORITY : PS3_SYNC_FIFO;
}

static bool lwm_try_own(uint32_t lwm, uint32_t self)
{
    uint64_t v = lwm_load(lwm);
    while (lwm_owner(v) == LWM_FREE)
        if (lwm_cas(lwm, &v, lwm_pack(self, lwm_waiter(v)))) return true;
    return false;
}

static void lwm_add_waiter(uint32_t lwm, int32_t d)
{
    uint64_t v = lwm_load(lwm);
    while (!lwm_cas(lwm, &v, lwm_pack(lwm_owner(v), lwm_waiter(v) + (uint32_t)d))) {}
}

static bool lwm_own_or_queue(ps3_park_bucket* b, uint32_t lwm, ps3_waiter* w)
{
    uint64_t v = lwm_load(lwm);
    for (;;) {
        if (lwm_owner(v) == LWM_FREE) {
            if (lwm_cas(lwm, &v, lwm_pack((uint32_t)w->tid, lwm_waiter(v)))) return true;
        } else if (lwm_cas(lwm, &v, lwm_pack(lwm_owner(v), lwm_waiter(v) + 1))) {
            w->key = lwm;
            ps3_waitq_push(&b->q, w);
            return false;
        }
    }
}

static ps3_waiter* lwm_release_locked(ps3_park_bucket* b, uint32_t lwm)
{
    ps3_waiter* w = ps3_waitq_pop(&b->q, lwm_protocol(lwm), lwm);
    uint64_t v = lwm_load(lwm);
    if (w) while (!lwm_cas(lwm, &v, lwm_pack((uint32_t)w->tid, lwm_waiter(v) - 1))) {}
    else   while (!lwm_cas(lwm, &v, lwm_pack(LWM_FREE, lwm_waiter(v)))) {}
    return w;
}

static void lwm_release(uint32_t lwm, uint32_t self)
{
    uint64_t v = lwm_pack(self, 0);
    if (lwm_cas(lwm, &v, lwm_pack(LWM_FREE, 0))) return;
    ps3_park_bucket* b = ps3_park_bucket_of(lwm);
    ps3_lock_acquire(&b->lock);
    ps3_waiter* w = lwm_release_locked(b, lwm);
    ps3_lock_release(&b->lock);
    if (w) ps3_waiter_wake(w, 0);
}

static uint64_t lwm_owned_again(uint32_t lwm)
{
    if (vm_read32(lwm + LWM_ATTR) & 0x20u) return err(CELL_EDEADLK_V);
    const uint32_t rc = vm_read32(lwm + LWM_RECUR);
    if (rc == 0xFFFFFFFFu) return err(CELL_EKRESOURCE_V);
    vm_write32(lwm + LWM_RECUR, rc + 1);
    return CELL_OK_V;
}

static void sys_lwmutex_create(ppu_context* ctx)
{
    static uint32_t s_queue_id = 0x4D000000u;
    const uint32_t lwm  = (uint32_t)ctx->gpr[3];
    const uint32_t attr = (uint32_t)ctx->gpr[4];
    const uint32_t protocol  = vm_read32(attr + 0);
    const uint32_t recursive = vm_read32(attr + 4);
    if ((recursive != 0x10u && recursive != 0x20u) || (protocol != 1u && protocol != 2u && protocol != 4u)) {
        ctx->gpr[3] = err(CELL_EINVAL_V);
        return;
    }
    __atomic_store_n(lwm_var(lwm), lwm_pack(LWM_FREE, 0), __ATOMIC_RELEASE);
    vm_write32(lwm + LWM_ATTR, protocol | recursive);
    vm_write32(lwm + LWM_RECUR, 0);
    vm_write32(lwm + LWM_QUEUE, __atomic_add_fetch(&s_queue_id, 1u, __ATOMIC_RELAXED));
    vm_write32(lwm + 0x14, 0);
    ctx->gpr[3] = CELL_OK_V;
}

static void sys_lwmutex_destroy(ppu_context* ctx)
{
    const uint32_t lwm = (uint32_t)ctx->gpr[3];
    uint64_t v = lwm_pack(LWM_FREE, 0);
    if (lwm_cas(lwm, &v, lwm_pack(LWM_DEAD, 0))) { ctx->gpr[3] = CELL_OK_V; return; }
    ctx->gpr[3] = lwm_owner(v) == LWM_DEAD ? err(CELL_EINVAL_V) : err(CELL_EBUSY_V);
}

static int sys_lwmutex_lock_fast(ppu_context* ctx)
{
    const uint32_t lwm = (uint32_t)ctx->gpr[3];
    const uint32_t self = LWM_SELF(ctx);
    if (lwm_try_own(lwm, self)) { ctx->gpr[3] = CELL_OK_V; return 1; }
    const uint32_t owner = lwm_owner(lwm_load(lwm));
    if (owner == self) { ctx->gpr[3] = lwm_owned_again(lwm); return 1; }
    if (owner == LWM_DEAD) { ctx->gpr[3] = err(CELL_EINVAL_V); return 1; }
    return 0;
}

static void sys_lwmutex_lock(ppu_context* ctx)
{
    if (sys_lwmutex_lock_fast(ctx)) return;
    const uint32_t lwm = (uint32_t)ctx->gpr[3];
    const uint64_t deadline = ps3_sync_deadline_us(ctx->gpr[4]);
    ps3_waiter w;
    ps3_waiter_init(&w, LWM_SELF(ctx), lwm);
    ppu_thread_note_block(ctx->thread_id);
    ps3_park_bucket* b = ps3_park_bucket_of(lwm);
    ps3_lock_acquire(&b->lock);
    const bool owned = lwm_own_or_queue(b, lwm, &w);
    ps3_lock_release(&b->lock);
    if (owned || ps3_waiter_sleep(&w, deadline)) { ctx->gpr[3] = CELL_OK_V; return; }
    ps3_lock_acquire(&b->lock);
    const bool removed = ps3_waitq_remove(&b->q, &w) != 0;
    if (removed) lwm_add_waiter(lwm, -1);
    ps3_lock_release(&b->lock);
    if (!removed) ps3_waiter_sleep(&w, 0);
    ctx->gpr[3] = removed ? err(CELL_ETIMEDOUT_V) : CELL_OK_V;
}

static void sys_lwmutex_trylock(ppu_context* ctx)
{
    const uint32_t lwm = (uint32_t)ctx->gpr[3];
    const uint32_t self = LWM_SELF(ctx);
    if (lwm_try_own(lwm, self)) { ctx->gpr[3] = CELL_OK_V; return; }
    const uint32_t owner = lwm_owner(lwm_load(lwm));
    if (owner == self) ctx->gpr[3] = lwm_owned_again(lwm);
    else if (owner == LWM_DEAD) ctx->gpr[3] = err(CELL_EINVAL_V);
    else ctx->gpr[3] = err(CELL_EBUSY_V);
}

static void sys_lwmutex_unlock(ppu_context* ctx)
{
    const uint32_t lwm = (uint32_t)ctx->gpr[3];
    const uint32_t self = LWM_SELF(ctx);
    if (lwm_owner(lwm_load(lwm)) != self) { ctx->gpr[3] = err(CELL_EPERM_V); return; }
    const uint32_t rc = vm_read32(lwm + LWM_RECUR);
    if (rc) vm_write32(lwm + LWM_RECUR, rc - 1);
    else lwm_release(lwm, self);
    ctx->gpr[3] = CELL_OK_V;
}

static int sys_lwmutex_unlock_fast(ppu_context* ctx)
{
    sys_lwmutex_unlock(ctx);
    return 1;
}

static void sys_lwcond_create(ppu_context* ctx)
{
    static uint32_t s_lwcond_id = 0x4C000000u;
    const uint32_t lwcond = (uint32_t)ctx->gpr[3];
    vm_write32(lwcond + 0x00, (uint32_t)ctx->gpr[4]);
    vm_write32(lwcond + 0x04, __atomic_add_fetch(&s_lwcond_id, 1u, __ATOMIC_RELAXED));
    ctx->gpr[3] = CELL_OK_V;
}

static void sys_lwcond_destroy(ppu_context* ctx)
{
    const uint32_t lwcond = (uint32_t)ctx->gpr[3];
    ps3_park_bucket* b = ps3_park_bucket_of(lwcond);
    ps3_lock_acquire(&b->lock);
    const bool busy = ps3_waitq_has(&b->q, lwcond) != 0;
    if (!busy) vm_write32(lwcond + 0x04, LWM_DEAD);
    ps3_lock_release(&b->lock);
    ctx->gpr[3] = busy ? err(CELL_EBUSY_V) : CELL_OK_V;
}

enum { LWCOND_ONE, LWCOND_ALL, LWCOND_TO };

static uint64_t lwcond_signal(uint32_t lwcond, int how, uint64_t tid)
{
    const uint32_t lwm = vm_read32(lwcond + 0x00);
    ps3_park_bucket* bc = ps3_park_bucket_of(lwcond);
    ps3_park_bucket* bm = ps3_park_bucket_of(lwm);
    ps3_waiter* wake = nullptr;
    bool any = false;
    ps3_park_lock2(bc, bm);
    for (;;) {
        ps3_waiter* w = how == LWCOND_TO ? ps3_waitq_pop_tid(&bc->q, lwcond, tid)
                                         : ps3_waitq_pop(&bc->q, how == LWCOND_ALL ? PS3_SYNC_PRIORITY : lwm_protocol(lwm), lwcond);
        if (!w) break;
        any = true;
        if (lwm_own_or_queue(bm, lwm, w)) { w->next = wake; wake = w; }
        if (how != LWCOND_ALL) break;
    }
    ps3_park_unlock2(bc, bm);
    while (wake) { ps3_waiter* n = wake->next; ps3_waiter_wake(wake, 0); wake = n; }
    return (how == LWCOND_TO && !any) ? err(CELL_EPERM_V) : CELL_OK_V;
}

static void sys_lwcond_signal(ppu_context* ctx)     { ctx->gpr[3] = lwcond_signal((uint32_t)ctx->gpr[3], LWCOND_ONE, 0); }
static void sys_lwcond_signal_all(ppu_context* ctx) { ctx->gpr[3] = lwcond_signal((uint32_t)ctx->gpr[3], LWCOND_ALL, 0); }
static void sys_lwcond_signal_to(ppu_context* ctx)
{
    const uint64_t tid = (uint32_t)ctx->gpr[4];
    ctx->gpr[3] = tid == 0xFFFFFFFFu ? lwcond_signal((uint32_t)ctx->gpr[3], LWCOND_ONE, 0)
                                     : lwcond_signal((uint32_t)ctx->gpr[3], LWCOND_TO, tid);
}

static void sys_lwcond_wait(ppu_context* ctx)
{
    const uint32_t lwcond = (uint32_t)ctx->gpr[3];
    const uint64_t deadline = ps3_sync_deadline_us(ctx->gpr[4]);
    const uint32_t lwm = vm_read32(lwcond + 0x00);
    const uint32_t self = LWM_SELF(ctx);
    if (lwm_owner(lwm_load(lwm)) != self) { ctx->gpr[3] = err(CELL_EPERM_V); return; }

    const uint32_t recursion = vm_read32(lwm + LWM_RECUR);
    ps3_waiter w;
    ps3_waiter_init(&w, self, lwcond);
    ppu_thread_note_block(ctx->thread_id);
    ps3_park_bucket* bc = ps3_park_bucket_of(lwcond);
    ps3_park_bucket* bm = ps3_park_bucket_of(lwm);

    ps3_park_lock2(bc, bm);
    ps3_waitq_push(&bc->q, &w);
    vm_write32(lwm + LWM_RECUR, 0);
    ps3_waiter* next_owner = lwm_release_locked(bm, lwm);
    ps3_park_unlock2(bc, bm);
    if (next_owner) ps3_waiter_wake(next_owner, 0);

    uint64_t rc = CELL_OK_V;
    if (!ps3_waiter_sleep(&w, deadline)) {
        ps3_park_lock2(bc, bm);
        bool owned = false;
        if (w.key == lwcond) {
            ps3_waitq_remove(&bc->q, &w);
            rc = err(CELL_ETIMEDOUT_V);
            owned = lwm_own_or_queue(bm, lwm, &w);
        }
        ps3_park_unlock2(bc, bm);
        if (!owned) ps3_waiter_sleep(&w, 0);
    }
    vm_write32(lwm + LWM_RECUR, recursion);
    ctx->gpr[3] = rc;
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
    const uint64_t us = tv ? vm_read64(tv) * 1000000ull + vm_read64(tv + 8) : 0;
    if (!tv || us) {
        volatile uint32_t never = 0;
        const uint64_t deadline = tv ? ps3_sync_deadline_us(us) : 0;
        while (ps3_futex_wait(&never, 0u, deadline)) {}
    }
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
    ps3_hle_register_ctx(ps3_compute_nid("sys_lwmutex_destroy"), "sys_lwmutex_destroy", sys_lwmutex_destroy);
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
    ps3_hle_register_ctx(ps3_compute_nid("sys_lwcond_signal_to"),  "sys_lwcond_signal_to",  sys_lwcond_signal_to);

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
