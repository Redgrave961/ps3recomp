#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "spu_raw.h"
#include "spu_context.h"
#include "spu_workload.h"
#include "spu_lifted_job.h"
#include "../syscalls/lv2_syscall_table.h"
#include "../../include/ps3emu/error_codes.h"
#include "ps3emu/nid.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../platform/ps3_sync.h"
#include "../memory/vm_fault_x86.h"
#include "../memory/vm_page_map.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <sys/mman.h>
#include <ucontext.h>
#endif

extern uint8_t* vm_base;
extern int spu_run_with_halt(void (*)(spu_context*), spu_context*);
#include "../ppu/ppu_context.h"
#include "ps3emu/thread_name.h"
extern PPU_THREAD_LOCAL ppu_context* g_active_ctx;
extern void ps3_hle_register_ctx(uint32_t nid, const char* name,
                                 void (*fn)(ppu_context*));

#define RUNCNTL_RUN   0x1u

static void ps3_intr_raise(uint32_t tag);

#define SPU_RAW_IN_MBOX_DEPTH  SPU_IN_MBOX_HW_DEPTH
#define SPU_IN_MBOX_FREE(n)     ((uint32_t)((n) >= SPU_RAW_IN_MBOX_DEPTH ? 0u : SPU_RAW_IN_MBOX_DEPTH - (n)))
#define MBOX_STATUS(out_n, in_free, intr_n) \
    (((uint32_t)(out_n) & 0xFF) | (((uint32_t)(in_free) & 0xFF) << 8) | \
     (((uint32_t)(intr_n) & 0xFF) << 16))

typedef struct raw_spu {
    int          used;
    uint32_t     base;
    spu_context* ctx;
    spu_lifted_entry_fn entry;
    uint32_t     intrtag;
    uint32_t     int_mask;
    uint32_t     int_stat;
    volatile long started;
    uint32_t     get_lo[8], get_hi[8];
    int          get_runs;
#ifdef _WIN32
    HANDLE       thread;
#endif
} raw_spu;

static raw_spu s_spu[SPU_RAW_COUNT];

#define SPU_RAW_REG_PAGES 0x43000u
#define SPU_RAW_REG_SPAN  0x2000u

static uint8_t s_reg_shadow[SPU_RAW_COUNT][SPU_RAW_REG_SPAN];
static int     s_shadowed[SPU_RAW_COUNT];

uint8_t* spu_raw_shadow(uint32_t ea)
{
    const uint32_t off = ea - SPU_RAW_BASE, n = off / SPU_RAW_STRIDE, at = off % SPU_RAW_STRIDE - SPU_RAW_REG_PAGES;
    if (n >= SPU_RAW_COUNT || at >= SPU_RAW_REG_SPAN || !__atomic_load_n(&s_shadowed[n], __ATOMIC_ACQUIRE)) return NULL;
    return &s_reg_shadow[n][at];
}

static uint8_t* raw_mem(uint32_t ea)
{
    uint8_t* p = spu_raw_shadow(ea);
    return p ? p : vm_base + ea;
}

static uint32_t be32_load(uint32_t ea)
{
    uint32_t b;
    memcpy(&b, raw_mem(ea), 4);
    return (b >> 24) | ((b >> 8) & 0xFF00u) | ((b << 8) & 0xFF0000u) | (b << 24);
}

