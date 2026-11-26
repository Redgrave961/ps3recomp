#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <atomic>
#ifdef _WIN32
#  include <windows.h>
#else
#  include "../platform/win32_compat.h"
#endif

#include "ppu_tls.h"
#define PPU_NO_INLINE_LOADS
#include "ppu_recomp.h"
#include "../spu/spu_coh_bitmap.h"
#include "../memory/vm_page_map.h"
#include "ps3emu/ps3_ftrace.h"
#include "ps3emu/milestone.h"
#include "../memory/vm.h"
#include "../platform/win32_compat.h"
#include "../platform/win32_backtrace.h"
#include "../platform/ps3_stdio.h"
#ifndef _WIN32
#include <unistd.h>
#endif
extern "C" uint32_t ppu_prof_resolve_host(void* ra);

extern "C" void ppu_guest_caller(char* out, size_t n)
{
    snprintf(out, n, "?");
    void* fr[24]; unsigned short cnt = RtlCaptureStackBackTrace(0, 24, fr, 0);
    for (unsigned short i = 0; i < cnt; i++) {
        uintptr_t tgt = (uintptr_t)fr[i], best_h = 0; uint32_t best_g = 0;
        for (uint64_t k = 0; k < function_table_count; k++) {
            uintptr_t h = (uintptr_t)function_table[k].func;
            if (h <= tgt && h > best_h) { best_h = h; best_g = function_table[k].addr; }
        }
        if (best_g && tgt - best_h < 0x20000) {
            snprintf(out, n, "func_%08X+0x%llX", best_g,
                     (unsigned long long)(tgt - best_h));
            return;
        }
    }
}

#include "ps3emu/log_first.h"

#include "ppu_resv.h"

extern "C" alignas(64) uint64_t g_ppu_resv[PPU_RESV_MASK + 1] = {};

extern "C" void ppu_resv_break_line(uint32_t ea) { ppu_resv_break_fenced(ea); }

extern "C" int ppu_stwcx32(ppu_context* self, uint64_t ea, uint32_t expected, uint32_t val)
{
    return ppu_store_conditional<uint32_t>(self, (uint32_t)ea, expected, val);
}

extern "C" int ppu_stdcx64(ppu_context* self, uint64_t ea, uint64_t expected, uint64_t val)
{
    return ppu_store_conditional<uint64_t>(self, (uint32_t)ea, expected, val);
}

extern "C" uint32_t ppu_vm_size = 0;

static uint32_t g_tls_vaddr = 0, g_tls_filesz = 0, g_tls_memsz = 0;
#define PPU_TLS_IMG   VM_PPU_TLS_BASE
#define PPU_TLS_TP    (PPU_TLS_IMG + 0x7000u)

#include "../spu/spu_raw.h"

static int vm_oob_report(uint32_t a, uint32_t n)
{
    {
        static int logged = 0;
        if (logged < 40) {
            fprintf(stderr, "[vm] OOB access 0x%08X (+%u)\n", a, n);
            static int bt = 0;
            if (PS3_LOG_FIRST(bt, 3)) {
                void* fr[20]; USHORT m = RtlCaptureStackBackTrace(0, 20, fr, NULL);
                HMODULE self = NULL;
                GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)&vm_oob_report, &self);
                for (USHORT i = 0; i < m; i++) {
                    HMODULE mm = NULL;
                    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)fr[i], &mm);
                    if (mm == self)
                        fprintf(stderr, "[vm-oob]   #%-2u rva=0x%llX\n", i,
                                (unsigned long long)((char*)fr[i] - (char*)self));
                }
            }
            logged++;
        }
        return 1;
    }
    return 0;
}

static inline int vm_oob(uint32_t a, uint32_t n)
{
    if (__builtin_expect(ppu_vm_size == 0, 1)) return 0;
    if (__builtin_expect((uint64_t)a + n <= ppu_vm_size, 1)) return 0;
    return vm_oob_report(a, n);
}

extern "C" void ppu_dump_guest_stack(ppu_context*, const char*);
extern "C" uint32_t g_null_sweep_hi = 0;
static uint32_t g_null_sweep_last = 0;
static unsigned g_null_sweep_tid = 0;
static int vm_null_store_report(uint32_t a, uint32_t v, int width, void* ra)
{
    static int en = -1;
    if (en < 0) { const char* e = getenv("PS3_NULL_WRITE");
                  en = (e && *e == (char)48) ? 0 : 1; }
    if (!en) return 0;
    { static int sweep = -1;
      if (sweep < 0) { const char* e = getenv("PS3_NULL_SWEEP"); sweep = e ? 1 : 0; }
      if (sweep) { g_null_sweep_hi = 1; g_null_sweep_last = a;
                   g_null_sweep_tid = g_active_ctx
                                    ? (unsigned)g_active_ctx->thread_id : 0u; } }
    { static long n = 0;
      enum { NW_MAX = 8 };
      long i = n++;
      if (i < NW_MAX)
          fprintf(stderr, "[null-write] guest wrote%d 0x%X to 0x%08X (NULL+0x%X)"
                          " by guest-fn=0x%08X -- DROPPED\n",
                  width * 8, v, a, a, ppu_prof_resolve_host(ra));
      else if (i == NW_MAX)
          fprintf(stderr, "[null-write] further stores into the null page are"
                          " dropped without a word\n");
      { static uint32_t last_fn = 0; uint32_t fn = ppu_prof_resolve_host(ra);
        if (i < NW_MAX && fn != last_fn && g_active_ctx) {
            last_fn = fn; ppu_dump_guest_stack(g_active_ctx, "nw"); } }
      if (i <= NW_MAX) fflush(stderr); }
    return 1;
}


static int vm_null_sweep(uint32_t a)
{
    if (!g_active_ctx || (unsigned)g_active_ctx->thread_id != g_null_sweep_tid)
        return 0;
    uint32_t d = a > g_null_sweep_last ? a - g_null_sweep_last
                                       : g_null_sweep_last - a;
    if (d <= 0x1000u) {
        if (a > g_null_sweep_last) g_null_sweep_last = a;
        return 1;
    }
    g_null_sweep_hi = 0;
    { static int said = 0;
      if (!said) { said = 1;
          fprintf(stderr, "[null-write] sweep contained, ended at 0x%08X\n",
                  g_null_sweep_last); fflush(stderr); } }
    return 0;
}

static inline int vm_null_store(uint32_t a, uint32_t v, int width, void* ra)
{
    if (__builtin_expect(a >= 0x1000u, 1)) {
        if (__builtin_expect(g_null_sweep_hi == 0, 1)) return 0;
        return vm_null_sweep(a);
    }
    return vm_null_store_report(a, v, width, ra);
}

static inline uint8_t* vm_host(uint32_t a)
{
    uint8_t* shadow = spu_raw_is_reg(a) ? spu_raw_shadow(a) : nullptr;
    return shadow ? shadow : vm_base + a;
}

