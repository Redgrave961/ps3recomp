#include <limits.h>
#include <stdio.h>
#include "ps3emu/log_first.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static int logged_from_site(void)
{
    static int _n = 0;
    return PS3_LOG_FIRST(_n, 3);
}

int main(void)
{
    int printed = 0;
    for (int i = 0; i < 1000; i++) printed += logged_from_site();
    CHECK(printed == 3);

    int n = 0;
    for (int i = 0; i < 100; i++) (void)PS3_LOG_FIRST(n, 8);
    CHECK(n == 8);

    int hi = INT_MAX;
    CHECK(!PS3_LOG_FIRST(hi, 8));
    CHECK(hi == INT_MAX);

    int k = 0;
    CHECK(PS3_LOG_FIRST(k, 4) && k == 1);

    int per[2] = {0, 0}, limit = 2, hits = 0;
    for (int i = 0; i < 10; i++) hits += PS3_LOG_FIRST(per[1], limit);
    CHECK(hits == 2 && per[0] == 0 && per[1] == 2);

    int z = 0;
    CHECK(!PS3_LOG_FIRST(z, 0) && z == 0);

    int c = 0, other = 1, extra = 0;
    for (int i = 0; i < 20; i++) extra += (PS3_LOG_FIRST(c, 2) || other);
    CHECK(extra == 20 && c == 2);

    if (fails) return 1;
    printf("log_first: ok\n");
    return 0;
}
