#ifndef SPU_CONTEXT_H
#define SPU_CONTEXT_H

#include "../../include/ps3emu/ps3types.h"
#include "../../include/ps3emu/log_first.h"
#include "../platform/ps3_sync.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPU_ALIGN16 __attribute__((aligned(16)))

#define SPU_LS_SIZE         (256 * 1024)
#define SPU_LS_MASK         (SPU_LS_SIZE - 1)

#define SPU_MBOX_DEPTH      1
#define SPU_INTR_MBOX_DEPTH 1

#define SPU_RdEventStat     0
#define SPU_WrEventMask     1
#define SPU_WrEventAck      2
#define SPU_RdSigNotify1    3
#define SPU_RdSigNotify2    4
#define SPU_WrDec           7
#define SPU_RdDec           8
#define SPU_RdEventMask     11
#define SPU_RdMachStat      13
#define SPU_WrSRR0          14
#define SPU_RdSRR0          15
#define SPU_WrOutMbox       28
#define SPU_RdInMbox        29
#define SPU_WrOutIntrMbox   30

#define MFC_WrMSSyncReq     9
#define MFC_RdTagMask       12
#define MFC_LSA             16
#define MFC_EAH             17
#define MFC_EAL             18
#define MFC_Size            19
#define MFC_TagID           20
#define MFC_Cmd             21
#define MFC_WrTagMask       22
#define MFC_WrTagUpdate     23
#define MFC_RdTagStat       24
#define MFC_RdListStallStat 25
#define MFC_WrListStallAck  26
#define MFC_RdAtomicStat    27

#define MFC_PUT_CMD         0x20
#define MFC_GET_CMD         0x40
#define MFC_GETB_CMD        0x41
#define MFC_GETF_CMD        0x42
#define MFC_GETL_CMD        0x44
#define MFC_GETLF_CMD       0x46
#define MFC_SDCRZ_CMD       0x89
#define MFC_BARRIER_CMD     0xC0
#define MFC_EIEIO_CMD       0xC8
#define MFC_SYNC_CMD        0xCC

#define MFC_GETLLAR_CMD     0xD0
#define MFC_PUTLLC_CMD      0xB4
#define MFC_PUTLLUC_CMD     0xB0
#define MFC_PUTQLLUC_CMD    0xB8
#define MFC_ATOMIC_LINE     128
#define SPU_RESV_BY_CONTENT 2u

#define SPU_CHANNEL_CAP      64
#define SPU_IN_MBOX_HW_DEPTH 4

typedef struct spu_channel {
    uint32_t value;
    uint32_t count;
    uint32_t q[SPU_CHANNEL_CAP];
    uint32_t head;
    ps3_lock lock;
} spu_channel;

typedef struct spu_context {
    SPU_ALIGN16 u128 gpr[128];

    SPU_ALIGN16 uint8_t ls_store[SPU_LS_SIZE];
    uint8_t* ls;

    uint32_t pc;

    uint32_t status;
    #define SPU_STATUS_STOPPED      0x0
    #define SPU_STATUS_RUNNING      0x1
    #define SPU_STATUS_STOPPED_BY_STOP  0x2
    #define SPU_STATUS_STOPPED_BY_HALT  0x4
    #define SPU_STATUS_WAITING_CHANNEL  0x8

    uint32_t stop_code;

    uint32_t spu_id;
    uint32_t spu_group_id;

    int image_id;

    int park_on_empty_inmbox;

    uint32_t decrementer;
    uint64_t dec_base_ns;

    uint32_t srr0;

    uint32_t event_status;
    uint32_t event_mask;

    spu_channel ch_out_mbox;
    spu_channel ch_in_mbox;
    uint32_t rcv_evt[4];
    int      rcv_evt_n;
    int      rcv_evt_i;
    spu_channel ch_out_intr_mbox;
    spu_channel ch_sig_notify[2];

    uint32_t mfc_lsa;
    uint32_t mfc_eah;
    uint32_t mfc_eal;
    uint32_t mfc_size;
    uint32_t mfc_tag;

    uint32_t mfc_tag_mask;
    uint32_t mfc_tag_status;

    uint32_t resv;
    uint32_t atomic_stat;
    uint32_t idle_bit, idle_run, idle_prio, idle_claim;
    uint8_t  resv_line[128];

    int resident_ovl;
#define SPU_RESIDENT_CODE_SLOTS 16u
    struct {
        uint32_t lsa, size, source_ea;
        int image_id;
        int verify_state, source_index;
        uint32_t canon;
    } resident_code[SPU_RESIDENT_CODE_SLOTS];
    int32_t reloc_delta;

    uint32_t dcache_id, dcache_residency;

    struct mfc_engine* mfc;
    volatile int ch_sleeping;

    uint32_t host_depth;

    uint32_t steps;

    uint64_t lockstep_quantum_ctr;
    uint64_t lockstep_release_tb;
    uint64_t dec_start_tb;

    volatile uint32_t ch_wake_seq;

    uint32_t int_enable;
    void*    irq_frame;

    uint32_t list_stall_mask;
    uint32_t list_stall_stat;
    uint32_t list_stall_elem_lsa[32];
    uint32_t list_stall_remaining[32];
    uint32_t list_stall_dest_lsa[32];
    uint64_t list_stall_ea_base[32];
    uint32_t list_stall_cmd[32];

    int      irq_saved;
    uint32_t irq_resume_pc;
    u128     irq_gpr[128];

    uint32_t drain_ret_pc;

    volatile int stop_request;

    int coh_listed;
} spu_context;