static inline __attribute__((always_inline)) void vm_write_coh(uint32_t a, const void* src, uint32_t n)
{
    if (spu_coh_line_reserved(a)) {
        spu_lockline_lock(a);
        memcpy(vm_host(a), src, n);
        spu_coh_notify_write(a);
        spu_lockline_unlock(a);
    } else {
        memcpy(vm_host(a), src, n);
        __atomic_signal_fence(__ATOMIC_SEQ_CST);
        if (spu_coh_line_reserved(a)) vm_coh_notify(a);
    }
    vm_track_write(a, n);
}

extern "C" const char* cellGame_get_boot_path(void);

extern "C" {
extern "C" int vm_inline_ok = 1;
extern "C" int vm_inline_st_ok = 0;
static struct VmInlineInit {
    VmInlineInit()
    {
        vm_inline_st_ok = !(getenv("PPU_WVAL") || getenv("PPU_WWATCH"));
        if (!vm_inline_st_ok || getenv("PS3_NULL_SWEEP")) ppu_special_pages(0, 1ull << 32);
        ppu_special_pages(0, 0x1000);
        for (uint32_t n = 0; n < SPU_RAW_COUNT; n++)
            ppu_special_pages(SPU_RAW_BASE + n * SPU_RAW_STRIDE + SPU_RAW_PROB_OFF, SPU_RAW_STRIDE - SPU_RAW_PROB_OFF);
    }
} s_vm_inline_init;

uint8_t  vm_read8 (uint64_t a) { if (vm_oob((uint32_t)a,1)) return 0; return *vm_host((uint32_t)a); }
uint16_t vm_read16(uint64_t a) { if (vm_oob((uint32_t)a,2)) return 0; uint16_t v; memcpy(&v, vm_host((uint32_t)a), 2);
    return __builtin_bswap16(v); }
uint32_t vm_read32(uint64_t a) { if (vm_oob((uint32_t)a,4)) return 0;
    if (spu_raw_is_reg((uint32_t)a)) { uint32_t _rv;
        if (spu_raw_reg_load((uint32_t)a, &_rv)) return _rv; }
    uint32_t v; memcpy(&v, vm_host((uint32_t)a), 4);
    return __builtin_bswap32(v); }
uint64_t vm_read64(uint64_t a) { if (vm_oob((uint32_t)a,8)) return 0; uint64_t v; memcpy(&v, vm_host((uint32_t)a), 8);
    return __builtin_bswap64(v); }

extern "C" uint32_t g_ww_lo = 0, g_ww_hi = 0;

extern "C" void ps3_ww_report_inline(uint32_t addr, uint64_t val, int width)
{
    static int n = 0;
    if (!PS3_LOG_FIRST(n, 64)) return;
    fprintf(stderr, "[ww-hle] 0x%08X <- 0x%llX (w%d) from an HLE (libs/)\n",
            addr, (unsigned long long)val, width);
    fflush(stderr);
}

static void ww_arm_inline_window(uint32_t ww)
{
    if (!ww) return;
    uint32_t len = 0x20;
    { const char* e = getenv("PPU_WWATCH_LEN");
      if (e) { len = (uint32_t)strtoul(e, 0, 0); if (len < 0x20) len = 0x20; } }
    g_ww_lo = ww & ~15u;
    g_ww_hi = (ww & ~15u) + len;
}
static int s_store_watch = -1;
static __attribute__((noinline)) void barrier_watch_hit_slow(uint32_t a, uint32_t v, int width, void* ra);
static inline void barrier_watch_hit(uint32_t a, uint32_t v, int width, void* ra)
{
    if (__builtin_expect(s_store_watch == 0, 1)) return;
    barrier_watch_hit_slow(a, v, width, ra);
}
static __attribute__((noinline)) void barrier_watch_hit_slow(uint32_t a, uint32_t v, int width, void* ra)
{
    if (s_store_watch < 0) s_store_watch = getenv("PPU_WVAL") || getenv("PPU_WWATCH") ? 1 : 0;
    { static uint32_t s_wv = 0xFFFFFFFFu, s_wvlo, s_wvhi;
      if (s_wv == 0xFFFFFFFFu) { const char* e = getenv("PPU_WVAL");
                                 s_wv = e ? (uint32_t)strtoul(e, 0, 0) : 0;
                                 const char* lo = getenv("PPU_WVAL_LO");
                                 const char* hi = getenv("PPU_WVAL_HI");
                                 s_wvlo = lo ? (uint32_t)strtoul(lo, 0, 0) : 0;
                                 s_wvhi = hi ? (uint32_t)strtoul(hi, 0, 0) : 0xFFFFFFFFu; }
      static const char* s_wvg = (const char*)-1;
      if (s_wvg == (const char*)-1) s_wvg = getenv("PPU_WVAL_GATE");
      if (s_wv && v == s_wv && a >= s_wvlo && a < s_wvhi &&
          (!s_wvg || GetFileAttributesA(s_wvg) != INVALID_FILE_ATTRIBUTES)) {
          static int _n = 0;
          if (PS3_LOG_FIRST(_n, 24)) {
              fprintf(stderr, "[wv] 0x%08X <- 0x%X (w%d) guest-fn=0x%08X\n",
                      a, v, width, ppu_prof_resolve_host(ra));
              extern PPU_THREAD_LOCAL ppu_context* g_active_ctx;
              extern void ppu_dump_guest_stack(ppu_context*, const char*);
              if (_n <= 3 && g_active_ctx) ppu_dump_guest_stack(g_active_ctx, "wv");
              fflush(stderr);
          }
      } }

    { static uint32_t s_ww = 0xFFFFFFFFu;
      if (s_ww == 0xFFFFFFFFu) { const char* e = getenv("PPU_WWATCH");
                                 s_ww = e ? (uint32_t)strtoul(e,0,16) : 0;
                                 ww_arm_inline_window(s_ww); }
      static uint32_t s_wwlen = 0;
      if (!s_wwlen) { const char* e2 = getenv("PPU_WWATCH_LEN");
                      s_wwlen = e2 ? (uint32_t)strtoul(e2,0,0) : 0x20;
                      if (s_wwlen < 0x20) s_wwlen = 0x20; }
      if (s_ww && a >= (s_ww & ~15u) && a < (s_ww & ~15u) + s_wwlen) {
          static long _n = 0;
          static long _cap = -1;
          if (_cap < 0) { const char* e = getenv("PPU_WWATCH_MAX");
                          _cap = e ? atol(e) : 64; }
          long _i = ++_n;
          if (_cap == 0 || _i <= _cap) {
              extern PPU_THREAD_LOCAL ppu_context* g_active_ctx;
              fprintf(stderr, "[ww] 0x%08X <- 0x%X (w%d) guest-fn=0x%08X"
                              " lr=0x%08X tid=%u\n",
                      a, v, width, ppu_prof_resolve_host(ra),
                      g_active_ctx ? (uint32_t)g_active_ctx->lr : 0u,
                      g_active_ctx ? (unsigned)g_active_ctx->thread_id : 0u);
              extern void ppu_dump_guest_stack(ppu_context*, const char*);
              if (_i == 1 && g_active_ctx) ppu_dump_guest_stack(g_active_ctx, "ww");
          } else if (_i == _cap + 1) {
              fprintf(stderr, "[ww] --- print cap %ld reached; further writes to "
                              "this window are NOT shown (raise PPU_WWATCH_MAX, "
                              "0 = unlimited) ---\n", _cap);
              fflush(stderr);
          }
      } }
}

extern "C" void (*g_rsx_window_write)(void);
static inline void ppu_window_note(uint32_t a, uint32_t n)
{
    void (*notify)(void) = g_rsx_window_write;
    if (notify && (a - ppu_hle_inject_base < 0xB000u || vm_watched(a, n))) notify();
}

static void store_block_at(uint32_t a, const void* src, uint32_t n, void* ra)
{
    if (vm_oob(a, n) || vm_null_store(a, 0, n, ra)) return;
    vm_write_coh(a, src, n);
    ppu_window_note(a, n);
}
static void store8_at(uint32_t a, uint8_t v, void* ra)
{
    barrier_watch_hit(a, v, 1, ra);
    if (vm_oob(a, 1) || vm_null_store(a, v, 1, ra)) return;
    vm_write_coh(a, &v, 1); ppu_window_note(a, 1);
}
static void store16_at(uint32_t a, uint16_t v, void* ra)
{
    barrier_watch_hit(a, v, 2, ra);
    if (vm_oob(a, 2) || vm_null_store(a, v, 2, ra)) return;
    v = __builtin_bswap16(v); vm_write_coh(a, &v, 2); ppu_window_note(a, 2);
}
static void store32_at(uint32_t a, uint32_t v, void* ra)
{
    barrier_watch_hit(a, v, 4, ra);
    if (vm_oob(a, 4) || vm_null_store(a, v, 4, ra)) return;
    { uint32_t _v = v;
      v = __builtin_bswap32(v); vm_write_coh(a, &v, 4); ppu_window_note(a, 4);
      if (spu_raw_is_reg(a)) spu_raw_reg_store(a, _v, 4); }
}
static void store64_at(uint32_t a, uint64_t v, void* ra)
{
    barrier_watch_hit(a,     (uint32_t)(v >> 32), 4, ra);
    barrier_watch_hit(a + 4, (uint32_t)v,         4, ra);
    if (vm_oob(a, 8) || vm_null_store(a, (uint32_t)(v >> 32), 8, ra)) return;
    v = __builtin_bswap64(v); vm_write_coh(a, &v, 8); ppu_window_note(a, 8);
}
extern "C" void ppu_store_block(uint64_t ea, const void* src, uint32_t n) { store_block_at((uint32_t)ea, src, n, __builtin_return_address(0)); }
extern "C" void ppu_store_recheck(uint64_t ea)
{
    const uint32_t a = (uint32_t)ea;
    if (spu_coh_line_reserved(a)) vm_coh_notify(a);
    vm_track_write(a, 1);
    ppu_window_note(a, 16);
}
void vm_write8 (uint64_t a, uint8_t  v) { store8_at((uint32_t)a, v, __builtin_return_address(0)); }
void vm_write16(uint64_t a, uint16_t v) { store16_at((uint32_t)a, v, __builtin_return_address(0)); }
void vm_write32(uint64_t a, uint32_t v) { store32_at((uint32_t)a, v, __builtin_return_address(0)); }
void vm_write64(uint64_t a, uint64_t v) { store64_at((uint32_t)a, v, __builtin_return_address(0)); }
__attribute__((used)) void ppu_store_slow(uint64_t w, uint64_t lo, uint64_t hi, void* ra)
{
    const uint32_t a = (uint32_t)w;
    switch ((uint32_t)(w >> 32)) {
    case 0: ppu_store_recheck(a); return;
    case 0x41: store8_at(a, (uint8_t)lo, ra); return;
    case 0x42: store16_at(a, __builtin_bswap16((uint16_t)lo), ra); return;
    case 0x44: store32_at(a, __builtin_bswap32((uint32_t)lo), ra); return;
    case 0x48: store64_at(a, __builtin_bswap64(lo), ra); return;
    default: {
        const uint32_t n = (uint32_t)(w >> 32);
        const uint64_t d[2] = {lo, hi};
        store_block_at(a, n > 16 ? (const void*)(uintptr_t)lo : d, n, ra);
    }
    }
}
}

