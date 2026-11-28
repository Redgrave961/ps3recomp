#include "sceNp.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "../../runtime/ppu/ppu_memory.h"
#include "../system/cellSysutil.h"
#include "../filesystem/npdrm.h"

static int  s_np_initialized = 0;

static void np_build_fake_id(SceNpId* npId)
{
    memset(npId, 0, sizeof(SceNpId));
    strncpy(npId->handle.data, "PS3Player", SCE_NP_ONLINEID_MAX_LENGTH);
    npId->handle.term = '\0';
}

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

    npId = GUEST_PTR(npId, SceNpId*);
    np_build_fake_id(npId);
    printf("[sceNp] GetNpId(user=%d) -> \"%s\"\n", userId, "PS3Player");
    return CELL_OK;
}

s32 sceNpManagerGetStatus(s32* status)
{
    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;
    if (!status)
        return SCE_NP_ERROR_INVALID_ARGUMENT;
    vm_write32((uint32_t)(uintptr_t)status, (uint32_t)SCE_NP_MANAGER_STATUS_OFFLINE);
    { static int logged;
      if (!logged) printf("[sceNp] ManagerGetStatus() -> OFFLINE\n");
      logged = 1; }
    return CELL_OK;
}

s32 sceNpManagerGetNpId(SceNpId* npId)               { return sceNpGetNpId(0, npId); }

s32 sceNpManagerGetContentRatingFlag(s32* isRestricted, s32* age)
{
    if (!s_np_initialized) return SCE_NP_ERROR_NOT_INITIALIZED;
    if (isRestricted) vm_write32((u32)(uintptr_t)isRestricted, 0);
    if (age) vm_write32((u32)(uintptr_t)age, 25);
    return CELL_OK;
}

s32 sceNpManagerGetAccountRegion(u32 countryCode, u32 language)
{
    if (!s_np_initialized) return SCE_NP_ERROR_NOT_INITIALIZED;
    if (!countryCode || !language) return SCE_NP_ERROR_INVALID_ARGUMENT;
    return SCE_NP_ERROR_OFFLINE;
}

int sys_fs_translate_path(const char* ps3_path, char* host_path, int host_path_size);

static s32 np_drm_is_available(const u8* k_licensee, const char* drm_path)
{
    const char* path = GUEST_PTR(drm_path, const char*);
    if (!path) return (s32)NPDRM_ERROR_INVALID_PARAM;
    char host[1024];
    if (!sys_fs_translate_path(path, host, (int)sizeof host)) return (s32)NPDRM_ERROR_IO;
    return (s32)npdrm_license_available(host, GUEST_PTR(k_licensee, const u8*));
}

s32 sceNpDrmIsAvailable2(const u8* k_licensee, const char* drm_path)
{
    return np_drm_is_available(k_licensee, drm_path);
}