#define SPU_EVENT_SET(c, b)   __atomic_fetch_or(&(c)->event_status, (uint32_t)(b), __ATOMIC_SEQ_CST)
#define SPU_EVENT_CLEAR(c, b) __atomic_fetch_and(&(c)->event_status, ~(uint32_t)(b), __ATOMIC_SEQ_CST)

void spu_mfc_release(spu_context* ctx);
void spu_mfc_forget_range(uintptr_t lo, uintptr_t hi);

static inline void spu_context_init(spu_context* ctx, uint32_t spu_id)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->ls     = ctx->ls_store;
    ctx->spu_id = spu_id;
    ctx->status = SPU_STATUS_STOPPED;
}

#if defined(__GNUC__) || defined(__clang__)
#define SPU_BSWAP32(x) __builtin_bswap32(x)
#define SPU_LS_FAST 1
#endif

static inline uint32_t spu_ls_read32(const spu_context* ctx, uint32_t lsa)
{
    lsa &= SPU_LS_MASK;
    const uint8_t* p = &ctx->ls[lsa];
#ifdef SPU_LS_FAST
    uint32_t w;
    memcpy(&w, p, 4);
    return SPU_BSWAP32(w);
#else
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
#endif
}

static inline void spu_ls_write32(spu_context* ctx, uint32_t lsa, uint32_t val)
{
    lsa &= SPU_LS_MASK;
    uint8_t* p = &ctx->ls[lsa];
#ifdef SPU_LS_FAST
    uint32_t w = SPU_BSWAP32(val);
    memcpy(p, &w, 4);
#else
    p[0] = (uint8_t)(val >> 24);
    p[1] = (uint8_t)(val >> 16);
    p[2] = (uint8_t)(val >> 8);
    p[3] = (uint8_t)(val);
#endif
}
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif
#if !defined(SPU_LS_VEC) && !defined(__ARM_NEON) && defined(__SSSE3__) && \
    defined(__x86_64__)
#define SPU_LS_VEC 1
#endif
#if SPU_LS_VEC
#include <tmmintrin.h>
static inline __m128i spu_ls_bswap_words(__m128i x) {
    return _mm_shuffle_epi8(x, _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12));
}
#endif

