#ifndef PS3EMU_GUEST_CALL_H
#define PS3EMU_GUEST_CALL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ps3_guest_caller_fn)(uint32_t opd_addr,
                                    uint64_t arg0, uint64_t arg1,
                                    uint64_t arg2, uint64_t arg3,
                                    uint64_t arg4, uint64_t arg5,
                                    uint64_t arg6, uint64_t arg7);

extern ps3_guest_caller_fn g_ps3_guest_caller;

int ppu_guest_call_ct_checked(uint32_t code, uint32_t toc,
    uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
    uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7, uint64_t* result);

#ifdef __cplusplus
}
#endif

#endif