#include "ppu_store_thunk.h"

extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*) = nullptr;

extern "C" PPU_THREAD_LOCAL ppu_context* g_active_ctx = nullptr;

typedef void (*ppu_fn)(ppu_context*);

#define PPU_HASH_BITS 18
#define PPU_HASH_SIZE (1u << PPU_HASH_BITS)
#define PPU_HASH_MASK (PPU_HASH_SIZE - 1)

static uint32_t g_addr[PPU_HASH_SIZE];
static ppu_fn   g_fn[PPU_HASH_SIZE];
static uint32_t g_fn_count = 0;
static uint32_t g_fn_gen = 0;

extern "C" void ps3_indirect_call(ppu_context* ctx);
extern "C" const int32_t* ppu_exec_tab = nullptr;
extern "C" uint32_t ppu_exec_lo = 0, ppu_exec_n = 0;

static inline uint32_t ppu_exec_index(uint32_t a)
{
    const uint32_t d = a - ppu_exec_lo;
    return d >> 2 | d << 30;
}

extern "C" void ppu_register_function(uint64_t addr, ppu_fn fn)
{
    uint32_t a = (uint32_t)addr;
    if (ppu_exec_index(a) < ppu_exec_n) const_cast<int32_t*>(ppu_exec_tab)[ppu_exec_index(a)] = 0;
    uint32_t i = (a * 2654435761u) & PPU_HASH_MASK;
    for (uint32_t probes = 0; probes < PPU_HASH_SIZE; probes++) {
        if (g_fn[i] == nullptr) { g_addr[i] = a; g_fn[i] = fn; g_fn_count++;
                                  __atomic_fetch_add(&g_fn_gen, 1u, __ATOMIC_RELEASE); return; }
        if (g_addr[i] == a) { g_fn[i] = fn;
                              __atomic_fetch_add(&g_fn_gen, 1u, __ATOMIC_RELEASE); return; }
        i = (i + 1) & PPU_HASH_MASK;
    }
    static int warned = 0;
    if (PS3_LOG_FIRST(warned, 4))
        fprintf(stderr, "[ppu] function table full (%u/%u) -- raise PPU_HASH_BITS; "
                "0x%08X dropped\n", g_fn_count, (unsigned)PPU_HASH_SIZE, a);
}

static struct { void* host; uint32_t guest; } g_inv[PPU_HASH_SIZE];
static uint32_t g_inv_n = 0;
static uint32_t ppu_prof_resolve_host_slow(void* ra);
extern "C" uint32_t ppu_prof_resolve_host(void* ra)
{
    struct RaSlot { void* ra; uint32_t guest; };
    static PPU_THREAD_LOCAL RaSlot t_ra[256] = {};
    RaSlot& slot = t_ra[((uintptr_t)ra >> 2) & 255u];
    if (slot.ra == ra && g_inv_n) return slot.guest;
    const uint32_t g = ppu_prof_resolve_host_slow(ra);
    if (g_inv_n) { slot.ra = ra; slot.guest = g; }
    return g;
}

