#ifndef PS3EMU_PS3_FTRACE_H
#define PS3EMU_PS3_FTRACE_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#  include <windows.h>
#else
#  include <time.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum ps3_ft_kind {
    PS3_FT_SYSCALL     = 1,
    PS3_FT_USLEEP      = 2,
    PS3_FT_SPU_WAIT    = 3,
    PS3_FT_SPU_BUSY    = 4,
    PS3_FT_HLE         = 15,
};

#if defined(_WIN32)
#  define PS3_FT_SHARED __declspec(selectany)
#else
#  define PS3_FT_SHARED __attribute__((weak))
#endif

typedef struct { uint64_t t0, t1; uint32_t a, b, who, kind; } ps3_ft_rec;

enum { PS3_FT_TIDS = 256 };

typedef struct {
    int on;
    int dumped;
    uint32_t n, cap, dropped;
    uint64_t base, start, end, min, merge;
    ps3_ft_rec* buf;
    char path[1024];
    struct { uint64_t t0, t1; uint32_t lr, n; } us[PS3_FT_TIDS];
    char names[PS3_FT_TIDS][28];
} ps3_ft_state;

#ifdef __cplusplus
PS3_FT_SHARED ps3_ft_state g_ps3_ft = {};
#else
PS3_FT_SHARED ps3_ft_state g_ps3_ft = { 0 };
#endif

