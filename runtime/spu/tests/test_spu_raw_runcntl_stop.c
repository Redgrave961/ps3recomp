#include "../spu_raw.c"

#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>

u128 spu_rdch(spu_context* ctx, uint32_t channel);

uint8_t* vm_base;
uint32_t ppu_vm_size;
PPU_THREAD_LOCAL ppu_context* g_active_ctx;
void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*))
{ (void)nid; (void)name; (void)fn; }

static uint8_t s_prog[2][0x400];
static volatile int s_parked, s_left_wait, s_ran_b;
static void prog_a(spu_context* c)
{
    __atomic_store_n(&s_parked, 1, __ATOMIC_RELEASE);
    spu_rdch(c, SPU_RdInMbox);
    __atomic_store_n(&s_left_wait, 1, __ATOMIC_RELEASE);
    c->stop_code = 0x2000;
}
static void prog_b(spu_context* c) { s_ran_b++; c->stop_code = 0x3000; }
static int g_fails;
#define CHECK(c, ...) do { if (!(c)) { g_fails++; fprintf(stderr, "FAIL line %d: %s: ", __LINE__, #c); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

#define G_PROG 0x00100000u

static void w32(uint32_t ea, uint32_t v) { be32_store(ea, v); spu_raw_reg_store(ea, v, 4); }
static void nap(void) { struct timespec ts = { 0, 1000000 }; nanosleep(&ts, NULL); }

static uint32_t create(void)
{
    ppu_context ctx; memset(&ctx, 0, sizeof ctx);
    ctx.gpr[3] = 0x1000;
    CHECK(sc_raw_spu_create(&ctx) == CELL_OK, "create");
    return be32_load(0x1000);
}

static void proxy_load(uint32_t n, int p)
{
    const uint32_t base = SPU_RAW_BASE + n * SPU_RAW_STRIDE;
    for (uint32_t o = 0; o < sizeof s_prog[p]; o += 0x100) {
        w32(base + SPU_RAW_MFC_LSA, o);
        w32(base + SPU_RAW_MFC_EAH, 0);
        w32(base + SPU_RAW_MFC_EAL, G_PROG + (uint32_t)p * 0x1000u + o);
        w32(base + SPU_RAW_MFC_SIZE_TAG, 0x100u << 16);
        w32(base + SPU_RAW_MFC_CLASS_CMD, 0x40u);
    }
}

static int started(uint32_t n) { return (int)__atomic_load_n(&s_spu[n].started, __ATOMIC_ACQUIRE); }

int main(void)
{
    void* p = mmap(NULL, 0x100010000ull, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); return 1; }
    vm_base = (uint8_t*)p;
    for (int i = 0; i < 2; i++) {
        for (size_t o = 0; o < sizeof s_prog[i]; o++) s_prog[i][o] = (uint8_t)(o * (i + 3) + 1);
        memcpy(vm_base + G_PROG + (uint32_t)i * 0x1000u, s_prog[i], sizeof s_prog[i]);
    }
    spu_workload_register_img(spu_workload_fingerprint(s_prog[0], sizeof s_prog[0]),
                              prog_a, 1, "A");
    spu_workload_register_img(spu_workload_fingerprint(s_prog[1], sizeof s_prog[1]),
                              prog_b, 2, "B");

    uint32_t n = create();
    const uint32_t base = SPU_RAW_BASE + n * SPU_RAW_STRIDE;

    proxy_load(n, 0);
    w32(base + SPU_RAW_NPC, 0);
    w32(base + SPU_RAW_RUNCNTL, RUNCNTL_RUN);
    for (int i = 0; i < 2000 && !__atomic_load_n(&s_parked, __ATOMIC_ACQUIRE); i++) nap();
    for (int i = 0; i < 50; i++) nap();
    CHECK(s_parked && started(n), "A did not start (parked=%d started=%d)", s_parked, started(n));

    const uint32_t int_before = s_spu[n].int_stat;
    w32(base + SPU_RAW_RUNCNTL, 0);
    CHECK(!started(n), "the host thread is still running after RunCntl = 0");
    CHECK(!s_left_wait, "A returned from its wait instead of being stopped");
    uint32_t st = be32_load(base + SPU_RAW_STATUS);
    CHECK(st == SPU_STATUS_STOPPED, "status 0x%08X, want a plain stop", st);
    CHECK(s_spu[n].int_stat == int_before, "a stop by RunCntl raised a stop-and-signal interrupt");

    proxy_load(n, 1);
    w32(base + SPU_RAW_NPC, 0);
    w32(base + SPU_RAW_RUNCNTL, RUNCNTL_RUN);
    for (int i = 0; i < 2000; i++) {
        if (s_ran_b && !started(n)) break;
        nap();
    }
    CHECK(s_ran_b == 1, "B ran %d times", s_ran_b);
    st = be32_load(base + SPU_RAW_STATUS);
    CHECK(st == (SPU_STATUS_STOPPED_BY_STOP | (0x3000u << 16)), "status after B 0x%08X", st);
    CHECK(!s_left_wait, "A's old run resumed");

    if (g_fails) { fprintf(stderr, "%d check(s) failed\n", g_fails); return 1; }
    puts("PASS: SPU_RunCntl = 0 stops a raw SPU parked in a channel wait, and the next start runs the new program");
    return 0;
}

unsigned long long ps3_ms_now(void) { return 0; }
void spu_thread_publish_ctx(uint32_t tid, void* c) { (void)tid; (void)c; abort(); }
void spurs_ef_set_from_spu(uint32_t ea, uint16_t bits) { (void)ea; (void)bits; abort(); }
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
