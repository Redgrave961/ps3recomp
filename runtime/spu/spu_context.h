/*
 * ps3recomp - SPU (Synergistic Processing Unit) execution context
 *
 * Models the full architectural state of an SPU:
 *   - 128 x 128-bit general-purpose registers
 *   - 256 KB local store
 *   - Channel state (MFC command queue, mailboxes, signal notification)
 */

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

/* Portable 16-byte alignment (MSVC __declspec vs GCC/clang __attribute__). */
#if defined(_MSC_VER)
#  define SPU_ALIGN16 __declspec(align(16))
#else
#  define SPU_ALIGN16 __attribute__((aligned(16)))
#endif

/* Local store size: 256 KB */
#define SPU_LS_SIZE         (256 * 1024)
#define SPU_LS_MASK         (SPU_LS_SIZE - 1)

/* Maximum number of MFC tag groups */
#define SPU_MFC_MAX_TAGS    32

/* Mailbox / signal capacities */
#define SPU_MBOX_DEPTH      1    /* SPU write outbound mailbox depth */
#define SPU_INTR_MBOX_DEPTH 1    /* SPU write inbound interrupt mailbox depth */

/* ---------------------------------------------------------------------------
 * SPU channel IDs
 * -----------------------------------------------------------------------*/
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

/* MFC channels */
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

/* ---------------------------------------------------------------------------
 * MFC DMA command opcodes
 * -----------------------------------------------------------------------*/
#define MFC_PUT_CMD         0x20
#define MFC_PUTB_CMD        0x21
#define MFC_PUTF_CMD        0x22
#define MFC_GET_CMD         0x40
#define MFC_GETB_CMD        0x41
#define MFC_GETF_CMD        0x42
#define MFC_PUTL_CMD        0x24
#define MFC_PUTLB_CMD       0x25
#define MFC_PUTLF_CMD       0x26
#define MFC_GETL_CMD        0x44
#define MFC_GETLB_CMD       0x45
#define MFC_GETLF_CMD       0x46
#define MFC_SNDSIG_CMD      0xA0
#define MFC_SDCRZ_CMD       0x89   /* storage-control range zero (no LS operand) */
#define MFC_BARRIER_CMD     0xC0
#define MFC_EIEIO_CMD       0xC8
#define MFC_SYNC_CMD        0xCC

/* Atomic (lock-line reservation) commands -- operate on a 128-byte line. */
#define MFC_GETLLAR_CMD     0xD0   /* get + reserve line                      */
#define MFC_PUTLLC_CMD      0xB4   /* store conditional (fails if line moved) */
#define MFC_PUTLLUC_CMD     0xB0   /* store unconditional, clears reservation */
#define MFC_PUTQLLUC_CMD    0xB8   /* queued store unconditional              */
#define MFC_ATOMIC_LINE     128

/* ---------------------------------------------------------------------------
 * Channel state
 * -----------------------------------------------------------------------*/
/* Channel capacity. Most SPU channels are genuinely one deep, but the PPU->SPU
 * inbound mailbox is FOUR on hardware, and a title that sends a multi-word
 * message writes the words back to back after checking the free-slot count. A
 * one-deep channel drops all but one of them, and the SPU then blocks forever
 * on a message it half received -- silently, because nothing is wrong with
 * either side. The Orange Box sends CB.SPU descriptors of two words from one
 * call site and FIVE from another -- one more than the mailbox is deep, so a
 * whole message never fits at once and the sender has to poll free slots while
 * the SPU drains, exactly as on hardware. (The rcv_evt[4] queue below is the
 * same problem, solved once ad hoc for sys_spu_thread_receive_event's
 * four-word reply.) */
/* Capacity is NOT the hardware depth, deliberately.
 *
 * Hardware is four deep and back-pressures the writer: the PPU polls the
 * free-slot count and its store stalls until there is room. We cannot model
 * that half -- an MMIO store into the problem-state window is a plain memory
 * write on the host, already committed by the time the handler runs, so there
 * is no way to refuse it. Anything that does not fit is simply lost.
 *
 * Losing one word of a multi-word descriptor desynchronises the reader
 * permanently: it takes the words it did get, then waits for a word that has
 * already been and gone. The Orange Box sends CB.SPU bursts of two words and
 * of five -- one more than hardware depth -- and its worker reads a long
 * sequence across several call sites, so a single lost word strands it.
 *
 * So buffer generously and let the free-slot count keep advertising the
 * hardware depth (SPU_IN_MBOX_HW_DEPTH): a guest that polls sees the numbers it
 * expects, and a guest that does not poll still cannot lose data. */
