#define PS3_INTR_MAX 8

typedef struct {
    uint32_t tag;
    uint64_t thread;
    int       used;
} ps3_intr_bind;

static ps3_intr_bind s_intr[PS3_INTR_MAX];
static volatile uint32_t s_intr_seq;

static void ps3_intr_signal(void)
{
    __atomic_add_fetch(&s_intr_seq, 1u, __ATOMIC_RELEASE);
    ps3_futex_wake_all(&s_intr_seq);
}

int ps3_intr_establish(uint32_t tag, uint64_t thread)
{
    for (int i = 0; i < PS3_INTR_MAX; i++) {
        if (s_intr[i].used && s_intr[i].tag == tag) {
            s_intr[i].thread = thread;
            return 0;
        }
    }
    for (int i = 0; i < PS3_INTR_MAX; i++) {
        if (!s_intr[i].used) {
            s_intr[i].used = 1;
            s_intr[i].tag = tag;
            s_intr[i].thread = thread;
            fprintf(stderr, "[intr] tag 0x%08X -> ppu thread %llu\n",
                    tag, (unsigned long long)thread);
            return 0;
        }
    }
    return -1;
}

int ps3_intr_is_handler(uint64_t thread)
{
    for (int i = 0; i < PS3_INTR_MAX; i++)
        if (s_intr[i].used && s_intr[i].thread == thread) return 1;
    return 0;
}

static void ps3_intr_raise(uint32_t tag)
{
    (void)tag;
    ps3_intr_signal();
}

static int ps3_intr_level(uint32_t tag)
{
    unsigned n = (tag >> 8) & 0xFFu;
    if (n >= SPU_RAW_COUNT) return 0;
    if (!s_spu[n].used) return 0;
    return (s_spu[n].int_stat & s_spu[n].int_mask) != 0;
}

int ps3_intr_wait(uint64_t thread)
{
    ps3_intr_bind* b = 0;
    for (int i = 0; i < PS3_INTR_MAX; i++)
        if (s_intr[i].used && s_intr[i].thread == thread) { b = &s_intr[i]; break; }
    if (!b) return 0;
    for (;;) {
        const uint32_t seq = __atomic_load_n(&s_intr_seq, __ATOMIC_ACQUIRE);
        if (ps3_intr_level(b->tag)) return 1;
        ps3_futex_wait(&s_intr_seq, seq, 0);
    }
}

static int64_t sc_interrupt_thread_establish(ppu_context* ctx)
{
    uint32_t ih_ea = (uint32_t)ctx->gpr[3];
    uint32_t tag   = (uint32_t)ctx->gpr[4];
    uint64_t thr   = ctx->gpr[5];
    if (ps3_intr_establish(tag, thr) != 0) return CELL_EAGAIN;
    if (ih_ea) { be32_store(ih_ea, 0); be32_store(ih_ea + 4, tag); }
    return CELL_OK;
}

static int64_t sc_interrupt_thread_disestablish(ppu_context* ctx)
{
    uint32_t tag    = (uint32_t)ctx->gpr[3];
    uint32_t out_ea = (uint32_t)ctx->gpr[4];
    for (int i = 0; i < PS3_INTR_MAX; i++)
        if (s_intr[i].used && s_intr[i].tag == tag) { s_intr[i].used = 0; break; }
    if (out_ea) { be32_store(out_ea, 0); be32_store(out_ea + 4, 0); }
    return CELL_OK;
}

static int64_t sc_interrupt_thread_eoi(ppu_context* ctx)
{
    (void)ctx;
    return CELL_OK;
}