static inline u128 spu_ls_ld128(const uint8_t* ls, uint32_t lsa)
{
    u128 v;
    lsa &= SPU_LS_MASK & ~0xFu;
    const uint8_t* p = &ls[lsa];
#if defined(__ARM_NEON)
    vst1q_u8(v._u8, vrev32q_u8(vld1q_u8(p)));
#elif SPU_LS_VEC
    _mm_storeu_si128((__m128i*)v._u8, spu_ls_bswap_words(_mm_loadu_si128((const __m128i*)p)));
#elif SPU_LS_FAST
    uint32_t w0, w1, w2, w3;
    memcpy(&w0, p,      4); memcpy(&w1, p + 4,  4);
    memcpy(&w2, p + 8,  4); memcpy(&w3, p + 12, 4);
    v._u32[0] = SPU_BSWAP32(w0); v._u32[1] = SPU_BSWAP32(w1);
    v._u32[2] = SPU_BSWAP32(w2); v._u32[3] = SPU_BSWAP32(w3);
#else
    for (int i = 0; i < 4; i++) {
        v._u32[i] = ((uint32_t)p[i*4]     << 24) |
                    ((uint32_t)p[i*4 + 1] << 16) |
                    ((uint32_t)p[i*4 + 2] <<  8) |
                    (uint32_t)p[i*4 + 3];
    }
#endif
    return v;
}

static inline void spu_ls_st128(uint8_t* ls, uint32_t lsa, u128 val)
{
    lsa &= SPU_LS_MASK & ~0xFu;
    uint8_t* p = &ls[lsa];
#if defined(__ARM_NEON)
    vst1q_u8(p, vrev32q_u8(vld1q_u8(val._u8)));
#elif SPU_LS_VEC
    _mm_storeu_si128((__m128i*)p, spu_ls_bswap_words(_mm_loadu_si128((const __m128i*)val._u8)));
#elif SPU_LS_FAST
    uint32_t w0 = SPU_BSWAP32(val._u32[0]), w1 = SPU_BSWAP32(val._u32[1]);
    uint32_t w2 = SPU_BSWAP32(val._u32[2]), w3 = SPU_BSWAP32(val._u32[3]);
    memcpy(p,      &w0, 4); memcpy(p + 4,  &w1, 4);
    memcpy(p + 8,  &w2, 4); memcpy(p + 12, &w3, 4);
#else
    for (int i = 0; i < 4; i++) {
        uint32_t w = val._u32[i];
        p[i*4]     = (uint8_t)(w >> 24);
        p[i*4 + 1] = (uint8_t)(w >> 16);
        p[i*4 + 2] = (uint8_t)(w >>  8);
        p[i*4 + 3] = (uint8_t)w;
    }
#endif
}
static inline u128 spu_ls_read128(const spu_context* ctx, uint32_t lsa) { return spu_ls_ld128(ctx->ls, lsa); }
static inline void spu_ls_write128(spu_context* ctx, uint32_t lsa, u128 val) { spu_ls_st128(ctx->ls, lsa, val); }

static inline u128 spu_make_preferred_u32(uint32_t val)
{
    u128 r;
    memset(&r, 0, sizeof(r));
    r._u32[0] = val;
    return r;
}

#define SPU_CH_LOCK(ch)   ps3_lock_acquire(&(ch)->lock)
#define SPU_CH_UNLOCK(ch) ps3_lock_release(&(ch)->lock)

static inline void spu_channel_write(spu_channel* ch, uint32_t val)
{
    SPU_CH_LOCK(ch);
    if (ch->count >= SPU_CHANNEL_CAP) {
        SPU_CH_UNLOCK(ch);
        return;
    }
    ch->q[(ch->head + ch->count) % SPU_CHANNEL_CAP] = val;
    ch->value = ch->q[ch->head];
    ch->count++;
    SPU_CH_UNLOCK(ch);
}

static inline uint32_t spu_channel_read(spu_channel* ch)
{
    SPU_CH_LOCK(ch);
    uint32_t val = ch->value;
    if (ch->count) {
        ch->head = (ch->head + 1u) % SPU_CHANNEL_CAP;
        if (ch->count > 1) ch->value = ch->q[ch->head];
        ch->count--;
    }
    SPU_CH_UNLOCK(ch);
    return val;
}

static inline int spu_channel_has_data(const spu_channel* ch)
{
    return ch->count > 0;
}

#define SPU_THREAD_LOCAL __thread

void spu_indirect_branch(spu_context* ctx);

extern SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*);

extern SPU_THREAD_LOCAL uint32_t g_spu_pch[8];
extern SPU_THREAD_LOCAL unsigned g_spu_pch_n;

void yz_lockstep_tick(spu_context* ctx);
void spu_drain_hop(spu_context* ctx);

