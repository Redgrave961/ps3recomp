/*
 * ps3recomp - PPU HLE bridge (NID -> host function dispatch)
 *
 * Connects the recompiled game's firmware imports to our HLE C libraries.
 * The game calls an imported function through its import stub; the lifter
 * emits `ps3_hle_call(<NID>, ctx)` for those addresses (see ppu_lifter.py
 * --imports). This resolves the NID to a registered HLE handler and marshals
 * the PPC calling convention into a native C call.
 *
 * PPC64 ELFv1 integer/pointer ABI: arguments in r3..r10 (gpr[3..10]), return
 * value in r3. The generic adapter casts the handler to a uint64-in/uint64-out
 * function and passes the 8 GPR argument slots; this covers the large majority
 * of cellXxx APIs (integer args/handles, s32 return). Functions that take or
 * return *pointers* need host<->guest address translation and so require a
 * per-function wrapper -- the generic path passes the raw value through.
 *
 * Compiled as C++ (matches the lifted output). Game-agnostic.
 */
#define PPU_NO_INLINE_LOADS   /* declares its own out-of-line vm_read* below */
#include <chrono>
#include "ppu_recomp.h"   /* ppu_context */
#include "ps3emu/nid.h"   /* ps3_nid_table, ps3_nid_entry */
#include "ps3emu/milestone.h" /* ps3_msf -- boot milestone log */
#include "ps3emu/ps3_ftrace.h"   /* PS3_FTRACE: spans of HLE import calls */
#include <stdlib.h>       /* getenv */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>       /* getenv, atoi (boot trace) */

/* Single flat NID -> handler table (all modules share it; resolution is by
 * NID which is globally unique). Sized for the firmware import surface. */
#define HLE_NID_CAP 4096
static ps3_nid_entry  g_hle_storage[HLE_NID_CAP];
static ps3_nid_table  g_hle_nids;
static int            g_hle_inited = 0;

extern "C" void ps3_hle_register(uint32_t nid, const char* name, void* handler)
{
    if (!g_hle_inited) { ps3_nid_table_init(&g_hle_nids, g_hle_storage, HLE_NID_CAP); g_hle_inited = 1; }
    ps3_nid_table_add(&g_hle_nids, nid, name, handler);
}

extern "C" uint32_t ps3_hle_count(void) { return g_hle_inited ? g_hle_nids.count : 0; }

/* Context-aware handlers: functions that need the full ppu_context (to read
 * args beyond the generic ABI, set registers like r13, touch memory, etc.).
 * Registered separately and dispatched before the generic table. */
typedef void (*hle_ctx_fn)(ppu_context*);
#define HLE_CTX_CAP 256
static struct { uint32_t nid; hle_ctx_fn fn; const char* name; } g_ctx[HLE_CTX_CAP];
static uint32_t g_ctx_count = 0;

/* NID -> handler, per calling thread. Both tables above are searched
 * linearly, g_ctx with about a hundred entries and g_hle_nids with over a
 * thousand, and a title calls a few imports tens of thousands of times a
 * second: God of War HD makes ~95k HLE calls a second in gameplay, most of
 * them cellGcmAddressToOffset and _sys_memcpy, and the two scans were most of
 * ps3_hle_call's cost. A slot keeps the indexes and the table sizes it was
 * filled against, so a registration made after the first call is still
 * found. Per thread, so no slot is read half-written. */
struct HleSlot { uint32_t nid, ctx_n, tab_n; int32_t ctx_i, tab_i; };
static thread_local HleSlot t_hle_slot[1024];
static const HleSlot* hle_slot(uint32_t nid)
{
    HleSlot* s = &t_hle_slot[(nid * 2654435761u) >> 22];
    const uint32_t ctx_n = g_ctx_count, tab_n = g_hle_inited ? g_hle_nids.count : 0;
    if (s->nid != nid || s->ctx_n != ctx_n || s->tab_n != tab_n || (!s->ctx_n && !s->tab_n)) {
        s->ctx_i = s->tab_i = -1;
        for (uint32_t i = 0; i < ctx_n; i++)
            if (g_ctx[i].nid == nid) { s->ctx_i = (int32_t)i; break; }
        if (s->ctx_i < 0 && tab_n) {
            const ps3_nid_entry* e = ps3_nid_table_find(&g_hle_nids, nid);
            if (e) s->tab_i = (int32_t)(e - g_hle_nids.entries);
        }
        s->nid = nid; s->ctx_n = ctx_n; s->tab_n = tab_n;
    }
    return s;
}

