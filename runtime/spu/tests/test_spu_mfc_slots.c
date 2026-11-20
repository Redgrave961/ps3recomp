#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint8_t guest_memory[1u << 20];
uint8_t* vm_base = guest_memory;
uint32_t ppu_vm_size = 0;

#include "../spu_channels.c"
#include "../spu_lifted_job.h"

static int g_pass, g_fail;

static void check(int ok, const char* what)
{
    if (ok) { g_pass++; return; }
    g_fail++;
    printf("  FAIL: %s\n", what);
}

#define CHECK(cond)  check((cond), #cond)

#define NCTX    12
#define ROUNDS  4

static spu_context* g_ctx[ROUNDS][NCTX];

static mfc_engine*  g_engine[ROUNDS][NCTX];
static int          g_fresh[ROUNDS][NCTX];
static int          g_stable[ROUNDS][NCTX];
static int          g_private[ROUNDS][NCTX];
static int          g_timeout;

static volatile LONG g_bar_arrived;
static volatile LONG g_bar_gen;

static void barrier_wait(void)
{
    LONG gen = g_bar_gen;
    if (InterlockedIncrement(&g_bar_arrived) == NCTX) {
        g_bar_arrived = 0;
        InterlockedIncrement(&g_bar_gen);
        return;
    }
    ULONGLONG start = GetTickCount64();
    while (g_bar_gen == gen) {
        if (GetTickCount64() - start > 10000) { g_timeout = 1; return; }
        YieldProcessor();
    }
}

static void* claim_thread(void* arg)
{
    int w = (int)(intptr_t)arg;

    for (int r = 0; r < ROUNDS; r++) {
        spu_context* ctx = g_ctx[r][w];

        barrier_wait();
        mfc_engine* e = mfc_for(ctx);
        g_engine[r][w] = e;
        g_fresh[r][w]  = (e->tag_completed == 0xFFFFFFFFu) && (e->queue_count == 0);
        g_stable[r][w] = (mfc_for(ctx) == e);

        uint32_t mark = 0xA5000000u | (uint32_t)(r * NCTX + w);
        e->tag_completed = mark;
        e->queue_count   = (uint32_t)w + 1u;

        barrier_wait();
        g_private[r][w] = (e->tag_completed == mark) &&
                          (e->queue_count == (uint32_t)w + 1u);

        barrier_wait();
        spu_mfc_release(ctx);
    }
    return NULL;
}

static void transient_job(spu_context* ctx)
{
    mfc_engine* engine = mfc_for(ctx);
    CHECK(engine->queue_count == 0);
    CHECK(engine->tag_completed == 0xFFFFFFFFu);
    engine->queue_count = 7;
    engine->tag_completed = 0;
}

