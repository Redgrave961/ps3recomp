#ifndef PS3RECOMP_RSX_TRANSFER_H
#define PS3RECOMP_RSX_TRANSFER_H

#include "ps3emu/ps3types.h"

#ifdef __cplusplus
extern "C" {
#endif

void rsx_transfer_reset(void);
u32  rsx_transfer_methods(u32 reg, u32 inc, const u32* be_args, u32 count);

#ifdef __cplusplus
}
#endif

#endif
