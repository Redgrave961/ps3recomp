#include "ps3emu/milestone.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MS_CAP      2048u
#define MS_KEY_MAX    96

typedef struct {
    int          state;
    unsigned     ord;
    unsigned     hits;
    char         key[MS_KEY_MAX];
} ms_slot;

static ms_slot   g_slots[MS_CAP];
static int       g_lock;
static unsigned  g_next_ord;
static int       g_full_warned;

static int       g_enabled = -1;
static FILE*     g_out;
static int       g_dumped;

#define MS_CKPT_SECS  2
#define MS_TICK       1024u
static unsigned  g_calls;
static time_t    g_last_ckpt;
static void ms_checkpoint(void);
static void ms_maybe_counts_locked(void);

static void ms_lock(void)
{
    while (__atomic_exchange_n(&g_lock, 1, __ATOMIC_ACQUIRE)) {
    }
}

static void ms_unlock(void) { __atomic_store_n(&g_lock, 0, __ATOMIC_RELEASE); }

static unsigned ms_hash(const char* s)
{
    unsigned h = 2166136261u;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
    return h;
}

static int ms_enabled(void)
{
    int e = __atomic_load_n(&g_enabled, __ATOMIC_RELAXED);
    if (e >= 0) return e;

    ms_lock();
    e = __atomic_load_n(&g_enabled, __ATOMIC_RELAXED);
    if (e < 0) {
        const char* path = getenv("PS3_MILESTONE_OUT");
        e = 0;
        if (path && *path) {
            g_out = fopen(path, "w");
            if (g_out) {
                fprintf(g_out, "# ps3recomp milestone log v1\n");
                fflush(g_out);
                atexit(ps3_ms_dump);
                e = 1;
            } else {
                fprintf(stderr, "[milestone] cannot open %s -- log disabled\n", path);
            }
        }
        __atomic_store_n(&g_enabled, e, __ATOMIC_RELAXED);
    }
    ms_unlock();
    return e;
}

static void ms_insert(const char* key, unsigned h)
{
    unsigned i;
    ms_lock();

    for (i = 0; i < MS_CAP; i++) {
        ms_slot* s = &g_slots[(h + i) & (MS_CAP - 1)];
        if (s->state == 0) {
            snprintf(s->key, sizeof s->key, "%s", key);
            s->ord  = ++g_next_ord;
            s->hits = 1;
            __atomic_store_n(&s->state, 2, __ATOMIC_RELEASE);
            if (g_out) {
                fprintf(g_out, "%u\t%s\n", s->ord, s->key);
                fflush(g_out);
                ms_maybe_counts_locked();
            }
            ms_unlock();
            return;
        }
        if (s->state == 2 && strcmp(s->key, key) == 0) {
            __atomic_fetch_add(&s->hits, 1u, __ATOMIC_RELAXED);
            ms_unlock();
            return;
        }
    }

    if (!g_full_warned) {
        g_full_warned = 1;
        fprintf(stderr, "[milestone] table full at %u keys -- dropping this key "
                        "and later ones; raise MS_CAP\n", MS_CAP);
    }
    ms_unlock();
}

void ps3_ms(const char* key)
{
    unsigned h, i;
    if (!key || !*key || !ms_enabled()) return;

    if ((__atomic_add_fetch(&g_calls, 1u, __ATOMIC_RELAXED) & (MS_TICK - 1)) == 0)
        ms_checkpoint();

    h = ms_hash(key);
    for (i = 0; i < MS_CAP; i++) {
        ms_slot* s = &g_slots[(h + i) & (MS_CAP - 1)];
        int st = __atomic_load_n(&s->state, __ATOMIC_ACQUIRE);
        if (st == 0) break;
        if (st == 2 && strcmp(s->key, key) == 0) {
            __atomic_fetch_add(&s->hits, 1u, __ATOMIC_RELAXED);
            return;
        }
    }
    ms_insert(key, h);
}

void ps3_msf(const char* fmt, ...)
{
    char buf[MS_KEY_MAX];
    va_list ap;
    if (!ms_enabled()) return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ps3_ms(buf);
}

void ps3_ms_kv(const char* key, long long value)
{
    if (!key || !*key || !ms_enabled()) return;

    ms_lock();
    if (g_out) { fprintf(g_out, "kv\t%s\t%lld\n", key, value); fflush(g_out); }
    ms_unlock();
}

static const char* ms_bucket(unsigned n)
{
    if (n == 0)      return "0";
    if (n == 1)      return "1";
    if (n < 10)      return "2-9";
    if (n < 100)     return "10-99";
    if (n < 1000)    return "100-999";
    if (n < 10000)   return "1k-9k";
    if (n < 100000)  return "10k-99k";
    return "100k+";
}

static void ms_write_counts_locked(void)
{
    static unsigned short by_ord[MS_CAP + 1];
    unsigned i, ord;

    memset(by_ord, 0, sizeof by_ord);
    for (i = 0; i < MS_CAP; i++)
        if (g_slots[i].state == 2 && g_slots[i].ord <= MS_CAP)
            by_ord[g_slots[i].ord] = (unsigned short)i;

    fprintf(g_out, "# counts\n");
    for (ord = 1; ord <= g_next_ord && ord <= MS_CAP; ord++) {
        const ms_slot* s = &g_slots[by_ord[ord]];
        if (s->ord == ord)
            fprintf(g_out, "count\t%s\t%s\n", s->key, ms_bucket(s->hits));
    }
    fflush(g_out);
}

static void ms_maybe_counts_locked(void)
{
    time_t now;
    if (!g_out || g_dumped) return;
    now = time(NULL);
    if (g_last_ckpt && (long)(now - g_last_ckpt) < MS_CKPT_SECS) return;
    g_last_ckpt = now;
    ms_write_counts_locked();
}

static void ms_checkpoint(void)
{
    ms_lock();
    ms_maybe_counts_locked();
    ms_unlock();
}

void ps3_ms_dump(void)
{
    if (ms_enabled() != 1) return;

    ms_lock();
    if (g_dumped || !g_out) { ms_unlock(); return; }
    g_dumped = 1;

    ms_write_counts_locked();
    fprintf(g_out, "# end\n");
    fflush(g_out);
    fclose(g_out);
    g_out = NULL;
    ms_unlock();
}
