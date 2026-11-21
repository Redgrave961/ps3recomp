#define PPU_NO_INLINE_LOADS
#include "ppu_recomp.h"
#include "ps3emu/nid.h"
#include "ps3emu/milestone.h"
#include "ps3emu/ps3_ftrace.h"
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

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

typedef void (*hle_ctx_fn)(ppu_context*);
#define HLE_CTX_CAP 256
static struct { uint32_t nid; hle_ctx_fn fn; const char* name; } g_ctx[HLE_CTX_CAP];
static uint32_t g_ctx_count = 0;

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
        g_ctx[g_ctx_count].name = name ? name : "?";
        g_ctx_count++;
    }
}

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

typedef uint64_t (*hle_generic)(uint64_t, uint64_t, uint64_t, uint64_t,
                                uint64_t, uint64_t, uint64_t, uint64_t);

void vm_write64(uint64_t addr, uint64_t val);

extern "C" uint32_t    g_last_hle_nid  = 0;
extern "C" const char* g_last_hle_name = "";

#define PS3_HLE_INFLIGHT_MAX 64
extern "C" const char* g_hle_inflight[PS3_HLE_INFLIGHT_MAX] = { nullptr };

extern "C" uint32_t prx_resolve_export(uint32_t nid);
extern "C" void     ps3_indirect_call(ppu_context* ctx);
extern "C" uint32_t vm_read32(uint64_t a);
#include "../platform/win32_backtrace.h"

extern "C" void ps3_hle_call(uint32_t nid, ppu_context* ctx)
{
    if (const hle_fast_fn f = hle_fast(nid))
        if (f(ctx)) return;

    struct _TocGuard { ppu_context* c; uint64_t toc, sp, ft0; uint32_t nid, lr;
        ~_TocGuard(){ c->gpr[2] = toc; vm_write64(sp + 0x28, toc);
                      unsigned t = (unsigned)c->thread_id;
                      if (t < PS3_HLE_INFLIGHT_MAX) g_hle_inflight[t] = nullptr;
                      if (ft0) { const uint64_t t1 = ps3_ft_now();
                                 if (t1 - ft0 >= ps3_ft_min_ns())
                                     ps3_ft_span(PS3_FT_HLE, t, nid, lr, ft0, t1); } }
    } _tg{ ctx, ctx->gpr[2], ctx->gpr[1], PS3_FT_ON() ? ps3_ft_now() : 0, nid, (uint32_t)ctx->lr };
    g_last_hle_nid = nid;
    g_last_hle_name = "(unimplemented)";

    if (nid == 0xE6F2C1E7u && (uint32_t)ctx->gpr[3] != 0) {
        uint32_t sp = (uint32_t)ctx->gpr[1];
        fprintf(stderr, "[exit-chain] code=0x%X lr=0x%08X sp=0x%08X\n", (uint32_t)ctx->gpr[3], (uint32_t)ctx->lr, sp);
        { uint32_t prev=0; int found=0; char b[1600]; int p=snprintf(b,sizeof b,"[exit-chain] codeptrs:");
          for(uint32_t a=sp; a<sp+0x3000 && a<0x0FF00000u && found<60; a+=4){ uint32_t v=vm_read32(a);
            if((v>=0x00010000u&&v<0x00900000u)||(v>=0x30000000u&&v<0x30100000u)){
              if(v!=prev){ p+=snprintf(b+p,sizeof(b)-p," %08X",v); prev=v; found++; } } }
          fprintf(stderr,"%s\n",b); }
        { void* bt[40]; unsigned short fr=RtlCaptureStackBackTrace(0,40,bt,0);
          char* mb=(char*)GetModuleHandleA(0); char line[1100]; int p=snprintf(line,sizeof line,"[exit-chain] HOST-bt rva:");
          for(int i=0;i<fr;i++) p+=snprintf(line+p,sizeof(line)-p," %llX",(unsigned long long)((char*)bt[i]-mb));
          fprintf(stderr,"%s\n",line); }
    }
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
        ctx->gpr[3] = 0;
        return;
    }
    g_last_hle_name = e->name;
    ps3_msf("hle:%s", e->name);
    { unsigned t = (unsigned)ctx->thread_id;
      if (t < PS3_HLE_INFLIGHT_MAX) g_hle_inflight[t] = e->name; }
    hle_generic fn = (hle_generic)e->handler;
    uint64_t r = fn(ctx->gpr[3], ctx->gpr[4], ctx->gpr[5], ctx->gpr[6],
                    ctx->gpr[7], ctx->gpr[8], ctx->gpr[9], ctx->gpr[10]);
    ctx->gpr[3] = r;
}

extern "C" void ppu_hle_register_all(void) __attribute__((weak));
extern "C" void ppu_hle_register_all(void) {}

extern "C" void ppu_hle_init(void)
{
    ppu_hle_register_all();

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
