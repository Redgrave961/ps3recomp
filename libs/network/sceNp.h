#ifndef PS3RECOMP_SCE_NP_H
#define PS3RECOMP_SCE_NP_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SCE_NP_ERROR_NOT_INITIALIZED            0x8002aa01
#define SCE_NP_ERROR_ALREADY_INITIALIZED        0x8002aa02
#define SCE_NP_ERROR_INVALID_ARGUMENT           0x8002aa03
#define SCE_NP_ERROR_OUT_OF_MEMORY              0x8002aa04
#define SCE_NP_ERROR_OFFLINE                    0x8002aa0c

#define SCE_NP_ONLINEID_MAX_LENGTH              16
#define SCE_NP_COMMUNICATION_ID_MAX_LENGTH      9
#define SCE_NP_COMMUNICATION_SIGNATURE_SIZE     160

#define SCE_NP_LANG_ENGLISH                     1

typedef struct SceNpCommunicationId {
    char data[SCE_NP_COMMUNICATION_ID_MAX_LENGTH];
    char term;
    u8   num;
    u8   padding[3];
} SceNpCommunicationId;

typedef struct SceNpCommunicationSignature {
    u8 data[SCE_NP_COMMUNICATION_SIGNATURE_SIZE];
} SceNpCommunicationSignature;

typedef struct SceNpOnlineId {
    char data[SCE_NP_ONLINEID_MAX_LENGTH];
    char term;
    char padding[3];
} SceNpOnlineId;

typedef struct SceNpId {
    SceNpOnlineId handle;
    u8            opt[8];
    u8            reserved[8];
} SceNpId;

s32 sceNpInit(u32 poolSize, void* poolPtr);
s32 sceNpTerm(void);

s32 sceNpGetNpId(s32 userId, SceNpId* npId);

#define SCE_NP_MANAGER_STATUS_OFFLINE          (-1)

s32 sceNpManagerGetStatus(s32* status);
s32 sceNpManagerGetNpId(SceNpId* npId);

s32 sceNpManagerGetAccountRegion(u32 countryCode, u32 language);

s32 sceNpDrmIsAvailable2(const u8* k_licensee, const char* drm_path);

#ifdef __cplusplus
}
#endif

#endif
