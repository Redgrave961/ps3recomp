#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t g_guest_mem[1u << 20];
uint8_t* vm_base = g_guest_mem;
uint32_t ppu_vm_size = 0;
void spu_thread_publish_ctx(uint32_t tid, void* ctx) { (void)tid; (void)ctx; }

#include "../spu_channels.c"

static int g_pass, g_fail;
#define CHECK(cond) do {                                                    \
    if (cond) g_pass++;                                                     \
    else { g_fail++; fprintf(stderr, "FAIL at %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

static const uint32_t k_sizes[] = { 1, 2, 4, 8, 16, 32, 48, 64, 80, 96, 112, 128, 144, 256, 272, 4096, 8192, 16384 };

static uint32_t g_rng = 0x9E3779B9u;
static uint8_t rnd(void)
{
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
    return (uint8_t)g_rng;
}
static void fill(uint8_t* p, uint32_t n) { for (uint32_t i = 0; i < n; i++) p[i] = rnd(); }

static uint8_t g_a[40u << 10], g_b[40u << 10], g_seed[40u << 10];

static void test_copy_matches_memmove(void)
{
    fill(g_seed, sizeof g_seed);
    for (unsigned k = 0; k < sizeof k_sizes / sizeof *k_sizes; k++) {
        const uint32_t n = k_sizes[k], step = n < 16 ? n : 16;
        for (uint32_t so = 0; so < 64; so += 16)
            for (uint32_t dlt = 0; dlt <= 256; dlt += step)
                for (int dir = -1; dir <= 1; dir += 2) {
                    const uint32_t s = 0x1000u + so, d = (uint32_t)((int32_t)s + dir * (int32_t)dlt);
                    memcpy(g_a, g_seed, sizeof g_a);
                    memcpy(g_b, g_seed, sizeof g_b);
                    mfc_copy(g_a + d, g_a + s, n);
                    memmove(g_b + d, g_b + s, n);
                    CHECK(memcmp(g_a, g_b, sizeof g_a) == 0);
                }
        memcpy(g_a, g_seed, sizeof g_a);
        fill(g_b, sizeof g_b);
        mfc_copy(g_a + 0x5000, g_b + 0x400, n);
        CHECK(memcmp(g_a + 0x5000, g_b + 0x400, n) == 0);
        CHECK(memcmp(g_a, g_seed, 0x5000) == 0);
        CHECK(memcmp(g_a + 0x5000 + n, g_seed + 0x5000 + n, sizeof g_a - 0x5000 - n) == 0);
    }
}

static spu_context g_spu;
static uint8_t g_ls_ref[SPU_LS_SIZE], g_ls_seed[SPU_LS_SIZE];
static uint8_t g_mem_ref[sizeof g_guest_mem], g_mem_seed[sizeof g_guest_mem];

static void issue(uint32_t cmd, uint32_t lsa, uint32_t ea, uint32_t n, uint32_t tag)
{
    spu_wrch(&g_spu, MFC_LSA, spu_splat_u32(lsa));
    spu_wrch(&g_spu, MFC_EAH, spu_splat_u32(0));
    spu_wrch(&g_spu, MFC_EAL, spu_splat_u32(ea));
    spu_wrch(&g_spu, MFC_Size, spu_splat_u32(n));
    spu_wrch(&g_spu, MFC_TagID, spu_splat_u32(tag));
    spu_wrch(&g_spu, MFC_Cmd, spu_splat_u32(cmd));
    spu_wrch(&g_spu, MFC_WrTagMask, spu_splat_u32(1u << tag));
    spu_wrch(&g_spu, MFC_WrTagUpdate, spu_splat_u32(2));
    CHECK(spu_rdch(&g_spu, MFC_RdTagStat)._u32[0] == (1u << tag));
}

static void snapshot(void)
{
    memcpy(g_spu.ls, g_ls_seed, SPU_LS_SIZE);
    memcpy(g_guest_mem, g_mem_seed, sizeof g_guest_mem);
    memcpy(g_ls_ref, g_spu.ls, SPU_LS_SIZE);
    memcpy(g_mem_ref, g_guest_mem, sizeof g_guest_mem);
}

static void verify(void)
{
    CHECK(memcmp(g_spu.ls, g_ls_ref, SPU_LS_SIZE) == 0);
    CHECK(memcmp(g_guest_mem, g_mem_ref, sizeof g_guest_mem) == 0);
}

static void test_get_put_sizes_alignments_and_ls_wrap(void)
{
    for (unsigned k = 0; k < sizeof k_sizes / sizeof *k_sizes; k++) {
        const uint32_t n = k_sizes[k], step = n < 16 ? n : 16;
        const uint32_t lsas[] = { 0x1000u, SPU_LS_SIZE - 128u + step - n, 0x40000u | 0x2000u, 0xFFFC0000u | 0x3000u };
        for (unsigned l = 0; l < sizeof lsas / sizeof *lsas; l++)
            for (uint32_t off = 0; off < 128; off += step) {
                const uint32_t lsa = lsas[l] + off, lsa_eff = lsa & SPU_LS_MASK, ea = 0x20000u + off;
                snapshot();
                issue(MFC_GET_CMD, lsa, ea, n, 3);
                memcpy(g_ls_ref + lsa_eff, g_mem_ref + ea, n);
                verify();
                snapshot();
                issue(MFC_PUT_CMD, lsa, ea + 0x40000u, n, 5);
                memcpy(g_mem_ref + ea + 0x40000u, g_ls_ref + lsa_eff, n);
                verify();
            }
    }
}

static void put_be32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static void test_list_get_wraps_element_list_and_packs_destinations(void)
{
    const uint32_t sizes[] = { 16, 48, 128, 4, 96, 8192, 32 };
    const uint32_t eals[] = { 0x30000u, 0x30100u, 0x30200u, 0x30304u, 0x30400u, 0x32000u, 0x30510u };
    const unsigned count = sizeof sizes / sizeof *sizes;
    const uint32_t list = SPU_LS_SIZE - 24u, dest = 0x10000u;
    snapshot();
    for (unsigned i = 0; i < count; i++) {
        uint8_t* e = g_spu.ls + ((list + i * 8u) & SPU_LS_MASK);
        put_be32(e, sizes[i]);
        put_be32(e + 4, eals[i]);
    }
    memcpy(g_ls_ref, g_spu.ls, SPU_LS_SIZE);
    issue(MFC_GETL_CMD, dest, list, count * 8u, 7);
    uint32_t d = dest;
    for (unsigned i = 0; i < count; i++) {
        const uint32_t at = (d & ~15u) | (eals[i] & 15u);
        memcpy(g_ls_ref + at, g_mem_ref + eals[i], sizes[i]);
        d = (d & ~15u) + ((sizes[i] + 15u) & ~15u);
    }
    verify();
}

static void test_put_across_peer_reservation(void)
{
    static spu_context peer;
    spu_context_init(&peer, 1);
    peer.image_id = -1;
    const uint32_t line = 0x50000u;
    const uint32_t spans[][2] = { { 0x60, 64 }, { 0x50, 48 }, { 0x00, 128 }, { 0x70, 112 }, { 0x40, 16384 } };
    for (unsigned s = 0; s < sizeof spans / sizeof *spans; s++) {
        snapshot();
        peer.event_status = 0;
        spu_lockline_lock(line);
        spu_coh_reserve(&peer, line);
        memcpy(peer.resv_line, g_guest_mem + line, 128);
        peer.resv = line | 1u;
        spu_lockline_unlock(line);
        issue(MFC_PUT_CMD, 0x4000u + spans[s][0], line + spans[s][0], spans[s][1], 9);
        memcpy(g_mem_ref + line + spans[s][0], g_ls_ref + 0x4000u + spans[s][0], spans[s][1]);
        verify();
        CHECK((peer.resv & 1u) == 0);
        CHECK((peer.event_status & SPU_EVENT_LR) != 0);
    }
}

int main(void)
{
    spu_context_init(&g_spu, 0);
    g_spu.image_id = -1;
    fill(g_ls_seed, SPU_LS_SIZE);
    fill(g_mem_seed, sizeof g_mem_seed);
    test_copy_matches_memmove();
    test_get_put_sizes_alignments_and_ls_wrap();
    test_list_get_wraps_element_list_and_packs_destinations();
    test_put_across_peer_reservation();
    printf("SPU DMA copy: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