extern "C" void ps3_hle_register_ctx(uint32_t nid, const char* name, hle_ctx_fn fn)
{
    if (g_ctx_count < HLE_CTX_CAP) {
        g_ctx[g_ctx_count].nid = nid; g_ctx[g_ctx_count].fn = fn;
        g_ctx[g_ctx_count].name = name ? name : "?";   /* named in the milestone log */
        g_ctx_count++;
    }
}

/* Leaf imports that can finish without the dispatcher: no guest code, no
 * change to r2, no blocking. The module that implements one registers a fast
 * form beside the full handler. ps3_hle_call tries it first, and the fast form
 * returns 0 to take the full call instead (a contended lock that has to wait,
 * say). God of War HD takes an lwmutex about 640,000 times in one frame (its
 * allocator, when a wave spawns), and the dispatcher's guard and bookkeeping
 * were a large part of each. Registered at init, before guest threads run.
 * Fast calls do not show in PS3_FTRACE. */
typedef int (*hle_fast_fn)(ppu_context*);
#define HLE_FAST_N 64u
static struct { uint32_t nid; hle_fast_fn fn; } g_hle_fast[HLE_FAST_N];
extern "C" void ps3_hle_register_fast(uint32_t nid, hle_fast_fn fn)
{
    const uint32_t h = (nid * 2654435761u) >> 26;
    for (uint32_t i = 0; i < HLE_FAST_N; i++) {
        auto& e = g_hle_fast[(h + i) & (HLE_FAST_N - 1)];
        if (!e.fn || e.nid == nid) { e.nid = nid; e.fn = fn; return; }
    }
}
static inline hle_fast_fn hle_fast(uint32_t nid)
{
    const uint32_t h = (nid * 2654435761u) >> 26;
    for (uint32_t i = 0; i < HLE_FAST_N; i++) {
        const auto& e = g_hle_fast[(h + i) & (HLE_FAST_N - 1)];
        if (!e.fn) return nullptr;
        if (e.nid == nid) return e.fn;
    }
    return nullptr;
}

/* Generic PPC integer/pointer ABI adapter. */
typedef uint64_t (*hle_generic)(uint64_t, uint64_t, uint64_t, uint64_t,
                                uint64_t, uint64_t, uint64_t, uint64_t);

/* Host VM store (defined in ppu_loader.cpp) â€” used for the TOC save below. */
void vm_write64(uint64_t addr, uint64_t val);

/* Breadcrumb for the crash reporter: the last firmware import dispatched, so a
 * host AV inside an HLE handler names the culprit NID/function. */
extern "C" uint32_t    g_last_hle_nid  = 0;
extern "C" const char* g_last_hle_name = "";

/* Which HLE each guest thread is currently INSIDE, indexed by ctx->thread_id.
 *
 * g_last_hle_name is a single global, so with several threads calling it names
 * whoever called most recently -- useless for "what is thread N wedged in".
 * A thread blocked in a host wait also leaves a stale CTR behind (Tokyo
 * Jungle's main thread reports CTR=sys_lwmutex_unlock long after that call
 * returned), so the CTR the watchdog prints is not the answer either. This is:
 * set on entry, cleared on every exit path by the guard below. */
#define PS3_HLE_INFLIGHT_MAX 64
extern "C" const char* g_hle_inflight[PS3_HLE_INFLIGHT_MAX] = { nullptr };

/* Real-PRX bridge: a loaded system PRX (libsre = cellSpurs/cellSync) may export
 * this NID. If so, dispatch into the REAL recompiled Sony code (its OPD -> our
 * indirect dispatcher -> the registered lifted libsre function) instead of the
 * HLE stub. prx_resolve_export returns 0 when no PRX exports the NID, so this is
 * a no-op when no PRX is loaded. */
extern "C" uint32_t prx_resolve_export(uint32_t nid);
extern "C" void     ps3_indirect_call(ppu_context* ctx);
extern "C" uint32_t vm_read32(uint64_t a);
#include "../platform/win32_backtrace.h"   /* RtlCaptureStackBackTrace / GetModuleHandleA on POSIX */
#include "ps3emu/log_first.h"