static uint32_t ppu_prof_resolve_host_slow(void* ra)
{
    if (!g_inv_n) {
        for (uint32_t i = 0; i < PPU_HASH_SIZE; i++)
            if (g_fn[i]) { g_inv[g_inv_n].host = (void*)g_fn[i]; g_inv[g_inv_n].guest = g_addr[i]; g_inv_n++; }
        qsort(g_inv, g_inv_n, sizeof(g_inv[0]),
              [](const void* a, const void* b) -> int {
                  const void* ha = ((decltype(&g_inv[0]))a)->host;
                  const void* hb = ((decltype(&g_inv[0]))b)->host;
                  return ha < hb ? -1 : (ha > hb ? 1 : 0);
              });
        if (!g_inv_n) return 0;
    }
    uint32_t lo = 0, hi = g_inv_n;
    while (lo < hi) { uint32_t mid = (lo + hi) / 2;
        if (g_inv[mid].host <= ra) lo = mid + 1; else hi = mid; }
    return lo ? g_inv[lo - 1].guest : 0;
}

static ppu_fn ppu_lookup(uint32_t a);

static ppu_fn ppu_lookup_icall(uint32_t a)
{
    struct IcSlot { uint32_t addr, gen; ppu_fn fn; };
    static PPU_THREAD_LOCAL IcSlot t_ic[512] = {};
    IcSlot& s = t_ic[(a >> 2) & 511u];
    const uint32_t gen = __atomic_load_n(&g_fn_gen, __ATOMIC_ACQUIRE);
    if (s.fn && s.addr == a && s.gen == gen) return s.fn;
    ppu_fn fn = ppu_lookup(a);
    if (fn) { s.addr = a; s.gen = gen; s.fn = fn; }
    return fn;
}

static ppu_fn ppu_lookup(uint32_t a)
{
    uint32_t i = (a * 2654435761u) & PPU_HASH_MASK;
    for (uint32_t probes = 0; probes < PPU_HASH_SIZE; probes++) {
        if (g_fn[i] == nullptr) return nullptr;
        if (g_addr[i] == a) return g_fn[i];
        i = (i + 1) & PPU_HASH_MASK;
    }
    return nullptr;
}

extern "C" uint32_t ppu_function_count(void) { return g_fn_count; }

extern "C" void ppu_recomp_register(void)
{
    for (uint64_t i = 0; i < function_table_count; i++)
        ppu_register_function(function_table[i].addr, function_table[i].func);
    uint32_t lo = UINT32_MAX, hi = 0;
    for (uint64_t i = 0; i < function_table_lifted; i++) {
        const uint32_t a = (uint32_t)function_table[i].addr;
        if (a < lo) lo = a;
        if (a > hi) hi = a;
    }
    if (lo > hi) return;
    const uint32_t n = ((hi - lo) >> 2) + 1;
    int32_t* tab = (int32_t*)calloc(n, sizeof *tab);
    for (uint64_t i = 0; i < function_table_lifted && tab; i++) {
        const int64_t off = (const char*)function_table[i].func - (const char*)ps3_indirect_call;
        if (!(function_table[i].addr & 3) && off == (int32_t)off)
            tab[((uint32_t)function_table[i].addr - lo) >> 2] = (int32_t)off;
    }
    ppu_exec_tab = tab;
    ppu_exec_lo = lo;
    __atomic_store_n(&ppu_exec_n, tab ? n : 0, __ATOMIC_RELEASE);
    if (ppu_vm_size) ppu_special_pages(ppu_vm_size, (1ull << 32) - ppu_vm_size);
}

static uint32_t ppu_code_hi(void)
{
    static uint32_t hi = 0;
    if (!hi) {
        uint32_t m = 0;
        for (uint64_t k = 0; k < function_table_count; k++)
            if (function_table[k].addr > m) m = function_table[k].addr;
        hi = m ? m + 0x10000u : 0x600000u;
    }
    return hi;
}

static bool ppu_dump_readable(uint32_t ea, uint32_t n)
{
    if (vm_oob(ea, n)) return false;
#ifdef _WIN32
    return true;
#else
    static int s_probe[2] = {-1, -1};
    static const bool s_have_probe = pipe(s_probe) == 0;
    if (!s_have_probe) return true;
    const uintptr_t pg    = (uintptr_t)vm_host_page_size();
    const uintptr_t first = (uintptr_t)(vm_base + ea) & ~(pg - 1);
    const uintptr_t last  = (uintptr_t)(vm_base + ea + n - 1) & ~(pg - 1);
    for (uintptr_t a = first; a <= last; a += pg) {
        char c;
        if (write(s_probe[1], (const void*)a, 1) != 1) return false;
        if (read(s_probe[0], &c, 1) != 1) return false;
    }
    return true;
#endif
}

extern "C" void ppu_dump_guest_stack(ppu_context* ctx, const char* tag)
{
    if (!ctx || !vm_base) return;
    uint32_t sp = (uint32_t)ctx->gpr[1];
    fprintf(stderr, "[GSTACK:%s] sp=0x%08X cia=0x%08X lr=0x%08X r3=0x%08X"
                    " r4=0x%08X r5=0x%08X r28=0x%08X r29=0x%08X r31=0x%08X r30=0x%08X r13=0x%08X tid=%u\n",
            tag?tag:"?", sp, (uint32_t)ctx->cia, (uint32_t)ctx->lr,
            (uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4], (uint32_t)ctx->gpr[5],
            (uint32_t)ctx->gpr[28], (uint32_t)ctx->gpr[29],
            (uint32_t)ctx->gpr[31], (uint32_t)ctx->gpr[30],
            (uint32_t)ctx->gpr[13], (unsigned)ctx->thread_id);
    { char gr[400]; 
      for (int row = 0; row < 4; row++) {
          int gp2 = snprintf(gr, sizeof gr, "      r%-2d:", row * 8);
          for (int c = 0; c < 8; c++)
              gp2 += snprintf(gr + gp2, sizeof(gr) - gp2, " %08X",
                              (uint32_t)ctx->gpr[row * 8 + c]);
          fprintf(stderr, "%s\n", gr);
      } }
    if (ppu_dump_readable(sp,4)) { char rw[600]; int rp=snprintf(rw,sizeof rw,"      rawstk:");
        for (int i=0;i<24 && ppu_dump_readable(sp+i*4,4);i++){ uint32_t t; memcpy(&t,vm_base+sp+i*4,4); rp+=snprintf(rw+rp,sizeof(rw)-rp," %08X",__builtin_bswap32(t)); }
        fprintf(stderr,"%s\n",rw); }
    { char bc[900]; int bp = snprintf(bc, sizeof bc, "[GSTACK:%s] chain:", tag ? tag : "?");
      uint32_t f = sp;
      for (int d = 0; d < 24 && bp < 820; d++) {
          if (!ppu_dump_readable(f, 8)) break;
          uint32_t t; memcpy(&t, vm_base + f + 4, 4);
          uint32_t prev = __builtin_bswap32(t);
          if (prev <= f || !ppu_dump_readable(prev + 0x14, 4)) break;
          memcpy(&t, vm_base + prev + 0x14, 4);
          uint32_t ra = __builtin_bswap32(t);
          if (ra >= 0x10000 && ra < ppu_code_hi()) {
              uint32_t bg = 0;
              for (uint64_t k = 0; k < function_table_count; k++) {
                  uint32_t aa = function_table[k].addr; if (aa <= ra && aa > bg) bg = aa; }
              if (bg) bp += snprintf(bc+bp, sizeof(bc)-bp, " func_%08X+0x%X", bg, ra - bg);
              else    bp += snprintf(bc+bp, sizeof(bc)-bp, " 0x%08X", ra);
          }
          f = prev;
      }
      fprintf(stderr, "%s\n", bc); }
    char gs[1200]; int gp = snprintf(gs, sizeof gs, "[GSTACK:%s] sp=0x%08X:", tag ? tag : "?", sp);
    uint32_t last = 0;
    for (int i = 0; i < 700 && gp < 1100; i++) {
        uint32_t a = sp + i*4; if (!ppu_dump_readable(a,4)) break;
        uint32_t t; memcpy(&t, vm_base + a, 4); uint32_t w = __builtin_bswap32(t);
        if (w < 0x10000 || w >= ppu_code_hi()) continue;
        uint32_t bg = 0;
        for (uint64_t k = 0; k < function_table_count; k++) { uint32_t aa = function_table[k].addr; if (aa <= w && aa > bg) bg = aa; }
        if (bg && (w - bg) > 0 && (w - bg) < 0x4000 && w != last) {
            gp += snprintf(gs+gp, sizeof(gs)-gp, " func_%08X+0x%X", bg, w - bg); last = w;
        }
    }
    fprintf(stderr, "%s\n", gs);
}

