#ifndef PS3RECOMP_SCE_NP_COMMERCE2_H
#define PS3RECOMP_SCE_NP_COMMERCE2_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SCE_NP_COMMERCE2_ERROR_INVALID_ARGUMENT (s32)0x80024402
#define SCE_NP_COMMERCE2_ERROR_SERVER_ERROR     (s32)0x80024410

typedef u32 SceNpCommerce2Context;

s32 sceNpCommerce2Init(void);
s32 sceNpCommerce2Term(void);
s32 sceNpCommerce2CreateCtx(u32 version, const void* npId, u32 handler, u32 arg,
                              SceNpCommerce2Context* ctx);
s32 sceNpCommerce2DestroyCtx(SceNpCommerce2Context ctx);
s32 sceNpCommerce2CreateSessionStart(SceNpCommerce2Context ctx);
s32 sceNpCommerce2CreateSessionFinish(SceNpCommerce2Context ctx);
s32 sceNpCommerce2GetCategoryContentsStart(SceNpCommerce2Context ctx,
                                             const char* categoryId,
                                             u32 startPosition, u32 maxItems);
s32 sceNpCommerce2GetCategoryContentsGetResult(SceNpCommerce2Context ctx,
                                                 void* result);
s32 sceNpCommerce2DestroyReq(SceNpCommerce2Context ctx);

#ifdef __cplusplus
}
#endif
#endif
