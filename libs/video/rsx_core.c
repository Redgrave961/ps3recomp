#include "rsx_core.h"
#include "rsx_transfer.h"
#include "../../runtime/ppu/ppu_memory.h"
#include "../../runtime/memory/vm_page_map.h"

#ifdef _WIN32
#include <windows.h>
#else
#include "../../runtime/platform/win32_compat.h"
#include <time.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RSX_TB_FREQ        79800000ull
#define RSX_NO_RETURN      0xFFFFFFFFu
#define RSX_IRQ_RING       1024u
#define RSX_VBLANK_GROUP   60u

extern uint64_t ppu_timebase_now(void);
uint32_t ppu_hle_inject_base = 0x20000000u;
extern void (*g_rsx_window_write)(void);
struct ppu_context;
struct ppu_context* ppu_thread_adopt(const char* name, uint32_t stack_size, int32_t priority);

typedef struct rsx_irq {
    u32 event;
    u32 arg;
} rsx_irq;

static struct {
    u32 local_size;
    u32 main_size;
    u32 io_ea[RSX_IO_PAGES];
    u16 ea_io[4096];
    const rsx_renderer* renderer;
    rsx_event_sink sink;
    volatile LONG started;

    volatile u32 bell;
    volatile u32 get_epoch;
    volatile u32 get_waiters;
    volatile u32 label_epoch;
    volatile u32 label_waiters;
    volatile u32 immediate_flip;
    u32 get;
    u32 ret;
    u32 method_reg;
    u32 method_inc;
    u32 method_left;
    u32 fault_get;

    u32 sema406e_ctx;
    u32 sema406e_off;
    u32 sema4097_ctx;
    u32 sema4097_off;
    u32 report_ctx;
    u32 notify_ctx;
    u64 timestamp_last;

    rsx_display_buffer display[8];
    volatile u32 display_mask;
    volatile u32 vsync;
    volatile u32 flip_status;
    volatile u64 last_flip_time;
    volatile u32 frames;
    int free_flips;
    volatile u64 frame_period;
    volatile u64 hidden_period;
    u64 frame_due;
    HANDLE frame_timer;
    volatile u64 progress;
    u32 put_seen;
    u64 batch;

    SRWLOCK vblank_lock;
    CONDITION_VARIABLE vblank_cv;
    volatile u64 vblank_count;

    SRWLOCK irq_lock;
    CONDITION_VARIABLE irq_cv;
    rsx_irq irq[RSX_IRQ_RING];
    u32 irq_head;
    u32 irq_tail;
    u32 irq_vblank_queued;
} s;

#define RSX_3D_REGS (0x2000u >> 2)
static u16 s_next_core[RSX_3D_REGS + 1u];

static inline u32 rsx_load(u32 ea)
{
    return __builtin_bswap32(__atomic_load_n((const u32*)(vm_base + ea), __ATOMIC_ACQUIRE));
}

static inline void rsx_store_control(u32 ea, u32 value)
{
    __atomic_store_n((u32*)(vm_base + ea), __builtin_bswap32(value), __ATOMIC_RELEASE);
}

static void rsx_publish_get(void)
{
    rsx_store_control(RSX_GET_EA, s.get);
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    if (__atomic_load_n(&s.get_waiters, __ATOMIC_RELAXED)) {
        __atomic_add_fetch(&s.get_epoch, 1u, __ATOMIC_RELEASE);
        WakeByAddressAll((PVOID)&s.get_epoch);
    }
}

void rsx_core_wait_get_outside(u32 lo, u32 hi)
{
    __atomic_add_fetch(&s.get_waiters, 1u, __ATOMIC_ACQ_REL);
    FlushProcessWriteBuffers();
    for (;;) {
        u32 seen = __atomic_load_n(&s.get_epoch, __ATOMIC_ACQUIRE);
        const u32 get = rsx_load(RSX_GET_EA);
        if (get < lo || get > hi) break;
        WaitOnAddress((volatile VOID*)&s.get_epoch, &seen, sizeof seen, INFINITE);
    }
    __atomic_sub_fetch(&s.get_waiters, 1u, __ATOMIC_ACQ_REL);
}

static void rsx_label_note(void)
{
    __atomic_add_fetch(&s.label_epoch, 1u, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&s.label_waiters, __ATOMIC_SEQ_CST)) WakeByAddressAll((PVOID)&s.label_epoch);
}

void rsx_core_wait_label(u32 ea, u32 value)
{
    __atomic_add_fetch(&s.label_waiters, 1u, __ATOMIC_SEQ_CST);
    for (;;) {
        u32 seen = __atomic_load_n(&s.label_epoch, __ATOMIC_SEQ_CST);
        if (rsx_load(ea) != value) break;
        WaitOnAddress((volatile VOID*)&s.label_epoch, &seen, sizeof seen, INFINITE);
    }
    __atomic_sub_fetch(&s.label_waiters, 1u, __ATOMIC_SEQ_CST);
}

