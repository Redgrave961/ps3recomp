/*
 * ps3recomp - cellSsl HLE implementation
 *
 * Stub SSL module. Init/End lifecycle works, certificate functions
 * return empty data.
 */

#include "cellSsl.h"
#include "../../runtime/ppu/ppu_memory.h"   /* vm_write32 -- out-params are guest EAs */
#include <stdio.h>

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static int s_ssl_initialized = 0;

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellSslInit(void* pool, u32 poolSize)
{
    (void)pool;
    (void)poolSize;
    printf("[cellSsl] Init(poolSize=%u)\n", poolSize);

    if (s_ssl_initialized)
        return (s32)CELL_SSL_ERROR_ALREADY_INITIALIZED;

    s_ssl_initialized = 1;
    return CELL_OK;
}

s32 cellSslEnd(void)
{
    printf("[cellSsl] End()\n");

    if (!s_ssl_initialized)
        return (s32)CELL_SSL_ERROR_NOT_INITIALIZED;

    s_ssl_initialized = 0;
    return CELL_OK;
}

s32 cellSslCertificateLoader(u64 flags, char* buffer, u32 size, u32* required)
{
    (void)buffer; (void)size;
    printf("[cellSsl] CertificateLoader(flags=0x%llX)\n", (unsigned long long)flags);
    if (required)
        vm_write32((uint32_t)(uintptr_t)required, 0);  /* required is a guest EA -> write guest mem, not host deref */
    return CELL_OK;
}

