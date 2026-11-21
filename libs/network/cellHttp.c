#include "cellHttp.h"
#include <stdio.h>

#ifdef _WIN32
#include <winsock2.h>
#endif

static int s_http_initialized = 0;
#ifdef _WIN32
static int s_wsa_initialized = 0;
#endif

s32 cellHttpInit(void* pool, u32 poolSize)
{
    printf("[cellHttp] Init(pool=%p, poolSize=%u)\n", pool, poolSize);

    if (s_http_initialized)
        return CELL_HTTP_ERROR_ALREADY_INITIALIZED;

#ifdef _WIN32
    if (!s_wsa_initialized) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            printf("[cellHttp] WSAStartup failed\n");
            return CELL_HTTP_ERROR_UNKNOWN;
        }
        s_wsa_initialized = 1;
    }
#endif

    s_http_initialized = 1;
    return CELL_OK;
}

s32 cellHttpEnd(void)
{
    printf("[cellHttp] End()\n");

    if (!s_http_initialized)
        return CELL_HTTP_ERROR_NOT_INITIALIZED;

    s_http_initialized = 0;

#ifdef _WIN32
    if (s_wsa_initialized) {
        WSACleanup();
        s_wsa_initialized = 0;
    }
#endif

    return CELL_OK;
}
