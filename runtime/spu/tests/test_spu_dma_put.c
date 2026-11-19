/*
 * MFC PUT against reserved lines: the reserved-range scan agrees with a
 * per-line scan for every span, a PUT over a peer's reserved line costs that
 * peer its reservation, the issuer keeps its own, and an unreserved PUT copies
 * exactly and touches no reservation.
 *
 * Build:
 *   clang -std=gnu17 -I include -I runtime/spu \
 *         -o /tmp/test_spu_dma_put runtime/spu/tests/test_spu_dma_put.c \
 *         runtime/spu/tests/spu_fixture_ppu.c runtime/platform/win32_compat.c \
 *         runtime/platform/ps3_sync.c -lpthread
 */

#include "../spu_coherency.c"
#include "../../ppu/ppu_memory.h"
#include "../spu_dma.h"

#include <stdio.h>
#include <string.h>

static uint8_t g_guest_mem[1u << 20];
uint8_t* vm_base = g_guest_mem;
void (*g_rsx_window_write)(void) = 0;

uint32_t g_ww_lo = 0xFFFFFFFFu, g_ww_hi = 0;
void ps3_ww_report_inline(uint32_t addr, uint64_t val, int width)
{
    (void)addr; (void)val; (void)width;
}
void spu_ch_wake(spu_context* ctx) { (void)ctx; }
void spu_overlay_note_get(spu_context* ctx, uint32_t ea, const uint8_t* ls, uint32_t size)
{
    (void)ctx; (void)ea; (void)ls; (void)size;
}

static int g_pass, g_fail;
#define CHECK(cond) do { if (cond) g_pass++; else { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static spu_context g_issuer;
static spu_context g_peer;

static void getllar(spu_context* ctx, uint32_t ea)
{
    ea &= ~127u;
    spu_lockline_lock(ea);
    spu_coh_reserve(ctx, ea);
    ctx->resv = (ea & ~127u) | 1u;
    spu_lockline_unlock(ea);
}

static void reset(spu_context* ctx, uint32_t id)
{
    spu_context_init(ctx, 0);
    ctx->spu_id = id;
}

static uint32_t rng = 0x12345678u;
static uint32_t next(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static void test_span_scan(void)
{
    static unsigned char bits[64];
    for (int round = 0; round < 2000; round++) {
        for (unsigned i = 0; i < sizeof bits; i++) bits[i] = (next() % 9u) ? 0 : (unsigned char)(1u << (next() & 7u));
        const uint32_t first_line = next() % 400u, count = 1u + next() % 100u;
        const uint32_t first = first_line << 7, last = (first_line + count - 1u) << 7;
        int expect = 0;
        for (uint32_t a = first; ; a += 128u) {
            if (mfc_line_reserved(bits, a)) expect = 1;
            if (a == last) break;
        }
        CHECK(mfc_span_reserved(bits, first, last) == expect);
    }
    CHECK(mfc_span_reserved(NULL, 0, 1u << 20) == 0);
    CHECK(mfc_line_reserved(NULL, 0) == 0);
}

static void put(spu_context* issuer, uint32_t ea, uint32_t size)
{
    for (uint32_t i = 0; i < size; i++) issuer->ls[0x1000 + i] = (uint8_t)(i * 7u + 3u);
    CHECK(mfc_do_transfer(issuer, 0x1000, ea, size, MFC_PUT_CMD) == 0);
    CHECK(memcmp(g_guest_mem + ea, issuer->ls + 0x1000, size) == 0);
}

static void test_put_semantics(void)
{
    const uint32_t base = 0x00080000u;
    const uint32_t spans[][2] = { {0, 16}, {0, 128}, {0, 4096}, {128 * 7, 128}, {128 * 7, 16384}, {16, 16384 - 16} };
    for (unsigned s = 0; s < sizeof spans / sizeof spans[0]; s++) {
        const uint32_t ea = base + 0x8000u * s + spans[s][0], size = spans[s][1];
        const uint32_t first = ea & ~127u, last = (ea + size - 1u) & ~127u;
        const uint32_t probes[] = { first, last, first + ((last - first) / 256u) * 128u };
        for (unsigned p = 0; p < 3; p++) {
            reset(&g_issuer, 0);
            reset(&g_peer, 1);
            getllar(&g_peer, probes[p]);
            put(&g_issuer, ea, size);
            CHECK((g_peer.resv & 1u) == 0);
            CHECK((g_peer.event_status & SPU_EVENT_LR) != 0);
        }
        reset(&g_issuer, 0);
        reset(&g_peer, 1);
        getllar(&g_peer, last + 128u);
        getllar(&g_issuer, first);
        put(&g_issuer, ea, size);
        CHECK((g_peer.resv & 1u) == 1);
        CHECK((g_peer.event_status & SPU_EVENT_LR) == 0);
        CHECK((g_issuer.resv & 1u) == 1);
    }
    reset(&g_issuer, 0);
    reset(&g_peer, 1);
    getllar(&g_peer, 0x000C0000u);
    put(&g_issuer, 0x000D0000u, 16384);
    CHECK((g_peer.resv & 1u) == 1);
    CHECK((g_peer.event_status & SPU_EVENT_LR) == 0);
}

int main(void)
{
    reset(&g_issuer, 0);
    put(&g_issuer, 0x00070000u, 16384);
    test_span_scan();
    test_put_semantics();
    printf("test_spu_dma_put: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail != 0;
}