/* PS3_HLE_TOP=<seconds>: the most-called HLE imports over each window, with
 * the guest thread and caller of the last call. PS3_HLE_TRACE logs the first N
 * calls from boot, which says nothing about a stall ten minutes in; this says
 * what every thread is doing now (polling a pad, spinning on a lwcond, feeding
 * a decoder that never takes the data). Counts are unlocked: a lost increment
 * can't change which import is on top. */
static void hle_top_note(uint32_t nid, const char* name, const ppu_context* ctx, long window_s)
{
    enum { HT_MAX = 256 };
    static uint32_t ht_nid[HT_MAX]; static const char* ht_name[HT_MAX];
    static unsigned ht_n[HT_MAX], ht_tid[HT_MAX], ht_lr[HT_MAX];
    static int ht_used = 0;
    static unsigned long long ht_t0 = 0;
    int i = 0;
    for (; i < ht_used; i++) if (ht_nid[i] == nid) break;
    if (i == ht_used && ht_used < HT_MAX) { ht_nid[i] = nid; ht_name[i] = name; ht_used++; }
    if (i < HT_MAX) {
        ht_n[i]++;
        ht_tid[i] = (unsigned)ctx->thread_id;
        ht_lr[i] = (unsigned)ctx->lr;
    }
    unsigned long long now = (unsigned long long)std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();   /* not ps3_qpc_us: fixtures link this file without cellGcmSys */
    if (!ht_t0) { ht_t0 = now; return; }
    if (now - ht_t0 < (unsigned long long)window_s * 1000000ull) return;
    ht_t0 = now;
    fprintf(stderr, "--- HLE calls (last %lds) ---\n", window_s);
    for (int k = 0; k < 10; k++) {
        int best = -1;
        for (int j = 0; j < ht_used; j++)
            if (ht_n[j] && (best < 0 || ht_n[j] > ht_n[best])) best = j;
        if (best < 0) break;
        fprintf(stderr, "  %8u  %-36s tid=%-3u lr=0x%08X\n",
                ht_n[best], ht_name[best], ht_tid[best], ht_lr[best]);
        ht_n[best] = 0;
    }
    for (int j = 0; j < ht_used; j++) ht_n[j] = 0;
}
extern "C" void ps3_hle_call(uint32_t nid, ppu_context* ctx)
{
    if (const hle_fast_fn f = hle_fast(nid))
        if (f(ctx)) return;

    /* HLE_BT_EVERY=<n>: dump the calling thread's guest stack every nth HLE
     * call it makes. WAITBT_EVERY only samples threads that are blocking on an
     * event queue, which misses a thread that stops looping while it is BUSY --
     * and "the title ran for a while and then quietly stopped advancing" is
     * exactly that shape. Every guest frame goes through HLE calls, so the last
     * dump in the log names the frame the loop was in when it stopped. */
    { static int every = -1;
      if (every < 0) { const char* e = getenv("HLE_BT_EVERY"); every = e ? atoi(e) : 0; }
      if (every > 0 && ctx) {
          static unsigned n[8] = {0};
          unsigned t = (unsigned)ctx->thread_id & 7;
          if ((n[t]++ % (unsigned)every) == 0) {
              extern void ppu_dump_guest_stack(ppu_context*, const char*);
              char tag[64];
              /* Print the REAL thread id, not the ring index: tid&7 collides
               * (1, 9 and 17 all land on slot 1) and a stack from a CRI worker
               * reads exactly like one from the main thread. That collision
               * cost a round. */
              snprintf(tag, sizeof tag, "hle#%u tid=%llu nid=0x%08X", n[t] - 1u,
                       (unsigned long long)ctx->thread_id, nid);
              ppu_dump_guest_stack(ctx, tag);
          }
      } }

    /* Preserve the caller TOC (r2) across the HLE call. ELFv1 makes r2 caller-saved
     * across a cross-module call: the glink stub does `std r2,40(r1)` before jumping and
     * the caller does `ld r2,40(r1)` after. Our HLE import stubs (`ps3_hle_call(nid);
     * return;`) skip that, AND some handlers (cellSpurs* -> 0xAA6269A8 etc.) dispatch
     * into lifted libsre_ns code that returns with r2 = libsre's TOC (0x30039AB0) AND
     * clobbers the caller's reserved TOC slot at 0x28(r1) to 0. The caller's post-call
     * `ld r2,0x28(r1)` then loads 0 -> r2=0 -> every later TOC-relative load reads a null
     * base (e.g. func_002B03AC's `*(r2-0x4B34)` FMOD list head -> null -> infinite walk
     * -> no draws). Snapshot the caller r2+sp now and restore BOTH the live r2 and the
     * ABI slot on every exit path (offset 40 = 0x28 is the reserved TOC doubleword). */
    /* It also closes the PS3_FTRACE span of the call: a thread blocked in an
     * import (a file read, a SPURS wait) shows up nowhere else. */
    struct _TocGuard { ppu_context* c; uint64_t toc, sp, ft0; uint32_t nid, lr;
        ~_TocGuard(){ c->gpr[2] = toc; vm_write64(sp + 0x28, toc);
                      unsigned t = (unsigned)c->thread_id;
                      if (t < PS3_HLE_INFLIGHT_MAX) g_hle_inflight[t] = nullptr;
                      if (ft0) { const uint64_t t1 = ps3_ft_now();
                                 if (t1 - ft0 >= ps3_ft_min_ns())
                                     ps3_ft_span(PS3_FT_HLE, t, nid, lr, ft0, t1); } }
    } _tg{ ctx, ctx->gpr[2], ctx->gpr[1], PS3_FT_ON() ? ps3_ft_now() : 0, nid, (uint32_t)ctx->lr };
    g_last_hle_nid = nid;
    /* Stamp the NAME here too, not only where a handler is found below.
     *
     * These are read as a pair -- the watchdog prints "last HLE call = <nid>
     * (<name>)" -- but they were written at different points: the nid on every
     * dispatch, the name only when a handler resolved. So an UNIMPLEMENTED nid
     * advanced the number and left the previous implemented call's name behind,
     * and the pair printed as one confident fact.
     *
     * Guitar Hero III wedges on sys_spinlock_unlock (0x5267CB35) and the
     * watchdog reported it as "cellPadInit" -- which for a game that needs a
     * guitar controller is an extremely convincing wrong answer. Placeholder
     * now, overwritten with the real name once the handler is known. */
    g_last_hle_name = "(unimplemented)";

    /* Boot trace: log the first N HLE calls (PS3_HLE_TRACE=N). Invaluable for
     * new-SDK bring-up (e.g. PSL1GHT) where the failure is "nothing happens". */
    static int s_trace = -2;
    if (s_trace == -2) { const char* e = getenv("PS3_HLE_TRACE"); s_trace = e ? atoi(e) : 0; }
    if (s_trace > 0) {
        s_trace--;
        fprintf(stderr, "[HLETRACE] nid=0x%08X r3=0x%08X r4=0x%08X r5=0x%08X lr=0x%08X\n",
                nid, (uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4],
                (uint32_t)ctx->gpr[5], (uint32_t)ctx->lr);
    }
    /* PS3_HLE_ARGS=<nid-hex>: dump the full PPC64 argument register set (r3-r10)
     * for one import. PS3_HLE_TRACE only shows r3-r5, which is not enough to tell
     * a MISDECLARED HLE prototype from a guest passing bad values -- the two look
     * identical from the callee side, and getting it wrong silently shifts every
     * argument after the mistake. */
    { static long s_ha = -1;
      if (s_ha < 0) { const char* e = getenv("PS3_HLE_ARGS");
                      s_ha = e ? (long)strtoul(e, 0, 16) : 0; }
      if (s_ha && nid == (uint32_t)s_ha) {
        static int _n = 0;
        if (PS3_LOG_FIRST(_n, 6))
          fprintf(stderr, "[hle-args] nid=0x%08X r3=0x%08X r4=0x%08X r5=0x%08X r6=0x%08X "
                          "r7=0x%08X r8=0x%08X r9=0x%08X r10=0x%08X lr=0x%08X\n",
                  nid, (uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4], (uint32_t)ctx->gpr[5],
                  (uint32_t)ctx->gpr[6], (uint32_t)ctx->gpr[7], (uint32_t)ctx->gpr[8],
                  (uint32_t)ctx->gpr[9], (uint32_t)ctx->gpr[10], (uint32_t)ctx->lr);
      } }

    /* sys_process_exit (abort path): dump the guest back-chain so we see WHO aborted.
     * Always-on for a NONZERO exit code (error/abort) -- the rare loader-thread
     * abort is usually a race that will not reproduce on demand, so capture it
     * whenever it fires. Clean code=0 exits (normal shutdown) stay quiet;
     * PS3_EXIT_CHAIN=1 dumps those too. */
    if (nid == 0xE6F2C1E7u && ((uint32_t)ctx->gpr[3] != 0 || getenv("PS3_EXIT_CHAIN"))) {
        uint32_t sp = (uint32_t)ctx->gpr[1];
        fprintf(stderr, "[exit-chain] code=0x%X lr=0x%08X sp=0x%08X\n", (uint32_t)ctx->gpr[3], (uint32_t)ctx->lr, sp);
        /* Scan the guest stack for words in the lifted code range â€” saved return
         * addresses; a lifted func name IS its guest addr. (back-chain LR slots are
         * 0 under the DRAIN/fragment model, so scan instead of walk.) */
        { uint32_t prev=0; int found=0; char b[1600]; int p=snprintf(b,sizeof b,"[exit-chain] codeptrs:");
          for(uint32_t a=sp; a<sp+0x3000 && a<0x0FF00000u && found<60; a+=4){ uint32_t v=vm_read32(a);
            if((v>=0x00010000u&&v<0x00900000u)||(v>=0x30000000u&&v<0x30100000u)){
              if(v!=prev){ p+=snprintf(b+p,sizeof(b)-p," %08X",v); prev=v; found++; } } }
          fprintf(stderr,"%s\n",b); }
#ifdef _WIN32
        { void* bt[40]; unsigned short fr=RtlCaptureStackBackTrace(0,40,bt,0);
          char* mb=(char*)GetModuleHandleA(0); char line[1100]; int p=snprintf(line,sizeof line,"[exit-chain] HOST-bt rva:");
          for(int i=0;i<fr;i++) p+=snprintf(line+p,sizeof(line)-p," %llX",(unsigned long long)((char*)bt[i]-mb));
          fprintf(stderr,"%s\n",line); }
#endif
    }
    /* PPC64 ELFv1 cross-module ABI: the caller restores its TOC right after the
     * call with `ld r2, 0x28(r1)`, expecting the import stub to have saved the
     * caller's r2 into that slot. The real .lib.stub trampoline did this; the
     * lifted --hle-stubs body (ps3_hle_call) doesn't, so without this every
     * import call leaves the caller with a garbage r2 -> all later TOC-relative
     * loads (the C++ ctor list, globals, ...) read garbage -> boot corruption. */
    vm_write64(ctx->gpr[1] + 0x28, ctx->gpr[2]);

    if (const uint32_t opd = prx_resolve_export(nid)) {
        ctx->gpr[2] = vm_read32(opd + 4);
        ctx->ctr    = vm_read32(opd);
        ps3_indirect_call(ctx);
        ps3_msf("prx:0x%08X", nid);
        return;
    }

    const HleSlot* hs = hle_slot(nid);
    if (hs->ctx_i >= 0) {
            const uint32_t i = (uint32_t)hs->ctx_i;
            ps3_msf("hle:%s", g_ctx[i].name);
            /* Record the name here too. This loop dispatches every
             * ps3_hle_register_ctx handler -- which is the whole sysPrxForUser
             * and CRT surface: spinlocks, lwmutex, thread and fs ops -- and it
             * returned without touching g_last_hle_name or the in-flight slot.
             * So for a title wedged anywhere in that surface the watchdog named
             * whichever TABLE handler ran last instead, with total confidence.
             *
             * Guitar Hero III wedges in sys_spinlock_unlock and was reported as
             * "cellPadInit", which for a game that needs a guitar controller is
             * a very convincing wrong answer to chase. */
            g_last_hle_name = g_ctx[i].name;
            { unsigned t = (unsigned)ctx->thread_id;
              if (t < PS3_HLE_INFLIGHT_MAX) g_hle_inflight[t] = g_ctx[i].name; }
            g_ctx[i].fn(ctx);
            { unsigned t = (unsigned)ctx->thread_id;
              if (t < PS3_HLE_INFLIGHT_MAX) g_hle_inflight[t] = nullptr; }
            return;
        }

    ps3_nid_entry* e = hs->tab_i >= 0 ? &g_hle_nids.entries[hs->tab_i] : nullptr;
    if (!e || !e->handler) {
        ps3_msf("hle:unresolved:0x%08X", nid);
        static int logged = 0;
        if (logged < 40) { fprintf(stderr, "[hle] unresolved NID 0x%08X\n", nid); logged++; }
        ctx->gpr[3] = 0;   /* CELL_OK-ish so the game keeps going */
        return;
    }
    g_last_hle_name = e->name;
    { static long s_ht = -1;
      if (s_ht < 0) { const char* ev = getenv("PS3_HLE_TOP"); s_ht = ev ? atol(ev) : 0;
                      if (s_ht < 0) s_ht = 0; }
      if (s_ht > 0) hle_top_note(nid, e->name, ctx, s_ht); }
    ps3_msf("hle:%s", e->name);
    { unsigned t = (unsigned)ctx->thread_id;
      if (t < PS3_HLE_INFLIGHT_MAX) g_hle_inflight[t] = e->name; }
    hle_generic fn = (hle_generic)e->handler;
    uint64_t r = fn(ctx->gpr[3], ctx->gpr[4], ctx->gpr[5], ctx->gpr[6],
                    ctx->gpr[7], ctx->gpr[8], ctx->gpr[9], ctx->gpr[10]);
    ctx->gpr[3] = r;   /* PPC return value */
    /* PS3_HLE_PTRCHECK: the guest stores HLE return values as guest pointers, so
     * an HLE returning a value outside guest RAM (>= 0x10000000) that is not a
     * CELL_ERROR code (0x8001xxxx / 0x8002xxxx) is leaking a truncated HOST
     * pointer. It surfaces much later as an unresolved indirect call with no hint
     * of where it came from, which is why it is worth checking for directly. */
    { static int _pl=-1; if(_pl<0)_pl=getenv("PS3_HLE_PTRCHECK")?1:0;
      if(_pl){ uint32_t rv=(uint32_t)r;
        if(rv>=0x10000000u && (rv & 0xFFFF0000u)!=0x80010000u && (rv & 0xFFFF0000u)!=0x80020000u){
          static int _n=0; if(PS3_LOG_FIRST(_n, 40)) fprintf(stderr,"[PTRLEAK] NID 0x%08X %s returned 0x%08X (out-of-guest-RAM host ptr?)\n",
            nid, e->name?e->name:"?", rv); } } }
}

