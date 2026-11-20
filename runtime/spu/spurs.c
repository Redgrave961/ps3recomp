#include "spurs.h"
#include "spu_dma.h"

#include <setjmp.h>
#include <stdio.h>
#include <string.h>

void spu_begin_image(int image_id);
void spu_register_function(uint32_t addr, void (*fn)(spu_context*));
int spu_run_with_halt(void (*entry)(spu_context*), spu_context* ctx);
void spu_halt(spu_context* ctx);
void spu_stop(spu_context* ctx);
void spu_wrch(spu_context* ctx, uint32_t channel, u128 value);
u128 spu_rdch(spu_context* ctx, uint32_t channel);
uint32_t spu_rchcnt(spu_context* ctx, uint32_t channel);

enum { SPURS_IMAGE = 99 };

static uint64_t s_pm_service, s_pm_taskset, s_pm_jobchain, s_pm_jobchain_plain;

static inline uint32_t g16(const uint8_t* p) { return (uint32_t)p[0] << 8 | p[1]; }
static inline uint32_t g32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static inline uint64_t g64(const uint8_t* p) { return (uint64_t)g32(p) << 32 | g32(p + 4); }
static inline void p16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void p32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static inline void p64(uint8_t* p, uint64_t v) { p32(p, (uint32_t)(v >> 32)); p32(p + 4, (uint32_t)v); }
static inline void psplat(uint8_t* p, uint32_t v) { for (int i = 0; i < 16; i += 4) p32(p + i, v); }
static inline uint32_t bit32(uint32_t n) { return n < 32 ? 1u << n : 0u; }
static inline u128 pref(uint32_t v) { return spu_make_preferred_u32(v); }
static inline u128 qsplat(uint32_t v) { u128 r; r._u32[0] = r._u32[1] = r._u32[2] = r._u32[3] = v; return r; }
static inline u128 qwords(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { u128 r; r._u32[0] = a; r._u32[1] = b; r._u32[2] = c; r._u32[3] = d; return r; }
static inline uint8_t* lsp(spu_context* c, uint32_t a) { return c->ls + (a & SPU_LS_MASK); }

static void vsub(uint8_t* d, const uint8_t* a, const uint8_t* b) { for (int i = 0; i < 16; i += 4) p32(d + i, g32(a + i) - g32(b + i)); }
static void vadd(uint8_t* d, const uint8_t* a, const uint8_t* b) { for (int i = 0; i < 16; i += 4) p32(d + i, g32(a + i) + g32(b + i)); }

static void spurs_fatal(const char* what, uint64_t v)
{
    fprintf(stderr, "\n[spurs] FATAL: %s (0x%llX)\n", what, (unsigned long long)v);
    fflush(stderr);
    _Exit(3);
}

static void pm_halt(spu_context* c, uint32_t at)
{
    fprintf(stderr, "[spurs] halt at policy module LS 0x%04X\n", at);
    c->status = SPU_STATUS_STOPPED_BY_HALT;
    spu_halt(c);
}

static void pm_stopd(spu_context* c)
{
    c->stop_code = 0x3FFFu;
    c->status = SPU_STATUS_STOPPED_BY_STOP;
    spu_halt(c);
}

static void mfc(spu_context* c, uint32_t lsa, uint64_t ea, uint32_t size, uint32_t tag, uint32_t cmd)
{
    c->mfc_lsa = lsa;
    c->mfc_eah = (uint32_t)(ea >> 32);
    c->mfc_eal = (uint32_t)ea;
    c->mfc_size = size;
    c->mfc_tag = tag & 31u;
    spu_wrch(c, MFC_Cmd, pref(cmd));
}

static void mfc_seq(spu_context* c, uint32_t lsa, uint64_t ea, uint32_t size, uint32_t tag, uint32_t cmd)
{
    while (size) {
        const uint32_t n = cmd == MFC_GETLF_CMD ? size : size > 16383 ? 16384u : size > 15 ? size & ~15u : size > 7 ? 8u : size > 3 ? 4u : size > 1 ? 2u : 1u;
        mfc(c, lsa, ea, n, tag, cmd);
        lsa += n;
        ea += n;
        size -= n;
    }
}

static void tag_wait(spu_context* c, uint32_t mask)
{
    c->mfc_tag_mask = mask;
    spu_wrch(c, MFC_WrTagUpdate, pref(2));
    spu_rdch(c, MFC_RdTagStat);
}

static void tag_drain(spu_context* c, uint32_t count_channel)
{
    spu_wrch(c, MFC_WrTagUpdate, pref(0));
    while (spu_rchcnt(c, count_channel) != 1) {}
    spu_rdch(c, MFC_RdTagStat);
}

static uint32_t atomic_cmd(spu_context* c, uint32_t lsa, uint64_t ea, uint32_t cmd)
{
    c->mfc_lsa = lsa;
    c->mfc_eah = (uint32_t)(ea >> 32);
    c->mfc_eal = (uint32_t)ea;
    c->mfc_size = MFC_ATOMIC_LINE;
    c->mfc_tag = 0;
    spu_wrch(c, MFC_Cmd, pref(cmd));
    return spu_rdch(c, MFC_RdAtomicStat)._u32[0];
}

static uint8_t* ll_begin(spu_context* c, uint32_t ea, uint32_t lsa)
{
    ea &= ~127u;
    if (!mfc_ea_valid(ea, MFC_ATOMIC_LINE)) mfc_fault(c, "atomic address", MFC_GETLLAR_CMD, lsa, ea, MFC_ATOMIC_LINE);
    spu_lockline_lock(ea);
    spu_coh_reserve(c, ea);
    memcpy(c->ls + lsa, vm_base + ea, MFC_ATOMIC_LINE);
    return c->ls + lsa;
}

static void ll_commit(spu_context* c, uint32_t ea, uint32_t lsa)
{
    ea &= ~127u;
    memcpy(vm_base + ea, c->ls + lsa, MFC_ATOMIC_LINE);
    __atomic_store_n(&c->resv, 0u, __ATOMIC_RELAXED);
    c->atomic_stat = 0;
    spu_coh_notify_write(ea);
    spu_lockline_unlock(ea);
    spu_rsx_window_check(ea, MFC_ATOMIC_LINE);
}

static void ll_hold(spu_context* c, uint32_t ea, uint32_t lsa)
{
    ea &= ~127u;
    memcpy(c->resv_line, c->ls + lsa, MFC_ATOMIC_LINE);
    __atomic_store_n(&c->resv, ea | 1u, __ATOMIC_RELAXED);
    c->atomic_stat = 4;
    spu_lockline_unlock(ea);
}

static uint32_t pm_dec(spu_context* c)
{
    return c->ls[0x175] == 1 || !c->dec_base_ns ? spu_rdch(c, SPU_RdDec)._u32[0] : 0u;
}

static void trace_put(spu_context* c, uint32_t pkt, uint32_t tag, const uint32_t* dec)
{
    uint8_t* ls = c->ls;
    c->mfc_tag = tag & 31u;
    if (ls[0x175] != 1) return;
    const uint32_t hi = g32(ls + 0x210), lo = g32(ls + 0x214), n = g32(ls + 0x218), max = g32(ls + 0x21C);
    if (!(max > n) || !((lo >> 4) | hi)) return;
    p32(ls + 0x218, n + 1 == max && (lo & 1) ? 0u : n + 1);
    uint8_t* p = ls + pkt;
    p[1] = 2;
    p[2] = ls[0x1CB];
    p[3] = ls[0x1DF];
    p32(p + 4, 0u - (dec ? *dec : spu_rdch(c, SPU_RdDec)._u32[0]));
    c->mfc_lsa = pkt;
    c->mfc_eah = hi;
    c->mfc_eal = ((lo >> 4) + n) << 4;
    c->mfc_size = 16;
    spu_wrch(c, MFC_Cmd, pref(MFC_PUT_CMD));
}

static uint64_t guid7(const uint8_t* q)
{
    uint64_t g = 0;
    for (int i = 0; i < 16; i += 4) {
        const uint32_t w = g32(q + i);
        g = g << 16 | (((w << 7) | (w >> 25)) >> 16);
    }
    return g;
}

static uint64_t guid1(const uint8_t* q)
{
    uint64_t g = 0;
    for (int i = 0; i < 16; i += 4) {
        const uint32_t w = g32(q + i);
        g = g << 16 | ((((w >> 1) | (w << 31)) >> 8) & 0xFFFFu);
    }
    return g;
}

static void call_lifted(spu_context* c, uint32_t pc, uint32_t ret)
{
    const int img = c->image_id;
    const int32_t rd = c->reloc_delta;
    c->gpr[0] = pref(ret);
    c->pc = pc & SPU_LS_MASK;
    c->host_depth++;
    SPU_ICALL(c);
    spu_call_return(c, ret);
    c->host_depth--;
    c->image_id = img;
    c->reloc_delta = rd;
}

static void spu_exit_stop(spu_context* c, uint32_t code, uint32_t status)
{
    tag_drain(c, MFC_RdTagStat);
    tag_wait(c, 0xFFFFFFFFu);
    spu_wrch(c, SPU_WrOutMbox, pref(status));
    c->stop_code = code;
    spu_stop(c);
}

static void kernel_select(spu_context* c, uint32_t mode, uint32_t r[4])
{
    uint8_t* ls = c->ls;
    const int not_poll = mode == 0, poll1 = mode == 1;
    const uint32_t cur = g32(ls + 0x1DC);
    const uint32_t ea = (uint32_t)g64(ls + 0x1C0) & ~127u;
    ll_begin(c, ea, 0x100);
    const uint32_t spu = g32(ls + 0x1C8), bit = spu < 32 ? 1u << spu : 0u;
    uint8_t base[16], pend[16], cont[16], inc[16] = { 0 };
    vsub(base, ls + 0x120, ls + 0x180);
    vsub(pend, ls + 0x130, ls + 0x190);
    if (not_poll) {
        memcpy(cont, base, 16);
    } else {
        uint8_t m[16];
        for (uint32_t i = 0; i < 16; i++) m[i] = i == cur ? 0 : pend[i];
        vadd(cont, base, m);
    }
    uint32_t sel;
    const uint32_t msg = ls[0x172];
    if (msg & bit) {
        ls[0x1EB] = 0;
        if (not_poll || cur == 32) ls[0x172] = (uint8_t)(msg & ~bit);
        sel = 32;
        r[0] = 0x20; r[1] = r[2] = r[3] = 0;
    } else {
        const uint32_t sig = g16(ls + 0x170), flag = g32(ls + 0x16C), recv = ls[0x177];
        const uint32_t run = g16(ls + 0x1EC), uid = ls[0x1DB];
        const uint32_t fm = flag == 0 && recv < 16 ? 0x8000u >> recv : 0u;
        uint8_t eight[16], dist[16];
        memset(eight, 8, 16);
        vsub(dist, eight, cont);
        uint32_t wt[16], best = 0, sel16 = 16;
        uint8_t cand[16], ps[16];
        for (uint32_t i = 0; i < 16; i++) {
            const uint32_t ct = cont[i], rc = ls[0x100 + i], idle = ls[0x110 + i], pr = ls[0x1A0 + i];
            const uint32_t sm = (sig >> (15 - i)) & 1, fl = (fm >> (15 - i)) & 1;
            const uint32_t rdy = (rc > 8 ? 8 : rc) + (idle > 8 ? 8 : idle);
            const int ready = sm || fl || (rc != 0 && rdy > ct);
            cand[i] = ((run >> (15 - i)) & 1) && pr != 0 && ls[0x150 + i] > ct && ready;
            const uint32_t hi = (sm || fl || rc > ct ? 0x80u : 0u) | (pr & 0x7F);
            const uint32_t lo = (i == cur ? 0x80u : 0u) | (ct > 0 && ls[0x140 + i] > ct ? 0x40u : 0u) |
                                ((uint32_t)(dist[i] << 2) & 0x3Cu) | (ls[0x220 + i] == uid ? 2u : 0u) | 1u;
            wt[i] = cand[i] ? hi << 8 | lo : 0u;
            ps[i] = (uint8_t)((rc > ct) | sm << 1 | fl << 2);
            if (wt[i] > best) best = wt[i];
        }
        for (uint32_t i = 0; i < 16 && sel16 == 16; i++)
            if (cand[i] && wt[i] == best) sel16 = i;
        ls[0x1EB] = sel16 == 16;
        if (sel16 < 16) {
            r[0] = sel16; r[1] = ps[sel16]; r[2] = r[3] = 16;
        } else {
            r[0] = 0x20; r[1] = r[2] = r[3] = 0;
        }
        sel = r[0];
        if (not_poll || sel == cur) {
            if (sel < 16) p16(ls + 0x170, sig & ~(0x8000u >> sel));
            if (sel == recv) p32(ls + 0x16C, 0xFFFFFFFFu);
        }
        if (sel < 16) inc[sel] = 1;
    }
    const int pd = poll1 && sel != cur;
    if (not_poll) vadd(ls + 0x120, base, inc);
    if (pd) vadd(ls + 0x130, pend, inc);
    else memcpy(ls + 0x130, pend, 16);
    ll_commit(c, ea, 0x100);
    if (pd) memcpy(ls + 0x190, inc, 16);
    else memset(ls + 0x190, 0, 16);
    if (not_poll) memcpy(ls + 0x180, inc, 16);
    p32(ls + 0x1DC, not_poll ? sel : cur);
}

static uint32_t kernel_poll(spu_context* c, uint32_t* status)
{
    uint32_t r[4];
    kernel_select(c, 1, r);
    if (status) *status = r[1];
    return r[0] != g32(c->ls + 0x1DC);
}

static uint32_t ss_bit(spu_context* c)
{
    const uint32_t s = g32(c->ls + 0x1C8);
    return s < 8 ? 1u << s : 0u;
}

static uint32_t spurs_ea(spu_context* c) { return (uint32_t)g64(c->ls + 0x1C0); }

static uint32_t ss_send_event(spu_context* c, uint32_t port, uint32_t data0, uint32_t data1)
{
    port &= 0xFF;
    if (port > 63) return 0x80010002u;
    if (spu_rchcnt(c, SPU_RdInMbox)) return 0x8001000Au;
    spu_wrch(c, SPU_WrOutMbox, pref(data1));
    spu_wrch(c, SPU_WrOutIntrMbox, pref(port << 24 | (data0 & 0xFFFFFFu)));
    return spu_rdch(c, SPU_RdInMbox)._u32[0];
}

static void ss_shutdown_events(spu_context* c, uint32_t mask)
{
    uint8_t* ls = c->ls;
    const uint32_t ea = spurs_ea(c) + 0x80;
    uint32_t notify = 0;
    ll_begin(c, ea, 0x2D80);
    for (uint32_t i = 0; i < 16; i++) {
        uint8_t* e1 = ls + 0x2DA0 + i;
        uint8_t* e2 = ls + 0x2DF0 + i;
        const uint32_t m1 = (mask >> (31 - i)) & 1, m2 = (mask >> (15 - i)) & 1;
        if (m1) {
            if (*e1 & 0x12) notify |= 1u << (31 - i);
            *e1 |= 1;
        }
        if (m2) {
            if (*e2 & 0x12) notify |= 1u << (15 - i);
            *e2 |= 1;
        }
    }
    ll_commit(c, ea, 0x2D80);
    if (notify) ss_send_event(c, ls[0x2DC9], 0, notify);
}

static void ss_activate(spu_context* c)
{
    uint8_t* ls = c->ls;
    const uint32_t buf = 0x3FB00;
    const uint64_t spurs = g64(ls + 0x1C0);
    mfc(c, buf, spurs + 0xB00, 512, 31, MFC_GET_CMD);
    if (ls[0x174] & 0x40) spurs_fatal("32-workload SPURS is not supported by kernel 1", ls[0x174]);
    tag_wait(c, 0x80000000u);
    const uint32_t spu = g32(ls + 0x1C8) & 15;
    for (uint32_t i = 0; i < 16; i++) {
        const uint8_t* w = ls + buf + 32 * i;
        const uint32_t p = spu < 8 ? w[0x18 + spu] : w[0x10 + spu - 8];
        ls[0x220 + i] = w[0x14];
        ls[0x1A0 + i] = p ? (uint8_t)(16 - p) : 0;
    }
    const uint32_t b = ss_bit(c), ea = (uint32_t)spurs + 0x80;
    uint32_t run = 0, rem = 0;
    ll_begin(c, ea, 0x2D80);
    for (uint32_t i = 0; i < 16; i++) {
        uint8_t* st = ls + 0x2D80 + i;
        uint8_t* ss = ls + 0x2D90 + i;
        const uint32_t r = *st == 2;
        const uint32_t n = (*ss & ~b) | (r ? b : 0u);
        const uint32_t rm = *st == 3 && (*ss & b) && n == 0;
        if (rm) *st = 4;
        *ss = (uint8_t)n;
        run |= r << (15 - i);
        rem |= rm << (15 - i);
    }
    ll_commit(c, ea, 0x2D80);
    p32(ls + 0x1EC, run << 16);
    if (rem) ss_shutdown_events(c, rem << 16);
}

static void ss_deactivate(spu_context* c)
{
    uint8_t* ls = c->ls;
    const uint32_t b = ss_bit(c), ea = spurs_ea(c) + 0x80;
    uint32_t rem = 0;
    ll_begin(c, ea, 0x2D80);
    for (uint32_t i = 0; i < 16; i++) {
        uint8_t* st = ls + 0x2D80 + i;
        uint8_t* ss = ls + 0x2D90 + i;
        const uint32_t n = *ss & ~b;
        const uint32_t rm = *st == 3 && (*ss & b) && n == 0;
        if (rm) *st = 4;
        *ss = (uint8_t)n;
        rem |= rm << (15 - i);
    }
    ll_commit(c, ea, 0x2D80);
    p32(ls + 0x1EC, 0);
    if (rem) ss_shutdown_events(c, rem << 16);
}

static void ss_trace_save_count(spu_context* c)
{
    uint8_t* ls = c->ls;
    const uint64_t tb = g64(ls + 0x210);
    if (!tb) return;
    const uint32_t tag = g32(ls + 0x1CC), spu = g32(ls + 0x1C8);
    mfc(c, 0x80, g64(ls + 0x1C0) + 0x900, 128, tag, MFC_GET_CMD);
    tag_wait(c, bit32(tag));
    p32(ls + 0x2C20 + 4 * spu, g32(ls + 0x218));
    const uint64_t ea = (tb & ~3ull) - (uint64_t)(g32(ls + 0x88 + 4 * spu) << 4) + 0x20 + 4 * spu;
    mfc(c, 0x2C20 + 4 * spu, ea, 4, tag, MFC_PUT_CMD);
    tag_wait(c, bit32(tag));
}

static void ss_trace_update(spu_context* c, uint32_t a2, uint32_t a3, uint32_t a4)
{
    uint8_t* ls = c->ls;
    const uint32_t b = ss_bit(c), spu = g32(ls + 0x1C8), ea = spurs_ea(c) + 0x80;
    uint32_t notify = 0, req;
    ll_begin(c, ea, 0x2D80);
    req = ls[0x2DCE] & b;
    ls[0x2DCE] = (uint8_t)(ls[0x2DCE] & ~b);
    ls[0x2DCC] = (uint8_t)((ls[0x2DCC] & ~b) | (spu < 32 ? a2 << spu : 0u));
    if (req && !ls[0x2DCE] && ls[0x2DCD]) {
        notify = 1;
        ls[0x2DCD] = 0;
    }
    if (a4 && ls[0x2DCD]) {
        notify = 1;
        ls[0x2DCD] = 0;
    }
    ll_commit(c, ea, 0x2D80);
    if (req || a3) {
        const uint32_t tag = g32(ls + 0x1CC);
        atomic_cmd(c, 0x80, g64(ls + 0x1C0) + 0x900, MFC_GETLLAR_CMD);
        const uint64_t tb = g64(ls + 0x80);
        uint64_t nt = 0;
        int fetched = 0;
        if (tb) {
            nt = tb + (uint32_t)(g32(ls + 0x88 + 4 * spu) << 4);
            if (g32(ls + 0x218) == 0xFFFFFFFFu && nt) {
                mfc(c, 0x2C00, (tb & 0xFFFFFFFF00000000ull) | ((uint32_t)tb & ~3u), 128, tag, MFC_GET_CMD);
                tag_wait(c, bit32(tag));
                p32(ls + 0x218, g32(ls + 0x2C20 + 4 * spu));
                fetched = 1;
            }
        }
        if (!fetched) ss_trace_save_count(c);
        p64(ls + 0x210, nt);
        p32(ls + 0x21C, g32(ls + 0x8C) - g32(ls + 0x88));
        if (!nt) p32(ls + 0x218, 0);
    }
    if (notify) ss_send_event(c, ls[0x2DC9], 2, 0);
}

static void ss_trace_save_update(spu_context* c, uint32_t a)
{
    ss_trace_save_count(c);
    ss_trace_update(c, 0, 0, a);
}

static void ss_cleanup(spu_context* c)
{
    uint8_t* ls = c->ls;
    const uint32_t spu = g32(ls + 0x1C8) & 0x7F, spurs = spurs_ea(c);
    ll_begin(c, spurs + 0x80, 0x2D80);
    const uint32_t w = ls[0x2DC0 + spu];
    if (w == 0xFF) {
        ll_hold(c, spurs + 0x80, 0x2D80);
        return;
    }
    ls[0x2DC0 + spu] = 0xFF;
    ll_commit(c, spurs + 0x80, 0x2D80);
    ss_activate(c);
    uint8_t one[16] = { 0 }, cnt[16] = { 0 };
    one[w & 15] = 1;
    cnt[w & 15] = w > 15 ? 16 : 1;
    ll_begin(c, spurs, 0x100);
    vsub(ls + 0x120, ls + 0x120, cnt);
    vsub(ls + (w > 15 ? 0x110 : 0x100), ls + (w > 15 ? 0x110 : 0x100), one);
    ll_commit(c, spurs, 0x100);
    const uint32_t cur = g32(ls + 0x1DC);
    p32(ls + 0x1DC, w);
    p64(ls + 0x2BF8, guid7(ls + 0xA00));
    trace_put(c, 0x2BF0, g32(ls + 0x1CC), NULL);
    p32(ls + 0x1DC, cur);
}

static void ss_exit_if_no_work(spu_context* c)
{
    uint8_t* ls = c->ls;
    const uint32_t b = ss_bit(c), spurs = spurs_ea(c);
    ss_deactivate(c);
    ss_trace_save_update(c, 0);
    while (!spu_rchcnt(c, SPU_WrOutIntrMbox)) {}
    ll_begin(c, spurs + 0x80, 0x2D80);
    ls[0x2DBC] |= (uint8_t)b;
    ls[0x2DC8] = (uint8_t)(ls[0x2DC8] & ~b);
    ll_commit(c, spurs + 0x80, 0x2D80);
    if ((uint32_t)__builtin_popcount(ls[0x2DBC]) != ls[0x176]) spu_halt(c);
    ss_trace_save_update(c, 1);
    ll_begin(c, spurs + 0x80, 0x2D80);
    ls[0x2DBC] = 0;
    p32(ls + 0x2DB8, 0);
    ls[0x2DBD] = 0xFF;
    ll_commit(c, spurs + 0x80, 0x2D80);
    ll_begin(c, spurs, 0x100);
    ls[0x173] = 0;
    ll_commit(c, spurs, 0x100);
    spu_exit_stop(c, 0x101, 0);
}

static uint32_t ss_prio_mask(const uint8_t* ls)
{
    uint32_t m = 0;
    for (uint32_t i = 0; i < 16; i++) m |= (ls[0x1A0 + i] ? 0x8000u : 0u) >> i;
    return m;
}

static int ss_idle_found_in(const uint8_t* l, uint32_t run, uint32_t prio)
{
    const uint32_t sig = g16(l + 0x70), flag = g32(l + 0x6C), recv = l[0x77];
    const uint32_t fm = flag == 0 && recv < 16 ? 0x8000u >> recv : 0u;
    for (uint32_t i = 0; i < 16; i++) {
        const uint32_t ct = l[0x20 + i], rc = l[i], idle = l[0x10 + i];
        const uint32_t rdy = (rc > 8 ? 8 : rc) + (idle > 8 ? 8 : idle);
        const int ready = ((sig | fm) >> (15 - i)) & 1 || (rc != 0 && rdy > ct);
        if (((run & prio) >> (15 - i)) & 1 && l[0x50 + i] > ct && ready) return 1;
    }
    return 0;
}

static int ss_idle_found(spu_context* c) { return ss_idle_found_in(c->ls + 0x100, g16(c->ls + 0x1EC), ss_prio_mask(c->ls)); }

static int ss_idle_wake(const spu_context* c, const uint8_t* l)
{
    const uint32_t b = c->idle_bit;
    if ((l[0x74] & 0x40) || !(l[0x73] & b) || (l[0x72] & b)) return 2;
    if ((uint32_t)__builtin_popcount(l[0x73]) == l[0x76] && (l[0x74] & 0x80)) return 2;
    return ss_idle_found_in(l, c->idle_run, c->idle_prio);
}

static uint32_t ss_idle_slots(const uint8_t* l)
{
    const uint32_t sig = g16(l + 0x70), flag = g32(l + 0x6C), recv = l[0x77];
    const uint32_t fm = flag == 0 && recv < 16 ? 0x8000u >> recv : 0u;
    uint32_t n = 0;
    for (uint32_t i = 0; i < 16; i++) {
        const uint32_t ct = l[0x20 + i], rc = l[i], idle = l[0x10 + i], mc = l[0x50 + i];
        const uint32_t rdy = (rc > 8 ? 8 : rc) + (idle > 8 ? 8 : idle);
        if (mc <= ct) continue;
        if (rc != 0 && rdy > ct) n += (rdy < mc ? rdy : mc) - ct;
        else if (((sig | fm) >> (15 - i)) & 1) n++;
    }
    return n ? n : 1u;
}

static void ss_idle_reserve(spu_context* c, uint32_t ea)
{
    c->mfc_lsa = 0x100;
    c->mfc_eah = 0;
    c->mfc_eal = ea;
    c->mfc_size = MFC_ATOMIC_LINE;
    c->mfc_tag = 0;
    yz_lockstep_tick(c);
    ll_begin(c, ea, 0x100);
    memcpy(c->resv_line, c->ls + 0x100, MFC_ATOMIC_LINE);
    c->idle_bit = ss_bit(c);
    c->idle_run = g16(c->ls + 0x1EC);
    c->idle_prio = ss_prio_mask(c->ls);
    __atomic_store_n(&c->resv, (ea & ~127u) | 1u | SPU_RESV_BY_CONTENT, __ATOMIC_RELAXED);
    c->atomic_stat = 4;
    spu_lockline_unlock(ea & ~127u);
    spu_rdch(c, MFC_RdAtomicStat);
}

static void ss_idle(spu_context* c)
{
    uint8_t* ls = c->ls;
    const uint32_t b = ss_bit(c), spurs = spurs_ea(c);
    spu_wrch(c, SPU_WrEventMask, pref(SPU_EVENT_LR));
    for (;;) {
        ss_idle_reserve(c, spurs);
        if (ls[0x174] & 0x40) spurs_fatal("32-workload SPURS is not supported by kernel 1", ls[0x174]);
        const int exit_now = (uint32_t)__builtin_popcount(ls[0x173]) == ls[0x176] && (ls[0x174] & 0x80);
        const int found = (ls[0x172] & b) || ss_idle_found(c);
        const int was_idle = (ls[0x173] & b) != 0;
        if (found && !exit_now) ls[0x173] = (uint8_t)(ls[0x173] & ~b);
        else ls[0x173] |= (uint8_t)b;
        if (was_idle && !exit_now && !found) {
            spu_rdch(c, SPU_RdEventStat);
            spu_wrch(c, SPU_WrEventAck, pref(SPU_EVENT_LR));
        }
        if (atomic_cmd(c, 0x100, spurs, MFC_PUTLLC_CMD)) continue;
        if (!exit_now && !found) continue;
        if (!exit_now) return;
        ss_exit_if_no_work(c);
    }
}

static void ss_process_requests(spu_context* c)
{
    uint8_t* ls = c->ls;
    const uint32_t b = ss_bit(c), ea = spurs_ea(c) + 0x80;
    ll_begin(c, ea, 0x2D80);
    const uint32_t upd = ls[0x2DBD] & b, term = ls[0x2DBF] & b, trc = ls[0x2DCE] & b;
    ls[0x2DBD] = (uint8_t)(ls[0x2DBD] & ~b);
    if (term) ls[0x2DC8] = (uint8_t)(ls[0x2DC8] & ~b);
    ll_commit(c, ea, 0x2D80);
    if (upd) ss_activate(c);
    if (trc) ss_trace_update(c, 1, 0, 0);
    if (term) {
        ss_deactivate(c);
        ss_trace_save_update(c, 0);
        while (!spu_rchcnt(c, SPU_WrOutIntrMbox)) {}
        spu_exit_stop(c, 0x102, 0);
    }
}

static void ss_main(spu_context* c)
{
    uint8_t* ls = c->ls;
    const uint32_t b = ss_bit(c), spurs = spurs_ea(c);
    if (g32(ls + 0x1C4) & 0x7F) pm_halt(c, 0x1258);
    if (!ls[0x1EA]) {
        ls[0x1EA] = 1;
        atomic_cmd(c, 0x100, spurs, MFC_GETLLAR_CMD);
        ll_begin(c, spurs + 0x80, 0x2D80);
        if (ls[0x2DC8] & b) {
            ll_hold(c, spurs + 0x80, 0x2D80);
            pm_halt(c, 0x1320);
        }
        ls[0x2DC8] |= (uint8_t)b;
        ll_commit(c, spurs + 0x80, 0x2D80);
        p64(ls + 0x210, 0);
        p32(ls + 0x218, 0xFFFFFFFFu);
        ss_trace_update(c, 1, 1, 0);
        ss_cleanup(c);
        trace_put(c, 0x2B90, g32(ls + 0x1CC), NULL);
    }
    trace_put(c, 0x2BC0, g32(ls + 0x1CC), NULL);
    for (;;) {
        ss_process_requests(c);
        for (;;) {
            if (kernel_poll(c, NULL)) {
                trace_put(c, 0x2BB0, g32(ls + 0x1CC), NULL);
                p64(ls + 0x2BA8, guid7(ls + 0xA00));
                trace_put(c, 0x2BA0, g32(ls + 0x1CC), NULL);
                tag_wait(c, bit32(g32(ls + 0x1CC)));
                return;
            }
            if (!ls[0x1EB]) break;
            trace_put(c, 0x2B80, g32(ls + 0x1CC), NULL);
            ss_idle(c);
        }
    }
}

static uint32_t ss_switch_module(spu_context* c, uint32_t entry)
{
    if (spu_rchcnt(c, SPU_RdInMbox)) return 0x8001000Au;
    tag_drain(c, MFC_RdTagStat);
    tag_wait(c, 0xFFFFFFFFu);
    spu_wrch(c, SPU_WrOutMbox, pref(entry));
    c->stop_code = 0x120;
    c->status = SPU_STATUS_STOPPED_BY_STOP;
    spu_halt(c);
    return 0;
}

static void ss_system_workload(spu_context* c, uint64_t arg, uint32_t ps)
{
    uint8_t* ls = c->ls;
    const uint32_t b = ss_bit(c), spurs = spurs_ea(c), spu = g32(ls + 0x1C8) & 0x7F;
    if (g32(ls + 0x1DC) == 32) pm_stopd(c);
    trace_put(c, 0x2BE0, g32(ls + 0x1CC), NULL);
    uint32_t entry = 0;
    for (;;) {
        if (ps & 2) pm_stopd(c);
        if (ps & 1) {
            for (;;) {
                atomic_cmd(c, 0x2C80, arg, MFC_GETLLAR_CMD);
                const uint32_t count = g16(ls + 0x2C8A);
                if (!count) break;
                const uint32_t idx = g16(ls + 0x2C88);
                mfc(c, 0x2C94 + 4 * idx, arg + 0x14 + 4 * idx, 4, 31, MFC_GET_CMD);
                tag_wait(c, 0x80000000u);
                entry = g32(ls + 0x2C94 + 4 * idx);
                p16(ls + 0x2C8A, count - 1);
                p16(ls + 0x2C88, (g16(ls + 0x2C88) + 1) & 31);
                if (!atomic_cmd(c, 0x2C80, arg, MFC_PUTLLC_CMD)) break;
            }
            for (;;) {
                atomic_cmd(c, 0x2D80, spurs + 0x80, MFC_GETLLAR_CMD);
                if (ls[0x2DC0 + spu] != 0xFF) pm_stopd(c);
                ls[0x2DC0 + spu] = ls[0x1DF];
                ls[0x2DC8] = (uint8_t)(ls[0x2DC8] & ~b);
                if (!atomic_cmd(c, 0x2D80, spurs + 0x80, MFC_PUTLLC_CMD)) break;
            }
            ss_trace_save_count(c);
            if (ss_switch_module(c, entry) == 0x80010002u) pm_stopd(c);
            ll_begin(c, spurs + 0x80, 0x2D80);
            ls[0x2DC0 + spu] = 0xFF;
            ll_commit(c, spurs + 0x80, 0x2D80);
            const uint32_t w = g32(ls + 0x1DC), a = (w > 15 ? 0x110 : 0x100) + (w & 15);
            ll_begin(c, spurs, 0x100);
            ls[a] = ls[a] ? (uint8_t)(ls[a] - 1) : 0;
            ll_commit(c, spurs, 0x100);
        }
        if (kernel_poll(c, &ps)) break;
    }
    p64(ls + 0x2BD8, guid7(ls + 0xA00));
    trace_put(c, 0x2BD0, g32(ls + 0x1CC), NULL);
    tag_wait(c, bit32(g32(ls + 0x1CC)));
}

static void ss_entry(spu_context* c, uint64_t arg, uint32_t ps)
{
    uint8_t* ls = c->ls;
    memset(ls + 0x3FFF0, 0, 16);
    psplat(ls + 0x3FFD0, 0x3FFF0);
    memset(ls + 0x3FFE0, 0, 16);
    if (g32(ls + 0x1DC) == 32) ss_main(c);
    else ss_system_workload(c, arg, ps);
}

typedef struct { uint64_t h, l; } tset;
static inline tset ts_ld(const uint8_t* p) { tset v = { g64(p), g64(p + 8) }; return v; }
static inline void ts_st(uint8_t* p, tset v) { p64(p, v.h); p64(p + 8, v.l); }
static inline tset ts_bit(uint32_t t) { tset v = { t < 64 ? 1ull << (63 - t) : 0, t >= 64 && t < 128 ? 1ull << (127 - t) : 0 }; return v; }
static inline tset ts_or(tset a, tset b) { tset v = { a.h | b.h, a.l | b.l }; return v; }
static inline tset ts_and(tset a, tset b) { tset v = { a.h & b.h, a.l & b.l }; return v; }
static inline tset ts_andn(tset a, tset b) { tset v = { a.h & ~b.h, a.l & ~b.l }; return v; }
static inline int ts_any(tset a) { return (a.h | a.l) != 0; }
static inline uint32_t ts_pop(tset a) { return (uint32_t)(__builtin_popcountll(a.h) + __builtin_popcountll(a.l)); }
static inline uint32_t ts_clz(tset a) { return a.h ? (uint32_t)__builtin_clzll(a.h) : a.l ? 64 + (uint32_t)__builtin_clzll(a.l) : 128; }
static inline tset ts_rotl(tset a, uint32_t n)
{
    n &= 127;
    if (n >= 64) {
        const uint64_t t = a.h;
        a.h = a.l;
        a.l = t;
        n -= 64;
    }
    if (!n) return a;
    tset v = { a.h << n | a.l >> (64 - n), a.l << n | a.h >> (64 - n) };
    return v;
}

static uint32_t ts_request(spu_context* c, int32_t req, uint32_t out_task, uint32_t out_wait)
{
    uint8_t* ls = c->ls;
    const uint32_t cur = g32(ls + 0x27D4);
    const tset cm = ts_bit(cur);
    const uint32_t ea = (uint32_t)g64(ls + 0x27B8) & ~127u;
    int32_t n;
    uint32_t rc;
    ll_begin(c, ea, 0x2700);
    const tset run = ts_ld(ls + 0x2700), rdy = ts_ld(ls + 0x2710), pend = ts_ld(ls + 0x2720);
    const tset en = ts_ld(ls + 0x2730), sig = ts_ld(ls + 0x2740), wait = ts_ld(ls + 0x2750);
    const uint32_t b72 = ls[0x2772], b73 = ls[0x2773];
    if (ts_any(ts_or(ts_or(ts_andn(ts_or(ts_or(ts_or(run, rdy), ts_or(pend, sig)), wait), en), ts_and(run, wait)), ts_and(rdy, pend)))) {
        ll_hold(c, ea, 0x2700);
        pm_halt(c, 0xF00);
    }
    if ((uint32_t)(req + 1) > 7) {
        ll_hold(c, ea, 0x2700);
        pm_halt(c, 0x11EC);
    }
    memset(ls + 0x2720, 0, 16);
    n = (int32_t)ts_pop(ts_andn(ts_or(pend, sig), rdy));
    const tset rdy2 = ts_or(ts_or(rdy, pend), sig);
    const tset sig2 = ts_and(sig, ts_or(rdy, pend));
    const tset cand = ts_andn(ts_andn(rdy2, run), ts_bit(b72));
    tset RUN = run, RDY = rdy2, EN = en, SIG = sig2, WAIT = wait;
    rc = 0;
    switch (req) {
    case -1:
        rc = ts_any(ts_and(sig2, cm));
        SIG = ts_andn(sig2, cm);
        break;
    case 0:
        RUN = ts_andn(run, cm);
        RDY = ts_andn(rdy2, cm);
        EN = ts_andn(en, cm);
        SIG = ts_andn(sig2, cm);
        n -= 1;
        break;
    case 1:
        RUN = ts_andn(run, cm);
        WAIT = ts_or(wait, cm);
        break;
    case 2:
        if (ts_any(ts_and(sig2, cm))) {
            SIG = ts_andn(sig2, cm);
            rc = 1;
        } else {
            RUN = ts_andn(run, cm);
            RDY = ts_andn(rdy2, cm);
            WAIT = ts_or(wait, cm);
            n -= 1;
        }
        break;
    case 3:
        rc = ts_any(ts_andn(cand, cm));
        break;
    case 4:
        if (b72 == 0x81) {
            ls[0x2772] = 0x80;
        } else if (b72 == 0x80) {
            ls[0x2772] = (uint8_t)cur;
            RUN = ts_andn(run, cm);
            WAIT = ts_or(wait, cm);
            n -= 1;
            rc = 1;
        } else {
            rc = 0x8041090Au;
        }
        break;
    case 5: {
        const uint32_t k = (b73 + 1) & 127, s = ts_clz(ts_rotl(cand, k));
        uint32_t sel = 128, waiting;
        if (s <= 127) {
            sel = (k + s) & 127;
            const tset sb = ts_bit(sel);
            waiting = ts_any(ts_and(wait, sb));
            RUN = ts_or(run, sb);
            WAIT = ts_andn(wait, sb);
            ls[0x2773] = (uint8_t)sel;
        } else {
            waiting = (uint32_t)(wait.h >> 63);
        }
        p32(lsp(c, out_task & ~3u), sel);
        p32(lsp(c, out_wait & ~3u), waiting);
        break;
    }
    default:
        if ((int8_t)b72 >= 0) {
            ls[0x2772] = 0x80;
            n += 1;
            rc = 1;
        } else {
            ls[0x2772] = 0x81;
        }
        break;
    }
    ts_st(ls + 0x2700, RUN);
    ts_st(ls + 0x2710, RDY);
    ts_st(ls + 0x2730, EN);
    ts_st(ls + 0x2740, SIG);
    ts_st(ls + 0x2750, WAIT);
    ll_commit(c, ea, 0x2700);
    const uint32_t spurs = spurs_ea(c) & ~127u;
    for (;;) {
        const uint32_t w = g32(ls + 0x1DC), a = (w > 15 ? 0x110 : 0x100) + (w & 15);
        ll_begin(c, spurs, 0x100);
        const int32_t old = ls[a], v = old + n;
        ls[a] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        ll_commit(c, spurs, 0x100);
        n += old;
        if (n >= 0) return rc;
    }
}

static SPU_THREAD_LOCAL struct { int switched, to_kernel; } t_task;

static uint32_t ts_poll(spu_context* c)
{
    uint32_t st;
    const uint32_t k = kernel_poll(c, &st);
    p32(c->ls + 0x2BE0, st);
    if (!k && (st & 4)) ts_request(c, 6, 0, 0);
    return k & 1;
}

static void ts_exit_to_kernel(spu_context* c)
{
    uint8_t* ls = c->ls;
    p64(ls + 0x2808, guid7(ls + 0xA00));
    trace_put(c, 0x2800, g32(ls + 0x27D0), NULL);
    p16(ls + 0x1E8, 0);
    if (memcmp(ls + 0x2840, ls + 0x2610, 16)) pm_halt(c, 0x142C);
}

static void ts_runs(spu_context* c, uint64_t ctx_ea, uint32_t tag, uint32_t cmd)
{
    const tset pat = ts_ld(c->ls + 0x27A0);
    uint64_t ea = ctx_ea + 0x400;
    for (uint32_t b = 6; b < 128;) {
        if (!ts_any(ts_and(pat, ts_bit(b)))) {
            b++;
            continue;
        }
        uint32_t e = b;
        while (e < 128 && ts_any(ts_and(pat, ts_bit(e)))) e++;
        mfc_seq(c, b << 11, ea, (e - b) << 11, tag, cmd);
        ea += (uint64_t)(e - b) << 11;
        b = e;
    }
}

static uint32_t ts_save(spu_context* c)
{
    uint8_t* ls = c->ls;
    tag_wait(c, 0xFFFFFFFFu);
    const tset pat = ts_ld(ls + 0x27A0);
    const uint64_t ctx = g64(ls + 0x2798);
    const tset counted = { pat.h & 0x00FFFFFFFFFFFFFFull, pat.l };
    int missing = 0;
    for (uint32_t blk = g32(ls + 0x2C90) >> 11; blk < 128 && !missing; blk++) missing = !ts_any(ts_and(pat, ts_bit(blk)));
    if (!ctx || ts_pop(counted) > ((uint32_t)ctx & 0x7F) || missing) return 0x8041090Fu;
    memset(ls + 0x2FA0, 0, 16);
    p32(ls + 0x2FB0, spu_rdch(c, MFC_RdTagMask)._u32[0]);
    p32(ls + 0x2FB4, spu_rdch(c, SPU_RdEventMask)._u32[0]);
    const uint32_t tag = g32(ls + 0x27D0);
    const uint64_t ea = ctx & ~0x7Full;
    mfc(c, 0x2C80, ea, 0x380, tag, MFC_PUT_CMD);
    ts_runs(c, ea, tag, MFC_PUT_CMD);
    tag_wait(c, bit32(tag));
    return 0;
}

static uint32_t ts_load_elf(spu_context* c, uint32_t entry_out, uint32_t lowest_out, uint32_t hdr, uint64_t elf, uint32_t tag, uint32_t skip_writable)
{
    static const uint8_t ident[16] = { 0x7F, 0x45, 0x4C, 0x46, 0x01, 0x02, 0x01, 0x00 };
    uint8_t* ls = c->ls;
    uint8_t* h = ls + hdr;
    if (!elf || (elf & 15)) return 0x80410902u;
    uint32_t loaded = 0, want = 256, need, phent, phnum, phoff, entry;
    for (;;) {
        mfc(c, hdr + loaded, elf + loaded, want - loaded, tag, MFC_GET_CMD);
        tag_wait(c, bit32(tag));
        loaded = want;
        entry = g32(h + 0x18);
        if (memcmp(h, ident, 16) || g16(h + 0x12) != 23 || entry <= 0x2FFF) return 0x80410907u;
        phent = g16(h + 0x2A);
        phnum = g16(h + 0x2C);
        phoff = g32(h + 0x1C);
        need = (phent * phnum + phoff + 127) & ~127u;
        if (need > 768) need = 768;
        if (need <= loaded) break;
        want = need;
    }
    p32(lsp(c, entry_out & ~3u), entry);
    uint32_t lowest = 0x40000;
    for (uint32_t i = 0; i < phnum; i++) {
        const uint32_t ph = hdr + phoff + i * phent;
        if (ph + phent > hdr + need) return 0x80410904u;
        const uint8_t* p = lsp(c, ph);
        const uint32_t type = g32(p), off = g32(p + 4), va = g32(p + 8), pa = g32(p + 12);
        const uint32_t filesz = g32(p + 16), memsz = g32(p + 20), flags = g32(p + 24);
        if (pa > 0x3FFFF) break;
        if (type != 1 || (skip_writable && (flags & 2))) continue;
        if (va <= 0x2FFF || va + memsz > 0x40000) return 0x8041090Du;
        if (va < lowest) lowest = va;
        mfc_seq(c, va, elf + off, filesz, tag, MFC_GET_CMD);
        const uint32_t zero = memsz - filesz;
        if (zero & 15) pm_halt(c, 0x2218);
        if (zero) memset(lsp(c, (va + filesz) & ~15u), 0, zero);
    }
    if (lowest_out) p32(lsp(c, lowest_out & ~3u), lowest);
    return 0;
}

static void ts_go(spu_context* c)
{
    t_task.switched = 0;
    const int halted = spu_run_with_halt(spu_indirect_branch, c);
    c->host_depth = 0;
    if (!halted || !t_task.switched) spu_halt(c);
}

static int ts_run(spu_context* c, uint32_t sel)
{
    uint8_t* ls = c->ls;
    const uint32_t tag = g32(ls + 0x27D0);
    const uint64_t taskset = g64(ls + 0x27B8);
    p32(ls + 0x27D4, sel);
    mfc(c, 0x2780, taskset + 0x80 + 0x30 * sel, 0x30, tag, MFC_GET_CMD);
    tag_wait(c, bit32(tag));
    const uint32_t flags = g32(ls + 0x2794);
    const uint64_t elf = g64(ls + 0x2790) & ~7ull;
    p64(ls + 0x2790, elf);
    p32(ls + 0x2828, 1);
    p32(ls + 0x282C, g32(ls + 0x27D4));
    trace_put(c, 0x2820, 31, NULL);
    if (!g32(ls + 0x2C60)) {
        p32(ls + 0x2FBC, 0x3000);
        if (ts_load_elf(c, 0x2C80, 0x2FBC, 0x2CA0, elf, tag, 0)) pm_halt(c, 0x1B68);
        tag_wait(c, bit32(tag));
        p32(ls + 0x2FD0, g32(ls + 0x2C60));
        p32(ls + 0x2FD4, flags & 5);
        p32(ls + 0x2FB8, 0x2700);
        p64(ls + 0x2FC0, 0);
        if ((flags & 5) == 1) mfc_seq(c, 0x2FC0, taskset + 0x1980 + 16 * g32(ls + 0x27D4), 16, tag, MFC_GET_CMD);
        p64(ls + 0x2838, guid7(lsp(c, g32(ls + 0x2FBC) & ~15u)));
        trace_put(c, 0x2830, 31, NULL);
        if (flags & 2) pm_stopd(c);
        u128 r4 = spu_ls_read128(c, 0x2760);
        c->gpr[0] = spu_ls_read128(c, 0x2C80);
        c->gpr[1] = qsplat(0x2C30);
        c->gpr[2] = qsplat(0);
        c->gpr[3] = spu_ls_read128(c, 0x2780);
        c->gpr[4] = qwords(r4._u32[2], r4._u32[3], r4._u32[0], r4._u32[1]);
        for (int r = 5; r < 128; r++) c->gpr[r] = qsplat(0);
    } else {
        if (ls[0x2770] & 1) memset(ls + 0x3000, 0, 0x3D000);
        const tset pat = ts_ld(ls + 0x27A0);
        if ((~pat.h & 0x03FFFFFFFFFFFFFFull) || ~pat.l)
            if (ts_load_elf(c, 0x2C70, 0, 0x2CA0, elf, tag, 1)) pm_halt(c, 0x1950);
        const uint64_t ctx_ea = g64(ls + 0x2798) & ~0x7Full;
        mfc(c, 0x2C80, ctx_ea, 0x380, tag, MFC_GETB_CMD);
        ts_runs(c, ctx_ea, tag, MFC_GET_CMD);
        tag_wait(c, bit32(tag));
        c->mfc_tag_mask = g32(ls + 0x2FB0);
        spu_wrch(c, SPU_WrEventMask, pref(g32(ls + 0x2FB4)));
        p64(ls + 0x2838, guid7(lsp(c, g32(ls + 0x2FBC) & ~15u)));
        trace_put(c, 0x2830, 31, NULL);
        if (flags & 2) pm_stopd(c);
        c->gpr[3] = qsplat(0);
        c->gpr[0] = spu_ls_read128(c, 0x2C80);
        c->gpr[1] = spu_ls_read128(c, 0x2C90);
        for (int r = 80; r < 128; r++) c->gpr[r] = spu_ls_read128(c, 0x2CA0 + 16 * (uint32_t)(r - 80));
    }
    c->pc = c->gpr[0]._u32[0] & SPU_LS_MASK;
    ts_go(c);
    return !t_task.to_kernel;
}

static void ts_switch_out(spu_context* c, uint32_t incident)
{
    uint8_t* ls = c->ls;
    p32(ls + 0x2828, incident);
    p32(ls + 0x282C, g32(ls + 0x27D4));
    trace_put(c, 0x2820, 31, NULL);
    memset(lsp(c, g32(ls + 0x2FBC) & ~15u), 0, 16);
    t_task.to_kernel = (int)ts_poll(c);
    t_task.switched = 1;
    spu_halt(c);
}

static void ts_exit_hook(spu_context* c, uint64_t hook, u128 arg)
{
    const u128 task = spu_ls_read128(c, 0x27D0), q = spu_ls_read128(c, 0x2FD0), ts = spu_ls_read128(c, 0x27B0);
    mfc_seq(c, 0x10000, hook & ~0x7Full, ((uint32_t)hook & 0x7F) << 7, 0, MFC_GET_CMD);
    tag_wait(c, 1);
    c->gpr[1] = qsplat(0x2C00);
    c->gpr[3] = qwords(ts._u32[2], ts._u32[3], ts._u32[0], ts._u32[1]);
    c->gpr[4] = qwords(task._u32[1], task._u32[2], task._u32[3], task._u32[0]);
    c->gpr[5] = q;
    c->gpr[6] = arg;
    call_lifted(c, 0x10000, 0x1D30);
}

static u128 ts_handle(spu_context* c, uint32_t num, u128 arg)
{
    uint8_t* ls = c->ls;
    uint32_t n = num;
    if (num & 0x10) n = num & 15;
    else tag_wait(c, 0xFFFFFFFFu);
    if (n > 4) return qsplat(0x80410903u);
    uint32_t rc;
    switch (n) {
    case 0:
        if (g32(ls + 0x2FD4) == 4) {
            ts_exit_hook(c, g64(ls + 0x2778), qsplat(0));
        } else {
            ts_request(c, 0, 0, 0);
            if (g32(ls + 0x2FC4)) {
                const u128 h = spu_ls_read128(c, 0x2FC0);
                ts_exit_hook(c, g64(ls + 0x2FC0), qwords(h._u32[2], h._u32[3], h._u32[0], h._u32[1]));
            }
        }
        ts_switch_out(c, 5);
        return qsplat(0);
    case 1: {
        const uint32_t k = ts_poll(c), p = ts_request(c, 3, 0, 0);
        if (!(k | p)) return qsplat(0);
        if ((rc = ts_save(c)) != 0) return qsplat(rc);
        ts_request(c, 1, 0, 0);
        ts_switch_out(c, 3);
        return qsplat(0);
    }
    case 2:
        if (ts_request(c, -1, 0, 0)) return qsplat(0);
        if ((rc = ts_save(c)) != 0) return qsplat(rc);
        if (ts_request(c, 2, 0, 0)) return qsplat(0);
        ts_switch_out(c, 4);
        return qsplat(0);
    case 3: {
        const uint32_t k = ts_poll(c), p = ts_request(c, 3, 0, 0), k2 = k ? 2u : 0u;
        return qwords((p ? 1u : 0u) | k2, k2, k2, k2);
    }
    default: {
        if (!arg._u32[0]) pm_halt(c, 0x1DFC);
        const uint32_t k = ts_poll(c), r = ts_request(c, 4, 0, 0);
        if (!k && r != 1) return qsplat(r);
        if ((rc = ts_save(c)) != 0) return qsplat(rc);
        ts_switch_out(c, 4);
        return qsplat(0);
    }
    }
}

static void ts_syscall(spu_context* c)
{
    spu_ls_write128(c, 0x2C80, c->gpr[0]);
    spu_ls_write128(c, 0x2C90, c->gpr[1]);
    for (uint32_t r = 80; r < 128; r++) spu_ls_write128(c, 0x2CA0 + 16 * (r - 80), c->gpr[r]);
    memset(c->ls + 0x2C50, 0, 16);
    psplat(c->ls + 0x2C30, 0x2C50);
    const u128 rc = ts_handle(c, c->gpr[3]._u32[0], c->gpr[4]);
    c->gpr[0] = spu_ls_read128(c, 0x2C80);
    c->gpr[1] = spu_ls_read128(c, 0x2C90);
    for (uint32_t r = 80; r < 128; r++) c->gpr[r] = spu_ls_read128(c, 0x2CA0 + 16 * (r - 80));
    c->gpr[3] = rc;
    SPU_RET(c);
}

static void ts_entry(spu_context* c, uint64_t taskset, uint32_t ps)
{
    uint8_t* ls = c->ls;
    p64(ls + 0x27B8, taskset);
    p32(ls + 0x27C0, 0x100);
    p32(ls + 0x27CC, g32(ls + 0x1C8));
    p32(ls + 0x27D0, g32(ls + 0x1CC));
    memcpy(ls + 0x2840, ls + 0x2600, 16);
    p16(ls + 0x1E8, 0x544B);
    trace_put(c, 0x2810, g32(ls + 0x27D0), NULL);
    if (ps & 4) ts_request(c, 6, 0, 0);
    for (;;) {
        psplat(ls + 0x2C30, 0x2C70);
        memset(ls + 0x2C70, 0, 16);
        ts_request(c, 5, 0x2C50, 0x2C60);
        const uint32_t sel = g32(ls + 0x2C50);
        if (sel > 127 || !ts_run(c, sel)) break;
    }
    ts_exit_to_kernel(c);
}

typedef struct {
    jmp_buf exit;
    ps3_lock* lock;
    int held;
    uint32_t b94, c90;
} jc_state;

typedef struct {
    uint32_t n, w1;
    uint64_t ea;
} jc_job;

typedef struct { uint32_t ea; ps3_lock lock; } spurs_chain_lock;
#define SPURS_CHAIN_LOCKS 1024u
static spurs_chain_lock s_chain_locks[SPURS_CHAIN_LOCKS];

static ps3_lock* chain_lock_of(uint32_t ea)
{
    for (uint32_t i = ((ea >> 7) * 2654435761u) >> 22, n = 0; n < SPURS_CHAIN_LOCKS; n++, i = (i + 1) & (SPURS_CHAIN_LOCKS - 1)) {
        uint32_t cur = __atomic_load_n(&s_chain_locks[i].ea, __ATOMIC_ACQUIRE);
        if (!cur && __atomic_compare_exchange_n(&s_chain_locks[i].ea, &cur, ea, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return &s_chain_locks[i].lock;
        if (cur == ea) return &s_chain_locks[i].lock;
    }
    spurs_fatal("job chain lock table full", ea);
    return NULL;
}

static void jc_unhold(jc_state* s)
{
    if (!s->held) return;
    s->held = 0;
    ps3_lock_release(s->lock);
}

static void jc_fatal(spu_context* c, jc_state* s, uint32_t err, uint64_t cause)
{
    uint8_t* ls = c->ls;
    const uint64_t chain = g64(ls + 0x4A90), spurs = g64(ls + 0x1C0);
    const uint32_t tag = g32(ls + 0x4A9C), wid = g32(ls + 0x4A74);
    const int halt_on_error = ls[0x4A2C] & 4;
    uint8_t* x = ll_begin(c, (uint32_t)chain, 0x80);
    const int newly = x[0x23] == 0;
    x[0x23] = 1;
    ll_commit(c, (uint32_t)chain, 0x80);
    if (newly && !halt_on_error) {
        x = ll_begin(c, (uint32_t)spurs + 0x80, 0x80);
        uint8_t* st = x + (wid > 15 ? 0x50 + (wid & 15) : wid);
        if (*st <= 1) {
            ll_hold(c, (uint32_t)spurs + 0x80, 0x80);
            jc_unhold(s);
            pm_halt(c, 0xD84);
        }
        if (*st != 3 && *st != 4) {
            *st = 3;
            x[0x3D] = 0xFF;
        }
        ll_commit(c, (uint32_t)spurs + 0x80, 0x80);
        x = ll_begin(c, (uint32_t)spurs, 0x80);
        x[0x72] = 0xFF;
        ll_commit(c, (uint32_t)spurs, 0x80);
    }
    if (newly) {
        uint8_t* q = ls + 0x3FAB0;
        p32(q, err);
        p64(q + 8, cause);
        mfc_seq(c, 0x3FAB0, chain + 0x80, 16, tag, MFC_PUT_CMD);
        tag_wait(c, bit32(tag));
    }
    jc_unhold(s);
    if (halt_on_error) pm_halt(c, 0xEE8);
    longjmp(s->exit, 1);
}

static uint32_t heap_alloc(spu_context* c, uint32_t size)
{
    uint8_t* ls = c->ls;
    uint8_t* hs = ls + 0x4B10;
    uint8_t* hz = ls + 0x4B20;
    const uint32_t k = size > 0x3FBFF ? 0xFFu : ((size + 1023) >> 10) & 0xFF;
    uint8_t ends[16], prev[16], gap[16], v[16];
    vadd(ends, hs, hz);
    prev[0] = ls[0x4A88];
    for (int i = 1; i < 16; i++) prev[i] = ends[i - 1];
    vsub(gap, hs, prev);
    uint32_t idx[16];
    for (int i = 0; i < 16; i++) {
        v[i] = k > gap[i] ? 0xFF : gap[i];
        idx[i] = (uint32_t)i;
    }
    for (int w = 8; w; w >>= 1)
        for (int j = 0; j < w; j++)
            if (v[j] > v[j + w]) {
                v[j] = v[j + w];
                idx[j] = idx[j + w];
            }
    if (v[0] == 0xFF) return 0;
    const uint32_t i = idx[0], start = prev[i];
    for (uint32_t j = 15; j > i; j--) {
        hs[j] = hs[j - 1];
        hz[j] = hz[j - 1];
    }
    hs[i] = (uint8_t)start;
    hz[i] = (uint8_t)k;
    return start;
}

static void heap_free(spu_context* c, uint32_t kb)
{
    kb &= 0xFF;
    if (!kb) return;
    uint8_t* ls = c->ls;
    uint8_t* hs = ls + 0x4B10;
    uint8_t* hz = ls + 0x4B20;
    uint32_t eq = 0;
    for (int i = 0; i < 16; i++)
        if (hs[i] == kb) eq |= 1u << (15 - i);
    const uint32_t m = (eq - 1) & 0xFFFF;
    for (int j = 0; j < 15; j++)
        if ((m >> (14 - j)) & 1) {
            hs[j] = hs[j + 1];
            hz[j] = hz[j + 1];
        }
    hs[15] = ls[0x4A87];
    hz[15] = 0;
}

static void heap_reset(spu_context* c)
{
    memset(c->ls + 0x4B20, 0, 16);
    memset(c->ls + 0x4B10, c->ls[0x4A87], 16);
}

static uint32_t cache_flag(const uint8_t* ls, uint32_t e) { return g32(ls + ((e & 1) ? 0x4B80 : 0x4B70) + 4 * (e >> 1)); }
static void cache_set_flag(uint8_t* ls, uint32_t e, uint32_t v) { p32(ls + ((e & 1) ? 0x4B80 : 0x4B70) + 4 * (e >> 1), v); }

static uint32_t cache_argmax(const uint32_t* age)
{
    uint32_t best = 0, idx = 0;
    for (uint32_t i = 0; i < 8; i++)
        if (age[i] > best) {
            best = age[i];
            idx = i;
        }
    return idx;
}

static uint32_t cache_acquire(spu_context* c, uint64_t ea, uint32_t size, uint32_t is_bin, uint32_t no_lookup)
{
    uint8_t* ls = c->ls;
    uint8_t* hash = ls + 0x4B30;
    uint8_t* rc = ls + 0x4B40;
    uint8_t* slot = ls + 0x4B50;
    uint8_t* age = ls + 0x4B60;
    const uint8_t h = (uint8_t)((((uint32_t)ea >> 7) & 127) + 1);
    for (uint32_t e = 0; e < 8; e++) {
        if (hash[e] != h || !((cache_flag(ls, e) & ~1u) > size - 1) || g64(ls + 0x4B90 + 8 * e) != ea || no_lookup) continue;
        uint8_t one[16] = { 0 };
        one[e] = 1;
        vadd(rc, rc, one);
        for (uint32_t i = 0; i < 8; i++) {
            const uint32_t a = g16(age + 2 * i);
            p16(age + 2 * i, a == 0 ? 0u : a == 0xFFFF ? 0xFFFFu : i == e ? 1u : a + 1);
        }
        return (uint32_t)slot[e] << 24 | e << 16;
    }
    uint32_t e = 8;
    for (uint32_t i = 0; i < 8 && e == 8; i++)
        if (!slot[i]) e = i;
    if (e == 8) {
        uint32_t a[8];
        int any = 0;
        for (uint32_t i = 0; i < 8; i++) {
            a[i] = rc[i] ? 0u : g16(age + 2 * i);
            any |= !rc[i];
        }
        if (!any) return 0;
        e = cache_argmax(a);
        heap_free(c, slot[e]);
        ls[0x4A8A]--;
    }
    const uint32_t kb = heap_alloc(c, size);
    if (kb) {
        p64(ls + 0x4B90 + 8 * e, ea);
        hash[e] = h;
        p16(age + 2 * e, 1);
        cache_set_flag(ls, e, size | is_bin);
        ls[0x4A8A]++;
        rc[e] = 1;
        slot[e] = (uint8_t)kb;
    } else {
        p64(ls + 0x4B90 + 8 * e, 0);
        hash[e] = 0;
        p16(age + 2 * e, 0xFFFF);
        cache_set_flag(ls, e, 0);
        rc[e] = 0;
        slot[e] = 0;
    }
    return kb << 24 | e << 16 | 3u << 8;
}

static uint32_t cache_release(spu_context* c, uint32_t kb, uint32_t e, uint32_t evict)
{
    uint8_t* ls = c->ls;
    if (!(kb & 0xFF)) return 0;
    e &= 7;
    uint8_t* rc = ls + 0x4B40 + e;
    if (*rc) (*rc)--;
    if (!evict || *rc) return 0;
    heap_free(c, ls[0x4B50 + e]);
    p64(ls + 0x4B90 + 8 * e, 0);
    ls[0x4A8A]--;
    ls[0x4B50 + e] = 0;
    ls[0x4B30 + e] = 0;
    p16(ls + 0x4B60 + 2 * e, 0xFFFF);
    return 1;
}

static uint32_t cache_evict_one(spu_context* c)
{
    uint8_t* ls = c->ls;
    uint32_t a[8];
    for (uint32_t i = 0; i < 8; i++) a[i] = !ls[0x4B40 + i] && ls[0x4B50 + i] ? g16(ls + 0x4B60 + 2 * i) : 0u;
    const uint32_t e = cache_argmax(a);
    if (!a[e]) return 1;
    if (cache_flag(ls, e) & 1) memset(ls + ((uint32_t)ls[0x4B50 + e] << 10), 0, 16);
    cache_release(c, 1, e, 1);
    return 0;
}

static void cache_flush(spu_context* c)
{
    while (!cache_evict_one(c)) {}
}

static void cache_reset(spu_context* c)
{
    uint8_t* ls = c->ls;
    memset(ls + 0x4B70, 0, 32);
    memset(ls + 0x4B40, 0, 16);
    memset(ls + 0x4B30, 0, 16);
    memset(ls + 0x4B60, 0xFF, 16);
    for (uint32_t e = 0; e < 8; e++) {
        p64(ls + 0x4B90 + 8 * e, 0);
        const uint32_t s = ls[0x4B50 + e];
        if (s >= ls[0x4A88] && s < ls[0x4A87]) memset(ls + (s << 10), 0, 16);
    }
    ls[0x4A8A] = 0;
    memset(ls + 0x4B50, 0, 16);
}

static uint32_t cache_rekey(spu_context* c, uint32_t kb, uint32_t old)
{
    uint8_t* slot = c->ls + 0x4B50;
    for (uint32_t i = 0; i < 16; i++)
        if (slot[i] == (old & 0xFF)) {
            slot[i] = (uint8_t)kb;
            return 1;
        }
    return 0;
}

static void jc_compact(spu_context* c, uint8_t sv[16], uint32_t d2)
{
    const uint8_t* d = lsp(c, d2);
    const uint32_t ni = g16(d + 0xA) >> 3, nc = g32(d + 0x24) >> 3, n = nc + 2;
    const uint32_t list = d2 + 0x30 + ni * 8;
    struct { uint8_t* ptr; uint32_t size; } e[6];
    if (nc > 4) spurs_fatal("job cache list longer than 4 at compaction", nc);
    heap_reset(c);
    e[0].ptr = sv + 10;
    e[0].size = (g16(d + 8) + g32(d)) << 4;
    e[1].ptr = sv + 3;
    e[1].size = g32(d + 0x14);
    for (uint32_t k = 0; k < nc; k++) {
        e[2 + k].ptr = lsp(c, 0x48E2 + 4 * k);
        e[2 + k].size = (g32(lsp(c, list + 8 * k)) + 15) & ~15u;
    }
    uint32_t prev = 0xFF;
    for (uint32_t j = 0; j < n; j++) {
        for (uint32_t k = j + 1; k < n; k++)
            if (*e[j].ptr > *e[k].ptr) {
                const uint8_t* tp = e[j].ptr;
                const uint32_t ts = e[j].size;
                e[j] = e[k];
                e[k].ptr = (uint8_t*)tp;
                e[k].size = ts;
            }
        const uint32_t old = *e[j].ptr;
        if (j && old == prev) {
            *e[j].ptr = *e[j - 1].ptr;
        } else if (old) {
            const uint32_t kb = heap_alloc(c, e[j].size), dst = kb << 10, src = old << 10;
            for (uint32_t u = 0; u < (e[j].size + 15) >> 4; u += 8) memmove(lsp(c, dst + u * 16), lsp(c, src + u * 16), 128);
            if (cache_rekey(c, kb, old) && !(dst + e[j].size > src)) memset(lsp(c, src), 0, 16);
            *e[j].ptr = (uint8_t)kb;
        }
        prev = old;
    }
}

static uint32_t jc_alloc(spu_context* c, jc_state* s, uint8_t sv[16], uint32_t size, uint32_t d2)
{
    const uint32_t kb = heap_alloc(c, size);
    if (!kb) {
        heap_free(c, sv[5]);
        sv[5] = 0;
        if (sv[7]) {
            heap_free(c, sv[7]);
            memset(c->ls + 0x4890, 0, 16);
            sv[7] = 0;
        }
        if (cache_evict_one(c) == 1) {
            if (sv[0] == 0) {
                heap_free(c, sv[6]);
                sv[6] = 0;
                heap_free(c, sv[4]);
                tag_wait(c, 0xFFFFFFFFu);
                sv[4] = 0;
                sv[0] = 1;
            } else if (sv[0] == 1) {
                jc_compact(c, sv, d2);
                sv[0] = 2;
            } else {
                jc_fatal(c, s, 0x80410A17u, g64(c->ls + 0x4968));
            }
        }
    }
    sv[1] = (uint8_t)kb;
    return kb;
}

static void jc_rollback(spu_context* c, jc_state* s, uint8_t w[16], uint32_t h, int first)
{
    heap_free(c, w[2]);
    w[2] = 0;
    const uint32_t sl = h >> 24;
    if (((sl - 1) & 0xFF) <= 0xFD) cache_release(c, sl, (h >> 16) & 0xFF, (h & 0xFF00) != 0);
    if (!first && cache_evict_one(c) == 1) jc_fatal(c, s, 0x80410A17u, g64(c->ls + 0x4888));
    w[0] = 0xFF;
}

static void jc_stage1(spu_context* c, jc_state* s, uint8_t sv[16], uint32_t* waited, int* first, uint32_t* s1, uint32_t inf[8], uint32_t d1, uint64_t e1)
{
    uint8_t* ls = c->ls;
    uint8_t* d = lsp(c, d1);
    const uint8_t l2f = ls[0x4A2F], ver = ls[0x4A2D], fixed = ls[0x4A2C] & 0x80;
    const uint32_t need = bit32(sv[13]) & ~*waited;
    if (need) {
        tag_wait(c, need);
        *waited |= need;
    }
    d[0x2C] |= (uint8_t)s->b94;
    if ((*first || (l2f & 1)) && (d[0x2C] & 2)) {
        if (g32(d + 0x14)) p32(d + 0x14, g32(d + 0x14) + 16);
        if (g32(d + 0x18)) p32(d + 0x18, g32(d + 0x18) + 16);
        if (ver > 2 || g16(d + 0x1E)) p16(d + 0x1E, g16(d + 0x1E) + 1);
        const uint32_t list = d1 + 0x30 + ((g16(d + 0xA) >> 3) & 0x1FFF) * 8;
        for (uint32_t k = 0; k < g32(d + 0x24) >> 3; k++) p32(lsp(c, list + 8 * k), g32(lsp(c, list + 8 * k)) + 16);
    }
    const uint32_t lo = g32(d + 4), bin16 = g16(d + 8) << 4, x16 = g16(d + 2) << 4;
    if (!(lo & ~3u) || (lo & 12) || !bin16) jc_fatal(c, s, 0x80410A1Fu, e1);
    uint8_t w[16];
    uint32_t h;
    for (;;) {
        const int retry = !*first;
        memcpy(w, sv, 16);
        w[0] = 0;
        w[2] = 0;
        int empty = 1;
        for (int i = 0; i < 16; i++) empty &= ls[0x4B20 + i] == 0;
        h = cache_acquire(c, lo & ~3u, bin16 + x16, 1, lo & 1);
        int nz = 0;
        for (int i = 0; i < 16; i++) nz += ls[0x4B20 + i] != 0;
        int ok = (h >> 24) && (!fixed || empty || (!((h >> 8) & 0xFF) && nz <= 1));
        if (ok) {
            const uint32_t in_size = g32(d + 0x14);
            if (in_size) {
                const uint32_t kb = heap_alloc(c, in_size);
                w[2] = (uint8_t)kb;
                w[0] = kb ? 0 : 0xFF;
            }
            ok = w[0] != 0xFF;
        }
        if (ok) {
            const uint32_t ldl = g16(d + 0xA), nc = g32(d + 0x24) >> 3, list = d1 + 0x30 + ldl;
            if (nc > 4) jc_fatal(c, s, 0x80410A1Eu, e1);
            const uint64_t high = (uint64_t)g32(d + 0x20) << 32;
            uint32_t hs[4] = { 0 }, ins = 0, prev = 0;
            for (uint32_t k = 0; k < nc && ok; k++) {
                const uint32_t sz = g32(lsp(c, list + 8 * k));
                if (!sz) {
                    prev = (prev & 0x00FF00FFu) | 0xFF000000u;
                } else {
                    prev = cache_acquire(c, high | g32(lsp(c, list + 8 * k + 4)), sz, 0, 0);
                    if (!(prev >> 24)) {
                        w[0] = 0xFF;
                        for (uint32_t j = k; j-- > 0;) {
                            const uint32_t sl = hs[j] >> 24;
                            if (((sl - 1) & 0xFF) <= 0xFD) cache_release(c, sl, (hs[j] >> 16) & 0xFF, (hs[j] & 0xFF00) != 0);
                        }
                        ok = 0;
                        break;
                    }
                    ins |= prev & 0xFF00;
                }
                hs[k] = prev;
            }
            if (ok && ins)
                for (uint32_t k = 0; k < nc; k++) {
                    if (!(hs[k] & 0xFF00)) continue;
                    const uint32_t sz = g32(lsp(c, list + 8 * k));
                    if (sz & 7) jc_fatal(c, s, 0x80410A1Du, e1);
                    const uint64_t ea = high | g32(lsp(c, list + 8 * k + 4));
                    mfc_seq(c, (hs[k] >> 24) << 10, ea, sz - ((d[0x2C] & 2) && sz ? 16u : 0u), w[12], MFC_GETF_CMD);
                    w[0] = 1;
                }
            if (ok) {
                for (uint32_t k = 0; k < 4; k++) p16(ls + 0x48E0 + 4 * k, hs[k] >> 16);
                w[8] = (uint8_t)(h >> 24);
                w[9] = (uint8_t)(h >> 16);
                if ((h >> 8) & 0xFF) {
                    mfc_seq(c, (h >> 24) << 10, lo & ~3u, bin16, w[12], MFC_GETF_CMD);
                    w[0] = 1;
                }
                if (lo & 2) w[9] |= 0x80;
                if (w[2]) {
                    if ((ldl & 7) || ldl > 0x4000) jc_fatal(c, s, 0x80410A1Du, e1);
                    mfc_seq(c, (uint32_t)w[2] << 10, (uint64_t)g32(d + 0xC) << 32 | (d1 + 0x30), ldl, w[12], MFC_GETLF_CMD);
                    w[0] = 1;
                }
                memcpy(sv, w, 16);
                *first = 1;
                inf[1] = sv[0] == 1 ? 0xFFFFu : 0u;
                (*s1)++;
                return;
            }
        }
        jc_rollback(c, s, w, h, *first);
        memcpy(sv, w, 16);
        if (!retry) break;
    }
    *first = 0;
    const uint32_t dec = pm_dec(c);
    trace_put(c, 0x4820, g32(ls + 0x4A9C), &dec);
}

static void jc_run(spu_context* c, jc_state* s, uint8_t sv[16], uint32_t inf[8], const uint32_t buf[4], uint32_t sc, uint32_t jt2, const uint8_t q10[16], uint32_t s1, uint32_t waited)
{
    uint8_t* ls = c->ls;
    const uint32_t d2 = buf[2] & SPU_LS_MASK, tag = g32(ls + 0x4A9C);
    const uint8_t* d = ls + d2;
    const uint32_t in = sv[3], out = sv[5], rg = sv[7];
    const uint32_t nc = g32(d + 0x24) >> 3, ni = g16(d + 0xA) >> 3;
    p32(ls + 0x4940, in << 10 | (ni ? g32(lsp(c, d2 + 0x34)) & 15 : 0u));
    for (uint32_t k = 0; k < (nc & 0xFFFF); k++) p32(ls + 0x4944 + 4 * (k & 3), (uint32_t)ls[0x48E2 + 4 * (k & 3)] << 10);
    p16(ls + 0x495A, nc);
    p16(ls + 0x4958, ni);
    p32(ls + 0x4960, rg << 10);
    p32(ls + 0x495C, out << 10);
    ls[0x4954] = (uint8_t)((ls[0x4954] & 0x0F) | (sc << 4));
    inf[0] = out || g32(d + 0x10) ? 1u : 0u;
    if (inf[3]) {
        const uint32_t need = bit32(sv[13]) & ~waited;
        if (need) tag_wait(c, need);
    }
    const uint32_t bin = (uint32_t)sv[10] << 10;
    if (ls[0x4A2D]) {
        const uint32_t m = g32(lsp(c, bin + 0x20));
        const uint32_t b2 = m == 0x42494E32u || m == 0x62696E32u;
        if (g32(d + 0x28) || (g32(d + 0x2C) & 0xFFFFFFu) || b2 != ((uint32_t)(d[0x2C] >> 2) & 1) || (b2 && g32(d) != g16(lsp(c, bin + 0x2E))))
            jc_fatal(c, s, 0x80410A0Bu, g64(ls + 0x4968));
    }
    uint32_t dec = pm_dec(c);
    p64(ls + 0x4818, g64(ls + 0x4968) | (uint64_t)sv[10] << 32);
    trace_put(c, 0x4810, tag, &dec);
    const uint32_t dbg = sv[11] & 0x80;
    sv[11] &= 0x7F;
    const uint32_t stub = g32(lsp(c, bin + 0x10));
    if (stub != 0x44012850u && stub != 0x4400A850u) jc_fatal(c, s, 0x80410A09u, g64(ls + 0x4968));
    if (dbg) {
        tag_wait(c, 0xFFFFFFFFu);
        pm_stopd(c);
    }
    const uint32_t sp = g32(ls + 0x4890) & ~15u;
    uint8_t frame[16];
    for (int i = 0; i < 16; i += 4) p32(frame + i, g32(ls + 0x4890 + i) + 32);
    memset(lsp(c, sp + 0x20), 0, 16);
    memset(lsp(c, sp + 0x10), 0, 16);
    memcpy(lsp(c, sp), frame, 16);
    const u128 b = spu_ls_read128(c, 0x48A0);
    c->gpr[1] = spu_ls_read128(c, 0x4890);
    c->gpr[3] = qsplat(0x4940);
    c->gpr[4] = qwords(b._u32[2], b._u32[3], b._u32[0], b._u32[1]);
    c->gpr[126] = qwords(bin, (uint32_t)sv[14] << 10, (uint32_t)sv[2] << 10, (uint32_t)sv[6] << 10);
    call_lifted(c, bin, 0x2570);
    tag_drain(c, MFC_WrTagUpdate);
    memcpy(ls + 0x3FCA0, ls + 0x48E0, 16);
    if (cache_release(c, sv[10], sv[11], g32(d + 4) & 1)) memset(lsp(c, bin), 0, 16);
    for (uint32_t k = 0; k < g16(ls + 0x495A); k++) {
        const uint8_t* e = ls + 0x3FCA0 + 4 * (k & 3);
        if (e[2] != 0xFF) cache_release(c, e[2], e[3], 0);
    }
    if (!g32(q10)) {
        heap_free(c, sv[3]);
        sv[3] = 0;
    }
    int keep = 0;
    if ((s1 & ~0x80u) == 3) {
        const uint8_t* n1 = lsp(c, buf[1]);
        const uint32_t st = g16(n1 + 0x1C) << 4;
        keep = g32(ls + 0x4898) == (((st ? st : 8192u) + (g16(n1 + 0x1E) << 4) + 1023) & ~1023u);
    }
    if (!keep) {
        heap_free(c, sv[7]);
        sv[7] = 0;
        memset(ls + 0x4890, 0, 16);
    }
    if (jt2 && spu_rchcnt(c, MFC_RdListStallStat) && (spu_rdch(c, MFC_RdListStallStat)._u32[0] & bit32(sv[12])))
        spu_wrch(c, MFC_WrListStallAck, pref(sv[12]));
    dec = pm_dec(c);
    tag_wait(c, bit32(tag));
    p64(ls + 0x4808, guid1(lsp(c, bin)));
    trace_put(c, 0x4800, tag, &dec);
}

static void jc_pipe(spu_context* c, jc_state* s, jc_job* j, uint32_t r[4], uint32_t* busy, uint32_t* lp)
{
    uint8_t* ls = c->ls;
    const uint32_t w1 = j->w1, size = w1 & ~8u, tag = g32(ls + 0x4A9C);
    for (int i = 0; i < 4; i++) p32(ls + 0x4990 + 4 * i, r[i]);
    if (!(size == 64 || size == 128 || (size > 255 && size <= g32(ls + 0x4980) && !(w1 & 0x77)))) jc_fatal(c, s, 0x80410A0Cu, j->ea);
    const uint32_t sc = (size >> 7) & 15;
    uint32_t st[5], inf[8], buf[4];
    uint8_t sv[16];
    for (int i = 0; i < 5; i++) st[i] = g16(ls + 0x48D0 + 2 * i);
    for (int i = 0; i < 8; i++) inf[i] = g16(ls + 0x48C0 + 2 * i);
    for (int i = 0; i < 4; i++) buf[i] = g32(ls + 0x48A0 + 4 * i);
    memcpy(sv, ls + 0x48B0, 16);
    uint64_t e0 = g64(ls + 0x4880), e1 = g64(ls + 0x4888);
    int first = 1;
    for (;;) {
        const uint32_t d2 = buf[2] & SPU_LS_MASK;
        uint8_t q10[16];
        memcpy(q10, ls + d2 + 0x10, 16);
        const uint32_t jt2 = ls[(d2 + 0x2C) & SPU_LS_MASK] & 1;
        uint32_t waited = 0;
        if (j->n && st[0] == 0) {
            e0 = j->ea;
            j->ea += size;
            j->n--;
            st[0] = 1;
        }
        if ((st[4] & ~0x80u) == 5) {
            st[4] = 0;
            if (inf[4]) {
                waited = bit32(sv[15]);
                tag_wait(c, waited);
            }
        }
        uint32_t s2 = st[2];
        if ((st[2] & ~0x80u) == 3) {
            sv[0] = 0;
            const uint32_t out_size = g32(q10 + 8);
            for (;;) {
                uint32_t okb = 0;
                if (out_size)
                    do okb = jc_alloc(c, s, sv, out_size, d2); while (!okb);
                sv[5] = (uint8_t)okb;
                const uint32_t ss = g16(q10 + 12) << 4, stack = ss ? ss : 8192u;
                uint32_t rk = sv[7];
                if (!rk) {
                    const uint32_t need = (g16(q10 + 14) << 4) + stack;
                    do rk = jc_alloc(c, s, sv, need, d2); while (!rk && !out_size);
                    sv[7] = (uint8_t)rk;
                    const uint32_t region = (need + 1023) & ~1023u;
                    p32(ls + 0x4890, (rk << 10) + region - 48);
                    p32(ls + 0x4898, region);
                }
                p32(ls + 0x4894, s->c90 > 0x20FFFF ? stack - 48 : stack);
                if (rk) break;
            }
            s2 = st[2] + 1;
            memcpy(ls + 0x4B00, ls + d2, 8);
            p32(ls + 0x4B08, (uint32_t)sv[10] << 10);
            p32(ls + 0x4B0C, 0);
            if (jt2) {
                uint8_t* list = ls + 0x3FC80;
                p32(list, 0x80000000u);
                p32(list + 4, g32(ls + 0x496C));
                p32(list + 8, 0x80000000u);
                p32(list + 12, g32(ls + 0x496C));
                mfc_seq(c, 0, (uint64_t)g32(ls + 0x4968) << 32 | 0x3FC80, 16, sv[12], MFC_GETL_CMD);
            }
        } else {
            memset(ls + 0x4B00, 0, 16);
        }
        uint32_t s3 = st[3];
        if ((st[3] & ~0x80u) == 4) {
            s3 = st[3] + 1;
            heap_free(c, sv[6]);
            sv[6] = 0;
            heap_free(c, sv[4]);
            sv[4] = 0;
        }
        uint32_t s1 = st[1];
        if ((st[1] & ~0x80u) == 2) jc_stage1(c, s, sv, &waited, &first, &s1, inf, buf[1], e1);
        uint32_t s0 = st[0];
        if ((st[0] & ~0x80u) == 1) {
            if ((uint32_t)e0 & 15) jc_fatal(c, s, 0x80410A1Au, e0);
            mfc_seq(c, buf[0], e0, size, sv[12], MFC_GET_CMD);
            s0 = st[0] + 1;
        }
        for (int i = 0; i < 4; i++) p32(ls + 0x48A0 + 4 * i, buf[i]);
        p64(ls + 0x4880, e0);
        p64(ls + 0x4888, e1);
        p32(ls + 0x4970, j->n);
        p32(ls + 0x4974, size);
        p64(ls + 0x4978, j->ea);
        const uint32_t ns[8] = { s0, s1, s2, s3, st[4], 0, 0, 0 };
        for (int i = 0; i < 8; i++) p16(ls + 0x48D0 + 2 * i, ns[i]);
        inf[0] = 0;
        if ((s2 & ~0x80u) == 4) {
            jc_run(c, s, sv, inf, buf, sc, jt2, q10, s1, waited);
        } else {
            pm_dec(c);
            tag_wait(c, bit32(tag));
        }
        const int stall = !first;
        uint32_t nb[4];
        if (stall) {
            nb[0] = buf[0]; nb[1] = buf[1]; nb[2] = buf[3]; nb[3] = buf[2];
            st[0] = s0;
            st[1] = s1;
            st[2] = 0;
        } else {
            nb[0] = buf[3]; nb[1] = buf[0]; nb[2] = buf[1]; nb[3] = buf[2];
            st[2] = s1;
            st[1] = s0;
            st[0] = 0;
            const uint64_t od1 = e1;
            e1 = e0;
            e0 = od1;
            p64(ls + 0x4968, od1);
            p32(ls + 0x4964, sv[12]);
        }
        st[3] = s2;
        st[4] = s3;
        const uint8_t nsv[16] = { 0, 0, 0, sv[2], sv[3], 0, sv[5], sv[7], 0, 0, sv[8], sv[9], sv[13], sv[12], sv[13], sv[14] };
        memcpy(sv, nsv, 16);
        for (int i = 7; i >= 2; i--) inf[i] = inf[i - 2];
        inf[0] = inf[1] = 0;
        for (int k = 0; k < 4; k++) {
            p16(ls + 0x48E2 + 4 * k, g16(ls + 0x48E0 + 4 * k));
            p16(ls + 0x48E0 + 4 * k, 0);
        }
        for (int i = 0; i < 5; i++) p16(ls + 0x48D0 + 2 * i, st[i]);
        for (int i = 0; i < 4; i++) {
            buf[i] = nb[i];
            p32(ls + 0x48A0 + 4 * i, nb[i]);
        }
        p64(ls + 0x4880, e0);
        p64(ls + 0x4888, e1);
        if (!j->n && !stall) break;
    }
    memcpy(ls + 0x48B0, sv, 16);
    for (int i = 0; i < 8; i++) p16(ls + 0x48C0 + 2 * i, inf[i]);
    const uint32_t any = st[0] | st[1] | st[2] | st[3] | st[4];
    const int label = ((w1 >> 3) & 1) && any;
    if (label)
        for (int i = 0; i < 8; i++)
            if (g16(ls + 0x48D0 + 2 * i)) {
                p16(ls + 0x48D0 + 2 * i, g16(ls + 0x48D0 + 2 * i) | 0x80);
                break;
            }
    const uint32_t mark = label ? any | 0x80 : any;
    *busy = mark & 0x7F;
    *lp = (mark >> 7) & 1;
}

static void jc_split(spu_context* c, uint32_t r[4], jc_job* j)
{
    if (r[1] > 63) {
        j->n = r[0];
        j->w1 = r[1];
        j->ea = (uint64_t)r[2] << 32 | r[3];
        r[0] = r[1] = r[2] = r[3] = 0;
        return;
    }
    const uint32_t k = r[0] != 0;
    j->n = k;
    j->w1 = g16(c->ls + 0x4A70);
    j->ea = g64(lsp(c, r[2] & ~7u));
    r[0] -= k;
    r[2] += 8 * k;
    if (!r[0] && (r[1] & 8)) {
        j->w1 |= 8;
        r[1] &= ~8u;
    }
}

static void jc_grab(spu_context* c, uint32_t r[4])
{
    uint8_t* l = c->ls + 0x4A00;
    const uint32_t slot = l[0x20];
    uint8_t* e = l + 0x50 + 16 * slot;
    const uint32_t max = g16(l + 0x72), num = g32(e), stride = g32(e + 4);
    const uint64_t ea = g64(e + 8);
    const uint32_t k = num > max ? max : num;
    r[0] = k;
    r[1] = stride;
    r[2] = (uint32_t)(ea >> 32);
    r[3] = (uint32_t)ea;
    p32(e, num - k);
    p64(e + 8, ea + (uint32_t)(k * stride));
    if (num - k == 0 && slot) l[0x20] = 0;
}

static void jc_dispatch(spu_context* c, jc_state* s, uint32_t acc[4], uint64_t cmd, uint32_t urgent, uint32_t rv[2])
{
    uint8_t* ls = c->ls;
    uint8_t* l = ls + 0x4A00;
    uint8_t* p = ls + 0x4A80;
    const uint32_t lo = (uint32_t)cmd, op = lo & 7, tag = g32(ls + 0x4A9C);
    const uint64_t t = cmd & ~7ull;
    const uint32_t m8 = bit32(g32(p)) & 0xFF;
    uint64_t pc = g64(l);
    uint32_t p13 = p[13], stop = 0, extra = 0, wait = 0, unknown = 0;
    switch (op) {
    case 0:
        unknown = (uint32_t)t != 0;
        break;
    case 1:
        memset(l, 0, 0x20);
        l[0x20] = 0;
        memset(l + 0x50, 0, 0x20);
        pc = t;
        stop = 1;
        break;
    case 2: {
        if (cmd == 0x0000000800000012ull) {
            stop = 1;
            break;
        }
        const uint32_t label = lo & 8, light = lo & 16;
        if ((label | light) ? (lo & ~0x1Fu) : ((uint32_t)t)) {
            unknown = 1;
            break;
        }
        if (label ? l[0x27] != 0 : (l[0x25] != 0 || acc[0] != 0)) {
            p[12] = 1;
            acc[1] = 2;
            if (!light) p13 = 1;
            stop = 1;
        }
        break;
    }
    case 3:
        if (urgent) pm_halt(c, 0x2F04);
        pc = t;
        stop = 1;
        break;
    case 4: {
        if (l[0x20]) pm_halt(c, 0x2F2C);
        if ((uint32_t)g64(l + 8)) jc_fatal(c, s, 0x80410A1Bu, pc);
        const uint64_t ret = urgent ? pc : pc + 8;
        const uint64_t lr1 = g64(l + 0x10), lr2 = g64(l + 0x18);
        p64(l + 8, lr1);
        p64(l + 0x10, lr2);
        p64(l + 0x18, ret);
        l[0x20] = (uint8_t)urgent;
        pc = t;
        stop = 1;
        break;
    }
    case 5:
        if ((uint32_t)t) {
            unknown = 1;
            break;
        }
        l[0x21] = 0xFF;
        break;
    case 6:
        if (!t) pm_halt(c, 0x2FF4);
        if ((uint32_t)t & 15) jc_fatal(c, s, 0x80410A19u, t);
        if (urgent) l[0x20] = 1;
        mfc_seq(c, 0x4A50 + 16 * l[0x20], t, 16, tag, MFC_GET_CMD);
        if (!urgent) pc += 8;
        stop = 1;
        wait = 1;
        break;
    default: {
        const uint32_t x = lo & 0x7F, olo = lo & ~0x7Fu;
        if (x == 0x17 && !olo) {
            l[0x26] = l[0x25];
            l[0x27] = (uint8_t)(acc[0] ? l[0x25] | m8 : l[0x25]);
            if (acc[0]) acc[1] |= 8;
            extra = 1;
        } else if (x == 0x07 && !olo) {
            jc_fatal(c, s, 0x80410A1Cu, pc);
        } else if (x == 0x0F) {
            const uint32_t g = (uint32_t)(cmd & ~0x7Full);
            uint8_t* gl = ll_begin(c, g, 0x80);
            const int pass = g32(gl) == 0;
            if (pass && g32(gl + 0x20)) p32(gl, g32(gl + 4));
            p32(gl + 0x30, pass ? 0u : 1u);
            ll_commit(c, g, 0x80);
            if (!pass) {
                l[0x2C] |= 8;
                acc[1] = 2;
                p13 = 0;
            } else {
                if (!l[0x24]) {
                    p[11] = 0;
                    l[0x28] = ls[0x93];
                }
                l[0x2C] &= (uint8_t)~8u;
                pc += 8;
            }
            stop = 1;
        } else if (x == 0x77 && !olo) {
            const uint64_t lr0 = g64(l + 8), lr1 = g64(l + 0x10), lr2 = g64(l + 0x18);
            if ((uint32_t)lr2) {
                p64(l + 8, 0);
                p64(l + 0x10, lr0);
                p64(l + 0x18, lr1);
                l[0x20] = 0;
                pc = lr2;
            } else {
                acc[1] = 3;
                p13 = 0;
            }
            stop = 1;
        } else if (x == 0x7F && !olo) {
            acc[1] = 3;
            p13 = 0;
            stop = 1;
        } else {
            unknown = 1;
        }
        break;
    }
    }
    if (unknown) {
        if (urgent) pm_halt(c, 0x331C);
        jc_fatal(c, s, 0x80410A18u, pc);
    }
    p[13] = (uint8_t)p13;
    p64(l, pc + (stop ? 0 : 8));
    rv[0] = stop | extra;
    rv[1] = wait;
}

static void jc_process(spu_context* c, jc_state* s, uint32_t src, uint32_t n, uint32_t urgent, uint32_t out, uint32_t acc[4])
{
    uint8_t* ls = c->ls;
    const uint32_t can = ls[0x4A8B], tag = g32(ls + 0x4A9C);
    uint32_t wait = 0;
    acc[0] = 0;
    acc[1] = 0;
    acc[2] = out;
    acc[3] = 0;
    for (uint32_t end = src + 8 * n; end > src; src += 8) {
        const uint64_t cmd = g64(lsp(c, src));
        if (cmd && !(cmd & 7)) {
            if (!can) break;
            p64(lsp(c, out), cmd);
            out += 8;
            acc[0]++;
            if (!urgent) p64(ls + 0x4A00, g64(ls + 0x4A00) + 8);
            continue;
        }
        uint32_t rv[2];
        jc_dispatch(c, s, acc, cmd, urgent, rv);
        wait = rv[1];
        if (rv[0]) break;
    }
    if (wait) tag_wait(c, bit32(tag));
}

static void jc_lock(spu_context* c, jc_state* s)
{
    uint8_t* ls = c->ls;
    const uint32_t ea = (uint32_t)g64(ls + 0x4A90);
    if (!s->lock) s->lock = chain_lock_of(ea & ~127u);
    ps3_lock_acquire_spin(s->lock, 256);
    s->held = 1;
    for (;;) {
        uint8_t* l = ll_begin(c, ea, 0x4A00);
        if (l[0x23]) {
            ll_hold(c, ea, 0x4A00);
            jc_unhold(s);
            longjmp(s->exit, 1);
        }
        if (!l[0x29]) {
            l[0x29] = 1;
            ll_commit(c, ea, 0x4A00);
            break;
        }
        ll_hold(c, ea, 0x4A00);
    }
    ls[0x4A86] = 0;
}

static void jc_commit(spu_context* c, jc_state* s)
{
    uint8_t* ls = c->ls;
    uint8_t* l = ls + 0x4A00;
    const uint32_t ea = (uint32_t)g64(ls + 0x4A90), k = ls[0x4A86] ? 8u : 0u;
    const uint8_t* f = ll_begin(c, ea, 0x3F980);
    memcpy(l + 0x30, f + 0x30 + k, 32 - k);
    memset(l + 0x50 - k, 0, k);
    l[0x72] = f[0x72];
    l[0x73] = f[0x73];
    l[0x23] = f[0x23];
    l[0x29] = 0;
    ll_commit(c, ea, 0x4A00);
    jc_unhold(s);
}

static void jc_ready(spu_context* c, const uint32_t r[4], uint32_t m8)
{
    uint8_t* ls = c->ls;
    uint8_t* l = ls + 0x4A00;
    uint32_t want;
    if (r[1] & 2) {
        want = ls[0x4A8D];
    } else if (!l[0x24]) {
        want = l[0x28];
    } else {
        want = (uint32_t)__builtin_popcount(l[0x25]) + 1;
        if (l[0x2E] > want && r[0]) return;
    }
    if (want == l[0x2E]) return;
    const uint32_t w = g32(ls + 0x1DC), a = (w > 15 ? 0x110 : 0x100) + (w & 15), spurs = spurs_ea(c);
    ll_begin(c, spurs, 0x100);
    ls[a] = (uint8_t)want;
    ll_commit(c, spurs, 0x100);
    if (l[0x2E] > want) l[0x22] |= (uint8_t)~m8;
    l[0x2E] = (uint8_t)want;
}

static void jc_fetch(spu_context* c, jc_state* s, uint32_t busy, uint32_t lp, uint32_t r[4])
{
    uint8_t* ls = c->ls;
    uint8_t* l = ls + 0x4A00;
    uint8_t* p = ls + 0x4A80;
    const uint32_t m8 = bit32(g32(p)) & 0xFF, tag = g32(ls + 0x4A9C);
    jc_lock(c, s);
    if (l[0x22] & m8) {
        l[0x22] = (uint8_t)(l[0x22] & ~m8);
        p[11] = 0;
    }
    const uint32_t held = busy ? l[0x26] & m8 : 0u;
    l[0x26] = (uint8_t)(l[0x26] & ~m8);
    if (!busy) l[0x25] = (uint8_t)(l[0x25] & ~m8);
    if (!lp && !held) l[0x27] = (uint8_t)(l[0x27] & ~m8);
    p[13] = l[0x2E];
    r[0] = 0; r[1] = 8; r[2] = r[3] = 0;
    if (!held) {
        r[1] = 0;
        int queued = 0;
        for (int i = 0; i < 32; i++) queued |= l[0x30 + i];
        if (!l[0x20] && queued) {
            if (!p[11]) {
                r[1] = 4;
                goto tail;
            }
            const uint64_t pc = g64(l);
            const int guard = l[0x2C] & 8;
            if (guard) mfc_seq(c, 0x3FA80, pc & ~15ull, 16, tag, MFC_GET_CMD);
            if (g64(l + 0x30)) {
                jc_process(c, s, 0x4A30, 1, 1, 0x3FB80, r);
                p[6] = 1;
            }
            if (guard) {
                l[0x2C] &= (uint8_t)~8u;
                tag_wait(c, bit32(tag));
                const uint64_t g = g64(ls + 0x3FA80 + ((uint32_t)pc & 8));
                if ((g & 0x7F) == 0x0F) {
                    memset(ls + 0x3FA80, 0, 16);
                    mfc_seq(c, 0x3FA80, (g & ~0x7Full) + 0x30, 4, tag, MFC_PUT_CMD);
                    tag_wait(c, bit32(tag));
                }
            }
        }
        if (p[6] || (r[1] & 4)) goto tail;
        const uint32_t slot = l[0x20];
        if (g32(l + 0x50 + 16 * slot)) {
            if (p[11]) jc_grab(c, r);
            else r[0] = r[1] = r[2] = r[3] = 0;
            goto tail;
        }
        const uint64_t pc = g64(l);
        atomic_cmd(c, 0x3FB80, pc & ~0x7Full, MFC_GETLLAR_CMD);
        const uint32_t i = ((uint32_t)pc & 0x7F) >> 3, max = g16(l + 0x72);
        const uint32_t n = (int32_t)(16 - i) > (int32_t)max ? max : 16 - i;
        jc_process(c, s, 0x3FB80 + 8 * i, n, 0, 0x3FB80, r);
        if (!r[0] && g32(l + 0x50 + 16 * slot) && p[11]) jc_grab(c, r);
    }
tail:
    if (r[0]) l[0x25] |= (uint8_t)m8;
    if (l[0x21] & m8) {
        cache_flush(c);
        l[0x21] = (uint8_t)(l[0x21] & ~m8);
    }
    jc_ready(c, r, m8);
    jc_commit(c, s);
}

static void jc_main(spu_context* c, jc_state* s)
{
    uint8_t* ls = c->ls;
    uint8_t* p = ls + 0x4A80;
    const uint32_t tag = g32(ls + 0x4A9C);
    uint8_t* pk = ls + 0x3FC40;
    memset(pk, 0, 16);
    pk[0] = 0x2C;
    p32(pk + 8, 0x4A6F6243u);
    p16(pk + 12, 1);
    p16(pk + 14, 0x280);
    uint32_t dec = pm_dec(c);
    trace_put(c, 0x3FC40, tag, &dec);
    uint32_t busy = 0, lp = 0, did = 0, r[4], yield;
    jc_job j;
    p[11] = 0;
    for (;;) {
        p[12] = 0;
        jc_fetch(c, s, busy, lp, r);
        for (;;) {
            jc_split(c, r, &j);
            if (j.n) did = 1;
            do {
                jc_pipe(c, s, &j, r, &busy, &lp);
                jc_split(c, r, &j);
            } while (j.n || (p[12] && busy));
            uint32_t st;
            yield = kernel_poll(c, &st);
            p32(ls + 0x3FC30, st);
            p16(p + 4, yield);
            p[11] = (uint8_t)(st & 1);
            p32(ls + 0x3FC30, st & 1);
            if (!(yield && busy)) break;
        }
        if (!yield) continue;
        if (!did) break;
        did = 0;
        p[11] = 0;
        busy = lp = 0;
    }
    pk = ls + 0x3FC90;
    pk[0] = 0x2D;
    p64(pk + 8, guid1(ls + 0xA00));
    dec = pm_dec(c);
    trace_put(c, 0x3FC90, tag, &dec);
    tag_wait(c, 0xFFFFFFFFu);
    cache_reset(c);
}

static void jc_init(spu_context* c, jc_state* s, uint64_t chain)
{
    uint8_t* ls = c->ls;
    uint8_t* l = ls + 0x4A00;
    uint8_t* p = ls + 0x4A80;
    const uint32_t tag = g32(ls + 0x1CC);
    mfc_seq(c, 0x4A00, chain, 256, tag, MFC_GET_CMD);
    tag_wait(c, bit32(tag));
    s->b94 = ls[0x4A94];
    s->c90 = g32(ls + 0x4A90);
    p64(ls + 0x4A90, chain);
    p32(ls + 0x4A98, 0x100);
    p32(ls + 0x4A9C, tag);
    p32(p, g32(ls + 0x1C8));
    p16(p + 4, 0);
    const uint8_t mb[4] = { l[0x2C], l[0x20], l[0x24], l[0x28] };
    for (int i = 0; i < 4; i++) p32(ls + 0x4980 + 4 * i, ((uint32_t)(mb[i] & 0x70) << 3) + 256);
    const uint32_t max = g32(ls + 0x4980), base = 0x3F800 - ((4 * max + 1023) & 0x3C00);
    memset(ls + 0x4B20, 0, 16);
    p[8] = 19;
    p[7] = (uint8_t)(base >> 10);
    memset(ls + 0x4B10, p[7], 16);
    p[9] = (uint8_t)(p[7] - p[8]);
    memset(ls + 0x48C0, 0, 16);
    memset(ls + 0x4890, 0, 16);
    memset(ls + 0x48D0, 0, 16);
    memset(ls + 0x48E0, 0, 16);
    memset(ls + 0x4880, 0, 16);
    memset(ls + 0x48B0, 0, 16);
    ls[0x48BC] = l[0x2A];
    ls[0x48BD] = l[0x2B];
    for (uint32_t i = 0; i < 4; i++) p32(ls + 0x48A0 + 4 * i, base + i * max);
    tag_drain(c, MFC_WrTagUpdate);
    cache_reset(c);
}

static void jc_entry(spu_context* c, uint64_t chain)
{
    uint8_t* ls = c->ls;
    memset(ls + 0x3FFF0, 0, 16);
    psplat(ls + 0x3FFD0, 0x3FFF0);
    memset(ls + 0x3FFE0, 0, 16);
    jc_state s;
    memset(&s, 0, sizeof s);
    jc_init(c, &s, chain);
    if (!setjmp(s.exit)) jc_main(c, &s);
}

static void kernel_dispatch(spu_context* c, const uint32_t r[4])
{
    uint8_t* ls = c->ls;
    const uint64_t info = g64(ls + 0x1C0) + (r[0] > 15 ? 0xD00u : 0xB00u + r[0] * 32);
    memset(ls + 0x3FFD0, 0, 16);
    psplat(ls + 0x3FFB0, 0x3FFD0);
    mfc(c, 0x3FFE0, info, 32, 31, MFC_GET_CMD);
    tag_wait(c, 0x80000000u);
    const uint64_t pm = g64(ls + 0x3FFE0);
    if (pm != g64(ls + 0x1D0)) {
        mfc(c, 0xA00, pm, g32(ls + 0x3FFF0), 31, MFC_GET_CMD);
        tag_wait(c, 0x80000000u);
        p64(ls + 0x1D0, pm);
        p32(ls + 0x1D8, ls[0x3FFF4]);
    }
    p16(ls + 0x1E8, 0);
    const uint64_t arg = g64(ls + 0x3FFE8);
    if (pm == s_pm_jobchain) jc_entry(c, arg);
    else if (pm == s_pm_taskset) ts_entry(c, arg, r[1]);
    else if (pm == s_pm_service) ss_entry(c, arg, r[1]);
    else if (pm == s_pm_jobchain_plain) spurs_fatal("cellSpursCreateJobChain policy module dispatched", pm);
    else spurs_fatal("unknown SPURS policy module dispatched", pm);
}

static void spurs_kernel(spu_context* c)
{
    p64(c->ls + 0x1C0, (uint64_t)c->gpr[4]._u32[0] << 32 | c->gpr[4]._u32[1]);
    p32(c->ls + 0x1C8, c->gpr[3]._u32[0]);
    uint32_t r[4] = { 0x20, 0, 0, 0 };
    for (;;) {
        kernel_dispatch(c, r);
        kernel_select(c, 0, r);
    }
}

void spurs_register(uint32_t service_pm, uint32_t taskset_pm, uint32_t jobchain_pm, uint32_t jobchain_plain_pm)
{
    s_pm_service = service_pm;
    s_pm_taskset = taskset_pm;
    s_pm_jobchain = jobchain_pm;
    s_pm_jobchain_plain = jobchain_plain_pm;
    g_spu_idle_wake = ss_idle_wake;
    g_spu_idle_slots = ss_idle_slots;
    spu_begin_image(SPURS_IMAGE);
    spu_register_function(0x818, spurs_kernel);
    spu_register_function(0xA70, ts_syscall);
    spu_begin_image(0);
}
