#include "cellHttps.h"
#include "../../runtime/ppu/ppu_memory.h"
#include <stdio.h>
#include <string.h>

static int s_https_initialized = 0;

s32 cellHttpsInit(u32 caCertNum, const CellHttpsData* caList)
{
    (void)caCertNum; (void)caList;
    printf("[cellHttps] Init(caCertNum=%u)\n", caCertNum);

    if (s_https_initialized)
        return (s32)CELL_HTTPS_ERROR_ALREADY_INITIALIZED;

    s_https_initialized = 1;
    return CELL_OK;
}

s32 cellHttpsEnd(void)
{
    printf("[cellHttps] End()\n");

    if (!s_https_initialized)
        return (s32)CELL_HTTPS_ERROR_NOT_INITIALIZED;

    s_https_initialized = 0;
    return CELL_OK;
}
