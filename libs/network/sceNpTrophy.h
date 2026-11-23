#ifndef PS3RECOMP_SCE_NP_TROPHY_H
#define PS3RECOMP_SCE_NP_TROPHY_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"
#include "sceNp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SCE_NP_TROPHY_ERROR_ALREADY_INITIALIZED          0x80022901
#define SCE_NP_TROPHY_ERROR_NOT_INITIALIZED              0x80022902
#define SCE_NP_TROPHY_ERROR_NOT_SUPPORTED                0x80022903
#define SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED       0x80022904
#define SCE_NP_TROPHY_ERROR_OUT_OF_MEMORY                0x80022905
#define SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT             0x80022906
#define SCE_NP_TROPHY_ERROR_EXCEEDS_MAX                  0x80022907
#define SCE_NP_TROPHY_ERROR_INSUFFICIENT_BUFFER          0x80022909
#define SCE_NP_TROPHY_ERROR_UNKNOWN_CONTEXT              0x8002290A
#define SCE_NP_TROPHY_ERROR_INVALID_CONTEXT              0x8002290E
#define SCE_NP_TROPHY_ERROR_PROCESSING_ABORTED           0x8002290F
#define SCE_NP_TROPHY_ERROR_UNKNOWN_HANDLE               0x80022911
#define SCE_NP_TROPHY_ERROR_HIDDEN                       0x80022913
#define SCE_NP_TROPHY_ERROR_CANNOT_UNLOCK_PLATINUM       0x80022914
#define SCE_NP_TROPHY_ERROR_ALREADY_UNLOCKED             0x80022915
#define SCE_NP_TROPHY_ERROR_INVALID_NP_COMM_ID           0x80022918
#define SCE_NP_TROPHY_ERROR_CONF_DOES_NOT_EXIST          0x8002291B
#define SCE_NP_TROPHY_ERROR_ICON_FILE_NOT_FOUND          0x8002291F
#define SCE_NP_TROPHY_ERROR_INVALID_TROPHY_ID            0x80022920
#define SCE_NP_TROPHY_ERROR_ILLEGAL_UPDATE               0x8002292A
#define SCE_NP_TROPHY_ERROR_UNKNOWN                      0x800229FF

#define SCE_NP_TROPHY_INVALID_TROPHY_ID     ((SceNpTrophyId)-1)

#define SCE_NP_TROPHY_MAX_NUM_TROPHIES      128
#define SCE_NP_TROPHY_NAME_MAX_SIZE         128
#define SCE_NP_TROPHY_DESC_MAX_SIZE         1024
#define SCE_NP_TROPHY_GAME_TITLE_MAX_SIZE   128
#define SCE_NP_TROPHY_GAME_DESC_MAX_SIZE    1024

#define SCE_NP_TROPHY_MAX_CONTEXTS          4
#define SCE_NP_TROPHY_MAX_HANDLES           4

#define SCE_NP_TROPHY_OPTIONS_CREATE_CONTEXT_READ_ONLY        1
#define SCE_NP_TROPHY_OPTIONS_REGISTER_CONTEXT_SHOW_ERROR_EXIT 1

#define SCE_NP_TROPHY_GRADE_UNKNOWN         0
#define SCE_NP_TROPHY_GRADE_PLATINUM        1
#define SCE_NP_TROPHY_GRADE_GOLD            2
#define SCE_NP_TROPHY_GRADE_SILVER          3
#define SCE_NP_TROPHY_GRADE_BRONZE          4

typedef s32 SceNpTrophyContext;
typedef s32 SceNpTrophyHandle;
typedef s32 SceNpTrophyId;

typedef struct SceNpTrophyDetails {
    s32  trophyId;
    u32  trophyGrade;
    char name[SCE_NP_TROPHY_NAME_MAX_SIZE];
    char description[SCE_NP_TROPHY_DESC_MAX_SIZE];
    u8   hidden;
    u8   reserved[3];
} SceNpTrophyDetails;

typedef struct SceNpTrophyData {
    u64  timestamp;
    s32  trophyId;
    u8   unlocked;
    u8   reserved[3];
} SceNpTrophyData;