static void be32_store(uint32_t ea, uint32_t v)
{
    uint32_t b = (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
    memcpy(raw_mem(ea), &b, 4);
}

static raw_spu* spu_for(uint32_t ea, uint32_t* off_out)
{
    uint32_t off = ea - SPU_RAW_BASE;
    uint32_t n   = off / SPU_RAW_STRIDE;
    if (n >= SPU_RAW_COUNT) return NULL;
    *off_out = off - n * SPU_RAW_STRIDE;
    return &s_spu[n];
}

static void publish(raw_spu* s)
{
    spu_context* c = s->ctx;
    if (!c) return;
    be32_store(s->base + SPU_RAW_STATUS, c->status);
    be32_store(s->base + SPU_RAW_MBOX_STATUS,
               MBOX_STATUS(c->ch_out_mbox.count,
                           SPU_IN_MBOX_FREE(c->ch_in_mbox.count),
                           c->ch_out_intr_mbox.count));
    if (c->ch_out_mbox.count)
        be32_store(s->base + SPU_RAW_OUT_MBOX, c->ch_out_mbox.value);
}

extern void (*g_spu_out_mbox_hook)(uint32_t group_id, uint32_t spu_id,
                                   int is_intr, uint32_t value);
static void (*s_prev_out_hook)(uint32_t, uint32_t, int, uint32_t);

static void raw_out_mbox_hook(uint32_t group, uint32_t spu_id, int is_intr, uint32_t v)
{
    if (spu_id < SPU_RAW_COUNT && s_spu[spu_id].used && s_spu[spu_id].ctx) {
        raw_spu* s = &s_spu[spu_id];
        be32_store(s->base + SPU_RAW_OUT_MBOX, v);
        s->int_stat |= is_intr ? 0x1u : 0x10u;
        if (s->int_stat & s->int_mask) ps3_intr_raise(s->intrtag);
        publish(s);
        return;
    }
    if (s_prev_out_hook) s_prev_out_hook(group, spu_id, is_intr, v);
}

static spu_lifted_entry_fn s_pending_entry;
static int                 s_pending_image_id;
static uint32_t            s_pending_elf_entry;

void spu_raw_note_image(uint32_t src_ea, uint32_t entry)
{
    if (!vm_base) return;
    const uint8_t* e = vm_base + src_ea;
    uint32_t shoff = ((uint32_t)e[0x20] << 24) | ((uint32_t)e[0x21] << 16) |
                     ((uint32_t)e[0x22] << 8)  |  (uint32_t)e[0x23];
    uint32_t shentsz = ((uint32_t)e[0x2E] << 8) | e[0x2F];
    uint32_t shnum   = ((uint32_t)e[0x30] << 8) | e[0x31];
    uint32_t len = shoff + shentsz * shnum;
    if (!len || len > 0x100000u) return;

    uint64_t fp = spu_workload_fingerprint(e, len);
    int image_id = 0;
    spu_lifted_entry_fn fn = spu_workload_find_img(fp, &image_id);
    s_pending_entry     = fn;
    s_pending_image_id  = image_id;
    s_pending_elf_entry = entry;

    fprintf(stderr, "[spu-raw] image_import src=0x%08X len=%u entry=0x%05X "
                    "fp=0x%016llX -> %s\n",
            src_ea, len, entry, (unsigned long long)fp,
            fn ? "lifted entry registered" : "NO LIFTED IMAGE (SPU will not run)");
    fflush(stderr);
}

#ifdef _WIN32
#define RAW_XCHG(p, v)    InterlockedExchange((p), (v))
#define RAW_CAS(p, v, c)  InterlockedCompareExchange((p), (v), (c))
#else
#include <pthread.h>
#define RAW_XCHG(p, v)    __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)
static inline long raw_cas(volatile long* p, long v, long c)
{
    long e = c;
    __atomic_compare_exchange_n(p, &e, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return e;
}
#define RAW_CAS(p, v, c)  raw_cas((p), (v), (c))
#endif

static void raw_spu_stopped(raw_spu* s)
{
    RAW_XCHG(&s->started, 0);
    ps3_futex_wake_all((volatile uint32_t*)&s->started);
}

static void raw_spu_run(raw_spu* s)
{
    ps3_thread_name("raw spu");
    spu_context* c = s->ctx;

    c->status = SPU_STATUS_RUNNING;
    publish(s);

    int halted = spu_run_with_halt(s->entry, c);

    if (c->stop_request) {
        c->status = SPU_STATUS_STOPPED;
        publish(s);
        fprintf(stderr, "[spu-raw] spu%u stopped by RunCntl at pc=0x%05X\n",
                (unsigned)(s - s_spu), (unsigned)(c->pc & SPU_LS_MASK));
        fflush(stderr);
        spu_coh_unregister(c);
        spu_mfc_release(c);
        raw_spu_stopped(s);
        return;
    }

    c->status = SPU_STATUS_STOPPED_BY_STOP | (c->stop_code << 16);
    s->int_stat |= 0x2u;
    if (s->int_stat & s->int_mask) ps3_intr_raise(s->intrtag);
    publish(s);
    raw_spu_stopped(s);

    fprintf(stderr, "[spu-raw] spu%u stopped: halted=%d pc=0x%05X lr=0x%05X "
                    "steps=%llu stop_code=0x%X outmbox(n=%u v=0x%08X) "
                    "outintr(n=%u v=0x%08X) inmbox(n=%u)\n",
            (unsigned)(s - s_spu), halted, (unsigned)(c->pc & SPU_LS_MASK),
            (unsigned)(c->gpr[0]._u32[0] & SPU_LS_MASK),
            (unsigned long long)c->steps, c->stop_code,
            (unsigned)c->ch_out_mbox.count, c->ch_out_mbox.value,
            (unsigned)c->ch_out_intr_mbox.count, c->ch_out_intr_mbox.value,
            (unsigned)c->ch_in_mbox.count);
    fflush(stderr);
}

static void raw_spu_stop(raw_spu* s)
{
    spu_context* c = s->ctx;
    if (!c) return;
    if (!s->started) { c->status = SPU_STATUS_STOPPED; return; }
    c->stop_request = 1;
    spu_ch_wake(c);
    const uint64_t deadline = ps3_sync_deadline_us(4000000);
    while (s->started && ps3_futex_wait((volatile uint32_t*)&s->started, 1u, deadline)) {}
    if (s->started)
        fprintf(stderr, "[spu-raw] spu%u did not stop on RunCntl = 0 (pc=0x%05X): "
                        "it is not reaching a channel operation\n",
                (unsigned)(s - s_spu), (unsigned)(c->pc & SPU_LS_MASK));
    else
        c->status = SPU_STATUS_STOPPED;
}

#ifdef _WIN32
static DWORD WINAPI raw_spu_thread(LPVOID arg) { raw_spu_run((raw_spu*)arg); return 0; }
#else
static void* raw_spu_thread(void* arg) { raw_spu_run((raw_spu*)arg); return NULL; }
#endif

static void raw_spu_start(raw_spu* s)
{
    uint32_t n = (uint32_t)(s - s_spu);
    spu_lifted_entry_fn entry = s_pending_entry;
    int image_id = s_pending_image_id;
    uint32_t elf_entry = s_pending_elf_entry;
    s_pending_entry = NULL;
    const int loaded = s->get_runs > 0;
    for (int r = 0; !entry && r < s->get_runs; r++) {
        const uint8_t* run = vm_base + s->base + s->get_lo[r];
        const uint32_t run_len = s->get_hi[r] - s->get_lo[r];
        uint64_t fp = spu_workload_fingerprint(run, run_len);
        uint64_t masked_fp = 0;
        entry = spu_workload_find_img(fp, &image_id);
        if (!entry)
            entry = spu_workload_find_img_masked(run, run_len, &masked_fp, &image_id);
        char how[64] = "";
        if (entry && masked_fp)
            snprintf(how, sizeof how, " (fp=0x%016llX with its parameter area zeroed)",
                     (unsigned long long)masked_fp);
        fprintf(stderr, "[spu-raw] spu%u proxy-loaded LS 0x%05X..0x%05X fp=0x%016llX npc=0x%05X -> %s%s\n",
                n, s->get_lo[r], s->get_hi[r], (unsigned long long)fp,
                be32_load(s->base + SPU_RAW_NPC) & ~3u,
                entry ? "lifted entry registered" : "no match", how);
        { const char* dir = getenv("SPU_RAW_DUMP_DIR");
          if (dir && *dir && !entry) {
              char path[512];
              snprintf(path, sizeof path, "%s/rawspu_%016llX_%05X.bin", dir,
                       (unsigned long long)fp, s->get_lo[r]);
              FILE* f = fopen(path, "wb");
              if (f) {
                  fwrite(vm_base + s->base + s->get_lo[r], 1, s->get_hi[r] - s->get_lo[r], f);
                  fclose(f);
                  fprintf(stderr, "[spu-raw] dumped -> %s\n", path);
              }
          } }
    }
    s->get_runs = 0;
    if (!entry && !loaded && s->entry && s->ctx) {
        entry = s->entry;
        image_id = s->ctx->image_id;
    }
    if (!entry) {
        fprintf(stderr, "[spu-raw] spu%u run requested but no lifted image is "
                        "registered -- the SPU cannot run. Lift the image and "
                        "register it (see a port's src/spu_images.c).\n", n);
        fflush(stderr);
        return;
    }
    if (RAW_CAS(&s->started, 1, 0) != 0) {
        fprintf(stderr, "[spu-raw] spu%u run requested while it is still running: "
                        "ignored\n", n);
        fflush(stderr);
        return;
    }

    if (!s->ctx) {
        s->ctx = (spu_context*)malloc(sizeof(spu_context));
        if (!s->ctx) return;
    }
    spu_context_init(s->ctx, n);

    s->ctx->ls       = vm_base + s->base;
    s->ctx->image_id = image_id;
    s->entry         = entry;
    s->ctx->gpr[1]._u32[0] = SPU_LS_SIZE - 0x10;

    uint32_t npc = be32_load(s->base + SPU_RAW_NPC) & ~3u;
    s->ctx->pc = npc ? npc : elf_entry;

    { const uint8_t* l = s->ctx->ls;
      uint32_t nz = 0, pc = s->ctx->pc & SPU_LS_MASK;
      for (uint32_t i = 0; i < SPU_LS_SIZE; i += 64) if (l[i]) nz++;
      fprintf(stderr, "[spu-raw] spu%u START pc=0x%05X ls=guest:0x%08X image=%d  "
                      "LS[0..7]=%02X%02X%02X%02X%02X%02X%02X%02X  "
                      "entry=%02X%02X%02X%02X  nonzero lines=%u/4096\n",
              n, pc, s->base, s->ctx->image_id,
              l[0],l[1],l[2],l[3],l[4],l[5],l[6],l[7],
              l[pc],l[pc+1],l[pc+2],l[pc+3], nz); }
    fflush(stderr);

#ifdef _WIN32
    s->thread = CreateThread(NULL, 1u << 20, raw_spu_thread, s, 0, NULL);
    if (!s->thread) RAW_XCHG(&s->started, 0);
#else
    {
        pthread_t t;
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setstacksize(&a, 8u << 20);
        if (pthread_create(&t, &a, raw_spu_thread, s) == 0) pthread_detach(t);
        else RAW_XCHG(&s->started, 0);
        pthread_attr_destroy(&a);
    }
#endif
}

void spu_raw_reg_store(uint32_t ea, uint32_t val, int width)
{
    uint32_t off;
    raw_spu* s = spu_for(ea, &off);
    if (!s) return;

    switch (off) {
    case SPU_RAW_RUNCNTL:
        if (val == RUNCNTL_RUN) raw_spu_start(s);
        else                    raw_spu_stop(s);
        break;

    case SPU_RAW_IN_MBOX:
        if (s->ctx) {
            spu_channel_write(&s->ctx->ch_in_mbox, val);
            spu_ch_wake(s->ctx);
            publish(s);
        }
        break;

    case SPU_RAW_SIG_NOTIFY1:
    case SPU_RAW_SIG_NOTIFY2:
        if (s->ctx) {
            int i = (off == SPU_RAW_SIG_NOTIFY1) ? 0 : 1;
            spu_channel_write(&s->ctx->ch_sig_notify[i], val);
            spu_ch_wake(s->ctx);
        }
        break;

    case SPU_RAW_MFC_CLASS_CMD: {
        uint32_t lsa  = be32_load(s->base + SPU_RAW_MFC_LSA) & (SPU_LS_SIZE - 1);
        uint32_t ea   = be32_load(s->base + SPU_RAW_MFC_EAL);
        uint32_t size = be32_load(s->base + SPU_RAW_MFC_SIZE_TAG) >> 16;
        uint32_t cmd  = val & 0xFFFFu;
        if (lsa + size > SPU_LS_SIZE) size = SPU_LS_SIZE - lsa;
        uint8_t* ls = vm_base + s->base + lsa;
        if (cmd & 0x40u) {
            memcpy(ls, vm_base + ea, size);
            int r = 0;
            while (r < s->get_runs && s->get_hi[r] != lsa) r++;
            if (r < s->get_runs)   s->get_hi[r] += size;
            else if (r < 8)      { s->get_lo[r] = lsa; s->get_hi[r] = lsa + size; s->get_runs++; }
        }
        else if (cmd & 0x20u) { memcpy(vm_base + ea, ls, size); vm_track_write(ea, size); }
        be32_store(s->base + SPU_RAW_MFC_CLASS_CMD, 0);
        break;
    }

    default:
        break;
    }
}

int spu_raw_reg_load(uint32_t ea, uint32_t* out)
{
    uint32_t off;
    raw_spu* s = spu_for(ea, &off);
    if (!s) return 0;

    if (off == SPU_RAW_MFC_QSTATUS)   { *out = 0x80000010u; return 1; }
    if (off == SPU_RAW_PRXY_TAGSTATUS) {
        *out = be32_load(s->base + SPU_RAW_PRXY_QUERYMASK); return 1;
    }
    if (!s->ctx) return 0;

    if (off == SPU_RAW_MBOX_STATUS) {
        { static PPU_THREAD_LOCAL unsigned n;
          if (++n % 20000000u == 0)
              fprintf(stderr, "[spu-raw] PPU spinning on spu%u Mbox_Stat: spu pc=0x%05X "
                              "status=0x%X steps=%llu out=%u in=%u lr=0x%08X\n",
                      (unsigned)(s - s_spu), (unsigned)(s->ctx->pc & SPU_LS_MASK),
                      s->ctx->status, (unsigned long long)s->ctx->steps,
                      (unsigned)s->ctx->ch_out_mbox.count, (unsigned)s->ctx->ch_in_mbox.count,
                      g_active_ctx ? (uint32_t)g_active_ctx->lr : 0); }
        *out = MBOX_STATUS(s->ctx->ch_out_mbox.count,
                           SPU_IN_MBOX_FREE(s->ctx->ch_in_mbox.count),
                           s->ctx->ch_out_intr_mbox.count);
        return 1;
    }
    if (off == SPU_RAW_STATUS) { *out = s->ctx->status; return 1; }

    if (off == SPU_RAW_OUT_MBOX) {
        uint32_t v = spu_channel_read(&s->ctx->ch_out_mbox);
        spu_ch_wake(s->ctx);
        publish(s);
        *out = v;
        return 1;
    }
    return 0;
}

#if defined(__x86_64__)
static void raw_read(uint32_t g, int width, uint8_t* out)
{
    uint32_t v;
    if (width == 4 && spu_raw_reg_load(g, &v)) {
        out[0] = (uint8_t)(v >> 24); out[1] = (uint8_t)(v >> 16); out[2] = (uint8_t)(v >> 8); out[3] = (uint8_t)v;
        return;
    }
    for (int i = 0; i < width; i++) out[i] = *raw_mem(g + (uint32_t)i);
}

static void raw_fault_fatal(uint64_t g, uint64_t pc)
{
    fprintf(stderr, "[spu-raw] unsupported access to raw SPU register page 0x%08X at host pc 0x%llX\n",
            (uint32_t)g, (unsigned long long)pc);
    fflush(stderr);
    abort();
}

#ifdef _WIN32
static LONG WINAPI raw_mmio_veh(EXCEPTION_POINTERS* ep)
{
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    const uint64_t g = (uint64_t)er->ExceptionInformation[1] - (uintptr_t)vm_base;
    if (g >= (1ull << 32) || !spu_raw_shadow((uint32_t)g)) return EXCEPTION_CONTINUE_SEARCH;
    if (er->ExceptionInformation[0] == 0 && vm_emulate_load(&ep->ContextRecord->Rax, &ep->ContextRecord->Rip, vm_base, raw_read))
        return EXCEPTION_CONTINUE_EXECUTION;
    raw_fault_fatal(g, ep->ContextRecord->Rip);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void raw_fault_install(void) { AddVectoredExceptionHandler(1, raw_mmio_veh); }

static void raw_protect(uint8_t* p)
{
    DWORD old;
    VirtualAlloc((void*)((uintptr_t)p & ~(uintptr_t)0xFFFF), 0x10000, MEM_COMMIT, PAGE_READWRITE);
    VirtualProtect(p, SPU_RAW_REG_SPAN, PAGE_NOACCESS, &old);
}
#else
static struct sigaction s_prev_segv;
static const int s_greg[16] = { REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP, REG_RSI, REG_RDI,
                                REG_R8, REG_R9, REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15 };

static void raw_mmio_sigsegv(int sig, siginfo_t* si, void* uc_)
{
    ucontext_t* uc = (ucontext_t*)uc_;
    const uint64_t g = (uint64_t)(uintptr_t)si->si_addr - (uintptr_t)vm_base;
    if (g < (1ull << 32) && spu_raw_shadow((uint32_t)g)) {
        uint64_t r[16], rip = (uint64_t)uc->uc_mcontext.gregs[REG_RIP];
        for (int i = 0; i < 16; i++) r[i] = (uint64_t)uc->uc_mcontext.gregs[s_greg[i]];
        if (!vm_emulate_load(r, &rip, vm_base, raw_read)) raw_fault_fatal(g, rip);
        for (int i = 0; i < 16; i++) uc->uc_mcontext.gregs[s_greg[i]] = (greg_t)r[i];
        uc->uc_mcontext.gregs[REG_RIP] = (greg_t)rip;
        return;
    }
    if (s_prev_segv.sa_flags & SA_SIGINFO) { s_prev_segv.sa_sigaction(sig, si, uc_); return; }
    if (s_prev_segv.sa_handler == SIG_IGN) return;
    if (s_prev_segv.sa_handler != SIG_DFL) { s_prev_segv.sa_handler(sig); return; }
    signal(sig, SIG_DFL);
    raise(sig);
}

static void raw_fault_install(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = raw_mmio_sigsegv;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &s_prev_segv);
}

static void raw_protect(uint8_t* p) { mprotect(p, SPU_RAW_REG_SPAN, PROT_NONE); }
#endif

static void raw_reset_problem_state(uint32_t n)
{
    const uint32_t base = s_spu[n].base;
    if (__atomic_load_n(&s_shadowed[n], __ATOMIC_ACQUIRE)) {
        memset(vm_base + base + SPU_RAW_PROB_OFF, 0, SPU_RAW_REG_PAGES - SPU_RAW_PROB_OFF);
        memset(vm_base + base + SPU_RAW_REG_PAGES + SPU_RAW_REG_SPAN, 0,
               SPU_RAW_STRIDE - SPU_RAW_REG_PAGES - SPU_RAW_REG_SPAN);
        memset(s_reg_shadow[n], 0, SPU_RAW_REG_SPAN);
        return;
    }
    memset(vm_base + base + SPU_RAW_PROB_OFF, 0, SPU_RAW_STRIDE - SPU_RAW_PROB_OFF);
    static int installed;
    if (!__atomic_exchange_n(&installed, 1, __ATOMIC_ACQ_REL)) raw_fault_install();
    __atomic_store_n(&s_shadowed[n], 1, __ATOMIC_RELEASE);
    raw_protect(vm_base + base + SPU_RAW_REG_PAGES);
}
#else
static void raw_reset_problem_state(uint32_t n)
{
    (void)n;
    fprintf(stderr, "[spu-raw] raw SPU MMIO needs an x86-64 host (lifted loads fault into the emulator)\n");
    fflush(stderr);
    abort();
}
#endif

static int64_t sc_raw_spu_create(ppu_context* ctx)
{
    uint32_t id_ea = (uint32_t)ctx->gpr[3];
    for (uint32_t n = 0; n < SPU_RAW_COUNT; n++) {
        if (s_spu[n].used) continue;
        s_spu[n].used = 1;
        s_spu[n].base = SPU_RAW_BASE + n * SPU_RAW_STRIDE;

        raw_reset_problem_state(n);
        be32_store(s_spu[n].base + SPU_RAW_MBOX_STATUS,
                   MBOX_STATUS(0, SPU_MBOX_DEPTH, 0));

        if (s_prev_out_hook == NULL && g_spu_out_mbox_hook != raw_out_mbox_hook) {
            s_prev_out_hook   = g_spu_out_mbox_hook;
            g_spu_out_mbox_hook = raw_out_mbox_hook;
        }
        if (id_ea) be32_store(id_ea, n);
        fprintf(stderr, "[spu-raw] create -> raw spu %u, window 0x%08X\n",
                n, s_spu[n].base);
        fflush(stderr);
        return CELL_OK;
    }
    return CELL_ENOMEM;
}

static int64_t sc_raw_spu_destroy(ppu_context* ctx)
{
    uint32_t n = (uint32_t)ctx->gpr[3];
    if (n < SPU_RAW_COUNT) s_spu[n].used = 0;
    return CELL_OK;
}

#include "spu_intr.inc"

static int64_t sc_raw_spu_create_interrupt_tag(ppu_context* ctx)
{
    uint32_t n      = (uint32_t)ctx->gpr[3];
    uint32_t class_ = (uint32_t)ctx->gpr[4];
    uint32_t out_ea = (uint32_t)ctx->gpr[6];
    if (n >= SPU_RAW_COUNT) return CELL_EINVAL;
    s_spu[n].intrtag = 0x52000000u | (n << 8) | (class_ & 0xFF);
    if (out_ea) be32_store(out_ea, s_spu[n].intrtag);
    return CELL_OK;
}

static int64_t sc_raw_spu_set_int_mask(ppu_context* ctx)
{
    uint32_t n = (uint32_t)ctx->gpr[3];
    if (n < SPU_RAW_COUNT) {
        s_spu[n].int_mask = (uint32_t)ctx->gpr[5];
        ps3_intr_raise(s_spu[n].intrtag);
    }
    return CELL_OK;
}

static int64_t sc_raw_spu_get_int_mask(ppu_context* ctx)
{
    uint32_t n = (uint32_t)ctx->gpr[3];
    uint32_t out_ea = (uint32_t)ctx->gpr[5];
    if (n < SPU_RAW_COUNT && out_ea) {
        be32_store(out_ea + 0, 0);
        be32_store(out_ea + 4, s_spu[n].int_mask);
    }
    return CELL_OK;
}

static int64_t sc_raw_spu_set_int_stat(ppu_context* ctx)
{
    uint32_t n = (uint32_t)ctx->gpr[3];
    if (n < SPU_RAW_COUNT) s_spu[n].int_stat &= ~(uint32_t)ctx->gpr[5];
    return CELL_OK;
}

static int64_t sc_raw_spu_get_int_stat(ppu_context* ctx)
{
    uint32_t n = (uint32_t)ctx->gpr[3];
    uint32_t out_ea = (uint32_t)ctx->gpr[5];
    if (n < SPU_RAW_COUNT && out_ea) {
        be32_store(out_ea + 0, 0);
        be32_store(out_ea + 4, s_spu[n].int_stat & s_spu[n].int_mask);
    }
    return CELL_OK;
}

static int64_t sc_raw_spu_read_puint_mb(ppu_context* ctx)
{
    uint32_t n = (uint32_t)ctx->gpr[3];
    uint32_t out_ea = (uint32_t)ctx->gpr[4];
    if (n >= SPU_RAW_COUNT || !s_spu[n].ctx) return CELL_EINVAL;
    uint32_t v = spu_channel_read(&s_spu[n].ctx->ch_out_intr_mbox);
    if (out_ea) be32_store(out_ea, v);
    publish(&s_spu[n]);
    return CELL_OK;
}

static void hle_raw_spu_image_load(ppu_context* ctx)
{
    uint32_t n      = (uint32_t)ctx->gpr[3];
    uint32_t img_ea = (uint32_t)ctx->gpr[4];
    if (n >= SPU_RAW_COUNT || !img_ea || !vm_base) {
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_EINVAL;
        return;
    }
    uint32_t base  = SPU_RAW_BASE + n * SPU_RAW_STRIDE;
    uint32_t entry = be32_load(img_ea + 0x04);
    uint32_t segs  = be32_load(img_ea + 0x08);
    uint32_t nsegs = be32_load(img_ea + 0x0C);

    uint32_t copied = 0;
    for (uint32_t i = 0; i < nsegs && i < 32; i++) {
        uint32_t sg   = segs + i * 0x18;
        uint32_t type = be32_load(sg + 0x00);
        uint32_t lsa  = be32_load(sg + 0x04) & SPU_LS_MASK;
        uint32_t size = be32_load(sg + 0x08);
        uint32_t src  = be32_load(sg + 0x14);
        if (size > SPU_LS_SIZE - lsa) size = SPU_LS_SIZE - lsa;
        if (type == 1 && src) { memcpy(vm_base + base + lsa, vm_base + src, size); copied += size; }
        else if (type == 2)   { memset(vm_base + base + lsa, 0, size); }
    }
    be32_store(base + SPU_RAW_NPC, entry);

    fprintf(stderr, "[spu-raw] image_load spu%u: %u segments, %u bytes into LS, "
                    "NPC=0x%05X\n", n, nsegs, copied, entry);
    fflush(stderr);
    ctx->gpr[3] = CELL_OK;
}

static void hle_spu_image_close(ppu_context* ctx) { ctx->gpr[3] = CELL_OK; }

void sys_raw_spu_init(lv2_syscall_table* tbl)
{
    lv2_syscall_register(tbl, 84, sc_interrupt_thread_establish);
    lv2_syscall_register(tbl, 88, sc_interrupt_thread_eoi);
    lv2_syscall_register(tbl, 89, sc_interrupt_thread_disestablish);
    lv2_syscall_register(tbl, 150, sc_raw_spu_create_interrupt_tag);
    lv2_syscall_register(tbl, 151, sc_raw_spu_set_int_mask);
    lv2_syscall_register(tbl, 152, sc_raw_spu_get_int_mask);
    lv2_syscall_register(tbl, 153, sc_raw_spu_set_int_stat);
    lv2_syscall_register(tbl, 154, sc_raw_spu_get_int_stat);
    lv2_syscall_register(tbl, 160, sc_raw_spu_create);
    lv2_syscall_register(tbl, 161, sc_raw_spu_destroy);
    lv2_syscall_register(tbl, 163, sc_raw_spu_read_puint_mb);

    ps3_hle_register_ctx(ps3_compute_nid("sys_raw_spu_image_load"),
                         "sys_raw_spu_image_load", hle_raw_spu_image_load);
    ps3_hle_register_ctx(ps3_compute_nid("sys_spu_image_close"),
                         "sys_spu_image_close", hle_spu_image_close);
}