int main(void)
{
    printf("SPU MFC engine slots\n");

    for (int r = 0; r < ROUNDS; r++) {
        for (int w = 0; w < NCTX; w++) {
            g_ctx[r][w] = (spu_context*)calloc(1, sizeof(spu_context));
            if (!g_ctx[r][w]) {
                printf("  FAIL: out of memory for %d SPU contexts\n", ROUNDS * NCTX);
                return 1;
            }
        }
    }

    pthread_t th[NCTX];
    for (int w = 0; w < NCTX; w++)
        if (pthread_create(&th[w], NULL, claim_thread, (void*)(intptr_t)w) != 0) {
            printf("  FAIL: could not start worker %d\n", w);
            return 1;
        }
    for (int w = 0; w < NCTX; w++)
        pthread_join(th[w], NULL);

    CHECK(!g_timeout);

    for (int r = 0; r < ROUNDS; r++) {
        int distinct = 1;
        for (int w = 0; w < NCTX; w++) {
            if (!g_engine[r][w]) { distinct = 0; continue; }
            for (int v = w + 1; v < NCTX; v++)
                if (g_engine[r][w] == g_engine[r][v]) distinct = 0;
        }
        CHECK(distinct);
        for (int w = 0; w < NCTX; w++) {
            CHECK(g_fresh[r][w]);
            CHECK(g_stable[r][w]);
            CHECK(g_private[r][w]);
        }
    }

    spu_context* idle = (spu_context*)calloc(1, sizeof(spu_context));
    CHECK(idle != NULL);
    spu_mfc_release(idle);

    spu_context* held[NCTX];
    mfc_engine* helde[NCTX];
    for (int i = 0; i < NCTX; i++) {
        held[i] = (spu_context*)calloc(1, sizeof(spu_context));
        CHECK(held[i] != NULL);
        spu_context_init(held[i], (uint32_t)i);
        helde[i] = mfc_for(held[i]);
        uint32_t ea = 0x20000u + (uint32_t)i * 0x100u;
        memset(guest_memory + ea, i + 1, 16);
        memset(guest_memory + ea + 16, i + 0x80, 16);
        spu_ls_write32(held[i], 0x1000, 0x80000010u);
        spu_ls_write32(held[i], 0x1004, ea);
        spu_ls_write32(held[i], 0x1008, 16);
        spu_ls_write32(held[i], 0x100C, ea + 16);
        held[i]->mfc_lsa = 0x2000;
        held[i]->mfc_eal = 0x1000;
        held[i]->mfc_size = 16;
        held[i]->mfc_tag = 3;
        spu_wrch(held[i], MFC_Cmd, spu_make_preferred_u32(MFC_GETL_CMD));
        CHECK(held[i]->ls[0x2000] == i + 1);
        CHECK(held[i]->ls[0x2010] == 0);
    }
    for (int i = 0; i < NCTX; i++) {
        CHECK(mfc_for(held[i]) == helde[i]);
        CHECK(!(helde[i]->tag_completed & (1u << 3)));
        CHECK(held[i]->list_stall_mask == (1u << 3));
        spu_wrch(held[i], MFC_WrListStallAck, spu_make_preferred_u32(3));
        for (unsigned n = 0; n < 16; ++n)
            CHECK(held[i]->ls[0x2010 + n] == i + 0x80);
        CHECK(helde[i]->tag_completed & (1u << 3));
        CHECK(held[i]->list_stall_mask == 0);
        for (int j = i + 1; j < NCTX; j++)
            CHECK(!(helde[j]->tag_completed & (1u << 3)));
    }
    helde[3]->queue_count = 7;
    helde[3]->tag_completed = 0;
    spu_mfc_release(held[3]);
    mfc_engine* reusere = mfc_for(held[3]);
    CHECK(reusere->queue_count == 0);
    CHECK(reusere->tag_completed == 0xFFFFFFFFu);
    for (int i = 0; i < NCTX; i++) {
        spu_mfc_release(held[i]);
        spu_mfc_release(held[i]);
        free(held[i]);
    }
    free(idle);
    for (int r = 0; r < ROUNDS; r++)
        for (int w = 0; w < NCTX; w++) free(g_ctx[r][w]);

    spu_context* range = calloc(3, sizeof(*range));
    spu_context* outside = calloc(1, sizeof(*outside));
    CHECK(range != NULL && outside != NULL);
    for (unsigned i = 0; i < 3; ++i) (void)mfc_for(&range[i]);
    mfc_engine* outside_engine = mfc_for(outside);
    outside_engine->tag_completed = 0x12345678;
    spu_mfc_forget_range((uintptr_t)range, (uintptr_t)(range + 3));
    CHECK(mfc_for(outside) == outside_engine);
    CHECK(outside_engine->tag_completed == 0x12345678);
    CHECK(s_mfc_slots && s_mfc_slots->ctx == outside && !s_mfc_slots->next);
    free(range);
    spu_mfc_release(outside);
    free(outside);

    for (unsigned i = 0; i < 12; ++i) {
        CHECK(spu_run_lifted_job_abi(transient_job, NULL, 0, 0, NULL) == 0);
        CHECK(s_mfc_slots == NULL);
    }
    printf("SPU MFC engine slots: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

void ps3_ww_report_inline(uint32_t a, uint64_t v, int w)
{
    (void)a; (void)v; (void)w;
}
unsigned long long ps3_ms_now(void)                    { return 0; }
uint32_t g_ww_lo = 0xFFFFFFFFu, g_ww_hi = 0;

void (*g_spurs_kernel_hook)(uint32_t) = 0;
void spurs_ef_set_from_spu(uint32_t ea, uint16_t bits) { (void)ea; (void)bits; }

uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
void spu_thread_publish_ctx(uint32_t tid, void* ctx) { (void)tid; (void)ctx; }
