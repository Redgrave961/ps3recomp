#include "runtime/platform/win32_compat.h"
#include "runtime/platform/ps3_sync.h"
#include <stdio.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); __atomic_add_fetch(&failures, 1, __ATOMIC_RELAXED); } } while (0)

enum { ROUNDS = 100000, CHURNERS = 3, RACES = 1000 };

int32_t ppu_thread_priority(uint64_t tid) { (void)tid; return 0; }

static int failures;
static volatile uint32_t ping, pong, churn, stop_noise, finished, settled;

static void expect_timeout(volatile uint32_t* w, uint64_t us)
{
    const uint64_t start = ps3_sync_now_ns();
    CHECK(ps3_brief_wait(w, __atomic_load_n(w, __ATOMIC_ACQUIRE), us) == 0);
    CHECK(ps3_sync_now_ns() - start >= us * 1000ull);
}

static void await_value(volatile uint32_t* w, uint32_t want)
{
    for (uint32_t v; (v = __atomic_load_n(w, __ATOMIC_ACQUIRE)) != want;) CHECK(ps3_brief_wait(w, v, 2000000));
}

static void await_settled(void)
{
    while (!__atomic_load_n(&settled, __ATOMIC_ACQUIRE)) ps3_sleep_us(100);
}

static DWORD WINAPI player(LPVOID arg)
{
    const int first = arg != NULL;
    volatile uint32_t* mine = first ? &ping : &pong;
    volatile uint32_t* theirs = first ? &pong : &ping;
    for (uint32_t i = 1; i <= ROUNDS; i++) {
        if (first) {
            __atomic_store_n(mine, i, __ATOMIC_RELEASE);
            ps3_brief_wake(mine);
            await_value(theirs, i);
        } else {
            await_value(theirs, i);
            __atomic_store_n(mine, i, __ATOMIC_RELEASE);
            ps3_brief_wake(mine);
        }
    }
    __atomic_add_fetch(&finished, 1u, __ATOMIC_RELEASE);
    await_settled();
    expect_timeout(theirs, 3000);
    return 0;
}

static DWORD WINAPI churner(LPVOID arg)
{
    (void)arg;
    while (!__atomic_load_n(&stop_noise, __ATOMIC_ACQUIRE))
        ps3_brief_wait(&churn, __atomic_load_n(&churn, __ATOMIC_ACQUIRE), 30);
    await_settled();
    expect_timeout(&churn, 1000);
    return 0;
}

static DWORD WINAPI noise(LPVOID arg)
{
    (void)arg;
    while (!__atomic_load_n(&stop_noise, __ATOMIC_ACQUIRE)) {
        ps3_brief_wake(&ping);
        ps3_brief_wake(&pong);
        __atomic_add_fetch(&churn, 1u, __ATOMIC_RELEASE);
        ps3_brief_wake(&churn);
    }
    return 0;
}

static volatile uint32_t race_word, race_round, race_stop, race_acked;
static volatile uint64_t race_at;

static DWORD WINAPI racer(LPVOID arg)
{
    (void)arg;
    for (uint32_t i = 1; i <= RACES; i++) {
        while (__atomic_load_n(&race_round, __ATOMIC_ACQUIRE) != i) ps3_pause();
        while (ps3_sync_now_ns() < __atomic_load_n(&race_at, __ATOMIC_ACQUIRE)) ps3_pause();
        while (!__atomic_load_n(&race_stop, __ATOMIC_ACQUIRE)) ps3_brief_wake(&race_word);
        __atomic_store_n(&race_acked, i, __ATOMIC_RELEASE);
    }
    return 0;
}

static void race_timeouts(void)
{
    HANDLE t = CreateThread(NULL, 0, racer, NULL, 0, NULL);
    for (uint32_t i = 1; i <= RACES; i++) {
        __atomic_store_n(&race_stop, 0u, __ATOMIC_RELEASE);
        __atomic_store_n(&race_at, ps3_sync_now_ns() + (i * 7919u % 1200u) * 1000ull, __ATOMIC_RELEASE);
        __atomic_store_n(&race_round, i, __ATOMIC_RELEASE);
        ps3_brief_wait(&race_word, race_word, 30);
        __atomic_store_n(&race_stop, 1u, __ATOMIC_RELEASE);
        while (__atomic_load_n(&race_acked, __ATOMIC_ACQUIRE) != i) ps3_pause();
        expect_timeout(&race_word, 50);
    }
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}

int main(void)
{
    for (int i = 0; i < 50; i++) expect_timeout(&ping, 200);
    race_timeouts();

    HANDLE threads[3 + CHURNERS];
    threads[0] = CreateThread(NULL, 0, player, (LPVOID)1, 0, NULL);
    threads[1] = CreateThread(NULL, 0, player, NULL, 0, NULL);
    threads[2] = CreateThread(NULL, 0, noise, NULL, 0, NULL);
    for (int i = 0; i < CHURNERS; i++) threads[3 + i] = CreateThread(NULL, 0, churner, NULL, 0, NULL);

    while (__atomic_load_n(&finished, __ATOMIC_ACQUIRE) != 2u) ps3_sleep_us(1000);
    __atomic_store_n(&stop_noise, 1u, __ATOMIC_RELEASE);
    WaitForSingleObject(threads[2], INFINITE);
    __atomic_store_n(&settled, 1u, __ATOMIC_RELEASE);
    for (int i = 0; i < 3 + CHURNERS; i++) {
        if (i != 2) WaitForSingleObject(threads[i], INFINITE);
        CloseHandle(threads[i]);
    }

    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("brief_wait: OK\n");
    return 0;
}
