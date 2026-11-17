/*
 * ps3recomp - sceNp HLE implementation
 *
 * Provides fake PSN identity so games can proceed through NP checks.
 */

#include "sceNp.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "../../runtime/ppu/ppu_memory.h"   /* vm_write*: guest EA -> host, byte-swapped */
#include "../system/cellSysutil.h"
#include "../filesystem/npdrm.h"


/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static int  s_np_initialized = 0;

/* Build a fake NP ID. */
static void np_build_fake_id(SceNpId* npId)
{
    memset(npId, 0, sizeof(SceNpId));
    strncpy(npId->handle.data, "PS3Player", SCE_NP_ONLINEID_MAX_LENGTH);
    npId->handle.term = '\0';
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 sceNpInit(u32 poolSize, void* poolPtr)
{
    (void)poolSize;
    (void)poolPtr;

    printf("[sceNp] Init(poolSize=%u, username=\"%s\")\n",
           poolSize, "PS3Player");

    if (s_np_initialized)
        return SCE_NP_ERROR_ALREADY_INITIALIZED;

    s_np_initialized = 1;
    return CELL_OK;
}

s32 sceNpTerm(void)
{
    printf("[sceNp] Term()\n");

    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    s_np_initialized = 0;
    return CELL_OK;
}

s32 sceNpGetNpId(s32 userId, SceNpId* npId)
{
    (void)userId;

    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!npId)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    /* A guest address, like every sibling getter's: writing through it
     * untranslated hit a host address and crashed Simpsons Arcade the first
     * time it asked for its NP ID online. */
    npId = GUEST_PTR(npId, SceNpId*);
    np_build_fake_id(npId);
    printf("[sceNp] GetNpId(user=%d) -> \"%s\"\n", userId, "PS3Player");
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * NP Manager (sign-in state)
 *
 * The toolkit runs with a fake local profile but no live PSN connection. So the
 * manager reports OFFLINE — games gate their online flows on GetStatus and skip
 * them cleanly — while the identity getters still hand back the fake account,
 * matching how NP behaves on a real signed-in-but-disconnected console.
 * Imported by most online-capable titles.
 * -----------------------------------------------------------------------*/

s32 sceNpManagerGetStatus(s32* status)
{
    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;
    if (!status)
        return SCE_NP_ERROR_INVALID_ARGUMENT;
    /* `status` is a GUEST address -- the HLE ABI adapter passes pointer
     * parameters straight through as guest values, so dereferencing one
     * writes to whatever host address shares that number. Same trap as
     * cellGcmSys had. Tokyo Jungle calls this during its online init. */
    vm_write32((uint32_t)(uintptr_t)status, (uint32_t)SCE_NP_MANAGER_STATUS_OFFLINE);
    { static int logged;
      if (!logged) printf("[sceNp] ManagerGetStatus() -> OFFLINE\n");
      logged = 1; }
    return CELL_OK;
}

/* Identity getters: reuse the fake-profile implementations (offline-with-account). */
s32 sceNpManagerGetNpId(SceNpId* npId)               { return sceNpGetNpId(0, npId); }

/* Parental controls: an unrestricted adult account. Unimplemented, these
 * returned OK without writing, and the title read whatever was in its stack. */
s32 sceNpManagerGetContentRatingFlag(s32* isRestricted, s32* age)
{
    if (!s_np_initialized) return SCE_NP_ERROR_NOT_INITIALIZED;
    if (isRestricted) vm_write32((u32)(uintptr_t)isRestricted, 0);
    if (age) vm_write32((u32)(uintptr_t)age, 25);
    return CELL_OK;
}

/* Tickets, the account region and the network time come from the PSN
 * servers. Unimplemented, these imports answered CELL_OK with nothing
 * written: a title that requested a ticket waited for a GOT_TICKET event
 * that never came, and one that read the region or the time used whatever
 * was in its buffer. Checks and offline answers as RPCS3's sceNp.cpp: the
 * manager is OFFLINE and so are these. */

s32 sceNpManagerGetAccountRegion(u32 countryCode, u32 language)
{
    if (!s_np_initialized) return SCE_NP_ERROR_NOT_INITIALIZED;
    if (!countryCode || !language) return SCE_NP_ERROR_INVALID_ARGUMENT;
    return SCE_NP_ERROR_OFFLINE;
}

void sys_fs_translate_path(const char* ps3_path, char* host_path, int host_path_size);

static s32 np_drm_is_available(const u8* k_licensee, const char* drm_path)
{
    const char* path = GUEST_PTR(drm_path, const char*);
    if (!path) return (s32)NPDRM_ERROR_INVALID_PARAM;
    char host[1024];
    sys_fs_translate_path(path, host, (int)sizeof host);
    return (s32)npdrm_license_available(host, GUEST_PTR(k_licensee, const u8*));
}

s32 sceNpDrmIsAvailable2(const u8* k_licensee, const char* drm_path)
{
    return np_drm_is_available(k_licensee, drm_path);
}