void spu_ch_wake(spu_context* ctx);
void spu_depth_guard(spu_context* ctx);
extern uint32_t g_spu_depth_guard_limit;
static inline void spu_depth_guard_check(spu_context* ctx)
{
    uint32_t limit = g_spu_depth_guard_limit;
    if (!limit || ctx->host_depth >= limit / 2) spu_depth_guard(ctx);
}
void (*spu_take_interrupt(spu_context* ctx,
                          void (*tf)(spu_context*)))(spu_context*);
extern volatile int g_spu_direct;
void spu_drain_call(spu_context* ctx, uint32_t return_pc);
static inline int spu_call_return(spu_context* ctx, uint32_t return_pc)
{
    if (__builtin_expect(!g_spu_trampoline_fn && !((ctx->pc ^ return_pc) & SPU_LS_MASK) && g_spu_direct >= 0, 1)) {
        spu_depth_guard_check(ctx);
        return 1;
    }
    spu_drain_call(ctx, return_pc);
    return 0;
}
#define SPU_DRAIN(ctx) do {                                    \
        spu_depth_guard_check(ctx);                            \
        while (g_spu_trampoline_fn) {                           \
            void (*_tf)(spu_context*) = g_spu_trampoline_fn;    \
            g_spu_trampoline_fn = 0;                            \
            spu_drain_hop(ctx);                                 \
            if ((ctx)->int_enable &&                            \
                ((ctx)->event_status & (ctx)->event_mask))      \
                _tf = spu_take_interrupt((ctx), _tf);          \
            g_spu_pch[g_spu_pch_n++ & 7u] =                    \
                (uint32_t)((ctx)->pc & SPU_LS_MASK);           \
            _tf(ctx);                                          \
        }                                                      \
    } while (0)

void spu_direct_init(void);
#if defined(__clang__) && defined(__has_attribute)
#  if __has_attribute(musttail)
#    define SPU_DIRECT_TAILCALLS 1
#  endif
#endif
#ifdef SPU_DIRECT_TAILCALLS
#define SPU_DIRECT(ctx, PC, FN) do {                                          \
        const uint32_t _dpc = (uint32_t)(PC);                                 \
        (ctx)->pc = _dpc;                                                     \
        if (__builtin_expect(g_spu_direct > 0, 1) && (_dpc & SPU_LS_MASK) &&  \
            (ctx)->steps &&                                                   \
            !((ctx)->int_enable && ((ctx)->event_status & (ctx)->event_mask))) \
            __attribute__((musttail)) return FN(ctx);                         \
        g_spu_trampoline_fn = FN;                                             \
        return;                                                               \
    } while (0)
#else
#define SPU_DIRECT(ctx, PC, FN) do {                                          \
        (ctx)->pc = (PC);                                                     \
        g_spu_trampoline_fn = FN;                                             \
        return;                                                               \
    } while (0)
#endif

#define SPU_RET(ctx) do {                                      \
        (ctx)->pc = (ctx)->gpr[0]._u32[0];                     \
        if ((ctx)->host_depth == 0)                            \
            g_spu_trampoline_fn = spu_indirect_branch;         \
        return;                                                \
    } while (0)

#define SPU_RET_REG(ctx, n) do {                                       (ctx)->pc = (ctx)->gpr[n]._u32[0];                             if ((ctx)->host_depth == 0)                                        g_spu_trampoline_fn = spu_indirect_branch;                 return;                                                    } while (0)

void (*spu_lifted_lookup(const spu_context* ctx, uint32_t lsa))(spu_context*);
static inline void (*spu_icall_target(spu_context* ctx))(spu_context*)
{
    ctx->pc &= SPU_LS_MASK;
    if (ctx->irq_saved) return 0;
    return spu_lifted_lookup(ctx, ctx->pc);
}
#define SPU_ICALL(ctx) do {                                                   \
        void (*_icf)(spu_context*) = spu_icall_target(ctx);                   \
        if (__builtin_expect(_icf != 0, 1)) _icf(ctx);                        \
        else spu_indirect_branch(ctx);                                        \
    } while (0)

#ifdef __cplusplus
}
#endif

#endif