static PPU_THREAD_LOCAL uint32_t g_icall_target;
static PPU_THREAD_LOCAL int      g_icall_target_set;

[[noreturn]] static void ppu_bad_call(ppu_context* ctx, uint32_t addr, const char* what)
{
    char who[64];
    ppu_guest_caller(who, sizeof who);
    fprintf(stderr, "\n[ppu] FATAL: %s 0x%08X is not lifted code (from %s, tid=%llu lr=0x%08X)\n", what, addr, who,
            (unsigned long long)ctx->thread_id, (uint32_t)ctx->lr);
    for (int r = 0; r < 32; r += 8)
        fprintf(stderr, "[ppu]   r%-2d: %08X %08X %08X %08X %08X %08X %08X %08X\n", r, (uint32_t)ctx->gpr[r], (uint32_t)ctx->gpr[r + 1],
                (uint32_t)ctx->gpr[r + 2], (uint32_t)ctx->gpr[r + 3], (uint32_t)ctx->gpr[r + 4], (uint32_t)ctx->gpr[r + 5],
                (uint32_t)ctx->gpr[r + 6], (uint32_t)ctx->gpr[r + 7]);
    for (int r = 9; r <= 12; r++) {
        const uint32_t p = (uint32_t)ctx->gpr[r];
        if (p && (!ppu_vm_size || p + 16 <= ppu_vm_size))
            fprintf(stderr, "[ppu]   [r%d=0x%08X] %08X %08X %08X %08X\n", r, p, vm_read32(p), vm_read32(p + 4), vm_read32(p + 8),
                    vm_read32(p + 12));
    }
    ppu_dump_guest_stack(ctx, "bad-call");
    fflush(stderr);
    _Exit(3);
}

extern "C" void ps3_indirect_call(ppu_context* ctx)
{
    uint32_t addr = (uint32_t)ctx->ctr;
    if (g_icall_target_set) {
        addr = g_icall_target;
        g_icall_target_set = 0;
    }
    const ppu_fn fn = ppu_lookup_icall(addr);
    if (!fn) ppu_bad_call(ctx, addr, "indirect call to");
    fn(ctx);
    while (g_trampoline_fn) {
        void (*tf)(void*) = g_trampoline_fn;
        g_trampoline_fn = 0;
        tf(ctx);
    }
}

extern "C" void ps3_indirect_call_to(ppu_context* ctx, uint32_t target)
{
    g_icall_target = target;
    g_icall_target_set = 1;
    ps3_indirect_call(ctx);
}

extern "C" int lv2_try_syscall(ppu_context* ctx);
extern "C" int64_t sys_mmapper_allocate_address(ppu_context* ctx);
extern "C" int64_t sys_memory_get_user_memory_size(ppu_context* ctx);
extern "C" int64_t sys_mmapper_search_map_address(uint32_t start, uint32_t size, uint32_t* out);

extern "C" void lv2_syscall(ppu_context* ctx)
{
    uint64_t num = ctx->gpr[11];
    ps3_msf("sys:%u", (unsigned)num);

    struct _ScGuard {
        ppu_context* c; uint64_t num, t0; uint32_t lr;
        ~_ScGuard() { unsigned t = (unsigned)c->thread_id;
                      if (PS3_FT_ON() && t0) {
                          const uint64_t t1 = ps3_ft_now();
                          if (num == 141) ps3_ft_usleep(t, lr, t0, t1);
                          else if (t1 - t0 >= ps3_ft_min_ns())
                              ps3_ft_span(PS3_FT_SYSCALL, t, (uint32_t)num, lr, t0, t1);
                      } }
    } _sg{ ctx, num, PS3_FT_ON() ? ps3_ft_now() : 0, (uint32_t)ctx->lr };
    switch (num) {
    case 352: {
        ctx->gpr[3] = (uint64_t)sys_memory_get_user_memory_size(ctx);
        return;
    }
    case 330: {
        int64_t rc = sys_mmapper_allocate_address(ctx);
        ctx->gpr[3] = (uint64_t)rc;
        return;
    }
    case 337: {
        extern uint32_t ps3_mmapper_block_size(uint32_t mem_id);
        uint32_t start  = (uint32_t)ctx->gpr[3];
        uint32_t mem_id = (uint32_t)ctx->gpr[4];
        uint32_t outp   = (uint32_t)ctx->gpr[6];
        uint32_t size = ps3_mmapper_block_size(mem_id);
        if (!size) size = 0x100000u;
        uint32_t va = 0;
        int64_t rc = sys_mmapper_search_map_address(start, size, &va);
        if (rc != CELL_OK) {
            ctx->gpr[3] = (uint64_t)rc;
            return;
        }
        if (outp) vm_write32(outp, va);
        ctx->gpr[3] = 0;
        return;
    }
    case 988:
    case 403: {
        uint32_t buf  = (uint32_t)ctx->gpr[4];
        uint32_t len  = (uint32_t)ctx->gpr[5];
        uint32_t pwl  = (uint32_t)ctx->gpr[6];
        if (len && (!buf || (uint64_t)buf + len > 0x100000000ull)) {
            ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x8001000D;
            return;
        }
        uint32_t wlen = len;
        { static char last[128]; static unsigned long long run_len = 0;
          if (vm_base && wlen > 0 && wlen < sizeof(last)) {
              char cur[128]; uint32_t cn = wlen;
              for (uint32_t i = 0; i < cn; i++) cur[i] = (char)vm_read8(buf + i);
              cur[cn] = 0;
              if (run_len && strcmp(cur, last) == 0) {
                  run_len++;
                  if (run_len > 4 && (run_len % 1000ull) != 0) {
                      if (pwl) vm_write32(pwl, wlen);
                      ctx->gpr[3] = 0;
                      return;
                  }
                  fprintf(stderr, "[tty] (x%llu) ", (unsigned long long)run_len);
              } else {
                  if (run_len > 4)
                      fprintf(stderr, "[tty] previous line repeated %llu times%c",
                              (unsigned long long)run_len, 10);
                  memcpy(last, cur, cn + 1);
                  run_len = 1;
              }
          } }

        FILE* out = stdout;
        {
            char tty[0x4001];
            uint32_t n = 0;
            for (uint32_t i = 0; i < wlen && n < sizeof tty - 1; i++) {
                if (ppu_vm_size && buf + i >= ppu_vm_size) break;
                tty[n++] = (char)vm_read8(buf + i);
            }
            if (n) fwrite(tty, 1, n, out);
        }
        fflush(out);
        if (pwl) vm_write32(pwl, len);
        ctx->gpr[3] = 0;
        return;
    }
    default: {
        if (lv2_try_syscall(ctx)) return;
        static std::atomic<uint32_t> reported[1024 / 32];
        const unsigned slot = (unsigned)(num & 1023u);
        if (!(reported[slot >> 5].fetch_or(1u << (slot & 31u), std::memory_order_relaxed) & (1u << (slot & 31u))))
            fprintf(stderr, "[ppu] lv2_syscall %llu is not implemented (ENOSYS)\n", (unsigned long long)num);
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010003;
        return;
    }
    }
}

static uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static uint64_t be64(const uint8_t* p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }

struct ppu_image_range { uint64_t begin, end; };
static std::vector<ppu_image_range> g_ppu_image_ranges;
extern "C" void sys_memory_set_image_query(uint64_t (*query)(uint64_t, uint64_t));

static uint64_t ppu_image_overlap_end(uint64_t addr, uint64_t end)
{
    uint64_t next = addr;
    for (const auto& r : g_ppu_image_ranges)
        if (addr < r.end && r.begin < end && r.end > next) next = r.end;
    return next;
}
static int32_t g_ppu_primary_priority = 1001;

extern "C" uint32_t ppu_load_elf_image(const uint8_t* file, uint64_t fsz)
{
    g_ppu_primary_priority = 1001;
    if (fsz < 64 || memcmp(file, "\x7F""ELF", 4) != 0 || file[4] != 2 || file[5] != 2) {
        fprintf(stderr, "[ppu] not a 64-bit big-endian ELF\n"); return 0;
    }
    uint32_t entry = (uint32_t)be64(file + 24);
    uint64_t phoff = be64(file + 32);
    uint16_t phentsize = be16(file + 54);
    uint16_t phnum = be16(file + 56);

    g_ppu_image_ranges.clear();
    sys_memory_set_image_query(ppu_image_overlap_end);
    int loaded = 0;
    for (uint16_t i = 0; i < phnum; i++) {
        uint64_t ph_off = phoff + (uint64_t)i * phentsize;
        if (ph_off + 56 > fsz) break;
        const uint8_t* ph = file + ph_off;
        uint32_t p_type = be32(ph + 0);
        if (p_type == 0x60000001u) {
            uint64_t offset = be64(ph + 8), size = be64(ph + 32);
            if (offset <= fsz && size >= 0x20 && size <= fsz - offset) {
                const uint8_t* param = file + offset;
                uint32_t declared = be32(param);
                int32_t priority = (int32_t)be32(param + 0x10);
                if (declared >= 0x20 && declared <= size &&
                    be32(param + 4) == 0x13BCC5F6u &&
                    priority >= 0 && priority <= 3071) {
                    g_ppu_primary_priority = priority;
                    fprintf(stderr, "[ppu] primary thread priority %d\n", priority);
                }
            }
            continue;
        }
        if (p_type == 7) {
            g_tls_vaddr  = (uint32_t)be64(ph + 16);
            g_tls_filesz = (uint32_t)be64(ph + 32);
            g_tls_memsz  = (uint32_t)be64(ph + 40);
            fprintf(stderr, "[ppu] PT_TLS template vaddr=0x%08X filesz=0x%X memsz=0x%X\n",
                    g_tls_vaddr, g_tls_filesz, g_tls_memsz);
            continue;
        }
        if (p_type != 1) continue;
        uint64_t p_offset = be64(ph + 8);
        uint64_t p_vaddr  = be64(ph + 16);
        uint64_t p_filesz = be64(ph + 32);
        uint64_t p_memsz  = be64(ph + 40);
        if (p_memsz == 0) continue;
        if (p_offset > fsz || p_filesz > fsz || p_offset + p_filesz > fsz) {
            fprintf(stderr, "[ppu] segment %u file range 0x%llX+0x%llX exceeds ELF (0x%llX), skipped\n",
                    i, (unsigned long long)p_offset, (unsigned long long)p_filesz, (unsigned long long)fsz);
            continue;
        }
        if (ppu_vm_size && (p_vaddr > ppu_vm_size || p_memsz > ppu_vm_size ||
                            p_vaddr + p_memsz > ppu_vm_size)) {
            fprintf(stderr, "[ppu] segment %u vaddr 0x%llX+0x%llX exceeds VM (0x%X), skipped\n",
                    i, (unsigned long long)p_vaddr, (unsigned long long)p_memsz, ppu_vm_size);
            continue;
        }
        memcpy(vm_base + (uint32_t)p_vaddr, file + p_offset, (size_t)p_filesz);
        if (p_memsz > p_filesz)
            memset(vm_base + (uint32_t)p_vaddr + (uint32_t)p_filesz, 0, (size_t)(p_memsz - p_filesz));
        g_ppu_image_ranges.push_back({p_vaddr, p_vaddr + (p_memsz > p_filesz ? p_memsz : p_filesz)});
        loaded++;
    }
    fprintf(stderr, "[ppu] loaded %d PT_LOAD segments, entry OPD 0x%08X\n", loaded, entry);
    return entry;
}

extern "C" uint32_t ppu_load_elf(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[ppu] cannot open %s\n", path); return 0; }
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* file = sz > 0 ? (uint8_t*)malloc((size_t)sz) : nullptr;
    const bool ok = file && fread(file, 1, (size_t)sz, f) == (size_t)sz;
    fclose(f);
    const uint32_t entry = ok ? ppu_load_elf_image(file, (uint64_t)sz) : 0;
    free(file);
    return entry;
}

extern "C" int ppu_opd_resolve(uint32_t opd, uint32_t* code, uint32_t* toc)
{
    uint32_t c = vm_read32(opd);
    uint32_t t = vm_read32(opd + 4);
    if (code) *code = c;
    if (toc)  *toc  = t;
    return c != 0;
}