/* Populated by the generated registration unit (gen_hle_nids.py). Weak so a
 * build without it still links (no HLE registered -> imports log + return 0). */
#if defined(_MSC_VER)
/* MSVC has no weak functions: /alternatename redirects references to the
 * default only while the strong definition from the generated unit is absent
 * -- weak semantics, and ignored when ppu_hle_nids.cpp is linked. */
extern "C" void ppu_hle_register_all_default(void) {}
extern "C" void ppu_hle_register_all(void);   /* generated unit; /alternatename falls back */
#  if defined(_M_IX86)
#    pragma comment(linker, "/alternatename:_ppu_hle_register_all=_ppu_hle_register_all_default")
#  else
#    pragma comment(linker, "/alternatename:ppu_hle_register_all=ppu_hle_register_all_default")
#  endif
#else
extern "C" void ppu_hle_register_all(void) __attribute__((weak));
extern "C" void ppu_hle_register_all(void) {}
#endif

extern "C" void ppu_hle_init(void)
{
    ppu_hle_register_all();

    /* The generated registration unit is per-game and easy to leave out: the
     * weak stub above means a build without it links and starts, then fails
     * later and somewhere else. Say so here, once, at the point where it is
     * still obvious what to do about it. */
    if (ps3_hle_count() == 0) {
        fprintf(stderr,
            "\n[ps3] WARNING: no HLE handlers are registered.\n"
            "  ppu_hle_register_all() is the weak do-nothing stub, so every firmware\n"
            "  import will return 0 and the first indirect call through one lands on\n"
            "  the import stub's own instruction word (a bare address like 0x39800000\n"
            "  is `li r12,0`, not a function).\n"
            "  Generate the table and add it to the build:\n"
            "      python tools/gen_hle_nids.py --all --out src/gen/ppu_hle_nids.cpp\n"
            "  See docs/GETTING_STARTED.md.\n\n");
        fflush(stderr);
    }
}