static void rsx_store_label(u32 ea, u32 value)
{
    __atomic_add_fetch(&s.progress, 1u, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    vm_write32(ea, value);
    rsx_label_note();
}

static u64 rsx_tb_scale(u64 tb, u64 unit)
{
    return (tb / RSX_TB_FREQ) * unit + (tb % RSX_TB_FREQ) * unit / RSX_TB_FREQ;
}

u64 rsx_core_system_time(void)
{
    return rsx_tb_scale(ppu_timebase_now(), 1000000ull);
}

u64 rsx_core_timestamp(void)
{
    const u64 t = rsx_tb_scale(ppu_timebase_now(), 1000000000ull);
    u64 last = __atomic_load_n(&s.timestamp_last, __ATOMIC_RELAXED), next;
    do next = t > last ? t : last + 1u;
    while (!__atomic_compare_exchange_n(&s.timestamp_last, &last, next, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
    return next;
}

static void rsx_fault(const char* what, u32 at, u32 word)
{
    if (s.fault_get == at) return;
    s.fault_get = at;
    fprintf(stderr, "[rsx] FIFO fault: %s at get=0x%08X (word 0x%08X, put=0x%08X)\n",
            what, at, word, rsx_load(RSX_PUT_EA));
}

static void rsx_core_window_written(void)
{
    rsx_label_note();
    rsx_core_doorbell();
}

void rsx_core_doorbell(void)
{
    __atomic_add_fetch(&s.bell, 1u, __ATOMIC_RELEASE);
    WakeByAddressSingle((PVOID)&s.bell);
}

static void rsx_wait_bell(u32 seen)
{
    while (__atomic_load_n(&s.bell, __ATOMIC_ACQUIRE) == seen)
        WaitOnAddress((volatile VOID*)&s.bell, &seen, sizeof seen, INFINITE);
}

static void rsx_irq_push(enum rsx_event event, u32 arg)
{
    AcquireSRWLockExclusive(&s.irq_lock);
    if (event == RSX_EVENT_VBLANK && s.irq_vblank_queued) {
        ReleaseSRWLockExclusive(&s.irq_lock);
        return;
    }
    if (s.irq_tail - s.irq_head == RSX_IRQ_RING) {
        ReleaseSRWLockExclusive(&s.irq_lock);
        fprintf(stderr, "[rsx] interrupt queue full, event %u dropped\n", (unsigned)event);
        return;
    }
    s.irq[s.irq_tail % RSX_IRQ_RING] = (rsx_irq){ (u32)event, arg };
    s.irq_tail++;
    if (event == RSX_EVENT_VBLANK) s.irq_vblank_queued = 1;
    ReleaseSRWLockExclusive(&s.irq_lock);
    WakeConditionVariable(&s.irq_cv);
}

void rsx_core_raise(enum rsx_event event, u32 arg)
{
    if (s.sink) rsx_irq_push(event, arg);
}

static DWORD WINAPI rsx_irq_thread(LPVOID unused)
{
    (void)unused;
    if (!ppu_thread_adopt("_gcm_intr_thread", 0x4000u, 1)) fprintf(stderr, "[rsx] no guest thread slot for the interrupt thread\n");
    for (;;) {
        AcquireSRWLockExclusive(&s.irq_lock);
        while (s.irq_head == s.irq_tail)
            SleepConditionVariableSRW(&s.irq_cv, &s.irq_lock, INFINITE, 0);
        const rsx_irq e = s.irq[s.irq_head % RSX_IRQ_RING];
        s.irq_head++;
        if (e.event == RSX_EVENT_VBLANK) s.irq_vblank_queued = 0;
        ReleaseSRWLockExclusive(&s.irq_lock);
        s.sink((enum rsx_event)e.event, e.arg);
    }
}

#ifdef _WIN32
static LARGE_INTEGER s_qpc_freq;

static u64 rsx_ticks(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (u64)t.QuadPart;
}

static u64 rsx_tick_freq(void)
{
    if (!s_qpc_freq.QuadPart) QueryPerformanceFrequency(&s_qpc_freq);
    return (u64)s_qpc_freq.QuadPart;
}

static void rsx_sleep_until(HANDLE timer, u64 target)
{
    const u64 freq = rsx_tick_freq(), slack = freq / 1000u, spin = freq / 20000u;
    u64 now = rsx_ticks();
    if (now >= target) return;
    if (target - now > slack) {
        const u64 left = target - now - slack;
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)((left / freq) * 10000000ull + (left % freq) * 10000000ull / freq);
        if (due.QuadPart && SetWaitableTimerEx(timer, &due, 0, NULL, NULL, NULL, 0)) WaitForSingleObject(timer, INFINITE);
    }
    while ((now = rsx_ticks()) < target) {
        if (target - now > spin) SwitchToThread();
        else YieldProcessor();
    }
}

static HANDLE rsx_timer_create(void)
{
    return CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
}
#else
static u64 rsx_ticks(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

static u64 rsx_tick_freq(void) { return 1000000000ull; }

static void rsx_sleep_until(HANDLE timer, u64 target)
{
    (void)timer;
    struct timespec ts = { (time_t)(target / 1000000000ull), (long)(target % 1000000000ull) };
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) != 0) {}
}

static HANDLE rsx_timer_create(void) { return NULL; }
#endif

static rsx_vblank_clock s_clock;
static void* s_clock_user;

void rsx_core_set_vblank_clock(rsx_vblank_clock clock, void* user)
{
    __atomic_store_n(&s_clock_user, user, __ATOMIC_RELEASE);
    __atomic_store_n(&s_clock, clock, __ATOMIC_RELEASE);
}

static DWORD WINAPI rsx_vblank_thread(LPVOID unused)
{
    (void)unused;
    HANDLE timer = rsx_timer_create();
    const u64 group = rsx_tick_freq() * 1001ull / 1000ull;
    u64 base = rsx_ticks();
    for (u32 n = 1;; n++) {
        const rsx_vblank_clock clock = __atomic_load_n(&s_clock, __ATOMIC_ACQUIRE);
        if (clock && clock(__atomic_load_n(&s_clock_user, __ATOMIC_ACQUIRE))) {
            base = rsx_ticks();
            n = 0;
        } else {
            if (n > RSX_VBLANK_GROUP) {
                base += group;
                n = 1;
            }
            if (base + group * n / RSX_VBLANK_GROUP + group < rsx_ticks()) {
                base = rsx_ticks();
                n = 1;
            }
            rsx_sleep_until(timer, base + group * n / RSX_VBLANK_GROUP);
        }
        AcquireSRWLockExclusive(&s.vblank_lock);
        const u64 count = ++s.vblank_count;
        ReleaseSRWLockExclusive(&s.vblank_lock);
        WakeAllConditionVariable(&s.vblank_cv);
        if (s.renderer && s.renderer->vblank) s.renderer->vblank(s.renderer->user, count);
        rsx_core_raise(RSX_EVENT_VBLANK, 1);
    }
}

static void rsx_wait_vblank(void)
{
    AcquireSRWLockExclusive(&s.vblank_lock);
    const u64 seen = s.vblank_count;
    while (s.vblank_count == seen)
        SleepConditionVariableSRW(&s.vblank_cv, &s.vblank_lock, INFINITE, 0);
    ReleaseSRWLockExclusive(&s.vblank_lock);
}

static void rsx_sync(void)
{
    __atomic_add_fetch(&s.progress, 1u, __ATOMIC_RELAXED);
}

static void rsx_idle(void)
{
    if (s.renderer && s.renderer->sync) s.renderer->sync(s.renderer->user);
}

static void rsx_flush(void)
{
    if (s.renderer && s.renderer->flush) s.renderer->flush(s.renderer->user);
}

static void rsx_frame_limit(void)
{
    const u64 chosen = __atomic_load_n(&s.frame_period, __ATOMIC_ACQUIRE);
    const u64 idle = __atomic_load_n(&s.hidden_period, __ATOMIC_ACQUIRE);
    const u64 period = chosen > idle ? chosen : idle;
    if (!period) return;
    if (!s.frame_timer) s.frame_timer = rsx_timer_create();
    const u64 now = rsx_ticks();
    s.frame_due = s.frame_due + 2u * period < now ? now : s.frame_due + period;
    rsx_sleep_until(s.frame_timer, s.frame_due);
}

static void rsx_scanout(u32 buffer)
{
    if (s.renderer && s.renderer->present && s.renderer->present(s.renderer->user, buffer, &s.display[buffer]))
        rsx_frame_limit();
}

static void rsx_flip_done(u32 buffer)
{
    __atomic_add_fetch(&s.progress, 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&s.last_flip_time, rsx_core_system_time(), __ATOMIC_RELEASE);
    __atomic_store_n(&s.flip_status, 0u, __ATOMIC_RELEASE);
    __atomic_add_fetch(&s.frames, 1u, __ATOMIC_RELEASE);
    rsx_core_raise(RSX_EVENT_FLIP, 1);
}

static void rsx_present(u32 buffer)
{
    rsx_scanout(buffer);
    rsx_flip_done(buffer);
}

static void rsx_flip(u32 buffer)
{
    buffer &= 7u;
    rsx_core_raise(RSX_EVENT_QUEUE, 1);
    s.sema406e_ctx = RSX_DMA_SEMAPHORE_R;
    s.sema406e_off = 0x10u;
    s.notify_ctx = 0x66604200u;
    s.sema4097_ctx = RSX_DMA_SEMAPHORE_RW;
    s.report_ctx = RSX_DMA_REPORT_LOCAL;
    if (s.renderer && s.renderer->reset) s.renderer->reset(s.renderer->user);
    rsx_scanout(buffer);
    if (s.vsync) rsx_wait_vblank();
    rsx_flip_done(buffer);
    for (u32 i = 0; i < 16u; i += 4u) rsx_store_label(RSX_SEMAPHORE_EA + 0x10u + i, 0);
}

static void rsx_service_immediate(void)
{
    if (__atomic_load_n(&s.immediate_flip, __ATOMIC_RELAXED))
        rsx_present(__atomic_exchange_n(&s.immediate_flip, 0u, __ATOMIC_ACQ_REL) - 1u);
}

void rsx_core_flip_immediate(u32 buffer)
{
    __atomic_store_n(&s.immediate_flip, (buffer & 7u) + 1u, __ATOMIC_RELEASE);
    rsx_core_doorbell();
}

static void rsx_watch(u32 ea)
{
    vm_watch(ea);
    FlushProcessWriteBuffers();
}

static void rsx_semaphore_acquire(u32 value)
{
    s.batch++;
    const u32 ea = rsx_address(s.sema406e_ctx, s.sema406e_off, 4);
    if (!ea) {
        rsx_fault("semaphore acquire address", s.get, s.sema406e_off);
        return;
    }
    if (rsx_load(ea) == value) return;
    rsx_publish_get();
    rsx_flush();
    rsx_watch(ea);
    for (;;) {
        const u32 seen = __atomic_load_n(&s.bell, __ATOMIC_ACQUIRE);
        rsx_service_immediate();
        if (rsx_load(ea) == value) break;
        WaitOnAddress((volatile VOID*)&s.bell, &seen, sizeof seen, 1);
    }
    vm_unwatch(ea);
}

static void rsx_label_retire(const void* arg)
{
    const u32* label = (const u32*)arg;
    rsx_store_label(label[0], label[1]);
    rsx_core_doorbell();
}

static void rsx_semaphore_release(u32 ctx, u32 off, u32 align, u32 value, int back_end)
{
    if (off % align) {
        rsx_fault("unaligned semaphore release", s.get, off);
        return;
    }
    const u32 ea = rsx_address(ctx, off, 4);
    if (!ea) {
        rsx_fault("semaphore release address", s.get, off);
        return;
    }
    const u32 label[2] = { ea, value };
    const rsx_write w = { ea, 4u, 0, 0, 1, rsx_label_retire, label, sizeof label };
    if (!back_end || !rsx_core_defer(&w)) rsx_store_label(ea, value);
}

static void rsx_notify_retire(const void* arg)
{
    const u32 ea = *(const u32*)arg;
    rsx_sync();
    vm_write64(ea, rsx_core_timestamp());
    vm_write64(ea + 8u, 0);
}

static void rsx_notify(void)
{
    if ((s.notify_ctx & ~7u) != (RSX_DMA_NOTIFY_MAIN_0 & ~7u)) return;
    const u32 ea = rsx_io_to_ea(0x0F100000u + ((s.notify_ctx & 7u) ^ 7u) * 0x40u);
    if (!ea) {
        rsx_fault("notify area unmapped", s.get, s.notify_ctx);
        return;
    }
    const rsx_write w = { ea, 16u, 0, 0, 1, rsx_notify_retire, &ea, sizeof ea };
    if (!rsx_core_defer(&w)) rsx_notify_retire(&ea);
}

static void rsx_get_report(u32 arg)
{
    const u32 type = arg >> 24, offset = arg & 0xFFFFFFu;
    const u32 ea = rsx_address(s.report_ctx, offset, 16);
    if (!ea) {
        rsx_fault("report address", s.get, arg);
        return;
    }
    if (s.renderer && s.renderer->report && s.renderer->report(s.renderer->user, type, ea)) return;
    vm_write64(ea, rsx_core_timestamp());
    vm_write32(ea + 8u, 0);
    vm_write32(ea + 12u, 0);
}

static void rsx_core_method(u32 reg, u32 value)
{
    switch (reg) {
    case 0x0050u:
        rsx_idle();
        rsx_sync();
        rsx_store_control(RSX_REF_EA, value);
        return;
    case 0x0060u: s.sema406e_ctx = value; return;
    case 0x0064u: s.sema406e_off = value; return;
    case 0x0068u: rsx_semaphore_acquire(value); return;
    case 0x006Cu: rsx_semaphore_release(s.sema406e_ctx, s.sema406e_off, 4, value, 0); return;
    case 0x0104u: rsx_notify(); return;
    case 0x0110u:
        rsx_idle();
        rsx_sync();
        return;
    case 0x0180u: s.notify_ctx = value; return;
    case 0x01A4u: s.sema4097_ctx = value; return;
    case 0x01A8u: s.report_ctx = value; return;
    case 0x1D6Cu: s.sema4097_off = value; return;
    case 0x1D70u:
        rsx_semaphore_release(s.sema4097_ctx, s.sema4097_off, 16,
                              (value & 0xFF00FF00u) | ((value & 0xFFu) << 16) | ((value >> 16) & 0xFFu), 1);
        return;
    case 0x1D74u: rsx_semaphore_release(s.sema4097_ctx, s.sema4097_off, 16, value, 1); return;
    case 0x1800u: rsx_get_report(value); return;
    }
}

static void rsx_driver_method(u32 reg, u32 value)
{
    switch (reg) {
    case 0xFEACu: rsx_flip(value); return;
    case 0xEB00u:
        rsx_sync();
        rsx_core_raise(RSX_EVENT_USER, value);
        return;
    case 0xE920u: case 0xE924u: rsx_flip(value); return;
    case 0xE000u: return;
    }
    if (reg >= 0xE940u && reg <= 0xE95Cu) {
        rsx_core_raise(RSX_EVENT_QUEUE, (reg - 0xE940u) >> 2);
        return;
    }
    rsx_fault("unknown driver method", s.get, reg);
}

static void rsx_3d_methods(u32 reg, const u32* be, u32 n)
{
    const rsx_renderer* r = s.renderer;
    const int render = r && r->methods;
    const u32 first = reg >> 2;
    u32 start = 0;
    for (u32 j = s_next_core[first] - first; j < n; j = s_next_core[first + j + 1u] - first) {
        if (render && j > start) r->methods(r->user, reg + 4u * start, 4u, be + start, j - start);
        rsx_core_method(reg + 4u * j, __builtin_bswap32(be[j]));
        start = j + 1u;
    }
    if (render && n > start) r->methods(r->user, reg + 4u * start, 4u, be + start, n - start);
}

static u32 rsx_dispatch(u32 reg, u32 inc, const u32* be, u32 n)
{
    if (reg < 0x2000u) {
        if (inc == 0) {
            if (s_next_core[reg >> 2] == reg >> 2) {
                for (u32 i = 0; i < n; i++) rsx_core_method(reg, __builtin_bswap32(be[i]));
            } else if (s.renderer && s.renderer->methods) {
                s.renderer->methods(s.renderer->user, reg, 0, be, n);
            }
            return n;
        }
        const u32 room = (0x2000u - reg) >> 2;
        const u32 k = n < room ? n : room;
        rsx_3d_methods(reg, be, k);
        return k;
    }
    if (reg >= 0xE000u) {
        rsx_driver_method(reg, __builtin_bswap32(be[0]));
        return 1;
    }
    return rsx_transfer_methods(reg, inc, be, n);
}

static void rsx_run_args(u32 put)
{
    while (s.method_left && s.get != put) {
        const u32 ea = rsx_io_to_ea(s.get);
        if (!ea) {
            rsx_fault("arguments unmapped", s.get, s.method_reg);
            return;
        }
        u32 n = s.method_left;
        const u32 page = (0x100000u - (s.get & 0xFFFFFu)) >> 2;
        if (n > page) n = page;
        if (put > s.get && (put - s.get) >> 2 < n) n = (put - s.get) >> 2;
        const u32* be = (const u32*)(vm_base + ea);
        u32 done = 0;
        while (done < n) {
            const u32 k = rsx_dispatch(s.method_reg, s.method_inc, be + done, n - done);
            s.method_reg += s.method_inc * k;
            done += k;
        }
        s.get += n << 2;
        s.method_left -= n;
    }
    rsx_publish_get();
}

static void rsx_park(u32 jump, u32 seen)
{
    s.batch++;
    const u32 ea = rsx_io_to_ea(s.get);
    rsx_publish_get();
    rsx_flush();
    rsx_watch(ea);
    for (;;) {
        rsx_service_immediate();
        if (rsx_load(ea) != jump || __atomic_load_n(&s.bell, __ATOMIC_ACQUIRE) != seen) break;
        WaitOnAddress((volatile VOID*)&s.bell, &seen, sizeof seen, 1);
    }
    vm_unwatch(ea);
}

static void rsx_control(u32 cmd, u32 seen)
{
    u32 target;
    if ((cmd & 0xE0000003u) == 0x20000000u) target = cmd & 0x1FFFFFFCu;
    else if ((cmd & 0xE0000003u) == 0x00000001u) target = cmd & 0xFFFFFFFCu;
    else if ((cmd & 0x00000003u) == 0x00000002u) {
        if (s.ret != RSX_NO_RETURN) {
            rsx_fault("nested CALL", s.get, cmd);
            rsx_wait_bell(seen);
            return;
        }
        s.ret = s.get + 4u;
        s.get = cmd & 0x1FFFFFFCu;
        rsx_publish_get();
        return;
    } else if ((cmd & 0xFFFF0003u) == 0x00020000u) {
        if (s.ret == RSX_NO_RETURN) {
            rsx_fault("RETURN without CALL", s.get, cmd);
            rsx_wait_bell(seen);
            return;
        }
        s.get = s.ret;
        s.ret = RSX_NO_RETURN;
        rsx_publish_get();
        return;
    } else {
        rsx_fault("invalid command", s.get, cmd);
        rsx_wait_bell(seen);
        return;
    }
    if (target == s.get) {
        rsx_park(cmd, seen);
        return;
    }
    s.get = target;
    rsx_publish_get();
}

static DWORD WINAPI rsx_fifo_thread(LPVOID unused)
{
    (void)unused;
    if (s.renderer && s.renderer->open && s.renderer->open(s.renderer->user) != 0) {
        fprintf(stderr, "[rsx] renderer failed to open\n");
        fflush(NULL);
#ifdef _WIN32
        MessageBoxA(NULL, "The Vulkan renderer failed to start. The console log has the reason.", "Renderer error", MB_OK | MB_ICONERROR);
#endif
        _Exit(1);
    }
    for (;;) {
        const u32 seen = __atomic_load_n(&s.bell, __ATOMIC_ACQUIRE);
        rsx_service_immediate();
        const u32 put = rsx_load(RSX_PUT_EA) & ~3u;
        if (put != s.put_seen) {
            s.put_seen = put;
            s.batch++;
        }
        if (s.get == put) {
            rsx_flush();
            rsx_wait_bell(seen);
            continue;
        }
        if (s.method_left) {
            rsx_run_args(put);
            continue;
        }
        const u32 ea = rsx_io_to_ea(s.get);
        if (!ea) {
            rsx_fault("get unmapped", s.get, 0);
            rsx_wait_bell(seen);
            continue;
        }
        const u32 cmd = rsx_load(ea);
        if (cmd & 0xA0030003u) {
            rsx_control(cmd, seen);
            continue;
        }
        const u32 count = (cmd >> 18) & 0x7FFu;
        s.get += 4u;
        if (!count) {
            rsx_publish_get();
            continue;
        }
        s.method_reg = cmd & 0xFFFCu;
        s.method_inc = (cmd & 0x40000000u) ? 0u : 4u;
        s.method_left = count;
        rsx_run_args(put);
    }
}

static void rsx_window_clear(void)
{
    memset(vm_base + RSX_SEMAPHORE_EA, 0, 0xA000u);
    memset(vm_base + RSX_CONTROL_EA, 0, 0x10u);
    memset(vm_base + RSX_IO_TABLE_EA, 0xFF, 0x2000u);
    memset(vm_base + RSX_EA_TABLE_EA, 0xFF, RSX_IO_PAGES * 2u);
}

void rsx_core_init(u32 local_size, u32 main_size)
{
    if (s.started) return;
    s.local_size = local_size;
    s.main_size = main_size;
    for (u32 i = 0; i < RSX_IO_PAGES; i++) s.io_ea[i] = 0;
    memset(s.ea_io, 0xFF, sizeof s.ea_io);
    s.get = 0;
    s.ret = RSX_NO_RETURN;
    s.method_left = 0;
    s.fault_get = 0xFFFFFFFFu;
    s.sema406e_ctx = RSX_DMA_SEMAPHORE_R;
    s.sema406e_off = 0x30u;
    s.notify_ctx = 0x66604200u;
    s.sema4097_ctx = RSX_DMA_SEMAPHORE_RW;
    s.report_ctx = RSX_DMA_REPORT_LOCAL;
    s.vsync = 1;
    s.free_flips = getenv("PS3_FREE_FLIPS") != NULL;
    s.flip_status = 0;
    memset(s.display, 0, sizeof s.display);
    s.display_mask = 0;
    InitializeSRWLock(&s.vblank_lock);
    InitializeConditionVariable(&s.vblank_cv);
    InitializeSRWLock(&s.irq_lock);
    InitializeConditionVariable(&s.irq_cv);
    static const u16 core_regs[] = { 0x0050, 0x0060, 0x0064, 0x0068, 0x006C, 0x0104, 0x0110,
                                     0x0180, 0x01A4, 0x01A8, 0x1D6C, 0x1D70, 0x1D74, 0x1800 };
    for (u32 i = 0; i <= RSX_3D_REGS; i++) s_next_core[i] = RSX_3D_REGS;
    for (u32 i = 0; i < sizeof core_regs / sizeof core_regs[0]; i++) s_next_core[core_regs[i] >> 2] = (u16)(core_regs[i] >> 2);
    for (u32 i = RSX_3D_REGS; i-- > 0;)
        if (s_next_core[i] != i) s_next_core[i] = s_next_core[i + 1u];
    rsx_window_clear();
    rsx_transfer_reset();
    ppu_slow_pages(VM_HLE_INJECT_BASE, 0x10000u);
    g_rsx_window_write = rsx_core_window_written;
}

void rsx_core_set_renderer(const rsx_renderer* renderer) { s.renderer = renderer; }
int rsx_core_free_flips(void) { return s.free_flips; }
u64 rsx_core_progress(void) { return __atomic_load_n(&s.progress, __ATOMIC_RELAXED); }
u64 rsx_core_batch(void) { return s.batch; }
void rsx_core_set_event_sink(rsx_event_sink sink) { s.sink = sink; }

static void rsx_thread(LPTHREAD_START_ROUTINE fn, SIZE_T stack, int priority, const wchar_t* name)
{
    HANDLE h = CreateThread(NULL, stack, fn, NULL, 0, NULL);
    if (!h) {
        fprintf(stderr, "[rsx] thread creation failed\n");
        return;
    }
    SetThreadPriority(h, priority);
    SetThreadDescription(h, name);
    CloseHandle(h);
}

void rsx_core_start(void)
{
    if (InterlockedCompareExchange(&s.started, 1, 0) != 0) return;
    rsx_thread(rsx_irq_thread, 16u << 20, THREAD_PRIORITY_ABOVE_NORMAL, L"RSX interrupts");
    rsx_thread(rsx_vblank_thread, 1u << 20, THREAD_PRIORITY_TIME_CRITICAL, L"RSX vblank");
    rsx_thread(rsx_fifo_thread, 16u << 20, THREAD_PRIORITY_HIGHEST, L"RSX");
}

void rsx_core_fifo_reset(u32 put, u32 get)
{
    s.get = get;
    s.ret = RSX_NO_RETURN;
    s.method_left = 0;
    rsx_store_control(RSX_GET_EA, get);
    rsx_store_control(RSX_PUT_EA, put);
    rsx_core_doorbell();
}

void rsx_mem_store(u32 ea, const void* src, u32 size)
{
    if (!size) return;
    const u32 l0 = ea & ~127u, l1 = (ea + size - 1u) & ~127u;
    int reserved = 0;
    for (u32 a = l0; a <= l1 && !reserved; a += 128u) reserved = spu_coh_is_reserved(a);
    if (!reserved) {
        memmove(vm_base + ea, src, size);
    } else {
        spu_lockline_lock_range(ea, size);
        memmove(vm_base + ea, src, size);
        for (u32 a = l0; a <= l1; a += 128u)
            if (spu_coh_is_reserved(a)) spu_coh_notify_write(a);
        spu_lockline_unlock_range(ea, size);
    }
}

int rsx_core_defer(const rsx_write* w)
{
    return s.renderer && s.renderer->defer && s.renderer->defer(s.renderer->user, w);
}

void rsx_core_read_back(u32 ea, u32 size)
{
    if (s.renderer && s.renderer->read_back) s.renderer->read_back(s.renderer->user, ea, size);
}

void rsx_core_settle(u32 ea, u32 size)
{
    if (s.renderer && s.renderer->settle) s.renderer->settle(s.renderer->user, ea, size);
}

void rsx_core_wrote(u32 ea, u32 size)
{
    if (s.renderer && s.renderer->wrote) s.renderer->wrote(s.renderer->user, ea, size);
}

int rsx_core_blit(const rsx_blit* b)
{
    return s.renderer && s.renderer->blit && s.renderer->blit(s.renderer->user, b);
}

int rsx_io_map(u32 ea, u32 io, u32 size)
{
    if ((ea | io | size) & 0xFFFFFu) return -1;
    const u32 pages = size >> 20, io_page = io >> 20, ea_page = ea >> 20;
    if (!pages || !ea_page || io_page + pages > RSX_IO_PAGES || ea_page + pages > 4096u) return -1;
    for (u32 i = 0; i < pages; i++) {
        __atomic_store_n(&s.io_ea[io_page + i], ea + (i << 20), __ATOMIC_RELEASE);
        s.ea_io[ea_page + i] = (u16)(io_page + i);
        vm_write16(RSX_IO_TABLE_EA + (ea_page + i) * 2u, (u16)(io_page + i));
        vm_write16(RSX_EA_TABLE_EA + (io_page + i) * 2u, (u16)(ea_page + i));
    }
    return 0;
}

void rsx_io_unmap(u32 io, u32 size)
{
    const u32 io_page = io >> 20, pages = (size + 0xFFFFFu) >> 20;
    for (u32 i = 0; i < pages && io_page + i < RSX_IO_PAGES; i++) {
        const u32 ea = s.io_ea[io_page + i];
        if (!ea) continue;
        __atomic_store_n(&s.io_ea[io_page + i], 0u, __ATOMIC_RELEASE);
        s.ea_io[ea >> 20] = 0xFFFFu;
        vm_write16(RSX_IO_TABLE_EA + (ea >> 20) * 2u, 0xFFFFu);
        vm_write16(RSX_EA_TABLE_EA + (io_page + i) * 2u, 0xFFFFu);
    }
}

u32 rsx_io_to_ea(u32 io)
{
    const u32 page = io >> 20;
    if (page >= RSX_IO_PAGES) return 0;
    const u32 base = __atomic_load_n(&s.io_ea[page], __ATOMIC_ACQUIRE);
    return base ? base | (io & 0xFFFFFu) : 0;
}

u32 rsx_ea_to_io(u32 ea)
{
    const u16 page = s.ea_io[ea >> 20];
    return page == 0xFFFFu ? 0xFFFFFFFFu : ((u32)page << 20) | (ea & 0xFFFFFu);
}

u32 rsx_address(u32 location, u32 offset, u32 size)
{
    switch (location) {
    case 0: case RSX_DMA_LOCAL:
        if (offset < s.local_size && s.local_size - offset >= size) return RSX_LOCAL_BASE + offset;
        return 0;
    case 1: case RSX_DMA_MAIN: {
        const u32 ea = rsx_io_to_ea(offset);
        if (!ea) return 0;
        for (u32 next = 0x100000u - (offset & 0xFFFFFu); next < size; next += 0x100000u)
            if (rsx_io_to_ea(offset + next) != ea + next) return 0;
        return ea;
    }
    case RSX_DMA_REPORT_LOCAL:
        return (offset < RSX_REPORT_SIZE && RSX_REPORT_SIZE - offset >= size) ? RSX_REPORT_EA + offset : 0;
    case RSX_DMA_REPORT_MAIN:
        return offset < 0x1000000u ? rsx_io_to_ea(0x0E000000u + offset) : 0;
    case RSX_DMA_SEMAPHORE_RW: case RSX_DMA_SEMAPHORE_R:
        return (offset < RSX_SEMAPHORE_SIZE && RSX_SEMAPHORE_SIZE - offset >= size) ? RSX_SEMAPHORE_EA + offset : 0;
    }
    return 0;
}

void rsx_core_set_display_buffer(u32 id, const rsx_display_buffer* buffer)
{
    if (id >= 8u) return;
    s.display[id] = *buffer;
    __atomic_or_fetch(&s.display_mask, 1u << id, __ATOMIC_RELEASE);
}

int rsx_core_display_buffer(u32 id, rsx_display_buffer* out)
{
    if (id >= 8u || !(__atomic_load_n(&s.display_mask, __ATOMIC_ACQUIRE) & (1u << id))) return 0;
    *out = s.display[id];
    return 1;
}

void rsx_core_set_hidden_refresh(u32 hz) { __atomic_store_n(&s.hidden_period, hz ? rsx_tick_freq() / hz : 0u, __ATOMIC_RELEASE); }
void rsx_core_set_frame_limit(u32 hz) { __atomic_store_n(&s.frame_period, hz ? rsx_tick_freq() / hz : 0u, __ATOMIC_RELEASE); }
void rsx_core_set_vsync(int vsync) { __atomic_store_n(&s.vsync, vsync ? 1u : 0u, __ATOMIC_RELEASE); }
u32 rsx_core_flip_status(void) { return __atomic_load_n(&s.flip_status, __ATOMIC_ACQUIRE); }
void rsx_core_set_flip_status(u32 status) { __atomic_store_n(&s.flip_status, status, __ATOMIC_RELEASE); }
u64 rsx_core_last_flip_time(void) { return __atomic_load_n(&s.last_flip_time, __ATOMIC_ACQUIRE); }
u32 rsx_core_frames_presented(void) { return __atomic_load_n(&s.frames, __ATOMIC_ACQUIRE); }