extern "C" void (*g_ppu_thread_entry_trampoline)(ppu_context*);
static uint32_t ppu_tls_new_block(void)
{
    if (!g_tls_memsz) return 0;
    uint32_t total = (g_tls_memsz + 0xFFFu) & ~0xFFFu;
    uint32_t lim   = 0x11000000u;
    if (ppu_vm_size && ppu_vm_size < lim) lim = ppu_vm_size;
    static uint32_t s_bump = 0;
    uint32_t off  = __atomic_fetch_add(&s_bump, total, __ATOMIC_RELAXED);
    uint32_t base = PPU_TLS_IMG + total + off;
    if (base + total > lim) {
        static int s_warned = 0;
        if (!__atomic_exchange_n(&s_warned, 1, __ATOMIC_RELAXED))
            fprintf(stderr, "[ppu] TLS window full after %u thread blocks: later "
                    "threads share the main thread's TLS\n", (unsigned)(off / total));
        return 0;
    }
    if (g_tls_filesz) memcpy(vm_base + base, vm_base + g_tls_vaddr, g_tls_filesz);
    if (g_tls_memsz > g_tls_filesz)
        memset(vm_base + base + g_tls_filesz, 0, g_tls_memsz - g_tls_filesz);
    return base + 0x7000u;
}

static void ppu_thread_entry_trampoline(ppu_context* ctx)
{
    uint32_t code = 0, toc = 0;
    ppu_opd_resolve((uint32_t)ctx->cia, &code, &toc);
    if (toc) ctx->gpr[2] = toc;
    if (!ctx->gpr[13]) { uint32_t tp = ppu_tls_new_block();
                         ctx->gpr[13] = tp ? tp : PPU_TLS_TP; }
    ppu_fn fn = ppu_lookup(code);
    if (!fn) {
        fprintf(stderr, "[ppu] thread entry 0x%08X (cia=0x%08X) not registered\n",
                code, (uint32_t)ctx->cia);
        return;
    }
    g_active_ctx = ctx;
    fn(ctx);
    while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = 0; tf(ctx); }
}

static std::atomic<uint64_t> s_cb_slots_used{0};
struct cb_slot_owner {
    int slot = -1;
    ~cb_slot_owner()
    {
        if (slot >= 0) s_cb_slots_used.fetch_and(~(1ull << slot), std::memory_order_acq_rel);
    }
};
static thread_local cb_slot_owner t_cb_slot;

static uint32_t ppu_cb_stack_sp(const ppu_context* active)
{
    static PPU_THREAD_LOCAL uint32_t s_lo = 0, s_hi = 0;
    if (!s_hi) {
        const uint32_t slots = VM_CB_STACK_REGION / VM_CB_STACK_SIZE;
        static_assert(VM_CB_STACK_REGION / VM_CB_STACK_SIZE <= 64, "one bit per slot");
        uint32_t i = slots;
        uint64_t used = s_cb_slots_used.load(std::memory_order_acquire);
        while (~used & (slots >= 64 ? ~0ull : (1ull << slots) - 1u)) {
            uint32_t k = 0;
            while (used >> k & 1u) k++;
            if (s_cb_slots_used.compare_exchange_weak(used, used | (1ull << k),
                                                      std::memory_order_acq_rel)) {
                i = k;
                t_cb_slot.slot = (int)k;
                break;
            }
        }
        if (i < slots) {
            s_lo = VM_CB_STACK_BASE + i * VM_CB_STACK_SIZE;
            s_hi = s_lo + VM_CB_STACK_SIZE;
        } else {
            s_lo = VM_CB_STACK_BASE + (slots - 1) * VM_CB_STACK_SIZE;
            s_hi = s_lo + VM_CB_STACK_SIZE;
            fprintf(stderr, "[ppu] guest_call: more than %u live host threads make guest "
                            "callbacks; callback stack slot %u is SHARED\n", slots, slots - 1);
        }
    }
    if (active) {
        uint32_t sp = (uint32_t)active->gpr[1];
        if (sp > s_lo + 0x1000 && sp <= s_hi) return (sp - 0x400) & ~0xFu;
    }
    return (s_hi - 0x100) & ~0xFu;
}

extern "C" uint64_t ppu_guest_call(uint32_t opd_addr,
                                   uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                   uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7)
{
    if (!opd_addr) return 0;
    uint32_t code = 0, toc = 0;
    ppu_opd_resolve(opd_addr, &code, &toc);
    ppu_fn fn = ppu_lookup(code);
    if (!fn) {
        fprintf(stderr, "\n[ppu] FATAL: guest callback OPD 0x%08X -> code 0x%08X is not lifted code (opd %08X %08X)\n", opd_addr, code,
                vm_read32(opd_addr), vm_read32(opd_addr + 4));
        if (g_active_ctx) ppu_dump_guest_stack(g_active_ctx, "bad-callback");
        fflush(stderr);
        _Exit(3);
    }

    ppu_context ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.vscr = 0x00010000u;
    ctx.gpr[1]  = ppu_cb_stack_sp(g_active_ctx);
    ctx.gpr[2]  = toc;
    ctx.gpr[3]  = a0; ctx.gpr[4] = a1; ctx.gpr[5] = a2; ctx.gpr[6] = a3;
    ctx.gpr[7]  = a4; ctx.gpr[8] = a5; ctx.gpr[9] = a6; ctx.gpr[10] = a7;
    ctx.gpr[13] = PPU_TLS_TP;
    ctx.cia     = code;
    ppu_context* saved_active = g_active_ctx;
    ctx.thread_id = saved_active ? saved_active->thread_id
                                 : (0x7FFF0000ull | (GetCurrentThreadId() & 0xFFFFu));
    g_active_ctx = &ctx;
    fn(&ctx);
    while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = 0; tf(&ctx); }
    g_active_ctx = saved_active;
    return ctx.gpr[3];
}

struct ppu_active_context_scope {
    ppu_context* previous;
    explicit ppu_active_context_scope(ppu_context* current) : previous(g_active_ctx) {
        g_active_ctx = current;
    }
    ~ppu_active_context_scope() { g_active_ctx = previous; }
};

extern "C" int ppu_guest_call_ct_checked(uint32_t code, uint32_t toc,
                                      uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                      uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7,
                                      uint64_t* result)
{
    if (!result) return 0;
    *result = 0;
    if (!code) return 0;
    ppu_fn fn = ppu_lookup(code);
    if (!fn) { fprintf(stderr, "[ppu] guest_call_ct: code 0x%08X not registered\n", code); return 0; }

    ppu_context ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.vscr = 0x00010000u;
    ctx.gpr[1]  = ppu_cb_stack_sp(g_active_ctx);
    ctx.gpr[2]  = toc;
    ctx.gpr[3]  = a0; ctx.gpr[4] = a1; ctx.gpr[5] = a2; ctx.gpr[6] = a3;
    ctx.gpr[7]  = a4; ctx.gpr[8] = a5; ctx.gpr[9] = a6; ctx.gpr[10] = a7;
    ctx.gpr[13] = PPU_TLS_TP;
    ctx.cia     = code;
    ppu_context* saved_active = g_active_ctx;
    ctx.thread_id = saved_active ? saved_active->thread_id
                                 : (0x7FFF0000ull | (GetCurrentThreadId() & 0xFFFFu));
    ppu_active_context_scope active_scope(&ctx);
    struct callback_trampoline_scope {
        void (*previous)(void*);
        callback_trampoline_scope() : previous(g_trampoline_fn) { g_trampoline_fn = nullptr; }
        ~callback_trampoline_scope() { g_trampoline_fn = previous; }
    } trampoline_scope;

    fn(&ctx);
    while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = 0; tf(&ctx); }
    *result = ctx.gpr[3];
    return 1;
}