typedef struct SceNpTrophyGameDetails {
    u32  numTrophies;
    u32  numPlatinum;
    u32  numGold;
    u32  numSilver;
    u32  numBronze;
    char title[SCE_NP_TROPHY_GAME_TITLE_MAX_SIZE];
    char description[SCE_NP_TROPHY_GAME_DESC_MAX_SIZE];
    u8   reserved[4];
} SceNpTrophyGameDetails;

typedef struct SceNpTrophyGameData {
    u32  unlockedTrophies;
    u32  unlockedPlatinum;
    u32  unlockedGold;
    u32  unlockedSilver;
    u32  unlockedBronze;
} SceNpTrophyGameData;

typedef struct SceNpTrophyFlagArray {
    u32 flag[SCE_NP_TROPHY_MAX_NUM_TROPHIES / 32];
} SceNpTrophyFlagArray;

s32 sceNpTrophyInit(void* poolPtr, u32 poolSize, u32 containerId, u64 options);
s32 sceNpTrophyTerm(void);

s32 sceNpTrophyCreateContext(SceNpTrophyContext* context,
                             const SceNpCommunicationId* commId,
                             const SceNpCommunicationSignature* commSign,
                             u64 options);
s32 sceNpTrophyDestroyContext(SceNpTrophyContext context);

s32 sceNpTrophyCreateHandle(SceNpTrophyHandle* handle);
s32 sceNpTrophyDestroyHandle(SceNpTrophyHandle handle);

s32 sceNpTrophyRegisterContext(SceNpTrophyContext context,
                               SceNpTrophyHandle handle,
                               u32 statusCb,
                               u32 arg,
                               u64 options);

#define SCE_NP_TROPHY_STATUS_NOT_INSTALLED       1
#define SCE_NP_TROPHY_STATUS_INSTALLED           3
#define SCE_NP_TROPHY_STATUS_PROCESSING_SETUP    5
#define SCE_NP_TROPHY_STATUS_PROCESSING_PROGRESS 6
#define SCE_NP_TROPHY_STATUS_PROCESSING_FINALIZE 7
#define SCE_NP_TROPHY_STATUS_PROCESSING_COMPLETE 8

s32 sceNpTrophyGetRequiredDiskSpace(SceNpTrophyContext context,
                                    SceNpTrophyHandle handle,
                                    u64* reqSpace, u64 options);

s32 sceNpTrophyGetGameInfo(SceNpTrophyContext context,
                           SceNpTrophyHandle handle,
                           SceNpTrophyGameDetails* details,
                           SceNpTrophyGameData* data);

s32 sceNpTrophyGetTrophyInfo(SceNpTrophyContext context,
                             SceNpTrophyHandle handle,
                             SceNpTrophyId trophyId,
                             SceNpTrophyDetails* details,
                             SceNpTrophyData* data);

s32 sceNpTrophyUnlockTrophy(SceNpTrophyContext context,
                            SceNpTrophyHandle handle,
                            SceNpTrophyId trophyId,
                            SceNpTrophyId* platinumId);

s32 sceNpTrophyGetTrophyUnlockState(SceNpTrophyContext context,
                                    SceNpTrophyHandle handle,
                                    SceNpTrophyFlagArray* flags,
                                    u32* count);

s32 sceNpTrophyGetGameIcon(SceNpTrophyContext context,
                           SceNpTrophyHandle handle,
                           void* buffer,
                           u32* size);

s32 sceNpTrophyGetTrophyIcon(SceNpTrophyContext context,
                             SceNpTrophyHandle handle,
                             SceNpTrophyId trophyId,
                             void* buffer,
                             u32* size);

typedef struct ps3_trophy_toast_event {
    int  id;
    char grade;
    char name[SCE_NP_TROPHY_NAME_MAX_SIZE];
    u8*  png;
    u32  png_size;
} ps3_trophy_toast_event;

int ps3_trophy_poll_toast(ps3_trophy_toast_event* out);

typedef void (*ps3_trophy_register_cb)(int id, const char* name, const char* detail, char grade, int unlocked);
typedef void (*ps3_trophy_unlock_cb)(int id);
typedef int  (*ps3_trophy_quiet_cb)(void);
typedef void (*ps3_trophy_frame_cb)(void);
void ps3_trophy_set_callbacks(ps3_trophy_register_cb on_register,
                              ps3_trophy_unlock_cb on_unlock,
                              ps3_trophy_quiet_cb is_quiet,
                              ps3_trophy_frame_cb on_frame);

#ifdef __cplusplus
}
#endif

#endif