#define SPU_CHANNEL_CAP      64
#define SPU_IN_MBOX_HW_DEPTH 4

typedef struct spu_channel {
    uint32_t value;   /* head: the value the next read returns */
    uint32_t count;   /* number of valid entries, 0..SPU_CHANNEL_CAP */
    uint32_t q[SPU_CHANNEL_CAP];
    uint32_t head;
    ps3_lock lock;
} spu_channel;

/* ---------------------------------------------------------------------------
 * SPU execution context
 * -----------------------------------------------------------------------*/
typedef struct spu_context {
    /* 128 general-purpose 128-bit registers */
    SPU_ALIGN16 u128 gpr[128];

    /* 256 KB local store, 16-byte aligned, addressed through `ls`.
     *
     * Indirect because a RAW SPU's local store is not private: lv2 maps it into
     * the process at 0xE0000000 + n*0x100000, and the PPU writes the SPU's code
     * and its command buffers there WHILE IT RUNS (runtime/spu/spu_raw.c). Such
     * a context points `ls` straight at guest memory; every other context points
     * it at ls_store, so no other path changes -- ctx->ls[i], &ctx->ls[i] and
     * ctx->ls + n all still compile and mean the same thing. */
    SPU_ALIGN16 uint8_t ls_store[SPU_LS_SIZE];
    uint8_t* ls;

    /* Program counter (local store address, 0-0x3FFFF) */
    uint32_t pc;

    /* SPU status (running, stopped, etc.) */
    uint32_t status;
    #define SPU_STATUS_STOPPED      0x0
    #define SPU_STATUS_RUNNING      0x1
    #define SPU_STATUS_STOPPED_BY_STOP  0x2
    #define SPU_STATUS_STOPPED_BY_HALT  0x4
    #define SPU_STATUS_WAITING_CHANNEL  0x8

    /* 14-bit signal code of the most recent `stop`/`stopd`. SPURS leaf tasks
     * invoke kernel syscalls via `stop <code>` (CELL_SPURS_TASK_SYSCALL_EXIT=0,
     * YIELD=1, WAIT_SIGNAL=2, POLL=3, RECV_WKL_FLAG=4; |0x10 = the v2 form). */
    uint32_t stop_code;
    #define SPU_STATUS_SINGLE_STEP  0x10

    /* SPU thread identification */
    uint32_t spu_id;
    uint32_t spu_group_id;

    /* Active recompiled image for per-context indirect-branch dispatch. SPURS
     * loads kernel/policy/job into overlapping LS addresses at different times,
     * so spu_indirect_branch resolves pc within the image currently selected
     * here. 0 = match any image (back-compat for single-image contexts). */
    int image_id;

    /* When set, a rchcnt(SPU_RdInMbox) that finds the inbound mailbox EMPTY halts
     * the SPU (spu_halt longjmp) instead of returning 0. Lets a persistent-worker
     * SPU (e.g. the Rubber Ducky AsyncCopy raw SPU) run SYNCHRONOUSLY: it does its
     * full init + ready-mailbox handshake, then parks the first time it idle-waits
     * for a PPU command -- no async host thread racing the PPU. */
    int park_on_empty_inmbox;

    /* Decrementer: a free-running down counter, ticking at the PS3 timebase.
     * SPU_WrDec latches the reload value here and stamps dec_base_ns; SPU_RdDec
     * derives the live value from the host clock. Storing only the written
     * value (and returning it unchanged) makes every timed wait immortal --
     * see SPU_RdDec in spu_channels.c. */
    uint32_t decrementer;
    uint64_t dec_base_ns;

    /* SRR0 - Save/Restore Register (exception return address) */
    uint32_t srr0;

    /* Event status / mask */
    uint32_t event_status;
    uint32_t event_mask;

    /* Channels */
    spu_channel ch_out_mbox;        /* SPU -> PPU outbound mailbox */
    spu_channel ch_in_mbox;         /* PPU -> SPU inbound mailbox */
    /* sys_spu_thread_receive_event (stop 0x110) replies with FOUR values --
     * {CELL_OK, data1, data2, data3} -- and the worker reads them back with
     * four rdch SPU_RdInMbox. spu_channel holds one, so queue the reply here
     * and let the in-mailbox read drain it first. */
    uint32_t rcv_evt[4];
    int      rcv_evt_n;             /* remaining unread reply words */
    int      rcv_evt_i;
    spu_channel ch_out_intr_mbox;   /* SPU -> PPU interrupt mailbox */
    spu_channel ch_sig_notify[2];   /* Signal notification 1 & 2 */

    /* MFC command staging registers (written via channels before issuing cmd) */
    uint32_t mfc_lsa;
    uint32_t mfc_eah;
    uint32_t mfc_eal;
    uint32_t mfc_size;
    uint32_t mfc_tag;

    /* MFC tag completion mask and status */
    uint32_t mfc_tag_mask;
    uint32_t mfc_tag_status;

    /* Atomic reservation state (GETLLAR/PUTLLC lock-line semantics). Multiple
     * SPU kernel threads share a lock-free queue in main memory; PUTLLC must
     * FAIL if the reserved 128-byte line changed since GETLLAR, or concurrent
     * claims corrupt the queue. */
    uint32_t resv;             /* reserved 128-byte line EA | 1 while live (0 = none) */
    uint32_t atomic_stat;      /* last atomic op result -> MFC_RdAtomicStat */
    uint8_t  resv_line[128];   /* snapshot of the line at GETLLAR time */

    /* Resident code-overlay image id (0 = none). FMOD's mixer swaps per-DSP
     * plugin code in and out of ONE LS scratch region (e.g. the Compressor
     * streams over the Fader plugin's slot), so one static per-image function
     * registry cannot hold both: their lifted addresses overlap. The MFC GET
     * path recognizes a registered overlay's source EA and records which
     * overlay is now resident; dispatch retries a missed lookup against it. */
    int resident_ovl;
    /* Independently streamed code buffers can coexist with the policy overlay.
     * Each mapping records which translated image owns that local-store span. */
#define SPU_RESIDENT_CODE_SLOTS 16u
    struct {
        uint32_t lsa, size, source_ea;
        int image_id;
        /* 0: unchecked legacy region; otherwise SPU_OVERLAY_* state of a
         * region registered with its code bytes (spu_overlay_residency.h). */
        int verify_state, source_index;
        /* Nonzero for position-independent code lifted once at this canonical LS base
         * (spu_overlay_register_region_reloc): its functions are registered at
         * canon + offset, wherever the title placed this copy. 0 = ordinary placed lift. */
        uint32_t canon;
    } resident_code[SPU_RESIDENT_CODE_SLOTS];
    /* Placement minus canonical base of the relocatable code running now (spu_lifter.py
     * --reloc): lifted relocatable code adds it to every address it derives from its own
     * pc. Set by the dispatcher on entry into such code; only that code reads it. */
    int32_t reloc_delta;

    /* Dispatch cache (spu_lifted_lookup, spu_channels.c). dcache_id names this
     * context's current lifetime: 0 after spu_context_init (or a zeroed
     * allocation) until its first cached dispatch, so a context reset at the
     * same address never meets an earlier lifetime's entries. dcache_residency
     * is bumped on every change to resident_code, dropping the cached
     * resolutions of this context. */
    uint32_t dcache_id, dcache_residency;

    struct mfc_engine* mfc;
    volatile int ch_sleeping;

    /* --- SPU_DRAIN trampoline execution model (faithful-adopt, canersaka) ---
     * host_depth counts live lifted call frames (matched brsl/bisl). SPU_RET
     * (`bi $r0`) does a host `return` while host_depth>0 (the caller's frame is
     * live); at 0 it dispatches to the link register instead (a cross-context
     * task resume / restack unwind killed the frame). */
    uint32_t host_depth;

    /* Trampoline steps taken since this context started running. Only needed
     * to tell a job's INITIAL entry at LS 0 from a later return to LS 0, which
     * means something quite different (see spu_drain.c). */
    uint32_t steps;

    /* SPU lockstep gate (spu_lockstep.c; env SPU_LOCKSTEP, default off).
     * quantum_ctr counts tick sites toward a token handoff; release_tb stamps
     * the guest-timebase moment this ctx last released the token. dec_start_tb
     * is the lockstep decrementer-freeze anchor (written only while armed). */
    uint64_t lockstep_quantum_ctr;
    uint64_t lockstep_release_tb;
    uint64_t dec_start_tb;

    /* Channel-stall contract (spu_channels.c): a blocking spu_rdch on an empty
     * RdInMbox/RdSigNotify/RdEventStat parks the SPU host thread on this per-SPU
     * CV until a producer (mailbox/signal write, event raise) calls spu_ch_wake.
     * Storage for a Win32 CONDITION_VARIABLE + SRWLOCK -- each is pointer-sized
     * and zero-init is the valid initial value (CONDITION_VARIABLE_INIT /
     * SRWLOCK_INIT == {0}), so spu_context_init's memset is enough. Kept as
     * void* to avoid pulling <windows.h> into this widely-included header;
     * spu_channels.c casts to the real types. */
    void* ch_wait_cv;
    void* ch_wait_lock;

    /* --- SPU interrupt facility (faithful; WWS jobmanager depends on it) ---
     * The bi-family E/D bits (bie/bid, irete/iretd...) toggle int_enable; while
     * enabled, a pending (event_status & event_mask) vectors execution to LS 0
     * at the next trampoline transfer (SPU_DRAIN hook -> spu_take_interrupt):
     * srr0 <- interrupted pc, int_enable <- 0, pc <- target of the guest-planted
     * branch instruction AT LS 0 (pm_wwsjob's entry writes `bra 0xA2C` there).
     * The handler returns via iret (pc <- srr0). */
    uint32_t int_enable;
    /* The host frame that took the pending interrupt (spu_drain_call's loop),
     * innermost first. The WWS handler leaves through a routine it assembles
     * at the top of local store, from wherever its state machine happens to
     * be -- several lifted calls deep -- so the iret rarely returns to the
     * frame that entered the handler. When the iret's register restore fires
     * deeper than the taking frame, execution unwinds to that frame and goes
     * on at srr0 from there; the handler's host frames hold nothing the guest
     * still needs. Without it the interrupted code resumed INSIDE the
     * handler's frames and its return address then missed every drain on the
     * way up (drain-mismatch return_pc=0xA90 pc=0x2D28, once per job). */
    void*    irq_frame;

    /* Parked DMA-list commands (stall-and-notify), keyed PER TAG GROUP. A
     * GETL/PUTL that transfers a list element with the stall-and-notify bit
     * (bit 31 of word0) set raises SPU event 0x2 (SPU_EVENT_SN) and parks the
     * remainder here; MFC_WrListStallAck for the tag resumes it.
     *
     * The WWS job manager pipelines Load/Run/Store stages, each issuing its own
     * barriered-null-list interrupt on a DISTINCT tag (readCommands=0,
     * writeShareableBuffers=3, writeAllShareableBuffers=5, writeJobBuffers=6 --
     * see jobmanagerdmatags.h), so multiple stalls can be outstanding at once.
     * A single-slot park dropped all but the last -> a pipeline stage's
     * completion interrupt was lost -> the job ring deadlocked. Hardware (and
     * RPCS3) accumulate: ch_stall_stat ORs (1<<tag) per stall; rdch returns the
     * accumulated mask then clears it; ch_stall_mask tracks parked tags and is
     * cleared per-tag by WrListStallAck. Modelled here as per-tag arrays. */
    uint32_t list_stall_mask;             /* bit t set = tag t has a parked list */
    uint32_t list_stall_stat;             /* readable notify mask (rdch clears) */
    uint32_t list_stall_elem_lsa[32];     /* LS addr of NEXT element, per tag */
    uint32_t list_stall_remaining[32];    /* elements left after stall, per tag */
    uint32_t list_stall_dest_lsa[32];     /* accumulated destination LSA, per tag */
    uint64_t list_stall_ea_base[32];      /* ea base (hi32 carries), per tag */
    uint32_t list_stall_cmd[32];          /* base (non-list) MFC command, per tag */

    /* Register file saved when an interrupt is taken, restored at its iret
     * (spu_drain.c). Per context: it used to be an 8-slot global table claimed
     * without a lock, and two SPU threads taking interrupts at once could take
     * the same slot -- one lost its save and returned from the handler with
     * the handler's registers. */
    int      irq_saved;
    uint32_t irq_resume_pc;
    u128     irq_gpr[128];

    /* Return pc of the innermost spu_drain_call (0 = none). The interpreter
     * rejoins there: a return point sits mid-function, so it is no lifted
     * entry, and without this the interpreter ran a job's return straight on
     * through the caller's code until the next lifted entry -- GH3's job
     * manager then ran its buffer loop a second time on the job's registers
     * and died in its own assert (the song-freeze). */
    uint32_t drain_ret_pc;

    /* Set by another host thread to stop this SPU from outside: a raw SPU's
     * PPU writing SPU_RunCntl = 0 (spu_raw.c). Lifted code never looks at it;
     * the channel layer does, at every rdch/rchcnt and inside its blocking
     * waits, and halts the SPU there (spu_halt). Cleared by spu_context_init. */
    volatile int stop_request;

    /* Nonzero while this context is in spu_coherency.c's list of reservation
     * holders, so its GETLLAR can skip the lock-line lock (see
     * spu_coh_reserve_fast). Written under that lock, read by the context's
     * own thread. */
    int coh_listed;
} spu_context;

