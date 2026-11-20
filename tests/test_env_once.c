#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_lookups[4];
static int g_total;

static const char* LONG_NAME =
    "PS3_ENV_ONCE_A_NAME_TOO_LONG_FOR_THE_TABLE_0123456789";

static char* counting_getenv(const char* name)
{
    g_total++;
    if (!strcmp(name, "PS3_ENV_ONCE_A")) g_lookups[0]++;
    else if (!strcmp(name, "PS3_ENV_ONCE_B")) g_lookups[1]++;
    else if (!strcmp(name, "PS3_ENV_ONCE_UNSET")) g_lookups[2]++;
    else if (!strcmp(name, LONG_NAME)) g_lookups[3]++;
    return getenv(name);
}

#define getenv counting_getenv
#include "ps3emu/ps3_env_once.h"
#undef getenv

#ifdef _WIN32
#define set_env(n, v) _putenv_s((n), (v))
#define unset_env(n)  _putenv_s((n), "")
#else
#define set_env(n, v) setenv((n), (v), 1)
#define unset_env(n)  unsetenv(n)
#endif

static int g_pass, g_fail;
#define CHECK(c) do { if (c) g_pass++; else { g_fail++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static const char* site_one(void) { return ps3_env_once("PS3_ENV_ONCE_A"); }
static const char* site_two(void) { return ps3_env_once("PS3_ENV_ONCE_A"); }

int main(void)
{
    set_env("PS3_ENV_ONCE_A", "first");
    set_env("PS3_ENV_ONCE_B", "7");
    unset_env("PS3_ENV_ONCE_UNSET");
    set_env(LONG_NAME, "long");

    const char* a1 = site_one();
    CHECK(a1 && !strcmp(a1, "first"));
    for (int i = 0; i < 1000; i++) CHECK(site_two() == a1);
    CHECK(g_lookups[0] == 1);

    char built[32];
    snprintf(built, sizeof built, "PS3_ENV_ONCE_%c", 'A');
    CHECK(ps3_env_once(built) == a1);
    CHECK(g_lookups[0] == 1);

    set_env("PS3_ENV_ONCE_A", "second");
    CHECK(site_one() == a1);
    CHECK(g_lookups[0] == 1);

    const char* b = ps3_env_once("PS3_ENV_ONCE_B");
    CHECK(b && atoi(b) == 7);
    CHECK(ps3_env_once("PS3_ENV_ONCE_B") == b);
    CHECK(g_lookups[1] == 1);

    for (int i = 0; i < 100; i++) CHECK(ps3_env_once("PS3_ENV_ONCE_UNSET") == NULL);
    CHECK(g_lookups[2] == 1);

    for (int i = 0; i < 3; i++) {
        const char* l = ps3_env_once(LONG_NAME);
        CHECK(l && !strcmp(l, "long"));
    }
    CHECK(g_lookups[3] == 3);

    char name[32];
    for (int i = 0; i < PS3_ENV_ONCE_SLOTS + 8; i++) {
        snprintf(name, sizeof name, "PS3_ENV_ONCE_FILL_%d", i);
        CHECK(ps3_env_once(name) == NULL);
    }
    CHECK(ps3_env_once("PS3_ENV_ONCE_B") == b);
    CHECK(ps3_env_once(NULL) == NULL);

    printf("ps3_env_once: %d getenv calls in all; %d passed, %d failed\n", g_total, g_pass, g_fail);
    return g_fail ? 1 : 0;
}