extern "C" uint64_t ppu_guest_call_ct(uint32_t code, uint32_t toc,
                                      uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                      uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7)
{
    uint64_t result = 0;
    ppu_guest_call_ct_checked(code,toc,a0,a1,a2,a3,a4,a5,a6,a7,&result);
    return result;
}

extern "C" ppu_context* volatile g_ppu_main_ctx = nullptr;

extern "C" int ppu_run(uint32_t entry_opd, uint32_t stack_top)
{
    ps3_stdio_init();

    g_ppu_thread_entry_trampoline = ppu_thread_entry_trampoline;
    uint32_t code = 0, toc = 0;
    ppu_opd_resolve(entry_opd, &code, &toc);
    ppu_fn fn = ppu_lookup(code);
    if (!fn) { fprintf(stderr, "[ppu] entry code 0x%08X not registered\n", code); return -1; }

    ppu_context ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.vscr = 0x00010000u;
    ctx.gpr[1] = stack_top;
    ctx.gpr[2] = toc;

    { extern uint64_t ppu_thread_register_main_priority(int32_t);
      ctx.thread_id = ppu_thread_register_main_priority(g_ppu_primary_priority); }
    { extern void ppu_thread_set_main_stack(uint32_t, uint32_t);
      ppu_thread_set_main_stack(stack_top - VM_PPU_STACK_SIZE, VM_PPU_STACK_SIZE); }
    g_ppu_main_ctx = &ctx;

    ctx.gpr[7]  = 1;
    ctx.gpr[8]  = g_tls_vaddr;
    ctx.gpr[9]  = g_tls_filesz;
    ctx.gpr[10] = g_tls_memsz;
    ctx.gpr[12] = 0x100000;

    if (g_tls_memsz && (PPU_TLS_IMG + g_tls_memsz < (ppu_vm_size ? ppu_vm_size : 0x11000000u))) {
        memcpy(vm_base + PPU_TLS_IMG, vm_base + g_tls_vaddr, g_tls_filesz);
        if (g_tls_memsz > g_tls_filesz)
            memset(vm_base + PPU_TLS_IMG + g_tls_filesz, 0, g_tls_memsz - g_tls_filesz);
        ctx.gpr[13] = PPU_TLS_TP;
        fprintf(stderr, "[ppu] TLS image 0x%08X (r13/TP=0x%08X)\n", PPU_TLS_IMG, PPU_TLS_TP);
    }
    {
        const char* boot_path = cellGame_get_boot_path();

        const char* extra = getenv("PS3_ARGV");
        const char* argp[32]; uint32_t argc_n = 0;
        argp[argc_n++] = boot_path;
        static char argbuf[1024]; argbuf[0] = 0;
        if (extra && *extra) {
            snprintf(argbuf, sizeof argbuf, "%s", extra);
            char* s = argbuf;
            while (*s && argc_n < 31) {
                argp[argc_n++] = s;
                char* semi = strchr(s, 59);
                if (!semi) break;
                *semi = 0; s = semi + 1;
            }
        }

        uint32_t slots = (argc_n + 2u) * 8u;
        uint64_t strings_offset = (slots + 0x1Fu) & ~uint64_t(0x1F);
        uint64_t bytes = strings_offset;
        for (uint32_t a = 0; a < argc_n; a++)
            bytes += (strlen(argp[a]) + 1u + 0xFu) & ~uint64_t(0xF);
        const uint32_t entry_frame = 0x80;
        uint64_t reserve = (entry_frame + bytes + 0x1Fu) & ~uint64_t(0x1F);
        uint64_t top = stack_top & ~uint64_t(0x1F);
        uint64_t vm_end = ppu_vm_size ? ppu_vm_size : 0x100000000ull;
        if (reserve > 0x10000 || top > vm_end || top < VM_MAIN_MEM_BASE + reserve) {
            fprintf(stderr, "[ppu] invalid entry argument stack reservation\n");
            return -1;
        }
        uint32_t entry_sp = (uint32_t)(top - reserve);
        for (const auto& range : g_ppu_image_ranges) {
            if (entry_sp < range.end && top > range.begin) {
                fprintf(stderr, "[ppu] entry argument stack overlaps loaded image\n");
                return -1;
            }
        }
        ctx.gpr[1] = entry_sp;
        for (uint32_t off = 0; off < entry_frame; off += 8)
            vm_write64(entry_sp + off, 0);
        uint32_t argv_base = entry_sp + entry_frame;
        uint32_t str_addr = argv_base + (uint32_t)strings_offset;
        uint32_t sp_cur    = str_addr;
        for (uint32_t a = 0; a < argc_n; a++) {
            uint32_t j = 0;
            for (; argp[a][j]; j++) vm_write8(sp_cur + j, (uint8_t)argp[a][j]);
            vm_write8(sp_cur + j, 0);
            vm_write32(argv_base + a * 8u + 0, 0);
            vm_write32(argv_base + a * 8u + 4, sp_cur);
            sp_cur = (sp_cur + j + 1u + 0xFu) & ~0xFu;
        }
        vm_write32(argv_base + argc_n * 8u + 0, 0);
        vm_write32(argv_base + argc_n * 8u + 4, 0);
        vm_write32(argv_base + (argc_n + 1u) * 8u + 0, 0);
        vm_write32(argv_base + (argc_n + 1u) * 8u + 4, 0);
        ctx.gpr[3] = argc_n;
        ctx.gpr[4] = argv_base;
        ctx.gpr[5] = argv_base + (argc_n + 1u) * 8u;
        ctx.gpr[6] = 0;
        for (uint32_t a = 0; a < argc_n; a++) {
            uint32_t pa = __builtin_bswap32(*(uint32_t*)(vm_base + argv_base + a * 8u + 4));
            char rb[80]; uint32_t k = 0;
            for (; k < 79; k++) { rb[k] = (char)vm_base[pa + k]; if (!rb[k]) break; }
            rb[79] = 0;
            fprintf(stderr, "[ppu] argv[%u] @0x%08X = \"%s\"\n", a, pa, rb);
        }
        fprintf(stderr, "[ppu] argc=%u argv=0x%08X\n", argc_n, argv_base);
    }
    fprintf(stderr, "[ppu] run: code 0x%08X, toc 0x%08X, sp 0x%08X\n", code, toc, (uint32_t)ctx.gpr[1]);
    g_active_ctx = &ctx;
    fn(&ctx);
    while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = 0; tf(&ctx); }
    return 0;
}