/* event_status is written from more than one host thread: the SPU's own
 * (Sn from a list stall, Tg, its own Lr poll, WrEventAck) and others (a PPU
 * store or a peer SPU's PUT/PUTLLC raising Lr through spu_coherency.c). A
 * plain |= on each side loses bits: the Sn of a stalled DMA list vanished
 * under a concurrent Lr raise, leaving the list parked (list_stall_stat set)
 * with Sn clear, and the job manager that armed it waiting for its interrupt
 * for ever. Every update goes through these. */
#if defined(_MSC_VER)
#include <intrin.h>
#define SPU_EVENT_SET(c, b)   _InterlockedOr((volatile long*)&(c)->event_status, (long)(uint32_t)(b))
#define SPU_EVENT_CLEAR(c, b) _InterlockedAnd((volatile long*)&(c)->event_status, (long)~(uint32_t)(b))
#else
#define SPU_EVENT_SET(c, b)   __atomic_fetch_or(&(c)->event_status, (uint32_t)(b), __ATOMIC_SEQ_CST)
#define SPU_EVENT_CLEAR(c, b) __atomic_fetch_and(&(c)->event_status, ~(uint32_t)(b), __ATOMIC_SEQ_CST)
#endif

/* ---------------------------------------------------------------------------
 * Initialization
 * -----------------------------------------------------------------------*/