static inline uint64_t ps3_ft_now(void)
{
#ifdef _WIN32
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f);
    return (uint64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

static inline void ps3_ft_dump(void)
{
    ps3_ft_state* s = &g_ps3_ft;
    if (__atomic_exchange_n(&s->dumped, 1, __ATOMIC_ACQ_REL)) return;
    __atomic_store_n(&s->on, 3, __ATOMIC_RELEASE);
    for (uint32_t t = 0; t < PS3_FT_TIDS; t++) {
        if (!s->us[t].n) continue;
        const uint32_t i = __atomic_fetch_add(&s->n, 1, __ATOMIC_RELAXED);
        if (i >= s->cap) break;
        ps3_ft_rec r = { s->us[t].t0, s->us[t].t1, s->us[t].n, s->us[t].lr, t, PS3_FT_USLEEP };
        s->buf[i] = r;
    }
    uint32_t n = __atomic_load_n(&s->n, __ATOMIC_ACQUIRE);
    if (n > s->cap) n = s->cap;
    FILE* f = fopen(s->path, "w");
    if (!f) { fprintf(stderr, "[ftrace] cannot write %s\n", s->path); return; }
    fprintf(f, "# ps3_ftrace start_us=%.3f end_us=%.3f spans=%u dropped=%u\n",
            (double)(s->start - s->base) / 1e3, (double)(s->end - s->base) / 1e3, n, s->dropped);
    for (uint32_t t = 0; t < PS3_FT_TIDS; t++)
        if (s->names[t][0]) fprintf(f, "T %u %s\n", t, s->names[t]);
    for (uint32_t i = 0; i < n; i++) {
        const ps3_ft_rec* r = &s->buf[i];
        if (!r->kind) continue;
        fprintf(f, "%u %u %x %x %.3f %.3f\n", r->kind, r->who, r->a, r->b,
                (double)(r->t0 - s->base) / 1e3, (double)(r->t1 - s->base) / 1e3);
    }
    fclose(f);
    fprintf(stderr, "[ftrace] wrote %u spans (%u dropped) to %s\n", n, s->dropped, s->path);
}

static inline void ps3_ft_dump_atexit(void) { ps3_ft_dump(); }

static inline int ps3_ft_init(void)
{
    ps3_ft_state* s = &g_ps3_ft;
    int expect = 0;
    if (!__atomic_compare_exchange_n(&s->on, &expect, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return expect == 2;
    int on = 3;
    const char* e = getenv("PS3_FTRACE");
    double start = 0, secs = 0;
    int used = 0;
    if (e && *e) {
        if (sscanf(e, "%lf:%lf:%n", &start, &secs, &used) < 2 || !used || secs <= 0) {
            fprintf(stderr, "[ftrace] PS3_FTRACE=<start s>:<seconds>:<path>, got '%s'\n", e);
        } else {
            snprintf(s->path, sizeof s->path, "%s", e + used);
            const char* m = getenv("PS3_FTRACE_MIN_US");
            s->min = (m ? strtoull(m, 0, 10) : 20ull) * 1000ull;
            const char* g = getenv("PS3_FTRACE_MERGE_US");
            s->merge = (g ? strtoull(g, 0, 10) : 300ull) * 1000ull;
            const char* c = getenv("PS3_FTRACE_CAP");
            s->cap = c ? (uint32_t)strtoul(c, 0, 10) : 4u << 20;
            s->buf = (ps3_ft_rec*)calloc(s->cap, sizeof(ps3_ft_rec));
            if (!s->buf) {
                fprintf(stderr, "[ftrace] no memory for %u spans\n", s->cap);
            } else {
                s->base = ps3_ft_now();
                s->start = s->base + (uint64_t)(start * 1e9);
                s->end = s->start + (uint64_t)(secs * 1e9);
                atexit(ps3_ft_dump_atexit);
                fprintf(stderr, "[ftrace] recording %.1f s from t=%.1f s into %s\n", secs, start, s->path);
                on = 2;
            }
        }
    }
    __atomic_store_n(&s->on, on, __ATOMIC_RELEASE);
    return on == 2;
}

static inline int ps3_ft_on(void)
{
    const int on = __atomic_load_n(&g_ps3_ft.on, __ATOMIC_RELAXED);
    if (__builtin_expect(on == 2, 0)) return 1;
    if (__builtin_expect(on != 0, 1)) return 0;
    return ps3_ft_init();
}
#define PS3_FT_ON() ps3_ft_on()

static inline uint64_t ps3_ft_min_ns(void) { return g_ps3_ft.min; }

static inline void ps3_ft_span(int kind, uint32_t who, uint32_t a, uint32_t b,
                               uint64_t t0, uint64_t t1)
{
    ps3_ft_state* s = &g_ps3_ft;
    if (__atomic_load_n(&s->on, __ATOMIC_RELAXED) != 2) return;
    if (t1 < s->start) return;
    if (t0 > s->end) { ps3_ft_dump(); return; }
    const uint32_t i = __atomic_fetch_add(&s->n, 1, __ATOMIC_RELAXED);
    if (i >= s->cap) { __atomic_fetch_add(&s->dropped, 1, __ATOMIC_RELAXED); return; }
    ps3_ft_rec r = { t0, t1, a, b, who, (uint32_t)kind };
    s->buf[i] = r;
}

static inline void ps3_ft_usleep(uint32_t tid, uint32_t lr, uint64_t t0, uint64_t t1)
{
    ps3_ft_state* s = &g_ps3_ft;
    if (tid >= PS3_FT_TIDS || __atomic_load_n(&s->on, __ATOMIC_RELAXED) != 2) return;
    if (s->us[tid].n && s->us[tid].lr == lr && t0 - s->us[tid].t1 <= s->merge) {
        s->us[tid].t1 = t1; s->us[tid].n++;
        return;
    }
    if (s->us[tid].n)
        ps3_ft_span(PS3_FT_USLEEP, tid, s->us[tid].n, s->us[tid].lr, s->us[tid].t0, s->us[tid].t1);
    s->us[tid].t0 = t0; s->us[tid].t1 = t1; s->us[tid].lr = lr; s->us[tid].n = 1;
}

static inline void ps3_ft_name(uint32_t tid, const char* name)
{
    if (tid >= PS3_FT_TIDS || !name || !PS3_FT_ON()) return;
    snprintf(g_ps3_ft.names[tid], sizeof g_ps3_ft.names[tid], "%s", name);
}

#ifdef __cplusplus
}
#endif

#endif