/* Release private DMA state only when the execution owner has stopped using
 * the context. Pair with spu_coh_unregister before a reset or destruction. */
void spu_mfc_release(spu_context* ctx);
void spu_mfc_forget_range(uintptr_t lo, uintptr_t hi);

static inline void spu_context_init(spu_context* ctx, uint32_t spu_id)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->ls     = ctx->ls_store;   /* a raw SPU repoints this at guest memory */
    ctx->spu_id = spu_id;
    ctx->status = SPU_STATUS_STOPPED;
}

/* ---------------------------------------------------------------------------
 * Local store access helpers
 * -----------------------------------------------------------------------*/
static inline uint32_t spu_ls_read32(const spu_context* ctx, uint32_t lsa)
{
    lsa &= SPU_LS_MASK;
    const uint8_t* p = &ctx->ls[lsa];
    /* SPU local store is big-endian */
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static inline void spu_ls_write32(spu_context* ctx, uint32_t lsa, uint32_t val)
{
    lsa &= SPU_LS_MASK;
    uint8_t* p = &ctx->ls[lsa];
    p[0] = (uint8_t)(val >> 24);
    p[1] = (uint8_t)(val >> 16);
    p[2] = (uint8_t)(val >> 8);
    p[3] = (uint8_t)(val);
}

/* Local-store quadword access.
 *
 * LS bytes are big-endian (the SPU's native order); registers in our
 * model are stored in native (little-endian on x86) order so that
 * `_u32[i]` directly gives lane i's value and the preferred slot is
 * `r._u32[0]`. The byte-swap below converts between the two on
 * every quadword load/store, mirroring what `spu_ls_read32` already
 * does explicitly. Doing the swap here (rather than in every channel
 * extractor) keeps the per-word `_u32[i]` semantics that the lifter
 * helpers in `spu_helpers.h` were written against. */
/* The 128-bit LS accessors are THE hot primitive of lifted SPU execution
 * (every load/store in a Bink movie frame's ~millions of lifted instructions
 * runs through here). SPU_LS_FAST swaps each word with a bswap; on an SSSE3
 * x86 host SPU_LS_VEC (below) does the whole BE<->LE quadword swizzle as one
 * byte-shuffle, as rev32 does on NEON, with the same per-lane _u32 semantics. */
#if defined(_MSC_VER)
#include <stdlib.h>
#define SPU_BSWAP32(x) _byteswap_ulong(x)
#define SPU_LS_FAST 1
#elif defined(__GNUC__) || defined(__clang__)
#define SPU_BSWAP32(x) __builtin_bswap32(x)
#define SPU_LS_FAST 1
#endif
#if defined(__ARM_NEON)
#include <arm_neon.h>     /* the quadword swap is one rev32 */
#endif
/* SPU_LS_VEC: the quadword as one 16-byte load or store and one pshufb that
 * byte-swaps each word. The four 32-bit stores of the scalar form, read back by
 * the next 16-byte load of the register, cannot be store-forwarded: a stall on
 * every lqd/lqx of lifted code. Define SPU_LS_VEC=0 to keep the scalar form. */
#if !defined(SPU_LS_VEC) && !defined(__ARM_NEON) && defined(__SSSE3__) && \
    (defined(__x86_64__) || defined(_M_X64))
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
    vst1q_u8(v._u8, vrev32q_u8(vld1q_u8(p)));     /* ldr q; rev32 */
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
    vst1q_u8(p, vrev32q_u8(vld1q_u8(val._u8)));   /* rev32; str q */
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

/* ---------------------------------------------------------------------------
 * Channel read/write helpers
 * -----------------------------------------------------------------------*/
/* `value` stays the head, and `count` the number of entries, so the many places
 * that read those two fields directly keep working unchanged.
 *
 * Write and read are serialised by a per-channel spinlock. A mailbox has its
 * producer and consumer on different host threads (PPU MMIO store vs the SPU's
 * rdch), and unlocked `count++` / `count--` lose updates: Tornado Outbreak's
 * raw SPU consumed one mailbox word while the PPU queued the next, count landed
 * on 0 with a word still queued, and the PPU waited forever for a reply to a
 * command the SPU never saw. The reader could also see the new count before
 * the new value. Plain reads of `count` elsewhere stay lock-free. */
#define SPU_CH_LOCK(ch)   ps3_lock_acquire(&(ch)->lock)
#define SPU_CH_UNLOCK(ch) ps3_lock_release(&(ch)->lock)

static inline void spu_channel_write(spu_channel* ch, uint32_t val)
{
    SPU_CH_LOCK(ch);
    if (ch->count >= SPU_CHANNEL_CAP) {
        /* Full. Hardware does not accept the write at all -- the sender polls
         * the free-slot count first -- so the NEW word is what is lost.
         *
         * Dropping the oldest instead silently REORDERS the queue, which is
         * worse than losing a word: a reader taking a fixed-length message off
         * the mailbox then gets a prefix of one message spliced to the tail of
         * the next, and every field after the splice is garbage that still
         * looks plausible. The Orange Box sends CB.SPU a five-word descriptor
         * from five consecutive call sites -- one more than the mailbox is
         * deep -- so it meets this on every send. */
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

/* ---------------------------------------------------------------------------
 * SPU_DRAIN trampoline execution model (faithful-adopt, from canersaka's fork).
 *
 * Replaces the musttail SPU_TAILCALL chain: a cross-function transfer sets
 * ctx->pc + g_spu_trampoline_fn and RETURNS (unwinds the host stack); the
 * enclosing SPU_DRAIN loop re-enters. This keeps the host stack bounded and
 * gives ONE central per-transfer hook (lockstep tick, later flight recorder).
 * The lifter (tools/spu_lifter.py) emits: calls as a bracketed nested host call
 * + SPU_DRAIN; tail/indirect branches as trampoline-set + return; `bi $r0` as
 * SPU_RET. Hooks below are STUBS for now (spu_drain.c) -- lockstep/fltrec land
 * in later milestones.
 * -----------------------------------------------------------------------*/
#if defined(_MSC_VER)
#  define SPU_THREAD_LOCAL __declspec(thread)
#else
#  define SPU_THREAD_LOCAL __thread
#endif

/* Indirect-branch dispatcher (spu_channels.c): resolves ctx->pc to a lifted
 * function in the active image and runs it. Referenced by SPU_RET/SPU_DRAIN. */
void spu_indirect_branch(spu_context* ctx);

/* Pending cross-function transfer target; NULL when none. Thread-local: each
 * spu_context is pinned to one host thread for its lifetime. */
extern SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*);

/* Ring of the last PCs this SPU thread executed, for the unlifted-branch
 * report. The dispatcher records indirect branches; SPU_DRAIN records every
 * trampoline hop, which is the one that actually precedes a bad branch. */
extern SPU_THREAD_LOCAL uint32_t g_spu_pch[8];
extern SPU_THREAD_LOCAL unsigned g_spu_pch_n;

/* Central per-transfer hooks (stubbed in spu_drain.c until their milestones). */
void yz_lockstep_tick(spu_context* ctx);             /* round-robin token gate */
void spu_drain_hop(spu_context* ctx);

/* Wake a host thread blocked in a channel wait (channel-stall milestone). */
void spu_ch_wake(spu_context* ctx);
/* Runaway lifted-call recursion guard. A lifted brsl/bisl nests a real host
 * call frame, so an SPU loop the lifter lowered as a CALL rather than a
 * transfer recurses until the host stack dies -- surfacing as a bare
 * STACKOVERFLOW backtrace of unsymbolised RVAs that says nothing about which
 * SPU code is looping. Report the SPU pc/image once and halt instead.
 * SPU_HOST_DEPTH_MAX overrides the limit. */
void spu_depth_guard(spu_context* ctx);
extern uint32_t g_spu_depth_guard_limit; /* initialized by spu_depth_guard */
static inline void spu_depth_guard_check(spu_context* ctx)
{
    /* Zero initializes (and preserves a configured zero); upper-half calls
     * must still record the same diagnostic ring before the final limit. */
    uint32_t limit = g_spu_depth_guard_limit;
    if (!limit || ctx->host_depth >= limit / 2) spu_depth_guard(ctx);
}
/* Take a pending SPU interrupt (spu_drain.c): srr0 <- ctx->pc, int_enable <- 0,
 * decode the guest-planted branch at LS 0 into ctx->pc, and return the
 * dispatcher to run instead of the interrupted transfer. Returns `tf`
 * unchanged (interrupt not taken) if LS 0 holds no branch instruction. */
void (*spu_take_interrupt(spu_context* ctx,
                          void (*tf)(spu_context*)))(spu_context*);
extern volatile int g_spu_direct;
/* Drain a translated call up to its explicit architectural return PC. */
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
/* Drain the pending trampoline chain: run each queued transfer target until
 * none remain. The one central hook site for the faithful execution model. */
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

/* A lifted cross-function branch with a known target (br, the conditional
 * branches, a fall-through into the next function). It used to set the
 * trampoline and return, and the enclosing drain loop called the target: a
 * host return, a loop pass and an indirect call per transfer. SPU_DIRECT
 * tail-calls the target instead (no host stack growth) when nothing the drain
 * loop does per step applies: the target is not LS 0 (a job's end), this is
 * not the run's first transfer (the LS 0 check counts steps from there), and
 * no interrupt is pending with interrupts enabled. Anything else takes the trampoline as before. Tail calls need
 * clang's musttail; other compilers always take the trampoline. SPU_LOCKSTEP,
 * which watches each drain step, turns it off (spu_drain.c). */
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

/* Depth-aware SPU link return (`bi $r0`): host `return` while a matched
 * brsl/bisl frame is live (host_depth>0); at 0 the frame was destroyed (task
 * resume / restack unwind) so dispatch to the link register instead -- faithful
 * to CBEA `bi` (branch to word 0 of RA). pc set unconditionally. */
#define SPU_RET(ctx) do {                                      \
        (ctx)->pc = (ctx)->gpr[0]._u32[0];                     \
        if ((ctx)->host_depth == 0)                            \
            g_spu_trampoline_fn = spu_indirect_branch;         \
        return;                                                \
    } while (0)

/* Host return through a NON-r0 link register. Sony's SPU compiler links
 * leaf/helper calls through r4/r5/r6/r8/r78 (`brsl $r4, helper` ...
 * `bi $r4`); SPU_RET would publish r0 as the return PC, which is not where
 * the guest is going, and spu_drain_call's return-address check can then
 * never match -- it restarts dispatch from r0's stale value instead. Same
 * host-frame return as SPU_RET, publishing the register actually branched. */
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
} /* extern "C" */
#endif

#endif /* SPU_CONTEXT_H */
